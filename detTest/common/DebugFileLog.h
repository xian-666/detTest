#pragma once

// ============================================================================
// DebugFileLogger - per-run independent file logger
// ----------------------------------------------------------------------------
// Compiled in BOTH Debug and Release. Runtime gating is done exclusively by
// g_bLogOutputEnabled, which is loaded from config.ini [Log] text at startup.
// Setting [Log] text = 0 keeps the file untouched (macros no-op); text = 1 in
// Release now produces a log identical to Debug, which is what field diagnostics
// usually need.
// ============================================================================

#include <string>
#include <fstream>
#include <mutex>
#include <ctime>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <vector>
#include <windows.h>

class DebugFileLogger {
private:
    std::ofstream m_logFile;
    std::mutex m_mutex;
    bool m_initialized;
    std::string m_logFilePath;

    // [Run identity] Optional tag appended to the log filename (e.g. game process name) and
    // banner lines written once right after the standard "Debug Log Started" header. Both must
    // be provided BEFORE Initialize() so the filename can include the tag. If the logger is
    // already open when SetRunContext() is called, only the banner lines are appended (the file
    // name cannot be changed after creation).
    std::string m_runTag;
    std::vector<std::string> m_pendingBanner;
    bool m_bannerWritten = false;

    DebugFileLogger() : m_initialized(false) {}

    // Replace characters illegal in a Windows file name with '_'. Truncate to 32 chars.
    static std::string SanitizeFileTag(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            switch (c) {
                case '\\': case '/': case ':': case '*': case '?':
                case '"': case '<': case '>': case '|':
                case ' ':
                    out.push_back('_'); break;
                default:
                    // Strip control chars entirely
                    if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
            }
        }
        if (out.size() > 32) out.resize(32);
        while (!out.empty() && (out.back() == '.' || out.back() == '_')) out.pop_back();
        return out;
    }

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

        // [Run identity] Append sanitized tag + PID to the filename so that every launch is
        // guaranteed its own log file:
        //   debug_20261006_142345_ZenlessZoneZero_pid12345.log
        // Rationale:
        //   - Timestamp has only second resolution; two launches within the same second would
        //     otherwise collide on the same file.
        //   - Concurrent instances (dual-launch / CI stress) also need separation.
        //   - Windows guarantees no two RUNNING processes share a PID, so PID is the minimal
        //     uniqueness key that solves both cases without a clock.
        //   - The trailing "_pid<N>" is unconditionally appended (even when tag is empty), so
        //     filename uniqueness never depends on config being loaded.
        std::string tag = SanitizeFileTag(m_runTag);
        char pidStr[32];
        _snprintf_s(pidStr, sizeof(pidStr), _TRUNCATE, "pid%lu",
            static_cast<unsigned long>(GetCurrentProcessId()));
        m_logFilePath = logDir + "\\debug_" + std::string(timeStr);
        if (!tag.empty()) m_logFilePath += "_" + tag;
        m_logFilePath += "_" + std::string(pidStr);
        m_logFilePath += ".log";

        // [Isolation] ios::trunc guarantees each PID owns a pristine file. If for any reason
        // the name collides (extremely unlikely: PID reuse + same-second launch + same tag),
        // the new run wipes the stale content instead of silently appending to a crashed
        // predecessor's tail. The PID suffix makes collision nearly impossible in practice.
        m_logFile.open(m_logFilePath, std::ios::out | std::ios::trunc);
        if (!m_logFile.is_open()) {
            return false;
        }

        m_initialized = true;

        char headerTime[128];
        std::strftime(headerTime, sizeof(headerTime), "%Y-%m-%d %H:%M:%S", localTime);
        m_logFile << "========================================" << std::endl;
        m_logFile << "Debug Log Started: " << headerTime << std::endl;
        m_logFile << "Log File: " << m_logFilePath << std::endl;
        // [Run identity] Write the caller-provided banner (target spec / env vars / OS build)
        // right inside the standard header block so opening any log immediately reveals which
        // game + configuration produced it.
        for (const auto& line : m_pendingBanner) {
            m_logFile << line << std::endl;
        }
        if (!m_pendingBanner.empty()) m_bannerWritten = true;
        m_logFile << "========================================" << std::endl;
        m_logFile.flush();

        return true;
    }

    // Must be called BEFORE the first Log()/Initialize() so the filename can carry the tag.
    // bannerLines are emitted verbatim inside the header block (each line is written as-is;
    // the caller is responsible for formatting). Safe to call multiple times: later calls
    // replace the pending banner; if the file is already open the new banner is appended
    // directly (once) and the filename is left untouched.
    void SetRunContext(const std::string& tag, const std::vector<std::string>& bannerLines) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_runTag = tag;
        m_pendingBanner = bannerLines;
        if (m_initialized && m_logFile.is_open() && !m_bannerWritten) {
            for (const auto& line : m_pendingBanner) {
                m_logFile << line << std::endl;
            }
            m_logFile.flush();
            m_bannerWritten = true;
        }
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
// [Run identity] Set tag + banner BEFORE DEBUG_LOG_INIT(); the tag affects the filename.
// Note: the tag/banner is stored on the logger singleton regardless of g_bLogOutputEnabled,
// so a later Initialize() (whether triggered by DEBUG_LOG_INIT() or lazily by Log()) still
// sees the correct filename. SetRunContext itself never touches disk.
#define DEBUG_LOG_SET_RUN_CONTEXT(tag, bannerLines) \
    DebugFileLogger::GetInstance().SetRunContext((tag), (bannerLines))
