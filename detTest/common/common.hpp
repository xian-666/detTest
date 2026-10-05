#ifndef COMMON_HPP
#define COMMON_HPP

#include <windows.h>
#include <shlobj.h>
#include <combaseapi.h>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <regex>
#include <unordered_map>
#include <cwchar>
#include <ctime>
#include <cmath>
#include <opencv2/opencv.hpp>
#include <atomic>
#include "IScreenshotProvider.hpp"
#include "ScreenAspectDetector.hpp"
#include "../ROIAdaptationConfig.h"
#include "ROIScaleManager.hpp"
#include "AppConfig.h"
#include "TargetWindow.h"
#include "RoiIndex.hpp"

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

// NOTE: WINDOW_TITLE used to be hard-coded to the Honour-of-Kings title here.
// detTest targets multiple games, so the window is now selected through the
// [Target] section of config.ini -- see common/TargetWindow.h.

const struct Resolution {
	int width;	int height;
	Resolution() {
		width = 1920; height = 1080;
	}
};

namespace roi
{
	// ====== Anchor-based ROI system ======
	// LT_1 is the primary anchor point — most stable across all resolutions.
	// All other ROIs are defined as offsets from LT_1.
	// Icons are grouped by proximity to minimize cumulative offset error.

	struct AnchorROI {
		int anchorX;          // Base anchor X (1920x1080 reference)
		int anchorY;          // Base anchor Y (1920x1080 reference)
		int offsetX;          // Offset from anchor X
		int offsetY;          // Offset from anchor Y
		int w, h;
	};

	// ====== Dynamic aspect ratio calculation ======
	// Calculate the actual aspect ratio from render resolution and match to
	// the closest aspect profile with tolerance. This replaces the JSON-based
	// aspectGroup lookup with pure mathematical calculation.
	inline std::string CalculateDynamicAspectCategory(int imgWidth, int imgHeight, double tolerance = 0.01) {
		if (imgHeight == 0) return "16:9";  // Fallback to prevent division by zero
		
		double actualAspect = static_cast<double>(imgWidth) / static_cast<double>(imgHeight);
		
		// Define standard aspect ratios with their target values
		struct AspectRatio {
			std::string category;
			double target;
		};
		
		// Standard aspect ratios in order of specificity (most specific first)
		std::vector<AspectRatio> standardAspects = {
			{"21:9", 2.333333},   // 21:9 ultrawide
			{"16:10", 1.6},      // 16:10
			{"16:9", 1.777778},  // 16:9
			{"5:3", 1.666667},   // 5:3 (1280x768)
			{"25:16", 1.5625},   // 25:16 (1600x1024)
			{"5:4", 1.25},       // 5:4 (1280x1024)
			{"4:3", 1.333333}    // 4:3
		};
		
		// Find the closest match within tolerance
		std::string bestMatch = "16:9";  // Default fallback
		double minDiff = std::abs(actualAspect - 1.777778);  // Default to 16:9 difference
		
		for (const auto& aspect : standardAspects) {
			double diff = std::abs(actualAspect - aspect.target);
			if (diff < tolerance) {
				// Found a match within tolerance
				return aspect.category;
			}
			if (diff < minDiff) {
				minDiff = diff;
				bestMatch = aspect.category;
			}
		}
		
		// Return the closest match even if outside tolerance (best effort)
		return bestMatch;
	}

	// ====== Generic specific adjustment system ======
	// For non-standard resolutions that need per-icon fine-tuning beyond global bias.
	struct Offset {
		int dx;  // X offset in 1920x1080 reference pixels
		int dy;  // Y offset in 1920x1080 reference pixels
	};

	struct SpecificAdjustment {
		int w;  // Target resolution width
		int h;  // Target resolution height
		std::vector<std::pair<std::string, Offset>> adjustments;  // Icon name -> offset

		// Constructor for aggregate initialization
		SpecificAdjustment(int width, int height,
			std::initializer_list<std::pair<std::string, Offset>> init)
			: w(width), h(height), adjustments(init) { }
	};

	// ============================================================================
	// SPECIFIC_ADJUSTMENT_REGISTRY
	// Per-resolution per-icon fine-tuning data is now externalized to
	// roi_adaptation.json. The two-stage C++ transform uses per-resolution
	// residual corrections from the JSON (dx, dy, dw, dh).
	//
	// This variable is kept as an empty inline const for backward compatibility
	// with any legacy code that references it; the actual registered data lives
	// in ROIAdaptationConfig.
	// ============================================================================
	inline const std::vector<SpecificAdjustment> SPECIFIC_ADJUSTMENT_REGISTRY = {};

