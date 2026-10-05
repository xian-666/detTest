// DXGIScreenCapture.h
#pragma once

#include <windows.h>
#include <atlbase.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <d3dcompiler.h> // Add shader compilation support
#include <opencv2/opencv.hpp>
#include <mutex>
#include <atomic>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib") // Link the shader compiler library

class DXGIScreenCapture {
private:
    DXGIScreenCapture();
public:
    struct FrameData {
        std::vector<uint8_t> buffer;  // RGBA format data
        uint32_t width;
        uint32_t height;
        uint32_t stride;      // Bytes per row
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
    };

    // Image processing configuration struct
    struct ImageProcessConfig {
        bool enableResize;           // Enable resizing
        uint32_t resizeWidth;        // Target resize width
        uint32_t resizeHeight;       // Target resize height
        bool enableROI;              // Enable ROI extraction
        RECT roiRect;                // ROI region
        bool enableCanvas;           // Enable canvas composition
        RECT canvasRect;             // Canvas size and position
        RECT destRect;               // ROI target position on canvas

        // Constructor, sets default values
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

    struct ImageProcessConfigEx {
        bool enableResize;           // Enable resizing
        uint32_t resizeWidth;        // Target resize width
        uint32_t resizeHeight;       // Target resize height
        bool enableROI;              // Enable ROI extraction
        RECT roiRect;                // ROI region
        bool enableCanvas;           // Enable canvas composition
        RECT canvasRect;             // Canvas size and position
        std::vector<RECT> destRects;               // ROI target positions on canvas

        // Constructor, sets default values
        ImageProcessConfigEx() :
            enableResize(false), resizeWidth(0), resizeHeight(0),
            enableROI(false), enableCanvas(false) {
            SetRectEmpty(&roiRect);
            SetRectEmpty(&canvasRect);
            destRects.clear();
        }
        void Reset() {
            enableResize = false;
            enableROI = false;
            enableCanvas = false;
            resizeWidth = 0;
            resizeHeight = 0;
            SetRectEmpty(&roiRect);
            SetRectEmpty(&canvasRect);
            destRects.clear();
        }
    };

    ~DXGIScreenCapture();

    static DXGIScreenCapture& GetInstance();

    // Start capturing the specified window //1111
    bool InitializeForWindow(HWND hWnd);

    // [Hybrid GPU] Set dGPU-preferred mode (only affects InitializeForWindow when m_isHybridGPU==true)
    void SetHybridGPUMode(bool mode) { m_isHybridGPU = mode; }

    void SetNewHandle(HWND &hWnd);

    int GetMonitorIndex(HMONITOR& hm);

    //1111
    bool GetFrame(cv::Mat& mat, int index);

    // Release current frame resources
    void ReleaseFrame(int index);

    // Set the frame capture callback function
    void SetFrameCallback(std::function<void(const FrameData&)> callback);
    void SetFrameCallback(std::function<void(const cv::Mat&)> callback);

    // Stop the capture loop
    void StopCapture();

    // Release all D3D/DXGI resources (for safe re-initialization when switching windows)
    void Release();

    // Check whether re-initialization is needed (set internally by GetFrame)
    bool NeedsReinitialization() const { return m_needsReinitialization.load(); }

    // Get the current backoff time and reset it (for exponential backoff in the external main loop)
    int GetBackoffAndReset() {
        int current = m_recoveryBackoffMs.load();
        m_recoveryBackoffMs.store(std::min(current * 2, 2000)); // Double, capped at 2 seconds
        return current;
    }

    // Reset the backoff counter (call after successful recovery)
    void ResetBackoff() { m_recoveryBackoffMs.store(10); }

    // Log a message
    static void LogMessage(const std::string& message);

    // Set the image processing configuration
    void SetImageProcessConfig(const ImageProcessConfig& config);

    // Get the current image processing configuration
    const ImageProcessConfig& GetImageProcessConfig() const;

    // Get the ROI region of a texture
    CComPtr<ID3D11Texture2D> GetROI(ID3D11Texture2D* srcTexture, const RECT& roiRect);

    // Copy the source texture to the specified position on the destination canvas
    bool CopyToCanvas(ID3D11Texture2D* srcTexture, ID3D11Texture2D* dstTexture, const RECT& dstRect);

    // Resize a texture
    CComPtr<ID3D11Texture2D> ResizeTexture(ID3D11Texture2D* srcTexture, uint32_t newWidth, uint32_t newHeight);
    
    // Full device reset procedure
    bool Reset();
private:
    // D3D11 and DXGI objects
    CComPtr<ID3D11Device> m_d3dDevice;
    CComPtr<ID3D11DeviceContext> m_d3dContext;
    std::vector<CComPtr<IDXGIOutputDuplication>> m_dxgiOutputDuplication;
	std::vector<HMONITOR> m_dxgiOutputDescMonitor;
    CComPtr<ID3D11Texture2D> m_acquiredDesktopImage;
    CComPtr<IDXGIAdapter1> m_pDesktopAdapter;

    // Capture region info
    RECT m_captureRect;
    bool m_isWindowCapture;
    HWND m_targetWindow;

    // [Hybrid GPU] dGPU-preferred mode flag (default false, zero impact on non-hybrid GPUs)
    bool m_isHybridGPU{false};

