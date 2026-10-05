// Force recompilation
#include "GameInferenceSystem.h"
#include "common/common.hpp"
#include "common/DebugFileLog.h"
#include "common/MosaicUtils.hpp"
#include "common/SystemUtils.hpp"
#include "common/ScreenAspectDetector.hpp"
#include <winrt/base.h>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>

static const std::vector<Region> DEFAULT_BASE_ROIS = {
  Region(1747, 950, 93, 93,  "LsRs"),
  Region(1529, 949, 57, 57,  "LB_1"),
  Region(1644, 949, 57, 57,  "RB_1"),
  Region(1529, 1014, 57, 57, "LT_1"),
  Region(1644, 1014, 57, 57, "RT_1"),
  Region(1497, 981, 57, 57,  "LB_2"),
  Region(1577, 981, 57, 57,  "LT_2"),
  Region(1656, 981, 57, 57,  "RT_2"),
};

GameInferenceSystem::GameInferenceSystem(const ORTCHAR_T* modelPath)
	: m_baseROIs(DEFAULT_BASE_ROIS)
	, inference(modelPath)
	, m_monitorIndex(0)
	, screenScale { 1.0, 1.0 }
{
	// Ensure the externalized ROI adaptation data is loaded before any
	// GetAdaptiveRect call. Load() is idempotent and thread-safe via singleton.
	roi::EnsureConfigLoaded();
}

GameInferenceSystem::GameInferenceSystem(const ORTCHAR_T* modelPath, const std::vector<Region>& regions)
	: inference(modelPath), m_baseROIs(regions) {
	roi::EnsureConfigLoaded();
	rebindToGameMonitor();
}

GameInferenceSystem::GameInferenceSystem(const ORTCHAR_T* modelPath, const ORTCHAR_T* detectionModelPath, const std::vector<Region>& regions, int target_imgsz)
	: inference(modelPath, target_imgsz), m_baseROIs(regions), m_targetImgsz(target_imgsz) {
	roi::EnsureConfigLoaded();
	// 存储模型路径用于后续推理复位
	m_modelPath = modelPath;
	if (detectionModelPath != nullptr) {
		m_detectionModelPath = detectionModelPath;
		detectionInference = std::make_unique<ONNXInference>(detectionModelPath, 64);
		std::cout << "[INIT] Classification model: target_imgsz = " << target_imgsz << std::endl;
		std::cout << "[INIT] Detection model: target_imgsz = 64" << std::endl;
	}
	else {
		std::cout << "[INIT] Classification model: target_imgsz = " << target_imgsz << " (no detection model)" << std::endl;
	}
}

bool GameInferenceSystem::isGameRunning() const {
	HWND gameWnd = common::findGameWindow();
	if (!gameWnd) {
		return false;
	}
	if (!IsWindow(gameWnd) || !IsWindowVisible(gameWnd) || IsIconic(gameWnd)) {
		return false;
	}
	// CRITICAL: Game must be the foreground window. WGC throws uncatchable
	// winrt::hresult_error on its internal threads when capturing a non-foreground window.
	HWND fgWnd = GetForegroundWindow();
	if (!fgWnd || fgWnd != gameWnd) {
		return false;
	}
	return true;
}

