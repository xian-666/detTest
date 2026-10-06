// WGCScreenCapture.cpp
//#include "../../Component/Log/debugLog.h"
#include "../common/DebugFileLog.h"
#include "WGCScreenCapture.h"
#include "DXGIScreenCapture.h"   // [Hybrid GPU pin-iGPU] reuse IsPinToIGPUEnvEnabled + DXGI headers
#include <condition_variable>
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <dwmapi.h>
#include <winrt/base.h>
#pragma comment(lib, "dwmapi.lib")

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

WGCScreenCapture::WGCScreenCapture()
    : m_captureRunning(false),
    m_isWindowCapture(false),
    m_targetWindow(nullptr),
    m_hasNewFrame(false),
    m_captureCursor(false),
    m_borderRequired(false) {
    try {
        winrt::init_apartment();
    }
    catch (const winrt::hresult_error& e) {
        if (e.code() == winrt::impl::error_wrong_thread) {
            // Thread mode already set, skip this error
        } else {
            throw;
        }
    }

    SetRectEmpty(&m_captureRect);
    m_imageProcessConfig = ImageProcessConfig();
}

WGCScreenCapture::~WGCScreenCapture() {
    StopCapture();

    if (m_resizeShader) m_resizeShader.Release();
    if (m_copyShader) m_copyShader.Release();
    if (m_bilinearSampler) m_bilinearSampler.Release();
    if (m_constantBuffer) m_constantBuffer.Release();
    if (m_outputUAV) m_outputUAV.Release();
    if (m_inputSRV) m_inputSRV.Release();

    m_captureItem = nullptr;
    m_framePool = nullptr;
    m_captureSession = nullptr;
    m_winrtDevice = nullptr;

    winrt::uninit_apartment();
}

WGCScreenCapture& WGCScreenCapture::GetInstance() {
    static WGCScreenCapture capture;
    return capture;
}

bool WGCScreenCapture::InitializeD3D11Device() {
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // [Hybrid GPU pin-iGPU] When BT_CAPTURE_PIN_IGPU=1, explicitly bind the WGC D3D device to an
    // iGPU/APU adapter. Without pin, pAdapter=nullptr lets the OS pick the "default" adapter
    // (usually the iGPU in Optimus setups, but can shift during MUX/migration windows).
    CComPtr<IDXGIAdapter1> pinIGPUAdapter;
    if (DXGIScreenCapture::IsPinToIGPUEnvEnabled()) {
        CComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                reinterpret_cast<void**>(&factory)))) {
            for (UINT i = 0; ; ++i) {
                CComPtr<IDXGIAdapter1> adapter;
                HRESULT hrEnum = factory->EnumAdapters1(i, &adapter);
                if (hrEnum == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(hrEnum) || !adapter) continue;

                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

                const SIZE_T vramMB = desc.DedicatedVideoMemory / (1024 * 1024);
                const bool isIntelIGPU = (desc.VendorId == 0x8086);
                const bool isAmdAPU    = (desc.VendorId == 0x1002 && vramMB <= 1024);
                if (!isIntelIGPU && !isAmdAPU) continue;

                pinIGPUAdapter = adapter;
                char bufA[256] = { 0 };
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, bufA, sizeof(bufA), nullptr, nullptr);
                DEBUG_LOG_FMT("[WGCScreenCapture] Pin-iGPU selected adapter=%s (BT_CAPTURE_PIN_IGPU=1)", bufA);
                break;
            }
        }
        if (!pinIGPUAdapter) {
            DEBUG_LOG_FMT("[WGCScreenCapture] BT_CAPTURE_PIN_IGPU=1 but no iGPU/APU adapter located, using OS default");
        }
    }

    HRESULT hr = D3D11CreateDevice(
        pinIGPUAdapter,                                             // nullptr when pin disabled; explicit iGPU when enabled
        pinIGPUAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_d3dDevice,
        nullptr,
        &m_d3dContext
    );

    if (FAILED(hr)) {
        LogMessage("Failed to create D3D11 device: " + std::to_string(hr));
        return false;
    }

    hr = m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice),
        reinterpret_cast<void**>(&m_dxgiDevice));
    if (FAILED(hr)) {
        LogMessage("Failed to get DXGI device: " + std::to_string(hr));
        return false;
    }

    // [Diag-3a] Record which physical adapter the default device actually landed on.
    // On hybrid-GPU laptops the "default" adapter is the one driving the primary display
    // (usually the iGPU). If the game window lives on another adapter's monitor, WGC
    // frames take a DWM cross-adapter copy - the prime suspect for intermittent black
    // frames under memory pressure. Log-only here; no behavior change (phase 3a).
    {
        CComPtr<IDXGIAdapter> deviceAdapter;
        if (SUCCEEDED(m_dxgiDevice->GetAdapter(&deviceAdapter))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(deviceAdapter->GetDesc(&desc))) {
                m_deviceAdapterLuid = desc.AdapterLuid;
                m_deviceAdapterVendorId = desc.VendorId;
                m_deviceAdapterDeviceId = desc.DeviceId;
                m_deviceAdapterDesc = desc.Description;
                m_deviceAdapterValid = true;
                char descA[256] = { 0 };
                WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, descA, sizeof(descA), NULL, NULL);
                DEBUG_LOG_FMT("[WGCScreenCapture] Device adapter: ven=0x%04X dev=0x%04X LUID=%08X:%08X desc=%s",
                    desc.VendorId, desc.DeviceId,
                    (unsigned)desc.AdapterLuid.HighPart, (unsigned)desc.AdapterLuid.LowPart, descA);
            }
        }
    }

    return true;
}

bool WGCScreenCapture::CreateWinRTDevice() {
    try {
        com_ptr<::IInspectable> inspectable;
        HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(
            m_dxgiDevice.p,
            inspectable.put()
        );

        if (FAILED(hr)) {
            LogMessage("Failed to create WinRT device: " + std::to_string(hr));
            return false;
        }

        m_winrtDevice = inspectable.as<IDirect3DDevice>();
        return true;
    }
    catch (const winrt::hresult_error& ex) {
        LogMessage("WinRT device exception: " + winrt::to_string(ex.message()));
        return false;
    }
}

