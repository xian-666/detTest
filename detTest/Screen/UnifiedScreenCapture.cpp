#include "UnifiedScreenCapture.h"
#include "../common/DebugFileLog.h"
#include "../common/TargetWindow.h"   // [Run identity] DescribeWindow + ActiveSpec for [RUN TARGET] log
#include <VersionHelpers.h>
#include <thread>
#include <chrono>
#include <algorithm>  // [Hybrid GPU optimization] std::abs for SoftReinitialize window size diff
#include <cstdlib>    // [Hybrid GPU optimization] std::abs
#include <opencv2/opencv.hpp>

#pragma comment(lib, "Version.lib")

// [Diag-v2] Method name for diagnostic logs (file-local, no header pollution)
static const char* MethodToStr(UnifiedScreenCapture::CaptureMethod method) {
    switch (method) {
    case UnifiedScreenCapture::CaptureMethod::DXGI: return "DXGI";
    case UnifiedScreenCapture::CaptureMethod::WGC:  return "WGC";
    case UnifiedScreenCapture::CaptureMethod::GDI:  return "GDI";
    }
    return "Unknown";
}

// ============================================================
// [Compat routing] EnhancedCaptureRuntime - runtime subsystem for the enhanced path
// ============================================================
// Design points (strictly follow core constraints):
//   - This is a file-local class inside an anonymous namespace; it is not a member
//     of UnifiedScreenCapture nor a global variable. All enhanced runtime state is
//     encapsulated here and does not pollute existing class member variables.
//   - Takes effect only after InitializeEnhanced() activates it; when IsActive()==false,
//     the original CaptureFrame path is completely unaffected (single atomic load, ~1ns,
//     meeting <0.3ms / binary-level equivalence).
//   - All five enhancement measures are concentrated here + InitializeEnhanced(),
//     not scattered into InitializeLegacy.
//   - Directly reuses the existing DXGI/WGC/GDI backend singletons to acquire raw
//     frames, then applies enhanced post-processing on top, avoiding duplicated
//     capture logic while bypassing InitializeLegacy's black/white frame validation
//     (which would falsely kill frames in HDR scenarios).
// ============================================================
namespace {

class EnhancedCaptureRuntime {
public:
    static EnhancedCaptureRuntime& GetInstance() {
        static EnhancedCaptureRuntime inst;
        return inst;
    }
    static bool IsActive() noexcept { return s_active.load(std::memory_order_acquire); }

    // Called by InitializeEnhanced(): injects environment info + UI mask + activation.
    // uiMaskRects is provided by a hardcoded local constant inside InitializeEnhanced(),
    // not exposed as external configuration.
    void Activate(HWND hwnd, bool captureClientAreaOnly,
                  const compat::EnvProbeResult& env,
                  const std::vector<RECT>& uiMaskRects) noexcept {
        m_hwnd = hwnd;
        m_captureClientAreaOnly = captureClientAreaOnly;
        m_env = env;
        m_crossAdapter = env.isIntelArcIGPU || env.isHybridWithMux || env.isAmdApuHybrid;

        // UI static-region mask scaling cache: scaled proportionally once frame size is
        // known, avoiding per-frame recomputation.
        m_uiMaskRectsRaw = uiMaskRects;
        m_maskScaleValid = false;

        // Display mode baseline snapshot (for change detection)
        m_lastDisplayMode = SnapshotDisplayMode();
        m_modeCheckCounter = 0;

        // Frame liveness state reset
        m_lastHash = 0;
        m_hashStreak = 0;
        m_hashInitialized = false;

        // GDI throttling state
        m_lastGdiTime = std::chrono::steady_clock::time_point{};
        m_gdiIntervalMs = 33;            // Default ~30fps target
        s_lowConfidence.store(false);

        // Rebuild throttling
        m_lastRebuildTime = std::chrono::steady_clock::time_point{};

        s_active.store(true, std::memory_order_release);

        DEBUG_LOG_FMT("[EnhancedRT] Activated: crossAdapter=%d hdr=%d intelArc=%d mux=%d amdHybrid=%d",
            (int)m_crossAdapter, (int)env.hdrEnabled, (int)env.isIntelArcIGPU,
            (int)env.isHybridWithMux, (int)env.isAmdApuHybrid);
    }

    void Deactivate() noexcept {
        s_active.store(false, std::memory_order_release);
        s_lowConfidence.store(false);
        DEBUG_LOG_FMT("[EnhancedRT] Deactivated");
    }

    // Downstream recognition module (GameRecognitionPipeline) can query current frame confidence:
    //   true = GDI low frame rate; time-sensitive recognition such as skill cooldowns
    //   should enable interpolation compensation.
    static bool IsLowConfidenceFrame() noexcept {
        return s_active.load(std::memory_order_acquire) &&
               s_lowConfidence.load(std::memory_order_acquire);
    }

    // ========================================================
    // Enhanced capture main entry: acquire raw frame + five enhanced post-processing steps
    // ========================================================
    bool CaptureFrameEnhanced(cv::Mat& outFrame) noexcept {
        // ---------- 0. Read current backend method (read live per frame, follows rebuilds automatically) ----------
        CaptureMethod method = UnifiedScreenCapture::GetInstance().GetCurrentMethod();
        HWND hwnd = m_hwnd;
        if (!hwnd || !IsWindow(hwnd)) return false;

        // ---------- 5. GDI smart throttling and compensation (GDI path only) ----------
        //    Game-scenario risk: GDI fallback has insufficient frame rate at high refresh
        //    rates, causing skill cooldown recognition latency to exceed limits.
        //    Measure: dynamic capture interval + low-frame-rate confidence flag for
        //    downstream interpolation compensation.
        if (method == CaptureMethod::GDI) {
            ThrottleGdi();
        } else {
            s_lowConfidence.store(false, std::memory_order_release);
        }

        // ---------- Acquire raw frame (bypassing InitializeLegacy's black/white frame validation) ----------
        if (!AcquireRawFrame(method, hwnd, outFrame)) {
            return false;
        }
        if (outFrame.empty()) return false;

        // ---------- 1. HDR / pixel format safety conversion ----------
        //    Game-scenario risk: HDR / special pixel formats yield black frames or color
        //    anomalies, causing recognition misjudgment.
        //    Measure: detect the cv::Mat pixel format at runtime and convert when not
        //          BGR8 (GPU texture layer is handled by backend shaders; this is a
        //          safety net at the cv::Mat boundary).
        EnsureBgr8(outFrame);

        // ---------- 2. DDA frame liveness detection ----------
        //    Game-scenario risk: DDA/WGC session hangs silently - no error code but frames
        //    stall (after Optimus hot-switch).
        //    Measure: content hash identical for >=3 consecutive frames -> judged as hung,
        //          proactively rebuild the session. The hash masks out Honor of Kings:
        //          World static UI regions to avoid false triggers from a static HUD.
        const uint64_t curHash = ComputeMaskedHash(outFrame);
        if (m_hashInitialized && curHash == m_lastHash) {
            m_hashStreak++;
            if (m_hashStreak >= 3) {
                DEBUG_LOG_FMT("[EnhancedRT] Frame stall detected (hash streak=%d), rebuilding session",
                    m_hashStreak);
                RebuildSession(method, hwnd);
                m_hashStreak = 0;
                m_hashInitialized = false;
                return false; // Discard this frame; re-evaluate on the next frame
            }
        } else {
            m_hashStreak = 0;
        }
        m_lastHash = curHash;
        m_hashInitialized = true;

        // ---------- 3. Display mode change proactive monitoring ----------
        //    Game-scenario risk: resolution/refresh-rate/HDR changes invalidate the DDA
        //    session; DXGI async notification is unreliable.
        //    Measure: lightweight polling (every 60 frames) comparing EnumDisplaySettings; rebuild on change.
        if (++m_modeCheckCounter >= 60) {
            m_modeCheckCounter = 0;
            DisplayModeSnapshot cur = SnapshotDisplayMode();
            if (cur.valid && m_lastDisplayMode.valid && cur != m_lastDisplayMode) {
                DEBUG_LOG_FMT("[EnhancedRT] Display mode changed: %dx%d@%d hdr=%d -> %dx%d@%d hdr=%d, rebuilding",
                    m_lastDisplayMode.w, m_lastDisplayMode.h, m_lastDisplayMode.refresh,
                    (int)m_lastDisplayMode.hdr,
                    cur.w, cur.h, cur.refresh, (int)cur.hdr);
                m_lastDisplayMode = cur;
                RebuildSession(method, hwnd);
                return false;
            }
            m_lastDisplayMode = cur;
        }

        return true;
    }

private:
    EnhancedCaptureRuntime() = default;
    EnhancedCaptureRuntime(const EnhancedCaptureRuntime&) = delete;
    EnhancedCaptureRuntime& operator=(const EnhancedCaptureRuntime&) = delete;

    using CaptureMethod = UnifiedScreenCapture::CaptureMethod;

    // ---------- Raw frame acquisition (reuses backend singletons, no repeated InitializeLegacy validation) ----------
    bool AcquireRawFrame(CaptureMethod method, HWND hwnd, cv::Mat& out) noexcept {
        switch (method) {
        case CaptureMethod::DXGI: {
            HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (!hMon) return false;
            int idx = DXGIScreenCapture::GetInstance().GetMonitorIndex(hMon);
            if (idx == -1) {
                DXGIScreenCapture::GetInstance().StopCapture();
                if (DXGIScreenCapture::GetInstance().InitializeForWindow(hwnd)) {
                    idx = DXGIScreenCapture::GetInstance().GetMonitorIndex(hMon);
                }
            }
            if (idx == -1) return false;
            return DXGIScreenCapture::GetInstance().GetFrame(out, idx);
        }
        case CaptureMethod::WGC:
            return WGCScreenCapture::GetInstance().GetFrame(out, 0);
        case CaptureMethod::GDI:
            Screenshot::getInstance().getScreenShot(out, hwnd);
            return !out.empty();
        }
        return false;
    }

    // ---------- Enhancement 1: HDR / format safety conversion ----------
    void EnsureBgr8(cv::Mat& frame) noexcept {
        if (frame.empty()) return;
        const int type = frame.type();
        const int ch = frame.channels();
        // The recognition pipeline (common.hpp / ONNXInference) expects CV_8UC3 (BGR8).
        if (type == CV_8UC3) return;                       // Already target format
        if (ch == 4) {
            cv::Mat converted;
            cv::cvtColor(frame, converted, cv::COLOR_BGRA2BGR);
            frame = converted;
            DEBUG_LOG_FMT("[EnhancedRT] Format convert: BGRA8 -> BGR8 (HDR/format safety)");
        } else if (ch == 1) {
            cv::Mat converted;
            cv::cvtColor(frame, converted, cv::COLOR_GRAY2BGR);
            frame = converted;
            DEBUG_LOG_FMT("[EnhancedRT] Format convert: GRAY8 -> BGR8");
        } else if (ch == 3 && type != CV_8UC3) {
            cv::Mat converted;
            frame.convertTo(converted, CV_8UC3);
            frame = converted;
            DEBUG_LOG_FMT("[EnhancedRT] Format convert: 16-bit BGR -> BGR8");
        } else {
            DEBUG_LOG_FMT("[EnhancedRT] Unusual frame format type=%d ch=%d, passthrough", type, ch);
        }
    }