std::tuple<cv::Mat, std::vector<Detection>, ONNXInference::PerformanceMetrics>
GameInferenceSystem::inferFromGame()
{
	try {
	HWND gameWnd = common::findGameWindow();
	if (!gameWnd) {
		return {};
	}

	auto& capture = UnifiedScreenCapture::GetInstance();
	if (!capture.UpdateTargetWindow(gameWnd)) {
		return {};
	}

	cv::Mat srcImg;
	if (!capture.CaptureFrame(srcImg) || srcImg.empty()) {
		return {};
	}

	// 【画面恢复检测】检测游戏是否从黑屏/加载状态恢复正常
	static bool wasBlackScreen = true;  // 初始假设为黑屏
	static cv::Mat prevFrame;
	
	cv::Mat grayFrame;
	if (srcImg.channels() == 3) {
		cv::cvtColor(srcImg, grayFrame, cv::COLOR_BGR2GRAY);
	} else {
		grayFrame = srcImg;
	}
	
	double meanBrightness = cv::mean(grayFrame)[0];
	
	// 检测画面变化（使用帧差法）
	double frameDiff = 0.0;
	if (!prevFrame.empty() && prevFrame.size() == grayFrame.size()) {
		cv::Mat diff;
		cv::absdiff(grayFrame, prevFrame, diff);
		frameDiff = cv::mean(diff)[0];
	}
	prevFrame = grayFrame.clone();
	
	// 画面恢复判定：亮度突然变大 或 帧差异常增大
	bool isCurrentlyBlack = (meanBrightness <= 5.0);
	bool screenRecovered = false;
	
	if (wasBlackScreen && !isCurrentlyBlack) {
		// 从黑屏变为有内容
		screenRecovered = true;
		DEBUG_LOG_FMT("[GAME-INFERENCE] Screen recovered from black! Brightness: %.1f -> %.1f", 
			0.0, meanBrightness);
	}
	else if (frameDiff > 30.0 && !isCurrentlyBlack) {
		// 帧差异常大，可能是场景切换完成
		screenRecovered = true;
		DEBUG_LOG_FMT("[GAME-INFERENCE] Large frame change detected (diff=%.1f), possible scene recovery", 
			frameDiff);
	}
	
	if (screenRecovered) {
		// 画面恢复了！强制重置缓存和状态
		clearCachedWindowInfo();
		
		// 重新获取窗口信息（使用最新分辨率）
		RECT clientRect;
		if (GetClientRect(gameWnd, &clientRect)) {
			int newGameW = clientRect.right - clientRect.left;
			int newGameH = clientRect.bottom - clientRect.top;
			
			if (newGameW > 200 && newGameH > 200) {  // 合理的尺寸
				DEBUG_LOG_FMT("[GAME-INFERENCE] Resolution after recovery: %dx%d", newGameW, newGameH);
				
				POINT clientTopLeft = { 0, 0 };
				ClientToScreen(gameWnd, &clientTopLeft);
				
				auto newRois = generateDynamicROIs(newGameW, newGameH);
				
				// 更新缓存
				updateCachedWindowInfo(gameWnd, newGameW, newGameH, clientTopLeft, 
					1.0, 1.0, newRois);
			}
		}
		
		wasBlackScreen = false;
	} else if (isCurrentlyBlack) {
		wasBlackScreen = true;
	}

	RECT clientRect;
	if (!GetClientRect(gameWnd, &clientRect)) {
		return {};
	}
	int gameW = clientRect.right - clientRect.left;
	int gameH = clientRect.bottom - clientRect.top;

	POINT clientTopLeft = { 0, 0 };
	ClientToScreen(gameWnd, &clientTopLeft);

	// Use actual frame dimensions for ROI generation (handles DPI scaling)
	int frameW = srcImg.cols;
	int frameH = srcImg.rows;

	auto rois = generateDynamicROIs(frameW, frameH);
	std::vector<cv::Mat> capturedMats;
	capturedMats.reserve(rois.size());

	for (const auto& r : rois) {
		if (r.x >= 0 && r.y >= 0 && r.x + r.width <= frameW && r.y + r.height <= frameH) {
			cv::Rect roiRect(r.x, r.y, r.width, r.height);
			capturedMats.push_back(srcImg(roiRect).clone());
		}
		else {
			cv::Size placeholderSize = cv::Size(80, 80);
			capturedMats.push_back(cv::Mat(placeholderSize, CV_8UC3, cv::Scalar(0, 0, 0)));
		}
	}

	cv::Mat mosaic = createMosaicFromMats(capturedMats, SCALED_SIZES, MOSAIC_SIZE, GAP);
	if (mosaic.empty()) return {};

	auto [detections, metrics] = inference.runInferenceWithMetrics(mosaic);
	return { mosaic, detections, metrics };
	} catch (const winrt::hresult_error& ex) {
		DEBUG_LOG_FMT("[GAME-INFERENCE] WinRT exception: %s", winrt::to_string(ex.message()).c_str());
		return {};
	} catch (const std::exception& e) {
		DEBUG_LOG_FMT("[GAME-INFERENCE] std exception: %s", e.what());
		return {};
	} catch (...) {
		DEBUG_LOG_FMT("[GAME-INFERENCE] Unknown exception");
		return {};
	}
}

std::pair<std::vector<Detection>, ONNXInference::PerformanceMetrics>
GameInferenceSystem::inferFromMosaic(const cv::Mat& mosaicImg)
{
	return inference.runInferenceWithMetrics(mosaicImg);
}

std::tuple<cv::Mat, std::vector<ClassificationResult>, ONNXInference::PerformanceMetrics>
GameInferenceSystem::inferFromGameClassification()
{
	try {
		auto& capture = UnifiedScreenCapture::GetInstance();

		// Window setup is handled by the main loop's UpdateTargetWindow call
		// Just capture the frame directly
		static cv::Mat srcImg;
		if (!capture.CaptureFrame(srcImg)) {
			DEBUG_LOG_FMT("[GAME-INFERENCE] CaptureFrame failed");
			return {};
		}

		if (srcImg.empty()) {
			DEBUG_LOG_FMT("[GAME-INFERENCE] srcImg is empty");
			return {};
		}

		return inferFromGameClassification(srcImg);
	} catch (const winrt::hresult_error& ex) {
		DEBUG_LOG_FMT("[GAME-INFERENCE] WinRT exception: %s", winrt::to_string(ex.message()).c_str());
		return {};
	} catch (const std::exception& e) {
		DEBUG_LOG_FMT("[GAME-INFERENCE] std exception: %s", e.what());
		return {};
	} catch (...) {
		DEBUG_LOG_FMT("[GAME-INFERENCE] Unknown exception");
		return {};
	}
}

