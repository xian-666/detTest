#pragma once
#include <Windows.h>
#include <shellscalingapi.h>
#include <comdef.h>
#include <vector>
#pragma comment(lib, "Shcore.lib")

// 兼容旧 Windows 版本
typedef HRESULT(WINAPI* LPGETDPIFORMONITOR)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);

inline double getSystemDpiScale(int monitorIndex) {
    static bool s_initialized = false;
    static double s_cachedScale = 1.0;

    if (s_initialized) {
        return s_cachedScale;
    }

    double scale = 1.0;

    // 尝试使用现代 API（Windows 8.1+）
    HMODULE shcore = LoadLibraryW(L"Shcore.dll");
    if (shcore) {
        LPGETDPIFORMONITOR getDpiForMonitor = (LPGETDPIFORMONITOR)GetProcAddress(shcore, "GetDpiForMonitor");
        if (getDpiForMonitor) {
            // 获取主显示器（或第 monitorIndex 个）
            int screenCount = GetSystemMetrics(SM_CMONITORS);
            HMONITOR hMon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
            if (monitorIndex > 0 && monitorIndex < screenCount) {
                hMon = MonitorFromPoint({ GetSystemMetrics(SM_CXSCREEN) * monitorIndex / 2, 0 }, MONITOR_DEFAULTTOPRIMARY);
            }

            UINT dpiX = 96, dpiY = 96;
            if (SUCCEEDED(getDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
                scale = dpiX / 96.0;
            }
        }
        FreeLibrary(shcore);
    }

    // 回退到老式方法（不精确，但兼容 Win7）
    if (scale == 1.0) {
        HDC screenDC = GetDC(nullptr);
        int dpi = GetDeviceCaps(screenDC, LOGPIXELSX);
        ReleaseDC(nullptr, screenDC);
        scale = dpi / 96.0;
    }

    s_cachedScale = scale;
    s_initialized = true;
    return scale;
}

// Return per-monitor DPI scale (dpi/96.0) for given HMONITOR. Falls back to system DPI on failure.
inline double getDpiScaleForMonitor(HMONITOR hMon)
{
    if (!hMon) {
        HDC screenDC = GetDC(nullptr);
        int dpi = GetDeviceCaps(screenDC, LOGPIXELSX);
        ReleaseDC(nullptr, screenDC);
        return dpi / 96.0;
    }

    HMODULE shcore = LoadLibraryW(L"Shcore.dll");
    if (shcore) {
        LPGETDPIFORMONITOR getDpiForMonitor = (LPGETDPIFORMONITOR)GetProcAddress(shcore, "GetDpiForMonitor");
        if (getDpiForMonitor) {
            UINT dpiX = 96, dpiY = 96;
            if (SUCCEEDED(getDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
                FreeLibrary(shcore);
                return dpiX / 96.0;
            }
        }
        FreeLibrary(shcore);
    }

    HDC screenDC = GetDC(nullptr);
    int dpi = GetDeviceCaps(screenDC, LOGPIXELSX);
    ReleaseDC(nullptr, screenDC);
    return dpi / 96.0;
}

// Monitor enumeration helpers (inline so they can be included in multiple TUs)
inline BOOL CALLBACK EnumerateMonitorsCallback_Local(HMONITOR hMonitor, HDC /*hDC*/, LPRECT /*lprcMonitor*/, LPARAM dwData)
{
    auto vec = reinterpret_cast<std::vector<HMONITOR>*>(dwData);
    vec->push_back(hMonitor);
    return TRUE;
}

inline std::vector<HMONITOR> EnumerateMonitors()
{
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(NULL, NULL, EnumerateMonitorsCallback_Local, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

inline HMONITOR GetHMonitorFromIndex(int index)
{
    std::vector<HMONITOR> monitors = EnumerateMonitors();
    if (!monitors.empty() && index >= 0 && static_cast<size_t>(index) < monitors.size()) {
        return monitors[index];
    }
    return MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
}