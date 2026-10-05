#pragma once
#include <opencv2/opencv.hpp>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <queue>

struct LetterBoxParams {
    int src_width, src_height;
    int padded_w, padded_h;
    int offset_x, offset_y;
    float scale;      // X direction scale
    float scale_y;    // Y direction scale (for non-square resize)
};

struct Detection {
    cv::Rect box;
    float confidence;
    int class_id;
    std::string class_name;
    int roi_index = 0;  // 0 = BulletX, 1 = BulletY
};

struct ClassificationResult {
    int class_id;
    float confidence;
    std::string class_name;
};

class ONNXInference {
public:
    struct PerformanceMetrics {
        double preprocess_time_ms;
        double inference_time_ms;
        double postprocess_time_ms;
        double total_time_ms;
        size_t input_width;
        size_t input_height;
    };

private:
    Ort::Env env;
    std::unique_ptr<Ort::Session> session;
    Ort::SessionOptions sessionOptions;

    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
    std::vector<Ort::AllocatedStringPtr> input_names_ptr;
    std::vector<Ort::AllocatedStringPtr> output_names_ptr;
    std::vector<const char*> input_node_names;
    std::vector<const char*> output_node_names;

    size_t input_height, input_width, input_channels;
    LetterBoxParams m_letterbox;
    
    int m_target_imgsz;  // -1 表示从模型元数据读取，否则强制使用此尺寸
    
    // 存储模型路径，用于推理会话重载（长时间运行后 DirectML 可能进入坏状态）
    std::wstring m_modelPath;
    
    // 【线程安全】推理与 ReloadModel 共享 session 需要互斥锁保护
    mutable std::mutex m_inferenceMutex;
    // 【线程安全】ReloadModel 本身需要互斥，避免并发重置
    mutable std::mutex m_reloadMutex;

    mutable std::vector<float> m_inputTensorValues;
    mutable bool m_tensorValuesInitialized = false;

    // 【性能优化】预分配批处理 tensor 缓冲区，避免每帧重复堆分配
    mutable std::vector<float> m_batchTensorValues;
    mutable size_t m_batchTensorCapacity = 0;

#ifdef MODEL_ENCRYPT
    std::string model_path_str;
    bool LoadEncryptedModel(const std::string& model_path);
#endif

    // 【异步流水线】推理线程相关
    std::unique_ptr<std::thread> m_inferenceThread;
    std::atomic<bool> m_inferenceThreadRunning{ false };
    
    struct InferenceRequest {
        std::vector<cv::Mat> images;
    };
    
    std::queue<std::unique_ptr<InferenceRequest>> m_requestQueue;
    std::mutex m_requestQueueMutex;
    std::condition_variable m_requestCV;
    
    // 【异步流水线】结果缓存（后台线程写入，主线程读取）
    mutable std::mutex m_resultMutex;
    mutable std::vector<ClassificationResult> m_lastAsyncResults;
    mutable PerformanceMetrics m_lastAsyncMetrics;
    mutable std::atomic<bool> m_hasAsyncResult{ false };
    mutable std::atomic<size_t> m_processedTaskCount{ 0 };
    
    // 【焦点恢复优化】异步推理暂停标志（不销毁线程，仅跳过推理）
    std::atomic<bool> m_asyncPaused{ false };
    
    void inferenceThreadLoop();

public:
    explicit ONNXInference(const ORTCHAR_T* modelPath, int target_imgsz = -1);
    ~ONNXInference();

    cv::Mat preprocessImage(const cv::Mat& image);

    std::vector<Detection> runInference(const cv::Mat& image, int targetSize = -1);
    ClassificationResult runClassification(const cv::Mat& image);

    std::pair<std::vector<Detection>, PerformanceMetrics> runInferenceWithMetrics(const cv::Mat& image, int targetSize = -1);
    std::pair<ClassificationResult, PerformanceMetrics> runClassificationWithMetrics(const cv::Mat& image);
    
    // 批处理分类推理 - 一次性处理多个 ROI（同步版本）
    std::pair<std::vector<ClassificationResult>, PerformanceMetrics> runBatchClassificationWithMetrics(
        const std::vector<cv::Mat>& images);

    // 【异步流水线】预处理并提交异步推理（任务入队，立即返回）
    void submitAsyncClassification(const std::vector<cv::Mat>& images);
    
    // 【异步流水线】非阻塞获取最新异步推理结果
    bool tryGetAsyncClassificationResult(std::vector<ClassificationResult>& results, PerformanceMetrics& metrics);
    
    // 【异步流水线】停止推理线程
    void stopAsyncInference();
    
    // 【焦点恢复优化】暂停推理线程（不销毁，仅跳过推理）
    // 用于焦点切出时停止推理，切回时恢复，避免反复 stop/start 导致死锁
    void pauseAsyncInference();
    void resumeAsyncInference();
    bool isAsyncPaused() const { return m_asyncPaused.load(); }

    cv::Size getInputSize() const;
    void initializeTensorBuffer() const;
    
    // 销毁并重建 ONNX Runtime 推理会话，用于从 DirectML 坏状态中恢复
    bool ReloadModel();
};