    // ---------- Enhancement 2: content hash (static UI region mask exclusion) ----------
    // Downsample to 32x18 first (INTER_NEAREST, ~0.1ms @1080p) then hash, meeting the <0.3ms budget.
    uint64_t ComputeMaskedHash(const cv::Mat& frame) noexcept {
        if (frame.empty() || frame.cols < 32 || frame.rows < 18) return 0;
        cv::Mat small;
        cv::resize(frame, small, cv::Size(32, 18), 0, 0, cv::INTER_NEAREST);

        // Static UI region mask (scaled to 32x18 and blacked out, excluding static HUD areas)
        if (!m_maskScaleValid) {
            m_uiMaskRectsScaled.clear();
            const double sx = 32.0 / frame.cols;
            const double sy = 18.0 / frame.rows;
            for (const RECT& r : m_uiMaskRectsRaw) {
                cv::Rect s;
                s.x = static_cast<int>(r.left * sx);
                s.y = static_cast<int>(r.top * sy);
                s.width = static_cast<int>((r.right - r.left) * sx);
                s.height = static_cast<int>((r.bottom - r.top) * sy);
                s &= cv::Rect(0, 0, 32, 18);
                if (s.width > 0 && s.height > 0) m_uiMaskRectsScaled.push_back(s);
            }
            m_maskScaleValid = true;
        }
        for (const cv::Rect& r : m_uiMaskRectsScaled) {
            cv::rectangle(small, r, cv::Scalar::all(0), -1);
        }

        // 64-bit perceptual hash: pixel brightness > mean -> set bit
        cv::Scalar mean = cv::mean(small);
        uint64_t hash = 0;
        int bit = 0;
        for (int y = 0; y < small.rows && bit < 64; ++y) {
            const uchar* p = small.ptr<uchar>(y);
            for (int x = 0; x < small.cols * small.channels() && bit < 64; ++x) {
                if (p[x] > mean[0]) hash |= (1ULL << bit);
                ++bit;
            }
        }
        return hash;
    }

    // ---------- Enhancement 3: display mode snapshot ----------
    struct DisplayModeSnapshot {
        int w = 0, h = 0, refresh = 0;
        bool hdr = false;
        bool valid = false;
        bool operator!=(const DisplayModeSnapshot& o) const {
            return w != o.w || h != o.h || refresh != o.refresh || hdr != o.hdr;
        }
    };
    DisplayModeSnapshot SnapshotDisplayMode() noexcept {
        DisplayModeSnapshot s;
        DISPLAY_DEVICEW dd; dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, 0, &dd, 0)) return s;
        DEVMODEW dm; dm.dmSize = sizeof(dm); dm.dmDriverExtra = 0;
        if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) return s;
        s.w = (int)dm.dmPelsWidth;
        s.h = (int)dm.dmPelsHeight;
        s.refresh = (int)dm.dmDisplayFrequency;
        // HDR: HKCU\...\VideoSettings\HDR
        DWORD hdrVal = 0;
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\VideoSettings",
                0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD sz = sizeof(hdrVal);
            RegQueryValueExW(hKey, L"HDR", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(&hdrVal), &sz);
            RegCloseKey(hKey);
        }
        s.hdr = (hdrVal != 0);
        s.valid = true;
        return s;
    }

    // ---------- Enhancement 2/3: session rebuild (with 2s throttle to prevent flapping) ----------
    void RebuildSession(CaptureMethod method, HWND hwnd) noexcept {
        auto now = std::chrono::steady_clock::now();
        if (m_lastRebuildTime.time_since_epoch().count() != 0) {
            auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastRebuildTime).count();
            if (since < 2000) {
                DEBUG_LOG_FMT("[EnhancedRT] Rebuild throttled (%lldms < 2000ms), skip", (long long)since);
                return;
            }
        }
        m_lastRebuildTime = now;

        // [Enhancement 4] Cross-adapter texture safe copy: force Staging Texture path on rebuild.
        //   Game-scenario risk: direct cross-device texture access crashes when iGPU/dGPU are separate.
        //   Measure: when separation is detected, backend GetFrame already transfers via
        //         Staging Texture + shared handle (DXGIScreenCapture::m_stagingTexture /
        //         WGC CopyFrameToBuffer); here we only rebuild the session and log,
        //         never accessing across devices directly.
        if (method == CaptureMethod::DXGI) {
            DXGIScreenCapture::GetInstance().StopCapture();
            DXGIScreenCapture::GetInstance().InitializeForWindow(hwnd);
            DEBUG_LOG_FMT("[EnhancedRT] DXGI session rebuilt (staging-texture cross-adapter safe path, crossAdapter=%d)",
                (int)m_crossAdapter);
        } else if (method == CaptureMethod::WGC) {
            WGCScreenCapture::GetInstance().StopCapture();
            WGCScreenCapture::GetInstance().InitializeForWindow(hwnd);
            DEBUG_LOG_FMT("[EnhancedRT] WGC session rebuilt (crossAdapter=%d)", (int)m_crossAdapter);
        }
        // GDI has no session concept; no rebuild needed.
    }

    // ---------- Enhancement 5: GDI smart throttling ----------
    void ThrottleGdi() noexcept {
        auto now = std::chrono::steady_clock::now();
        if (m_lastGdiTime.time_since_epoch().count() != 0) {
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastGdiTime).count();
            // Dynamic adjustment: if the actual frame interval is too short, throttle to
            // the target interval; if too long, lower confidence.
            if (elapsedMs < m_gdiIntervalMs) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(m_gdiIntervalMs - (long long)elapsedMs));
            }
            // Measured frame rate significantly below target (interval > 1.5x) -> flag low confidence
            s_lowConfidence.store(elapsedMs > (m_gdiIntervalMs * 3 / 2),
                                  std::memory_order_release);
        }
        m_lastGdiTime = std::chrono::steady_clock::now();
    }

    // ---- State ----
    HWND m_hwnd = nullptr;
    bool m_captureClientAreaOnly = true;
    compat::EnvProbeResult m_env;
    bool m_crossAdapter = false;

    std::vector<RECT> m_uiMaskRectsRaw;
    std::vector<cv::Rect> m_uiMaskRectsScaled;
    bool m_maskScaleValid = false;

    uint64_t m_lastHash = 0;
    int m_hashStreak = 0;
    bool m_hashInitialized = false;

    DisplayModeSnapshot m_lastDisplayMode;
    int m_modeCheckCounter = 0;

    std::chrono::steady_clock::time_point m_lastGdiTime;
    int m_gdiIntervalMs = 33;

    std::chrono::steady_clock::time_point m_lastRebuildTime;

    static std::atomic<bool> s_active;
    static std::atomic<bool> s_lowConfidence;
};

std::atomic<bool> EnhancedCaptureRuntime::s_active{ false };
std::atomic<bool> EnhancedCaptureRuntime::s_lowConfidence{ false };

} // namespace

// ============================================================
// State Machine Transitions
// ============================================================

void UnifiedScreenCapture::TransitionTo(CaptureState newState) {
    CaptureState old = m_state.load();
    if (old == newState) return;

    DEBUG_LOG_FMT("[UnifiedCapture] State transition: %d -> %d", (int)old, (int)newState);
    m_state.store(newState);

    if (newState == CaptureState::ExceptionWait) {
        m_lastErrorTime = std::chrono::steady_clock::now();
    }
    if (newState == CaptureState::Stopped) {
        m_stoppedSince = std::chrono::steady_clock::now();
    }
    
    // [Focus recovery optimization] Record focus loss when state transitions away from Capturing
    if (newState != CaptureState::Capturing && old == CaptureState::Capturing) {
        m_focusLostTime = std::chrono::steady_clock::now();
        m_focusLost = true;
    }
}

bool UnifiedScreenCapture::ShouldRetry() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastErrorTime).count();
    return elapsed >= RETRY_DELAY_MS;
}

bool UnifiedScreenCapture::ShouldDowngrade() const {
    return m_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS;
}

// ============================================================
// Initialize - compat routing entry (lossless routing)
// ============================================================
// The routing decision is added only at this entry; high-risk scenarios -> InitializeEnhanced(),
// everything else -> InitializeLegacy() (original logic moved line by line, binary-level identical).
// Configuration switch compat_mode: auto (default) / force_enhanced / force_legacy.
bool UnifiedScreenCapture::Initialize(HWND hWnd, bool captureClientAreaOnly) {
    if (!ValidateWindow(hWnd)) {
        DEBUG_LOG_FMT("[UnifiedCapture] Invalid window handle");
        return false;
    }

    // Ensure the enhanced runtime is off by default (prevents a leftover enhanced session
    // from hijacking the legacy path). Called only if previously activated; zero cost otherwise.
    if (EnhancedCaptureRuntime::IsActive()) {
        EnhancedCaptureRuntime::GetInstance().Deactivate();
    }

    // 1. Read compat_mode (environment variable BT_CAPTURE_COMPAT_MODE, default Auto; no config file)
    compat::CompatMode mode = compat::GpuEnvironmentProbe::GetCompatMode();

    // 2. force_legacy: skip probing, go straight to the legacy path (equivalent to pre-change behavior)
    if (mode == compat::CompatMode::ForceLegacy) {
        DEBUG_LOG_FMT("[UnifiedCapture] compat_mode=force_legacy -> InitializeLegacy");
        return InitializeLegacy(hWnd, captureClientAreaOnly);
    }

    // 3. force_enhanced: skip probing, go straight to the enhanced path
    if (mode == compat::CompatMode::ForceEnhanced) {
        DEBUG_LOG_FMT("[UnifiedCapture] compat_mode=force_enhanced -> InitializeEnhanced");
        return InitializeEnhanced(hWnd, captureClientAreaOnly);
    }

    // 4. auto: read-only probe decision (WMI/SetupAPI/registry/EnumDisplayDevices; no DXGI/D3D/WGC/GDI)
    compat::EnvProbeResult env = compat::GpuEnvironmentProbe::Detect();
    {
        // Logging convention: path selection reason + GPU model + driver version + display mode
        char summaryA[512] = { 0 };
        WideCharToMultiByte(CP_ACP, 0, env.summary.c_str(), -1, summaryA, sizeof(summaryA), NULL, NULL);
        DEBUG_LOG_FMT("[UnifiedCapture] Probe: %s", summaryA);
        DEBUG_LOG_FMT("[UnifiedCapture] NeedsEnhancedCompat=%d",
            (int)compat::GpuEnvironmentProbe::NeedsEnhancedCompat(env));
    }

    // [Hybrid GPU] Derive flag from probe result
    m_isHybridGPU = (env.isHybridWithMux || env.isAmdApuHybrid);
    DEBUG_LOG_FMT("[UnifiedCapture] m_isHybridGPU=%d (mux=%d amdHybrid=%d)",
        (int)m_isHybridGPU, (int)env.isHybridWithMux, (int)env.isAmdApuHybrid);

    // [Display output detection] Route by which GPU drives the monitor containing the window:
    //   iGPU output (e.g. Optimus mode) -> InitializeLegacy (EnhancedRT not activated, reference-solution behavior)
    //   dGPU output (e.g. MUX direct mode) -> InitializeEnhanced (activate EnhancedRT + dGPU priority)
    //   Detection failure -> fall back to the original NeedsEnhancedCompat routing logic (byte-for-byte unchanged)
    //
    // Note: an earlier revision added a "risk-aware upgrade" here that promoted iGPU-output
    // runs to Enhanced whenever NeedsEnhancedCompat returned 1 (e.g. MUX / HDR / VRR+HiDPI).
    // Field testing showed this is a REGRESSION for titles like NGR-Win64-Shipping running
    // borderless on the iGPU: their DXGI desktop duplication (Legacy) is stable, while the
    // Enhanced path's WGC fallback hits a DXGI_ERROR_DEVICE_REMOVED flap loop on Intel UHD
    // drivers with mux=1/vrr=1. Reverted to output-only routing to match the reference project.
    {
        int gpuType = DetectDisplayGPUType(hWnd);
        if (gpuType == 0) {
            // Monitor driven by iGPU -> Legacy (reference-solution behavior)
            DEBUG_LOG_FMT("[UnifiedCapture] display on iGPU -> InitializeLegacy");
            return InitializeLegacy(hWnd, captureClientAreaOnly);
        } else if (gpuType == 1) {
            // Monitor driven by dGPU -> Enhanced (dGPU direct, EnhancedRT enhancements effective)
            DEBUG_LOG_FMT("[UnifiedCapture] display on dGPU -> InitializeEnhanced");
            return InitializeEnhanced(hWnd, captureClientAreaOnly);
        }
        // gpuType == -1: detection failed, fall back to original logic
        DEBUG_LOG_FMT("[UnifiedCapture] DetectDisplayGPUType failed, fallback to NeedsEnhancedCompat");
    }

    if (compat::GpuEnvironmentProbe::NeedsEnhancedCompat(env)) {
        DEBUG_LOG_FMT("[UnifiedCapture] auto -> InitializeEnhanced (high-risk scenario detected)");
        return InitializeEnhanced(hWnd, captureClientAreaOnly);
    }

    DEBUG_LOG_FMT("[UnifiedCapture] auto -> InitializeLegacy (no high-risk scenario)");
    return InitializeLegacy(hWnd, captureClientAreaOnly);
}

