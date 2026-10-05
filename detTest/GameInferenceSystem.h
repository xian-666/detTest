#pragma once
#include "ONNXInference.h"
#include "common/IScreenshotProvider.hpp"
#include "Screen/UnifiedScreenCapture.h"
#include <thread>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <string>
#include <tuple>
#include <memory>

class GameInferenceSystem {
private:
    ONNXInference inference;
    std::unique_ptr<ONNXInference> detectionInference;
    std::vector<Region> captureRegions;
    int m_monitorIndex;

    std::vector<Region> m_baseROIs;
    // 存储模型路径，用于推理复位（长时间运行后 ONNX 会话可能损坏）
    std::wstring m_modelPath;
    std::wstring m_detectionModelPath;
    int m_targetImgsz = -1;

    HWND m_gameWindow = nullptr;

    struct ScreenScale {
        double kw, kh;
    }screenScale;

    mutable std::atomic<bool> m_cachedWindowValid { false };
    mutable HWND m_cachedHwnd { nullptr };
    mutable int m_cachedGameWidth { 0 };
    mutable int m_cachedGameHeight { 0 };
    mutable POINT m_cachedClientTopLeft { 0, 0 };
    mutable double m_cachedScaleW { 1.0 };
    mutable double m_cachedScaleH { 1.0 };
    mutable std::vector<Region> m_cachedROIs;
    mutable std::chrono::steady_clock::time_point m_cacheTimestamp;

    std::chrono::steady_clock::time_point m_firstCaptureTime{};

    // 【异步流水线】缓存上次提交的 ROI 用于生成 mosaic
    mutable std::vector<cv::Mat> m_pendingROIMats;
    mutable bool m_hasPendingInference { false };

    const std::vector<cv::Size> SCALED_SIZES = {
        cv::Size(80, 80),  // LsRs
        cv::Size(80, 80),  // LB_1
        cv::Size(80, 80),  // RB_1
        cv::Size(80, 80),  // LT_1
        cv::Size(80, 80),  // RT_1
        cv::Size(80, 80),  // LB_2
        cv::Size(80, 80),  // LT_2
        cv::Size(80, 80)   // RT_2
    };

    const cv::Size MOSAIC_SIZE = cv::Size(640, 480);
    const int GAP = 30;

    static constexpr auto CACHE_TIMEOUT = std::chrono::milliseconds(500);

    void invalidateCacheIfStale() const {
        if (m_cachedWindowValid) {
            auto now = std::chrono::steady_clock::now();
            if (now - m_cacheTimestamp > CACHE_TIMEOUT) {
                m_cachedWindowValid = false;
            }
        }
    }

public:
    explicit GameInferenceSystem(const ORTCHAR_T* modelPath);
    GameInferenceSystem(const ORTCHAR_T* modelPath, const std::vector<Region>& regions);
    GameInferenceSystem(const ORTCHAR_T* modelPath, const ORTCHAR_T* detectionModelPath, const std::vector<Region>& regions, int target_imgsz = -1);
    ~GameInferenceSystem();

    std::pair<std::vector<Detection>, ONNXInference::PerformanceMetrics> runDetectionInference(const cv::Mat& bulletXMosaic, const cv::Mat& bulletYMosaic);

    std::tuple<cv::Mat, cv::Mat, std::vector<Detection>, ONNXInference::PerformanceMetrics> captureBulletROIsWithDetections();

    std::tuple<cv::Mat, cv::Mat> captureBulletROIs();
    
    struct DetectionLayout {
        cv::Rect bulletXRect;
        cv::Rect bulletYRect;
        int combinedWidth;
        int combinedHeight;
    };
    DetectionLayout getDetectionLayout(const cv::Mat& bulletXMat, const cv::Mat& bulletYMat) const;

    bool isGameRunning() const;

    std::tuple<cv::Mat, std::vector<Detection>, ONNXInference::PerformanceMetrics> inferFromGame();

    std::tuple<cv::Mat, std::vector<ClassificationResult>, ONNXInference::PerformanceMetrics> inferFromGameClassification();

    // 【性能优化】接受预捕获帧的分类推理，避免主循环重复截屏。
    // 主循环已为黑屏检查捕获了一帧，直接传给推理管线，消除第2次 CaptureFrame 调用。
    std::tuple<cv::Mat, std::vector<ClassificationResult>, ONNXInference::PerformanceMetrics> inferFromGameClassification(const cv::Mat& capturedFrame);
    
    // 【异步流水线】提交异步推理并立即返回，结果在后台计算
    void submitAsyncClassification(const cv::Mat& capturedFrame);
    
    // 【异步流水线】检查并获取上一帧的推理结果
    bool tryGetAsyncClassificationResult(
        std::vector<ClassificationResult>& results, 
        ONNXInference::PerformanceMetrics& metrics,
        cv::Mat& mosaic);

    std::pair<std::vector<Detection>, ONNXInference::PerformanceMetrics> inferFromMosaic(const cv::Mat& mosaicImg);

    std::pair<ClassificationResult, ONNXInference::PerformanceMetrics> inferFromMosaicClassification(const cv::Mat& mosaicImg);

    void rebindToGameMonitor();

    std::vector<Region> generateDynamicROIs(int W, int H) const;

    std::string getScreenshotProviderName() const;

    void ensureScreenshotInitialized();

    void ensureScreenCaptureInitialized();

    std::vector<cv::Mat> captureCurrentROIs();
    
    cv::Size getDetectionModelInputSize() const;
    
    bool getCachedWindowInfo(
        HWND& hwnd,
        int& gameW,
        int& gameH,
        POINT& clientTopLeft,
        double& scaleW,
        double& scaleH,
        std::vector<Region>& rois
    ) const;
    
    void updateCachedWindowInfo(
        HWND hwnd,
        int gameW,
        int gameH,
        const POINT& clientTopLeft,
        double scaleW,
        double scaleH,
        const std::vector<Region>& rois
    ) const;
    
    void clearCachedWindowInfo() const;  // Clear cached window info on resolution change
    
    const std::vector<cv::Size>& getScaledSizes() const { return SCALED_SIZES; }
    
    // 强制重置推理会话，销毁并重建 ONNX Runtime session
    // 用于从 DirectML 长时间运行的坏状态中恢复
    bool ResetInferenceSession();
    
    // 停止后台异步推理线程
    void stopAsyncInference() {
        inference.stopAsyncInference();
        if (detectionInference) {
            detectionInference->stopAsyncInference();
        }
    }
    
    // 【焦点恢复优化】暂停/恢复推理线程（不销毁）
    void pauseAsyncInference() {
        inference.pauseAsyncInference();
        if (detectionInference) {
            detectionInference->pauseAsyncInference();
        }
    }
    void resumeAsyncInference() {
        inference.resumeAsyncInference();
        if (detectionInference) {
            detectionInference->resumeAsyncInference();
        }
    }
};
