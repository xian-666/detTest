// UnifiedScreenCapture.h
#pragma once

#include "WGCScreenCapture.h"
#include "DXGIScreenCapture.h"
#include "ScreenShot.h"
#include "GpuEnvironmentProbe.h"
//#include "../../Component/Log/debugLog.h"
#include <opencv2/opencv.hpp>
#include <string>
#include <Windows.h>
#include <chrono>
#include <atomic>

typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

class UnifiedScreenCapture {
public:
    // Fixed priority order: DXGI > WGC > GDI
    enum class CaptureMethod {
        DXGI,   // Desktop Duplication API - highest priority, global capture, D3D12 compatible
        WGC,    // Windows Graphics Capture - window capture
        GDI     // Traditional GDI screenshot - fallback only
    };

    // Capture state machine
    enum class CaptureState {
        Idle,           // Not initialized or released
        Capturing,      // Actively capturing frames
        ExceptionWait,  // Waiting after error before retry
        Paused,         // Paused during game CG/loading screens
        Stopped         // Permanently stopped after all methods exhausted
    };

    static UnifiedScreenCapture& GetInstance() {
        static UnifiedScreenCapture instance;
        return instance;
    }

    // Initialize capture system - auto-selects best method by priority order
    bool Initialize(HWND hWnd, bool captureClientAreaOnly = true);

    // Initialize with WGC forced (skip DXGI) - used during startup grace period
    // to avoid DXGI ACCESS_LOST when the game window first appears.
    bool InitializeWGCForced(HWND hWnd, bool captureClientAreaOnly = true);

    // Capture a single frame with full state machine management
    bool CaptureFrame(cv::Mat& outFrame);

    // Update target window handle
    bool UpdateTargetWindow(HWND hWnd);

    // Get current capture method
    CaptureMethod GetCurrentMethod() const { return m_currentMethod; }

    // Get current capture state
    CaptureState GetCaptureState() const { return m_state; }

    // Release all resources
    void Release();

    // Set image processing config (forwards to active backend)
    void SetImageProcessConfig(const WGCScreenCapture::ImageProcessConfig& config);

    // Get current window handle
    HWND GetCurrentWindow() const { return m_currentWindow; }

    // Check if capture is actively running (for external monitoring)
    bool IsCapturing() const { return m_state == CaptureState::Capturing && m_initialized; }

    // Compatibility no-op for legacy foreground-state tracking.
    void SetForegroundStable(bool) {}
    bool IsForegroundStable() const { return false; }

    // Pause/resume for game CG/loading screens
    void PauseCapture();
    void ResumeCapture();

    // Force reinitialize (e.g., after focus switch recovery)
    void ForceReinitialize(HWND hWnd = nullptr);

    // [Hybrid GPU optimization] Soft-preferred re-initialization on focus recovery
    // Difference from ForceReinitialize: first tries ResumeCapture + warmup validation,
    // and only falls back to Release+Initialize when validation fails.
    // This is the key optimization for focus switching on hybrid-GPU laptops: avoids
    // destructive operations like Release/Initialize during the GPU migration window.
    // Returns true if restored to the Capturing state; false means the caller needs to
    // fall back to ForceReinitialize or wait for the next retry.
    //
    // [Long switch-out improvement] Dynamic warmup frames: choose 30/60/120 frames based on focus-loss duration
    // [Long switch-out improvement] Multi-stage attempts: warmup frames increase per round, up to 3 rounds
    // [Long switch-out improvement] Window size change detection: detect resolution changes during switch-out
    bool SoftReinitialize(HWND hWnd);

    // [WGC optimization] Reset the WGC yellow-frame friendly flag
    // Called on focus recovery, giving WGC another chance to prove it is not yellow-framed.
    void ResetWGCFriendlyFlag() { m_wgcFriendly = true; }

    // [Hybrid GPU optimization] Check whether the currently captured frame is "alive"
    // Detects pure black/pure white + frame-to-frame difference (guards against stale buffers or wrong GPU)
    bool IsFrameAlive(const cv::Mat& frame) const;

    // [Hybrid GPU optimization] Get the description of the currently selected GPU adapter (for diagnostic logs)
    void LogAdapterDiagnostics(HWND hWnd) const;

