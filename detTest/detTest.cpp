// detTest.cpp
//
// Screen-capture + inference stability harness.
//
// Forked from btHonorWorldDet. The game domain layer (HeroStrategy /
// StrategyController / WeaponObserver / GamePadDev) has been removed; what is
// left is exactly the part being evaluated: how reliably the capture subsystem
// pulls frames off a hybrid-GPU laptop across three games, and whether the
// inference pipeline stays fed.
//
// The capture scheduling logic itself is preserved verbatim from the parent
// project: frame limiter, HWND stability gate, foreground gate, black-screen
// pause + DXGI keep-alive, soft vs force reinit, and the graded inference
// recovery ladder. Changing any of that would change the thing under test.

#include "common/common.hpp"
#include "GameInferenceSystem.h"
#include "Component/Log/debugLog.h"
#include "Component/LogConfigReader.hpp"
#include "ImageSaver.h"
#include "CaptureMetrics.h"
#include "common/AppConfig.h"
#include "common/DebugFileLog.h"
#include "common/ScreenAspectDetector.hpp"
#include "common/TargetWindow.h"
#include "Screen/UnifiedScreenCapture.h"
#include <winrt/base.h>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cctype>
#include <csignal>
#include <fstream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <string>

const bool USE_CLASSIFICATION_MODEL = true;

#ifndef LAN_TEST
#ifdef _DEBUG
#pragma comment(linker, "/subsystem:console")
#else
#pragma comment(linker, "/subsystem:windows /entry:mainCRTStartup")
#endif
#endif

const bool bEnableOfflineMode = false;

LogTarget LogConfigReader::m_logTarget;

// Runtime switch for the DEBUG_LOG file logger (DebugFileLog.h).
// Set once at startup from config.ini ([Log] text) before any thread is spawned.
bool g_bLogOutputEnabled = true;

std::atomic<int> g_crashStage{ 0 };

// Human readable description of the configured capture target, shown on the
// "Waiting for ..." overlay. Filled in when [Target] is parsed.
std::string g_targetLabel = "target";

// Set by the 's' key while the debug window has focus. Consumed by the main
// loop, which dumps the current ROI crops for visual inspection. In the parent
// project this was a HID button on the gamepad, which no longer exists here.
std::atomic<bool> g_saveRoiFrames{ false };

// Metrics helpers. The capture enums and the metrics enums share their
// declaration order, so this stays a straight static_cast through int.
static CaptureMetrics::Method ToMetricsMethod(UnifiedScreenCapture::CaptureMethod m) {
    switch (m) {
        case UnifiedScreenCapture::CaptureMethod::DXGI: return CaptureMetrics::Method::DXGI;
        case UnifiedScreenCapture::CaptureMethod::WGC:  return CaptureMetrics::Method::WGC;
        case UnifiedScreenCapture::CaptureMethod::GDI:  return CaptureMetrics::Method::GDI;
        default:                                        return CaptureMetrics::Method::Unknown;
    }
}

static CaptureMetrics::CapState ToMetricsState(UnifiedScreenCapture::CaptureState s) {
    switch (s) {
        case UnifiedScreenCapture::CaptureState::Idle:          return CaptureMetrics::CapState::Idle;
        case UnifiedScreenCapture::CaptureState::Capturing:     return CaptureMetrics::CapState::Capturing;
        case UnifiedScreenCapture::CaptureState::ExceptionWait: return CaptureMetrics::CapState::ExceptionWait;
        case UnifiedScreenCapture::CaptureState::Paused:        return CaptureMetrics::CapState::Paused;
        case UnifiedScreenCapture::CaptureState::Stopped:       return CaptureMetrics::CapState::Stopped;
        default:                                                return CaptureMetrics::CapState::Unknown;
    }
}
static void WriteAbortBreadcrumb(const char* reason) {
    try {
        wchar_t exePath[MAX_PATH]{};
        std::string dir = ".\\";
        if (GetModuleFileNameW(NULL, exePath, MAX_PATH) != 0) {
            std::wstring wpath(exePath);
            size_t pos = wpath.find_last_of(L"\\/");
            if (pos != std::wstring::npos) {
                wpath = wpath.substr(0, pos + 1);
            }
            std::string tmp;
            tmp.reserve(wpath.size());
            for (wchar_t c : wpath) {
                if (c > 0 && c <= 127) tmp.push_back(static_cast<char>(c));
                else tmp.push_back('?');
            }
            dir = tmp;
        }
        std::ofstream ofs(dir + "abort_breadcrumb.txt", std::ios::app);
        ofs << "reason=" << (reason ? reason : "null")
            << " stage=" << g_crashStage.load() << "\n";
        ofs.flush();
    }
    catch (...) {}
}
static void OnSigAbort(int) {
    WriteAbortBreadcrumb("SIGABRT");
    std::_Exit(134);
}

inline std::string GetExeDir() {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
        return "";
    }
    std::wstring wpath(exePath);
    size_t pos = wpath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        wpath = wpath.substr(0, pos + 1);
    }
    std::string result;
    result.reserve(wpath.size());
    for (wchar_t c : wpath) {
        if (c > 0 && c <= 127) {
            result.push_back(static_cast<char>(c));
        }
        else {
            result.push_back('?');
        }
    }
    return result;
}

const std::string AI_SCREENSHOT_DIR = GetExeDir() + "ROI_Captures";

// ROI layout fed to the classification model, in 1920x1080 reference units.
// These are the HoK button regions; for the other games under test they simply
// land on unrelated pixels and the model answers "Other", which is the expected
// result -- the point of the harness is the capture path, not the labels.

const std::vector<Region> captureRegions = {
  // 1920x1080
  Region(1747, 950, 93, 93,  "LsRs"),
  Region(1529, 949, 57, 57,  "LB_1"),
  Region(1644, 949, 57, 57,  "RB_1"),
  Region(1529, 1014, 57, 57, "LT_1"),
  Region(1644, 1014, 57, 57, "RT_1"),
  Region(1497, 981, 57, 57,  "LB_2"),
  Region(1577, 981, 57, 57,  "LT_2"),
  Region(1656, 981, 57, 57,  "RT_2"),
};

struct PerformanceStats {
    double total_time_sum = 0.0;
    double inference_time_sum = 0.0;
    int count = 0;

    void add(const ONNXInference::PerformanceMetrics& metrics) {
        total_time_sum += metrics.total_time_ms;
        inference_time_sum += metrics.inference_time_ms;
        count++;
    }

    double get_avg_total() const { return count > 0 ? total_time_sum / count : 0.0; }
    double get_avg_inference() const { return count > 0 ? inference_time_sum / count : 0.0; }
    double get_fps() const { return count > 0 ? 1000.0 / (total_time_sum / count) : 0.0; }
};

// NOTE: parseBulletCount(), IsRightTriggerLongPressCompleted(), DetectDevice()
// and HeroNotify() were part of the game (bullet-count / HID-gamepad / hero
// strategy) layer and have been removed with it.

void ShowConsole(bool show) {
    HWND consoleWindow = GetConsoleWindow();
    if (consoleWindow != NULL) {
        // A console already exists (e.g. Debug /subsystem:console build or one launched
        // from a terminal): just show/hide it.
        ShowWindow(consoleWindow, show ? SW_SHOW : SW_HIDE);
        return;
    }
    if (show) {
        // Running as a GUI app (/subsystem:windows) with no console attached, but the
        // user enabled console logging. Allocate a fresh console and redirect stdout/stderr
        // so std::cout / std::cerr / printf actually appear.
        if (AllocConsole()) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            freopen("CONIN$", "r", stdin);
            SetConsoleOutputCP(CP_UTF8);
        }
    }
}

