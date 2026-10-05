// GpuEnvironmentProbe.cpp
// ============================================================================
// Purely read-only environment probe implementation. Uses only SetupAPI / registry /
// EnumDisplayDevices / EnumDisplaySettings / GetDpiForMonitor. DXGI/D3D/WGC/GDI
// capture APIs are strictly forbidden.
// ============================================================================
#include "GpuEnvironmentProbe.h"
#include "../common/DebugFileLog.h"

#include <SetupAPI.h>
#include <devguid.h>          // GUID_DEVCLASS_DISPLAY
#include <shellscalingapi.h>  // GetDpiForMonitor (Shcore)
#include <regex>
#include <sstream>
#include <algorithm>
#include <cwchar>

#pragma comment(lib, "Setupapi.lib")
#pragma comment(lib, "Shcore.lib")

namespace compat {

namespace {

// ----------------------------------------------------------------------------
// Safe registry string read (REG_SZ / REG_EXPAND_SZ)
// ----------------------------------------------------------------------------
bool RegReadString(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName,
                   std::wstring& out) noexcept {
    out.clear();
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(hRoot, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[512] = { 0 };
    DWORD bufSize = sizeof(buf);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(hKey, valueName, nullptr, &type,
                               reinterpret_cast<LPBYTE>(buf), &bufSize);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    // bufSize includes the trailing '\0' in bytes
    size_t chars = bufSize / sizeof(wchar_t);
    if (chars > 0 && buf[chars - 1] == L'\0') chars--;
    out.assign(buf, chars);
    return true;
}

bool RegReadDword(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName,
                  DWORD& out) noexcept {
    out = 0;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(hRoot, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD val = 0;
    DWORD sz = sizeof(val);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(hKey, valueName, nullptr, &type,
                               reinterpret_cast<LPBYTE>(&val), &sz);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) return false;
    out = val;
    return true;
}

// ----------------------------------------------------------------------------
// Parse VEN_xxxx & DEV_yyyy from a hardware ID string
//   e.g.: PCI\VEN_8086&DEV_7D55&SUBSYS_...
// ----------------------------------------------------------------------------
bool ParsePciVenDev(const std::wstring& hwid, UINT& ven, UINT& dev) noexcept {
    ven = 0; dev = 0;
    std::wregex re(L"VEN_([0-9A-Fa-f]{4})&DEV_([0-9A-Fa-f]{4})",
                   std::regex_constants::icase);
    std::wsmatch m;
    if (!std::regex_search(hwid, m, re) || m.size() < 3) return false;
    try {
        ven = static_cast<UINT>(std::stoul(m.str(1), nullptr, 16));
        dev = static_cast<UINT>(std::stoul(m.str(2), nullptr, 16));
        return true;
    } catch (...) {
        return false;
    }
}

// ----------------------------------------------------------------------------
// Dot-separated version comparison: returns true when version >= minimum
//   Input like "31.0.101.5972"; missing parts are padded with 0.
// ----------------------------------------------------------------------------
std::vector<unsigned long> SplitVersion(const std::wstring& v) noexcept {
    std::vector<unsigned long> parts;
    std::wistringstream iss(v);
    std::wstring tok;
    while (std::getline(iss, tok, L'.')) {
        try { parts.push_back(std::stoul(tok)); }
        catch (...) { parts.push_back(0); }
    }
    while (parts.size() < 4) parts.push_back(0);
    return parts;
}

} // namespace

// ============================================================================
// Public interface
// ============================================================================

EnvProbeResult GpuEnvironmentProbe::Detect() noexcept {
    EnvProbeResult env;

    // 1. Enumerate all display adapters via SetupAPI (with VEN/DEV + driver version)
    EnumerateAdaptersViaSetupAPI(env);

    // 2. EnumDisplayDevices: primary display adapter + current display mode
    DetectPrimaryDisplay(env);

    // 3. Read HDR / VRR / DPI from the registry
    DetectDisplayFeatures(env);

    // ---- Derived decisions ----
    bool hasIntelArc = false;
    bool hasIntelArcPrimary = false;
    bool hasNvidia = false;
    bool hasAmd = false;
    bool hasAmdApu = false;
    bool hasAmdDgpu = false;
    bool hasIntelIGpu = false;
    bool hasAnyDiscrete = false;

    for (const auto& a : env.adapters) {
        if (a.isSoftware) continue;
        if (a.vendorId == 0x8086) {
            hasIntelIGpu = true;
            if (IsIntelArcDeviceId(a.deviceId) || IsIntelArcDescription(a.description)) {
                hasIntelArc = true;
                if (a.displayDeviceName == L"\\\\.\\DISPLAY1") {
                    hasIntelArcPrimary = true;
                }
            }
        } else if (a.vendorId == 0x10DE) {
            hasNvidia = true;
            hasAnyDiscrete = a.isDiscrete || hasAnyDiscrete;
        } else if (a.vendorId == 0x1002) {
            hasAmd = true;
            // AMD APU: integrated heuristic (driver description contains Radeon Graphics / APU shared memory)
            if (a.isIntegrated) hasAmdApu = true;
            if (a.isDiscrete) hasAmdDgpu = true;
            if (a.isDiscrete) hasAnyDiscrete = true;
        }
        if (a.isDiscrete) hasAnyDiscrete = true;
    }

    // 1) Intel Arc iGPU + primary display directly attached
    env.primaryScreenDirectOnIGPU = hasIntelArcPrimary;
    env.isIntelArcIGPU = hasIntelArc && hasIntelArcPrimary;

    // 2) NVIDIA Optimus + MUX Switch: NVIDIA dGPU present + another (integrated) adapter present
    //    MUX hot-switch heuristic: a laptop hybrid setup (NVIDIA + iGPU) is treated as having hot-switch risk.
    env.isHybridWithMux = hasNvidia && (hasIntelIGpu || hasAmdApu);

    // 3) AMD APU + dGPU hybrid
    env.isAmdApuHybrid = hasAmdApu && (hasAmdDgpu || hasNvidia);

    // 4) HDR / adaptive refresh / high DPI (from DetectDisplayFeatures)
    env.hdrEnabled = env.primaryDisplay.hdrEnabled;
    env.adaptiveRefreshEnabled = env.primaryDisplay.refreshRate != 0 &&
                                 env.primaryDisplay.refreshRate != 60 &&
                                 env.primaryDisplay.refreshRate != 30;
    env.highDpiScaling = env.primaryDisplay.dpiScale > 1.0 + 1e-3;

    // 5) Intel Arc driver version >= 31.0.101.5972 is considered safe
    env.igpuDriverSafe = true; // Safe by default (treated as true without Arc, does not trigger enhanced)
    if (hasIntelArc) {
        const std::wstring kMinArcDriver = L"31.0.101.5972";
        for (const auto& a : env.adapters) {
            if (a.vendorId != 0x8086) continue;
            if (!(IsIntelArcDeviceId(a.deviceId) || IsIntelArcDescription(a.description))) {
                continue;
            }
            if (!DriverVersionAtLeast(a.driverVersion, kMinArcDriver)) {
                env.igpuDriverSafe = false;
                break;
            }
        }
    }

    // ---- Summary (for logging) ----
    std::wostringstream oss;
    oss << L"adapters=" << env.adapters.size()
        << L" intelArc=" << env.isIntelArcIGPU
        << L" mux=" << env.isHybridWithMux
        << L" amdHybrid=" << env.isAmdApuHybrid
        << L" hdr=" << env.hdrEnabled
        << L" vrr=" << env.adaptiveRefreshEnabled
        << L" dpi=" << env.highDpiScaling
        << L" arcDriverSafe=" << env.igpuDriverSafe
        << L" primary=" << env.primaryDisplay.width << L"x" << env.primaryDisplay.height
        << L"@" << env.primaryDisplay.refreshRate << L"Hz";
    env.summary = oss.str();

    return env;
}

bool GpuEnvironmentProbe::NeedsEnhancedCompat(const EnvProbeResult& env) noexcept {
    // Any high-risk scenario hit requires the enhanced path.
    if (env.isIntelArcIGPU) return true;          // DDA silent failure
    if (!env.igpuDriverSafe) return true;         // Early Intel Arc driver
    if (env.isHybridWithMux) return true;         // Optimus hot-switch hang
    if (env.isAmdApuHybrid) return true;          // Cross-adapter crash
    if (env.hdrEnabled) return true;              // HDR black frames / color anomalies
    if (env.adaptiveRefreshEnabled && env.highDpiScaling) return true; // Compound display risk
    return false;
}

// ============================================================================
// SetupAPI adapter enumeration
// ============================================================================
void GpuEnvironmentProbe::EnumerateAdaptersViaSetupAPI(EnvProbeResult& out) noexcept {
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
                                             DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA devData;
    devData.cbSize = sizeof(devData);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devData); ++i) {
        AdapterRecord rec;

        // Hardware ID -> VEN/DEV
        wchar_t hwidBuf[512] = { 0 };
        if (SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devData, SPDRP_HARDWAREID,
                nullptr, reinterpret_cast<PBYTE>(hwidBuf), sizeof(hwidBuf), nullptr)) {
            UINT ven = 0, dev = 0;
            if (ParsePciVenDev(hwidBuf, ven, dev)) {
                rec.vendorId = ven;
                rec.deviceId = dev;
            }
        }