// 【性能优化】接受预捕获帧，避免主循环重复截屏
std::tuple<cv::Mat, std::vector<ClassificationResult>, ONNXInference::PerformanceMetrics>
GameInferenceSystem::inferFromGameClassification(const cv::Mat& capturedFrame)
{
	try {
		if (capturedFrame.empty()) {
			DEBUG_LOG_FMT("[GAME-INFERENCE] capturedFrame is empty");
			return {};
		}

		auto totalStart = std::chrono::steady_clock::now();

		// 使用预捕获帧替代内部 CaptureFrame
		const cv::Mat& srcImg = capturedFrame;

		HWND hwnd;
		int gameW, gameH;
		POINT clientTopLeft;
		double scaleW, scaleH;
		std::vector<Region> rois;

		bool wasCached = getCachedWindowInfo(hwnd, gameW, gameH, clientTopLeft, scaleW, scaleH, rois);
		if (!wasCached) {
			hwnd = common::findGameWindow();
			if (!hwnd) {
				std::cerr << "[ERROR] Game window not found!\n";
				return {};
			}

			RECT clientRect;
			if (!GetClientRect(hwnd, &clientRect)) return {};
			gameW = clientRect.right - clientRect.left;
			gameH = clientRect.bottom - clientRect.top;

			clientTopLeft = { 0, 0 };
			ClientToScreen(hwnd, &clientTopLeft);

			rois = generateDynamicROIs(gameW, gameH);
			if (rois.empty()) return {};

			double kw = 1.0, kh = 1.0;
			common::GetWindowScalingFactor(hwnd, kw, kh);
			updateCachedWindowInfo(hwnd, gameW, gameH, clientTopLeft, kw, kh, rois);
		}

		std::vector<cv::Mat> roiMats;
		roiMats.reserve(rois.size());

		// 【RESOLUTION CHANGE FIX】Use actual frame dimensions for boundary check
		int frameW = srcImg.cols;
		int frameH = srcImg.rows;

		// [DEBUG] Log ROI source and aspect category for diagnosing 21:9 issues.
		{
			std::string aspectCat = screen_aspect::GetAspectCategory();
			double physW = screen_aspect::GetPhysicalWidth();
			double physH = screen_aspect::GetPhysicalHeight();
			DEBUG_LOG_FMT("[ROI-SRC] aspect=%s phys=%.0fx%.0f cached=%d rois=%zu",
				aspectCat.c_str(), physW, physH,
				(int)wasCached, rois.size());
			if (!rois.empty()) {
				std::string names;
				for (size_t i = 0; i < rois.size(); ++i) {
					if (i > 0) names += ", ";
					names += rois[i].name;
				}
				DEBUG_LOG_FMT("[ROI-SRC] order: %s", names.c_str());
			}
		}

		// Only re-derive ROIs from frameW/H when they EXACTLY match a registered
		// non-16:9 resolution (4:3 / 5:4 / 16:10). Otherwise keep the cached
		// gameW/gameH-derived ROIs to preserve byte-exact precision for
		// previously-tuned resolutions. (e.g. DXGI may return the screen output
		// size 1920x1080 for a 1024x768 window; in that case we keep the
		// 1024x768 ROIs and let the boundary check below drop out-of-range ones.)
		{
			const auto& cfg = roi::ROIAdaptationConfig::GetInstance();
			roi::ResidualEntry dummy;
			bool isRegistered = cfg.GetResidual(frameW, frameH, std::string("LT_1"), dummy)
		                  || cfg.GetResidual(frameW, frameH, std::string("LsRs"), dummy);
			bool wasGameSize = (frameW == gameW && frameH == gameH);
			if (!wasGameSize && isRegistered) {
				DEBUG_LOG_FMT("[GAME-INFERENCE] Frame size (%dx%d) is a registered resolution, regenerating ROIs",
					frameW, frameH);
				rois = generateDynamicROIs(frameW, frameH);
				if (rois.empty()) return {};
				gameW = frameW;
				gameH = frameH;
				updateCachedWindowInfo(hwnd, gameW, gameH, clientTopLeft, scaleW, scaleH, rois);
			}
		}

		for (size_t i = 0; i < rois.size(); ++i) {
			const auto& r = rois[i];

			// 【FIX】Check against ACTUAL frame dimensions, not window dimensions
			if (r.x >= 0 && r.y >= 0 && 
				r.x + r.width <= frameW && r.y + r.height <= frameH &&
				r.width > 0 && r.height > 0) {
				
				cv::Rect roiRect(r.x, r.y, r.width, r.height);
				
				// Additional safety: clamp to frame boundaries
				roiRect.x = std::max(0, roiRect.x);
				roiRect.y = std::max(0, roiRect.y);
				roiRect.width = std::min(roiRect.width, frameW - roiRect.x);
				roiRect.height = std::min(roiRect.height, frameH - roiRect.y);
				
				if (roiRect.width > 0 && roiRect.height > 0) {
					cv::Mat roi = srcImg(roiRect).clone();
					roiMats.push_back(roi);
				} else {
					cv::Size placeholderSize = (i < SCALED_SIZES.size()) ? SCALED_SIZES[i] : SCALED_SIZES.front();
					cv::Mat placeholder(placeholderSize, CV_8UC3, cv::Scalar(0, 0, 0));
					roiMats.push_back(placeholder);
				}
			}
			else {
				cv::Size placeholderSize = (i < SCALED_SIZES.size()) ? SCALED_SIZES[i] : SCALED_SIZES.front();
				cv::Mat placeholder(placeholderSize, CV_8UC3, cv::Scalar(0, 0, 0));
				roiMats.push_back(placeholder);
			}
		}

		auto inferenceStart = std::chrono::steady_clock::now();

	auto [batchResults, batchMetrics] = inference.runBatchClassificationWithMetrics(roiMats);

		auto inferenceEnd = std::chrono::steady_clock::now();

		std::vector<ClassificationResult> roiResults = std::move(batchResults);

		// [DEBUG] Log ROI→classification mapping for diagnosing index mismatches.
		// Output goes to Log/debug_*.log file only, not console.
		{
			std::ostringstream dbg;
			dbg << "[ROI-MAP] frame=" << frameW << "x" << frameH
			    << " game=" << gameW << "x" << gameH
			    << " rois=" << rois.size()
			    << " results=" << roiResults.size();
			for (size_t i = 0; i < rois.size() && i < roiResults.size(); ++i) {
				dbg << "\n  [" << i << "] " << rois[i].name
				    << " (" << rois[i].x << "," << rois[i].y
				    << " " << rois[i].width << "x" << rois[i].height << ")"
				    << " => " << roiResults[i].class_name
				    << " " << std::fixed << std::setprecision(3) << roiResults[i].confidence;
			}
			DEBUG_LOG_FMT("%s", dbg.str().c_str());
		}

		// [Task 4] Classification consistency check — detect frame-to-frame jitter.
		{
			static std::vector<std::string> s_lastNames;
			if (s_lastNames.size() == roiResults.size()) {
				int matchCount = 0;
				for (size_t i = 0; i < roiResults.size(); ++i) {
					if (roiResults[i].class_name == s_lastNames[i]) matchCount++;
				}
				if (matchCount < static_cast<int>(roiResults.size())) {
					DEBUG_LOG_FMT("[ROI-CONSIST] %d/%zu classes changed from last frame",
						static_cast<int>(roiResults.size()) - matchCount, roiResults.size());
				}
			}
			s_lastNames.clear();
			for (auto& r : roiResults) s_lastNames.push_back(r.class_name);
		}

		ONNXInference::PerformanceMetrics avgMetrics = batchMetrics;

		if (!roiResults.empty()) {
			avgMetrics.preprocess_time_ms = batchMetrics.preprocess_time_ms / roiResults.size();
			avgMetrics.inference_time_ms = batchMetrics.inference_time_ms / roiResults.size();
			avgMetrics.postprocess_time_ms = batchMetrics.postprocess_time_ms / roiResults.size();
		}

		auto totalEnd = std::chrono::steady_clock::now();
		auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart).count();

		avgMetrics.total_time_ms = totalDuration;

		cv::Mat mosaic(MOSAIC_SIZE, CV_8UC3, cv::Scalar(0, 0, 0));
		int currentX = 0;
		int currentY = GAP;
		int rowHeight = 0;

		for (size_t i = 0; i < roiMats.size(); ++i) {
			if (roiMats[i].empty()) continue;

			cv::Size targetSize = cv::Size(80, 80);  // Fixed size for consistent scaling
			cv::Mat resized;
			cv::resize(roiMats[i], resized, targetSize);

			if (currentX + resized.cols > MOSAIC_SIZE.width) {
				currentX = 0;
				currentY += rowHeight;
				rowHeight = 0;
			}

			if (currentY + resized.rows + 20 > MOSAIC_SIZE.height) break;

			cv::Rect roi(currentX, currentY, resized.cols, resized.rows);
			if (roi.x + roi.width <= mosaic.cols && roi.y + roi.height <= mosaic.rows) {
				resized.copyTo(mosaic(roi));
			}

			currentX += resized.cols;
			rowHeight = (std::max)(rowHeight, resized.rows);
		}

		return { mosaic, roiResults, avgMetrics };
	} catch (const winrt::hresult_error& ex) {
		DEBUG_LOG_FMT("[GAME-INFERENCE] WinRT exception: %s", winrt::to_string(ex.message()).c_str());
		return {};
	} catch (const std::exception& e) {
		DEBUG_LOG_FMT("[GAME-INFERENCE] std exception: %s", e.what());
		return {};
	} catch (...) {
		DEBUG_LOG_FMT("[GAME-INFERENCE] Unknown exception");
		return {};
	}
}

