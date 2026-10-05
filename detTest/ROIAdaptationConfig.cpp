// ============================================================================
// ROIAdaptationConfig implementation
// Lightweight JSON parsing, no external dependencies.
// All files in this module MUST remain UTF-8 without BOM.
// ============================================================================

#include "ROIAdaptationConfig.h"
#include "common/DebugFileLog.h"

#include <windows.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <array>

namespace roi {

// ============================================================================
// JSON helpers
// ============================================================================

static std::string TrimString(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    std::string result = s.substr(start, end - start + 1);
    if (result.size() >= 2 && result.front() == '"' && result.back() == '"') {
        result = result.substr(1, result.size() - 2);
    }
    return result;
}

std::string ROIAdaptationConfig::ExtractJsonValue(const std::string& json,
                                                  const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";

    pos++;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) {
        pos++;
    }

    if (pos >= json.size()) return "";

    size_t end_pos;
    if (json[pos] == '"') {
        // String value
        pos++;
        end_pos = json.find('"', pos);
        if (end_pos == std::string::npos) return "";
        return json.substr(pos, end_pos - pos);
    }
    else if (json[pos] == '{') {
        return ExtractSection(json, pos);
    }
    else if (json[pos] == '[') {
        int bracket_count = 1;
        end_pos = pos + 1;
        while (end_pos < json.size() && bracket_count > 0) {
            if (json[end_pos] == '[') bracket_count++;
            else if (json[end_pos] == ']') bracket_count--;
            end_pos++;
        }
        return json.substr(pos, end_pos - pos);
    }
    else {
        end_pos = json.find_first_of(",}]\n\r", pos);
        if (end_pos == std::string::npos) end_pos = json.size();
        return TrimString(json.substr(pos, end_pos - pos));
    }
}

std::string ROIAdaptationConfig::ExtractSection(const std::string& json, size_t start_pos) {
    if (start_pos >= json.size() || json[start_pos] != '{') return "";

    int brace_count = 1;
    size_t end_pos = start_pos + 1;
    while (end_pos < json.size() && brace_count > 0) {
        if (json[end_pos] == '{') brace_count++;
        else if (json[end_pos] == '}') brace_count--;
        end_pos++;
    }
    return json.substr(start_pos, end_pos - start_pos);
}