	// Ensure the JSON config has been loaded exactly once. Safe to call repeatedly.
	inline void EnsureConfigLoaded() {
		auto& cfg = ROIAdaptationConfig::GetInstance();
		if (!cfg.IsLoaded()) {
			cfg.Load();
		}
	}

	// Group 1: 技能操作区 (LT_1 is the anchor, all offsets relative to it)
	// 1920x1080 reference: LT_1 = (1529, 1015)
	constexpr int REF_W = 1920;
	constexpr int REF_H = 1080;

	// ====== Legacy compatibility shim ======
	// SKILL_BASE_ROIS is deprecated; use SKILL_ANCHOR_ROIS instead.
	// This empty alias keeps old code from breaking.
	inline const std::unordered_map<std::string, AnchorROI> SKILL_BASE_ROIS = { };

	constexpr int ANCHOR_LT1_X = 1529;
	constexpr int ANCHOR_LT1_Y = 1015;

	inline const std::unordered_map<std::string, AnchorROI> SKILL_ANCHOR_ROIS = {
		// --- 技能区 (直接贴在 LT_1 周围) ---
		{"LT_1",        {ANCHOR_LT1_X, ANCHOR_LT1_Y,    0,    0, 55, 54}},  // 锚点本身
		{"RT_1",        {ANCHOR_LT1_X, ANCHOR_LT1_Y,  115,    0, 55, 54}},  // LT_1 右侧
		{"LB_1",        {ANCHOR_LT1_X, ANCHOR_LT1_Y,    0,  -65, 55, 54}},  // LT_1 上方
		{"RB_1",        {ANCHOR_LT1_X, ANCHOR_LT1_Y,  116,  -65, 55, 54}},  // LT_1 右上方

		// --- 按键2区 (紧贴技能区中排) ---
		{"LB_2",        {ANCHOR_LT1_X, ANCHOR_LT1_Y,  -30,  -33, 55, 59}},
		{"LT_2",        {ANCHOR_LT1_X, ANCHOR_LT1_Y,   48,  -33, 55, 59}},
		{"RT_2",        {ANCHOR_LT1_X, ANCHOR_LT1_Y,  128,  -33, 55, 59}},

		// --- 状态区 (LsRs 在技能区右侧，距离中等) ---
		{"LsRs",        {ANCHOR_LT1_X, ANCHOR_LT1_Y,  213,  -67, 98, 97}},
	};

	// ============================================================================
	// GetResolutionBias — legacy compatibility wrapper.
	// Returns the legacy (xBias, yBias, sizeScale) tuple for the given resolution,
	// read from roi_adaptation.json. The two-stage C++ transform does NOT use
	// these values directly; the residual correction in JSON already accounts
	// for the offset these values would otherwise introduce.
	//
	// This function is preserved for any external callers that depend on the
	// legacy tuple semantics.
	// ============================================================================
	// v2.3: GetResolutionBias is now hybrid-aware — when the physical screen
	// aspect (e.g. 21:9) does not match the specificResolutions entry's
	// aspectGroup (e.g. 16:10 for 1920x1200), the returned params come from
	// the physical screen's aspectProfile rather than the specific entry.
	// This ensures image preprocessing params match the actual display,
	// preventing misclassifications while preserving correct ROI geometry.
	inline void GetResolutionBias(int W, int H, int& out_xBias, int& out_yBias, double& out_sizeScale) {
		out_xBias = 0;
		out_yBias = 0;
		out_sizeScale = 1.0;

		EnsureConfigLoaded();
		const auto& cfg = ROIAdaptationConfig::GetInstance();

		// Use hybrid params: when aspectCategory is available (from ScreenAspectDetector),
		// this returns the correct image-processing params for the physical screen,
		// even when the specific resolution entry (e.g. 1920x1200 as 16:10) belongs
		// to a different aspect group than the physical screen (e.g. 21:9).
		std::string aspectCategory = screen_aspect::GetAspectCategory();
		if (!aspectCategory.empty() && aspectCategory != screen_aspect::ASPECT_UNKNOWN) {
			roi::ImageProcessingParams hybrid;
			cfg.GetHybridProcessingParams(W, H, hybrid, aspectCategory);
			out_xBias = hybrid.xBias;
			out_yBias = hybrid.yBias;
			out_sizeScale = hybrid.sizeScale;
		} else {
			// No aspect category available — use legacy fallback
			cfg.GetResolutionMeta(W, H, out_xBias, out_yBias, out_sizeScale);
		}
	}

