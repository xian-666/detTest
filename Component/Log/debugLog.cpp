#include "debugLog.h"
#include "ServerLog.h"
#include "../LogConfigReader.hpp"
#include <iostream>

//std::string g_strLastLogContent("");

void StartTextLogThread()
{
    if(LogConfigReader::m_logTarget.logText.load())
        ServerLog::GetInstance().StartLogThread();
}
void MyDbgStr(LPCSTR lpszFormat, ...)
{
    if (!LogConfigReader::m_logTarget.logText.load()
        && !LogConfigReader::m_logTarget.logConsole.load()) {
        return;
    }

    va_list   args;
    int       nBuf;
    char     szBuffer[512];
    va_start(args, lpszFormat);
    nBuf = _vsnprintf(szBuffer, sizeof(szBuffer) * sizeof(TCHAR), lpszFormat, args);
    assert(nBuf > 0);

    va_end(args);

    //if (g_strLastLogContent != std::string(szBuffer)) {
    //    g_strLastLogContent = std::string(szBuffer);
    //}
    //else {
    //    return;
    //}

    if (LogConfigReader::m_logTarget.logText.load()) {
        ServerLog::GetInstance().LogInfo("", std::string(szBuffer));
    }
    if (LogConfigReader::m_logTarget.logConsole.load()) {
        OutputDebugStringA(szBuffer);
        std::cout << std::string(szBuffer) << std::endl;
    }

}

void EndTextLogThread()
{
    if (LogConfigReader::m_logTarget.logText.load())
        ServerLog::GetInstance().EndLogThread();
}
