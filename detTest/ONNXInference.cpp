#include "ONNXInference.h"
#include "common/common.hpp"
#include "common/DebugFileLog.h"
#include "Component/Log/debugLog.h"
#include "component/AES.h"
#include "../Component/BaseUtility.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <onnxruntime_cxx_api.h>
#include <numeric>
#include <chrono>
#include <atomic>
#ifdef _WIN32
#include "dml_provider_factory.h"
#endif

#define CONF_THRESH 0.6f

const int NUM_CLASSES = DETECTION_LABELS.size();
const int STRIDE = 6;  // YOLOv8 format: x1, y1, x2, y2, conf, class

ONNXInference::ONNXInference(const ORTCHAR_T* modelPath, int target_imgsz)
	: env(ORT_LOGGING_LEVEL_WARNING, "ONNXInference")
	, m_target_imgsz(target_imgsz)
{
	// 保存模型路径，用于会话重置
#ifdef _WIN32
	m_modelPath = modelPath;
#else
	// 非 Windows 下从 ORTCHAR_T 转换
#endif
#ifdef _WIN32
	// [Task 2] Check ONNX_FORCE_CPU environment variable for diagnostic/fix.
	char forceCPUBuf[8] = {};
	bool forceCPU = (GetEnvironmentVariableA("ONNX_FORCE_CPU", forceCPUBuf, sizeof(forceCPUBuf)) > 0)
	             && (forceCPUBuf[0] == '1' || forceCPUBuf[0] == 'Y' || forceCPUBuf[0] == 'y');

	if (!forceCPU) {
		// [Task 1] Log DirectML provider status for cross-machine diagnosis.
		auto dml_status = OrtSessionOptionsAppendExecutionProvider_DML(sessionOptions, 0);
		if (dml_status != nullptr) {
			Ort::GetApi().ReleaseStatus(dml_status);
			DEBUG_LOG_FMT("[ONNXInference] DirectML FAILED, falling back to CPU");
		} else {
			DEBUG_LOG_FMT("[ONNXInference] DirectML initialized successfully");
		}
	} else {
		DEBUG_LOG_FMT("[ONNXInference] ONNX_FORCE_CPU=1 set, skipping DirectML");
	}
#endif

	// 性能优化配置
	sessionOptions.SetIntraOpNumThreads(1);  // 单线程避免线程切换开销
	sessionOptions.SetInterOpNumThreads(1);  // 单线程避免线程切换开销
	sessionOptions.SetExecutionMode(ORT_SEQUENTIAL);  // 顺序执行模式
	sessionOptions.SetGraphOptimizationLevel(ORT_ENABLE_EXTENDED);  // 启用所有图优化

	// 转换模型路径，用于加密加载
#ifdef MODEL_ENCRYPT
#ifdef _WIN32
	std::wstring wstr(modelPath);
	std::string model_path_str = BaseUtility::WString2String(wstr);
#else
	std::string model_path_str(modelPath);
#endif
	MyDbgStr("gsdet: 尝试加载加密模型: %s", model_path_str.c_str());

	if (!LoadEncryptedModel(model_path_str)) {
		throw std::runtime_error("Failed to load and decrypt model!");
	}
#else
	session = std::make_unique<Ort::Session>(env, modelPath, sessionOptions);
#endif
	auto input_info = session->GetInputTypeInfo(0);
	auto output_info = session->GetOutputTypeInfo(0);
	auto input_tensor_info = input_info.GetTensorTypeAndShapeInfo();
	auto output_tensor_info = output_info.GetTensorTypeAndShapeInfo();
	auto input_shape = input_tensor_info.GetShape();
	auto output_shape = output_tensor_info.GetShape();

	if (input_shape.size() == 4) {
		if (input_shape[1] == 3) {
			input_channels = input_shape[1];
			input_height = (input_shape[2] == -1) ? 96 : input_shape[2];
			input_width = (input_shape[3] == -1) ? 96 : input_shape[3];
		}
		else {
			input_height = (input_shape[1] == -1) ? 96 : input_shape[1];
			input_width = (input_shape[2] == -1) ? 96 : input_shape[2];
			input_channels = input_shape[3];
		}
	}
	else {
		throw std::runtime_error("Unsupported input shape");
	}

	if (m_target_imgsz > 0) {
		input_width = m_target_imgsz;
		input_height = m_target_imgsz;
	}

	// [Task 1] Log model info for cross-machine diagnosis.
	DEBUG_LOG_FMT("[ONNXInference] Model loaded: input=%dx%dx%d output_classes=%zu",
		input_width, input_height, input_channels, CLASS_LABELS.size());

	size_t num_input_nodes = session->GetInputCount();
	size_t num_output_nodes = session->GetOutputCount();
	for (size_t i = 0; i < num_input_nodes; ++i) {
		input_names_ptr.push_back(session->GetInputNameAllocated(i, Ort::AllocatorWithDefaultOptions()));
		input_names.push_back(input_names_ptr.back().get());
		input_node_names.push_back(input_names.back().c_str());
	}
	for (size_t i = 0; i < num_output_nodes; ++i) {
		output_names_ptr.push_back(session->GetOutputNameAllocated(i, Ort::AllocatorWithDefaultOptions()));
		output_names.push_back(output_names_ptr.back().get());
		output_node_names.push_back(output_names.back().c_str());
	}
}

