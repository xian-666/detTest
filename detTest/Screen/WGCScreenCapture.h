// WGCScreenCapture.h
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <atlbase.h>
#include <d3dcompiler.h>
#include <opencv2/opencv.hpp>

// WinRT headers
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <inspectable.h>
#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowsapp.lib")

class WGCScreenCapture {
private:
    WGCScreenCapture();
public:
    struct FrameData {
        std::vector<uint8_t> buffer;  // RGBA pixel format
        uint32_t width;
        uint32_t height;
        uint32_t stride;      // bytes per row
    };

    // Image processing config struct (kept consistent with DXGIScreenCapture)
    struct ImageProcessConfig {
        bool enableResize;           // whether resize is enabled
        uint32_t resizeWidth;        // target resize width
        uint32_t resizeHeight;       // target resize height
        bool enableROI;              // whether ROI cropping is enabled
        RECT roiRect;                // ROI rectangle
        bool enableCanvas;           // whether canvas composition is enabled
        RECT canvasRect;             // canvas size and position
        RECT destRect;               // target position of the ROI within the canvas

        ImageProcessConfig() :
            enableResize(false), resizeWidth(0), resizeHeight(0),
            enableROI(false), enableCanvas(false) {
            SetRectEmpty(&roiRect);
            SetRectEmpty(&canvasRect);
            SetRectEmpty(&destRect);
        }

        void Reset() {
            enableResize = false;
            enableROI = false;
            enableCanvas = false;
            resizeWidth = 0;
            resizeHeight = 0;
            SetRectEmpty(&roiRect);
            SetRectEmpty(&canvasRect);
            SetRectEmpty(&destRect);
        }
    };

    ~WGCScreenCapture();

    static WGCScreenCapture& GetInstance();

    // Captures the specified window; interface kept consistent with DXGI
    bool InitializeForWindow(HWND hWnd);

    void SetNewHandle(HWND& hWnd);

    // Data interface: WGC internally uses monitor index
    int GetMonitorIndex(HMONITOR& hm);

    // Get the frame with the given index (for internal debugging)
    bool GetFrame(cv::Mat& mat, int index = 0);

    // Release the current frame resource (data interface)
    void ReleaseFrame(int index = 0);

    // Set the frame callback
    void SetFrameCallback(std::function<void(const FrameData&)> callback);
    void SetFrameCallback(std::function<void(const cv::Mat&)> callback);

    // Stop the capture loop
    void StopCapture();

    // Log a message
    static void LogMessage(const std::string& message);

    // Set the image processing config
    void SetImageProcessConfig(const ImageProcessConfig& config);

    // Get the current image processing config
    const ImageProcessConfig& GetImageProcessConfig() const;

    // Get the ROI of a texture
    CComPtr<ID3D11Texture2D> GetROI(ID3D11Texture2D* srcTexture, const RECT& roiRect);

    // Copy the source texture to the target canvas at the specified position
    bool CopyToCanvas(ID3D11Texture2D* srcTexture, ID3D11Texture2D* dstTexture, const RECT& dstRect);

    // Resize a texture
    CComPtr<ID3D11Texture2D> ResizeTexture(ID3D11Texture2D* srcTexture, uint32_t newWidth, uint32_t newHeight);

    // WGC control
    void SetCursorCaptureEnabled(bool enabled);
    void SetBorderRequired(bool required);
    void SetCaptureClientAreaOnly(bool enable);

    // Check whether reinitialization is needed (device lost / no new frame for a long time)
    bool NeedsReinitialization() const { return m_needsReinitialization.load(); }

    // Full device reset flow (release + reinitialize)
    bool Reset();

    // [Diag-v2] Source of the last GetFrame result, for remote diagnosis of
    // intermittent black frames (memory-starved iGPU shared memory / DWM cross-adapter copy).
    //   none   : never received any usable WGC frame (GetFrame returns false)
    //   new    : a freshly processed WGC frame was returned
    //   cached : the last processed frame was reused (no new frame this call)
    enum class FrameSource { None = 0, New = 1, Cached = 2 };

    const char* GetLastFrameSourceStr() const {
        switch (m_lastFrameSource) {
        case FrameSource::New:    return "new";
        case FrameSource::Cached: return "cached";
        default:                  return "none";
        }
    }
private:
    // D3D11 device
    CComPtr<ID3D11Device> m_d3dDevice;
    CComPtr<ID3D11DeviceContext> m_d3dContext;
    CComPtr<IDXGIDevice> m_dxgiDevice;

    // WGC capture objects
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_captureItem{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_captureSession{ nullptr };
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };

    // Frame-arrival event token
    winrt::event_token m_frameArrivedToken;

    // Current frame
    CComPtr<ID3D11Texture2D> m_currentFrame;
    std::mutex m_frameMutex;
    std::condition_variable m_frameCondVar;
    bool m_hasNewFrame;

    // [D3D race prevention] Mutex to prevent concurrent D3D context access
    // between OnFrameArrived (TryGetNextFrame/Recreate) and ProcessFrame (GetFrame).
    std::mutex m_processedMutex;
    // [Frame cache] Cached processed frame for latest-only reads without D3D race.
    cv::Mat m_latestFrameMat;

    // Capture info kept consistent with DXGI
    RECT m_captureRect;
    bool m_isWindowCapture;
    HWND m_targetWindow;

    // Intermediate resource for data transfer
    CComPtr<ID3D11Texture2D> m_stagingTexture;