std::pair<ClassificationResult, ONNXInference::PerformanceMetrics>
GameInferenceSystem::inferFromMosaicClassification(const cv::Mat& mosaicImg)
{
	return inference.runClassificationWithMetrics(mosaicImg);
}

std::pair<std::vector<Detection>, ONNXInference::PerformanceMetrics>
GameInferenceSystem::runDetectionInference(const cv::Mat& bulletXMosaic, const cv::Mat& bulletYMosaic)
{
	std::vector<Detection> allDetections;
	ONNXInference::PerformanceMetrics totalMetrics { };

	if (!detectionInference) {
		std::cerr << "[ERROR] Detection model not initialized!" << std::endl;
		return { allDetections, totalMetrics };
	}

	auto totalStart = std::chrono::high_resolution_clock::now();

	if (!bulletXMosaic.empty()) {
		auto [detectionsX, metricsX] = detectionInference->runInferenceWithMetrics(bulletXMosaic, 64);

		for (auto& det : detectionsX) {
			Detection adjustedDet = det;
			adjustedDet.roi_index = 0;
			allDetections.push_back(adjustedDet);
		}

		totalMetrics.inference_time_ms += metricsX.inference_time_ms;
		totalMetrics.preprocess_time_ms += metricsX.preprocess_time_ms;
	}

	if (!bulletYMosaic.empty()) {
		auto [detectionsY, metricsY] = detectionInference->runInferenceWithMetrics(bulletYMosaic, 64);

		for (auto& det : detectionsY) {
			Detection adjustedDet = det;
			adjustedDet.roi_index = 1;
			allDetections.push_back(adjustedDet);
		}

		totalMetrics.inference_time_ms += metricsY.inference_time_ms;
		totalMetrics.preprocess_time_ms += metricsY.preprocess_time_ms;
	}

	auto totalEnd = std::chrono::high_resolution_clock::now();
	totalMetrics.total_time_ms = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

	return { allDetections, totalMetrics };
}

