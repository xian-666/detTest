#ifndef _SERVERLOG_H
#define _SERVERLOG_H

#include "../BTSafeQueue.h"
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <thread>
#include <time.h>
#include <chrono>

#ifdef _WINDOWS
#define LOG_ENCRYPT 0
#else
#define LOG_ENCRYPT 1
#endif

#define MAX_LOG_SIZE 10*1024*1024    //日志文件最大为10M，超过10M之后需要重新新建文件

#define TIMESTAMP_FORMAT_LOGCONTENT 0    //日志文本里的时间戳格式
#define TIMESTAMP_FORMAT_LOGNAME 1    //日志名称的时间戳格式 

//#define YSCONCAT(a,b) a##b

class ServerLog
{
private:
    std::string m_fileName;//文件名稱
    std::string m_filePath;//文件路徑
    std::string m_fileSuffix;//文件后缀名
    std::ofstream m_file;//文件流對象
    int m_currentFileSize;//当前文件大小
    std::chrono::seconds m_reopen_interval;//日志文件處於open狀態的持續時間
    std::chrono::system_clock::time_point m_last_reopen;//日志文件上一次打開的時間

    std::thread m_consumerLogThread;//消費者綫程，用於將日志數據寫入文件中
    YSSafeQueue<std::string> m_logDataQueue;//日志數據隊列
    bool m_threadRunningFlag;//消費者綫程運行標識
    std::mutex m_logLock; //互斥鎖，用於綫程安全的日志    
    bool m_bLogTimeStamp;//是否记录时间戳，true 记录 false 不记录

    std::string GetTimeStamp(int formatType);//獲取時間戳
    void ReOpen();//间隔一段时间后重新打開文件
    void ReOpen_New();//日志文件涨到一定大小，需要重开一份新的文件
    void ReOpen_NewDay();//新的一天新建一份新的日志文件
    bool DetectDateChange();//檢查日期是否發生變化

    void FormatLogStringSuffix(std::string& str, int& len);

    time_t m_time_pre;//上一次的時間
    time_t m_time_now;//當前時間

protected:

    void PushLogString(const std::string& tag, const std::string& log);

private:
    ServerLog();
public:
    ServerLog(const ServerLog&) = delete;
    ServerLog(ServerLog&&) = delete;
    ServerLog& operator=(const ServerLog&) = delete;
    ServerLog& operator=(ServerLog&&) = delete;

    static ServerLog& GetInstance();

    ~ServerLog();

	void LogInfo(const std::string& tag, const std::string& log, long long timestamp = 0) ;

    void SetLogName(const std::string& logName);

    void SetLogPath(const std::string& logPath);

    void StartLogThread() ;
    
    void EndLogThread();
};

#endif
