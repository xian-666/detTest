#pragma once
// ============================================================================
// ROIAdaptationConfig - Externalized ROI adaptation data (v2.0)
//
// Loads roi_adaptation.json once at startup and caches it in memory.
// The two-stage C++ transform (16:9 base + min() scale + centering, then
// per-resolution residual) is the only authoritative compute path; the JSON
// provides per-resolution residual corrections to preserve byte-exact output
// for all previously-registered resolutions.
//
// v2.0: aspect profiles can carry a "canonical anchor" + canonical residuals
// + a scale-law. When a (W, H) request misses the specificResolutions table,
// the config will try the matching aspect profile and apply the scale law
// to derive the residual for (W, H) from the canonical anchor.
//
// Lookup order in GetResidualWithFallback:
//   1. specificResolutions entry matching both (W, H) AND aspectCategory
//      (when provided). If aspectCategory does not match any entry for (W,H),
//      falls through to the first (W,H) entry regardless of aspectGroup —
//      this ensures resolutions that only have one aspect group (e.g. 1920x1200
//      only has "16:10") are still served correctly even when the physical
//      screen is a different aspect.
//   2. matching aspect profile (aspectCategory or targetAspect within tolerance)
//      - if profile has canonicalResiduals + scaleLaw: derive and apply
//   3. zero residual (GetAdaptiveRect will then try canonicalTargets)
//
// v2.3 — HYBRID MODE (2026-06-23):
//   When a specificResolutions entry exists for (W, H) BUT its aspectGroup
//   does NOT match the physical screen's aspectCategory (e.g. 1920x1200
//   registered as "16:10" but running on a 21:9 monitor), the system enters
//   "hybrid mode":
//     • The residual (geometry offsets dx/dy/dw/dh) from the specific entry
//       IS preserved — ROI coordinates remain correct.
//     • The legacy metadata (xBias, yBias, sizeScale) from that entry are
//       DISCARDED for image-processing purposes.
//     • Instead, the matching aspect profile for the physical screen provides
//       replacement image-processing parameters.
//   This ensures ROI position stays accurate while preventing the
//   wrong aspect-group's image enhancements from causing misclassification.
//
// All files in this module MUST remain UTF-8 without BOM.
// ============================================================================

#include <string>
#include <vector>
#include <unordered_map>
#include <array>

namespace roi {

// Per-icon residual correction entry (integer pixel offsets).
struct ResidualEntry {
    int dx = 0;  // added to x
    int dy = 0;  // added to y
    int dw = 0;  // added to w
    int dh = 0;  // added to h
};

// v2.3: Image-processing parameters per aspect profile.
// These control image enhancement (contrast, brightness, etc.)
// applied to ROIs before classification inference.
struct ImageProcessingParams {
    // Currently, the system uses sizeScale/xBias/yBias for ROI normalization.
    // Future: add contrast, brightness, CLAHE clip-limit, etc.
    double sizeScale = 1.0;
    int xBias = 0;
    int yBias = 0;
};

class ROIAdaptationConfig {
public:
    static ROIAdaptationConfig& GetInstance();

    // Load config from JSON file. Returns true on success.
    // Safe to call multiple times; subsequent calls are no-ops if already loaded.
    bool Load(const std::string& jsonPath = "roi_adaptation.json");

    // True if the JSON was successfully parsed and cached.
    bool IsLoaded() const { return m_loaded; }

    // True if Load() failed and we are now serving hardcoded 16:9 fallback data.
    bool IsFallbackActive() const { return m_fallbackActive; }

        // Look up residual correction for a specific (resolution, icon).
    // Returns true and fills outResidual if found; returns false otherwise.
    // When multiple entries exist for (W, H) across aspect groups, the
    // aspectCategory parameter disambiguates. If no aspectCategory match is
    // found, falls back to the first (W, H) entry (ensures single-group
    // resolutions like 1920x1200 still work when physical screen aspect differs).
    bool GetResidual(int W, int H,
                     const std::string& iconName,
                     ResidualEntry& outResidual,
                     const std::string& aspectCategory = "") const;