std::tuple<cv::Mat, cv::Mat, std::vector<Detection>, ONNXInference::PerformanceMetrics>
GameInferenceSystem::captureBulletROIsWithDetections()
{
	auto [bulletXMat, bulletYMat] = captureBulletROIs();

	if (bulletXMat.empty() || bulletYMat.empty()) {
		return { cv::Mat(), cv::Mat(), {}, {} };
	}

	auto [detections, metrics] = runDetectionInference(bulletXMat, bulletYMat);

	for (const auto& det : detections) {
		std::string className = (det.class_id >= 0 && det.class_id <= 9 && det.class_id < static_cast<int>(DETECTION_LABELS.size()))
			? DETECTION_LABELS[det.class_id] : "unknown";

		cv::Scalar boxColor(0, 255, 0);

		cv::Mat& targetMat = (det.roi_index == 0) ? bulletXMat : bulletYMat;
		cv::rectangle(targetMat, det.box, boxColor, 2);

		std::string detLabel = className + " " + std::to_string(static_cast<int>(det.confidence * 100)) + "%";
		int baseline = 0;
		cv::Size labelSize = cv::getTextSize(detLabel, cv::FONT_HERSHEY_SIMPLEX, 0.4, 1, &baseline);
		cv::Point labelTop(det.box.x, det.box.y - labelSize.height - 2);
		cv::rectangle(targetMat, cv::Rect(labelTop, cv::Size(labelSize.width, labelSize.height + baseline)),
			cv::Scalar(0, 255, 0), cv::FILLED);
		cv::putText(targetMat, detLabel, cv::Point(det.box.x, det.box.y - 2),
			cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 0), 1);
	}

	return { bulletXMat, bulletYMat, detections, metrics };
}

std::tuple<cv::Mat, cv::Mat>
GameInferenceSystem::captureBulletROIs()
{
	HWND hwnd;
	int gameW, gameH;
	POINT clientTopLeft;
	double scaleW, scaleH;
	std::vector<Region> rois;

	if (!getCachedWindowInfo(hwnd, gameW, gameH, clientTopLeft, scaleW, scaleH, rois)) {
		hwnd = common::findGameWindow();
		if (!hwnd) return { cv::Mat(), cv::Mat() };

		RECT clientRect;
		if (!GetClientRect(hwnd, &clientRect)) return { cv::Mat(), cv::Mat() };
		gameW = clientRect.right - clientRect.left;
		gameH = clientRect.bottom - clientRect.top;

		clientTopLeft = { 0, 0 };
		ClientToScreen(hwnd, &clientTopLeft);

		rois = generateDynamicROIs(gameW, gameH);
		if (rois.empty()) return { cv::Mat(), cv::Mat() };

		double kw = 1.0, kh = 1.0;
		common::GetWindowScalingFactor(hwnd, kw, kh);
		updateCachedWindowInfo(hwnd, gameW, gameH, clientTopLeft, kw, kh, rois);
	}

	auto& capture = UnifiedScreenCapture::GetInstance();
	if (!capture.UpdateTargetWindow(hwnd)) {
		return { cv::Mat(), cv::Mat() };
	}

	static cv::Mat srcImg;
	if (!capture.CaptureFrame(srcImg) || srcImg.empty()) {
		return { cv::Mat(), cv::Mat() };
	}

	cv::Mat bulletXMat, bulletYMat;

	for (const auto& r : rois) {
		if (r.name == "BulletX") {
			if (r.x >= 0 && r.y >= 0 && r.x + r.width <= gameW && r.y + r.height <= gameH) {
				cv::Rect roiRect(r.x, r.y, r.width, r.height);
				bulletXMat = srcImg(roiRect).clone();
			}
		}
		else if (r.name == "BulletY") {
			if (r.x >= 0 && r.y >= 0 && r.x + r.width <= gameW && r.y + r.height <= gameH) {
				cv::Rect roiRect(r.x, r.y, r.width, r.height);
				bulletYMat = srcImg(roiRect).clone();
			}
		}
	}

	return { bulletXMat, bulletYMat };
}