// ============================================================
// InitializeLegacy - original Initialize() logic moved line by line
// ============================================================
// Calling convention / parameters / return value semantics are fully identical
// to before; when enhancement conditions are not hit, Initialize() delegates
// directly to it, with an execution path binary-level identical to before.
bool UnifiedScreenCapture::InitializeLegacy(HWND hWnd, bool captureClientAreaOnly) {
    if (!ValidateWindow(hWnd)) {
        DEBUG_LOG_FMT("[UnifiedCapture] Invalid window handle");
        return false;
    }

    wchar_t title[256] = {};
    GetWindowTextW(hWnd, title, 256);
    DEBUG_LOG_FMT("[UnifiedCapture] Initialize: hwnd=%p title=%ls clientOnly=%d",
        (void*)hWnd, title, (int)captureClientAreaOnly);

    // [Run identity] Cross-check the actual captured window against the configured [Target] spec
    // so a log opened days later still answers "which game did this run point at?".
    DEBUG_LOG_FMT("[RUN TARGET] hit: %ls", target::DescribeWindow(hWnd).c_str());
    DEBUG_LOG_FMT("[RUN TARGET] spec: %ls", target::ActiveSpec().Describe().c_str());

    // Log Windows build number for WGC support diagnosis.
    {
        HMODULE hMod = ::GetModuleHandleW(L"ntdll.dll");
        if (hMod) {
            typedef LONG(WINAPI* RtlGetVerPtr)(PRTL_OSVERSIONINFOW);
            auto fxPtr = (RtlGetVerPtr)::GetProcAddress(hMod, "RtlGetVersion");
            if (fxPtr) {
                RTL_OSVERSIONINFOW rovi = { 0 };
                rovi.dwOSVersionInfoSize = sizeof(rovi);
                if (fxPtr(&rovi) == 0) {
                    DEBUG_LOG_FMT("[UnifiedCapture] Windows version: %d.%d build %d",
                        rovi.dwMajorVersion, rovi.dwMinorVersion, rovi.dwBuildNumber);
                }
            }
        }
    }

    m_currentWindow = hWnd;
    m_captureClientAreaOnly = captureClientAreaOnly;
    m_consecutiveErrors = 0;
    m_highestWorkingMethod = CaptureMethod::GDI;  // Start with lowest

    // [Hybrid GPU diagnostics] Probe the GPU adapter for hwnd
    LogAdapterDiagnostics(hWnd);

    // Priority 1: DXGI (global capture, D3D12 compatible)
    DEBUG_LOG_FMT("[UnifiedCapture] Trying DXGI (priority 1)...");
    if (TryInitializeDXGI(hWnd)) {
        m_currentMethod = CaptureMethod::DXGI;
        m_highestWorkingMethod = CaptureMethod::DXGI;
        m_initialized = true;
        m_wgcFriendly = true;  // DXGI succeeded; WGC availability undetermined
        TransitionTo(CaptureState::Capturing);
        DEBUG_LOG_FMT("[UnifiedCapture] Initialized with DXGI mode");
        return true;
    }

    // Priority 2: WGC (window capture) - try only when the system is WGC-friendly
    bool bSupportWGC = IsRealWindows10BuildOrGreater(22000) && m_wgcFriendly;
    if (bSupportWGC) {
        DEBUG_LOG_FMT("[UnifiedCapture] DXGI failed, trying WGC (priority 2, friendly=%d)...", m_wgcFriendly);
        if (TryInitializeWGC(hWnd)) {
            m_currentMethod = CaptureMethod::WGC;
            m_highestWorkingMethod = CaptureMethod::WGC;
            m_initialized = true;
            TransitionTo(CaptureState::Capturing);
            DEBUG_LOG_FMT("[UnifiedCapture] Initialized with WGC mode");
            return true;
        }
    } else {
        DEBUG_LOG_FMT("[UnifiedCapture] WGC skipped (build<22000 or unfriendly=%d)", !m_wgcFriendly);
    }

    // Priority 3: GDI (fallback only)
    DEBUG_LOG_FMT("[UnifiedCapture] DXGI and WGC failed, using GDI fallback...");
    if (TryInitializeGDI()) {
        m_currentMethod = CaptureMethod::GDI;
        m_highestWorkingMethod = CaptureMethod::GDI;
        m_initialized = true;
        TransitionTo(CaptureState::Capturing);
        DEBUG_LOG_FMT("[UnifiedCapture] Initialized with GDI mode");
        return true;
    }

    DEBUG_LOG_FMT("[UnifiedCapture] All capture methods failed to initialize");
    m_initialized = false;
    TransitionTo(CaptureState::Idle);
    return false;
}