        // Driver description
        wchar_t descBuf[256] = { 0 };
        if (SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devData, SPDRP_DEVICEDESC,
                nullptr, reinterpret_cast<PBYTE>(descBuf), sizeof(descBuf), nullptr)) {
            rec.description = descBuf;
        }

        // Driver key -> read DriverVersion from the registry
        wchar_t drvKeyBuf[256] = { 0 };
        if (SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devData, SPDRP_DRIVER,
                nullptr, reinterpret_cast<PBYTE>(drvKeyBuf), sizeof(drvKeyBuf), nullptr)) {
            std::wstring classPath = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
            classPath += drvKeyBuf;
            std::wstring drvVer;
            if (RegReadString(HKEY_LOCAL_MACHINE, classPath.c_str(), L"DriverVersion", drvVer)) {
                rec.driverVersion = drvVer;
            }
            // Fall back to DriverDesc when the description is empty
            if (rec.description.empty()) {
                std::wstring drvDesc;
                if (RegReadString(HKEY_LOCAL_MACHINE, classPath.c_str(), L"DriverDesc", drvDesc)) {
                    rec.description = drvDesc;
                }
            }
        }

        // Software rendering adapter (Microsoft Basic Render Driver)
        std::wstring descLower = rec.description;
        std::transform(descLower.begin(), descLower.end(), descLower.begin(), ::towlower);
        if (descLower.find(L"microsoft") != std::wstring::npos &&
            (descLower.find(L"basic") != std::wstring::npos ||
             descLower.find(L"render") != std::wstring::npos)) {
            rec.isSoftware = true;
        }

        // Integrated / discrete GPU heuristic
        if (!rec.isSoftware) {
            if (rec.vendorId == 0x8086) {
                rec.isIntegrated = true; // Intel iGPU defaults to integrated
            } else if (rec.vendorId == 0x1002) {
                // AMD APU integrated descriptions usually contain "Radeon Graphics" / "APU"
                if (descLower.find(L"radeon graphics") != std::wstring::npos ||
                    descLower.find(L"apu") != std::wstring::npos) {
                    rec.isIntegrated = true;
                } else {
                    rec.isDiscrete = true;
                }
            } else if (rec.vendorId == 0x10DE) {
                rec.isDiscrete = true; // NVIDIA defaults to discrete
            } else {
                rec.isDiscrete = true;
            }
        }

        out.adapters.push_back(std::move(rec));
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
}