	// Legacy overload for backward compatibility
	inline void GetResolutionBias(int W, int H, int& out_xBias, int& out_yBias) {
		double sizeScale;
		GetResolutionBias(W, H, out_xBias, out_yBias, sizeScale);
	}

	inline int GetResolutionPadding(int W, int H) {
		double aspect = static_cast<double>(W) / static_cast<double>(H);
		double refAspect = static_cast<double>(REF_W) / static_cast<double>(REF_H);

		double aspectDiff = std::abs(aspect - refAspect);
		if (aspectDiff > 0.15) {
			return 10;
		}
		else if (aspectDiff > 0.08) {
			return 6;
		}
		return 4;
	}

		// ============================================================================
	// GetAdaptiveRect — the two-stage transform with dynamic aspect ratio.
	//
	// STAGE 1 — 16:9 base + geometric transform (no per-resolution branching):
	//     scale  = min(imgW/REF_W, imgH/REF_H)        (uniform scale factor)
	//     cX     = (imgW - REF_W*scale) / 2           (centering offset X)
	//     cY     = (imgH - REF_H*scale) / 2           (centering offset Y)
	//     x0     = (anchorX + offsetX) * scale + cX
	//     y0     = (anchorY + offsetY) * scale + cY
	//     w0     = baseW * scale
	//     h0     = baseH * scale
	//
	// STAGE 2 — Per-resolution residual correction (from JSON):
	//     (x, y, w, h) = (x0, y0, w0, h0) + (dx, dy, dw, dh)
	//
	// NEW: Dynamic aspect ratio calculation (v3.0):
	//     - Aspect category is calculated dynamically from render resolution (W/H)
	//     - No longer relies on JSON aspectGroup or physical screen detection
	//     - Preserves backward compatibility with existing specificResolutions
	//     - Falls back to aspect profile canonicalTargets for unregistered resolutions
	//
	// The optional aspectCategory parameter is kept for backward compatibility but
	// is overridden by dynamic calculation when empty. For resolutions with multiple
	// aspect group entries (e.g. 1920x1080 for both "16:9" and "21:9"), the dynamic
	// calculation determines the correct group based on actual render aspect.
	// ============================================================================
	inline cv::Rect GetAdaptiveRect(const std::string& skillName, int imgWidth, int imgHeight,
	                                const std::string& aspectCategory = "") {
		EnsureConfigLoaded();

		auto it = SKILL_ANCHOR_ROIS.find(skillName);
		if (it == SKILL_ANCHOR_ROIS.end()) {
			return cv::Rect();
		}

		const auto& base = it->second;
		const auto& cfg = ROIAdaptationConfig::GetInstance();
		const int refW = cfg.GetReferenceWidth();
		const int refH = cfg.GetReferenceHeight();

		// --- DYNAMIC ASPECT CALCULATION ---
		// Calculate aspect category from render resolution (W/H) with tolerance
		// This replaces the JSON-based aspectGroup lookup with pure math
		std::string dynamicAspectCategory = CalculateDynamicAspectCategory(imgWidth, imgHeight, 0.01);
		
		// Use provided aspectCategory if available (for backward compatibility),
		// otherwise use the dynamically calculated one
		std::string effectiveAspectCategory = aspectCategory.empty() ? 
			dynamicAspectCategory : aspectCategory;

		// --- STAGE 1: 16:9 base + geometric transform ---
		const double scale  = std::min(static_cast<double>(imgWidth) / refW,
		                              static_cast<double>(imgHeight) / refH);
		const double cX     = (static_cast<double>(imgWidth) - refW * scale) * 0.5;
		const double cY     = (static_cast<double>(imgHeight) - refH * scale) * 0.5;
		const double x0Raw  = (base.anchorX + base.offsetX) * scale + cX;
		const double y0Raw  = (base.anchorY + base.offsetY) * scale + cY;
		const double w0Raw  = static_cast<double>(base.w) * scale;
		const double h0Raw  = static_cast<double>(base.h) * scale;

		int x = static_cast<int>(x0Raw);
		int y = static_cast<int>(y0Raw);
		int w = static_cast<int>(w0Raw);
		int h = static_cast<int>(h0Raw);

		// --- STAGE 2: Per-resolution residual correction ---
		// First try to get residual from specificResolutions using dynamic aspect
		ResidualEntry r;
		roi::ImageProcessingParams hybridParamsForLog;
		
		// Try dynamic aspect category first (pure math approach)
		cfg.GetHybridResidual(imgWidth, imgHeight, skillName, r, dynamicAspectCategory, hybridParamsForLog);
		
		// If no residual found with dynamic aspect, try the provided aspectCategory
		// (for backward compatibility with existing configurations)
		if (r.dx == 0 && r.dy == 0 && r.dw == 0 && r.dh == 0 && !aspectCategory.empty()) {
			cfg.GetHybridResidual(imgWidth, imgHeight, skillName, r, aspectCategory, hybridParamsForLog);
		}

		x += r.dx;
		y += r.dy;
		w += r.dw;
		h += r.dh;

		// --- STAGE 3: canonicalTargets fallback ---
		// If no residual was found (all zeros), check if the matching aspect
		// profile carries canonicalTargets (absolute [x,y,w,h] coordinates at
		// a canonical anchor resolution).  If so, scale linearly and compute
		// residual = predicted - stage1_output.
		bool jsonFallbackUsed = false;
		if (r.dx == 0 && r.dy == 0 && r.dw == 0 && r.dh == 0) {
			int predX = 0, predY = 0, predW = 0, predH = 0;
			// Try dynamic aspect category first
			if (cfg.GetCanonicalTarget(dynamicAspectCategory, skillName,
			                           imgWidth, imgHeight,
			                           predX, predY, predW, predH)) {
				x = predX;
				y = predY;
				w = predW;
				h = predH;
				jsonFallbackUsed = true;
			}
			// Fallback to provided aspectCategory if dynamic fails
			else if (!aspectCategory.empty() && cfg.GetCanonicalTarget(aspectCategory, skillName,
			                           imgWidth, imgHeight,
			                           predX, predY, predW, predH)) {
				x = predX;
				y = predY;
				w = predW;
				h = predH;
				jsonFallbackUsed = true;
			}
		}

		// --- STAGE 4: ROIScaleManager mathematical fallback ---
		// If JSON lookup failed (no residual, no canonicalTargets), use pure
		// mathematical scaling from ROIScaleManager. This provides automatic
		// support for any resolution without requiring JSON entries.
		if (!jsonFallbackUsed && r.dx == 0 && r.dy == 0 && r.dw == 0 && r.dh == 0) {
			// Include ROIScaleManager header (must be included at top of file)
			// Use mathematical scaling with aspect-ratio-specific Y-axis offsets
			roi::AbsoluteROI absRoi = roi::ROIScaleManager::GetInstance().GetROI(skillName, imgWidth, imgHeight, true);
			x = absRoi.x;
			y = absRoi.y;
			w = absRoi.width;
			h = absRoi.height;
		}

		// Clamp size to a minimum (matches legacy behavior).
		w = std::max(10, w);
		h = std::max(10, h);

		return cv::Rect(x, y, w, h);
	}