    // Look up residual with the full fallback chain:
    //   1) exact (W, H, aspectCategory) entry — falls back to first (W,H) if no match
    //   2) aspect-profile scale-law derivation / canonicalTargets
    //   3) zero
    // The aspectCategory disambiguates when (W, H) has entries for multiple
    // aspect groups (e.g. 1920x1080 for both "16:9" and "21:9").
    void GetResidualWithFallback(int W, int H,
                                 const std::string& iconName,
                                 ResidualEntry& outResidual,
                                 const std::string& aspectCategory = "") const;

    // v2.3 — HYBRID MODE: Like GetResidualWithFallback, but when a specific
    // resolution entry exists for (W, H) with a DIFFERENT aspectGroup than
    // aspectCategory, the residual geometry is preserved AND the caller can
    // optionally obtain the correct ImageProcessingParams from the matching
    // aspect profile.
    //
    // outHybridParams is filled only when hybrid mode activates (returns
    // geometry from specific entry + params from aspect profile).  If no
    // specific entry exists at all, outHybridParams is zeroed and the
    // function behaves identically to GetResidualWithFallback.
    //
    // Returns true if a residual was found (same as GetResidualWithFallback).
    void GetHybridResidual(int W, int H,
                           const std::string& iconName,
                           ResidualEntry& outResidual,
                           const std::string& aspectCategory,
                           ImageProcessingParams& outHybridParams) const;

    // Look up legacy (xBias, yBias, sizeScale) as ImageProcessingParams.
    // When aspectCategory is provided and the specific entry's aspectGroup
    // does NOT match, this falls back to the matching aspect profile's params.
    bool GetHybridProcessingParams(int W, int H,
                                   ImageProcessingParams& outParams,
                                   const std::string& aspectCategory) const;

    // v2.3: Get image-processing params from an aspect profile by category.
    // Returns the profile's baseTransform (xBias, yBias, sizeScale).
    bool GetAspectProfileProcessingParams(const std::string& aspectCategory,
                                          ImageProcessingParams& outParams) const;

    // Look up legacy (xBias, yBias, sizeScale) for a specific resolution.
    // aspectCategory disambiguates when (W, H) has entries for multiple groups.
    // Falls back to first (W,H) entry if aspectCategory has no match.
    bool GetResolutionMeta(int W, int H,
                           int& outXbias,
                           int& outYbias,
                           double& outSizeScale,
                           const std::string& aspectCategory = "") const;

    // Find the closest aspect profile by target aspect ratio.
    // Fills outGroup (e.g. "16:9"), outSizeScale (1.0 if no match), outXbias,
    // outYbias. Returns true if a profile was found.
    bool GetAspectProfile(double aspect,
                          std::string& outGroup,
                          double& outSizeScale,
                          int& outXbias,
                          int& outYbias) const;

    // Get reference resolution (always 1920x1080 in v1.0/v2.0).
    int GetReferenceWidth()  const { return m_refW; }
    int GetReferenceHeight() const { return m_refH; }

    // v2.2: Look up scaled canonical target coordinates for an icon.
    // If the profile matching aspectCategory (or render W/H as fallback)
    // carries canonicalTargets, this computes:
    //   predicted_i = round(canonicalTarget_i * targetW / canonicalW)
    // Returns true and fills outX/Y/W/H if found; false otherwise.
    // The caller (GetAdaptiveRect) uses this as:
    //   residual = predicted - stage1_output
    bool GetCanonicalTarget(const std::string& aspectCategory,
                            const std::string& iconName,
                            int targetW, int targetH,
                            int& outX, int& outY,
                            int& outW, int& outH) const;