std::vector<std::pair<std::string, std::string>>
ROIAdaptationConfig::SplitJsonEntries(const std::string& json_obj) {
    std::vector<std::pair<std::string, std::string>> entries;
    if (json_obj.empty() || json_obj.front() != '{' || json_obj.back() != '}') {
        return entries;
    }
    std::string content = json_obj.substr(1, json_obj.size() - 2);

    size_t pos = 0;
    while (pos < content.size()) {
        size_t key_start = content.find('"', pos);
        if (key_start == std::string::npos) break;
        key_start++;
        size_t key_end = content.find('"', key_start);
        if (key_end == std::string::npos) break;
        std::string key = content.substr(key_start, key_end - key_start);

        // Skip underscore-prefixed keys (metadata)
        if (!key.empty() && key[0] == '_') {
            size_t next_key = content.find('"', key_end + 1);
            size_t comma = content.find(',', key_end);
            if (comma != std::string::npos && (next_key == std::string::npos || comma < next_key)) {
                pos = comma + 1;
            } else if (next_key != std::string::npos) {
                pos = next_key;
            } else {
                break;
            }
            continue;
        }

        size_t colon = content.find(':', key_end);
        if (colon == std::string::npos) break;

        size_t value_start = colon + 1;
        while (value_start < content.size() &&
               (content[value_start] == ' ' || content[value_start] == '\t' ||
                content[value_start] == '\n' || content[value_start] == '\r')) {
            value_start++;
        }
        if (value_start >= content.size()) break;

        std::string value;
        if (content[value_start] == '{') {
            value = ExtractSection(content, value_start);
        }
        else if (content[value_start] == '[') {
            int bracket_count = 1;
            size_t end_pos = value_start + 1;
            while (end_pos < content.size() && bracket_count > 0) {
                if (content[end_pos] == '[') bracket_count++;
                else if (content[end_pos] == ']') bracket_count--;
                end_pos++;
            }
            value = content.substr(value_start, end_pos - value_start);
        }
        else if (content[value_start] == '"') {
            value_start++;
            size_t value_end = content.find('"', value_start);
            if (value_end != std::string::npos) {
                value = content.substr(value_start, value_end - value_start);
            }
        }
        else {
            size_t value_end = content.find_first_of(",}\n\r", value_start);
            if (value_end == std::string::npos) value_end = content.size();
            value = TrimString(content.substr(value_start, value_end - value_start));
        }

        entries.push_back({key, value});

        size_t comma = content.find(',', colon);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return entries;
}

// ============================================================================
// Singleton
// ============================================================================

ROIAdaptationConfig& ROIAdaptationConfig::GetInstance() {
    static ROIAdaptationConfig instance;
    return instance;
}

// ============================================================================
// Load
// ============================================================================

bool ROIAdaptationConfig::Load(const std::string& jsonPath) {
    if (m_loaded) {
        return true;
    }
    std::string path = jsonPath;
    // If the path is a plain filename (no directory separator), resolve it
    // relative to the executable directory — same approach as common::GetModelPath(L"HONOR").
    if (path.find_first_of("/\\") == std::string::npos) {
        wchar_t exePath[MAX_PATH];
        DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring wpath(exePath);
            size_t pos = wpath.find_last_of(L"\\/");
            if (pos != std::wstring::npos) {
                wpath = wpath.substr(0, pos + 1) + std::wstring(path.begin(), path.end());
                // Convert back to narrow string (UTF-8 on modern Windows).
                int size = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (size > 0) {
                    path.resize(size - 1);
                    WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, &path[0], size, nullptr, nullptr);
                }
            }
        }
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[ROIAdaptationConfig] Failed to open " << path
                  << " - falling back to 16:9 baseline (zero bias, sizeScale=1.0)"
                  << std::endl;
        DEBUG_LOG_FMT("[ROIAdaptationConfig] Failed to open %s, using 16:9 fallback", path.c_str());
        m_fallbackActive = true;
        m_loaded = true;  // Still mark as loaded to avoid repeated attempts
        m_loadedPath = path;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    if (!ParseJson(content)) {
        std::cerr << "[ROIAdaptationConfig] Parse failed for " << path
                  << " - falling back to 16:9 baseline" << std::endl;
        DEBUG_LOG_FMT("[ROIAdaptationConfig] Parse failed for %s, using 16:9 fallback", path.c_str());
        m_fallbackActive = true;
        m_loaded = true;
        m_loadedPath = path;
        return false;
    }

    m_loaded = true;
    m_fallbackActive = false;
    m_loadedPath = path;
    std::cout << "[ROIAdaptationConfig] Loaded " << m_resolutions.size()
              << " resolutions, " << m_aspectProfiles.size()
              << " aspect profiles from " << path << std::endl;
    DEBUG_LOG_FMT("[ROIAdaptationConfig] Loaded %zu resolutions, %zu aspect profiles from %s",
                  m_resolutions.size(), m_aspectProfiles.size(), path.c_str());
    return true;
}

// ============================================================================
// Parse
// ============================================================================