	inline std::vector<cv::Rect> GetAllAdaptiveRects(int imgWidth, int imgHeight,
	                                                 const std::string& aspectCategory = "") {
		std::vector<cv::Rect> result;
		result.reserve(SKILL_ANCHOR_ROIS.size());
		for (const auto& [name, base] : SKILL_ANCHOR_ROIS) {
			result.push_back(GetAdaptiveRect(name, imgWidth, imgHeight, aspectCategory));
		}
		return result;
	}

	inline std::vector<Region> GetAllAdaptiveRegions(int imgWidth, int imgHeight,
	                                                 const std::string& aspectCategory = "") {
		std::vector<Region> result;
		result.reserve(SKILL_ANCHOR_ROIS.size());
		for (const auto& [name, base] : SKILL_ANCHOR_ROIS) {
			cv::Rect r = GetAdaptiveRect(name, imgWidth, imgHeight, aspectCategory);
			result.emplace_back(r.x, r.y, r.width, r.height, name);
		}
		return result;
	}

	// ====== ROI Normalization for Inference ======
	// Normalize ROI image to standard size before inference to ensure consistent
	// model input regardless of actual resolution. This fixes confidence drops
	// caused by varying ROI pixel sizes across different resolutions.

	constexpr int NORMALIZED_ROI_SIZE = 55;  // Base ROI size at 1920x1080 (matches training data)

