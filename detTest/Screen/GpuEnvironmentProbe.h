// GpuEnvironmentProbe.h
// ============================================================================
// Lightweight environment probe - used for lossless path routing in UnifiedScreenCapture.
//
// Design constraints:
//   - No DXGI / D3D / WGC / GDI capture-related APIs may be called during detection.
//   - Only read-only means allowed: WMI / SetupAPI / registry / EnumDisplayDevices /
//     EnumDisplaySettings / GetDpiForMonitor (Shcore).
//   - Detect() / NeedsEnhancedCompat() are both noexcept static functions, no exceptions.
//   - No global state is introduced; all information is returned locally via EnvProbeResult.
//
// High-risk scenarios detected (Honor of Kings: World):
//   1. Intel Arc iGPU (Device ID: 7D55/7D45/7D67/ADL-P series) + primary display directly attached
//      -> DDA silently fails on Meteor Lake when the internal panel is directly attached.
//   2. NVIDIA Optimus + MUX Switch hot-switch capability
//      -> After a hot switch the WGC/DDA session hangs: no error code but frames stall.
//   3. AMD APU + dGPU hybrid configuration
//      -> Cross-adapter texture access crashes.
//   4. HDR / adaptive refresh rate / high DPI scaling enabled
//      -> Captured black frames or color anomalies mislead the recognition pipeline.
//   5. Intel Arc driver version < 31.0.101.5972
//      -> Early Arc drivers have DDA compatibility defects.
// ============================================================================
#pragma once

#include <Windows.h>
#include <string>
#include <vector>

namespace compat {

// Single display adapter record (from SetupAPI, contains no DXGI objects)
struct AdapterRecord {
    std::wstring description;     // DriverDesc, e.g. "Intel(R) Arc(TM) Graphics"
    UINT vendorId = 0;            // 0x8086=Intel, 0x10DE=NVIDIA, 0x1002=AMD
    UINT deviceId = 0;            // PCI Device ID
    std::wstring driverVersion;   // e.g. "31.0.101.5972"
    bool isIntegrated = false;    // Integrated GPU (Intel iGPU / AMD APU) heuristic
    bool isDiscrete = false;      // Discrete GPU
    bool isSoftware = false;      // Microsoft Basic Render Driver
    std::wstring displayDeviceName; // Associated \\.\DISPLAYN; non-empty if directly driving the primary display
};

// Primary display mode info (from EnumDisplaySettings + registry)
struct DisplayModeInfo {
    int width = 0;
    int height = 0;
    int refreshRate = 0;          // Hz
    int bitsPerPixel = 0;
    bool hdrEnabled = false;
    double dpiScale = 1.0;
};

// Probe output struct (fields required by the deliverable)
struct EnvProbeResult {
    std::vector<AdapterRecord> adapters;
    DisplayModeInfo primaryDisplay;

    // ---- Decision fields required by the task ----
    bool isIntelArcIGPU = false;          // Intel Arc iGPU + primary display directly attached
    bool isHybridWithMux = false;         // NVIDIA Optimus + MUX Switch
    bool isAmdApuHybrid = false;          // AMD APU + dGPU hybrid
    bool hdrEnabled = false;              // HDR enabled
    bool adaptiveRefreshEnabled = false;  // Adaptive refresh rate / VRR
    bool highDpiScaling = false;          // High DPI scaling (>1.0)
    bool igpuDriverSafe = false;          // Intel Arc driver >= 31.0.101.5972

    // ---- Diagnostic helper fields (logging only, not used for path routing) ----
    bool primaryScreenDirectOnIGPU = false; // Primary display directly attached to the iGPU
    std::wstring primaryAdapterDesc;        // Description of the adapter driving the primary display
    std::wstring summary;                   // Human-readable summary for log output
};

// ============================================================================
// Compat mode configuration (zero-config-file implementation)
//   compat_mode is switched via the environment variable BT_CAPTURE_COMPAT_MODE:
//     "auto"           - default; NeedsEnhancedCompat() routes automatically
//     "force_enhanced" - force the InitializeEnhanced() path
//     "force_legacy"   - force the InitializeLegacy() path (fully equivalent to the pre-change behavior)
//   If the environment variable is unset or its value is unrecognized, Auto is the default.
//   The UI static-region mask is not exposed as an external config item; it is hardcoded inside InitializeEnhanced().
// ============================================================================
enum class CompatMode {
    Auto,            // default
    ForceEnhanced,   // force enhanced path
    ForceLegacy      // force legacy path
};

class GpuEnvironmentProbe {
public:
    // Purely read-only environment detection. No exceptions; internally swallows all
    // recoverable errors and returns a best-effort result.
    static EnvProbeResult Detect() noexcept;

    // Determines whether the enhanced compat path is needed. Returns true if any
    // high-risk scenario is hit.
    static bool NeedsEnhancedCompat(const EnvProbeResult& env) noexcept;

    // Reads compat_mode (environment variable BT_CAPTURE_COMPAT_MODE, default Auto).
    // No exceptions; returns Auto if the variable is missing or unrecognized.
    static CompatMode GetCompatMode() noexcept;

private:
    // ---- SetupAPI adapter enumeration ----
    static void EnumerateAdaptersViaSetupAPI(EnvProbeResult& out) noexcept;

    // ---- EnumDisplayDevices: locate the primary display adapter + display mode ----
    static void DetectPrimaryDisplay(EnvProbeResult& out) noexcept;

    // ---- Registry read of HDR / VRR state ----
    static void DetectDisplayFeatures(EnvProbeResult& out) noexcept;

    // ---- Intel Arc iGPU detection ----
    static bool IsIntelArcDeviceId(UINT deviceId) noexcept;
    static bool IsIntelArcDescription(const std::wstring& desc) noexcept;

    // ---- Driver version comparison: a >= b (four dot-separated parts) ----
    static bool DriverVersionAtLeast(const std::wstring& version,
                                     const std::wstring& minimum) noexcept;
};

} // namespace compat