// Vectored exception handler to catch WinRT exceptions from GraphicsCapture.dll
LONG WINAPI WGCVectoredExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo) {
    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;

    // Only intercept true error-level exceptions (high bit set = error).
    // 0xC000027B = STATUS_STOWED_EXCEPTION (WinRT fatal), 0x80070005 = E_ACCESSDENIED,
    // 0x887A0005 = DXGI_ERROR_DEVICE_REMOVED, 0xC0000005 = ACCESS_VIOLATION.
    //
    // DO NOT intercept 0x40080201 — this is a severity=0 status code used by
    // WinRT's normal exception propagation (hresult_error throw/catch). VEH
    // interception + EXCEPTION_CONTINUE_EXECUTION skips WinRT's internal catch,
    // leaving WinRT state inconsistent and causing secondary crashes in Debug mode.
    if (code == 0xC000027B || code == 0x80070005 ||
        code == 0x887A0005 || code == 0xC0000005) {
        DEBUG_LOG_FMT("[VEH] Caught exception: 0x%X at address 0x%p - attempting recovery",
            code, pExceptionInfo->ExceptionRecord->ExceptionAddress);

        // Release capture resources to prevent stale state
        UnifiedScreenCapture::GetInstance().Release();

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // For other exceptions (including WinRT 0x40080201 status propagation),
    // let the normal handler chain process them so WinRT try/catch works.
    return EXCEPTION_CONTINUE_SEARCH;
}

// Global unhandled exception filter - last resort before process termination
LONG WINAPI GlobalExceptionFilter(PEXCEPTION_POINTERS pExceptionInfo) {
    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
    DEBUG_LOG_FMT("[GLOBAL EXCEPTION FILTER] Caught unhandled exception: 0x%X", code);
    
    // Force release resources to prevent GPU state corruption
    UnifiedScreenCapture::GetInstance().Release();
    
    // Return EXCEPTION_CONTINUE_SEARCH to allow normal crash dump generation
    return EXCEPTION_CONTINUE_SEARCH;
}

int main()
{
    // Install vectored exception handler as first line of defense
    PVOID vehHandle = AddVectoredExceptionHandler(1, WGCVectoredExceptionHandler);
    if (vehHandle) {
        DEBUG_LOG_FMT("[MAIN] Vectored exception handler installed");
    }
    
    // Install global unhandled exception filter as last resort
    SetUnhandledExceptionFilter(GlobalExceptionFilter);

    //CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    // Improve Windows scheduler resolution to 1ms so that
    // std::this_thread::sleep_for(< 15ms) actually honors microsecond granularity.
    // This is required for accurate 60 FPS frame rate limiting.
    timeBeginPeriod(1);
#endif

    g_crashStage.store(1);

    // ---- Load log/display config from config.ini (next to the exe) ----
    // Standard INI format, section [Log]. All keys are optional and default
    // to false when absent. Values: 1/0, true/false, yes/no, on/off.
    //   console = true  -> stdout debug logs (PERF lines, MyDbgStr console output)
    //   text    = true  -> write text logs to files under \Log\
    //   show    = true  -> show the OpenCV debug window (can steal game focus)
    {
        // Auto-sync config.ini to the exe run path at startup, so editing the source
        // config takes effect without re-running copy_resources.bat. The authoritative
        // source is the config.ini located in the same directory as the .vcxproj file
        // (project root); it is copied when the destination is missing or older.
        {
            wchar_t exePath[MAX_PATH]{};
            std::filesystem::path exeDir;
            if (GetModuleFileNameW(NULL, exePath, MAX_PATH) != 0) {
                exeDir = std::filesystem::path(exePath).parent_path();
            }
            std::filesystem::path dest = exeDir / "config.ini";
            std::filesystem::path srcCfg;
            // Walk upward from the exe dir (skipping the exe dir itself) to find the
            // directory that contains the .vcxproj, then use its config.ini.
            auto dir = exeDir.parent_path();
            for (int i = 0; i < 6 && !dir.empty(); ++i, dir = dir.parent_path()) {
                bool hasVcxproj = std::any_of(
                    std::filesystem::directory_iterator(dir),
                    std::filesystem::directory_iterator(),
                    [](const std::filesystem::directory_entry& e) {
                        return e.is_regular_file() && e.path().extension() == ".vcxproj";
                    });
                if (hasVcxproj) {
                    auto candidate = dir / "config.ini";
                    if (std::filesystem::exists(candidate)) {
                        srcCfg = candidate;
                    }
                    break;
                }
            }
            if (!srcCfg.empty()) {
                bool needCopy = !std::filesystem::exists(dest);
                if (needCopy) {
                    CopyFileW(srcCfg.c_str(), dest.c_str(), FALSE);
                    std::cout << "[CONFIG] Auto-copied " << srcCfg.string()
                              << " -> " << dest.string() << "\n";
                } else {
                    // Copy when the source tree config is newer than the run-path one.
                    auto srcT = std::filesystem::last_write_time(srcCfg);
                    auto dstT = std::filesystem::last_write_time(dest);
                    if (srcT > dstT) {
                        CopyFileW(srcCfg.c_str(), dest.c_str(), FALSE);
                        std::cout << "[CONFIG] Updated " << srcCfg.string()
                                  << " -> " << dest.string() << "\n";
                    }
                }
            }
        }

        std::string cfgPath = GetExeDir() + "config.ini";

        // Parsed with appcfg::IniFile rather than GetPrivateProfileString*:
        // the file is UTF-8 and contains Chinese window titles, which the
        // private-profile API would decode through the system ANSI code page.
        static appcfg::IniFile g_ini;
        bool cfgLoaded = g_ini.Load(strconv::ToWide(cfgPath));
        if (cfgLoaded) {
            LogConfigReader::m_logTarget.logConsole.store(
                g_ini.GetBool(L"Log", L"console", false), std::memory_order_release);
            LogConfigReader::m_logTarget.logText.store(
                g_ini.GetBool(L"Log", L"text", false), std::memory_order_release);
            LogConfigReader::m_logTarget.showDetection.store(
                g_ini.GetBool(L"Log", L"show", false), std::memory_order_release);

            // ---- Capture target ----
            target::ActiveSpec() = target::LoadSpec(g_ini);
            g_targetLabel = strconv::ToUtf8(target::ActiveSpec().Describe());
        }
        std::cout << "[CONFIG] " << (cfgLoaded ? "Loaded " : "NOT found, using defaults: ")
            << cfgPath
            << " | console=" << (LogConfigReader::m_logTarget.logConsole.load() ? "on" : "off")
            << " text=" << (LogConfigReader::m_logTarget.logText.load() ? "on" : "off")
            << " show=" << (LogConfigReader::m_logTarget.showDetection.load() ? "on" : "off")
            << "\n";
        std::cout << "[CONFIG] Target " << g_targetLabel << "\n";

        // Say so up front when the configured executable simply is not running.
        // A typo in [Target] process and "the game is not started yet" look
        // identical from inside the main loop, and that ambiguity costs a lot of
        // time when you are bisecting a capture problem.
        if (cfgLoaded && !target::ActiveSpec().process.empty() &&
            !target::ProcessRunning(target::ActiveSpec())) {
            std::cout << "[CONFIG] WARNING: no running process matches '"
                << strconv::ToUtf8(target::ActiveSpec().process)
                << "' - start the game, or fix [Target] process in config.ini\n";
        }

        // Gate the DEBUG_LOG file logger with the text flag
        g_bLogOutputEnabled = LogConfigReader::m_logTarget.logText.load();
        // Start the ServerLog consumer thread so text logs get flushed to disk
        if (LogConfigReader::m_logTarget.logText.load()) {
            StartTextLogThread();
        }

        // ---- Stability instrumentation ----
        CaptureMetrics::Config mcfg;
        mcfg.enable = cfgLoaded ? g_ini.GetBool(L"Metrics", L"enable", true) : true;
        mcfg.hud = cfgLoaded ? g_ini.GetBool(L"Metrics", L"hud", true) : true;
        mcfg.reportIntervalSec = cfgLoaded ? g_ini.GetInt(L"Metrics", L"report_interval_sec", 5) : 5;
        mcfg.csv = cfgLoaded ? g_ini.GetBool(L"Metrics", L"csv", false) : false;
        mcfg.csvDir = cfgLoaded ? strconv::ToUtf8(g_ini.GetString(L"Metrics", L"csv_dir", L"metrics"))
                                : std::string("metrics");
        CaptureMetrics::GetInstance().Init(mcfg, GetExeDir());
    }

    DEBUG_LOG_INIT();
    DEBUG_LOG("Application started");

    g_crashStage.store(2);
    std::signal(SIGABRT, OnSigAbort);
    std::set_terminate([]() {
        WriteAbortBreadcrumb("terminate");
        std::_Exit(1);
    });

    g_crashStage.store(3);
    // Show an output console only when console logging is enabled ([Log] console = true).
    // In Release (/subsystem:windows) this actually allocates a console so stdout appears.
    ShowConsole(LogConfigReader::m_logTarget.logConsole.load());

    // ---- Pre-initialize screen aspect detector ----
    // This reads GameUserSettings.ini to determine the physical monitor aspect
    // ratio (16:9 vs 21:9 vs 16:10, etc.). The detection happens once and is
    // cached in the singleton. It must happen before any ROI generation so
    // that generateDynamicROIs can pick the correct aspect-specific residual.
    {
        std::string aspectCat = screen_aspect::GetAspectCategory();
        DEBUG_LOG_FMT("[MAIN] Screen aspect category: %s", aspectCat.c_str());
    }

    try {
        // No thread pool and no HID workers any more: everything runs on the
        // main loop plus the inference thread owned by GameInferenceSystem.
        // The capture singleton is pre-created here on the main thread to avoid
        // COM initialization happening later on a worker thread.
        (void)UnifiedScreenCapture::GetInstance();

        GameInferenceSystem system(
            common::GetModelPath(L"HONOR").c_str(),
            nullptr,
            captureRegions,
            224
        );

        bool wasRealtime = false;

        PerformanceStats realtimeStats;

        const std::string WIN_NAME = "detTest - Capture / Inference";
        bool windowReady = false;

        std::cout << "System ready.\n"
            << "- Real-time: capture runs while the target window is in focus\n"
            << "- Target: " << g_targetLabel << "\n"
            << "- ROIs: LsRs, LB_1, RB_1, LT_1, RT_1, LB_2, LT_2, RT_2\n"
            << "- Keys: 'q'/ESC quit, 's' save the current ROI crops\n\n";

        static cv::Mat s_lastMosaic;
        static std::vector<ClassificationResult> s_lastClassifications;
        static bool s_hasLastInference = false;

        static auto lastCheck = std::chrono::steady_clock::now();
        HWND pendingForegroundGameWnd = nullptr;
        auto foregroundStableSince = std::chrono::steady_clock::time_point{};
        constexpr auto kForegroundStabilizeDelay = std::chrono::milliseconds(300);

        while (true) {
            // [Frame rate limiter] MUST be at loop top to ensure ALL exit paths
            // 动态帧率限制器：跟踪实际帧间隔，动态调整 sleep
            static auto s_lastIterStart = std::chrono::steady_clock::now();
            static auto s_prevIterStart = s_lastIterStart;
            static int64_t s_smoothedProcessTimeUs = 16667;
            constexpr int MAX_FPS = 60;
            constexpr int MIN_FPS = 20;
            constexpr long long MIN_FRAME_TIME_US = 1000000LL / MAX_FPS;
            constexpr long long MAX_FRAME_TIME_US = 1000000LL / MIN_FPS;

            // --- 静态状态变量（提前声明，供焦点恢复和推理失败两处使用）---
            // Track consecutive inference failures (separate from capture errors)
            static int s_inferenceFailures = 0;
            // Track recovery attempts to avoid repeated recovery attempts
            static int s_recoveryAttempts = 0;
            static std::chrono::steady_clock::time_point s_lastRecoveryTime;
            static std::chrono::steady_clock::time_point s_lastSuccessfulInferenceTime;
            static constexpr std::chrono::milliseconds RECOVERY_PROTECTION_PERIOD{500}; // 500ms保护期
            // 采集连续失败计数器（独立于推理失败计数器）
            static int s_captureFailCount = 0;

            s_prevIterStart = s_lastIterStart;
            auto iterStart = std::chrono::steady_clock::now();
            auto sinceLast = std::chrono::duration_cast<std::chrono::microseconds>(iterStart - s_lastIterStart).count();

            // 更新平滑处理时间（基于上一帧实际间隔）
            s_smoothedProcessTimeUs = (s_smoothedProcessTimeUs * 7 + sinceLast) / 8;
            long long targetFrameTimeUs = s_smoothedProcessTimeUs;
            if (targetFrameTimeUs < MIN_FRAME_TIME_US) targetFrameTimeUs = MIN_FRAME_TIME_US;
            if (targetFrameTimeUs > MAX_FRAME_TIME_US) targetFrameTimeUs = MAX_FRAME_TIME_US;

            long long sleepUs = 0;
            if (sinceLast < targetFrameTimeUs) {
                sleepUs = targetFrameTimeUs - sinceLast;
                if (sleepUs > 2000) {
                    std::this_thread::sleep_for(std::chrono::microseconds(sleepUs - 1000));
                }
            }
            s_lastIterStart = std::chrono::steady_clock::now();

            // [Debug] Log frame interval and sleep every 60 frames
            static int s_frameCount = 0;
            static auto s_fpsLogStart = std::chrono::steady_clock::now();
            static double s_lastMeasuredFps = 0.0;
            s_frameCount++;
            if (s_frameCount >= 60) {
                auto fpsNow = std::chrono::steady_clock::now();
                auto fpsElapsed = std::chrono::duration_cast<std::chrono::microseconds>(fpsNow - s_fpsLogStart).count();
                s_lastMeasuredFps = s_frameCount * 1000000.0 / fpsElapsed;
                DEBUG_LOG_FMT("[FPS] actual=%.1f target=%d sleepUs=%lld sinceLast=%lld smoothed=%lld",
                    s_lastMeasuredFps, (int)(1000000.0 / targetFrameTimeUs), sleepUs, sinceLast, s_smoothedProcessTimeUs);
                s_frameCount = 0;
                s_fpsLogStart = fpsNow;
            }

            try {
                g_crashStage.store(100);
                auto frameStart = std::chrono::steady_clock::now();

                cv::Mat displayImg;
                std::string mode;
                ONNXInference::PerformanceMetrics currentMetrics;
                bool hasMetrics = false;

                // Per-iteration capture outcome, filled in by whichever branch
                // runs and read by the instrumentation block at RENDER_FRAME.
                bool   frameCaptureOk = false;
                double frameCaptureMs = 0.0;
                bool   frameBlack = false;
                bool   frameDuplicate = false;
                cv::Mat frameForMetrics;

                auto& capture = UnifiedScreenCapture::GetInstance();

                // Track backend transitions. A DXGI -> WGC -> GDI slide is the
                // clearest signal that capture is degrading on this machine.
                static CaptureMetrics::Method s_lastMethod = CaptureMetrics::Method::Unknown;
                static CaptureMetrics::CapState s_lastState = CaptureMetrics::CapState::Unknown;
                CaptureMetrics::Method curMethod = ToMetricsMethod(capture.GetCurrentMethod());
                CaptureMetrics::CapState curState = ToMetricsState(capture.GetCaptureState());
                if (s_lastMethod != CaptureMetrics::Method::Unknown && curMethod != s_lastMethod) {
                    CaptureMetrics::GetInstance().NoteMethodChange(s_lastMethod, curMethod);
                }
                if (s_lastState != CaptureMetrics::CapState::Unknown && curState != s_lastState) {
                    CaptureMetrics::GetInstance().NoteStateChange(s_lastState, curState);
                }
                s_lastMethod = curMethod;
                s_lastState = curState;

                // Wrappers so every recovery path is counted without each call
                // site having to remember to report it.
                auto forceReinit = [&](HWND h) {
                    CaptureMetrics::GetInstance().NoteReinit("force");
                    capture.ForceReinitialize(h);
                };
                auto softReinit = [&](HWND h) {
                    CaptureMetrics::GetInstance().NoteReinit("soft");
                    capture.SoftReinitialize(h);
                };

                // === Target resolution: by process name, not by focus ===
                // The process name identifies the game; the foreground window is
                // only used as a fast path inside it. Never gate this on focus --
                // the user alt-tabbing would tear the capture session down.
                HWND gameWnd = common::findGameWindow();
                bool gameExists = (gameWnd != nullptr && IsWindow(gameWnd) && IsWindowVisible(gameWnd));
                HWND foregroundWnd = GetForegroundWindow();
                HWND foregroundRootWnd = (foregroundWnd != nullptr) ? GetAncestor(foregroundWnd, GA_ROOT) : nullptr;
                bool isGameForeground = (gameWnd != nullptr && (foregroundWnd == gameWnd || foregroundRootWnd == gameWnd));

                // Debug: log when the resolved window changes
                static HWND s_lastLoggedGameWnd = nullptr;
                if (gameWnd != s_lastLoggedGameWnd) {
                    if (gameWnd == nullptr) {
                        DEBUG_LOG_FMT("[MAIN] findGameWindow returned NULL - target not found (%ls)",
                            target::ActiveSpec().Describe().c_str());
                    } else {
                        DEBUG_LOG_FMT("[MAIN] findGameWindow resolved: %ls (visible=%d iconic=%d)",
                            target::DescribeWindow(gameWnd).c_str(),
                            IsWindowVisible(gameWnd), IsIconic(gameWnd));
                    }
                    s_lastLoggedGameWnd = gameWnd;
                }

                // Track window state changes for proper reinitialization
                static HWND s_lastGameWnd = nullptr;
                static int s_missingWindowCount = 0;
                static int s_updateTargetFailures = 0;
                static HWND s_hwndStabilityRef = nullptr;
                static int s_hwndStableFrameCount = 0;
                static bool s_wasNotForeground = false;  // 追踪焦点是否曾失去
                // 【混合显卡优化 - 长切出改进】记录失焦开始时间，焦点恢复时计算时长作为诊断
                static std::chrono::steady_clock::time_point s_lostFocusTime{};
                static bool s_hasLostFocusRecord = false;

                // 【推理异步启动期状态】
                // 问题：游戏启动时第一帧调用 tryGetAsyncClassificationResult 返回 false（推理未完成），
                //       主循环错误地认为 "推理失败"，递增 s_inferenceFailures 并进入恢复阶段。
                // 修复：引入 s_asyncInferenceNotReadyStreak 区分"推理未完成（不等价于失败）"与"推理真正失败"，
                //       避免在异步推理完成前递增失败计数。
                static int s_asyncInferenceNotReadyStreak = 0;

                // 【启动门控诊断快照】每 3 秒最多一次，统一前缀 [STARTUP-GATE]，纯日志无副作用
                static std::chrono::steady_clock::time_point s_lastGateSnapshotTime{};
                auto logStartupGate = [&](const char* gateName) {
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastGateSnapshotTime).count() < 3000) return;
                    s_lastGateSnapshotTime = now;
                    const char* methodStr = "None";
                    switch (capture.GetCurrentMethod()) {
                        case UnifiedScreenCapture::CaptureMethod::DXGI: methodStr = "DXGI"; break;
                        case UnifiedScreenCapture::CaptureMethod::WGC: methodStr = "WGC"; break;
                        case UnifiedScreenCapture::CaptureMethod::GDI: methodStr = "GDI"; break;
                    }
                    const char* stateStr = "Unknown";
                    switch (capture.GetCaptureState()) {
                        case UnifiedScreenCapture::CaptureState::Idle: stateStr = "Idle"; break;
                        case UnifiedScreenCapture::CaptureState::Capturing: stateStr = "Capturing"; break;
                        case UnifiedScreenCapture::CaptureState::ExceptionWait: stateStr = "ExceptionWait"; break;
                        case UnifiedScreenCapture::CaptureState::Paused: stateStr = "Paused"; break;
                        case UnifiedScreenCapture::CaptureState::Stopped: stateStr = "Stopped"; break;
                    }
                    DEBUG_LOG_FMT("[STARTUP-GATE] gate=%s gameWnd=%p exists=%d stable=%d/15 fg=%d method=%s state=%s",
                        gateName, (void*)gameWnd, (int)gameExists, s_hwndStableFrameCount,
                        (int)isGameForeground, methodStr, stateStr);
                };

                if (!gameExists) {
                    s_missingWindowCount++;
                    s_hwndStabilityRef = nullptr;
                    s_hwndStableFrameCount = 0;
                    capture.SetForegroundStable(false);

                    if (wasRealtime) {
                        wasRealtime = false;
                        s_hasLastInference = false;
                        CaptureMetrics::GetInstance().NoteFocusLost();
                        capture.Release();
                    }
                    displayImg = cv::Mat::zeros(480, 640, CV_8UC3);
                    cv::putText(displayImg, "Waiting for game...", cv::Point(180, 230),
                        cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 255, 255), 1);
                    cv::putText(displayImg, g_targetLabel, cv::Point(250, 260),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 1);
                    logStartupGate("no-game");
                    goto RENDER_FRAME;
                }

                // Same HWND for several frames before attaching capture (launcher/splash vs real client)
                if (gameWnd != s_hwndStabilityRef) {
                    s_hwndStabilityRef = gameWnd;
                    s_hwndStableFrameCount = 1;
                } else {
                    s_hwndStableFrameCount++;
                }
                static constexpr int kGameHwndStableFrames = 15;
                if (s_hwndStableFrameCount < kGameHwndStableFrames) {
                    displayImg = cv::Mat::zeros(480, 640, CV_8UC3);
                    cv::putText(displayImg, "Connecting to game...", cv::Point(160, 230),
                        cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 255, 255), 1);
                    cv::putText(displayImg, g_targetLabel, cv::Point(250, 260),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 1);
                    logStartupGate("unstable-hwnd");
                    goto RENDER_FRAME;
                }
                
                // 【关键修复】游戏窗口必须在前台才启动捕获
                // WGC 初始化必须在游戏窗口是前台窗口时进行，否则会捕获黑屏或崩溃
                
                if (!isGameForeground) {
                    // 游戏不在前台，重置稳定计数并等待
                    s_hwndStabilityRef = nullptr;
                    s_hwndStableFrameCount = 0;
                    capture.SetForegroundStable(false);
                    s_wasNotForeground = true;  // 标记游戏曾失去前台
                    // 【混合显卡优化 - 长切出改进】记录失焦起始时间
                    if (!s_hasLostFocusRecord) {
                        s_lostFocusTime = std::chrono::steady_clock::now();
                        s_hasLostFocusRecord = true;
                    }
                    
                    static int s_foregroundLogCounter = 0;
                    s_foregroundLogCounter++;
                    if (s_foregroundLogCounter >= 180) {  // 每3秒输出一次，避免刷屏
                        DEBUG_LOG_FMT("[FOREGROUND] Game window NOT in foreground (fg=0x%p game=0x%p root=0x%p)",
                            foregroundWnd, gameWnd, foregroundRootWnd);
                        s_foregroundLogCounter = 0;
                    }
                    
                    if (wasRealtime) {
                        wasRealtime = false;
                        s_hasLastInference = false;
                        // 【混合显卡优化】失焦时采用软暂停，不释放 DXGI/WGC 资源
                        // 原因：在混合显卡笔记本上，Release() 后焦点恢复时 Initialize() 会在
                        // GPU 迁移窗口期内调用，极易选错 adapter、拿到 stale buffer。
                        // PauseCapture 仅在状态机层禁运行采集，后台 D3D/DXGI 资源保持存活。
                        CaptureMetrics::GetInstance().NoteFocusLost();
                        system.pauseAsyncInference();
                        capture.PauseCapture();
                    }

                    displayImg = cv::Mat::zeros(480, 640, CV_8UC3);
                    cv::putText(displayImg, "Waiting for game focus...", cv::Point(140, 230),
                        cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 255, 255), 1);
                    cv::putText(displayImg, g_targetLabel, cv::Point(250, 260),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 1);
                    logStartupGate("not-foreground");
                    goto RENDER_FRAME;
                }

                capture.SetForegroundStable(true);

                // Force reinitialize if window handle changed
                if (gameWnd != s_lastGameWnd) {
                    DEBUG_LOG_FMT("[MAIN] Window changed, forcing reinit");
                    forceReinit(gameWnd);
                    s_lastGameWnd = gameWnd;
                    s_missingWindowCount = 0;
                    s_wasNotForeground = false;
                }
                // Force reinitialize when returning from background (focus recovery)
                else if (s_wasNotForeground) {
                    // 【混合显卡优化】焦点恢复时采用软重新初始化
                    // 优势：
                    //  1. 不销毁现有 DXGI/WGC 资源，避免在 GPU 迁移窗口期重新初始化
                    //  2. 内部 warmup 30 帧（~1 秒）让 GPU pipeline 稳定后才会被采纳
                    //  3. 验证失败后才 Fallback 到硬重置 ForceReinitialize
                    //  4. 同时重置 WGC 黄框友好标记，给 WGC 重新证明机会
                    // 【长切出改进】warmup 帧数根据失焦时长动态调整（30/60/120）
                    // 【长切出改进】多阶段尝试，最多 3 轮逐步扩大 warmup
                    // 【长切出改进】窗口大小变化检测，切出期间改了分辨率会跳过软恢复
                    if (s_hasLostFocusRecord) {
                        auto lostMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - s_lostFocusTime).count();
                        DEBUG_LOG_FMT("[MAIN] Focus recovered from background, lostFocusDuration=%lldms, attempting SoftReinitialize",
                            (long long)lostMs);
                        s_hasLostFocusRecord = false;
                    } else {
                        DEBUG_LOG_FMT("[MAIN] Focus recovered from background, attempting SoftReinitialize");
                    }
                    softReinit(gameWnd);
                    CaptureMetrics::GetInstance().NoteFocusRecovered();
                    s_wasNotForeground = false;

                    // 【焦点恢复优化】恢复异步推理线程（之前只是暂停，不是停止）
                    // 不需要调用 stopAsyncInference，因为失焦时只是 pause 了线程
                    system.resumeAsyncInference();

                    // 【焦点恢复优化】重置推理失败计数器，避免恢复后立即触发恢复逻辑
                    s_inferenceFailures = 0;
                    s_recoveryAttempts = 0;
                }
                
                try {
                    if (!capture.UpdateTargetWindow(gameWnd)) {
                        s_updateTargetFailures++;
                        if (s_updateTargetFailures >= 10) {
                            DEBUG_LOG_FMT("[MAIN] UpdateTargetWindow failed %d times, forcing reinit", s_updateTargetFailures);
                            forceReinit(gameWnd);
                            s_updateTargetFailures = 0;
                            s_lastGameWnd = nullptr;
                            s_missingWindowCount = 0;
                        }
                        goto RENDER_FRAME;
                    }
                    // Reset failure counter on success
                    s_updateTargetFailures = 0;
                    
                    g_crashStage.store(110);
                    
                    if (!wasRealtime) {
                        std::cout << "[SWITCHED TO REAL-TIME MODE]\n";
                        realtimeStats = PerformanceStats();
                        wasRealtime = true;
                    }
                    
                } catch (const winrt::hresult_error& e) {
                    DEBUG_LOG_FMT("[PROTECTION] WGC hresult_error during capture: %d - %s",
                        (int)e.code(), winrt::to_string(e.message()).c_str());
                    // Stop the inference thread first so it cannot race the
                    // capture resources being rebuilt underneath it.
                    system.stopAsyncInference();
                    forceReinit(gameWnd);
                    pendingForegroundGameWnd = nullptr;
                    foregroundStableSince = std::chrono::steady_clock::time_point{};
                    wasRealtime = false;
                    s_hasLastInference = false;
                    goto RENDER_FRAME;
                } catch (const std::exception& e) {
                    DEBUG_LOG_FMT("[PROTECTION] std exception during capture: %s", e.what());
                    // Stop the inference thread first so it cannot race the
                    // capture resources being rebuilt underneath it.
                    system.stopAsyncInference();
                    forceReinit(gameWnd);
                    pendingForegroundGameWnd = nullptr;
                    foregroundStableSince = std::chrono::steady_clock::time_point{};
                    wasRealtime = false;
                    s_hasLastInference = false;
                    goto RENDER_FRAME;
                } catch (...) {
                    DEBUG_LOG_FMT("[PROTECTION] Unknown exception during capture");
                    // Stop the inference thread first so it cannot race the
                    // capture resources being rebuilt underneath it.
                    system.stopAsyncInference();
                    forceReinit(gameWnd);
                    pendingForegroundGameWnd = nullptr;
                    foregroundStableSince = std::chrono::steady_clock::time_point{};
                    wasRealtime = false;
                    s_hasLastInference = false;
                    goto RENDER_FRAME;
                }

                if (g_saveRoiFrames.exchange(false)) {
                    auto systemPtr = &system;
                    std::thread([systemPtr]() {
                        try {
                            std::cout << "[ACTION] Saving ROI crops...\n";
                            auto rois = systemPtr->captureCurrentROIs();
                            std::cout << "[AI] Captured " << rois.size() << " ROIs\n";
                            if (!rois.empty()) {
                                std::vector<std::string> roiNames = { "HP", "BulletX", "BulletY", "hasBullet", "LS" };
                                bool saved = ImageSaver::SaveROIsToSubfolders(rois, roiNames, AI_SCREENSHOT_DIR);
                                if (saved) {
                                    std::cout << "[INFO] Screenshot saved successfully\n";
                                }
                            }
                            else {
                                std::cerr << "[ERROR] No ROIs captured\n";
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "[AI] Screenshot capture failed: " << e.what() << "\n";
                        } catch (...) {
                            std::cerr << "[AI] Screenshot capture failed: unknown exception\n";
                        }
                    }).detach();
                }

                mode = "[REAL]";

                if (USE_CLASSIFICATION_MODEL) {
                    try {
                        auto classStart = std::chrono::steady_clock::now();
                        g_crashStage.store(130);
                        
                        // 【第二层防护：黑屏检测与推理跳过】
                        // 在推理前先检测画面是否为全黑（游戏加载/场景切换）
                        cv::Mat preCheckFrame;
                        auto capT0 = std::chrono::steady_clock::now();
                        bool captureOk = capture.CaptureFrame(preCheckFrame);
                        frameCaptureMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - capT0).count();
                        frameCaptureOk = captureOk;
                        frameForMetrics = preCheckFrame;
                        
                        bool isAllBlack = false;
                        static int s_blackScreenCount = 0;  // 连续黑屏帧计数
                        static int s_prevBlackCount = 0;    // 上次黑屏计数（用于检测黑屏结束）
                        // 黑屏结束防抖：需要连续多帧非黑屏才能确认退出黑屏
                        // 防止画面亮度在临界值附近抖动时频繁切换状态
                        static int s_blackScreenExitStableCount = 0;
                        static constexpr int BLACK_SCREEN_EXIT_STABLE_FRAMES = 5;
                        
                        if (captureOk && !preCheckFrame.empty()) {
                            // 计算画面平均亮度
                            cv::Mat grayPreCheck;
                            if (preCheckFrame.channels() == 3) {
                                cv::cvtColor(preCheckFrame, grayPreCheck, cv::COLOR_BGR2GRAY);
                            } else {
                                grayPreCheck = preCheckFrame;
                            }
                            
                            double meanBrightness = cv::mean(grayPreCheck)[0];
                            
                            // 判定条件：使用略微宽松的阈值，配合窗口尺寸检查
                            // 原阈值 1.0 在某些游戏场景（加载画面略暗但不全黑）下可能造成误判
                            constexpr double BLACK_SCREEN_BRIGHTNESS_THRESHOLD = 2.0;
                            if (meanBrightness < BLACK_SCREEN_BRIGHTNESS_THRESHOLD && preCheckFrame.cols > 100 && preCheckFrame.rows > 100) {
                                isAllBlack = true;
                                frameBlack = true;
                                s_blackScreenCount++;
                                s_blackScreenExitStableCount = 0;  // 重置退出防抖计数
                                
                                if (s_blackScreenCount == 1) {
                                    DEBUG_LOG_FMT("[BLACKSCREEN] Black screen entered (brightness=%.2f) - capture paused", meanBrightness);
                                    CaptureMetrics::GetInstance().NoteBlackEpisode();
                                }
                                
                                capture.PauseCapture();
                                
                                // 【游戏启动优化】黑屏暂停时周期性 Resume 一次来维持 DXGI 管道活性。
                                // DXGI Desktop Duplication 的 AcquireNextFrame 长时间不调用后，
                                // 内部帧缓冲可能超时/stale，导致 Resume 后首帧采集失败。
                                // 每 ~5 秒（约 300 帧 @ 16ms/帧）Resume 一次采一帧再 Pause。
                                if (s_blackScreenCount > 0 && s_blackScreenCount % 300 == 0) {
                                    DEBUG_LOG_FMT("[BLACKSCREEN] Periodic resume to keep DXGI pipeline alive (paused %d frames)",
                                        s_blackScreenCount);
                                    capture.ResumeCapture();
                                    // 采一帧维持管道活性，结果不用于推理
                                    cv::Mat keepAliveFrame;
                                    capture.CaptureFrame(keepAliveFrame);
                                    if (!keepAliveFrame.empty()) {
                                        displayImg = keepAliveFrame.clone();
                                    }
                                    capture.PauseCapture();
                                }
                                
                                // 每100帧输出一次日志，避免日志刷屏
                                if (s_blackScreenCount % 100 == 1) {
                                    DEBUG_LOG_FMT("[MAIN] Black screen detected (brightness=%.2f, %dx%d) - capture paused", 
                                        meanBrightness, preCheckFrame.cols, preCheckFrame.rows);
                                }
                                
                                displayImg = preCheckFrame.clone();
                                cv::putText(displayImg, "Game Loading / Black Screen [PAUSED]", 
                                    cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);
                                cv::putText(displayImg, "Waiting for game to render...", 
                                    cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(200, 200, 200), 1);
                                
                                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                            } else if (s_blackScreenCount > 0) {
                                // 防抖：只有连续多帧非黑屏才确认退出黑屏状态
                                // 防止画面亮度抖动导致黑屏/非黑屏状态频繁切换
                                s_blackScreenExitStableCount++;
                                if (s_blackScreenExitStableCount >= BLACK_SCREEN_EXIT_STABLE_FRAMES) {
                                    if (s_blackScreenCount != s_prevBlackCount) {
                                        DEBUG_LOG_FMT("[MAIN] Black screen ended after %d frames (stable exit) - resuming capture", 
                                            s_blackScreenCount);
                                        s_prevBlackCount = s_blackScreenCount;
                                        

                                        capture.ResumeCapture();
                                    }
                                    s_blackScreenCount = 0;
                                    s_blackScreenExitStableCount = 0;
                                }
                            } else {
                                s_blackScreenExitStableCount = 0;
                            }
                        }
                        
                        // 如果是全黑，跳过推理，直接进入显示逻辑
                        if (isAllBlack) {
                            logStartupGate("black-screen-paused");
                            goto RENDER_FRAME;  // 跳转到显示部分
                        }

                        // 【关键修复】采集失败时跳过推理，避免空帧推理浪费时间
                        // 并误增推理失败计数器导致 "INFERENCE RETRY" / "MODEL ERROR" 交替显示
                        if (!captureOk || preCheckFrame.empty()) {
                            s_captureFailCount++;
                            logStartupGate("capture-failed");
                            if (s_captureFailCount % 30 == 1) {
                                DEBUG_LOG_FMT("[STARTUP-GATE] capture-failed detail: fails=%d method=%d state=%d",
                                    s_captureFailCount, (int)capture.GetCurrentMethod(), (int)capture.GetCaptureState());
                            }

                            // 焦点切回后如果采集因黑屏被暂停，重置黑屏状态并尝试恢复采集
                            // 避免 Paused 状态卡死：PauseCapture 后 CaptureFrame 返回 false，
                            // 但 ResumeCapture 需要 captureOk=true 才能触发，形成死锁
                            // 【修复】移除 s_captureFailCount <= 3 限制：该限制导致 4 次黑屏暂停循环后
                            //   ResumeCapture 永不再被调用，系统永久卡在 Paused 状态。
                            //   黑屏期间每帧 Resume→Capture(黑)→Pause→Fail→Resume 循环是正确行为：
                            //   持续检测黑屏是否结束。s_captureFailCount 在此重置避免退避 sleep 干扰。
                            if (s_blackScreenCount > 0) {
                                DEBUG_LOG_FMT("[MAIN] Resetting black screen state during capture failure (blackFrames=%d, failCount=%d)",
                                    s_blackScreenCount, s_captureFailCount);
                                capture.ResumeCapture();
                                s_blackScreenCount = 0;
                                s_blackScreenExitStableCount = 0;
                                s_prevBlackCount = 0;
                                s_captureFailCount = 0;  // 重置：Paused 导致的失败不是真实采集失败，避免退避 sleep
                            }

                            // 显示采集重试画面
                            if (s_hasLastInference && !s_lastMosaic.empty()) {
                                displayImg = s_lastMosaic.clone();
                            } else {
                                displayImg = cv::Mat::zeros(480, 640, CV_8UC3);
                                cv::putText(displayImg, "WAITING FOR GAME...", cv::Point(120, 240),
                                    cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(255, 255, 255), 1);
                            }
                            cv::putText(displayImg, "CAPTURE RETRY...", cv::Point(150, 300),
                                cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);

                            // 采集持续失败超过阈值时，不再调用 ForceReinitialize。
                            // ForceReinitialize 会重置 DowngradeMethod 的内部状态（Release+Initialize），
                            // 但不干预 m_foregroundReinitAttempts（已修复），让 DowngradeMethod 自行
                            // 完成 reinit→downgrade 流程。主循环只需等待 DowngradeMethod 降级到 WGC/GDI。
                            // 此处仅输出日志，不执行任何恢复操作。
                            if (s_captureFailCount % 60 == 30) {
                                DEBUG_LOG_FMT("[MAIN] Capture failed %d consecutive times - DowngradeMethod should be handling recovery", s_captureFailCount);
                            }

                            // 【游戏启动优化】采集失败时降低循环频率，避免空转烧 CPU
                            // 游戏启动阶段可能长时间拿不到图（DXGI 未就绪/窗口最小化等），
                            // 全速空转不仅浪费 CPU 还会导致 debug 模式 FPS 极低（大量无效日志）
                            // 使用渐进退避：前几次快一点，稳定失败后降低到 ~10 FPS
                            int sleepMs = 10;
                            if (s_captureFailCount > 100) {
                                sleepMs = 100;   // 稳定失败：~10 FPS 轮询
                            } else if (s_captureFailCount > 30) {
                                sleepMs = 50;    // 中期失败：~20 FPS 轮询
                            } else if (s_captureFailCount > 10) {
                                sleepMs = 30;    // 早期失败：~33 FPS 轮询
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

                            s_hasLastInference = false;
                            s_lastMosaic.release();

                            if (s_captureFailCount % 30 == 1) {
                                DEBUG_LOG_FMT("[MAIN] Capture failed, skipping inference (fails: %d)", s_captureFailCount);
                            }
                            goto RENDER_FRAME;
                        }

                        // 采集成功，重置采集失败计数器
                        s_captureFailCount = 0;

                        // 【异步流水线】先检查上一帧的推理结果是否完成
                        cv::Mat mosaic;
                        std::vector<ClassificationResult> classifications;
                        ONNXInference::PerformanceMetrics metrics;
                        
                        bool resultReady = system.tryGetAsyncClassificationResult(classifications, metrics, mosaic);
                        if (!resultReady) {
                            // 推理还未完成，使用上一帧结果（如果有）
                            if (s_hasLastInference && !s_lastMosaic.empty()) {
                                mosaic = s_lastMosaic.clone();
                                classifications = s_lastClassifications;
                            }
                        }
                        
                        // 【异步流水线】提交当前帧的异步推理
                        system.submitAsyncClassification(preCheckFrame);
                        
                        auto classEnd = std::chrono::steady_clock::now();
                        auto classDuration = std::chrono::duration_cast<std::chrono::milliseconds>(classEnd - classStart).count();
                        DEBUG_LOG_FMT("[DEBUG] Classification took %d ms", classDuration);
                
                // Function to check if window is in fullscreen mode
                auto isFullscreenWindow = [](HWND hwnd) -> bool {
                    if (!hwnd) return false;
                    LONG style = GetWindowLong(hwnd, GWL_STYLE);
                    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
                    
                    // Check for typical fullscreen flags
                    // WS_POPUP and no WS_CAPTION/BORDER are typical for fullscreen
                    bool isPopup = (style & WS_POPUP) != 0;
                    bool hasCaption = (style & WS_CAPTION) != 0;
                    bool hasBorder = (style & WS_BORDER) != 0;
                    bool hasFrame = (style & WS_THICKFRAME) != 0;
                    
                    // Also check window dimensions vs screen dimensions
                    RECT windowRect, monitorRect;
                    GetWindowRect(hwnd, &windowRect);
                    int windowWidth = windowRect.right - windowRect.left;
                    int windowHeight = windowRect.bottom - windowRect.top;
                    
                    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
                    if (GetMonitorInfo(monitor, &monitorInfo)) {
                        int monitorWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
                        int monitorHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
                        
                        // Window covers at least 95% of monitor (accounting for possible offsets)
                        bool coversMonitor = (windowWidth >= monitorWidth * 0.95 && 
                                             windowHeight >= monitorHeight * 0.95);
                        
                        return (isPopup && !hasCaption && !hasBorder && !hasFrame) || coversMonitor;
                    }
                    
                    return isPopup && !hasCaption && !hasBorder && !hasFrame;
                };
                
                // Check if game window is in fullscreen mode
                bool isFullscreen = false;
                if (gameWnd != nullptr) {
                    isFullscreen = isFullscreenWindow(gameWnd);
                }

                if (mosaic.empty()) {
                    g_crashStage.store(131);

                    // 【核心修复】区分"推理未完成"与"推理真正失败"
                    // 问题：异步推理首帧 resultReady=false 时 mosaic 保持空，递增 s_inferenceFailures。
                    //       修复：只在 s_asyncInferenceNotReadyStreak 超时时才计入失败。
                    //       逻辑是 "被零帧型"：首帧一定未完成（streak=1），后续才是真正运行。
                    s_asyncInferenceNotReadyStreak++;
                    if (s_asyncInferenceNotReadyStreak < 5) {
                        // 推理仍在进行中，尚未完成。这是正常状态，不计入失败。
                        if (s_asyncInferenceNotReadyStreak == 1) {
                            DEBUG_LOG_FMT("[MAIN] First async inference frame not ready yet (normal startup delay), not counting as failure");
                        }
                        // 不递增 s_inferenceFailures、不触发恢复阶段。
                        // 但仍显示 "WAITING FOR GAME..." 友好提示，区分于 "INFERENCE RETRY"。
                        if (!preCheckFrame.empty()) {
                            displayImg = preCheckFrame.clone();
                        } else {
                            displayImg = cv::Mat::zeros(480, 640, CV_8UC3);
                        }
                        cv::putText(displayImg, "WAITING FOR INFERENCE...", cv::Point(120, 240),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);
                        cv::putText(displayImg, "(Startup delay - first async result)", cv::Point(80, 280),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(200, 200, 200), 1);
                        goto RENDER_FRAME;
                    }

                    s_inferenceFailures++;
                    
                    if (!preCheckFrame.empty()) {
                        displayImg = preCheckFrame.clone();
                    } else if (s_hasLastInference && !s_lastMosaic.empty()) {
                        displayImg = s_lastMosaic.clone();
                        cv::putText(displayImg, "RECONNECTING...", cv::Point(150, 300),
                            cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2);
                    } else {
                        displayImg = cv::Mat::zeros(480, 640, CV_8UC3);
                        cv::putText(displayImg, "WAITING FOR GAME...", cv::Point(120, 240),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(255, 255, 255), 1);
                    }
                    
                    // 【改进】渐进式恢复机制 - 根据失败次数和是否全屏采取不同措施
                    bool recoveryAttempted = false;
                    bool isSeriousFailure = (s_inferenceFailures > 30);
                    
                    // 准备显示消息
                    if (!isSeriousFailure) {
                        cv::putText(displayImg, "INFERENCE RETRY...", cv::Point(150, 300),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);
                    } else {
                        cv::putText(displayImg, "MODEL ERROR - CHECKING...", cv::Point(100, 300),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 165, 255), 2);
                    }
                    
                    // 添加全屏模式信息到显示
                    if (isFullscreen) {
                        cv::putText(displayImg, "[FULLSCREEN]", cv::Point(10, 350),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(255, 100, 0), 1);
                    }
                    
                    // 【关键修复】根据设计文档实施渐进式恢复策略
                    auto now = std::chrono::steady_clock::now();
                    auto recoveryCooldown = std::chrono::milliseconds(1000); // 1秒冷却时间
                    bool canAttemptRecovery = (s_recoveryAttempts == 0) || 
                                             (now - s_lastRecoveryTime > recoveryCooldown);
                    
                    if (canAttemptRecovery) {
                        // 【优化】更保守的恢复策略 - 避免过早干预正常推理
                        // 注意：过度恢复会导致分类抖动，因为ResetInferenceSession()会破坏推理会话状态
                        
                        // 阶段1：轻微失败（15-25帧）- 仅在全屏模式下尝试轻微恢复
                        if (s_inferenceFailures >= 15 && s_inferenceFailures < 25 && isFullscreen) {
                            DEBUG_LOG_FMT("[MAIN] Early fullscreen recovery attempt (%d failures)", 
                                        s_inferenceFailures);
                            
                            // 仅在真正全屏且持续失败时才尝试
                            forceReinit(gameWnd);
                            s_lastGameWnd = gameWnd; // 保持窗口绑定
                            s_recoveryAttempts++;
                            s_lastRecoveryTime = now;
                            recoveryAttempted = true;
                        }
                        // 阶段2：中等失败（25-35帧）- 谨慎尝试恢复
                        else if (s_inferenceFailures >= 25 && s_inferenceFailures < 35) {
                            DEBUG_LOG_FMT("[MAIN] Moderate failure recovery (%d failures, fullscreen=%d)", 
                                        s_inferenceFailures, (int)isFullscreen);
                            
                            // 对于全屏模式：重新初始化捕获
                            if (isFullscreen) {
                                forceReinit(gameWnd);
                                s_lastGameWnd = gameWnd;
                                DEBUG_LOG_FMT("[MAIN] Fullscreen capture reinitialized");
                            }
                            // 对于窗口模式：只重置窗口绑定，不重置推理会话
                            else {
                                DEBUG_LOG_FMT("[MAIN] Windowed mode - only resetting window binding");
                                s_lastGameWnd = gameWnd;
                            }
                            
                            s_recoveryAttempts++;
                            s_lastRecoveryTime = now;
                            recoveryAttempted = true;
                        }
                        // 阶段3：严重失败（35-45帧）- 仅在绝对必要时重置推理会话
                        else if (s_inferenceFailures >= 35 && s_inferenceFailures < 45) {
                            DEBUG_LOG_FMT("[MAIN] Serious failure recovery (%d failures, fullscreen=%d)", 
                                        s_inferenceFailures, (int)isFullscreen);
                            
                            // 【关键】推理会话重置是昂贵的操作，只在绝对必要时执行
                            // 检查是否真的是持久性问题，而不是临时抖动
                            if (isFullscreen && s_recoveryAttempts >= 2) {
                                // 只在全屏模式且之前的恢复尝试失败后才重置推理会话
                                bool resetOk = system.ResetInferenceSession();
                                DEBUG_LOG_FMT("[MAIN] Fullscreen inference session reset: %s", 
                                            resetOk ? "OK" : "FAILED");
                            } else {
                                DEBUG_LOG_FMT("[MAIN] Avoiding expensive inference session reset (windowed or first attempt)");
                                // 只重置捕获，不重置推理会话
                                forceReinit(gameWnd);
                                s_lastGameWnd = gameWnd;
                            }
                            
                            s_recoveryAttempts++;
                            s_lastRecoveryTime = now;
                            recoveryAttempted = true;
                        }
                        // 阶段4：非常严重失败（≥45帧）- 最后手段
                        else if (s_inferenceFailures >= 45) {
                            DEBUG_LOG_FMT("[MAIN] Critical failure - last resort recovery (%d failures, fullscreen=%d)", 
                                        s_inferenceFailures, (int)isFullscreen);
                            
                            // 执行完整恢复，但保留推理会话状态作为最后手段
                            s_inferenceFailures = 0;
                            s_recoveryAttempts = 0;
                            
                            // 【重要】先尝试不重置推理会话的恢复
                            forceReinit(gameWnd);
                            s_lastGameWnd = gameWnd;
                            
                            // 如果仍然失败，才考虑重置推理会话（在下一轮中）
                            DEBUG_LOG_FMT("[MAIN] Critical recovery executed (inference session preserved for stability)");
                            
                            if (isFullscreen) {
                                cv::putText(displayImg, "FULLSCREEN RECOVERY (STABLE)", cv::Point(60, 380),
                                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 200, 100), 2);
                            }
                            
                            recoveryAttempted = true;
                        }
                    }
                    
                    // 显示恢复尝试信息
                    if (recoveryAttempted) {
                        std::string recoveryMsg = "Recovery attempt #" + std::to_string(s_recoveryAttempts);
                        cv::putText(displayImg, recoveryMsg, cv::Point(10, 320),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 0), 1);
                    }
                    
                    DEBUG_LOG_FMT("[MAIN] Inference failed (failures: %d, fullscreen: %d, recovery attempts: %d)",
                                s_inferenceFailures, (int)isFullscreen, s_recoveryAttempts);

                    s_hasLastInference = false;
                    s_lastMosaic.release();
                } else {
                    g_crashStage.store(132);
                    currentMetrics = metrics;
                    hasMetrics = true;
                    realtimeStats.add(metrics);
                    // 【核心修复】推理成功 → 重置 "推理未完成" 状态
                    s_asyncInferenceNotReadyStreak = 0;
                    if (!mosaic.empty() && !classifications.empty()) {
                        s_lastMosaic = mosaic;
                        s_lastClassifications = classifications;
                        s_hasLastInference = true;
                    }
                    // Reset inference failure counter on successful capture
                    s_inferenceFailures = 0;
                            displayImg = mosaic.clone();

                            int fontFace = cv::FONT_HERSHEY_SIMPLEX;
                            double fontScale = 0.5;
                            int thickness = 1;
                            int lineHeight = 18;
                            int startX = 0;
                            int startY = displayImg.rows - 230;

                            for (size_t i = 0; i < classifications.size(); ++i) {
                                const auto& cls = classifications[i];
                                std::ostringstream ss;
                                
                                // 将索引映射到实际的ROI名称
                                std::string roiName;
                                switch (static_cast<ButtonIndex>(i)) {
                                    case ButtonIndex::LsRs:   roiName = "LsRs"; break;
                                    case ButtonIndex::LB_1:   roiName = "LB_1"; break;
                                    case ButtonIndex::RB_1:   roiName = "RB_1"; break;
                                    case ButtonIndex::LT_1:   roiName = "LT_1"; break;
                                    case ButtonIndex::RT_1:   roiName = "RT_1"; break;
                                    case ButtonIndex::LB_2:   roiName = "LB_2"; break;
                                    case ButtonIndex::LT_2:   roiName = "LT_2"; break;
                                    case ButtonIndex::RT_2:   roiName = "RT_2"; break;
                                    default:                  roiName = "ROI" + std::to_string(i); break;
                                }
                                
                                ss << roiName << ": " << cls.class_name << " " << std::fixed << std::setprecision(2) << cls.confidence;
                                std::string label = ss.str();

                                cv::Size textSize = cv::getTextSize(label, fontFace, fontScale, thickness, nullptr);
                                cv::Point textOrg(startX, startY + static_cast<int>(i) * lineHeight);
                                cv::Rect bgRect(textOrg.x - 4, textOrg.y - textSize.height - 4, textSize.width + 8, textSize.height + 6);
                                cv::rectangle(displayImg, bgRect, cv::Scalar(0, 0, 0), cv::FILLED);
                                cv::putText(displayImg, label, textOrg, fontFace, fontScale, cv::Scalar(255, 255, 255), thickness);
                            }

                            constexpr float CONF_THRESH = 0.6f;

                            if (classifications.size() < static_cast<size_t>(ButtonIndex::COUNT)) {
                                g_crashStage.store(133);
                                if (s_hasLastInference && s_lastClassifications.size() >= static_cast<size_t>(ButtonIndex::COUNT)) {
                                    classifications = s_lastClassifications;
                                }
                            }

                            // Nothing is acted upon here any more. The harness only
                            // cares that 8 classifications came back; for a non-HoK
                            // target they will mostly read "Other", which is expected.
                        }

                    } catch (const std::exception& e) {
                        std::cerr << "[CLASSIFICATION] Exception: " << e.what() << "\n";
                        MyDbgStr("[CLASS] Exception: %s", e.what());
                        if (displayImg.empty() && s_hasLastInference) {
                            displayImg = s_lastMosaic.clone();
                        } else if (displayImg.empty()) {
                            displayImg = cv::Mat::zeros(480, 640, CV_8UC3);
                            cv::putText(displayImg, "Inference Error", cv::Point(200, 240),
                                cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 0, 255), 2);
                        }
                    } catch (...) {
                        std::cerr << "[CLASSIFICATION] Unknown exception\n";
                        if (displayImg.empty() && s_hasLastInference) {
                            displayImg = s_lastMosaic.clone();
                        } else if (displayImg.empty()) {
                            displayImg = cv::Mat::zeros(480, 640, CV_8UC3);
                            cv::putText(displayImg, "Unknown Error", cv::Point(200, 240),
                                cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 0, 255), 2);
                        }
                }
            }

