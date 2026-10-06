#include "ServerLog.h"
#include "../BaseUtility.hpp"
#include "../AES.h"
#include <string>
#include <chrono>
#include <ctime>
#include <windows.h>   // [Run identity] GetCurrentProcessId for per-run .bin isolation
#include <sstream>
using namespace std;

std::string ServerLog::GetTimeStamp(int formatType)
{
    //get the time
    std::chrono::system_clock::time_point tp = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm gmt{};//gmtime返回的是未经过时区转换的时间，localtime返回的为经过时区转换的时间
    gmt = *(localtime(&tt));
    std::chrono::duration<double> fractional_seconds =
        (tp - std::chrono::system_clock::from_time_t(tt)) + std::chrono::seconds(gmt.tm_sec);
    //format the string
    std::string buffer_content("hr:mn:sc.xxxxxx");//日志文件记录的日期格式为 11:23:01.123456，只记录时分秒
    std::string buffer_name("_year_mo_dy_ho_mi_se");//日志文件名日期格式为 2022_08_09

    if (formatType == 0)
    {
        sprintf(&buffer_content.front(), "%02d:%02d:%09.6f", gmt.tm_hour, gmt.tm_min, fractional_seconds.count());
        return buffer_content;
    }
    else {
        sprintf(&buffer_name.front(), "_%04d_%02d_%02d_%02d_%02d_%02d", gmt.tm_year + 1900, gmt.tm_mon + 1,
            gmt.tm_mday, gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
        return buffer_name;
    }
}

ServerLog::ServerLog()
{
    //if we specify an interval
    m_reopen_interval = std::chrono::seconds(300);//初始化打開文件持續時間
    //初始化文件路徑
    m_fileName = "";
    m_filePath = "\\log\\";
    m_fileSuffix = ".bin";
    m_currentFileSize = MAX_LOG_SIZE;
    m_threadRunningFlag = true;
    //crack the file open
    //ReOpen_New();

    m_time_pre = time_t(0);
    m_time_now = time_t(0);

    m_bLogTimeStamp = true;
    std::string strFolder = BaseUtility::GetCurrentDirectory_();
    std::string strLogImage = strFolder + "\\Log\\debugImages\\";
    BaseUtility::CreateDirectory_(strLogImage);
}

ServerLog& ServerLog::GetInstance()
{
    static ServerLog ServerLogger;
    return ServerLogger;
}

ServerLog::~ServerLog()
{

}

void ServerLog::ReOpen() {
    //TODO: use CLOCK_MONOTONIC_COARSE
    //check if it should be closed and reopened
    auto now = std::chrono::system_clock::now();
    m_logLock.lock();
    if (now - m_last_reopen > m_reopen_interval) {
        m_last_reopen = now;
        try { m_file.close(); }
        catch (...) {}
        try {
            m_file.open(m_fileName, std::ofstream::out | std::ofstream::app);
            m_last_reopen = std::chrono::system_clock::now();
        }
        catch (std::exception& e) {
            try { m_file.close(); }
            catch (...) {}
            throw e;
        }
    }
    m_logLock.unlock();
}

void ServerLog::ReOpen_New()
{
    if (m_currentFileSize>=MAX_LOG_SIZE) {
        try { 
            m_file.close(); 
            m_currentFileSize = 0;
        }
        catch (...) {}
        try {
            std::string strCurrentPath = BaseUtility::GetCurrentDirectory_();
            std::string strLogDirectory = strCurrentPath + m_filePath;
            if (!BaseUtility::CheckDirectoryExist(strLogDirectory)) {
                BaseUtility::CreateDirectory_(strLogDirectory);
            }
            // [Run identity] Timestamp alone has second resolution; two launches in the same
            // second (or a crash-retry loop) would collide and append into the same .bin.
            // Appending PID guarantees per-process isolation for the upload log too.
            std::ostringstream pidTag;
            pidTag << "_pid" << static_cast<unsigned long>(GetCurrentProcessId());
            std::string fullFilePath = strCurrentPath + m_filePath + m_fileName
                + GetTimeStamp(TIMESTAMP_FORMAT_LOGNAME)
                + pidTag.str()
                + m_fileSuffix;
            // [Isolation] trunc: never inherit a crashed predecessor's stale tail.
            m_file.open(fullFilePath, std::ios::out | std::ios::trunc | std::ios::binary);
        }
        catch (std::exception& e) {
            try { m_file.close(); }
            catch (...) {}
            throw e;
        }
    }
}

    void ServerLog::ReOpen_NewDay()
    {
        //TODO: use CLOCK_MONOTONIC_COARSE
        //check if it should be closed and reopened
        //m_logLock.lock();
        if (DetectDateChange()) {
            try { m_file.close(); }
            catch (...) {}
            try {
                std::string fullFilePath = m_filePath + m_fileName + "_" + 
                    to_string(localtime(&m_time_now)->tm_year + 1900)+
                    to_string(localtime(&m_time_now)->tm_mon+1) + 
                    to_string(localtime(&m_time_now)->tm_mday)+
                    m_fileSuffix;
                m_file.open(fullFilePath, std::ofstream::out | std::ofstream::app|std::ofstream::binary);
            }
            catch (std::exception& e) {
                try { m_file.close(); }
                catch (...) {}
                throw e;
            }
        }
        //m_logLock.unlock();
    }

bool ServerLog::DetectDateChange()
{
    // detect midnight -- call regularly
    m_time_now = time(0);

    int d_pre = localtime(&m_time_pre)->tm_yday;
    int d_now = localtime(&m_time_now)->tm_yday;
    m_time_pre = m_time_now;
    if (d_pre != d_now)
        return true;
    else
        return false;
}

void ServerLog::FormatLogStringSuffix(std::string& str, int& len)
{
    char subStr[16] = { 0x20,0x20,0x20,0x20,
                    0x20,0x20,0x20,0x20,
                    0x20,0x20,0x20,0x20,
                    0x20,0x20,0x20,'\n' };
    int modV = len % 16;
    str.reserve(len + 16 - modV);
    str.append(subStr, modV, 16 - modV);
    len = len + 16 - modV;
}

void ServerLog::PushLogString(const std::string& tag, const std::string& logMsg)
{
    std::unique_lock<std::mutex> lock(m_logLock);
    
    if (logMsg == "")//没有需要记录的日志，直接返回
        return;

    std::string output;
    output.reserve(logMsg.length() + 64);
    output.append((GetTimeStamp(TIMESTAMP_FORMAT_LOGCONTENT))+"--->");
    output.append(logMsg); 
    int len = output.length();
    FormatLogStringSuffix(output, len);//加密的字符串长度必须为16的整数倍
    int rounds = len / 16;
    for (int i = 0; i < rounds; ++i)
        AES_128::GetInstance().AES_Encrypt((uint8_t*)output.data() + i * 16);
    m_logDataQueue.enqueue(output);
}

void ServerLog::SetLogName(const std::string& logName)
{
    m_fileName = logName;
}

void ServerLog::SetLogPath(const std::string& logPath)
{
    m_filePath = logPath;
}

void ServerLog::StartLogThread()
{
    m_currentFileSize = MAX_LOG_SIZE;
    m_threadRunningFlag = true;
    SetLogName("b1");
    SetLogPath("\\Log\\");
    
    //檢查當前綫程對象是否表示了一個活動的執行綫程
    if (m_consumerLogThread.joinable())
    {
        m_threadRunningFlag = false;
        m_consumerLogThread.join();
        if (m_file.is_open())
            m_file.close();
    }
    
    m_consumerLogThread = std::thread([&] {
        while (m_threadRunningFlag)
        {
            while (!m_logDataQueue.empty())
            {
                ReOpen_New();
                string strLogMsg;
                if (m_logDataQueue.dequeue(strLogMsg))
                {
                    //m_file << strLogMsg;
                    m_file.write(strLogMsg.c_str(), strLogMsg.length());
                    m_file.flush();
                    m_currentFileSize += strLogMsg.length();

                }                
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        });
}


void ServerLog::EndLogThread()
{
    if (m_consumerLogThread.joinable())
    {
        m_threadRunningFlag = false;
        m_consumerLogThread.join();
        if(m_file.is_open())
            m_file.close();
    }
}

void ServerLog::LogInfo( const string& tag, const string& logMsg, long long timestamp )
{
    //WriteLog(logMsg, log_level::INFO);
    std::string output;
    PushLogString(tag, logMsg);
}