bool ROIAdaptationConfig::ParseJson(const std::string& json) {
    m_resolutions.clear();
    m_aspectProfiles.clear();
    m_resolutionIndex.clear();

    // Optional: read reference width/height (defaults to 1920x1080).
    {
        std::string bt = ExtractJsonValue(json, "baseTransform");
        if (!bt.empty()) {
            std::string rw_str = ExtractJsonValue(bt, "referenceWidth");
            std::string rh_str = ExtractJsonValue(bt, "referenceHeight");
            if (!rw_str.empty()) m_refW = std::atoi(rw_str.c_str());
            if (!rh_str.empty()) m_refH = std::atoi(rh_str.c_str());
        }
    }

    // --- Parse aspectProfiles (informational; used for fallback grouping) ---
    {
        std::string arr = ExtractJsonValue(json, "aspectProfiles");
        if (!arr.empty() && arr.front() == '[') {
            std::string inner = arr.substr(1, arr.size() - 2);
            // Each entry is a {...} object
            size_t pos = 0;
            while (pos < inner.size()) {
                size_t obj_start = inner.find('{', pos);
                if (obj_start == std::string::npos) break;
                std::string obj = ExtractSection(inner, obj_start);
                if (obj.empty()) break;

                AspectProfileEntry p;
                p.aspectGroup = ExtractJsonValue(obj, "aspectGroup");
                std::string ta = ExtractJsonValue(obj, "targetAspect");
                if (!ta.empty()) p.targetAspect = std::atof(ta.c_str());
                std::string to = ExtractJsonValue(obj, "tolerance");
                if (!to.empty()) p.tolerance = std::atof(to.c_str());
                std::string bt = ExtractJsonValue(obj, "baseTransform");
                if (!bt.empty()) {
                    std::string xb = ExtractJsonValue(bt, "xBias");
                    std::string yb = ExtractJsonValue(bt, "yBias");
                    std::string ss = ExtractJsonValue(bt, "sizeScale");
                    if (!xb.empty()) p.xBias = std::atoi(xb.c_str());
                    if (!yb.empty()) p.yBias = std::atoi(yb.c_str());
                    if (!ss.empty()) p.sizeScale = std::atof(ss.c_str());
                }

                // v2.0: optional canonicalAnchor + canonicalResiduals + scaleLaw.
                std::string canAnch = ExtractJsonValue(obj, "canonicalAnchor");
                std::string canRes  = ExtractJsonValue(obj, "canonicalResiduals");
                std::string law     = ExtractJsonValue(obj, "scaleLaw");
                if (!canAnch.empty() && !canRes.empty()) {
                    std::string cw = ExtractJsonValue(canAnch, "width");
                    std::string ch = ExtractJsonValue(canAnch, "height");
                    if (!cw.empty() && !ch.empty()) {
                        p.canonicalW = std::atoi(cw.c_str());
                        p.canonicalH = std::atoi(ch.c_str());
                        auto iconEntries = SplitJsonEntries(canRes);
                        for (const auto& ie : iconEntries) {
                            const std::string& iconName = ie.first;
                            const std::string& iconVal  = ie.second;
                            ResidualEntry re;
                            std::string dx = ExtractJsonValue(iconVal, "dx");
                            std::string dy = ExtractJsonValue(iconVal, "dy");
                            std::string dw = ExtractJsonValue(iconVal, "dw");
                            std::string dh = ExtractJsonValue(iconVal, "dh");
                            if (!dx.empty()) re.dx = std::atoi(dx.c_str());
                            if (!dy.empty()) re.dy = std::atoi(dy.c_str());
                            if (!dw.empty()) re.dw = std::atoi(dw.c_str());
                            if (!dh.empty()) re.dh = std::atoi(dh.c_str());
                            p.canonicalResiduals[iconName] = re;
                        }
                        p.hasCanonical = !p.canonicalResiduals.empty();

                        // Parse scale law field lists (default: dy/dw/dh linear, dx invariant).
                        auto parseFieldList = [](const std::string& arr) {
                            std::vector<std::string> out;
                            if (arr.empty() || arr.front() != '[') return out;
                            std::string inner = arr.substr(1, arr.size() - 2);
                            size_t q = 0;
                            while (q < inner.size()) {
                                size_t qs = inner.find('"', q);
                                if (qs == std::string::npos) break;
                                qs++;
                                size_t qe = inner.find('"', qs);
                                if (qe == std::string::npos) break;
                                out.push_back(inner.substr(qs, qe - qs));
                                q = qe + 1;
                            }
                            return out;
                        };
                        if (!law.empty()) {
                            std::string linStr = ExtractJsonValue(law, "scaleLinearFields");
                            std::string invStr = ExtractJsonValue(law, "scaleInvariantFields");
                            p.scaleLinearFields    = parseFieldList(linStr);
                            p.scaleInvariantFields = parseFieldList(invStr);
                        }
                        if (p.scaleLinearFields.empty()) {
                            p.scaleLinearFields    = {"dy", "dw", "dh"};
                        }
                        if (p.scaleInvariantFields.empty()) {
                            p.scaleInvariantFields = {"dx"};
                        }
                    }
                }

                // v2.2: optional canonicalTargets (absolute [x,y,w,h] arrays).
                // Format: "canonicalTargets": { "IconName": [x, y, w, h], ... }
                // Requires canonicalAnchor to be present for the canonical width.
                {
                    std::string canTgt = ExtractJsonValue(obj, "canonicalTargets");
                    if (!canTgt.empty() && !canAnch.empty()) {
                        // Ensure canonicalW is set (may already be from canonicalResiduals block)
                        if (p.canonicalW == 0) {
                            std::string cw = ExtractJsonValue(canAnch, "width");
                            std::string ch = ExtractJsonValue(canAnch, "height");
                            if (!cw.empty()) p.canonicalW = std::atoi(cw.c_str());
                            if (!ch.empty()) p.canonicalH = std::atoi(ch.c_str());
                        }
                        if (p.canonicalW > 0) {
                            auto tgtEntries = SplitJsonEntries(canTgt);
                            for (const auto& te : tgtEntries) {
                                const std::string& tName = te.first;
                                const std::string& tArr  = te.second;
                                // Parse [x, y, w, h] array
                                if (tArr.size() >= 2 && tArr.front() == '[' && tArr.back() == ']') {
                                    std::string inner = tArr.substr(1, tArr.size() - 2);
                                    std::array<int, 4> coords = {0, 0, 0, 0};
                                    int idx = 0;
                                    std::istringstream ss(inner);
                                    std::string tok;
                                    while (std::getline(ss, tok, ',') && idx < 4) {
                                        // Trim whitespace
                                        size_t s = tok.find_first_not_of(" \t\n\r");
                                        size_t e = tok.find_last_not_of(" \t\n\r");
                                        if (s != std::string::npos && e != std::string::npos) {
                                            coords[idx++] = std::atoi(tok.substr(s, e - s + 1).c_str());
                                        }
                                    }
                                    if (idx == 4) {
                                        p.canonicalTargets[tName] = coords;
                                    }
                                }
                            }
                            p.hasCanonicalTargets = !p.canonicalTargets.empty();
                        }
                    }
                }

                m_aspectProfiles.push_back(p);

                size_t close = obj_start + obj.size();
                pos = inner.find(',', close);
                if (pos == std::string::npos) break;
                pos++;
            }
        }
    }

    // --- Parse specificResolutions ---
    {
        std::string arr = ExtractJsonValue(json, "specificResolutions");
        if (!arr.empty() && arr.front() == '[') {
            std::string inner = arr.substr(1, arr.size() - 2);
            size_t pos = 0;
            while (pos < inner.size()) {
                size_t obj_start = inner.find('{', pos);
                if (obj_start == std::string::npos) break;
                std::string obj = ExtractSection(inner, obj_start);
                if (obj.empty()) break;

                ResolutionEntry r;
                std::string w_str = ExtractJsonValue(obj, "width");
                std::string h_str = ExtractJsonValue(obj, "height");
                if (w_str.empty() || h_str.empty()) {
                    size_t close = obj_start + obj.size();
                    pos = inner.find(',', close);
                    if (pos == std::string::npos) break;
                    pos++;
                    continue;
                }
                r.w = std::atoi(w_str.c_str());
                r.h = std::atoi(h_str.c_str());
                r.aspectGroup = ExtractJsonValue(obj, "aspectGroup");

                // baseTransform fields (legacy metadata for audit/fallback)
                std::string bt = ExtractJsonValue(obj, "baseTransform");
                if (!bt.empty()) {
                    std::string xb = ExtractJsonValue(bt, "xBias");
                    std::string yb = ExtractJsonValue(bt, "yBias");
                    std::string ss = ExtractJsonValue(bt, "sizeScale");
                    if (!xb.empty()) r.xBias = std::atoi(xb.c_str());
                    if (!yb.empty()) r.yBias = std::atoi(yb.c_str());
                    if (!ss.empty()) r.sizeScale = std::atof(ss.c_str());
                }

                // residualCorrection entries
                std::string rc = ExtractJsonValue(obj, "residualCorrection");
                if (!rc.empty()) {
                    auto iconEntries = SplitJsonEntries(rc);
                    for (const auto& ie : iconEntries) {
                        const std::string& iconName = ie.first;
                        const std::string& iconVal = ie.second;
                        ResidualEntry re;
                        std::string dx = ExtractJsonValue(iconVal, "dx");
                        std::string dy = ExtractJsonValue(iconVal, "dy");
                        std::string dw = ExtractJsonValue(iconVal, "dw");
                        std::string dh = ExtractJsonValue(iconVal, "dh");
                        if (!dx.empty()) re.dx = std::atoi(dx.c_str());
                        if (!dy.empty()) re.dy = std::atoi(dy.c_str());
                        if (!dw.empty()) re.dw = std::atoi(dw.c_str());
                        if (!dh.empty()) re.dh = std::atoi(dh.c_str());
                        r.residuals[iconName] = re;
                    }
                }

                m_resolutionIndex.insert({MakeKey(r.w, r.h), m_resolutions.size()});
                m_resolutions.push_back(r);

                size_t close = obj_start + obj.size();
                pos = inner.find(',', close);
                if (pos == std::string::npos) break;
                pos++;
            }
        }
    }

    return !m_resolutions.empty();
}