// ============================================================
// InitializeEnhanced - enhanced initialization path (five game-scenario-driven enhancements)
// ============================================================
// Backend selection priority matches InitializeLegacy (DXGI > WGC > GDI); differences:
//   - Injects environment probe results and activates EnhancedCaptureRuntime
//     (the runtime carrier of the five enhancement measures)
//   - Fully logs path selection reason / GPU model + driver version + display mode
// The runtime logic of the five enhancement measures lives in
// EnhancedCaptureRuntime::CaptureFrameEnhanced, dispatched by a single atomic check
// at the CaptureFrame entry; zero impact on non-activated devices.
bool UnifiedScreenCapture::InitializeEnhanced(HWND hWnd, bool captureClientAreaOnly) {
    if (!ValidateWindow(hWnd)) {
        DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] Invalid window handle");
        return false;
    }

    wchar_t title[256] = {};
    GetWindowTextW(hWnd, title, 256);
    DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] Initialize: hwnd=%p title=%ls clientOnly=%d",
        (void*)hWnd, title, (int)captureClientAreaOnly);

    // [Run identity] Same cross-check as the Legacy path (see InitializeLegacy).
    DEBUG_LOG_FMT("[RUN TARGET] hit: %ls", target::DescribeWindow(hWnd).c_str());
    DEBUG_LOG_FMT("[RUN TARGET] spec: %ls", target::ActiveSpec().Describe().c_str());

    // Log Windows build number for WGC support diagnosis.
    {
        HMODULE hMod = ::GetModuleHandleW(L"ntdll.dll");
        if (hMod) {
            typedef LONG(WINAPI* RtlGetVerPtr)(PRTL_OSVERSIONINFOW);
            auto fxPtr = (RtlGetVerPtr)::GetProcAddress(hMod, "RtlGetVersion");
            if (fxPtr) {
                RTL_OSVERSIONINFOW rovi = { 0 };
                rovi.dwOSVersionInfoSize = sizeof(rovi);
                if (fxPtr(&rovi) == 0) {
                    DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] Windows version: %d.%d build %d",
                        rovi.dwMajorVersion, rovi.dwMinorVersion, rovi.dwBuildNumber);
                }
            }
        }
    }

    m_currentWindow = hWnd;
    m_captureClientAreaOnly = captureClientAreaOnly;
    m_consecutiveErrors = 0;
    m_highestWorkingMethod = CaptureMethod::GDI;  // Start with lowest

    // [Hybrid GPU diagnostics] Probe the GPU adapter for hwnd
    LogAdapterDiagnostics(hWnd);

    // ---- Environment probe (purely read-only, used to activate the enhanced runtime + logging) ----
    compat::EnvProbeResult env = compat::GpuEnvironmentProbe::Detect();

    // [Hybrid GPU] The force_enhanced path bypasses Initialize(); set the dGPU priority flag here
    m_isHybridGPU = (env.isHybridWithMux || env.isAmdApuHybrid);

    // ---- Static UI region mask (hardcoded defaults, not exposed as external configuration) ----
    // Static Honor of Kings: World UI regions to exclude during frame liveness detection
    // (DDA hang detection). Coordinates are relative to the game client area's top-left
    // corner (pixels), defined on a 1920x1080 reference resolution and scaled at runtime
    // by ComputeMaskedHash according to the actual frame size.
    // Note: these values must match the actual game UI layout; maintainers adjust as needed.
    static const std::vector<RECT> kDefaultUiMaskRects = {
        { 0,    0,    1920, 60   }, // Top info bar (health bars/scoreboard) static region
        { 1700, 1000, 1920, 1080 }, // Bottom-right skill HUD (static skill icons excluded from liveness hash)
        { 0,    1020, 1920, 1080 }, // Bottom status bar
    };

    // Logging convention: path selection reason + GPU model + driver version + display mode
    DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] Path reason: intelArc=%d mux=%d amdHybrid=%d hdr=%d vrr=%d dpi=%d arcDriverSafe=%d",
        (int)env.isIntelArcIGPU, (int)env.isHybridWithMux, (int)env.isAmdApuHybrid,
        (int)env.hdrEnabled, (int)env.adaptiveRefreshEnabled, (int)env.highDpiScaling, (int)env.igpuDriverSafe);
    for (const auto& a : env.adapters) {
        if (a.isSoftware) continue;
        char descA[256] = { 0 };
        WideCharToMultiByte(CP_ACP, 0, a.description.c_str(), -1, descA, sizeof(descA), NULL, NULL);
        char drvA[64] = { 0 };
        WideCharToMultiByte(CP_ACP, 0, a.driverVersion.c_str(), -1, drvA, sizeof(drvA), NULL, NULL);
        DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] GPU ven=0x%04X dev=0x%04X drv=%s desc=%s",
            a.vendorId, a.deviceId, drvA, descA);
    }
    DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] Display %dx%d@%dHz bpp=%d hdr=%d dpi=%.2f",
        env.primaryDisplay.width, env.primaryDisplay.height, env.primaryDisplay.refreshRate,
        env.primaryDisplay.bitsPerPixel, (int)env.primaryDisplay.hdrEnabled, env.primaryDisplay.dpiScale);

    // ---- Backend initialization (DXGI > WGC > GDI, same priority as InitializeLegacy) ----
    bool backendOk = false;

    DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] Trying DXGI (priority 1)...");
    if (TryInitializeDXGI(hWnd)) {
        m_currentMethod = CaptureMethod::DXGI;
        m_highestWorkingMethod = CaptureMethod::DXGI;
        m_initialized = true;
        m_wgcFriendly = true;
        TransitionTo(CaptureState::Capturing);
        DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] Initialized with DXGI mode");
        backendOk = true;
    }

    if (!backendOk) {
        bool bSupportWGC = IsRealWindows10BuildOrGreater(22000) && m_wgcFriendly;
        if (bSupportWGC) {
            DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] DXGI failed, trying WGC (priority 2, friendly=%d)...", m_wgcFriendly);
            if (TryInitializeWGC(hWnd)) {
                m_currentMethod = CaptureMethod::WGC;
                m_highestWorkingMethod = CaptureMethod::WGC;
                m_initialized = true;
                TransitionTo(CaptureState::Capturing);
                DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] Initialized with WGC mode");
                backendOk = true;
            }
        } else {
            DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] WGC skipped (build<22000 or unfriendly=%d)", !m_wgcFriendly);
        }
    }

    if (!backendOk) {
        DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] DXGI and WGC failed, using GDI fallback...");
        if (TryInitializeGDI()) {
            m_currentMethod = CaptureMethod::GDI;
            m_highestWorkingMethod = CaptureMethod::GDI;
            m_initialized = true;
            TransitionTo(CaptureState::Capturing);
            DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] Initialized with GDI mode");
            backendOk = true;
        }
    }

    if (!backendOk) {
        DEBUG_LOG_FMT("[UnifiedCapture][Enhanced] All capture methods failed to initialize");
        m_initialized = false;
        TransitionTo(CaptureState::Idle);
        EnhancedCaptureRuntime::GetInstance().Deactivate();
        return false;
    }

    // ============================================================
    // Activation of the five game-scenario-driven enhancement measures
    // (runtime logic in EnhancedCaptureRuntime)
    // ============================================================
    // [Enhancement 1] HDR / pixel format safety conversion
    //   Risk: HDR / special pixel formats -> captured black frames or color anomalies,
    //         misjudged by the recognition pipeline.
    //   Measure: EnsureBgr8() inside CaptureFrameEnhanced detects the cv::Mat pixel format
    //         at runtime and converts when not BGR8 (GPU texture layer handled by backend
    //         shaders; this is the cv::Mat safety net).
    //         Log: "Format convert: ..." (format conversion trigger event).
    // [Enhancement 2] DDA frame liveness detection
    //   Risk: DDA/WGC session hangs silently (after Optimus hot-switch); no error code
    //         but frames stall.
    //   Measure: ComputeMaskedHash() hashes with Honor of Kings: World static UI regions
    //         masked out; >=3 identical consecutive frames without an error code ->
    //         RebuildSession() proactively rebuilds the session.
    //         Log: "Frame stall detected ... rebuilding session" / "session rebuilt".
    // [Enhancement 3] Display mode change proactive monitoring
    //   Risk: resolution/refresh-rate/HDR changes invalidate the DDA session; DXGI async
    //         notification is unreliable.
    //   Measure: every 60 frames SnapshotDisplayMode() lightly polls EnumDisplaySettings
    //         and compares; rebuild on change (no reliance on DXGI async notification).
    //         Log: "Display mode changed ... rebuilding".
    // [Enhancement 4] Cross-adapter texture safe copy
    //   Risk: direct cross-device texture access crashes when iGPU/dGPU are separate
    //         (AMD APU + discrete GPU, etc.).
    //   Measure: when separation is detected (crossAdapter), backend GetFrame transfers
    //         via Staging Texture + shared handle (DXGIScreenCapture::m_stagingTexture /
    //         WGC CopyFrameToBuffer); RebuildSession keeps that path on rebuild; direct
    //         cross-device access is forbidden.
    //         Log: "session rebuilt (staging-texture cross-adapter safe path ...)".
    // [Enhancement 5] GDI smart throttling and compensation
    //   Risk: GDI fallback has insufficient frame rate at high refresh rates; skill
    //         cooldown recognition latency exceeds limits.
    //   Measure: ThrottleGdi() dynamic capture interval; IsLowConfidenceFrame() exposes
    //         the confidence flag so the downstream recognition module
    //         (GameRecognitionPipeline) can enable interpolation compensation for
    //         time-sensitive regions such as skill cooldowns (target: latency jitter <=+/-50ms).
    // ============================================================
    EnhancedCaptureRuntime::GetInstance().Activate(hWnd, captureClientAreaOnly, env, kDefaultUiMaskRects);

    return true;
}

// ============================================================
// InitializeWGCForced - Startup grace period: skip DXGI to
// avoid ACCESS_LOST when the game window first appears.
// ============================================================

bool UnifiedScreenCapture::InitializeWGCForced(HWND hWnd, bool captureClientAreaOnly) {
    if (!ValidateWindow(hWnd)) {
        DEBUG_LOG_FMT("[UnifiedCapture] InitializeWGCForced: Invalid window handle");
        return false;
    }

    m_currentWindow = hWnd;
    m_captureClientAreaOnly = captureClientAreaOnly;
    m_consecutiveErrors = 0;

    // Force WGC mode - skip DXGI during startup grace period
    bool bSupportWGC = IsRealWindows10BuildOrGreater(22000);
    if (bSupportWGC) {
        DEBUG_LOG_FMT("[UnifiedCapture] Startup grace period: forcing WGC mode...");
        if (TryInitializeWGC(hWnd)) {
            m_currentMethod = CaptureMethod::WGC;
            m_highestWorkingMethod = CaptureMethod::WGC;
            m_initialized = true;
            TransitionTo(CaptureState::Capturing);
            DEBUG_LOG_FMT("[UnifiedCapture] Initialized with WGC mode (forced for startup)");
            return true;
        }
        // WGC initialization returned false (yellow border/session setup failure, etc.);
        // log the reason then fall back
        DEBUG_LOG_FMT("[UnifiedCapture] WGC forced init failed (TryInitializeWGC returned false), trying DXGI...");
    } else {
        DEBUG_LOG_FMT("[UnifiedCapture] WGC forced init skipped (build<22000), trying DXGI...");
    }

    // WGC not available or failed, try DXGI as fallback
    // [Hybrid GPU] TryInitializeDXGI already calls SetHybridGPUMode internally, so the
    // fallback also prefers dGPU
    if (TryInitializeDXGI(hWnd)) {
        m_currentMethod = CaptureMethod::DXGI;
        m_highestWorkingMethod = CaptureMethod::DXGI;
        m_initialized = true;
        TransitionTo(CaptureState::Capturing);
        DEBUG_LOG_FMT("[UnifiedCapture] Initialized with DXGI mode (WGC force fallback)");
        return true;
    }

    DEBUG_LOG_FMT("[UnifiedCapture] All capture methods failed in InitializeWGCForced");
    m_initialized = false;
    TransitionTo(CaptureState::Idle);
    return false;
}

// ============================================================
// CaptureFrame - Core state machine
// ============================================================