#ifdef MODEL_ENCRYPT
bool ONNXInference::LoadEncryptedModel(const std::string& model_path) {
#ifdef _WIN32
	std::wstring wmodel_path = BaseUtility::String2WString(model_path);
	FILE* modelFile = _wfopen(wmodel_path.c_str(), L"rb");
	if (!modelFile) {
		MyDbgStr("gsdet: %s - %s", __FUNCTION__, "无法打开模型文件");
		return false;
	}

	fseek(modelFile, 0, SEEK_END);
	long size = ftell(modelFile);
	fseek(modelFile, 0, SEEK_SET);

	std::vector<uint8_t> data(size);
	size_t readBytes = fread(data.data(), 1, size, modelFile);
	fclose(modelFile);
	if (readBytes != static_cast<size_t>(size)) {
		MyDbgStr("gsdet: %s - %s", __FUNCTION__, "无法读取模型文件数据");
		return false;
	}
#else
	std::ifstream modelFile(model_path, std::ios::binary);
	if (!modelFile) {
		MyDbgStr("gsdet: %s - %s", __FUNCTION__, "无法打开模型文件");
		return false;
	}

	modelFile.seekg(0, std::ios::end);
	std::streamsize size = modelFile.tellg();
	modelFile.seekg(0, std::ios::beg);

	std::vector<uint8_t> data(size);
	if (!modelFile.read(reinterpret_cast<char*>(data.data()), size)) {
		MyDbgStr("gsdet: %s - %s", __FUNCTION__, "无法读取模型文件数据");
		return false;
	}
#endif

	if (size % 16 != 0) {
		MyDbgStr("gsdet: %s - %s", __FUNCTION__, "模型大小不是 16 的倍数");
		return false;
	}

	int rounds = static_cast<int>(size / 16);
	cv::parallel_for_(cv::Range(0, rounds), [&](const cv::Range& range) {
		for (int i = range.start; i < range.end; ++i) {
			AES_128::GetInstance().AES_Decrypt(data.data() + i * 16);
		}
					  });

	// 去除空格填充
	while (!data.empty() && data.back() == uint8_t(' ')) {
		data.pop_back();
	}

	// 创建 session，使用当前的 env 和 sessionOptions
	try {
		session = std::make_unique<Ort::Session>(env, data.data(), data.size(), sessionOptions);
		return true;
	} catch (const Ort::Exception& e) {
		MyDbgStr("gsdet: %s - ONNX Runtime error: %s", __FUNCTION__, e.what());
		return false;
	}
}
#endif

bool ONNXInference::ReloadModel() {
    try {
        DEBUG_LOG_FMT("[ONNXInference] Destroying old ONNX session and reloading model...");

        std::lock_guard<std::mutex> reloadLock(m_reloadMutex);

        // 1. 销毁旧的 session（释放 GPU 资源）
        session.reset();
        
        // 2. 重置所有缓存的 tensor 状态（下次推理时重新初始化）
        m_tensorValuesInitialized = false;
        m_inputTensorValues.clear();
        m_batchTensorValues.clear();
        m_batchTensorCapacity = 0;
        
        // 3. 清空 input/output node names（下次重新读取）
        input_names.clear();
        output_names.clear();
        input_names_ptr.clear();
        output_names_ptr.clear();
        input_node_names.clear();
        output_node_names.clear();

        // 4. 使用相同的 sessionOptions 重新创建 session
        // 重新应用 DirectML 提供者
#ifdef _WIN32
        char forceCPUBuf[8] = {};
        bool forceCPU = (GetEnvironmentVariableA("ONNX_FORCE_CPU", forceCPUBuf, sizeof(forceCPUBuf)) > 0)
                     && (forceCPUBuf[0] == '1' || forceCPUBuf[0] == 'Y' || forceCPUBuf[0] == 'y');
        if (!forceCPU) {
            auto dml_status = OrtSessionOptionsAppendExecutionProvider_DML(sessionOptions, 0);
            if (dml_status != nullptr) {
                Ort::GetApi().ReleaseStatus(dml_status);
                DEBUG_LOG_FMT("[ONNXInference-Reload] DirectML FAILED during reload, falling back to CPU");
            } else {
                DEBUG_LOG_FMT("[ONNXInference-Reload] DirectML re-initialized successfully");
            }
        } else {
            DEBUG_LOG_FMT("[ONNXInference-Reload] ONNX_FORCE_CPU=1, skipping DirectML");
        }
#endif

        // 5. 重建 session
#ifdef MODEL_ENCRYPT
        if (!LoadEncryptedModel(model_path_str)) {
            DEBUG_LOG_FMT("[ONNXInference-Reload] Failed to reload encrypted model!");
            return false;
        }
#else
        session = std::make_unique<Ort::Session>(env, m_modelPath.c_str(), sessionOptions);
#endif

        if (!session) {
            DEBUG_LOG_FMT("[ONNXInference-Reload] Failed to create new session!");
            return false;
        }

        // 6. 重新读取输入/输出元数据
        auto input_info = session->GetInputTypeInfo(0);
        auto output_info = session->GetOutputTypeInfo(0);
        auto input_tensor_info = input_info.GetTensorTypeAndShapeInfo();
        auto output_tensor_info = output_info.GetTensorTypeAndShapeInfo();
        auto input_shape = input_tensor_info.GetShape();
        auto output_shape = output_tensor_info.GetShape();

        if (input_shape.size() == 4) {
            if (input_shape[1] == 3) {
                input_channels = input_shape[1];
                input_height = (input_shape[2] == -1) ? 96 : input_shape[2];
                input_width = (input_shape[3] == -1) ? 96 : input_shape[3];
            } else {
                input_height = (input_shape[1] == -1) ? 96 : input_shape[1];
                input_width = (input_shape[2] == -1) ? 96 : input_shape[2];
                input_channels = input_shape[3];
            }
        }

        if (m_target_imgsz > 0) {
            input_width = m_target_imgsz;
            input_height = m_target_imgsz;
        }

        size_t num_input_nodes = session->GetInputCount();
        size_t num_output_nodes = session->GetOutputCount();
        for (size_t i = 0; i < num_input_nodes; ++i) {
            input_names_ptr.push_back(session->GetInputNameAllocated(i, Ort::AllocatorWithDefaultOptions()));
            input_names.push_back(input_names_ptr.back().get());
            input_node_names.push_back(input_names.back().c_str());
        }
        for (size_t i = 0; i < num_output_nodes; ++i) {
            output_names_ptr.push_back(session->GetOutputNameAllocated(i, Ort::AllocatorWithDefaultOptions()));
            output_names.push_back(output_names_ptr.back().get());
            output_node_names.push_back(output_names.back().c_str());
        }

        DEBUG_LOG_FMT("[ONNXInference-Reload] ONNX Runtime session reloaded successfully (input=%zdx%zd)", input_width, input_height);
        return true;
    } catch (const std::exception& e) {
        DEBUG_LOG_FMT("[ONNXInference-Reload] Exception during reload: %s", e.what());
        return false;
    } catch (...) {
        DEBUG_LOG_FMT("[ONNXInference-Reload] Unknown exception during reload");
        return false;
    }
}