// ============================================================================
// Lookups
// ============================================================================

bool ROIAdaptationConfig::GetResidual(int W, int H,
                                      const std::string& iconName,
                                      ResidualEntry& outResidual,
                                      const std::string& aspectCategory) const {
    outResidual = ResidualEntry{};
    if (!m_loaded) return false;

    // If an aspect category is specified, try to find a resolution entry
    // that matches both (W, H) and the aspect category.
    if (!aspectCategory.empty()) {
        auto range = m_resolutionIndex.equal_range(MakeKey(W, H));
        for (auto it = range.first; it != range.second; ++it) {
            const auto& entry = m_resolutions[it->second];
            if (entry.aspectGroup == aspectCategory) {
                auto rit = entry.residuals.find(iconName);
                if (rit != entry.residuals.end()) {
                    outResidual = rit->second;
                    return true;
                }
                // Entry exists for (W, H, aspectCategory) but no residual for icon
                // Fall through to return zeros for this icon
                return false;
            }
        }
        // No entry for (W, H, aspectCategory) at all => fall through to single-key lookup
    }

    // Original logic: single-key lookup (first match)
    auto it = m_resolutionIndex.find(MakeKey(W, H));
    if (it == m_resolutionIndex.end()) return false;
    const auto& entry = m_resolutions[it->second];
    auto rit = entry.residuals.find(iconName);
    if (rit == entry.residuals.end()) return false;
    outResidual = rit->second;
    return true;
}