    bool IsWindows10BuildOrGreater(DWORD build);
    bool IsRealWindows10BuildOrGreater(DWORD build);


private:
    UnifiedScreenCapture()
        : m_currentWindow(nullptr)
        , m_currentMethod(CaptureMethod::DXGI)
        , m_captureClientAreaOnly(true)
        , m_initialized(false)
        , m_state(CaptureState::Idle)
        , m_consecutiveErrors(0)
        , m_lastErrorTime(std::chrono::steady_clock::now())
        , m_pausedSince(std::chrono::steady_clock::time_point{})
        , m_stoppedSince(std::chrono::steady_clock::time_point{}) {}

    ~UnifiedScreenCapture() {
        Release();
    }

    UnifiedScreenCapture(const UnifiedScreenCapture&) = delete;
    UnifiedScreenCapture& operator=(const UnifiedScreenCapture&) = delete;

    // --- Initialization by fixed priority ---
    bool TryInitializeDXGI(HWND hWnd);     // Priority 1: DXGI global capture
    bool TryInitializeWGC(HWND hWnd);      // Priority 2: WGC window capture
    bool TryInitializeGDI();               // Priority 3: GDI fallback

    // [Compat routing] Legacy initialization path -- line-by-line port of the original Initialize() logic,
    // calling convention / parameters / return semantics fully unchanged. When enhancement conditions
    // are not hit, Initialize() delegates directly to it; the execution path is binary-identical to
    // before the change (no added per-frame overhead).
    bool InitializeLegacy(HWND hWnd, bool captureClientAreaOnly);

    // [Compat routing] Enhanced initialization path -- the five game-scenario-driven enhancements exist only here:
    //   1) HDR / pixel format safe conversion (Shader / cvtColor)
    //   2) DDA frame liveness detection (content hash + UI static region mask exclusion)
    //   3) Active display-mode change monitoring (WM_DISPLAYCHANGE / lightweight polling)
    //   4) Cross-adapter safe texture copy (Staging Texture + shared handle)
    //   5) GDI smart throttling and compensation (dynamic capture interval + confidence flag)
    // Only invoked when Initialize() hits a high-risk scenario at its entry (compat_mode=auto
    // and the probe triggers) or when the config forces force_enhanced.
    bool InitializeEnhanced(HWND hWnd, bool captureClientAreaOnly);

    // --- State machine transitions ---
    void TransitionTo(CaptureState newState);
    bool ShouldRetry() const;
    bool ShouldDowngrade() const;

    // --- Frame validation ---
    bool ValidateFrame(const cv::Mat& frame) const;

    // --- Method switching (internal only) ---
    bool SwitchToMethod(CaptureMethod method, HWND hWnd);
    bool DowngradeMethod(HWND hWnd);

    // --- Backend-specific capture ---
    bool CaptureFrameDXGI(cv::Mat& outFrame);
    bool CaptureFrameWGC(cv::Mat& outFrame);
    bool CaptureFrameGDI(cv::Mat& outFrame);

    // [Diag-v2] Frame source of the last backend read, for the throttled
    // black/empty frame diagnostic logs ("new"/"cached"/"none"; "n/a" for non-WGC).
    const char* FrameSourceStr() const;

    // [SelfHeal-v2] Consecutive black/empty frame tracking -> WGC session rebuild.
    // Black/empty frames are intentionally "free" (no error count) because they are
    // transient on healthy machines - but that also means a degraded WGC session
    // (e.g. intermittent black frames from iGPU shared-memory starvation on a 96%-RAM
    // laptop) can flap forever with no recovery. This adds a dedicated counter with
    // three guardrails (see implementation for details):
    //   1) Counts only in WGC mode while Capturing; ResumeCapture resets the streak
    //      so the main-loop black-screen pause never feeds it.
    //   2) Exponential backoff 30 -> 60 -> 120 frames + rebuild circuit breaker
    //      (max 5 rebuilds; re-armed after 600 consecutive good frames).
    //   3) Rebuild keeps WGC mode (StopCapture + InitializeWGCForced), no DXGI
    //      re-route, no inference-thread interaction; exceptions swallowed.
    void NoteBadFrameAndMaybeRebuildWGC();