bool WGCScreenCapture::CreateCaptureItemForWindow(HWND hWnd) {
    try {
        auto interop = winrt::get_activation_factory<GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();

        winrt::guid itemGuid = winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>();

        winrt::check_hresult(interop->CreateForWindow(
            hWnd,
            itemGuid,
            winrt::put_abi(m_captureItem)
        ));

        if (!m_captureItem) {
            LogMessage("Failed to create capture item");
            return false;
        }

        LogMessage("Window capture item created successfully");
        return true;
    }
    catch (const winrt::hresult_error& ex) {
        LogMessage("Capture item exception: " + winrt::to_string(ex.message()));
        return false;
    }
    catch (...) {
        LogMessage("Unknown exception during capture item creation");
        return false;
    }
}

bool WGCScreenCapture::StartCapture() {
    try {
        if (!m_captureItem || !m_winrtDevice) {
            LogMessage("Capture device not initialized");
            return false;
        }

        auto size = m_captureItem.Size();

        m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_winrtDevice,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            size
        );

        if (!m_framePool) {
            LogMessage("Failed to create frame pool");
            return false;
        }

        m_frameArrivedToken = m_framePool.FrameArrived(
            { this, &WGCScreenCapture::OnFrameArrived }
        );

        m_captureSession = m_framePool.CreateCaptureSession(m_captureItem);

        if (!m_captureSession) {
            LogMessage("Failed to create capture session");
            return false;
        }

        m_captureSession.IsCursorCaptureEnabled(m_captureCursor);

        auto session3 = m_captureSession.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>();
        if (session3) {
            session3.IsBorderRequired(m_borderRequired);
        }
        else {
            LogMessage("Current system does not support IsBorderRequired (requires Win11 or later)");
        }

        m_isFirstFrame = true;
        m_lastSize = { 0, 0 };
        m_captureSession.StartCapture();
        m_captureRunning = true;

        LogMessage("WGC capture started");
        return true;
    }
    catch (const winrt::hresult_error& ex) {
        LogMessage("Start capture exception: " + winrt::to_string(ex.message()));
        return false;
    }
}

bool WGCScreenCapture::InitializeForWindow(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) {
        LogMessage("Invalid window handle");
        return false;
    }

    // Stop old capture session first to prevent OnFrameArrived callback
    // from firing during re-initialization (D3D race condition).
    if (m_captureRunning) {
        StopCapture();
    }

    m_isWindowCapture = true;
    m_targetWindow = hWnd;

    // [Diag-3a] Reset per-init so each (re)initialization re-evaluates adapter
    // consistency and re-logs the verdict exactly once (plus on later changes).
    m_diagLastMonitor = nullptr;
    m_diagLastMismatch = false;
    m_diagFrameCounter = 0;
    m_lastFrameSource = FrameSource::None;

    if (!InitializeD3D11Device()) {
        LogMessage("D3D11 device init failed");
        return false;
    }

    if (!CreateWinRTDevice()) {
        LogMessage("WinRT device creation failed");
        return false;
    }

    if (!CreateCaptureItemForWindow(hWnd)) {
        LogMessage("Capture item creation failed");
        return false;
    }

    if (!StartCapture()) {
        LogMessage("Capture start failed");
        return false;
    }

    if (!InitializeImageProcessing()) {
        LogMessage("Image processing init failed, resize/crop may be affected");
    }

    LogMessage("WGCScreenCapture initialized successfully");
    return true;
}

void WGCScreenCapture::OnFrameArrived(
    Direct3D11CaptureFramePool const& sender,
    winrt::Windows::Foundation::IInspectable const& args) {

    try {
        // Hold m_processedMutex to prevent concurrent D3D context access
        // with ProcessFrame in GetFrame. TryGetNextFrame and frame pool
        // Recreate use the D3D device internally.
        std::lock_guard<std::mutex> plock(m_processedMutex);

        auto frame = sender.TryGetNextFrame();
        if (!frame) return;

        auto contentSize = frame.ContentSize();

        bool sizeChanged = (contentSize.Width != m_lastSize.Width || contentSize.Height != m_lastSize.Height);

        if (m_isFirstFrame || sizeChanged) {
            m_lastSize = contentSize;

            if (sizeChanged) {
                m_framePool.Recreate(
                    m_winrtDevice,
                    DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    2,
                    m_lastSize
                );
            }

            if (m_isWindowCapture && m_imageProcessConfig.enableROI) {
                UpdateClientAreaROI(contentSize.Width, contentSize.Height);
            }

            m_isFirstFrame = false;
        }

        auto surface = frame.Surface();
        auto texture = GetTextureFromDirect3DSurface(surface);

        if (texture) {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_currentFrame = texture;
            m_hasNewFrame = true;
            m_frameCondVar.notify_one();
        }
    }
    catch (const winrt::hresult_error& ex) {
        LogMessage("Frame callback exception: " + winrt::to_string(ex.message()));
    }
}

CComPtr<ID3D11Texture2D> WGCScreenCapture::GetTextureFromDirect3DSurface(
    IDirect3DSurface const& surface) {

    try {
        auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

        com_ptr<::ID3D11Texture2D> texture;
        HRESULT hr = access->GetInterface(
            __uuidof(::ID3D11Texture2D),
            texture.put_void()
        );

        if (FAILED(hr)) {
            LogMessage("Failed to get ID3D11Texture2D: " + std::to_string(hr));
            return nullptr;
        }

        CComPtr<ID3D11Texture2D> result;
        result.Attach(texture.detach());

        return result;
    }
    catch (const winrt::hresult_error& ex) {
        LogMessage("Get texture exception: " + winrt::to_string(ex.message()));
        return nullptr;
    }
    catch (...) {
        LogMessage("Unknown exception during get texture");
        return nullptr;
    }
}