ResidualEntry ROIAdaptationConfig::ApplyScaleLaw(
    const ResidualEntry& canonical,
    int canonicalW, int canonicalH,
    int targetW, int targetH,
    const std::vector<std::string>& scaleLinearFields,
    const std::vector<std::string>& scaleInvariantFields) {
    ResidualEntry out;
    // Default: all fields linear (legacy).
    bool isLinear[4] = {true, true, true, true};
    bool isInvariant[4] = {false, false, false, false};
    auto mark = [&](const std::vector<std::string>& fields, bool v[]) {
        for (const auto& f : fields) {
            if (f == "dx") v[0] = true;
            else if (f == "dy") v[1] = true;
            else if (f == "dw") v[2] = true;
            else if (f == "dh") v[3] = true;
        }
    };
    // Reset to "not linear" then mark linear fields.
    for (int i = 0; i < 4; ++i) isLinear[i] = false;
    mark(scaleLinearFields, isLinear);
    mark(scaleInvariantFields, isInvariant);

    // Apply.
    if (isInvariant[0]) out.dx = canonical.dx;
    else if (isLinear[0]) out.dx = ScaleField(canonical.dx, canonicalW, targetW);
    else out.dx = canonical.dx;

    if (isInvariant[1]) out.dy = canonical.dy;
    else if (isLinear[1]) out.dy = ScaleField(canonical.dy, canonicalW, targetW);
    else out.dy = canonical.dy;

    if (isInvariant[2]) out.dw = canonical.dw;
    else if (isLinear[2]) out.dw = ScaleField(canonical.dw, canonicalW, targetW);
    else out.dw = canonical.dw;

    if (isInvariant[3]) out.dh = canonical.dh;
    else if (isLinear[3]) out.dh = ScaleField(canonical.dh, canonicalW, targetW);
    else out.dh = canonical.dh;

    return out;
}