bool UnifiedScreenCapture::CaptureFrame(cv::Mat& outFrame) {
    // If not initialized but we have a valid window, try to reinitialize
    // This handles the case where ForceReinitialize's Initialize() failed
    // (e.g., DXGI OutputDuplication not yet ready after focus switch)
    if (!m_initialized) {
        if (m_currentWindow && ValidateWindow(m_currentWindow)) {
            DEBUG_LOG_FMT("[UnifiedCapture] Not initialized, attempting auto-reinit");
            if (!Initialize(m_currentWindow, m_captureClientAreaOnly)) {
                return false;
            }
        } else {
            return false;
        }
    }

    // Stopped state - auto-recover after cooldown period
    if (m_state.load() == CaptureState::Stopped) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_stoppedSince).count();
        if (elapsed >= STOPPED_RECOVERY_MS) {
            DEBUG_LOG_FMT("[UnifiedCapture] Stopped state recovery: reinitializing after %dms", (int)elapsed);
            HWND savedWnd = m_currentWindow;
            bool savedClientOnly = m_captureClientAreaOnly;
            Release();
            if (savedWnd && ValidateWindow(savedWnd)) {
                m_currentWindow = savedWnd;
                Initialize(savedWnd, savedClientOnly);
            }
        }
        return false;
    }

    // Paused state - skip capture entirely
    // [Hybrid GPU defense] Paused is the only state without a timeout; add a timeout
    // backstop to prevent permanent hangs
    if (m_state.load() == CaptureState::Paused) {
        if (m_isHybridGPU) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_pausedSince).count();
            if (elapsed >= PAUSED_TIMEOUT_MS) {
                DEBUG_LOG_FMT("[UnifiedCapture] Paused timeout (%lldms >= %dms), auto-resuming",
                    (long long)elapsed, PAUSED_TIMEOUT_MS);
                ResumeCapture();
            }
        }
        return false;
    }

    // ExceptionWait state - check retry delay
    if (m_state.load() == CaptureState::ExceptionWait) {
        if (!ShouldRetry()) {
            return false;
        }
        TransitionTo(CaptureState::Capturing);
    }

    // Window validation
    if (!ValidateWindow(m_currentWindow)) {
        m_consecutiveErrors++;
        TransitionTo(CaptureState::ExceptionWait);
        return false;
    }

    if (IsIconic(m_currentWindow)) {
        return false;
    }

    // [Compat routing] Enhanced path dispatch - take the enhanced capture only when the
    // enhanced runtime is active. Single atomic load (~1ns); on non-activated devices
    // this check falls straight through, and the entire capture/validation/state machine
    // logic below is binary-level identical to before (zero added per-frame overhead).
    if (EnhancedCaptureRuntime::IsActive()) {
        bool ok = EnhancedCaptureRuntime::GetInstance().CaptureFrameEnhanced(outFrame);
        if (ok) {
            m_consecutiveErrors = 0;
            return true;
        }
        // [Hybrid GPU fix] Increment the error count when EnhancedRT fails so the state
        // machine can downgrade to WGC. Keep the original behavior (no increment) for
        // non-hybrid scenarios to avoid false downgrades.
        if (m_isHybridGPU) {
            m_consecutiveErrors++;
            DEBUG_LOG_FMT("[UnifiedCapture] EnhancedRT capture failed (hybrid), consecutive errors: %d/%d",
                m_consecutiveErrors, MAX_CONSECUTIVE_ERRORS);
            if (ShouldDowngrade()) {
                DowngradeMethod(m_currentWindow);
            } else {
                TransitionTo(CaptureState::ExceptionWait);
            }
        }
        return false;
    }

    // Record start time for circuit breaker
    auto startTime = std::chrono::steady_clock::now();

    // Dispatch to backend-specific capture based on current method
    bool success = false;
    switch (m_currentMethod) {
    case CaptureMethod::DXGI:
        success = CaptureFrameDXGI(outFrame);
        break;
    case CaptureMethod::WGC:
        success = CaptureFrameWGC(outFrame);
        break;
    case CaptureMethod::GDI:
        success = CaptureFrameGDI(outFrame);
        break;
    }

    // Circuit breaker check
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();
    if (elapsed > CIRCUIT_BREAKER_MS && !success) {
        DEBUG_LOG_FMT("[UnifiedCapture] Circuit breaker triggered (%dms > %dms), forcing retry",
            elapsed, CIRCUIT_BREAKER_MS);
        success = false;
    }

    // Frame validation on success
    if (success && !outFrame.empty()) {
        if (!ValidateFrame(outFrame)) {
            DEBUG_LOG_FMT("[UnifiedCapture] Frame validation failed (black/white screen), discarding");
            success = false;
        }
    }

    if (success) {
        // Reset error counters on successful frame
        m_consecutiveErrors = 0;
        // [SelfHeal-v2] A valid frame also clears the bad-frame streak and, after a
        // sustained recovery (~600 consecutive good frames), re-arms the rebuild
        // circuit breaker so the system can self-heal again later (e.g. after the
        // memory pressure on the machine goes away).
        m_blackStreakCount = 0;
        m_goodStreakCount++;
        if (m_goodStreakCount >= BLACK_STREAK_GOOD_RESET && m_blackStreakRebuildCount > 0) {
            m_blackStreakRebuildCount = 0;
            DEBUG_LOG_FMT("[UnifiedCapture][SelfHeal] %d consecutive good frames - WGC rebuild budget re-armed",
                m_goodStreakCount);
        }
        return true;
    }

    // Treat empty or invalid/black frames as transient and do not count them as source failures.
    if (outFrame.empty()) {
        // [Diag-v2] Throttled diagnostics (was: per-frame log). src=none for WGC means
        // "no frame ever received" (blank-frame fallback now returns false upstream).
        static int s_emptyFrameDiagCount = 0;
        s_emptyFrameDiagCount++;
        if (s_emptyFrameDiagCount == 1 || s_emptyFrameDiagCount % 30 == 0) {
            DEBUG_LOG_FMT("[UnifiedCapture][Diag] Empty frame received, skipping error count (method=%s src=%s diagFails=%d)",
                MethodToStr(m_currentMethod), FrameSourceStr(), s_emptyFrameDiagCount);
        }
        NoteBadFrameAndMaybeRebuildWGC();
        return false;
    }

    if (!ValidateFrame(outFrame)) {
        // [Diag-v2] Throttled diagnostics: brightness + frame source - the key evidence
        // for memory-starved intermittent black frames (iGPU shared-memory exhaustion /
        // DWM cross-adapter copy stalls). Only WGC carries a meaningful src tag.
        static int s_invalidFrameDiagCount = 0;
        s_invalidFrameDiagCount++;
        if (s_invalidFrameDiagCount == 1 || s_invalidFrameDiagCount % 30 == 0) {
            cv::Mat gray;
            if (outFrame.channels() == 3) {
                cvtColor(outFrame, gray, cv::COLOR_BGR2GRAY);
            } else {
                gray = outFrame;
            }
            DEBUG_LOG_FMT("[UnifiedCapture][Diag] Frame validation failed (black/white screen), skipping error count (method=%s src=%s brightness=%.2f size=%dx%d diagFails=%d)",
                MethodToStr(m_currentMethod), FrameSourceStr(), cv::mean(gray)[0],
                outFrame.cols, outFrame.rows, s_invalidFrameDiagCount);
        }
        NoteBadFrameAndMaybeRebuildWGC();
        return false;
    }

    // Handle real source failures
    m_consecutiveErrors++;
    DEBUG_LOG_FMT("[UnifiedCapture] Capture failed, consecutive errors: %d/%d",
        m_consecutiveErrors, MAX_CONSECUTIVE_ERRORS);

    if (ShouldDowngrade()) {
        DowngradeMethod(m_currentWindow);
    } else {
        TransitionTo(CaptureState::ExceptionWait);
    }

    return false;
}

// ============================================================
// Backend-specific capture methods
// ============================================================

bool UnifiedScreenCapture::CaptureFrameDXGI(cv::Mat& outFrame) {
    // Pre-check: skip overly frequent reinitialization
    // Initialized with epoch (far past) so the first call is not throttled
    static auto lastReinitTime = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    auto timeSinceReinit = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReinitTime).count();

    if (timeSinceReinit < 100 && timeSinceReinit >= 0) { // Just reinitialized within the last 100ms; skip
        return false;
    }

    HMONITOR hMonitor = MonitorFromWindow(m_currentWindow, MONITOR_DEFAULTTONEAREST);
    if (hMonitor == nullptr) {
        return false;
    }

    int monitorIndex = DXGIScreenCapture::GetInstance().GetMonitorIndex(hMonitor);
    if (monitorIndex == -1) {
        DXGIScreenCapture::GetInstance().StopCapture();
        if (DXGIScreenCapture::GetInstance().InitializeForWindow(m_currentWindow)) {
            monitorIndex = DXGIScreenCapture::GetInstance().GetMonitorIndex(hMonitor);
            lastReinitTime = now;
        } else {
            return false;
        }
    }

    // Try to capture a frame
    if (!DXGIScreenCapture::GetInstance().GetFrame(outFrame, monitorIndex)) {
        // Device-lost recovery: exponential backoff + full reset
        if (DXGIScreenCapture::GetInstance().NeedsReinitialization()) {
            int waitMs = DXGIScreenCapture::GetInstance().GetBackoffAndReset();
            DEBUG_LOG_FMT("[UnifiedCapture] DXGI device lost, backing off %dms", waitMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));

            // Try to reset the device
            if (DXGIScreenCapture::GetInstance().Reset()) {
                DXGIScreenCapture::GetInstance().ResetBackoff();
                // Reinitialize window capture
                if (DXGIScreenCapture::GetInstance().InitializeForWindow(m_currentWindow)) {
                    monitorIndex = DXGIScreenCapture::GetInstance().GetMonitorIndex(hMonitor);
                    lastReinitTime = now;
                    if (monitorIndex != -1) {
                        return DXGIScreenCapture::GetInstance().GetFrame(outFrame, monitorIndex);
                    }
                }
            } else {
                // If reset failed, attempt a full reinitialization
                DEBUG_LOG_FMT("[UnifiedCapture] Reset failed, attempting full reinitialization");
                DXGIScreenCapture::GetInstance().StopCapture();
                if (DXGIScreenCapture::GetInstance().InitializeForWindow(m_currentWindow)) {
                    monitorIndex = DXGIScreenCapture::GetInstance().GetMonitorIndex(hMonitor);
                    lastReinitTime = now;
                    if (monitorIndex != -1) {
                        return DXGIScreenCapture::GetInstance().GetFrame(outFrame, monitorIndex);
                    }
                }
            }
        }
        return false;
    }
    return true;
}

bool UnifiedScreenCapture::CaptureFrameWGC(cv::Mat& outFrame) {
    if (!WGCScreenCapture::GetInstance().GetFrame(outFrame, 0)) {
        // WGC device-lost recovery
        if (WGCScreenCapture::GetInstance().NeedsReinitialization()) {
            DEBUG_LOG_FMT("[UnifiedCapture] WGC device lost, attempting reset");
            if (WGCScreenCapture::GetInstance().Reset()) {
                // Reinitialization succeeded; try to get a frame
                return WGCScreenCapture::GetInstance().GetFrame(outFrame, 0);
            } else {
                DEBUG_LOG_FMT("[UnifiedCapture] WGC reset failed");
            }
        }
        return false;
    }

    // [WGC yellow-border avoidance + hysteresis] Detect whether the current frame shows a yellow border
    // Optimization: mark as unfriendly only after WGC_YELLOW_FRAME_THRESHOLD consecutive frames
    // with yellow borders. Fixes single-frame false positives caused by color shifts from
    // DWM cross-GPU copies during focus switches.
    if (m_wgcFriendly && !outFrame.empty() && outFrame.cols > 100 && outFrame.rows > 100) {
        cv::Mat hsv;
        cv::cvtColor(outFrame, hsv, cv::COLOR_BGR2HSV);

        // Check the right/bottom border regions (the yellow border usually appears at edges)
        int borderW = std::min(50, outFrame.cols / 10);
        int borderH = std::min(50, outFrame.rows / 10);

        // Check the right border
        cv::Rect rightBorder(outFrame.cols - borderW, 0, borderW, outFrame.rows);
        cv::Mat rightRegion = hsv(rightBorder);
        cv::Scalar meanH = cv::mean(rightRegion)[0];
        cv::Scalar meanS = cv::mean(rightRegion)[1];

        // Check the bottom border
        cv::Rect bottomBorder(0, outFrame.rows - borderH, outFrame.cols, borderH);
        cv::Mat bottomRegion = hsv(bottomBorder);
        cv::Scalar bottomMeanH = cv::mean(bottomRegion)[0];
        cv::Scalar bottomMeanS = cv::mean(bottomRegion)[1];

        // Yellow HSV range: H: 25-45, S: 80-255
        bool hasYellowBorder = (meanH[0] >= 25 && meanH[0] <= 45 && meanS[0] >= 80) ||
                              (bottomMeanH[0] >= 25 && bottomMeanH[0] <= 45 && bottomMeanS[0] >= 80);

        if (hasYellowBorder) {
            m_wgcYellowStreak++;
            if (m_wgcYellowStreak >= WGC_YELLOW_FRAME_THRESHOLD) {
                m_wgcFriendly = false;
                DEBUG_LOG_FMT("[UnifiedCapture] WGC yellow frame detected %d consecutive frames! H=%.1f S=%.1f bottomH=%.1f bottomS=%.1f - marking as unfriendly",
                    m_wgcYellowStreak, meanH[0], meanS[0], bottomMeanH[0], bottomMeanS[0]);
            }
            // Also log single hits without marking, for diagnostics
            else if (m_wgcYellowStreak == 1 || m_wgcYellowStreak % 3 == 0) {
                DEBUG_LOG_FMT("[UnifiedCapture] WGC yellow frame streak=%d/%d H=%.1f S=%.1f bottomH=%.1f bottomS=%.1f",
                    m_wgcYellowStreak, WGC_YELLOW_FRAME_THRESHOLD,
                    meanH[0], meanS[0], bottomMeanH[0], bottomMeanS[0]);
            }
        } else {
            // Reset streak when no yellow border
            if (m_wgcYellowStreak > 0) {
                m_wgcYellowStreak = 0;
            }
        }
    }

    return true;
}