	inline cv::Mat NormalizeROIForInference(const cv::Mat& roiImage, int targetSize = NORMALIZED_ROI_SIZE) {
		if (roiImage.empty()) return roiImage;

		int currentSize = std::max(roiImage.cols, roiImage.rows);

		if (currentSize <= 0) return roiImage;

		double scale = static_cast<double>(targetSize) / currentSize;

		cv::Mat normalized;
		cv::resize(roiImage, normalized, cv::Size(
			static_cast<int>(roiImage.cols * scale),
			static_cast<int>(roiImage.rows * scale)
		));

		return normalized;
	}

	inline double GetROIScaleFactor(int imgWidth, int imgHeight) {
		// Legacy behavior preserved: aspect-aware uniform scale, then multiply
		// by the JSON's legacy sizeScale. Used by NormalizeROIForInference etc.
		double scaleX = static_cast<double>(imgWidth) / REF_W;
		double scaleY = static_cast<double>(imgHeight) / REF_H;

		double refAspect = static_cast<double>(REF_W) / REF_H;
		double curAspect = static_cast<double>(imgWidth) / imgHeight;
		double aspectDiff = std::abs(curAspect - refAspect);

		double uniformScale = (aspectDiff < 0.02) ? scaleX : scaleY;

		int xBias = 0, yBias = 0;
		double sizeScale = 1.0;
		GetResolutionBias(imgWidth, imgHeight, xBias, yBias, sizeScale);

		return uniformScale * sizeScale;
	}
}

inline const std::vector<std::string> CLASS_LABELS = {
	"ChildofOriginStream","ColdSpring","DisabledState","DongfangYao",
	"HuaMulan","HuaMulanHeavySword","HuaMulanLightSword","Jialuo","Kai",
	"LongYa","LubanDaShi","MengYa","Other","Skill","SnowRabbit",
	"SunBin","SunShangxian","WangZhaojun","XiShi"
};

inline const std::vector<std::string> DETECTION_LABELS = {
	"0","1","2","3","4","5","6","7","8","9"
};

namespace common_detail
{
	struct MonitorIndexData {
		HMONITOR target;
		int index = -1;
		int current = 0;
	};

	static BOOL CALLBACK MonitorIndexEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
		auto& data = *reinterpret_cast<common_detail::MonitorIndexData*>(lParam);
		if (hMon == data.target) {
			data.index = data.current;
			return FALSE;
		}
		data.current++;
		return TRUE;
	}
}

namespace
{
	struct MonitorData {
		POINT target;
		int index = 0;
		bool found = false;
	};

	BOOL CALLBACK MonitorEnumProc(HMONITOR, HDC, LPRECT rect, LPARAM lParam) {
		auto* d = reinterpret_cast<MonitorData*>(lParam);
		if (PtInRect(rect, d->target)) {
			d->found = true;
			return FALSE;
		}
		d->index++;
		return TRUE;
	}
}

namespace common
{
	inline std::wstring ExtractXmlTag(const std::wstring& content, const std::wstring& tagName) {
		try {
			std::wregex pattern(L"<" + tagName + L">([^<]+)</" + tagName + L">",
				std::regex_constants::icase | std::regex_constants::optimize);
			std::wsmatch match;
			if (std::regex_search(content, match, pattern) && match.size() > 1) {
				return match.str(1);
			}
		} catch (...) { }
		return L"";
	}

	inline std::wstring Utf8ToWstring(const std::string& utf8) {
		if (utf8.empty()) return std::wstring();

		int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
		if (size <= 0) return std::wstring();

		std::wstring wstr(size - 1, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], size);
		return wstr;
	}

	inline bool ReadUtf8FileAsWstring(const std::wstring& filePath, std::wstring& outContent) {
		std::ifstream file(std::filesystem::path(filePath), std::ios::binary);
		if (!file.is_open()) {
			return false;
		}

		std::vector<char> buffer((std::istreambuf_iterator<char>(file)),
			std::istreambuf_iterator<char>());
		file.close();

		if (buffer.size() < 3) {
			outContent = Utf8ToWstring(std::string(buffer.begin(), buffer.end()));
			return true;
		}

		if (static_cast<unsigned char>(buffer[0]) == 0xEF &&
			static_cast<unsigned char>(buffer[1]) == 0xBB &&
			static_cast<unsigned char>(buffer[2]) == 0xBF) {
			std::string utf8Str(buffer.begin() + 3, buffer.end());
			outContent = Utf8ToWstring(utf8Str);
		}
		else {
			std::string utf8Str(buffer.begin(), buffer.end());
			outContent = Utf8ToWstring(utf8Str);
		}

		return true;
	}

	inline std::string getTimestamp()
	{
		auto now = std::chrono::system_clock::now();
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
		auto timer = std::chrono::system_clock::to_time_t(now);

		std::tm bt;
		gmtime_s(&bt, &timer);

		std::ostringstream oss;
		oss << std::put_time(&bt, "%Y%m%d_%H%M%S")
			<< "_" << std::setfill('0') << std::setw(3) << ms.count();

		return oss.str();
	}