void ROIAdaptationConfig::GetResidualWithFallback(
    int W, int H,
    const std::string& iconName,
    ResidualEntry& outResidual,
    const std::string& aspectCategory) const {
    outResidual = ResidualEntry{};

    // 1) Exact (W, H) entry — pass aspectCategory to disambiguate
    //    same render resolution across different physical screens
    //    (e.g. 1920x1080 on 16:9 vs 21:9).
    if (GetResidual(W, H, iconName, outResidual, aspectCategory)) {
        return;
    }

    // 2) Aspect profile with canonical anchor + scale law
    if (!m_aspectProfiles.empty() && H > 0 && W > 0) {
        const AspectProfileEntry* best = nullptr;

        // 2a) When aspectCategory is provided (from ScreenAspectDetector),
        //     prefer exact string match on aspectGroup. This correctly
        //     handles 21:9 physical screen at 16:9 render resolution.
        if (!aspectCategory.empty()) {
            for (const auto& p : m_aspectProfiles) {
                if (p.aspectGroup == aspectCategory) {
                    best = &p;
                    break;
                }
            }
        }

        // 2b) Fallback: match by render resolution aspect ratio (legacy path)
        if (!best) {
            double aspect = static_cast<double>(W) / static_cast<double>(H);
            double bestDist = 1e9;
            for (const auto& p : m_aspectProfiles) {
                double d = std::fabs(p.targetAspect - aspect);
                if (d <= p.tolerance && d < bestDist) {
                    bestDist = d;
                    best = &p;
                }
            }
        }

        if (best && best->hasCanonical) {
            auto cit = best->canonicalResiduals.find(iconName);
            if (cit != best->canonicalResiduals.end()) {
                outResidual = ApplyScaleLaw(
                    cit->second,
                    best->canonicalW, best->canonicalH,
                    W, H,
                    best->scaleLinearFields,
                    best->scaleInvariantFields);
                return;
            }
        }

        // 2c) canonicalTargets path: absolute target coordinates scaled
        //     linearly from the canonical anchor resolution.  The caller
        //     (GetAdaptiveRect in common.hpp) handles this externally via
        //     GetCanonicalTarget() because Stage-1 output is not available here.
    }

    // 3) Zero residual
    outResidual = ResidualEntry{};
}

// ============================================================================
// v2.3 — HYBRID MODE
// ============================================================================

bool ROIAdaptationConfig::HasAspectMismatch(
    int W, int H,
    const std::string& aspectCategory,
    std::string& outGeometryGroup) const
{
    outGeometryGroup.clear();
    if (!m_loaded || aspectCategory.empty()) return false;

    // Check if there is ANY entry for (W, H) via single-key lookup
    auto it = m_resolutionIndex.find(MakeKey(W, H));
    if (it == m_resolutionIndex.end()) return false;

    const auto& firstEntry = m_resolutions[it->second];
    std::string entryGroup = firstEntry.aspectGroup;

    // Now check if the aspectCategory matches any entry for this (W, H)
    auto range = m_resolutionIndex.equal_range(MakeKey(W, H));
    for (auto rit = range.first; rit != range.second; ++rit) {
        const auto& entry = m_resolutions[rit->second];
        if (entry.aspectGroup == aspectCategory) {
            // Found a matching entry — no mismatch
            return false;
        }
    }

    // No entry matches aspectCategory, but (W, H) exists as some other group.
    outGeometryGroup = entryGroup;
    return true;
}