// [Diag-v2] Frame source of the last backend read (only WGC carries a meaningful tag;
// GetFrame and this getter are both called from the capture/main-loop thread).
const char* UnifiedScreenCapture::FrameSourceStr() const {
    if (m_currentMethod == CaptureMethod::WGC) {
        return WGCScreenCapture::GetInstance().GetLastFrameSourceStr();
    }
    return "n/a";
}

// ============================================================
// [SelfHeal-v2] Consecutive black/empty frames -> WGC session rebuild
// Called from CaptureFrame's empty-frame / black-frame branches (same thread).
// Three guardrails (design agreed in the WGC black-frame hardening plan):
//   1) Count only in WGC mode while Capturing. Paused / ExceptionWait return from
//      CaptureFrame before the dispatch section, so legitimate game loading screens
//      (main-loop PauseCapture) never feed this counter; ResumeCapture additionally
//      resets the streak explicitly (guardrail against state-machine interference).
//   2) Exponential backoff: thresholds 30 -> 60 -> 120 consecutive bad frames per
//      rebuild consumed. Circuit breaker: after BLACK_STREAK_REBUILD_LIMIT rebuilds,
//      log-only (environmental causes - e.g. RAM starvation - are not fixed by
//      rebuilding). Re-armed automatically after 600 consecutive good frames.
//   3) The rebuild keeps WGC mode (StopCapture + InitializeWGCForced): no DXGI
//      re-route, no inference-thread stop, exceptions swallowed. This reuses the
//      exact StopCapture-then-restart pattern InitializeForWindow already uses
//      ("stop the old session first to avoid OnFrameArrived firing during
//      re-initialization"), protected by m_processedMutex/m_frameMutex plus the
//      process-wide VEH, so no new crash-surface is introduced.
// ============================================================
void UnifiedScreenCapture::NoteBadFrameAndMaybeRebuildWGC() {
    // Guardrail 1a: WGC only. DXGI/GDI black frames go through their own recovery paths.
    if (m_currentMethod != CaptureMethod::WGC) return;
    // Guardrail 1b: Capturing state only (defensive; Paused/ExceptionWait return earlier).
    if (m_state.load() != CaptureState::Capturing) return;

    m_goodStreakCount = 0;
    m_blackStreakCount++;

    // Backoff threshold: 30 << 0/1/2 capped at 120. Clamp the shift so the counter
    // can never overflow once the breaker is tripped and rebuilds stop being consumed.
    int shift = m_blackStreakRebuildCount;
    if (shift > 2) shift = 2;
    const int threshold = (BLACK_STREAK_REBUILD_BASE << shift) < BLACK_STREAK_REBUILD_MAX
        ? (BLACK_STREAK_REBUILD_BASE << shift)
        : BLACK_STREAK_REBUILD_MAX;

    if (m_blackStreakCount < threshold) {
        if (m_blackStreakCount == 1 || m_blackStreakCount % 30 == 0) {
            DEBUG_LOG_FMT("[UnifiedCapture][SelfHeal] WGC bad-frame streak %d (threshold %d, rebuilds used %d/%d)",
                m_blackStreakCount, threshold, m_blackStreakRebuildCount, BLACK_STREAK_REBUILD_LIMIT);
        }
        return;
    }

    // Guardrail 2: circuit breaker tripped - log only.
    if (m_blackStreakRebuildCount >= BLACK_STREAK_REBUILD_LIMIT) {
        if (m_blackStreakCount % 120 == 0) {
            DEBUG_LOG_FMT("[UnifiedCapture][SelfHeal] WGC bad-frame streak %d (rebuild limit %d reached) - logging only, no rebuild",
                m_blackStreakCount, BLACK_STREAK_REBUILD_LIMIT);
        }
        return;
    }

    DEBUG_LOG_FMT("[UnifiedCapture][SelfHeal] WGC bad-frame streak %d >= %d - rebuilding WGC session (attempt %d/%d)",
        m_blackStreakCount, threshold, m_blackStreakRebuildCount + 1, BLACK_STREAK_REBUILD_LIMIT);
    m_blackStreakCount = 0;
    m_goodStreakCount = 0;
    m_blackStreakRebuildCount++;

    HWND rebuildWnd = m_currentWindow;
    bool clientOnly = m_captureClientAreaOnly;
    if (!rebuildWnd || !ValidateWindow(rebuildWnd)) {
        DEBUG_LOG_FMT("[UnifiedCapture][SelfHeal] WGC rebuild skipped: target window no longer valid");
        return;
    }

    try {
        WGCScreenCapture::GetInstance().StopCapture();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let COM/D3D settle (same as ForceReinitialize)
        if (InitializeWGCForced(rebuildWnd, clientOnly)) {
            DEBUG_LOG_FMT("[UnifiedCapture][SelfHeal] WGC session rebuilt OK");
        } else {
            DEBUG_LOG_FMT("[UnifiedCapture][SelfHeal] WGC session rebuild FAILED (CaptureFrame will auto-reinit via !m_initialized path)");
        }
    } catch (...) {
        DEBUG_LOG_FMT("[UnifiedCapture][SelfHeal] WGC session rebuild exception (swallowed)");
    }
}

bool UnifiedScreenCapture::CaptureFrameGDI(cv::Mat& outFrame) {
    Screenshot::getInstance().getScreenShot(outFrame, m_currentWindow);
    return !outFrame.empty();
}

// ============================================================
// Frame Validation - Black/White detection
// ============================================================

bool UnifiedScreenCapture::ValidateFrame(const cv::Mat& frame) const {
    if (frame.empty()) return false;
    if (frame.cols < 100 || frame.rows < 100) return false;

    cv::Mat gray;
    if (frame.channels() == 3) {
        cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = frame;
    }

    double meanBrightness = cv::mean(gray)[0];

    if (meanBrightness < BLACK_THRESHOLD) {
        return false;  // Black screen (game loading/CG)
    }
    if (meanBrightness > WHITE_THRESHOLD) {
        return false;  // White screen (overexposed/transition)
    }

    return true;
}

// ============================================================
// Method switching (internal control only)
// ============================================================

bool UnifiedScreenCapture::SwitchToMethod(CaptureMethod method, HWND hWnd) {
    if (method == m_currentMethod) return true;

    DEBUG_LOG_FMT("[UnifiedCapture] Switching method: %d -> %d", (int)m_currentMethod, (int)method);

    // [Focus recovery optimization] If just recovering from focus loss and upgrading, try directly
    bool wasFocusRecovery = (m_focusLost && method == CaptureMethod::DXGI && m_currentMethod == CaptureMethod::WGC);
    if (wasFocusRecovery) {
        DEBUG_LOG_FMT("[UnifiedCapture] Focus recovery upgrade attempt: WGC -> DXGI");
        m_focusLost = false;
    }

    // Stop current backend
    switch (m_currentMethod) {
    case CaptureMethod::DXGI:
        DXGIScreenCapture::GetInstance().StopCapture();
        // Wait for GPU driver to recover before switching to WGC
#ifdef _DEBUG
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
#endif
        break;
    case CaptureMethod::WGC:
        WGCScreenCapture::GetInstance().StopCapture();
        // [Critical fix] Wait for WGC COM resources to be fully released
#ifdef _DEBUG
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
#else
        // [Focus recovery optimization] Shorter wait on recovery to speed up DXGI rebuild
        std::this_thread::sleep_for(std::chrono::milliseconds(wasFocusRecovery ? 50 : 100));
#endif
        break;
    case CaptureMethod::GDI:
        break;
    }

    // Start new backend
    bool success = false;
    switch (method) {
    case CaptureMethod::DXGI:
        success = TryInitializeDXGI(hWnd);
        break;
    case CaptureMethod::WGC:
        success = TryInitializeWGC(hWnd);
        break;
    case CaptureMethod::GDI:
        success = TryInitializeGDI();
        break;
    }

    if (success) {
        m_currentMethod = method;
        m_consecutiveErrors = 0;
        // [SelfHeal-v2] A method switch means a fresh session; clear the bad-frame streak.
        m_blackStreakCount = 0;
        TransitionTo(CaptureState::Capturing);

        // [Focus recovery optimization] Upgrade succeeded; clear the focus-lost flag
        if (wasFocusRecovery) {
            m_focusLost = false;
            DEBUG_LOG_FMT("[UnifiedCapture] Focus recovery upgrade SUCCESS");
        }

        // Update highest working method (only upgrade, never downgrade here)
        if ((int)method > (int)m_highestWorkingMethod) {
            m_highestWorkingMethod = method;
        }
    } else if (wasFocusRecovery) {
        // [Focus recovery optimization] Upgrade failed; mark as focus-lost and wait for the next attempt
        m_focusLost = true;
        DEBUG_LOG_FMT("[UnifiedCapture] Focus recovery upgrade FAILED, will retry");
    }

    return success;
}

bool UnifiedScreenCapture::DowngradeMethod(HWND hWnd) {
    DEBUG_LOG_FMT("[UnifiedCapture] Downgrading after %d consecutive errors", m_consecutiveErrors);

    CaptureMethod nextMethod;
    switch (m_currentMethod) {
    case CaptureMethod::DXGI:
        nextMethod = CaptureMethod::WGC;
        break;
    case CaptureMethod::WGC:
        nextMethod = CaptureMethod::GDI;
        break;
    case CaptureMethod::GDI:
        DEBUG_LOG_FMT("[UnifiedCapture] Already at GDI, cannot downgrade further");
        m_consecutiveErrors = 0;  // Reset to avoid infinite loop
        TransitionTo(CaptureState::Stopped);
        return false;
    }

    return SwitchToMethod(nextMethod, hWnd);
}

// ============================================================
// Initialization helpers
// ============================================================

bool UnifiedScreenCapture::TryInitializeDXGI(HWND hWnd) {
    try {
        wchar_t title[256] = {};
        GetWindowTextW(hWnd, title, 256);
        DEBUG_LOG_FMT("[UnifiedCapture] TryInitializeDXGI: hwnd=%p title=%ls", (void*)hWnd, title);
        // [Hybrid GPU] Pass the flag before InitializeForWindow so it prefers dGPU when m_isHybridGPU==true
        DXGIScreenCapture::GetInstance().SetHybridGPUMode(m_isHybridGPU);
        bool ok = DXGIScreenCapture::GetInstance().InitializeForWindow(hWnd);
        if (!ok) {
            DEBUG_LOG_FMT("[UnifiedCapture] DXGI InitializeForWindow returned false for hwnd=%p", (void*)hWnd);
        }
        return ok;
    }
    catch (...) {
        DEBUG_LOG_FMT("[UnifiedCapture] DXGI initialization exception for hwnd=%p", (void*)hWnd);
        return false;
    }
}

bool UnifiedScreenCapture::TryInitializeWGC(HWND hWnd) {
    try {
        wchar_t title[256] = {};
        GetWindowTextW(hWnd, title, 256);
        DEBUG_LOG_FMT("[UnifiedCapture] TryInitializeWGC: hwnd=%p title=%ls", (void*)hWnd, title);
        if (!WGCScreenCapture::GetInstance().InitializeForWindow(hWnd)) {
            DEBUG_LOG_FMT("[UnifiedCapture] WGC InitializeForWindow returned false for hwnd=%p", (void*)hWnd);
            return false;
        }
        if (m_captureClientAreaOnly) {
            WGCScreenCapture::GetInstance().SetCaptureClientAreaOnly(true);
        }
        return true;
    }
    catch (winrt::hresult_error const& ex) {
        DEBUG_LOG_FMT("[UnifiedCapture] WGC initialization hresult_error: %ls", winrt::to_string(ex.message()).c_str());
        return false;
    }
    catch (...) {
        DEBUG_LOG_FMT("[UnifiedCapture] WGC initialization unknown exception for hwnd=%p", (void*)hWnd);
        return false;
    }
}