#ifdef _WIN32
	inline bool imwriteChinese(const std::string& filePath, const cv::Mat& img) {
		std::wstring wfilePath = Utf8ToWstring(filePath);
		FILE* f = _wfopen(wfilePath.c_str(), L"wb");
		if (!f) {
			return false;
		}
		std::vector<unsigned char> buf;
		bool encodeSuccess = cv::imencode(".png", img, buf);
		if (!encodeSuccess) {
			fclose(f);
			return false;
		}
		size_t written = fwrite(buf.data(), 1, buf.size(), f);
		fclose(f);
		return written == buf.size();
	}
#endif

	inline bool saveMosaic(const cv::Mat& mosaic, const std::string& outFolder)
	{
		std::string timestamp = getTimestamp();
		std::ostringstream oss;
		oss << "mosaic_" << timestamp
			<< "_batch" << std::setfill('0') << std::setw(4)
			<< ".png";

		std::string outPath = (std::filesystem::path(outFolder) / oss.str()).string();

#ifdef _WIN32
		if (imwriteChinese(outPath, mosaic)) {
#else
		if (cv::imwrite(outPath, mosaic)) {
#endif
			std::cout << "已保存: " << outPath << std::endl;
			return true;
		}
		else {
			std::cerr << "保存失败: " << outPath << std::endl;
			return false;
		}
	}

	// Resolve the capture target using the active [Target] spec from config.ini.
	// See common/TargetWindow.h for the matching rules and candidate ranking.
	inline HWND findGameWindow() {
		return target::Find(target::ActiveSpec());
	}

	inline int getMonitorIndexFromWindow(HWND hwnd) {
		if (!hwnd) return 0;

		RECT rc;
		if (!GetWindowRect(hwnd, &rc)) return 0;

		POINT center { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
		HMONITOR hGameMon = MonitorFromPoint(center, MONITOR_DEFAULTTOPRIMARY);
		if (!hGameMon) return 0;

		common_detail::MonitorIndexData data {};
		data.target = hGameMon;

		EnumDisplayMonitors(
			nullptr,
			nullptr,
			common_detail::MonitorIndexEnumProc,
			reinterpret_cast<LPARAM>(&data)
		);

		return data.index >= 0 ? data.index : 0;
	}

	inline bool GetWindowScalingFactor(HWND hwnd, double& out_kw, double& out_kh) {
		out_kw = 1.0;
		out_kh = 1.0;

		if (!hwnd || !IsWindow(hwnd)) {
			return false;
		}

		RECT clientRect = {};
		if (!GetClientRect(hwnd, &clientRect)) {
			return false;
		}

		int clientWidth = clientRect.right - clientRect.left;
		int clientHeight = clientRect.bottom - clientRect.top;

		if (clientWidth <= 0 || clientHeight <= 0) {
			return false;
		}

		out_kw = static_cast<double>(clientWidth) / 1920.0;
		out_kh = static_cast<double>(clientHeight) / 1080.0;

		return true;
	}

	inline std::wstring GetModelPath(const wchar_t* modelName) {
		if (!modelName || modelName[0] == L'\0') {
			return L"";
		}

		wchar_t exePath[MAX_PATH];
		if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
			return L"";
		}

		std::wstring fullPath(exePath);
		size_t lastSlash = fullPath.find_last_of(L"\\/");
		if (lastSlash != std::wstring::npos) {
			return fullPath.substr(0, lastSlash + 1) + modelName;
		}
		else {
			return std::wstring(modelName);
		}
	}

	inline int classNameToDigit(const std::string & s) {
		if (s.size() == 1 && std::isdigit(static_cast<unsigned char>(s[0]))) {
			return s[0] - '0';
		}
		return -1;
	}

}

#endif