cv::Mat ONNXInference::preprocessImage(const cv::Mat& image)
{
	// [Task 3] Log preprocessing params on first frame for cross-machine diagnosis.
	static bool s_preprocessLogged = false;
	if (!s_preprocessLogged && !image.empty()) {
		DEBUG_LOG_FMT("[ONNXInference] Preprocess: input=%dx%d type=%d channels=%d",
			image.cols, image.rows, image.type(), image.channels());
		s_preprocessLogged = true;
	}

	cv::Mat processed;
	
	if (m_target_imgsz > 0) {
		cv::Mat resized;
		cv::resize(image, resized, cv::Size(static_cast<int>(input_width), static_cast<int>(input_height)), 0, 0, cv::INTER_LINEAR);

		m_letterbox = {image.cols, image.rows, static_cast<int>(input_width), static_cast<int>(input_height), 0, 0,
		               static_cast<float>(input_width) / image.cols,
		               static_cast<float>(input_height) / image.rows};

		resized.convertTo(processed, CV_32F, 1.0 / 255.0);
		cv::cvtColor(processed, processed, cv::COLOR_BGR2RGB);

		return processed;
	}
	
	// 原有的 letterbox 逻辑（用于分类模型）
	float model_aspect = static_cast<float>(input_width) / input_height;
	float img_aspect = static_cast<float>(image.cols) / image.rows;
	
	int padded_w, padded_h, offset_x, offset_y;
	float scale;
	
	if (img_aspect > model_aspect) {
		// 图像更宽，以宽度为准
		padded_w = input_width;
		padded_h = static_cast<int>(input_width / img_aspect);
		offset_x = 0;
		offset_y = (input_height - padded_h) / 2;
		scale = static_cast<float>(input_width) / image.cols;
	} else {
		// 图像更高，以高度为准
		padded_h = input_height;
		padded_w = static_cast<int>(input_height * img_aspect);
		offset_y = 0;
		offset_x = (input_width - padded_w) / 2;
		scale = static_cast<float>(input_height) / image.rows;
	}
	
	// 存储 letterbox 参数到成员变量，供推理后坐标转换使用
	m_letterbox = {image.cols, image.rows, padded_w, padded_h, offset_x, offset_y, scale, scale};
	
	// 创建黑边填充的画布
	cv::Mat canvas(input_height, input_width, CV_8UC3, cv::Scalar(114, 114, 114));
	
	// 缩放图像到 padded 尺寸
	cv::Mat resized;
	cv::resize(image, resized, cv::Size(padded_w, padded_h));
	
	// 对小目标进行对比度增强（图像尺寸小于 160x160 时）
	if (image.cols * image.rows < 160 * 160) {
		cv::Mat enhanced;
		resized.convertTo(enhanced, -1, 1.3, 10);  // 对比度增强
		
		// 自适应直方图均衡化
		cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
		cv::Mat lab;
		cv::cvtColor(enhanced, lab, cv::COLOR_BGR2Lab);
		std::vector<cv::Mat> lab_channels;
		cv::split(lab, lab_channels);
		clahe->apply(lab_channels[0], lab_channels[0]);
		cv::merge(lab_channels, lab);
		cv::cvtColor(lab, enhanced, cv::COLOR_Lab2BGR);
		
		enhanced.copyTo(canvas(cv::Rect(offset_x, offset_y, padded_w, padded_h)));
	} else {
		resized.copyTo(canvas(cv::Rect(offset_x, offset_y, padded_w, padded_h)));
	}
	
	// 转换为 float 并归一化
	canvas.convertTo(processed, CV_32F, 1.0 / 255.0);
	cv::cvtColor(processed, processed, cv::COLOR_BGR2RGB);
	
	return processed;
}

std::vector<Detection> ONNXInference::runInference(const cv::Mat& image, int targetSize)
{
	auto result = runInferenceWithMetrics(image, targetSize);
	return result.first;
}