void GameInferenceSystem::rebindToGameMonitor() {
	HWND gameWnd = common::findGameWindow();

	if (!gameWnd) {
		return;
	}

	if (IsIconic(gameWnd)) {
		return;
	}

	if (m_gameWindow == gameWnd) {
		return;
	}

	m_gameWindow = gameWnd;
	clearCachedWindowInfo();

	auto& capture = UnifiedScreenCapture::GetInstance();
	// Use WGC-forced init to avoid DXGI ACCESS_LOST during window transition
	if (!capture.InitializeWGCForced(gameWnd, true)) {
		DEBUG_LOG_FMT("[GAME-INFERENCE] InitializeWGCForced failed in rebindToGameMonitor");
	}
	else {
		DEBUG_LOG_FMT("[GAME-INFERENCE] Capture rebind with WGC forced");
	}
}

void GameInferenceSystem::ensureScreenCaptureInitialized()
{
	auto& capture = UnifiedScreenCapture::GetInstance();
	HWND gameWnd = common::findGameWindow();

	if (!gameWnd) {
		return;
	}

	if (IsIconic(gameWnd)) {
		return;
	}

	if (m_gameWindow == gameWnd) {
		return;
	}

	m_gameWindow = gameWnd;
	clearCachedWindowInfo();

	// Record the first capture initialization time
	m_firstCaptureTime = std::chrono::steady_clock::now();

	// During startup grace period, force WGC to avoid DXGI ACCESS_LOST
	// when the game window first appears.
	if (!capture.InitializeWGCForced(gameWnd, true)) {
		DEBUG_LOG_FMT("[GAME-INFERENCE] InitializeWGCForced failed");
	}
	else {
		DEBUG_LOG_FMT("[GAME-INFERENCE] Capture initialized with startup grace period (WGC forced)");
	}
}

std::vector<Region> GameInferenceSystem::generateDynamicROIs(int W, int H) const {
	if (m_baseROIs.empty()) {
		return {};
	}

	// v3.0: Dynamic aspect ratio calculation
	// The aspect category is now calculated dynamically from render resolution (W/H)
	// inside GetAdaptiveRect, eliminating the need for physical screen detection.
	// Pass empty string to trigger dynamic calculation based on W/H ratio.
	std::string aspectCategory = "";  // Empty to use dynamic calculation

	std::vector<Region> result;
	result.reserve(m_baseROIs.size());

	for (const auto& r : m_baseROIs) {
		cv::Rect rect = roi::GetAdaptiveRect(r.name, W, H, aspectCategory);
		result.emplace_back(rect.x, rect.y, rect.width, rect.height, r.name);
	}

	return result;
}

cv::Size GameInferenceSystem::getDetectionModelInputSize() const
{
	if (detectionInference) {
		return detectionInference->getInputSize();
	}
	return cv::Size(0, 0);
}

GameInferenceSystem::DetectionLayout
GameInferenceSystem::getDetectionLayout(const cv::Mat& bulletXMat, const cv::Mat& bulletYMat) const
{
	DetectionLayout layout;

	if (bulletXMat.empty() || bulletYMat.empty()) {
		layout = { cv::Rect(), cv::Rect(), 0, 0 };
		return layout;
	}

	layout.combinedWidth = bulletXMat.cols;
	layout.combinedHeight = bulletXMat.rows + bulletYMat.rows;

	layout.bulletXRect = cv::Rect(0, 0, bulletXMat.cols, bulletXMat.rows);

	layout.bulletYRect = cv::Rect(0, bulletXMat.rows, bulletYMat.cols, bulletYMat.rows);

	return layout;
}

std::string GameInferenceSystem::getScreenshotProviderName() const {
	auto& capture = UnifiedScreenCapture::GetInstance();
	switch (capture.GetCurrentMethod()) {
		case UnifiedScreenCapture::CaptureMethod::WGC: return "WGC";
		case UnifiedScreenCapture::CaptureMethod::DXGI: return "DXGI";
		case UnifiedScreenCapture::CaptureMethod::GDI: return "GDI";
		default: return "None";
	}
}