    // --- Window validation ---
    bool ValidateWindow(HWND hWnd) const;

    // [Hybrid GPU optimization] Find the most suitable GPU adapter for a given hwnd (for diagnostics)
    struct AdapterInfo {
        bool found = false;
        bool usedSoftware = false;
        bool usedFallback = false;
        std::wstring description;
        SIZE_T dedicatedVideoMemory = 0;
        UINT flags = 0;
    };
    AdapterInfo ProbeAdapterForWindow(HWND hWnd) const;

    // [Display output detection] Detect whether the monitor hosting the window is driven by dGPU or iGPU
    // Return value: 1=dGPU output, 0=iGPU output, -1=detection failed (caller should fall back to the original logic)
    // Reuses ProbeAdapterForWindow logic; traverses DXGI to find the adapter that owns the window's HMONITOR
    int DetectDisplayGPUType(HWND hWnd) const;

    // --- Member variables ---
    HWND m_currentWindow;
    CaptureMethod m_currentMethod;
    CaptureMethod m_highestWorkingMethod;  // Best method that ever succeeded
    bool m_captureClientAreaOnly;
    bool m_initialized;

    // [Hybrid GPU] dGPU-preferred mode flag (set to true by GpuEnvironmentProbe when isHybridWithMux/isAmdApuHybrid
    // is detected; default false)
    bool m_isHybridGPU{false};

    // State machine
    std::atomic<CaptureState> m_state;
    int m_consecutiveErrors;
    static constexpr int MAX_CONSECUTIVE_ERRORS = 5;  // Downgrade after N failures

    // Timing controls
#ifdef _DEBUG
    static constexpr int RETRY_DELAY_MS = 100;       // Shorter delay in debug mode for faster recovery
#else
    static constexpr int RETRY_DELAY_MS = 300;       // Delay between retries on failure
#endif
    static constexpr int CIRCUIT_BREAKER_MS = 800;    // Timeout for single capture attempt
    static constexpr int STOPPED_RECOVERY_MS = 5000;  // Auto-recover from Stopped state after 5s
    static constexpr int PAUSED_TIMEOUT_MS = 30000;    // Auto-resume from Paused after 30s (m_isHybridGPU only)
    std::chrono::steady_clock::time_point m_lastErrorTime;
    std::chrono::steady_clock::time_point m_pausedSince;
    std::chrono::steady_clock::time_point m_stoppedSince;  // When Stopped state began

    // Frame validation thresholds
    static constexpr double BLACK_THRESHOLD = 1.0;    // Mean brightness < this = black screen
    static constexpr double WHITE_THRESHOLD = 250.0;  // Mean brightness > this = white screen

    // [Focus recovery optimization] Record focus-loss time for special handling during recovery
    std::chrono::steady_clock::time_point m_focusLostTime{};
    bool m_focusLost = false;

    // [Hybrid GPU optimization - long switch-out improvement] Distinguish state-machine-level focus loss
    // from PauseCapture-level pausing. Previously m_focusLostTime was the "first focus loss" time, not
    // the "current focus-loss duration", which was unreliable. Here m_pauseStartTime is set in
    // PauseCapture() and cleared in ResumeCapture(), making it the reliable basis for the dynamic
    // warmup frame count.
    std::chrono::steady_clock::time_point m_pauseStartTime{};
    bool m_pauseActive = false;  // Indicates whether currently in the PauseCapture state

    // [Hybrid GPU optimization - long switch-out improvement] Record the window size of the last successful capture
    // While switched out, the player may have changed the game resolution/fullscreen state, while the WGC FramePool
    // is still created with the old size -> misaligned or black frames after Resume.
    // Stores the last window client size here and compares it with the current one in SoftReinitialize.
    int m_lastValidWindowW = 0;
    int m_lastValidWindowH = 0;
    bool m_hasLastValidSize = false;

    // [WGC yellow-frame avoidance] Flag whether the current system is WGC-friendly
    // Some system/driver combinations produce yellow frames with WGC; avoid downgrading to WGC
    bool m_wgcFriendly = true;
    static constexpr int WGC_YELLOW_FRAME_THRESHOLD = 10;  // Mark as unfriendly after N consecutive yellow-frame detections