void WGCScreenCapture::SetNewHandle(HWND& hWnd) {
    m_targetWindow = hWnd;
    if (m_captureRunning) {
        StopCapture();
        InitializeForWindow(hWnd);
    }
}

int WGCScreenCapture::GetMonitorIndex(HMONITOR& hm) {
    return 0;
}

bool WGCScreenCapture::GetFrame(cv::Mat& mat, int index) {
    if (!m_captureRunning) return false;

    // [Diag-3a] Adapter consistency observability (log-only, negligible per-frame cost).
    CheckAdapterConsistency();

    // Non-blocking read: grab the latest frame if available.
    CComPtr<ID3D11Texture2D> frameTexture;
    bool hasNewFrame = false;

    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_hasNewFrame) {
            frameTexture = m_currentFrame;
            m_hasNewFrame = false;
            hasNewFrame = true;
        }
    }

    if (hasNewFrame && frameTexture && m_d3dDevice) {
        // Hold m_processedMutex to prevent concurrent D3D context access
        // with OnFrameArrived's TryGetNextFrame/Recreate.
        cv::Mat processedMat;
        bool processOk = false;
        {
            std::lock_guard<std::mutex> plock(m_processedMutex);
            processOk = ProcessFrame(processedMat);
        }
        if (processOk && !processedMat.empty()) {
            // Cache the processed frame for reuse when no new WGC frame arrives.
            {
                std::lock_guard<std::mutex> plock(m_processedMutex);
                std::swap(m_latestFrameMat, processedMat);
            }
            mat = m_latestFrameMat;
            m_lastFrameSource = FrameSource::New;   // [Diag-v2]
            return true;
        }
        // ProcessFrame failed - fall through to cached/blank frame
    }

    // No new frame from WGC, or ProcessFrame failed.
    // Try to return the last cached processed frame.
    {
        std::lock_guard<std::mutex> plock(m_processedMutex);
        if (!m_latestFrameMat.empty()) {
            mat = m_latestFrameMat;
            m_lastFrameSource = FrameSource::Cached;   // [Diag-v2]
            return true;
        }
    }

    // [Diag-v2] No cached frame either - never received any usable WGC frame yet.
    // Previously this returned a blank 1920x1080 black frame as success, which
    // ValidateFrame then rejected as "black screen" - indistinguishable from a
    // real black frame. Return false so upstream can tell "no frame at all"
    // apart from "got a black frame" (both callers check the return value).
    // mat is left empty on purpose.
    m_lastFrameSource = FrameSource::None;
    return false;
}

// ============================================================
// [Diag-3a] Adapter consistency observability (log-only)
// Phase 3a of the WGC hardening plan: observation only - capture behavior is
// NEVER changed here. Cost: per call = one MonitorFromWindow (cheap); the
// heavy DXGI enumeration runs only at init, on monitor change, or every 300
// calls (~5s @ 60fps). Logging: healthy match logs once per init; a mismatch
// re-logs every ~5s while it persists so field logs always contain evidence.
// ============================================================
void WGCScreenCapture::CheckAdapterConsistency() {
    if (!m_deviceAdapterValid) return;
    if (!m_targetWindow || !IsWindow(m_targetWindow)) return;

    HMONITOR hMon = MonitorFromWindow(m_targetWindow, MONITOR_DEFAULTTONEAREST);
    if (!hMon) return;

    const bool monitorChanged = (hMon != m_diagLastMonitor);
    if (!monitorChanged) {
        m_diagFrameCounter++;
        if (m_diagFrameCounter < 300) return;
    }
    m_diagFrameCounter = 0;

    AdapterMatchResult mon = FindAdapterForMonitor(hMon);

    const bool mismatch = mon.found &&
        (mon.luid.LowPart != m_deviceAdapterLuid.LowPart ||
         mon.luid.HighPart != m_deviceAdapterLuid.HighPart);

    if (mismatch || mismatch != m_diagLastMismatch || monitorChanged) {
        char devA[256] = { 0 };
        WideCharToMultiByte(CP_ACP, 0, m_deviceAdapterDesc.c_str(), -1, devA, sizeof(devA), NULL, NULL);
        char monA[256] = { 0 };
        WideCharToMultiByte(CP_ACP, 0, mon.description.c_str(), -1, monA, sizeof(monA), NULL, NULL);
        DEBUG_LOG_FMT("[WGCScreenCapture][Diag-3a] Adapter consistency: deviceVen=0x%04X desc='%s' | monitorVen=0x%04X desc='%s' found=%d MISMATCH=%d monitorChanged=%d",
            m_deviceAdapterVendorId, devA, mon.vendorId, monA,
            (int)mon.found, (int)mismatch, (int)monitorChanged);
    }
    m_diagLastMismatch = mismatch;
    m_diagLastMonitor = hMon;
}

WGCScreenCapture::AdapterMatchResult WGCScreenCapture::FindAdapterForMonitor(HMONITOR hMon) {
    AdapterMatchResult result;
    CComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&factory)))) {
        return result;
    }
    for (UINT i = 0; ; ++i) {
        CComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;
        DXGI_ADAPTER_DESC1 ad;
        if (FAILED(adapter->GetDesc1(&ad))) continue;
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        for (UINT o = 0; ; ++o) {
            CComPtr<IDXGIOutput> output;
            HRESULT hrO = adapter->EnumOutputs(o, &output);
            if (hrO == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hrO)) continue;
            DXGI_OUTPUT_DESC od;
            if (FAILED(output->GetDesc(&od))) continue;
            if (od.Monitor == hMon) {
                result.found = true;
                result.luid = ad.AdapterLuid;
                result.vendorId = ad.VendorId;
                result.deviceId = ad.DeviceId;
                result.description = ad.Description;
                return result;
            }
        }
    }
    return result;
}