    // v2.3: Check if a specific resolution entry for (W, H) exists with
    // a DIFFERENT aspectGroup than the given aspectCategory.
    // Returns true and sets outGeometryGroup to the entry's aspectGroup
    // if such a mismatch is detected.
    bool HasAspectMismatch(int W, int H,
                           const std::string& aspectCategory,
                           std::string& outGeometryGroup) const;

    // Counters for diagnostics.
    size_t GetResolutionCount()    const { return m_resolutions.size(); }
    size_t GetAspectProfileCount() const { return m_aspectProfiles.size(); }

private:
    ROIAdaptationConfig() = default;
    ~ROIAdaptationConfig() = default;
    ROIAdaptationConfig(const ROIAdaptationConfig&) = delete;
    ROIAdaptationConfig& operator=(const ROIAdaptationConfig&) = delete;

    // ---- Lightweight JSON parsing (no external dep) ----
    bool ParseJson(const std::string& json);
    static std::string ExtractJsonValue(const std::string& json, const std::string& key);
    static std::string ExtractSection(const std::string& json, size_t start);
    static std::vector<std::pair<std::string, std::string>> SplitJsonEntries(
        const std::string& json_obj);
    static int MakeKey(int w, int h) { return (w * 100000) + h; }

    // Apply the scale-law to derive a residual for (W, H) from a canonical
    // anchor. scaleLinearFields list the fields that scale linearly with the
    // ratio W/canonical.W. scaleInvariantFields are kept constant.
    static ResidualEntry ApplyScaleLaw(
        const ResidualEntry& canonical,
        int canonicalW, int canonicalH,
        int targetW, int targetH,
        const std::vector<std::string>& scaleLinearFields,
        const std::vector<std::string>& scaleInvariantFields);

    static int ScaleField(int canonicalValue, int canonicalW, int targetW) {
        if (canonicalW == 0) return canonicalValue;
        // Round to nearest, matching the expected behavior of dy/dw/dh scaling
        double ratio = static_cast<double>(targetW) / static_cast<double>(canonicalW);
        double scaled = static_cast<double>(canonicalValue) * ratio;
        return static_cast<int>(scaled + (scaled >= 0 ? 0.5 : -0.5));
    }

    // ---- Data structures ----
    struct ResolutionEntry {
        int w = 0;
        int h = 0;
        std::string aspectGroup;
        int xBias = 0;
        int yBias = 0;
        double sizeScale = 1.0;
        std::unordered_map<std::string, ResidualEntry> residuals;
    };

    struct AspectProfileEntry {
        std::string aspectGroup;
        double targetAspect = 0.0;
        double tolerance = 0.02;
        int xBias = 0;
        int yBias = 0;
        double sizeScale = 1.0;

        // v2.0: optional canonical anchor + residuals + scale law.
        // When present, any (W, H) matching this profile that misses the
        // specificResolutions table can derive its residual from canonicalResiduals.
        bool hasCanonical = false;
        int canonicalW = 0;
        int canonicalH = 0;
        std::unordered_map<std::string, ResidualEntry> canonicalResiduals;
        std::vector<std::string> scaleLinearFields;
        std::vector<std::string> scaleInvariantFields;

        // v2.2: optional canonical TARGETS (absolute coordinates [x,y,w,h]).
        // When present, the caller computes:
        //   predicted = round(target_i * W / canonicalW)
        //   residual  = predicted - stage1_output
        bool hasCanonicalTargets = false;
        std::unordered_map<std::string, std::array<int, 4>> canonicalTargets;
    };

    // ---- State ----
    int m_refW = 1920;
    int m_refH = 1080;
    std::vector<ResolutionEntry> m_resolutions;
    std::unordered_multimap<int, size_t> m_resolutionIndex;  // key -> index (supports multiple aspect groups per (W,H))
    std::vector<AspectProfileEntry> m_aspectProfiles;
    bool m_loaded = false;
    bool m_fallbackActive = false;
    std::string m_loadedPath;
};

} // namespace roi