std::pair<std::vector<Detection>, ONNXInference::PerformanceMetrics>
ONNXInference::runInferenceWithMetrics(const cv::Mat& image, int targetSize)
{
	ONNXInference::PerformanceMetrics metrics;
	metrics.input_width = image.cols;
	metrics.input_height = image.rows;

	// 优化：预分配 tensor 缓冲区
	if (!m_tensorValuesInitialized) {
		initializeTensorBuffer();
		m_tensorValuesInitialized = true;
	}

	// 预处理
	auto start_total = std::chrono::high_resolution_clock::now();
	auto start_preprocess = std::chrono::high_resolution_clock::now();

	// 如果指定了 targetSize，临时修改 input_width 和 input_height
	size_t original_input_width = input_width;
	size_t original_input_height = input_height;
	
	if (targetSize > 0) {
		input_width = targetSize;
		input_height = targetSize;
	}

	cv::Mat preprocessed = preprocessImage(image);
	
	// 恢复原始尺寸
	input_width = original_input_width;
	input_height = original_input_height;

	auto end_preprocess = std::chrono::high_resolution_clock::now();
	metrics.preprocess_time_ms = std::chrono::duration<double, std::milli>(end_preprocess - start_preprocess).count();

	// 推理 - 优化预处理和tensor创建
	auto start_inference = std::chrono::high_resolution_clock::now();

	// 优化：使用OpenCV高效操作替代三重循环
	cv::Mat channels[3];
	cv::split(preprocessed, channels);
	
	// 直接使用连续内存创建tensor，避免数据拷贝
	std::vector<int64_t> input_shape = { 1, static_cast<int64_t>(input_channels),
					   static_cast<int64_t>(input_height),
					   static_cast<int64_t>(input_width) };
	
	auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
	
	// 优化：使用成员变量缓存的 tensor 缓冲区
	// 高效填充tensor数据
	for (int c = 0; c < 3; ++c) {
		float* channel_data = reinterpret_cast<float*>(channels[c].data);
		std::memcpy(&m_inputTensorValues[c * input_height * input_width], 
			channel_data, input_height * input_width * sizeof(float));
	}

	auto input_tensor = Ort::Value::CreateTensor<float>(
		memory_info, m_inputTensorValues.data(), m_inputTensorValues.size(),
		input_shape.data(), input_shape.size()
	);

	// 优化：使用高性能推理选项
	Ort::RunOptions run_options;
	run_options.SetRunLogVerbosityLevel(ORT_LOGGING_LEVEL_FATAL);  // 最小化日志输出
	//run_options.SetTerminate();  // 提前终止
	
	std::vector<Ort::Value> output_tensors;
	{
		std::lock_guard<std::mutex> lock(m_inferenceMutex);
		
		// 【关键修复】检查 session 有效性，如果为 null 则尝试自动重载
		if (!session) {
			DEBUG_LOG_FMT("[ONNXInference] Session is null, attempting auto-reload");
			
			// 临时释放推理锁，避免死锁
			lock.~lock_guard();
			
			// 尝试重载 session
			bool reloadOk = ReloadModel();
			
			// 重新获取推理锁
			new (&lock) std::lock_guard<std::mutex>(m_inferenceMutex);
			
			if (!reloadOk || !session) {
				DEBUG_LOG_FMT("[ONNXInference] Session reload failed, skipping inference");
				return { std::vector<Detection>(), metrics };
			}
			DEBUG_LOG_FMT("[ONNXInference] Session reloaded successfully");
		}
		
		output_tensors = session->Run(
			run_options,
			input_node_names.data(), &input_tensor, 1,
			output_node_names.data(), output_node_names.size()
		);
	}

	auto end_inference = std::chrono::high_resolution_clock::now();
	metrics.inference_time_ms = std::chrono::duration<double, std::milli>(end_inference - start_inference).count();

	auto start_postprocess = std::chrono::high_resolution_clock::now();
	 
	//// ===== ���������� Ultralytics YOLOv8/v11 �˵������ (1, 84, 8400) =====
	//auto& output_tensor = output_tensors[0];
	//auto type_info = output_tensor.GetTensorTypeAndShapeInfo();
	//auto output_shape = type_info.GetShape();

	//// ��֤�����״��ӦΪ (1, num_outputs, num_boxes) = (1, 84, 8400)
	//if (output_shape.size() != 3 || output_shape[0] != 1) {
	//	std::cerr << "Unexpected output shape! Expected (1, C, N), got [";
	//	for (auto s : output_shape) std::cout << s << " ";
	//	std::cout << "]" << std::endl;
	//	return { { }, metrics };
	//}

	//const float* data = output_tensor.GetTensorData<float>();
	//int num_outputs = static_cast<int>(output_shape[1]); // ӦΪ 84
	//int num_boxes = static_cast<int>(output_shape[2]);   // ӦΪ 8400

	//// �����������num_classes = num_outputs - 4
	//int num_classes = num_outputs - 4;
	//if (num_classes <= 0) {
	//	std::cerr << "Invalid number of classes: " << num_classes << std::endl;
	//	return { {}, metrics };
	//}

	//std::vector<cv::Rect> boxes;
	//std::vector<float> scores;
	//std::vector<int> class_ids;

	//const float CONF_THRESH = 0.25f;

	//// ���ݲ����� (1, 84, 8400)�����Ե� i �������š� data + i * num_outputs
	//for (int i = 0; i < num_boxes; ++i) {
	//	const float* box_data = data + i * num_outputs;

	//	float x1 = box_data[0];
	//	float y1 = box_data[1];
	//	float x2 = box_data[2];
	//	float y2 = box_data[3];

	//	// ������Ч��
	//	if (x2 <= x1 || y2 <= y1) continue;

	//	// �����������Ŷ�
	//	int best_cls = 0;
	//	float best_conf = box_data[4]; // ��5��Ԫ���� class 0
	//	for (int c = 1; c < num_classes; ++c) {
	//		if (box_data[4 + c] > best_conf) {
	//			best_conf = box_data[4 + c];
	//			best_cls = c;
	//		}
	//	}

	//	if (best_conf < CONF_THRESH) continue;

	//	// 边界裁剪（防止越界）
	//	x1 = std::max(0.0f, std::min(static_cast<float>(input_width - 1), x1));
	//	y1 = std::max(0.0f, std::min(static_cast<float>(input_height - 1), y1));
	//	x2 = std::max(0.0f, std::min(static_cast<float>(input_width - 1), x2));
	//	y2 = std::max(0.0f, std::min(static_cast<float>(input_height - 1), y2));

	//	boxes.emplace_back(cv::Point(static_cast<int>(x1), static_cast<int>(y1)),
	//					   cv::Point(static_cast<int>(x2), static_cast<int>(y2)));
	//	scores.push_back(best_conf);      // ֱ��ʹ��������Ŷ�
	//	class_ids.push_back(best_cls);
	//}

	// ===== 处理 YOLOv8 输出格式 =====
	std::vector<Detection> detections;
	
	// 验证输出张量
	if (output_tensors.empty()) {
		std::cerr << "[ERROR] No output tensors from model!" << std::endl;
		return { detections, metrics };
	}
	
	auto& output_tensor = output_tensors[0];
	auto type_info = output_tensor.GetTensorTypeAndShapeInfo();
	auto output_shape = type_info.GetShape();
	
	const float* data = output_tensor.GetTensorMutableData<float>();
	
	// 根据输出形状判断格式
	if (output_shape.size() == 3 && output_shape[0] == 1 && output_shape[2] == 6) {
		int num_dets = static_cast<int>(output_shape[1]);

		for (int i = 0; i < num_dets; ++i) {
			const float* det = data + i * 6;
			float x1 = det[0];
			float y1 = det[1];
			float x2 = det[2];
			float y2 = det[3];
			float raw_conf = det[4];
			int class_id = static_cast<int>(det[5]);

			if (raw_conf <= 0 || class_id < 0 || class_id >= static_cast<int>(DETECTION_LABELS.size())) {
				continue;
			}

			float clamped = std::max(-100.0f, std::min(100.0f, raw_conf));
			float conf = 1.0f / (1.0f + std::exp(-clamped));

			if (conf > CONF_THRESH) {
				float x1_model = det[0];
				float y1_model = det[1];
				float x2_model = det[2];
				float y2_model = det[3];
				
				float x1_orig = (x1_model - m_letterbox.offset_x) / m_letterbox.scale;
				float y1_orig = (y1_model - m_letterbox.offset_y) / m_letterbox.scale_y;
				float x2_orig = (x2_model - m_letterbox.offset_x) / m_letterbox.scale;
				float y2_orig = (y2_model - m_letterbox.offset_y) / m_letterbox.scale_y;
				
				x1_orig = std::max(0.0f, std::min(static_cast<float>(m_letterbox.src_width - 1), x1_orig));
				y1_orig = std::max(0.0f, std::min(static_cast<float>(m_letterbox.src_height - 1), y1_orig));
				x2_orig = std::max(0.0f, std::min(static_cast<float>(m_letterbox.src_width - 1), x2_orig));
				y2_orig = std::max(0.0f, std::min(static_cast<float>(m_letterbox.src_height - 1), y2_orig));
				
				float box_w = x2_orig - x1_orig;
				float box_h = y2_orig - y1_orig;
				float box_area = box_w * box_h;
				float img_area = m_letterbox.src_width * m_letterbox.src_height;
				
				if (box_area < img_area * 0.01f || box_area > img_area * 0.9f) {
					continue;
				}
				if (box_w < 5 || box_h < 5) {
					continue;
				}
				
				Detection detection;
				detection.box = cv::Rect(cv::Point(static_cast<int>(x1_orig), static_cast<int>(y1_orig)),
										 cv::Point(static_cast<int>(x2_orig), static_cast<int>(y2_orig)));
				detection.confidence = conf;
				detection.class_id = class_id;
				detection.class_name = DETECTION_LABELS[class_id];
				detections.push_back(detection);
			}
		}
	}
	else if (output_shape.size() == 3 && output_shape[0] == 1) {
		int num_outputs = static_cast<int>(output_shape[1]);
		int num_boxes = static_cast<int>(output_shape[2]);
		int num_classes = num_outputs - 4;
		
		if (num_classes > 0) {
			for (int i = 0; i < num_boxes; ++i) {
				const float* box_data = data + i * num_outputs;

				float x1 = box_data[0];
				float y1 = box_data[1];
				float x2 = box_data[2];
				float y2 = box_data[3];

				if (x2 <= x1 || y2 <= y1) continue;

				int best_cls = 0;
				float best_conf = box_data[4];
				for (int c = 1; c < num_classes; ++c) {
					if (box_data[4 + c] > best_conf) {
						best_conf = box_data[4 + c];
						best_cls = c;
					}
				}

				if (best_conf < CONF_THRESH) continue;
				if (best_cls >= static_cast<int>(DETECTION_LABELS.size())) continue;

				float x1_orig = (x1 - m_letterbox.offset_x) / m_letterbox.scale;
				float y1_orig = (y1 - m_letterbox.offset_y) / m_letterbox.scale_y;
				float x2_orig = (x2 - m_letterbox.offset_x) / m_letterbox.scale;
				float y2_orig = (y2 - m_letterbox.offset_y) / m_letterbox.scale_y;
				
				x1_orig = std::max(0.0f, std::min(static_cast<float>(m_letterbox.src_width - 1), x1_orig));
				y1_orig = std::max(0.0f, std::min(static_cast<float>(m_letterbox.src_height - 1), y1_orig));
				x2_orig = std::max(0.0f, std::min(static_cast<float>(m_letterbox.src_width - 1), x2_orig));
				y2_orig = std::max(0.0f, std::min(static_cast<float>(m_letterbox.src_height - 1), y2_orig));
				
				Detection detection;
				detection.box = cv::Rect(cv::Point(static_cast<int>(x1_orig), static_cast<int>(y1_orig)),
										 cv::Point(static_cast<int>(x2_orig), static_cast<int>(y2_orig)));
				detection.confidence = best_conf;
				detection.class_id = best_cls;
				detection.class_name = DETECTION_LABELS[best_cls];
				detections.push_back(detection);
			}
		}
	}
	else {
		std::cerr << "[ERROR] Unsupported output shape! Expected (1, N, 6) or (1, C, N), got [";
		for (auto s : output_shape) std::cerr << s << " ";
		std::cerr << "]" << std::endl;
	}

	// ģ������ NMS�������ٵ��� nonMaxSuppression��
	std::vector<int> indices(detections.size());
	std::iota(indices.begin(), indices.end(), 0);

	// ִ�� NMS
	//std::vector<int> indices;
	//cv::dnn::NMSBoxes(boxes, scores, CONF_THRESH, 0.45f, indices);

	// ���� Detection ���
	//std::vector<Detection> detections;
	//for (int idx : indices) {
	//	Detection det;
	//	det.box = boxes[idx];
	//	det.confidence = scores[idx];
	//	det.class_id = class_ids[idx];
	//	if (det.class_id >= 0 && det.class_id < CLASS_LABELS.size()) {
	//		det.class_name = CLASS_LABELS[det.class_id];
	//	}
	//	else {
	//		det.class_name = "unknown";
	//	}
	//	detections.push_back(det);
	//}

	auto end_postprocess = std::chrono::high_resolution_clock::now();
	auto end_total = std::chrono::high_resolution_clock::now();

	metrics.postprocess_time_ms = std::chrono::duration<double, std::milli>(end_postprocess - start_postprocess).count();
	metrics.total_time_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();

	return { detections, metrics };
}