bool UnifiedScreenCapture::TryInitializeGDI() {
    return true;
}

// ============================================================
// Window management
// ============================================================

bool UnifiedScreenCapture::UpdateTargetWindow(HWND hWnd) {
    if (!ValidateWindow(hWnd)) {
        return false;
    }
    if (hWnd == m_currentWindow) {
        return true;
    }

    DEBUG_LOG_FMT("[UnifiedCapture] Window changed, reinitializing...");
    Release();
    // [Critical fix] Wait for COM/D3D resources to be fully released
#ifdef _DEBUG
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
    return Initialize(hWnd, m_captureClientAreaOnly);
}

void UnifiedScreenCapture::Release() {
    if (!m_initialized) return;

    switch (m_currentMethod) {
    case CaptureMethod::DXGI:
        DXGIScreenCapture::GetInstance().StopCapture();
        break;
    case CaptureMethod::WGC:
        WGCScreenCapture::GetInstance().StopCapture();
        break;
    case CaptureMethod::GDI:
        break;
    }

    m_initialized = false;
    m_currentWindow = nullptr;
    m_consecutiveErrors = 0;
    TransitionTo(CaptureState::Idle);
}

void UnifiedScreenCapture::PauseCapture() {
    if (m_state.load() == CaptureState::Capturing) {
        m_pausedSince = std::chrono::steady_clock::now();
        // [Hybrid GPU optimization - long focus-out improvement] Record pause start time for SoftReinitialize dynamic warmup
        m_pauseStartTime = m_pausedSince;
        m_pauseActive = true;
        TransitionTo(CaptureState::Paused);
        DEBUG_LOG_FMT("[UnifiedCapture] Capture paused (CG/loading screen detected)");
    }
}

void UnifiedScreenCapture::ResumeCapture() {
    if (m_state.load() == CaptureState::Paused) {
        m_consecutiveErrors = 0;
        // [SelfHeal-v2] Guardrail: the main-loop black-screen pause is legitimate
        // (game loading screens); reset the streak so it never feeds the WGC
        // rebuild counter.
        m_blackStreakCount = 0;
        // [Hybrid GPU optimization - long focus-out improvement] Clear pause state; SoftReinitialize will re-check
        m_pauseActive = false;
        TransitionTo(CaptureState::Capturing);
        DEBUG_LOG_FMT("[UnifiedCapture] Capture resumed (was paused for %lld ms)",
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_pauseStartTime).count());
    }
}

void UnifiedScreenCapture::ForceReinitialize(HWND hWnd) {
    // Prefer the passed-in hWnd; m_currentWindow may already be cleared by Release
    HWND savedWnd = hWnd ? hWnd : m_currentWindow;
    bool savedClientOnly = m_captureClientAreaOnly;
    Release();

    // [Critical fix] Wait for COM/D3D resources to be fully released, avoiding crashes
    // from resource races. Device release takes longer in Debug mode.
#ifdef _DEBUG
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif

    // Keep a valid window handle even if Initialize fails,
    // so the automatic reinitialization in CaptureFrame can retry later
    if (savedWnd && ValidateWindow(savedWnd)) {
        m_currentWindow = savedWnd;
        Initialize(savedWnd, savedClientOnly);
    }

    // [DXGI upgrade blacklist] Reset the DXGI blacklist after forced reinitialization to allow retrying DXGI
    m_dxgiUpgradeBlacklisted = false;
}

// ============================================================
// [Hybrid GPU optimization] SoftReinitialize - soft-first reinitialization for focus recovery
// ============================================================
// Fundamental difference from ForceReinitialize: on hybrid-GPU laptops, at the moment of
// a focus switch the driver may be performing a GPU migration (Mux switch / Optimus state
// change). Calling Release+Initialize at that moment will almost certainly:
//   1. Pick the wrong GPU adapter
//   2. Get stale buffers / black frames
//   3. Trigger WGC yellow-border detection false positives
//   4. Trigger black-screen protection -> PauseCapture -> deadlock
//
// SoftReinitialize prefers keeping the existing capture session and validates itself
// by capturing warmup frames via CaptureFrame. Only when validation fails does it fall
// back to ForceReinitialize.
//
// [Long focus-out improvement] Three key optimizations:
//  1. Window size change detection: the player may have changed the resolution while
//     unfocused, while the WGC FramePool still uses the old size
//  2. Dynamic warmup frame count: focus loss <10s=30 frames, 10-120s=60 frames, >120s=120 frames
//  3. Multi-stage attempts: if round 1 fails, retry with a larger warmup, up to 3 rounds;
//     only ForceReinitialize after all fail
bool UnifiedScreenCapture::SoftReinitialize(HWND hWnd) {
    HWND savedWnd = hWnd ? hWnd : m_currentWindow;
    if (!savedWnd || !ValidateWindow(savedWnd)) {
        DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: invalid HWND, fallback to hard reinit");
        return false;
    }

    // 1. Diagnostics: report current GPU adapter state (for hybrid-GPU troubleshooting)
    LogAdapterDiagnostics(savedWnd);

    // 2. Compute focus-out duration (from PauseCapture until now)
    long long pauseDurationMs = 0;
    bool hadPauseRecord = false;
    if (m_pauseActive) {
        pauseDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_pauseStartTime).count();
        hadPauseRecord = true;
    }

    // 3. [Long focus-out improvement] Window size change detection
    // The player may have changed the game resolution while unfocused. The WGC FramePool
    // was created with the old size -> after Resume, frames are misaligned / black / WGC yellow-bordered
    RECT currentRect = {};
    if (GetClientRect(savedWnd, &currentRect)) {
        int curW = currentRect.right - currentRect.left;
        int curH = currentRect.bottom - currentRect.top;
        if (m_hasLastValidSize && m_lastValidWindowW > 0 && m_lastValidWindowH > 0) {
            // Allow 10% tolerance (borders may shift slightly during window drags/taskbar switches)
            int wDiff = std::abs(curW - m_lastValidWindowW);
            int hDiff = std::abs(curH - m_lastValidWindowH);
            int wThresh = std::max(50, m_lastValidWindowW / 10);
            int hThresh = std::max(50, m_lastValidWindowH / 10);
            if (wDiff > wThresh || hDiff > hThresh) {
                DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: WINDOW SIZE CHANGED during pause! old=%dx%d new=%dx%d diff=(%d,%d) > thresh=(%d,%d)",
                    m_lastValidWindowW, m_lastValidWindowH, curW, curH, wDiff, hDiff, wThresh, hThresh);
                DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: skipping soft path, hard reinit needed for new WGC FramePool size");
                // Window size changed -> WGC FramePool is definitely wrong -> skip soft recovery and hard-reset directly
                ResetWGCFriendlyFlag();
                ForceReinitialize(savedWnd);
                m_hasLastValidSize = true;
                m_lastValidWindowW = curW;
                m_lastValidWindowH = curH;
                return false;
            }
        }
        // Record current window size as reference for the next round
        m_lastValidWindowW = curW;
        m_lastValidWindowH = curH;
        m_hasLastValidSize = true;
    }

    // 4. Prefer a pure Resume path first (without releasing any resources)
    DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: state=%d initialized=%d pauseDuration=%lldms",
        (int)m_state.load(), (int)m_initialized, pauseDurationMs);

    // 4.1 If in the Paused state (after black-screen pause), just resume
    if (m_state.load() == CaptureState::Paused) {
        ResumeCapture();
    }

    // 4.2 If not initialized but the window is valid, call Initialize
    if (!m_initialized) {
        if (ValidateWindow(savedWnd)) {
            m_currentWindow = savedWnd;
            if (Initialize(savedWnd, m_captureClientAreaOnly)) {
                DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: Initialize succeeded");
            } else {
                DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: Initialize failed, will retry in CaptureFrame");
            }
        }
    }

    // 5. [Long focus-out improvement] Choose the initial warmup frame count from focus-out duration
    int warmupFrames = SOFT_REINIT_WARMUP_FRAMES_SHORT;  // Default 30 frames
    const char* pauseBucket = "SHORT";
    if (hadPauseRecord) {
        if (pauseDurationMs >= SOFT_REINIT_PAUSE_THRESHOLD_LONG_S * 1000) {
            warmupFrames = SOFT_REINIT_WARMUP_FRAMES_LONG;  // 120 frames ~4s
            pauseBucket = "LONG";
        } else if (pauseDurationMs >= SOFT_REINIT_PAUSE_THRESHOLD_MEDIUM_S * 1000) {
            warmupFrames = SOFT_REINIT_WARMUP_FRAMES_MEDIUM;  // 60 frames ~2s
            pauseBucket = "MEDIUM";
        }
    }
    DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: pause bucket=%s, initial warmup=%d frames (~%dms)",
        pauseBucket, warmupFrames, warmupFrames * SOFT_REINIT_WARMUP_FRAME_DELAY_MS);

    // 6. [Long focus-out improvement] Multi-stage attempts: warmup frame count grows each round, up to 3 rounds
    int finalStage = 0;
    int finalValid = 0;
    int finalSampled = 0;
    bool succeeded = false;
    for (int stage = 1; stage <= SOFT_REINIT_MAX_STAGES; stage++) {
        int stageWarmup = warmupFrames * stage;  // 30 -> 60 -> 120 frames
        int validFrames = 0;
        int sampledFrames = 0;
        cv::Mat warmupFrame;

        DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: STAGE %d/%d, warmup=%d frames",
            stage, SOFT_REINIT_MAX_STAGES, stageWarmup);

        for (int i = 0; i < stageWarmup; i++) {
            if (m_state.load() == CaptureState::Paused) {
                // Capture skipped by black-screen protection during warmup; count as sampled but not valid
                sampledFrames++;
            } else {
                bool capOk = CaptureFrame(warmupFrame);
                sampledFrames++;
                if (capOk && !warmupFrame.empty() && IsFrameAlive(warmupFrame)) {
                    validFrames++;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(SOFT_REINIT_WARMUP_FRAME_DELAY_MS));

            // Provide in-stage progress feedback midway through warmup (keeps the log from looking frozen)
            if ((i + 1) % 30 == 0) {
                DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: stage %d progress %d/%d valid=%d",
                    stage, i + 1, stageWarmup, validFrames);
            }
        }

        finalStage = stage;
        finalValid = validFrames;
        finalSampled = sampledFrames;

        DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: stage %d done, valid=%d/%d",
            stage, validFrames, sampledFrames);

        // Check whether this round succeeded
        if (validFrames >= SOFT_REINIT_MIN_VALID_RATIO_NUM) {
            succeeded = true;
            break;
        }

        // Round failed: if not the last stage, give the driver some time to recover before the next round
        if (stage < SOFT_REINIT_MAX_STAGES) {
            int backoffMs = 200 * stage;  // 200ms -> 400ms
            DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: stage %d failed, backoff %dms before stage %d",
                stage, backoffMs, stage + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
        }
    }

    DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: final stage=%d/%d, valid=%d/%d, method=%d, state=%d",
        finalStage, SOFT_REINIT_MAX_STAGES, finalValid, finalSampled,
        (int)m_currentMethod, (int)m_state.load());

    // 7. Handle the result
    if (succeeded) {
        DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: SUCCESS (stage=%d valid=%d/%d >= %d/%d)",
            finalStage, finalValid, finalSampled,
            SOFT_REINIT_MIN_VALID_RATIO_NUM, SOFT_REINIT_MIN_VALID_RATIO_DEN);

        // Reset internal state counters
        m_consecutiveErrors = 0;
        m_staticFrameCount = 0;
        m_lastFrameForDiff = cv::Mat();

        // Reset the WGC-friendly flag (give WGC another chance to prove itself)
        if (m_currentMethod == CaptureMethod::WGC) {
            ResetWGCFriendlyFlag();
        }

        // Ensure the state machine is in Capturing
        if (m_state.load() != CaptureState::Capturing) {
            TransitionTo(CaptureState::Capturing);
        }

        // Clear the focus-lost flag
        m_focusLost = false;
        m_pauseActive = false;  // Success path; clear pause state
        return true;
    }

    // 8. All soft-recovery stages failed -> hard reset
    DEBUG_LOG_FMT("[UnifiedCapture] SoftReinitialize: ALL STAGES FAILED (final valid=%d/%d < %d/%d), falling back to hard reinit",
        finalValid, finalSampled,
        SOFT_REINIT_MIN_VALID_RATIO_NUM, SOFT_REINIT_MIN_VALID_RATIO_DEN);

    // Reset the WGC-friendly flag before hard reset
    ResetWGCFriendlyFlag();

    ForceReinitialize(savedWnd);
    return false;
}