std::vector<cv::Mat> GameInferenceSystem::captureCurrentROIs() {
	HWND hwnd = common::findGameWindow();
	if (!hwnd) {
		std::cerr << "[GameInferenceSystem] Game window not found!" << std::endl;
		return {};
	}

	RECT clientRect;
	if (!GetClientRect(hwnd, &clientRect)) {
		std::cerr << "[GameInferenceSystem] Failed to get client rect!" << std::endl;
		return {};
	}

	int gameW = clientRect.right - clientRect.left;
	int gameH = clientRect.bottom - clientRect.top;

	POINT topLeft = { 0, 0 };
	ClientToScreen(hwnd, &topLeft);

	std::vector<Region> rois = generateDynamicROIs(gameW, gameH);
	if (rois.empty()) {
		std::cerr << "[GameInferenceSystem] Failed to generate dynamic ROIs!" << std::endl;
		return {};
	}

	auto& capture = UnifiedScreenCapture::GetInstance();
	if (!capture.UpdateTargetWindow(hwnd)) {
		return {};
	}

	static cv::Mat srcImg;
	if (!capture.CaptureFrame(srcImg) || srcImg.empty()) {
		std::cerr << "[GameInferenceSystem] Failed to capture game window!" << std::endl;
		return {};
	}

	std::vector<cv::Mat> capturedROIs;
	capturedROIs.reserve(rois.size());

	for (const auto& r : rois) {
		if (r.x >= 0 && r.y >= 0 && r.x + r.width <= gameW && r.y + r.height <= gameH) {
			cv::Rect roiRect(r.x, r.y, r.width, r.height);
			cv::Mat roi = srcImg(roiRect).clone();
			capturedROIs.push_back(roi);
		}
		else {
			capturedROIs.push_back(cv::Mat());
		}
	}

	return capturedROIs;
}

bool GameInferenceSystem::getCachedWindowInfo(
	HWND& hwnd,
	int& gameW,
	int& gameH,
	POINT& clientTopLeft,
	double& scaleW,
	double& scaleH,
	std::vector<Region>& rois
) const {
	invalidateCacheIfStale();

	if (!m_cachedWindowValid) {
		return false;
	}

	hwnd = m_cachedHwnd;
	gameW = m_cachedGameWidth;
	gameH = m_cachedGameHeight;
	clientTopLeft = m_cachedClientTopLeft;
	scaleW = m_cachedScaleW;
	scaleH = m_cachedScaleH;
	rois = m_cachedROIs;

	return true;
}

void GameInferenceSystem::updateCachedWindowInfo(
	HWND hwnd,
	int gameW,
	int gameH,
	const POINT& clientTopLeft,
	double scaleW,
	double scaleH,
	const std::vector<Region>& rois
) const {
	m_cachedHwnd = hwnd;
	m_cachedGameWidth = gameW;
	m_cachedGameHeight = gameH;
	m_cachedClientTopLeft = clientTopLeft;
	m_cachedScaleW = scaleW;
	m_cachedScaleH = scaleH;
	m_cachedROIs = rois;
	m_cachedWindowValid = true;
	m_cacheTimestamp = std::chrono::steady_clock::now();
}

void GameInferenceSystem::clearCachedWindowInfo() const {
	DEBUG_LOG_FMT("[GAME-INFERENCE] Clearing cached window info (resolution change)");
	m_cachedWindowValid = false;
	m_cachedHwnd = nullptr;
	m_cachedGameWidth = 0;
	m_cachedGameHeight = 0;
	m_cachedClientTopLeft = { 0, 0 };
	m_cachedScaleW = 1.0;
	m_cachedScaleH = 1.0;
	m_cachedROIs.clear();
}

bool GameInferenceSystem::ResetInferenceSession() {
	DEBUG_LOG_FMT("[GAME-INFERENCE] Resetting inference session (model=%ls)", m_modelPath.c_str());
	
	// 委托给 ONNXInference 的 ReloadModel，销毁并重建推理会话
	bool success = inference.ReloadModel();
	
	if (success) {
		DEBUG_LOG_FMT("[GAME-INFERENCE] Inference session reset successfully");
		
		// 同时清除缓存的窗口信息，确保下一帧重新生成 ROI
		clearCachedWindowInfo();
	} else {
		DEBUG_LOG_FMT("[GAME-INFERENCE] Inference session reset FAILED");
	}
	
	return success;
}

void GameInferenceSystem::ensureScreenshotInitialized() {
	ensureScreenCaptureInitialized();
}

GameInferenceSystem::~GameInferenceSystem() {
	// ONNXInference 的析构函数会自动停止推理线程
}