ClassificationResult ONNXInference::runClassification(const cv::Mat& image)
{
	auto result = runClassificationWithMetrics(image);
	return result.first;
}

std::pair<ClassificationResult, ONNXInference::PerformanceMetrics>
ONNXInference::runClassificationWithMetrics(const cv::Mat& image)
{
	ONNXInference::PerformanceMetrics metrics;
	metrics.input_width = image.cols;
	metrics.input_height = image.rows;

	// Ԥ����
	auto start_total = std::chrono::high_resolution_clock::now();
	auto start_preprocess = std::chrono::high_resolution_clock::now();

	cv::Mat preprocessed = preprocessImage(image);

	auto end_preprocess = std::chrono::high_resolution_clock::now();
	metrics.preprocess_time_ms = std::chrono::duration<double, std::milli>(end_preprocess - start_preprocess).count();

	auto start_inference = std::chrono::high_resolution_clock::now();

	// 优化：使用 OpenCV 的高效操作替代三重循环
	// 将 HWC 转换为 CHW 格式
		std::vector<float> input_tensor_values(input_channels * input_height * input_width);
	
	// 使用 split 和 reshape 提高效率
	std::vector<cv::Mat> channels(3);
	cv::split(preprocessed, channels);
	
	for (int c = 0; c < 3; ++c) {
		std::memcpy(&input_tensor_values[c * input_height * input_width],
			channels[c].data, input_height * input_width * sizeof(float));
	}

	std::vector<int64_t> input_shape = { 1, static_cast<int64_t>(input_channels),
						   static_cast<int64_t>(input_height),
						   static_cast<int64_t>(input_width) };
	auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
	auto input_tensor = Ort::Value::CreateTensor<float>(
		memory_info, input_tensor_values.data(), input_tensor_values.size(),
		input_shape.data(), input_shape.size()
	);

	std::vector<Ort::Value> output_tensors;
	{
		std::lock_guard<std::mutex> lock(m_inferenceMutex);
		
		// 【关键修复】检查 session 有效性，如果为 null 则尝试自动重载
		if (!session) {
			DEBUG_LOG_FMT("[ONNXInference] Session is null, attempting auto-reload");
			
			// 临时释放推理锁，避免死锁
			lock.~lock_guard();
			
			// 尝试重载 session
			bool reloadOk = ReloadModel();
			
			// 重新获取推理锁
			new (&lock) std::lock_guard<std::mutex>(m_inferenceMutex);
			
			if (!reloadOk || !session) {
				DEBUG_LOG_FMT("[ONNXInference] Session reload failed, skipping inference");
				ClassificationResult emptyResult;
				emptyResult.class_id = -1;
				emptyResult.confidence = 0.0f;
				emptyResult.class_name = "unknown";
				return { emptyResult, metrics };
			}
			DEBUG_LOG_FMT("[ONNXInference] Session reloaded successfully");
		}
		
		output_tensors = session->Run(
			Ort::RunOptions { nullptr },
			input_node_names.data(), &input_tensor, 1,
			output_node_names.data(), output_node_names.size()
		);
	}

	auto end_inference = std::chrono::high_resolution_clock::now();
	metrics.inference_time_ms = std::chrono::duration<double, std::milli>(end_inference - start_inference).count();

	auto start_postprocess = std::chrono::high_resolution_clock::now();

	// ===== ���������� ��ͼƬ���汾 =====
	ClassificationResult result;
	result.class_id = -1;
	result.confidence = 0.0f;
	result.class_name = "unknown";

	// ��ȡ�������
	auto& output_tensor = output_tensors[0];
	auto type_info = output_tensor.GetTensorTypeAndShapeInfo();
	auto output_shape = type_info.GetShape();

	// ��֤�����״��ӦΪ (1, num_classes)
	if (output_shape.size() != 2 || output_shape[0] != 1) {
		std::cerr << "Unexpected classification output shape! Expected (1, C), got [";
		for (auto s : output_shape) std::cout << s << " ";
		std::cout << "]" << std::endl;
		return { result, metrics };
	}

	const float* data = output_tensor.GetTensorData<float>();
	int num_classes = static_cast<int>(output_shape[1]);

	// ���Ҹ���置信度��类别
	int bestClassId = 0;
	float bestConfidence = data[0];
	for (int i = 1; i < num_classes; ++i) {
		if (data[i] > bestConfidence) {
			bestConfidence = data[i];
			bestClassId = i;
		}
	}

	// ���ز���
	result.class_id = bestClassId;
	result.confidence = bestConfidence;
	if (bestClassId >= 0 && bestClassId < CLASS_LABELS.size()) {
		result.class_name = CLASS_LABELS[bestClassId];
	}

	auto end_postprocess = std::chrono::high_resolution_clock::now();
	auto end_total = std::chrono::high_resolution_clock::now();

	metrics.postprocess_time_ms = std::chrono::duration<double, std::milli>(end_postprocess - start_postprocess).count();
	metrics.total_time_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();

	return { result, metrics };
}

