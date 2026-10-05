#pragma once

#ifdef _DEBUG

#include <string>
#include <fstream>
#include <mutex>
#include <ctime>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <windows.h>

class DebugFileLogger {
private:
    std::ofstream m_logFile;
    std::mutex m_mutex;
    bool m_initialized;
    std::string m_logFilePath;

    DebugFileLogger() : m_initialized(false) {}

public:
    static DebugFileLogger& GetInstance() {
        static DebugFileLogger instance;
        return instance;
    }

    bool Initialize() {
        if (m_initialized) return true;

        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_initialized) return true;

        std::time_t now = std::time(nullptr);
        std::tm* localTime = std::localtime(&now);

        char timeStr[64];
        std::strftime(timeStr, sizeof(timeStr), "%Y%m%d_%H%M%S", localTime);

        std::string logDir = "Log";
        
        CreateDirectoryA(logDir.c_str(), NULL);

        m_logFilePath = logDir + "\\debug_" + std::string(timeStr) + ".log";

        m_logFile.open(m_logFilePath, std::ios::out | std::ios::app);
        if (!m_logFile.is_open()) {
            return false;
        }

        m_initialized = true;

        char headerTime[128];
        std::strftime(headerTime, sizeof(headerTime), "%Y-%m-%d %H:%M:%S", localTime);
        m_logFile << "========================================" << std::endl;
        m_logFile << "Debug Log Started: " << headerTime << std::endl;
        m_logFile << "Log File: " << m_logFilePath << std::endl;
        m_logFile << "========================================" << std::endl;
        m_logFile.flush();

        return true;
    }

    void Log(const std::string& message) {
        if (!m_initialized) {
            Initialize();
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_logFile.is_open()) return;

        auto now = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm* localTime = std::localtime(&timeT);

        char timeStr[32];
        std::strftime(timeStr, sizeof(timeStr), "%H:%M:%S", localTime);

        m_logFile << "[" << timeStr << "." 
                  << std::setfill('0') << std::setw(3) << ms.count() << "] "
                  << message << std::endl;
        m_logFile.flush();
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_logFile.is_open()) {
            m_logFile << "========================================" << std::endl;
            m_logFile << "Debug Log Ended" << std::endl;
            m_logFile << "========================================" << std::endl;
            m_logFile.close();
        }
        m_initialized = false;
    }

    ~DebugFileLogger() {
        Shutdown();
    }
};

// Runtime switch for DEBUG_LOG* file logging. Defined in detTest.cpp
// and set once at startup from config.ini ([Log] text) before any thread runs.
extern bool g_bLogOutputEnabled;

#define DEBUG_LOG_INIT() do { if (g_bLogOutputEnabled) DebugFileLogger::GetInstance().Initialize(); } while (0)
#define DEBUG_LOG(msg) do { if (g_bLogOutputEnabled) DebugFileLogger::GetInstance().Log(msg); } while (0)
#define DEBUG_LOG_FMT(fmt, ...) do { \
    if (g_bLogOutputEnabled) { \
        char _buf[1024]; \
        _snprintf_s(_buf, sizeof(_buf), fmt, ##__VA_ARGS__); \
        DebugFileLogger::GetInstance().Log(_buf); \
    } \
} while(0)
#define DEBUG_LOG_SHUTDOWN() do { if (g_bLogOutputEnabled) DebugFileLogger::GetInstance().Shutdown(); } while (0)

#else

#define DEBUG_LOG_INIT()
#define DEBUG_LOG(msg)
#define DEBUG_LOG_FMT(fmt, ...)
#define DEBUG_LOG_SHUTDOWN()

#endif