void ROIAdaptationConfig::GetHybridResidual(
    int W, int H,
    const std::string& iconName,
    ResidualEntry& outResidual,
    const std::string& aspectCategory,
    ImageProcessingParams& outHybridParams) const
{
    outResidual = ResidualEntry{};
    outHybridParams = ImageProcessingParams{};

    // Step 1: Check if there's an aspect mismatch.
    // If the specific entry exists for (W, H) but belongs to a different
    // aspect group than aspectCategory, we need hybrid mode.
    std::string geometryGroup;
    bool mismatch = HasAspectMismatch(W, H, aspectCategory, geometryGroup);

    if (mismatch) {
        // HYBRID MODE ACTIVATED.
        // Use the geometry (residual) from the specific entry.
        // The aspectCategory-based lookup will fail (step 1 in
        // GetResidualWithFallback), but single-key lookup will find it.
        GetResidualWithFallback(W, H, iconName, outResidual, "");

        // Now get the correct image processing params from the physical
        // screen's aspect profile, not from the resolution entry.
        if (!GetAspectProfileProcessingParams(aspectCategory, outHybridParams)) {
            // Cannot find aspect profile for physical screen category.
            // Output a warning but keep the geometry — ROI position
            // correctness takes priority over perfect image params.
            DEBUG_LOG_FMT("[ROI-HYBRID] WARNING: aspectCategory=%s has no profile for W=%d H=%d. "
                          "Using default params (ROI geometry preserved from %s entry).",
                          aspectCategory.c_str(), W, H, geometryGroup.c_str());
            // outHybridParams is already zeroed (sizeScale=1.0, bias=0).
        } else {
            DEBUG_LOG_FMT("[ROI-HYBRID] Activated for W=%d H=%d icon=%s. "
                          "Geometry from %s spec entry, params from %s profile "
                          "(sizeScale=%.2f, xBias=%d, yBias=%d).",
                          W, H, iconName.c_str(),
                          geometryGroup.c_str(), aspectCategory.c_str(),
                          outHybridParams.sizeScale,
                          outHybridParams.xBias,
                          outHybridParams.yBias);
        }
        return;
    }

    // Step 2: No mismatch — either aspectCategory matches (W,H), or there
    // is no specific entry for (W,H), or no aspectCategory was given.
    // In all these cases, use standard fallback logic.
    // outHybridParams remains zeroed (default).
    GetResidualWithFallback(W, H, iconName, outResidual, aspectCategory);
}

bool ROIAdaptationConfig::GetHybridProcessingParams(
    int W, int H,
    ImageProcessingParams& outParams,
    const std::string& aspectCategory) const
{
    outParams = ImageProcessingParams{};

    if (!m_loaded) return false;

    // Check if there's an aspect mismatch
    std::string geometryGroup;
    if (!aspectCategory.empty() && HasAspectMismatch(W, H, aspectCategory, geometryGroup)) {
        // Mismatch: get params from aspect profile, not from specific entry
        if (GetAspectProfileProcessingParams(aspectCategory, outParams)) {
            DEBUG_LOG_FMT("[ROI-HYBRID] GetHybridProcessingParams: W=%d H=%d "
                          "using %s profile (geometry from %s entry)",
                          W, H, aspectCategory.c_str(), geometryGroup.c_str());
            return true;
        }
        // Fall through to legacy path if profile isn't found
        DEBUG_LOG_FMT("[ROI-HYBRID] GetHybridProcessingParams: W=%d H=%d "
                      "profile %s not found, falling back to entry params",
                      W, H, aspectCategory.c_str());
    }

    // Legacy path: get from specific resolution entry
    int xBias = 0, yBias = 0;
    double sizeScale = 1.0;
    bool found = GetResolutionMeta(W, H, xBias, yBias, sizeScale, aspectCategory);
    if (found) {
        outParams.xBias = xBias;
        outParams.yBias = yBias;
        outParams.sizeScale = sizeScale;
    }
    return found;
}

bool ROIAdaptationConfig::GetAspectProfileProcessingParams(
    const std::string& aspectCategory,
    ImageProcessingParams& outParams) const
{
    outParams = ImageProcessingParams{};
    if (m_aspectProfiles.empty() || aspectCategory.empty()) return false;

    for (const auto& p : m_aspectProfiles) {
        if (p.aspectGroup == aspectCategory) {
            outParams.xBias = p.xBias;
            outParams.yBias = p.yBias;
            outParams.sizeScale = p.sizeScale;
            return true;
        }
    }
    return false;
}