    // [Hybrid GPU optimization] WGC yellow-frame hysteresis: only counts when K consecutive frames all detect yellow
    // Solves single-frame misdetections caused by DWM cross-GPU copy color shifts at the instant of focus switching
    int m_wgcYellowStreak = 0;

    // [Hybrid GPU optimization] Frame-to-frame difference detection (guards against stale buffers)
    mutable cv::Mat m_lastFrameForDiff;
    mutable int m_staticFrameCount = 0;
    static constexpr int MAX_STATIC_FRAMES = 30;  // 30 consecutive unchanged frames = stale
    static constexpr double MIN_FRAME_DIFF = 0.5; // Average frame-to-frame difference threshold

    // [Hybrid GPU optimization - long switch-out improvement] SoftReinitialize warmup parameters
    // Dynamically adjust the warmup frame count:
    //  <10s switch-out: 30 frames (~1s), enough for present path rebuild
    //  10s-2min: 60 frames (~2s), gives the GPU time to restore state
    //  >2min: 120 frames (~4s), the driver may have paged out resources and needs rescheduling
    static constexpr int SOFT_REINIT_WARMUP_FRAMES_SHORT = 30;     // Short switch-out
    static constexpr int SOFT_REINIT_WARMUP_FRAMES_MEDIUM = 60;    // Medium switch-out
    static constexpr int SOFT_REINIT_WARMUP_FRAMES_LONG = 120;     // Long switch-out
    static constexpr int SOFT_REINIT_WARMUP_FRAME_DELAY_MS = 33;  // ~30fps

    // Switch-out duration thresholds (seconds)
    static constexpr int SOFT_REINIT_PAUSE_THRESHOLD_MEDIUM_S = 10;
    static constexpr int SOFT_REINIT_PAUSE_THRESHOLD_LONG_S = 120;

    // Minimum valid-frame ratio (numerator): how many frames must pass IsFrameAlive in SoftReinitialize to count as success
    static constexpr int SOFT_REINIT_MIN_VALID_RATIO_NUM = 24;     // At least 24 of 30 frames valid
    static constexpr int SOFT_REINIT_MIN_VALID_RATIO_DEN = 30;

    // [Hybrid GPU optimization - long switch-out improvement] Multi-stage SoftReinitialize parameters
    // After a round fails, enter the next round with an increased warmup frame count.
    // Up to 3 rounds; only go to ForceReinitialize when all rounds fail.
    static constexpr int SOFT_REINIT_MAX_STAGES = 3;

    // [DXGI upgrade blacklist] Avoid 5-second stutter in fullscreen mode
    // After DXGI fails due to fullscreen exclusive, record a cooldown during which upgrades are forbidden
    std::chrono::steady_clock::time_point m_dxgiBlacklistedUntil{};
    static constexpr std::chrono::milliseconds DXGI_UPGRADE_COOLDOWN{ 5 * 60 * 1000 };  // 5-minute cooldown
    bool m_dxgiUpgradeBlacklisted = false;

    // [SelfHeal-v2] Consecutive black/empty frame tracking + WGC rebuild backoff state.
    // All touched only from CaptureFrame / ResumeCapture / SwitchToMethod on the
    // capture (main-loop) thread; no cross-thread access, no atomics needed.
    int m_blackStreakCount = 0;        // consecutive bad (black/empty) frames while Capturing
    int m_blackStreakRebuildCount = 0; // rebuilds consumed since last re-arm
    int m_goodStreakCount = 0;         // consecutive valid frames (re-arms the breaker)
    static constexpr int BLACK_STREAK_REBUILD_BASE = 30;   // 1st rebuild after 30 consecutive bad frames (~1s)
    static constexpr int BLACK_STREAK_REBUILD_MAX = 120;   // backoff cap: 30 -> 60 -> 120
    static constexpr int BLACK_STREAK_REBUILD_LIMIT = 5;   // circuit breaker: stop rebuilding after N attempts
    static constexpr int BLACK_STREAK_GOOD_RESET = 600;    // sustained recovery (~20s+) re-arms the breaker

};