// ============================================================
// [Hybrid GPU optimization] IsFrameAlive - frame liveness check
// ============================================================
// Checks whether the current frame is:
//   1. Not black, not white
//   2. Inter-frame difference > threshold (game visuals differ every frame)
//   3. Not a stale buffer (30 consecutive frames unchanged)
// Solves hybrid-GPU issues of picking the wrong adapter or stale buffers.
bool UnifiedScreenCapture::IsFrameAlive(const cv::Mat& frame) const {
    if (frame.empty()) return false;
    if (frame.cols < 100 || frame.rows < 100) return false;

    cv::Mat gray;
    if (frame.channels() == 3) {
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = frame;
    }

    double meanBrightness = cv::mean(gray)[0];
    if (meanBrightness < BLACK_THRESHOLD) return false;  // Black screen
    if (meanBrightness > WHITE_THRESHOLD) return false;  // White screen

    // Inter-frame difference detection (guards against stale buffers)
    if (!m_lastFrameForDiff.empty() && m_lastFrameForDiff.size() == frame.size()
        && m_lastFrameForDiff.type() == gray.type()) {
        cv::Mat diff;
        cv::absdiff(gray, m_lastFrameForDiff, diff);
        double diffMean = cv::mean(diff)[0];
        if (diffMean < MIN_FRAME_DIFF) {
            m_staticFrameCount++;
            if (m_staticFrameCount >= MAX_STATIC_FRAMES) {
                // 30 consecutive frames with almost no change: possibly a stale buffer.
                // Do not return false directly (may be a static game scene); emit a warning
                // instead so the caller can decide from context whether to use this frame.
                DEBUG_LOG_FMT("[UnifiedCapture] Frame static warning: %d consecutive frames with diff=%.3f",
                    m_staticFrameCount, diffMean);
            }
        } else {
            m_staticFrameCount = 0;
        }
    }
    m_lastFrameForDiff = gray.clone();

    return true;
}

// ============================================================
// [Hybrid GPU optimization] ProbeAdapterForWindow - diagnostics: probe the adapter for hwnd
// ============================================================
UnifiedScreenCapture::AdapterInfo UnifiedScreenCapture::ProbeAdapterForWindow(HWND hWnd) const {
    AdapterInfo info;
    if (!hWnd || !IsWindow(hWnd)) {
        return info;
    }

    HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    if (hMonitor == nullptr) {
        return info;
    }

    CComPtr<IDXGIFactory1> dxgiFactory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&dxgiFactory));
    if (FAILED(hr)) {
        return info;
    }

    CComPtr<IDXGIAdapter1> bestAdapter;
    SIZE_T bestVideoMem = 0;
    for (UINT i = 0; ; ++i) {
        CComPtr<IDXGIAdapter1> adapter;
        hr = dxgiFactory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;

        DXGI_ADAPTER_DESC1 desc;
        if (FAILED(adapter->GetDesc1(&desc))) continue;

        // Skip software adapters
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            if (bestAdapter == nullptr) {
                bestAdapter = adapter;
                bestVideoMem = desc.DedicatedVideoMemory;
                info.usedSoftware = true;
            }
            continue;
        }

        // Check whether it owns the target monitor
        CComPtr<IDXGIOutput> output;
        for (UINT j = 0; ; ++j) {
            hr = adapter->EnumOutputs(j, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) continue;

            DXGI_OUTPUT_DESC outDesc;
            if (SUCCEEDED(output->GetDesc(&outDesc)) && outDesc.Monitor == hMonitor) {
                info.found = true;
                info.description = desc.Description;
                info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
                info.flags = desc.Flags;
                return info;
            }
        }

        // Fallback: keep the adapter with the largest video memory
        if (desc.DedicatedVideoMemory > bestVideoMem) {
            bestVideoMem = desc.DedicatedVideoMemory;
            bestAdapter = adapter;
            info.description = desc.Description;
            info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
            info.flags = desc.Flags;
        }
    }

    if (!info.found) {
        info.usedFallback = true;
    }
    return info;
}

int UnifiedScreenCapture::DetectDisplayGPUType(HWND hWnd) const {
    AdapterInfo info = ProbeAdapterForWindow(hWnd);
    if (!info.found || info.usedFallback) {
        DEBUG_LOG_FMT("[UnifiedCapture] DetectDisplayGPUType: detection failed (found=%d fallback=%d)",
            (int)info.found, (int)info.usedFallback);
        return -1;
    }
    // dGPU criterion: dedicatedVideoMemory > 1GB (consistent with the TryInitializeOnDiscreteGPU standard)
    constexpr SIZE_T DGPU_VRAM_THRESHOLD = 1ull * 1024 * 1024 * 1024;
    bool isDiscrete = (info.dedicatedVideoMemory >= DGPU_VRAM_THRESHOLD);
    char descA[256] = { 0 };
    WideCharToMultiByte(CP_ACP, 0, info.description.c_str(), -1, descA, sizeof(descA), NULL, NULL);
    DEBUG_LOG_FMT("[UnifiedCapture] DetectDisplayGPUType: '%s' vram=%lluMB -> %s",
        descA, (unsigned long long)(info.dedicatedVideoMemory / (1024 * 1024)),
        isDiscrete ? "dGPU" : "iGPU");
    return isDiscrete ? 1 : 0;
}

void UnifiedScreenCapture::LogAdapterDiagnostics(HWND hWnd) const {
    AdapterInfo info = ProbeAdapterForWindow(hWnd);
    if (info.found) {
        char descA[256] = { 0 };
        WideCharToMultiByte(CP_ACP, 0, info.description.c_str(), -1, descA, sizeof(descA), NULL, NULL);
        DEBUG_LOG_FMT("[UnifiedCapture] [GPU] selected='%s' vram=%lluMB flags=0x%x",
            descA, (unsigned long long)(info.dedicatedVideoMemory / (1024 * 1024)), info.flags);
    } else if (info.usedFallback) {
        char descA[256] = { 0 };
        WideCharToMultiByte(CP_ACP, 0, info.description.c_str(), -1, descA, sizeof(descA), NULL, NULL);
        DEBUG_LOG_FMT("[UnifiedCapture] [GPU] FALLBACK selected='%s' vram=%lluMB software=%d",
            descA, (unsigned long long)(info.dedicatedVideoMemory / (1024 * 1024)), info.usedSoftware);
    } else {
        DEBUG_LOG_FMT("[UnifiedCapture] [GPU] NO ADAPTER FOUND for hwnd=%p", (void*)hWnd);
    }
}


void UnifiedScreenCapture::SetImageProcessConfig(const WGCScreenCapture::ImageProcessConfig& config) {
    switch (m_currentMethod) {
    case CaptureMethod::WGC:
        WGCScreenCapture::GetInstance().SetImageProcessConfig(config);
        break;
    case CaptureMethod::DXGI: {
        DXGIScreenCapture::ImageProcessConfig dxgiConfig;
        dxgiConfig.enableResize = config.enableResize;
        dxgiConfig.resizeWidth = config.resizeWidth;
        dxgiConfig.resizeHeight = config.resizeHeight;
        dxgiConfig.enableROI = config.enableROI;
        dxgiConfig.roiRect = config.roiRect;
        dxgiConfig.enableCanvas = config.enableCanvas;
        dxgiConfig.canvasRect = config.canvasRect;
        dxgiConfig.destRect = config.destRect;
        DXGIScreenCapture::GetInstance().SetImageProcessConfig(dxgiConfig);
        break;
    }
    case CaptureMethod::GDI:
        break;
    }
}

bool UnifiedScreenCapture::ValidateWindow(HWND hWnd) const {
    return hWnd != nullptr && IsWindow(hWnd);
}

// ============================================================
// Version detection utilities
// ============================================================

bool UnifiedScreenCapture::IsWindows10BuildOrGreater(DWORD build)
{
    OSVERSIONINFOEXW osvi = { sizeof(osvi), 0, 0, 0, 0, {0}, 0, 0 };
    DWORDLONG const dwlConditionMask = VerSetConditionMask(
        VerSetConditionMask(
            VerSetConditionMask(
                0, VER_MAJORVERSION, VER_GREATER_EQUAL),
            VER_MINORVERSION, VER_GREATER_EQUAL),
        VER_BUILDNUMBER, VER_GREATER_EQUAL);

    osvi.dwMajorVersion = 10;
    osvi.dwMinorVersion = 0;
    osvi.dwBuildNumber = build;

    return VerifyVersionInfoW(&osvi,
        VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER,
        dwlConditionMask) != FALSE;
}

bool UnifiedScreenCapture::IsRealWindows10BuildOrGreater(DWORD build) {
    HMODULE hMod = ::GetModuleHandleW(L"ntdll.dll");
    if (hMod) {
        RtlGetVersionPtr fxPtr = (RtlGetVersionPtr)::GetProcAddress(hMod, "RtlGetVersion");
        if (fxPtr != nullptr) {
            RTL_OSVERSIONINFOW rovi = { 0 };
            rovi.dwOSVersionInfoSize = sizeof(rovi);

            if (fxPtr(&rovi) == 0) {
                if (rovi.dwMajorVersion > 10) return true;
                if (rovi.dwMajorVersion < 10) return false;
                return rovi.dwBuildNumber >= build;
            }
        }
    }
    return false;
}