    // [Hybrid GPU] Establish capture on the discrete GPU (including fullscreen-exclusive outputs with AttachedToDesktop==FALSE)
    // On success returns true and assigns m_d3dDevice/m_d3dContext/m_pDesktopAdapter/m_dxgiOutputDuplication
    // On failure returns false (existing resources untouched, caller falls through to the original logic)
    bool TryInitializeOnDiscreteGPU(HWND hWnd);

    // Intermediate resource for data processing
    CComPtr<ID3D11Texture2D> m_stagingTexture;

    // Frame processing callbacks
    std::function<void(const FrameData&)> m_frameCallback;
    std::function<void(const cv::Mat&)> m_frameCallback_New;

    // Capture control
    bool m_captureRunning;

    // Image processing configuration
    ImageProcessConfig m_imageProcessConfig;

    // Image processing resources
    CComPtr<ID3D11ComputeShader> m_resizeShader;
    CComPtr<ID3D11ComputeShader> m_copyShader;
    CComPtr<ID3D11SamplerState> m_bilinearSampler;
    CComPtr<ID3D11UnorderedAccessView> m_outputUAV;
    CComPtr<ID3D11ShaderResourceView> m_inputSRV;
    CComPtr<ID3D11Buffer> m_constantBuffer;

    // Helper methods
    bool ReinitializeDuplication();
    bool ProcessFrame(FrameData& frameData);
    bool ProcessFrame(cv::Mat& frameData);  // SEH wrapper
    bool ProcessFrameImpl(cv::Mat& frameData);  // Actual implementation
    bool CreateStagingTexture(uint32_t width, uint32_t height);
    bool CopyFrameToBuffer(ID3D11Texture2D* texture, FrameData& frameData);
    RECT GetWindowRect(HWND hWnd);
    bool SetupOutputDuplication(IDXGIOutput* output, int index);

    // New image processing helper methods
    bool InitializeImageProcessing();
    bool ProcessImageWithShaders(ID3D11Texture2D* srcTexture, CComPtr<ID3D11Texture2D>& processedTexture, const ImageProcessConfig& imageProcessConfig);
    bool ProcessImageWithShadersEx(ID3D11Texture2D* srcTexture1, ID3D11Texture2D* srcTexture2, CComPtr<ID3D11Texture2D>& processedTexture, const RECT& canvasRect, const RECT& roiRECT1,const RECT&roiRECT2);
    CComPtr<ID3D11Texture2D> CreateTexture2D(uint32_t width, uint32_t height,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
        UINT bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    bool CreateComputeShaders();
    bool UpdateConstantBuffer(const RECT& srcRect, const RECT& dstRect);
    void Resize_W_H(const int srcH, const int srcW, const int nResizeWidth, const int nResizeHeight, int* newh, int* neww, int* top, int* left, float& hw_scale);

    bool ProcessFrameInner(CComPtr<ID3D11Texture2D>& processedTexture);

    // SEH-safe wrapper for D3D calls (prevents SEH exceptions thrown by the SDK Layer in Debug mode)
    HRESULT SafeCopySubresourceRegion(ID3D11DeviceContext* ctx,
        ID3D11Texture2D* dst, UINT dstSubresource,
        UINT dstX, UINT dstY, UINT dstZ,
        ID3D11Texture2D* src, UINT srcSubresource,
        const D3D11_BOX* srcBox);

    //std::string m_strResolutionRatio;
    int m_nGameMonitorIndex;
    cv::Size m_targetGameScreenSize;
    cv::Rect m_targetWeaponPos;
    cv::Rect m_targetCarPos;

    // Texture cache
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

    // View cache
    struct ViewCacheEntry {
        CComPtr<ID3D11ShaderResourceView> srv;
        CComPtr<ID3D11UnorderedAccessView> uav;
        ID3D11Texture2D* texture;
        bool inUse;
    };
    std::vector<ViewCacheEntry> m_viewCache;

    // Get or create a cached texture
    CComPtr<ID3D11Texture2D> GetCachedTexture(uint32_t width, uint32_t height,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
        UINT bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);

    // Get or create cached views
    bool GetCachedViews(ID3D11Texture2D* texture,
        CComPtr<ID3D11ShaderResourceView>& srv,
        CComPtr<ID3D11UnorderedAccessView>& uav);

    // Reset cache state
    void ResetResourceCache();

    void CleanupResourceCache();

    bool CopyROIToCanvas(ID3D11Texture2D* srcTexture,
        ID3D11UnorderedAccessView* canvasUAV,
        const RECT& roiRect,
        const RECT& destRect,
        uint32_t canvasWidth,
        uint32_t canvasHeight);

    bool GetCachedUAV(ID3D11Texture2D* texture, CComPtr<ID3D11UnorderedAccessView>& uav);

    bool GetCachedSRV(ID3D11Texture2D* texture, CComPtr<ID3D11ShaderResourceView>& srv);

    // Thread safety and recovery mechanism
    mutable std::mutex m_mutex;                      // Mutex protecting core COM pointers
    std::atomic<bool> m_needsReinitialization;       // Device-lost flag
    std::atomic<int> m_recoveryBackoffMs;            // Current backoff wait time (ms)

};