std::pair<std::vector<ClassificationResult>, ONNXInference::PerformanceMetrics>
ONNXInference::runBatchClassificationWithMetrics(const std::vector<cv::Mat>& images)
{
	ONNXInference::PerformanceMetrics metrics{};
	
	if (images.empty()) {
		return { {}, metrics };
	}
	
	auto start_total = std::chrono::high_resolution_clock::now();
	
	size_t batch_size = images.size();
	
	// 【性能优化】预分配批处理 tensor 缓冲区，避免每帧重复堆分配
	size_t required_size = batch_size * input_channels * input_height * input_width;
	if (m_batchTensorCapacity < required_size) {
		m_batchTensorValues.resize(required_size);
		m_batchTensorCapacity = required_size;
	}
	float* batch_data = m_batchTensorValues.data();
	
	// 【性能优化】并行预处理并直接写入 batch_data
	// 合并预处理(resize+normalize+split)和数据转换(CHW)为一步并行操作
	auto start_preprocess = std::chrono::high_resolution_clock::now();
	
	struct PreprocessTask {
		const cv::Mat* img;
		float* output;
		size_t input_width;
		size_t input_height;
		size_t input_channels;
	};
	
	std::vector<PreprocessTask> tasks(batch_size);
	for (size_t i = 0; i < batch_size; ++i) {
		tasks[i].img = &images[i];
		tasks[i].output = batch_data + i * input_channels * input_height * input_width;
		tasks[i].input_width = input_width;
		tasks[i].input_height = input_height;
		tasks[i].input_channels = input_channels;
	}
	
	cv::parallel_for_(cv::Range(0, static_cast<int>(batch_size)), [&](const cv::Range& range) {
		for (int i = range.start; i < range.end; ++i) {
			const auto& task = tasks[i];
			cv::Mat resized;
			cv::resize(*task.img, resized, 
				cv::Size(static_cast<int>(task.input_width), static_cast<int>(task.input_height)), 
				0, 0, cv::INTER_LINEAR);
			
			cv::Mat float_img;
			resized.convertTo(float_img, CV_32F, 1.0 / 255.0);
			cv::cvtColor(float_img, float_img, cv::COLOR_BGR2RGB);
			
			std::vector<cv::Mat> channels(3);
			cv::split(float_img, channels);
			
			for (int c = 0; c < 3; ++c) {
				std::memcpy(task.output + c * task.input_height * task.input_width,
					channels[c].data, task.input_height * task.input_width * sizeof(float));
			}
		}
	});
	
	auto end_preprocess = std::chrono::high_resolution_clock::now();
	metrics.preprocess_time_ms = std::chrono::duration<double, std::milli>(end_preprocess - start_preprocess).count();
	
	// 创建批处理 tensor
	auto start_inference = std::chrono::high_resolution_clock::now();
	
	// [Task 3] Log batch input statistics on first batch for cross-machine diagnosis.
	{
		static bool s_batchLogged = false;
		if (!s_batchLogged) {
			float minVal = batch_data[0], maxVal = batch_data[0];
			for (size_t i = 1; i < required_size; ++i) {
				if (batch_data[i] < minVal) minVal = batch_data[i];
				if (batch_data[i] > maxVal) maxVal = batch_data[i];
			}
			DEBUG_LOG_FMT("[ONNXInference] Batch input: batch=%zu size=%zu range=[%.4f, %.4f]",
				batch_size, required_size, minVal, maxVal);
			s_batchLogged = true;
		}
	}
	
	// 创建批处理 tensor (batch_size, channels, height, width)
	std::vector<int64_t> input_shape = { 
		static_cast<int64_t>(batch_size), 
		static_cast<int64_t>(input_channels),
		static_cast<int64_t>(input_height),
		static_cast<int64_t>(input_width) 
	};
	
	auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
	auto input_tensor = Ort::Value::CreateTensor<float>(
		memory_info, batch_data, required_size,
		input_shape.data(), input_shape.size()
	);
	
	// 执行批处理推理
	std::vector<Ort::Value> output_tensors;
	{
		std::lock_guard<std::mutex> lock(m_inferenceMutex);
		
		// 【关键修复】检查 session 有效性，如果为 null 则尝试自动重载
		if (!session) {
			DEBUG_LOG_FMT("[ONNXInference] Session is null, attempting auto-reload");
			
			// 临时释放推理锁，避免死锁
			lock.~lock_guard();
			
			// 尝试重载 session
			bool reloadOk = ReloadModel();
			
			// 重新获取推理锁
			new (&lock) std::lock_guard<std::mutex>(m_inferenceMutex);
			
			if (!reloadOk || !session) {
				DEBUG_LOG_FMT("[ONNXInference] Session reload failed, skipping inference");
				return { std::vector<ClassificationResult>(), metrics };
			}
			DEBUG_LOG_FMT("[ONNXInference] Session reloaded successfully");
		}
		
		output_tensors = session->Run(
			Ort::RunOptions{ nullptr },
			input_node_names.data(), &input_tensor, 1,
			output_node_names.data(), output_node_names.size()
		);
	}
	
	auto end_inference = std::chrono::high_resolution_clock::now();
	metrics.inference_time_ms = std::chrono::duration<double, std::milli>(end_inference - start_inference).count();
	
	// 后处理
	auto start_postprocess = std::chrono::high_resolution_clock::now();
	
	std::vector<ClassificationResult> results;
	results.reserve(batch_size);
	
	auto& output_tensor = output_tensors[0];
	auto type_info = output_tensor.GetTensorTypeAndShapeInfo();
	auto output_shape = type_info.GetShape();
	
	// 输出形状应该是 (batch_size, num_classes)
	if (output_shape.size() != 2 || output_shape[0] != static_cast<int64_t>(batch_size)) {
		std::cerr << "Unexpected batch classification output shape! Expected (" << batch_size << ", C), got [";
		for (auto s : output_shape) std::cout << s << " ";
		std::cout << "]" << std::endl;
		
		// 返回默认结果
		for (size_t i = 0; i < batch_size; ++i) {
			results.push_back({ -1, 0.0f, "unknown" });
		}
		return { results, metrics };
	}
	
	const float* data = output_tensor.GetTensorData<float>();
	int num_classes = static_cast<int>(output_shape[1]);

	// 处理每个样本的结果
	for (size_t i = 0; i < batch_size; ++i) {
		const float* sample_data = data + i * num_classes;
		
		// 找到最高置信度的类别
		int bestClassId = 0;
		float bestConfidence = sample_data[0];
		for (int j = 1; j < num_classes; ++j) {
			if (sample_data[j] > bestConfidence) {
				bestConfidence = sample_data[j];
				bestClassId = j;
			}
		}
		
		ClassificationResult result;
		result.class_id = bestClassId;
		result.confidence = bestConfidence;
		if (bestClassId >= 0 && bestClassId < CLASS_LABELS.size()) {
			result.class_name = CLASS_LABELS[bestClassId];
		} else {
			result.class_name = "unknown";
		}
		
		results.push_back(result);
	}
	
	auto end_postprocess = std::chrono::high_resolution_clock::now();
	auto end_total = std::chrono::high_resolution_clock::now();
	
	metrics.postprocess_time_ms = std::chrono::duration<double, std::milli>(end_postprocess - start_postprocess).count();
	metrics.total_time_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();
	
	// 设置输入尺寸（使用第一个图像的尺寸）
	if (!images.empty()) {
		metrics.input_width = images[0].cols;
		metrics.input_height = images[0].rows;
	}
	
	return { results, metrics };
}