// ============================================================================
// EnumDisplayDevices: primary display adapter + display mode
// ============================================================================
void GpuEnvironmentProbe::DetectPrimaryDisplay(EnvProbeResult& out) noexcept {
    // DISPLAY_DEVICE.DeviceName == "\\\\.\\DISPLAY1" is the primary display (EnumDisplayDevices index 0)
    DISPLAY_DEVICEW dd;
    dd.cb = sizeof(dd);
    if (!EnumDisplayDevicesW(nullptr, 0, &dd, 0)) return;

    const std::wstring primaryName = dd.DeviceName; // \\\\.\\DISPLAY1
    std::wstring adapterStr = dd.DeviceString;      // Adapter name (Intel/NVIDIA/AMD ...)

    // Associate the primary display with a SetupAPI adapter record
    for (auto& a : out.adapters) {
        if (a.isSoftware) continue;
        // Fuzzy match adapter description against DeviceString (description is usually longer; use containment)
        if (!a.description.empty() &&
            a.description.find(adapterStr) != std::wstring::npos) {
            a.displayDeviceName = primaryName;
            out.primaryAdapterDesc = a.description;
            break;
        }
    }
    if (out.primaryAdapterDesc.empty()) {
        out.primaryAdapterDesc = adapterStr;
    }

    // Current display mode
    DEVMODEW dm;
    dm.dmSize = sizeof(dm);
    dm.dmDriverExtra = 0;
    if (EnumDisplaySettingsW(primaryName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
        out.primaryDisplay.width = static_cast<int>(dm.dmPelsWidth);
        out.primaryDisplay.height = static_cast<int>(dm.dmPelsHeight);
        out.primaryDisplay.refreshRate = static_cast<int>(dm.dmDisplayFrequency);
        out.primaryDisplay.bitsPerPixel = static_cast<int>(dm.dmBitsPerPel);
    }

    // Primary display DPI (Shcore, not GDI)
    POINT pt{ 0, 0 };
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    if (hMon) {
        HMODULE shcore = LoadLibraryW(L"Shcore.dll");
        if (shcore) {
            typedef HRESULT(WINAPI* PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
            auto fn = reinterpret_cast<PFN_GetDpiForMonitor>(
                GetProcAddress(shcore, "GetDpiForMonitor"));
            if (fn) {
                UINT dpiX = 96, dpiY = 96;
                if (SUCCEEDED(fn(hMon, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY))) {
                    out.primaryDisplay.dpiScale = dpiX / 96.0;
                }
            }
            FreeLibrary(shcore);
        }
    }
}

// ============================================================================
// Registry: HDR / VRR state
// ============================================================================
void GpuEnvironmentProbe::DetectDisplayFeatures(EnvProbeResult& out) noexcept {
    // HDR: HKCU\Software\Microsoft\Windows\CurrentVersion\VideoSettings\HDR
    //      (1 = enabled; key names vary across Windows versions, multiple key names as fallback)
    DWORD hdrVal = 0;
    if (RegReadDword(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\VideoSettings",
            L"HDR", hdrVal)) {
        out.primaryDisplay.hdrEnabled = (hdrVal != 0);
    }
    if (!out.primaryDisplay.hdrEnabled) {
        if (RegReadDword(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\VideoSettings",
                L"AdvancedColorEnabled", hdrVal)) {
            out.primaryDisplay.hdrEnabled = (hdrVal != 0);
        }
    }
}

// ============================================================================
// Intel Arc detection
// ============================================================================
bool GpuEnvironmentProbe::IsIntelArcDeviceId(UINT deviceId) noexcept {
    // Known Device IDs for the Meteor Lake / Arc A series
    // 7D55 / 7D45 / 7D67 (Meteor Lake iGPU)
    // ADL-P (Alder Lake-P) iGPUs are not Arc-branded, but some 12th-gen mobile iGPUs also use the Arc driver stack
    switch (deviceId) {
        case 0x7D55: case 0x7D45: case 0x7D67:   // Meteor Lake Arc iGPU
        case 0x7D40: case 0x7D60: case 0x7DD5:   // Meteor Lake variants
        case 0x5690: case 0x5691: case 0x5692:   // DG2 / Alchemist (Arc A380 etc.)
        case 0x56A0: case 0x56A1: case 0x56A5:
        case 0x56B0: case 0x56B1:
            return true;
        default:
            return false;
    }
}

bool GpuEnvironmentProbe::IsIntelArcDescription(const std::wstring& desc) noexcept {
    if (desc.empty()) return false;
    std::wstring lower = desc;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    // "Arc(TM)" is the Intel Arc brand marker; Meteor Lake iGPU descriptions contain "Arc"
    return lower.find(L"arc") != std::wstring::npos;
}

bool GpuEnvironmentProbe::DriverVersionAtLeast(const std::wstring& version,
                                               const std::wstring& minimum) noexcept {
    if (version.empty()) return false;
    auto a = SplitVersion(version);
    auto b = SplitVersion(minimum);
    for (size_t i = 0; i < 4; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return true; // exactly equal
}

// ============================================================================
// compat_mode reading (zero-config-file: environment variable BT_CAPTURE_COMPAT_MODE)
//   Unset / unrecognized -> Auto (default)
//   "auto"           -> Auto
//   "force_enhanced" -> ForceEnhanced
//   "force_legacy"   -> ForceLegacy
// ============================================================================
CompatMode GpuEnvironmentProbe::GetCompatMode() noexcept {
    try {
        wchar_t buf[32] = { 0 };
        DWORD n = GetEnvironmentVariableW(L"BT_CAPTURE_COMPAT_MODE", buf, 32);
        if (n == 0 || n >= 32) return CompatMode::Auto; // unset / truncated
        // Compare lowercased
        std::wstring v(buf);
        std::transform(v.begin(), v.end(), v.begin(), ::towlower);
        if (v == L"force_enhanced") return CompatMode::ForceEnhanced;
        if (v == L"force_legacy")   return CompatMode::ForceLegacy;
        return CompatMode::Auto; // includes "auto" / unknown / missing
    } catch (...) {
        return CompatMode::Auto;
    }
}

} // namespace compat