bool ROIAdaptationConfig::GetResolutionMeta(int W, int H,
                                            int& outXbias,
                                            int& outYbias,
                                            double& outSizeScale,
                                            const std::string& aspectCategory) const {
    outXbias = 0;
    outYbias = 0;
    outSizeScale = 1.0;
    if (!m_loaded) return false;

    // If aspectCategory specified, find matching entry
    if (!aspectCategory.empty()) {
        auto range = m_resolutionIndex.equal_range(MakeKey(W, H));
        for (auto it = range.first; it != range.second; ++it) {
            const auto& entry = m_resolutions[it->second];
            if (entry.aspectGroup == aspectCategory) {
                outXbias = entry.xBias;
                outYbias = entry.yBias;
                outSizeScale = entry.sizeScale;
                return true;
            }
        }
    }

    // Fallback: first match
    auto it = m_resolutionIndex.find(MakeKey(W, H));
    if (it == m_resolutionIndex.end()) return false;
    const auto& entry = m_resolutions[it->second];
    outXbias = entry.xBias;
    outYbias = entry.yBias;
    outSizeScale = entry.sizeScale;
    return true;
}

bool ROIAdaptationConfig::GetAspectProfile(double aspect,
                                           std::string& outGroup,
                                           double& outSizeScale,
                                           int& outXbias,
                                           int& outYbias) const {
    outGroup.clear();
    outSizeScale = 1.0;
    outXbias = 0;
    outYbias = 0;
    if (m_aspectProfiles.empty()) return false;

    const AspectProfileEntry* best = nullptr;
    double bestDist = 1e9;
    for (const auto& p : m_aspectProfiles) {
        double d = std::fabs(p.targetAspect - aspect);
        if (d <= p.tolerance && d < bestDist) {
            bestDist = d;
            best = &p;
        }
    }
    if (!best) {
        // Pick the closest profile (without tolerance gate) for graceful degradation.
        for (const auto& p : m_aspectProfiles) {
            double d = std::fabs(p.targetAspect - aspect);
            if (d < bestDist) {
                bestDist = d;
                best = &p;
            }
        }
    }
    if (!best) return false;
    outGroup = best->aspectGroup;
    outSizeScale = best->sizeScale;
    outXbias = best->xBias;
    outYbias = best->yBias;
    return true;
}

// ============================================================================
// GetCanonicalTarget (v2.2)
// ============================================================================

bool ROIAdaptationConfig::GetCanonicalTarget(
    const std::string& aspectCategory,
    const std::string& iconName,
    int targetW, int targetH,
    int& outX, int& outY,
    int& outW, int& outH) const
{
    outX = outY = outW = outH = 0;
    if (m_aspectProfiles.empty() || targetW <= 0 || targetH <= 0) return false;

    const AspectProfileEntry* best = nullptr;

    // 1) Exact string match on aspectGroup when aspectCategory is provided
    if (!aspectCategory.empty()) {
        for (const auto& p : m_aspectProfiles) {
            if (p.aspectGroup == aspectCategory && p.hasCanonicalTargets) {
                best = &p;
                break;
            }
        }
    }

    // 2) Fallback: match by render resolution aspect ratio
    if (!best) {
        double aspect = static_cast<double>(targetW) / static_cast<double>(targetH);
        double bestDist = 1e9;
        for (const auto& p : m_aspectProfiles) {
            if (!p.hasCanonicalTargets) continue;
            double d = std::fabs(p.targetAspect - aspect);
            if (d <= p.tolerance && d < bestDist) {
                bestDist = d;
                best = &p;
            }
        }
    }

    if (!best || best->canonicalW <= 0) return false;

    auto it = best->canonicalTargets.find(iconName);
    if (it == best->canonicalTargets.end()) return false;

    // Scale: predicted_i = round(canonicalTarget_i * targetW / canonicalW)
    const auto& coords = it->second;
    double ratio = static_cast<double>(targetW) / static_cast<double>(best->canonicalW);

    auto roundScaled = [](double v) -> int {
        return static_cast<int>(v + (v >= 0 ? 0.5 : -0.5));
    };

    outX = roundScaled(static_cast<double>(coords[0]) * ratio);
    outY = roundScaled(static_cast<double>(coords[1]) * ratio);
    outW = roundScaled(static_cast<double>(coords[2]) * ratio);
    outH = roundScaled(static_cast<double>(coords[3]) * ratio);

    return true;
}

} // namespace roi