void GameInferenceSystem::submitAsyncClassification(const cv::Mat& capturedFrame) {
	try {
		if (capturedFrame.empty()) {
			return;
		}

		const cv::Mat& srcImg = capturedFrame;

		HWND hwnd;
		int gameW, gameH;
		POINT clientTopLeft;
		double scaleW, scaleH;
		std::vector<Region> rois;

		bool wasCached = getCachedWindowInfo(hwnd, gameW, gameH, clientTopLeft, scaleW, scaleH, rois);
		if (!wasCached) {
			hwnd = common::findGameWindow();
			if (!hwnd) {
				return;
			}

			RECT clientRect;
			if (!GetClientRect(hwnd, &clientRect)) return;
			gameW = clientRect.right - clientRect.left;
			gameH = clientRect.bottom - clientRect.top;

			clientTopLeft = { 0, 0 };
			ClientToScreen(hwnd, &clientTopLeft);

			rois = generateDynamicROIs(gameW, gameH);
			if (rois.empty()) return;

			double kw = 1.0, kh = 1.0;
			common::GetWindowScalingFactor(hwnd, kw, kh);
			updateCachedWindowInfo(hwnd, gameW, gameH, clientTopLeft, kw, kh, rois);
		}

		std::vector<cv::Mat> roiMats;
		roiMats.reserve(rois.size());

		int frameW = srcImg.cols;
		int frameH = srcImg.rows;

		{
			const auto& cfg = roi::ROIAdaptationConfig::GetInstance();
			roi::ResidualEntry dummy;
			bool isRegistered = cfg.GetResidual(frameW, frameH, std::string("LT_1"), dummy)
		                  || cfg.GetResidual(frameW, frameH, std::string("LsRs"), dummy);
			bool wasGameSize = (frameW == gameW && frameH == gameH);
			if (!wasGameSize && isRegistered) {
				rois = generateDynamicROIs(frameW, frameH);
				if (rois.empty()) return;
				gameW = frameW;
				gameH = frameH;
				updateCachedWindowInfo(hwnd, gameW, gameH, clientTopLeft, scaleW, scaleH, rois);
			}
		}

		for (size_t i = 0; i < rois.size(); ++i) {
			const auto& r = rois[i];

			if (r.x >= 0 && r.y >= 0 && 
				r.x + r.width <= frameW && r.y + r.height <= frameH &&
				r.width > 0 && r.height > 0) {
				
				cv::Rect roiRect(r.x, r.y, r.width, r.height);
				
				roiRect.x = std::max(0, roiRect.x);
				roiRect.y = std::max(0, roiRect.y);
				roiRect.width = std::min(roiRect.width, frameW - roiRect.x);
				roiRect.height = std::min(roiRect.height, frameH - roiRect.y);
				
				if (roiRect.width > 0 && roiRect.height > 0) {
					cv::Mat roi = srcImg(roiRect).clone();
					roiMats.push_back(roi);
				} else {
					cv::Size placeholderSize = (i < SCALED_SIZES.size()) ? SCALED_SIZES[i] : SCALED_SIZES.front();
					cv::Mat placeholder(placeholderSize, CV_8UC3, cv::Scalar(0, 0, 0));
					roiMats.push_back(placeholder);
				}
			}
			else {
				cv::Size placeholderSize = (i < SCALED_SIZES.size()) ? SCALED_SIZES[i] : SCALED_SIZES.front();
				cv::Mat placeholder(placeholderSize, CV_8UC3, cv::Scalar(0, 0, 0));
				roiMats.push_back(placeholder);
			}
		}

		m_pendingROIMats = std::move(roiMats);
		inference.submitAsyncClassification(m_pendingROIMats);
		m_hasPendingInference = true;

	} catch (...) {}
}

bool GameInferenceSystem::tryGetAsyncClassificationResult(
	std::vector<ClassificationResult>& results,
	ONNXInference::PerformanceMetrics& metrics,
	cv::Mat& mosaic) {

	if (!m_hasPendingInference) {
		return false;
	}

	try {
		bool ready = inference.tryGetAsyncClassificationResult(results, metrics);
		if (!ready) {
			return false;
		}

		if (!results.empty()) {
			metrics.preprocess_time_ms = metrics.preprocess_time_ms / results.size();
			metrics.inference_time_ms = metrics.inference_time_ms / results.size();
			metrics.postprocess_time_ms = metrics.postprocess_time_ms / results.size();
		}

		mosaic = cv::Mat(MOSAIC_SIZE, CV_8UC3, cv::Scalar(0, 0, 0));
		int currentX = 0;
		int currentY = GAP;
		int rowHeight = 0;

		for (size_t i = 0; i < m_pendingROIMats.size(); ++i) {
			if (m_pendingROIMats[i].empty()) continue;

			cv::Size targetSize = cv::Size(80, 80);
			cv::Mat resized;
			cv::resize(m_pendingROIMats[i], resized, targetSize);

			if (currentX + resized.cols > MOSAIC_SIZE.width) {
				currentX = 0;
				currentY += rowHeight;
				rowHeight = 0;
			}

			if (currentY + resized.rows + 20 > MOSAIC_SIZE.height) break;

			cv::Rect roi(currentX, currentY, resized.cols, resized.rows);
			if (roi.x + roi.width <= mosaic.cols && roi.y + roi.height <= mosaic.rows) {
				resized.copyTo(mosaic(roi));
			}

			currentX += resized.cols;
			rowHeight = (std::max)(rowHeight, resized.rows);
		}

		m_hasPendingInference = false;
		m_pendingROIMats.clear();

		return true;

	} catch (...) {
		m_hasPendingInference = false;
		m_pendingROIMats.clear();
		return false;
	}
}