void WGCScreenCapture::ReleaseFrame(int index) {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    m_currentFrame.Release();
    m_hasNewFrame = false;
}

void WGCScreenCapture::SetFrameCallback(std::function<void(const FrameData&)> callback) {
    m_frameCallback = callback;
}

void WGCScreenCapture::SetFrameCallback(std::function<void(const cv::Mat&)> callback) {
    m_frameCallback_New = callback;
}

void WGCScreenCapture::StopCapture() {
    m_captureRunning = false;

    try {
        if (m_captureSession) {
            m_captureSession.Close();
            m_captureSession = nullptr;
        }

        if (m_framePool) {
            if (m_frameArrivedToken) {
                m_framePool.FrameArrived(m_frameArrivedToken);
            }
            m_framePool.Close();
            m_framePool = nullptr;
        }

        m_captureItem = nullptr;
    }
    catch (const winrt::hresult_error& ex) {
        LogMessage("Stop capture exception: " + winrt::to_string(ex.message()));
    }

    LogMessage("WGC capture stopped");
}

void WGCScreenCapture::LogMessage(const std::string& message) {
    std::string strTag = "[WGCScreenCapture] ";
    std::string strMessage = strTag + message;
    DEBUG_LOG_FMT(strMessage.c_str());
}

void WGCScreenCapture::SetImageProcessConfig(const ImageProcessConfig& config) {
    m_imageProcessConfig = config;

    if (config.enableResize || config.enableROI || config.enableCanvas) {
        InitializeImageProcessing();
    }
}

const WGCScreenCapture::ImageProcessConfig& WGCScreenCapture::GetImageProcessConfig() const {
    return m_imageProcessConfig;
}

void WGCScreenCapture::SetCursorCaptureEnabled(bool enabled) {
    m_captureCursor = enabled;
    if (m_captureSession) {
        m_captureSession.IsCursorCaptureEnabled(enabled);
    }
}

void WGCScreenCapture::SetBorderRequired(bool required) {
    m_borderRequired = required;
    if (m_captureSession) {
        auto session3 = m_captureSession.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>();
        if (session3) {
            session3.IsBorderRequired(required);
        }
        else {
            LogMessage("Current system does not support dynamic IsBorderRequired");
        }
    }
}

void WGCScreenCapture::SetCaptureClientAreaOnly(bool enable) {
    if (!m_targetWindow || !IsWindow(m_targetWindow)) {
        LogMessage("SetCaptureClientAreaOnly: Invalid window handle");
        return;
    }

    m_imageProcessConfig.enableROI = enable;

    if (enable) {
        SetRectEmpty(&m_imageProcessConfig.roiRect);
        m_isFirstFrame = true;
        LogMessage("Set client area only (will auto-calculate ROI on first frame)");
    }
    else {
        SetRectEmpty(&m_imageProcessConfig.roiRect);
        LogMessage("Restored to full client area");
    }

    SetImageProcessConfig(m_imageProcessConfig);
}

bool WGCScreenCapture::InitializeImageProcessing() {
    if (m_resizeShader && m_copyShader && m_bilinearSampler) {
        return true;
    }

    if (!CreateComputeShaders()) {
        LogMessage("Failed to create compute shaders");
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = m_d3dDevice->CreateSamplerState(&samplerDesc, &m_bilinearSampler);
    if (FAILED(hr)) {
        LogMessage("Failed to create sampler state: " + std::to_string(hr));
        return false;
    }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(float) * 16;
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
    if (FAILED(hr)) {
        LogMessage("Failed to create constant buffer: " + std::to_string(hr));
        return false;
    }

    return true;
}

bool WGCScreenCapture::CreateComputeShaders() {
    const char* resizeShaderCode = R"(
        Texture2D<float4> InputTexture : register(t0);
        RWTexture2D<float4> OutputTexture : register(u0);
        SamplerState BilinearSampler : register(s0);
        
        cbuffer TransformParams : register(b0)
        {
            float2 InputSize;
            float2 OutputSize;
            float4 SrcRect;
            float4 DstRect;
        };
        
        [numthreads(16, 16, 1)]
        void main(uint3 DTid : SV_DispatchThreadID)
        {
            if (DTid.x >= OutputSize.x || DTid.y >= OutputSize.y)
                return;
                
            float2 uv = float2(DTid.xy) / OutputSize;
            float2 inputUV = float2(
                SrcRect.x + uv.x * SrcRect.z,
                SrcRect.y + uv.y * SrcRect.w
            ) / InputSize;
            
            float4 color = InputTexture.SampleLevel(BilinearSampler, inputUV, 0);
            OutputTexture[DTid.xy] = color;
        }
    )";

    const char* copyShaderCode = R"(
        Texture2D<float4> InputTexture : register(t0);
        RWTexture2D<float4> OutputTexture : register(u0);
        
        cbuffer CopyParams : register(b0)
        {
            float2 InputSize;
            float2 OutputSize;
            float4 SrcRect;
            float4 DstRect;
        };
        
        [numthreads(16, 16, 1)]
        void main(uint3 DTid : SV_DispatchThreadID)
        {
            if (DTid.x >= OutputSize.x || DTid.y >= OutputSize.y)
                return;
                
            if (DTid.x >= DstRect.x && DTid.x < DstRect.x + DstRect.z &&
                DTid.y >= DstRect.y && DTid.y < DstRect.y + DstRect.w)
            {
                uint2 srcCoord;
                srcCoord.x = (DTid.x - DstRect.x) * SrcRect.z / DstRect.z + SrcRect.x;
                srcCoord.y = (DTid.y - DstRect.y) * SrcRect.w / DstRect.w + SrcRect.y;
                
                if (srcCoord.x < InputSize.x && srcCoord.y < InputSize.y)
                {
                    OutputTexture[DTid.xy] = InputTexture[srcCoord];
                }
                else
                {
                    OutputTexture[DTid.xy] = float4(0, 0, 0, 1);
                }
            }
        }
    )";

    ID3DBlob* resizeShaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = D3DCompile(
        resizeShaderCode, strlen(resizeShaderCode),
        "ResizeShader", nullptr, nullptr,
        "main", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &resizeShaderBlob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::string errorMsg = "Resize shader compile failed: ";
            errorMsg += std::string((char*)errorBlob->GetBufferPointer(), errorBlob->GetBufferSize());
            LogMessage(errorMsg);
            errorBlob->Release();
        }
        if (resizeShaderBlob) resizeShaderBlob->Release();
        return false;
    }

    hr = m_d3dDevice->CreateComputeShader(
        resizeShaderBlob->GetBufferPointer(),
        resizeShaderBlob->GetBufferSize(),
        nullptr,
        &m_resizeShader
    );
    resizeShaderBlob->Release();

    if (FAILED(hr)) {
        LogMessage("Failed to create resize compute shader: " + std::to_string(hr));
        return false;
    }

    ID3DBlob* copyShaderBlob = nullptr;
    hr = D3DCompile(
        copyShaderCode, strlen(copyShaderCode),
        "CopyShader", nullptr, nullptr,
        "main", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &copyShaderBlob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::string errorMsg = "Copy shader compile failed: ";
            errorMsg += std::string((char*)errorBlob->GetBufferPointer(), errorBlob->GetBufferSize());
            LogMessage(errorMsg);
            errorBlob->Release();
        }
        if (copyShaderBlob) copyShaderBlob->Release();
        return false;
    }

    hr = m_d3dDevice->CreateComputeShader(
        copyShaderBlob->GetBufferPointer(),
        copyShaderBlob->GetBufferSize(),
        nullptr,
        &m_copyShader
    );
    copyShaderBlob->Release();

    if (FAILED(hr)) {
        LogMessage("Failed to create copy compute shader: " + std::to_string(hr));
        return false;
    }

    return true;
}