RENDER_FRAME:
                // ===== stability instrumentation =====
                // Every exit path lands here, so this single block accounts for
                // frames that were skipped by the black-screen / no-window /
                // capture-failure branches too.
                {
                    auto& m = CaptureMetrics::GetInstance();
                    CaptureMetrics::FrameSample fs;
                    fs.captureMs = frameCaptureMs;
                    fs.inferenceMs = hasMetrics ? currentMetrics.inference_time_ms : 0.0;
                    fs.method = ToMetricsMethod(capture.GetCurrentMethod());
                    fs.state = ToMetricsState(capture.GetCaptureState());
                    fs.captureOk = frameCaptureOk;
                    fs.black = frameBlack;
                    fs.duplicate = frameDuplicate;
                    fs.width = frameForMetrics.cols;
                    fs.height = frameForMetrics.rows;
                    m.OnFrame(fs, frameForMetrics);
                }

                // 通用模式：Release 和 Debug 都显示 OpenCV 窗口
                if (!displayImg.empty()) {
                    g_crashStage.store(900);
                    {
                        std::string provider = system.getScreenshotProviderName();
                        static std::string s_prevProvider;
                        if (provider != s_prevProvider) {
                            if (LogConfigReader::m_logTarget.logConsole.load()) {
                                std::cout << "[SCREENSHOT] Active provider: " << provider << "\n";
                            }
                            s_prevProvider = provider;
                        }
                    }
                    CaptureMetrics::GetInstance().DrawHud(displayImg);
                    g_crashStage.store(902);
                    try {
                        // OpenCV window is only created/shown when config.ini [Log] show = true.
                        // When off the window stays hidden so it never steals game focus.
                        const bool showDetection = LogConfigReader::m_logTarget.showDetection.load();
                        if (!windowReady && showDetection) {
                            g_crashStage.store(901);
                            cv::namedWindow(WIN_NAME, cv::WINDOW_NORMAL | cv::WINDOW_GUI_NORMAL);
                            cv::resizeWindow(WIN_NAME, 640, 640);
                            // Prevent the OpenCV window from stealing focus from the game
                            HWND cvWnd = FindWindowW(NULL, strconv::ToWide(WIN_NAME).c_str());
                            if (cvWnd) {
                                // 【关键修复】SWP_NOACTIVATE 防止OpenCV窗口抢占游戏前台焦点
                                // 缺少此标志会导致每次imshow时OpenCV窗口被激活，游戏失焦后触发StopAsyncInference+capture.Release
                                SetWindowPos(cvWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                            }
                            windowReady = true;
                        }
                        if (showDetection) {
                            cv::imshow(WIN_NAME, displayImg);
                        }
                    } catch (const cv::Exception&) {
                        g_crashStage.store(903);
                        windowReady = false;
                    }
                }

                static auto lastLog = std::chrono::steady_clock::now();
SKIP_INFERENCE_AND_DISPLAY:  // 【第二层防护】黑屏时跳转到这里，跳过推理直接显示
                auto logNow = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(logNow - lastLog).count() >= 2) {
                    if (LogConfigReader::m_logTarget.logConsole.load()) {
                        if (wasRealtime && realtimeStats.count > 0) {
                            std::cout << "[PERF REAL] Avg Total: " << std::fixed << std::setprecision(2)
                                << realtimeStats.get_avg_total() << "ms | "
                                << "Avg Inf: " << realtimeStats.get_avg_inference() << "ms | "
                                << "FPS: " << std::setprecision(1) << s_lastMeasuredFps
                                << "\n";
                        }
                    }
                    lastLog = logNow;
                }

                // waitKey must run every frame to let OpenCV process window
                // messages (drag, close, key presses) and support 'q'/ESC quit.
                // It is only used when the debug window is enabled (show = true).
                if (LogConfigReader::m_logTarget.showDetection.load()) {
                    int key = cv::waitKey(1);
                    if (key == 'q' || key == 27) {
                        break;
                    }
                    if (key == 's' || key == 'S') {
                        g_saveRoiFrames.store(true);
                    }
                } else {
                    // No window: use ctrl+c to quit. Extra delay prevents pure
                    // CPU spinning in Release builds.
#ifndef _DEBUG
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
                }

            } catch (const winrt::hresult_error& e) {
                std::cerr << "[MAIN LOOP] hresult_error: " << winrt::to_string(e.message()) << "\n";
                MyDbgStr("[MAIN] hresult_error: %s", winrt::to_string(e.message()).c_str());
                // A WGC failure here means the capture session is unrecoverable;
                // rebuild it from scratch and let metrics count the event.
                CaptureMetrics::GetInstance().NoteReinit("force");
                UnifiedScreenCapture::GetInstance().ForceReinitialize(
                    UnifiedScreenCapture::GetInstance().GetCurrentWindow());
                pendingForegroundGameWnd = nullptr;
                foregroundStableSince = std::chrono::steady_clock::time_point{};
            } catch (const std::exception& e) {
                std::cerr << "[MAIN LOOP] Exception: " << e.what() << "\n";
                MyDbgStr("[MAIN] Exception: %s", e.what());
            } catch (...) {
                std::cerr << "[MAIN LOOP] Unknown exception\n";
            }
        }

        // ===== SHUTDOWN =====
        // Nothing keeps a hardware effect alive any more, so this is much simpler
        // than the parent project: stop the inference thread, tear the capture
        // session down, flush the stability report.
        system.stopAsyncInference();
        UnifiedScreenCapture::GetInstance().Release();
        DEBUG_LOG_FMT("[SHUTDOWN] capture released");

        cv::destroyAllWindows();
        std::cout << "\nExited cleanly.\n";

        if (realtimeStats.count > 0) {
            std::cout << "\n=== FINAL REAL-TIME PERFORMANCE ===\n";
            std::cout << "Samples: " << realtimeStats.count << "\n";
            std::cout << "Average Total Latency: " << std::fixed << std::setprecision(2)
                << realtimeStats.get_avg_total() << " ms\n";
            std::cout << "Average Inference Latency: " << realtimeStats.get_avg_inference() << " ms\n";
            std::cout << "Average FPS: " << std::setprecision(1) << realtimeStats.get_fps() << "\n";
        }

        // Whole-run stability summary: this is the thing you compare between
        // capture-scheduling variants.
        if (CaptureMetrics::GetInstance().IsEnabled()) {
            std::cout << "\n=== FINAL CAPTURE STABILITY ===\n";
            std::cout << CaptureMetrics::GetInstance().FormatReport("[SUMMARY]") << "\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "\n FATAL ERROR: " << e.what() << std::endl;
        std::cerr << "Press Enter to exit...";
        std::cin.get();
        return -1;
    } catch (...) {
        std::cerr << "\n Unknown exception occurred!" << std::endl;
        std::cin.get();
        DEBUG_LOG_SHUTDOWN();
        return -1;
    }

    DEBUG_LOG("Application exiting");
    DEBUG_LOG_SHUTDOWN();
    EndTextLogThread();

    // Flushes and closes the per-frame CSV when it was enabled.
    CaptureMetrics::GetInstance().Shutdown();

#ifdef _WIN32
    // Restore default scheduler resolution
    timeEndPeriod(1);
#endif

    return 0;
}