cv::Size ONNXInference::getInputSize() const
{
	return cv::Size(static_cast<int>(input_width), static_cast<int>(input_height));
}

void ONNXInference::initializeTensorBuffer() const
{
	size_t bufferSize = input_channels * input_height * input_width;
	m_inputTensorValues.resize(bufferSize);
	m_tensorValuesInitialized = true;
}

ONNXInference::~ONNXInference() {
	stopAsyncInference();
}

void ONNXInference::stopAsyncInference() {
	if (m_inferenceThreadRunning.load()) {
		m_inferenceThreadRunning.store(false);
		m_requestCV.notify_all();
		if (m_inferenceThread && m_inferenceThread->joinable()) {
			m_inferenceThread->join();
		}
		m_inferenceThread.reset();
	}
}

void ONNXInference::pauseAsyncInference() {
	m_asyncPaused.store(true);
}

void ONNXInference::resumeAsyncInference() {
	m_asyncPaused.store(false);
}

void ONNXInference::inferenceThreadLoop() {
	while (m_inferenceThreadRunning.load()) {
		std::unique_ptr<InferenceRequest> request;
		
		{
			std::unique_lock<std::mutex> lock(m_requestQueueMutex);
			m_requestCV.wait(lock, [this] {
				return !m_inferenceThreadRunning.load() || !m_requestQueue.empty();
			});
			
			if (!m_inferenceThreadRunning.load() && m_requestQueue.empty()) {
				break;
			}
			
			if (!m_requestQueue.empty()) {
				request = std::move(m_requestQueue.front());
				m_requestQueue.pop();
			}
		}
		
		if (request) {
			// 【焦点恢复优化】暂停时跳过推理，但不退出线程
			if (m_asyncPaused.load()) {
				// 丢弃队列中的所有任务，保持队列为空
				std::lock_guard<std::mutex> lock(m_requestQueueMutex);
				while (!m_requestQueue.empty()) {
					m_requestQueue.pop();
				}
				m_hasAsyncResult.store(false);
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			
			try {
				// 【性能优化】后台线程执行完整预处理+推理+后处理
				auto result = runBatchClassificationWithMetrics(request->images);
				
				// 将结果写入缓存，供主线程读取
				{
					std::lock_guard<std::mutex> lock(m_resultMutex);
					m_lastAsyncResults = std::move(result.first);
					m_lastAsyncMetrics = result.second;
					m_hasAsyncResult.store(true);
					m_processedTaskCount.fetch_add(1);
				}
			} catch (...) {
				DEBUG_LOG_FMT("[ONNXInference] Async inference error, task discarded");
			}
		}
	}
	// 【关键修复】线程退出时必须重置标志，否则主线程无法重启线程
	m_inferenceThreadRunning.store(false);
}

void ONNXInference::submitAsyncClassification(const std::vector<cv::Mat>& images) {
	if (images.empty()) {
		return;
	}
	
	// 启动推理线程（首次调用或线程已退出时）
	if (!m_inferenceThreadRunning.load()) {
		// 【关键修复】清理可能已退出但未 join 的旧线程
		if (m_inferenceThread && m_inferenceThread->joinable()) {
			m_inferenceThread->join();
		}
		m_inferenceThread.reset();
		m_inferenceThreadRunning.store(true);
		m_inferenceThread = std::make_unique<std::thread>(&ONNXInference::inferenceThreadLoop, this);
		DEBUG_LOG_FMT("[ONNXInference] Async inference thread started");
	}
	
	// 【性能优化】丢弃队列中旧任务，只保留最新一帧，避免后台线程堆积
	{
		std::lock_guard<std::mutex> lock(m_requestQueueMutex);
		while (!m_requestQueue.empty()) {
			m_requestQueue.pop();
		}
	}
	
	auto request = std::make_unique<InferenceRequest>();
	request->images.reserve(images.size());
	for (const auto& img : images) {
		request->images.push_back(img.clone());
	}
	
	{
		std::lock_guard<std::mutex> lock(m_requestQueueMutex);
		m_requestQueue.push(std::move(request));
	}
	
	m_requestCV.notify_one();
}

bool ONNXInference::tryGetAsyncClassificationResult(std::vector<ClassificationResult>& results, PerformanceMetrics& metrics) {
	if (!m_hasAsyncResult.load()) {
		return false;
	}
	
	std::lock_guard<std::mutex> lock(m_resultMutex);
	if (!m_hasAsyncResult.load()) {
		return false;
	}
	
	results = std::move(m_lastAsyncResults);
	metrics = m_lastAsyncMetrics;
	m_hasAsyncResult.store(false);
	
	return true;
}