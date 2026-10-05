#pragma once
// ============================================================================
// ScreenAspectDetector
//
// Reads the game's configuration file (GameUserSettings.ini) to determine the
// physical monitor's aspect ratio.  This is needed because the screenshot
// dimensions reflect the **game render resolution**, not the physical monitor
// resolution.  A 21:9 ultrawide monitor may render at 1920x1080 (identical to
// a native 16:9 screenshot), but requires different ROI coordinates.
//
// Detection flow (priority order):
//   1. GetGameMonitorResolution() — physical monitor native resolution via
//      MonitorFromWindow + GetMonitorInfoW.  Multi-monitor aware.  This is
//      the ONLY reliable way to distinguish 21:9 from 16:9 when the game
//      renders at the same resolution (e.g. 1920x1080 on both).
//   2. DetectPhysicalResolution() — reads ResolutionSizeX/Y from INI.
//      WARNING: this is the render resolution, not the physical monitor.
//      Used only when the game window cannot be located.
//   3. GetDesktopWindow() rect — last resort, unreliable on multi-monitor.
//   4. Hardcoded 16:9 fallback.
// ============================================================================

#include <string>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cmath>
#include <windows.h>
#include <shlobj.h>   // SHGetKnownFolderPath / SHGetFolderPathW

namespace screen_aspect {

// ---------------------------------------------------------------------------
// Aspect category constants
// ---------------------------------------------------------------------------
inline constexpr const char* ASPECT_16_9  = "16:9";
inline constexpr const char* ASPECT_16_10 = "16:10";
inline constexpr const char* ASPECT_21_9  = "21:9";
inline constexpr const char* ASPECT_4_3   = "4:3";
inline constexpr const char* ASPECT_5_4   = "5:4";
inline constexpr const char* ASPECT_UNKNOWN = "16:9";  // fallback

// ---------------------------------------------------------------------------
// Classify aspect ratio into a category string
// ---------------------------------------------------------------------------
inline std::string ClassifyAspect(double aspect) {
    // Order matters: check narrower tolerances first, wider tolerances last
    if (std::fabs(aspect - 21.0 / 9.0) < 0.15) return ASPECT_21_9;   // ~2.333
    if (std::fabs(aspect - 16.0 / 9.0) < 0.05) return ASPECT_16_9;   // 1.778
    if (std::fabs(aspect - 16.0 / 10.0) < 0.05) return ASPECT_16_10;  // 1.6
    if (std::fabs(aspect - 4.0 / 3.0) < 0.05)  return ASPECT_4_3;    // 1.333
    if (std::fabs(aspect - 5.0 / 4.0) < 0.05)  return ASPECT_5_4;    // 1.25

    // Wider tolerance for 21:9 (some monitors are 2.37:1, 2.35:1, etc.)
    if (aspect > 2.2) return ASPECT_21_9;

    return ASPECT_16_9;  // safe fallback
}

// ---------------------------------------------------------------------------
// Resolve %LOCALAPPDATA% to a filesystem path
// ---------------------------------------------------------------------------
inline std::wstring GetLocalAppDataPath() {
    wchar_t* path = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path);
    if (FAILED(hr)) {
        // Fallback: use environment variable
        wchar_t envBuf[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", envBuf, MAX_PATH) > 0) {
            return std::wstring(envBuf);
        }
        return L"";
    }
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

// ---------------------------------------------------------------------------
// Read an INI-style key=value from a file stream (ignores sections)
// ---------------------------------------------------------------------------
inline std::string ReadIniValue(const std::string& fileContent,
                                 const std::string& key) {
    // Search for "key=" or "key = " patterns
    std::istringstream stream(fileContent);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;

        // Skip section headers [Section]
        if (line[start] == '[') continue;
        // Skip comments
        if (line[start] == ';' || line[start] == '#') continue;

        size_t eq = line.find('=', start);
        if (eq == std::string::npos) continue;

        std::string k = line.substr(start, eq - start);
        // Trim trailing whitespace from key
        size_t kend = k.find_last_not_of(" \t\r");
        if (kend != std::string::npos) k = k.substr(0, kend + 1);

        if (k == key) {
            std::string v = line.substr(eq + 1);
            // Trim leading/trailing whitespace from value
            size_t vstart = v.find_first_not_of(" \t\r");
            if (vstart != std::string::npos) v = v.substr(vstart);
            size_t vend = v.find_last_not_of(" \t\r\n");
            if (vend != std::string::npos) v = v.substr(0, vend + 1);
            return v;
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// Read the whole INI file into a string
// ---------------------------------------------------------------------------
inline std::string ReadFileToString(const std::wstring& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::ostringstream buf;
    buf << file.rdbuf();
    return buf.str();
}

// ---------------------------------------------------------------------------
// Detect physical screen resolution from GameUserSettings.ini
// Returns true on success.
// ---------------------------------------------------------------------------
inline bool DetectPhysicalResolution(int& outW, int& outH) {
    outW = 0;
    outH = 0;

    // Build path: %LOCALAPPDATA%\NGR\Saved\Config\WindowsNoEditor\GameUserSettings.ini
    std::wstring localAppData = GetLocalAppDataPath();
    if (localAppData.empty()) return false;

    std::wstring iniPath = localAppData + L"\\NGR\\Saved\\Config\\WindowsNoEditor\\GameUserSettings.ini";

    std::string content = ReadFileToString(iniPath);
    if (content.empty()) return false;

    // Try multiple possible key names (Unreal Engine uses different keys
    // depending on version and whether the game is windowed/fullscreen)
    static const char* RES_KEYS_X[] = {
        "ResolutionSizeX",
        "ResX",
        "DesktopResolutionX",
        "LastUserConfirmedResolutionSizeX"
    };
    static const char* RES_KEYS_Y[] = {
        "ResolutionSizeY",
        "ResY",
        "DesktopResolutionY",
        "LastUserConfirmedResolutionSizeY"
    };

    int w = 0, h = 0;
    for (int i = 0; i < 4; ++i) {
        std::string val = ReadIniValue(content, RES_KEYS_X[i]);
        if (!val.empty()) {
            w = std::atoi(val.c_str());
            if (w > 0) break;
        }
    }
    for (int i = 0; i < 4; ++i) {
        std::string val = ReadIniValue(content, RES_KEYS_Y[i]);
        if (!val.empty()) {
            h = std::atoi(val.c_str());
            if (h > 0) break;
        }
    }

    if (w > 0 && h > 0) {
        outW = w;
        outH = h;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Detect the physical resolution of the monitor the game window is on.
// Uses MonitorFromWindow + GetMonitorInfo for multi-monitor awareness.
// Returns true on success.
// ---------------------------------------------------------------------------
inline bool GetGameMonitorResolution(int& outW, int& outH) {
    outW = 0;
    outH = 0;

    // Find the game window by title
    struct GameFinder {
        HWND hwnd = nullptr;
        static BOOL CALLBACK EnumProc(HWND h, LPARAM lParam) {
            auto* self = reinterpret_cast<GameFinder*>(lParam);
            if (!IsWindowVisible(h)) return TRUE;
            wchar_t title[256]{};
            GetWindowTextW(h, title, 256);
            std::wstring wt(title);
            if (wt.find(L"王者荣耀世界") != std::wstring::npos) {
                self->hwnd = h;
                return FALSE;
            }
            return TRUE;
        }
    };

    GameFinder finder;
    EnumWindows(GameFinder::EnumProc, reinterpret_cast<LPARAM>(&finder));
    if (!finder.hwnd) return false;

    // Get the monitor the game window is on
    HMONITOR hMon = MonitorFromWindow(finder.hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hMon) return false;

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return false;

    outW = mi.rcMonitor.right - mi.rcMonitor.left;
    outH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    return (outW > 0 && outH > 0);
}

// ---------------------------------------------------------------------------
// Singleton / cached detector
// ---------------------------------------------------------------------------
class ScreenAspect {
public:
    static ScreenAspect& GetInstance() {
        static ScreenAspect instance;
        return instance;
    }

    // Returns the aspect category string (e.g. "21:9", "16:9").
    // Lazy-initializes by reading the INI on first call.
    const std::string& GetAspectCategory() {
        if (!m_initialized) {
            Initialize();
        }
        return m_aspectCategory;
    }

    // Returns the computed physical aspect ratio (width/height).
    // Returns 16/9 (= 1.777...) if detection fails.
    double GetAspectRatio() {
        if (!m_initialized) {
            Initialize();
        }
        return m_aspectRatio;
    }

    // Returns physical resolution width (0 if unknown).
    int GetPhysicalWidth() {
        if (!m_initialized) Initialize();
        return m_physW;
    }

    // Returns physical resolution height (0 if unknown).
    int GetPhysicalHeight() {
        if (!m_initialized) Initialize();
        return m_physH;
    }

    // Force re-detection (call after resolution change).
    void Reset() {
        m_initialized = false;
        m_aspectCategory = ASPECT_UNKNOWN;
        m_aspectRatio = 16.0 / 9.0;
        m_physW = 0;
        m_physH = 0;
    }

private:
    ScreenAspect()
        : m_aspectCategory(ASPECT_UNKNOWN)
        , m_aspectRatio(16.0 / 9.0)
        , m_physW(0)
        , m_physH(0)
        , m_initialized(false)
    {}

    void Initialize() {
        m_initialized = true;

        int w = 0, h = 0;

        // Priority 1: physical monitor resolution via Windows API.
        // GetGameMonitorResolution() returns the native resolution of the
        // monitor the game window is actually displayed on.  This is the
        // ONLY reliable source for distinguishing 21:9 from 16:9 when the
        // game renders at the same resolution (e.g. 1920x1080 on both).
        if (GetGameMonitorResolution(w, h) && w > 0 && h > 0) {
            m_physW = w;
            m_physH = h;
            m_aspectRatio = static_cast<double>(w) / static_cast<double>(h);
            m_aspectCategory = ClassifyAspect(m_aspectRatio);

            char buf[256]{};
            sprintf_s(buf, sizeof(buf),
                "[ScreenAspect] Game monitor: %dx%d (aspect=%.4f, category=%s)",
                w, h, m_aspectRatio, m_aspectCategory.c_str());
            OutputDebugStringA(buf);
            return;
        }

        // Priority 2: INI render resolution (fallback when game window not found).
        // WARNING: this is the game's *render* resolution, NOT the physical
        // monitor resolution.  A 21:9 monitor rendering at 1920x1080 will be
        // misclassified as 16:9 — but this path only fires when the game
        // window cannot be located, so it is an acceptable best-effort.
        if (DetectPhysicalResolution(w, h) && w > 0 && h > 0) {
            m_physW = w;
            m_physH = h;
            m_aspectRatio = static_cast<double>(w) / static_cast<double>(h);
            m_aspectCategory = ClassifyAspect(m_aspectRatio);

            char buf[256]{};
            sprintf_s(buf, sizeof(buf),
                "[ScreenAspect] INI render res: %dx%d (aspect=%.4f, category=%s)",
                w, h, m_aspectRatio, m_aspectCategory.c_str());
            OutputDebugStringA(buf);
            return;
        }

        // Priority 3: desktop window rect (last resort, unreliable on multi-monitor).
        RECT desktopRect{};
        HWND desktop = GetDesktopWindow();
        if (desktop && GetWindowRect(desktop, &desktopRect)) {
            w = desktopRect.right - desktopRect.left;
            h = desktopRect.bottom - desktopRect.top;
            if (w > 0 && h > 0) {
                m_physW = w;
                m_physH = h;
                m_aspectRatio = static_cast<double>(w) / static_cast<double>(h);
                m_aspectCategory = ClassifyAspect(m_aspectRatio);

                char buf[256]{};
                sprintf_s(buf, sizeof(buf),
                    "[ScreenAspect] Fallback desktop: %dx%d (aspect=%.4f, category=%s)",
                    w, h, m_aspectRatio, m_aspectCategory.c_str());
                OutputDebugStringA(buf);
                return;
            }
        }

        // Ultimate fallback: 16:9
        m_aspectCategory = ASPECT_16_9;
        m_aspectRatio = 16.0 / 9.0;
        OutputDebugStringA("[ScreenAspect] All detection failed, using 16:9 fallback");
    }

    std::string m_aspectCategory;
    double m_aspectRatio;
    int m_physW;
    int m_physH;
    bool m_initialized;
};

// Convenience inline helpers that use the singleton.
inline const std::string& GetAspectCategory() {
    return ScreenAspect::GetInstance().GetAspectCategory();
}
inline double GetAspectRatio() {
    return ScreenAspect::GetInstance().GetAspectRatio();
}
inline int GetPhysicalWidth() {
    return ScreenAspect::GetInstance().GetPhysicalWidth();
}
inline int GetPhysicalHeight() {
    return ScreenAspect::GetInstance().GetPhysicalHeight();
}
inline void ResetAspectDetection() {
    ScreenAspect::GetInstance().Reset();
}

} // namespace screen_aspect