    // Frame callback
    std::function<void(const FrameData&)> m_frameCallback;
    std::function<void(const cv::Mat&)> m_frameCallback_New;

    // Capture running flag
    bool m_captureRunning;

    // Image processing config
    ImageProcessConfig m_imageProcessConfig;

    // Image-processing resources (shader system kept consistent with DXGI)
    CComPtr<ID3D11ComputeShader> m_resizeShader;
    CComPtr<ID3D11ComputeShader> m_copyShader;
    CComPtr<ID3D11SamplerState> m_bilinearSampler;
    CComPtr<ID3D11UnorderedAccessView> m_outputUAV;
    CComPtr<ID3D11ShaderResourceView> m_inputSRV;
    CComPtr<ID3D11Buffer> m_constantBuffer;

    // WGC specific
    bool m_captureCursor;
    bool m_borderRequired;

    // Device-lost / long-time-no-frame detection
    std::atomic<bool> m_needsReinitialization;
    std::chrono::steady_clock::time_point m_lastNewFrameTime;

    // [Diag-v2] Source of the last GetFrame result. Written and read on the same
    // thread as the GetFrame caller (capture/main-loop thread); not touched by
    // OnFrameArrived, so no atomic needed.
    FrameSource m_lastFrameSource = FrameSource::None;

    // [Diag-3a] Adapter consistency observability (log-only, never changes capture behavior).
    // Compares the adapter the WGC D3D device was created on (the "default" adapter) with
    // the adapter that owns the target window's monitor. Mismatch => every captured frame
    // takes a DWM cross-adapter copy, the prime suspect for intermittent black frames on
    // hybrid-GPU laptops under memory pressure.
    LUID m_deviceAdapterLuid{};
    bool m_deviceAdapterValid = false;
    std::wstring m_deviceAdapterDesc;
    UINT m_deviceAdapterVendorId = 0;
    UINT m_deviceAdapterDeviceId = 0;
    HMONITOR m_diagLastMonitor = nullptr;
    bool m_diagLastMismatch = false;
    UINT m_diagFrameCounter = 0;

    struct AdapterMatchResult {
        bool found = false;
        LUID luid{};
        UINT vendorId = 0;
        UINT deviceId = 0;
        std::wstring description;
    };
    AdapterMatchResult FindAdapterForMonitor(HMONITOR hMon);
    void CheckAdapterConsistency();

    // Initialization
    bool InitializeD3D11Device();
    bool CreateWinRTDevice();
    bool CreateCaptureItemForWindow(HWND hWnd);
    bool StartCapture();
    void OnFrameArrived(
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const& args);

    bool ProcessFrame(cv::Mat& frameData);  // SEH wrapper
    bool ProcessFrameImpl(cv::Mat& frameData);  // actual implementation
    bool ProcessFrame(FrameData& frameData);
    bool ProcessFrameInner(CComPtr<ID3D11Texture2D>& processedTexture);

    bool CreateStagingTexture(uint32_t width, uint32_t height);
    bool CopyFrameToBuffer(ID3D11Texture2D* texture, FrameData& frameData);
    bool ToCVMat(ID3D11Texture2D* inputTexture, cv::Mat& outputMat);

    CComPtr<ID3D11Texture2D> GetTextureFromDirect3DSurface(
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface const& surface);

    // Image-processing methods (logic kept consistent with DXGI)
    bool InitializeImageProcessing();
    bool ProcessImageWithShaders(
        ID3D11Texture2D* srcTexture,
        CComPtr<ID3D11Texture2D>& processedTexture,
        const ImageProcessConfig& imageProcessConfig);

    CComPtr<ID3D11Texture2D> CreateTexture2D(
        uint32_t width, uint32_t height,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
        UINT bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);

    bool CreateComputeShaders();
    bool UpdateConstantBuffer(const RECT& srcRect, const RECT& dstRect);

    // Texture cache (similar to DXGI style)
    struct TextureCacheEntry {
        CComPtr<ID3D11Texture2D> texture;
        uint32_t width;
        uint32_t height;
        DXGI_FORMAT format;
        D3D11_USAGE usage;
        UINT bindFlags;
        bool inUse;
    };
    std::vector<TextureCacheEntry> m_textureCache;

    struct ViewCacheEntry {
        CComPtr<ID3D11ShaderResourceView> srv;
        CComPtr<ID3D11UnorderedAccessView> uav;
        ID3D11Texture2D* texture;
        bool inUse;
    };
    std::vector<ViewCacheEntry> m_viewCache;

    CComPtr<ID3D11Texture2D> GetCachedTexture(
        uint32_t width, uint32_t height,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
        UINT bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);

    bool GetCachedViews(
        ID3D11Texture2D* texture,
        CComPtr<ID3D11ShaderResourceView>& srv,
        CComPtr<ID3D11UnorderedAccessView>& uav);

    bool GetCachedUAV(ID3D11Texture2D* texture, CComPtr<ID3D11UnorderedAccessView>& uav);
    bool GetCachedSRV(ID3D11Texture2D* texture, CComPtr<ID3D11ShaderResourceView>& srv);

    void ResetResourceCache();
    void CleanupResourceCache();

    // Target size (used to compute the DXGI layout)
    cv::Size m_targetGameScreenSize;
    cv::Rect m_targetWeaponPos;
    cv::Rect m_targetCarPos;
    
    bool m_isFirstFrame = true;
    winrt::Windows::Graphics::SizeInt32 m_lastSize = { 0, 0 };
    void UpdateClientAreaROI(uint32_t textureWidth, uint32_t textureHeight);
};