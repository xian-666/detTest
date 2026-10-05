#pragma once
#include <onnxruntime_cxx_api.h>

#ifndef ONNX_CUDA_PROVIDER_DECLARED
#define ONNX_CUDA_PROVIDER_DECLARED

extern "C" {
    struct OrtStatus;
    struct OrtSessionOptions;
    OrtStatus* OrtSessionOptionsAppendExecutionProvider_CUDA(
        OrtSessionOptions* options, int device_id);
}

#endif // ONNX_CUDA_PROVIDER_DECLARED

namespace OrtUtils
{
    inline void EnableCUDA(Ort::SessionOptions& options, int device_id = 0)
    {
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CUDA(options, device_id));
    }
}