bool WGCScreenCapture::ProcessFrame(cv::Mat& frameData) {
    CComPtr<ID3D11Texture2D> processedTexture;

    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (!m_currentFrame) {
            return false;
        }

        if (!ProcessFrameInner(processedTexture)) {
            return false;
        }
    }

    if (!processedTexture) {
        return false;
    }

    D3D11_TEXTURE2D_DESC processedDesc;
    processedTexture->GetDesc(&processedDesc);

    bool needNewStagingTexture = false;
    if (!m_stagingTexture) {
        needNewStagingTexture = true;
    }
    else {
        D3D11_TEXTURE2D_DESC texDesc;
        m_stagingTexture->GetDesc(&texDesc);
        if (texDesc.Width != processedDesc.Width || texDesc.Height != processedDesc.Height) {
            m_stagingTexture.Release();
            needNewStagingTexture = true;
        }
    }

    if (needNewStagingTexture) {
        if (!CreateStagingTexture(processedDesc.Width, processedDesc.Height)) {
            LogMessage("Failed to create staging texture");
            return false;
        }
    }

    m_d3dContext->CopyResource(m_stagingTexture, processedTexture);
    return ToCVMat(m_stagingTexture, frameData);
}

bool WGCScreenCapture::ProcessFrameInner(CComPtr<ID3D11Texture2D>& processedTexture) {
    ResetResourceCache();

    if (!m_currentFrame) {
        LogMessage("ProcessFrameInner: no current frame");
        return false;
    }

    CComPtr<ID3D11Texture2D> sourceTexture = m_currentFrame;

    CComPtr<ID3D11Texture2D> resizeTexture;
    bool processResult = ProcessImageWithShaders(sourceTexture, resizeTexture, m_imageProcessConfig);

    if (!processResult || !resizeTexture) {
        resizeTexture = sourceTexture;
    }

    processedTexture = resizeTexture;
    CleanupResourceCache();

    return processResult;
}

bool WGCScreenCapture::CreateStagingTexture(uint32_t width, uint32_t height) {
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
    if (FAILED(hr)) {
        LogMessage("Create staging texture failed: " + std::to_string(hr));
        return false;
    }

    return true;
}

bool WGCScreenCapture::ToCVMat(ID3D11Texture2D* inputTexture, cv::Mat& outputMat) {
    D3D11_TEXTURE2D_DESC texDesc;
    inputTexture->GetDesc(&texDesc);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_d3dContext->Map(inputTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
    if (FAILED(hr)) {
        // [Diag] Include HRESULT + texture geometry + device state. A recurring DXGI_ERROR_*
        // here on the staging texture is the fingerprint of game-side capture blocking
        // (WDA_EXCLUDEFROMCAPTURE / exclusive-fullscreen device churn) rather than a random
        // WGC hiccup, and is impossible to distinguish from a "Map resource failed" alone.
        HRESULT removed = S_OK;
        if (m_d3dDevice) removed = m_d3dDevice->GetDeviceRemovedReason();
        DEBUG_LOG_FMT("[WGCScreenCapture] Map failed hr=0x%08X deviceRemoved=0x%08X tex=%ux%u fmt=%d usage=%d",
            (unsigned)hr, (unsigned)removed, texDesc.Width, texDesc.Height,
            (int)texDesc.Format, (int)texDesc.Usage);
        return false;
    }

    const int rows = texDesc.Height;
    const int cols = texDesc.Width;

    cv::Mat bgra(rows, cols, CV_8UC4, mappedResource.pData, mappedResource.RowPitch);
    cv::cvtColor(bgra, outputMat, cv::COLOR_BGRA2BGR);

    m_d3dContext->Unmap(inputTexture, 0);
    return true;
}

CComPtr<ID3D11Texture2D> WGCScreenCapture::CreateTexture2D(
    uint32_t width, uint32_t height,
    DXGI_FORMAT format,
    D3D11_USAGE usage,
    UINT bindFlags) {

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = usage;
    desc.BindFlags = bindFlags;
    desc.CPUAccessFlags = (usage == D3D11_USAGE_STAGING) ? D3D11_CPU_ACCESS_READ : 0;
    desc.MiscFlags = 0;

    CComPtr<ID3D11Texture2D> texture;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, &texture);

    if (FAILED(hr)) {
        LogMessage("Create texture failed: " + std::to_string(hr));
        return nullptr;
    }

    return texture;
}

bool WGCScreenCapture::ProcessImageWithShaders(
    ID3D11Texture2D* srcTexture,
    CComPtr<ID3D11Texture2D>& processedTexture,
    const ImageProcessConfig& imageProcessConfig) {

    if (!srcTexture || !m_d3dDevice || !m_d3dContext) {
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc;
    srcTexture->GetDesc(&srcDesc);

    if (!InitializeImageProcessing()) {
        LogMessage("Image processing init failed");
        return false;
    }

    CComPtr<ID3D11Texture2D> currentTexture = srcTexture;
    CComPtr<ID3D11ShaderResourceView> currentSRV;

    if (imageProcessConfig.enableROI) {
        RECT roiRect = imageProcessConfig.roiRect;
        roiRect.left = std::max(0L, roiRect.left);
        roiRect.top = std::max(0L, roiRect.top);
        roiRect.right = std::min((LONG)srcDesc.Width, roiRect.right);
        roiRect.bottom = std::min((LONG)srcDesc.Height, roiRect.bottom);

        if (roiRect.right > roiRect.left && roiRect.bottom > roiRect.top) {
            uint32_t roiWidth = roiRect.right - roiRect.left;
            uint32_t roiHeight = roiRect.bottom - roiRect.top;

            CComPtr<ID3D11Texture2D> roiTexture = GetCachedTexture(roiWidth, roiHeight);

            if (!roiTexture) {
                LogMessage("ROI texture creation failed");
                return false;
            }

            D3D11_BOX sourceBox;
            sourceBox.left = roiRect.left;
            sourceBox.top = roiRect.top;
            sourceBox.right = roiRect.right;
            sourceBox.bottom = roiRect.bottom;
            sourceBox.front = 0;
            sourceBox.back = 1;

            m_d3dContext->CopySubresourceRegion(
                roiTexture, 0, 0, 0, 0,
                currentTexture, 0, &sourceBox
            );

            currentTexture = roiTexture;
        }
    }

    if (imageProcessConfig.enableResize &&
        imageProcessConfig.resizeWidth > 0 &&
        imageProcessConfig.resizeHeight > 0) {

        D3D11_TEXTURE2D_DESC currentDesc;
        currentTexture->GetDesc(&currentDesc);

        CComPtr<ID3D11Texture2D> resizedTexture = GetCachedTexture(
            imageProcessConfig.resizeWidth,
            imageProcessConfig.resizeHeight);

        if (!resizedTexture) {
            LogMessage("Failed to create resized texture");
            return false;
        }

        if (!currentSRV) {
            if (!GetCachedSRV(currentTexture, currentSRV)) {
                LogMessage("Failed to get shader resource view");
                return false;
            }
        }

        CComPtr<ID3D11UnorderedAccessView> resizedUAV;
        if (!GetCachedUAV(resizedTexture, resizedUAV)) {
            LogMessage("Failed to get unordered access view");
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mappedResource;
        HRESULT hr = m_d3dContext->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        if (SUCCEEDED(hr)) {
            float* data = (float*)mappedResource.pData;
            data[0] = (float)currentDesc.Width;
            data[1] = (float)currentDesc.Height;
            data[2] = (float)imageProcessConfig.resizeWidth;
            data[3] = (float)imageProcessConfig.resizeHeight;
            data[4] = 0.0f;
            data[5] = 0.0f;
            data[6] = (float)currentDesc.Width;
            data[7] = (float)currentDesc.Height;
            data[8] = 0.0f;
            data[9] = 0.0f;
            data[10] = (float)imageProcessConfig.resizeWidth;
            data[11] = (float)imageProcessConfig.resizeHeight;

            m_d3dContext->Unmap(m_constantBuffer, 0);

            m_d3dContext->CSSetShader(m_resizeShader, nullptr, 0);
            m_d3dContext->CSSetShaderResources(0, 1, &currentSRV.p);
            m_d3dContext->CSSetUnorderedAccessViews(0, 1, &resizedUAV.p, nullptr);
            m_d3dContext->CSSetSamplers(0, 1, &m_bilinearSampler.p);
            m_d3dContext->CSSetConstantBuffers(0, 1, &m_constantBuffer.p);

            uint32_t threadGroupX = (imageProcessConfig.resizeWidth + 15) / 16;
            uint32_t threadGroupY = (imageProcessConfig.resizeHeight + 15) / 16;
            m_d3dContext->Dispatch(threadGroupX, threadGroupY, 1);

            ID3D11ShaderResourceView* nullSRV = nullptr;
            ID3D11UnorderedAccessView* nullUAV = nullptr;
            ID3D11SamplerState* nullSampler = nullptr;
            ID3D11Buffer* nullBuffer = nullptr;

            m_d3dContext->CSSetShaderResources(0, 1, &nullSRV);
            m_d3dContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
            m_d3dContext->CSSetSamplers(0, 1, &nullSampler);
            m_d3dContext->CSSetConstantBuffers(0, 1, &nullBuffer);

            currentTexture = resizedTexture;
            currentSRV.Release();
        }
    }

    processedTexture = currentTexture;
    return true;
}

CComPtr<ID3D11Texture2D> WGCScreenCapture::GetCachedTexture(
    uint32_t width, uint32_t height,
    DXGI_FORMAT format,
    D3D11_USAGE usage,
    UINT bindFlags) {

    for (auto& entry : m_textureCache) {
        if (!entry.inUse &&
            entry.width == width &&
            entry.height == height &&
            entry.format == format &&
            entry.usage == usage &&
            entry.bindFlags == bindFlags) {

            entry.inUse = true;
            return entry.texture;
        }
    }

    CComPtr<ID3D11Texture2D> newTexture = CreateTexture2D(width, height, format, usage, bindFlags);
    if (newTexture) {
        TextureCacheEntry newEntry;
        newEntry.texture = newTexture;
        newEntry.width = width;
        newEntry.height = height;
        newEntry.format = format;
        newEntry.usage = usage;
        newEntry.bindFlags = bindFlags;
        newEntry.inUse = true;
        m_textureCache.push_back(newEntry);
    }

    return newTexture;
}

bool WGCScreenCapture::GetCachedSRV(ID3D11Texture2D* texture, CComPtr<ID3D11ShaderResourceView>& srv) {
    if (!texture) return false;

    for (auto& entry : m_viewCache) {
        if (!entry.inUse && entry.texture == texture && entry.srv) {
            entry.inUse = true;
            srv = entry.srv;
            return true;
        }
    }

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        LogMessage("Texture does not support SRV");
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    HRESULT hr = m_d3dDevice->CreateShaderResourceView(texture, &srvDesc, &srv);
    if (FAILED(hr)) {
        LogMessage("Create SRV failed: " + std::to_string(hr));
        return false;
    }

    ViewCacheEntry newEntry;
    newEntry.srv = srv;
    newEntry.uav = nullptr;
    newEntry.texture = texture;
    newEntry.inUse = true;
    m_viewCache.push_back(newEntry);

    return true;
}

bool WGCScreenCapture::GetCachedUAV(ID3D11Texture2D* texture, CComPtr<ID3D11UnorderedAccessView>& uav) {
    if (!texture) return false;

    for (auto& entry : m_viewCache) {
        if (!entry.inUse && entry.texture == texture && entry.uav) {
            entry.inUse = true;
            uav = entry.uav;
            return true;
        }
    }

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    if (!(desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) {
        LogMessage("Texture does not support UAV");
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = desc.Format;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;

    HRESULT hr = m_d3dDevice->CreateUnorderedAccessView(texture, &uavDesc, &uav);
    if (FAILED(hr)) {
        LogMessage("Create UAV failed: " + std::to_string(hr));
        return false;
    }

    ViewCacheEntry newEntry;
    newEntry.srv = nullptr;
    newEntry.uav = uav;
    newEntry.texture = texture;
    newEntry.inUse = true;
    m_viewCache.push_back(newEntry);

    return true;
}

bool WGCScreenCapture::Reset() {
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_currentFrame.Release();
        m_hasNewFrame = false;
    }

    if (m_targetWindow && IsWindow(m_targetWindow)) {
        return InitializeForWindow(m_targetWindow);
    }

    LogMessage("Reset: no valid target window");
    return false;
}

void WGCScreenCapture::ResetResourceCache() {
    for (auto& entry : m_textureCache) {
        entry.inUse = false;
    }
    for (auto& entry : m_viewCache) {
        entry.inUse = false;
    }
}

void WGCScreenCapture::CleanupResourceCache() {
    const size_t MAX_TEXTURE_CACHE_SIZE = 10;
    const size_t MAX_VIEW_CACHE_SIZE = 10;

    if (m_textureCache.size() > MAX_TEXTURE_CACHE_SIZE) {
        auto newEnd = std::remove_if(m_textureCache.begin(), m_textureCache.end(),
            [](const TextureCacheEntry& entry) { return !entry.inUse; });

        if (std::distance(m_textureCache.begin(), newEnd) > MAX_TEXTURE_CACHE_SIZE) {
            newEnd = m_textureCache.begin() + MAX_TEXTURE_CACHE_SIZE;
        }

        m_textureCache.erase(newEnd, m_textureCache.end());
    }

    if (m_viewCache.size() > MAX_VIEW_CACHE_SIZE) {
        auto newEnd = std::remove_if(m_viewCache.begin(), m_viewCache.end(),
            [](const ViewCacheEntry& entry) { return !entry.inUse; });

        if (std::distance(m_viewCache.begin(), newEnd) > MAX_VIEW_CACHE_SIZE) {
            newEnd = m_viewCache.begin() + MAX_VIEW_CACHE_SIZE;
        }

        m_viewCache.erase(newEnd, m_viewCache.end());
    }
}

void WGCScreenCapture::UpdateClientAreaROI(uint32_t textureWidth, uint32_t textureHeight) {
    if (!m_targetWindow || !IsWindow(m_targetWindow)) return;

    HMONITOR hMonitor = MonitorFromWindow(m_targetWindow, MONITOR_DEFAULTTONEAREST);

    MONITORINFOEX info;
    info.cbSize = sizeof(MONITORINFOEX);
    GetMonitorInfo(hMonitor, &info);
    int cxLogical = (info.rcMonitor.right - info.rcMonitor.left);

    DEVMODE dm;
    dm.dmSize = sizeof(dm);
    dm.dmDriverExtra = 0;
    EnumDisplaySettings(info.szDevice, ENUM_CURRENT_SETTINGS, &dm);
    int cxPhysical = dm.dmPelsWidth;

    double dpiScale = 1.0;
    if (cxLogical > 0 && cxPhysical > 0) {
        dpiScale = (double)cxPhysical / (double)cxLogical;
    }

    RECT clientRect;
    GetClientRect(m_targetWindow, &clientRect);
    int logicalClientW = clientRect.right - clientRect.left;
    int logicalClientH = clientRect.bottom - clientRect.top;

    int clientW = (int)(logicalClientW * dpiScale);
    int clientH = (int)(logicalClientH * dpiScale);

    int diffW = (int)textureWidth - clientW;
    int diffH = (int)textureHeight - clientH;

    if (diffW <= 4 && diffH <= 4) {
        m_imageProcessConfig.roiRect.left = 0;
        m_imageProcessConfig.roiRect.top = 0;
        m_imageProcessConfig.roiRect.right = textureWidth;
        m_imageProcessConfig.roiRect.bottom = textureHeight;
        LogMessage("ROI: matched client size (no border) - DPI adjusted");
    }
    else {
        int borderX = diffW / 2;
        if (borderX < 0) borderX = 0;

        int borderBottom = borderX;
        int titleBarH = diffH - borderBottom;
        if (titleBarH < 0) titleBarH = 0;

        m_imageProcessConfig.roiRect.left = borderX;
        m_imageProcessConfig.roiRect.top = titleBarH;
        m_imageProcessConfig.roiRect.right = borderX + clientW;
        m_imageProcessConfig.roiRect.bottom = titleBarH + clientH;

        if (m_imageProcessConfig.roiRect.right > (LONG)textureWidth)
            m_imageProcessConfig.roiRect.right = (LONG)textureWidth;
        if (m_imageProcessConfig.roiRect.bottom > (LONG)textureHeight)
            m_imageProcessConfig.roiRect.bottom = (LONG)textureHeight;

        char logMsg[128];
        sprintf_s(logMsg, "ROI: has border. Offset(%d, %d)", borderX, titleBarH);
        LogMessage(logMsg);
    }

    m_imageProcessConfig.enableROI = true;
    SetImageProcessConfig(m_imageProcessConfig);
}

CComPtr<ID3D11Texture2D> WGCScreenCapture::GetROI(ID3D11Texture2D* srcTexture, const RECT& roiRect) {
    ImageProcessConfig config;
    config.enableROI = true;
    config.roiRect = roiRect;

    CComPtr<ID3D11Texture2D> result;
    ProcessImageWithShaders(srcTexture, result, config);
    return result;
}

bool WGCScreenCapture::CopyToCanvas(ID3D11Texture2D* srcTexture, ID3D11Texture2D* dstTexture, const RECT& dstRect) {
    if (!srcTexture || !dstTexture) return false;

    D3D11_BOX srcBox;
    srcBox.left = 0;
    srcBox.top = 0;

    D3D11_TEXTURE2D_DESC srcDesc;
    srcTexture->GetDesc(&srcDesc);
    srcBox.right = srcDesc.Width;
    srcBox.bottom = srcDesc.Height;
    srcBox.front = 0;
    srcBox.back = 1;

    m_d3dContext->CopySubresourceRegion(
        dstTexture, 0,
        dstRect.left, dstRect.top, 0,
        srcTexture, 0, &srcBox
    );

    return true;
}

CComPtr<ID3D11Texture2D> WGCScreenCapture::ResizeTexture(ID3D11Texture2D* srcTexture, uint32_t newWidth, uint32_t newHeight) {
    ImageProcessConfig config;
    config.enableResize = true;
    config.resizeWidth = newWidth;
    config.resizeHeight = newHeight;

    CComPtr<ID3D11Texture2D> result;
    ProcessImageWithShaders(srcTexture, result, config);
    return result;
}

bool WGCScreenCapture::ProcessFrame(FrameData& frameData) {
    CComPtr<ID3D11Texture2D> processedTexture;
    if (!ProcessFrameInner(processedTexture)) {
        return false;
    }
    if (!processedTexture) return false;

    D3D11_TEXTURE2D_DESC processedDesc;
    processedTexture->GetDesc(&processedDesc);

    if (!m_stagingTexture) {
        if (!CreateStagingTexture(processedDesc.Width, processedDesc.Height)) {
            return false;
        }
    }

    m_d3dContext->CopyResource(m_stagingTexture, processedTexture);
    return CopyFrameToBuffer(m_stagingTexture, frameData);
}

bool WGCScreenCapture::CopyFrameToBuffer(ID3D11Texture2D* texture, FrameData& frameData) {
    if (!texture) return false;

    D3D11_TEXTURE2D_DESC texDesc;
    texture->GetDesc(&texDesc);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_d3dContext->Map(texture, 0, D3D11_MAP_READ, 0, &mappedResource);
    if (FAILED(hr)) {
        LogMessage("Copy frame buffer map failed: " + std::to_string(hr));
        return false;
    }

    uint32_t width = texDesc.Width;
    uint32_t height = texDesc.Height;
    uint32_t stride = mappedResource.RowPitch;
    size_t bufferSize = stride * height;

    if (frameData.buffer.size() != bufferSize) {
        frameData.buffer.resize(bufferSize);
    }

    const uint8_t* srcData = static_cast<const uint8_t*>(mappedResource.pData);
    memcpy(frameData.buffer.data(), srcData, bufferSize);

    m_d3dContext->Unmap(texture, 0);

    frameData.width = width;
    frameData.height = height;
    frameData.stride = stride;

    return true;
}

bool WGCScreenCapture::GetCachedViews(
    ID3D11Texture2D* texture,
    CComPtr<ID3D11ShaderResourceView>& srv,
    CComPtr<ID3D11UnorderedAccessView>& uav) {

    bool srvResult = GetCachedSRV(texture, srv);
    bool uavResult = GetCachedUAV(texture, uav);

    return srvResult && uavResult;
}

bool WGCScreenCapture::UpdateConstantBuffer(const RECT& srcRect, const RECT& dstRect) {
    if (!m_constantBuffer || !m_d3dContext) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_d3dContext->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (FAILED(hr)) {
        LogMessage("Map constant buffer failed: " + std::to_string(hr));
        return false;
    }

    float* data = (float*)mappedResource.pData;
    data[0] = (float)srcRect.left;
    data[1] = (float)srcRect.top;
    data[2] = (float)(srcRect.right - srcRect.left);
    data[3] = (float)(srcRect.bottom - srcRect.top);
    data[4] = (float)dstRect.left;
    data[5] = (float)dstRect.top;
    data[6] = (float)(dstRect.right - dstRect.left);
    data[7] = (float)(dstRect.bottom - dstRect.top);

    m_d3dContext->Unmap(m_constantBuffer, 0);
    return true;
}
