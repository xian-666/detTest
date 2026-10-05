// DXGIScreenCapture.cpp
#include "../common/DebugFileLog.h"
#include "DXGIScreenCapture.h"
//#include "DataCenter.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
DXGIScreenCapture::DXGIScreenCapture()
    : m_captureRunning(false),
    m_isWindowCapture(false),
    m_targetWindow(nullptr),
    m_nGameMonitorIndex(0),
    m_needsReinitialization(false),
    m_recoveryBackoffMs(10){
    // Initialize the capture rect to empty
    SetRectEmpty(&m_captureRect);
    m_imageProcessConfig = ImageProcessConfig();
}

DXGIScreenCapture::~DXGIScreenCapture() {
    StopCapture();
    Release();
}

DXGIScreenCapture& DXGIScreenCapture::GetInstance()
{
    static DXGIScreenCapture capture;
    return capture;
}

// [Hybrid GPU dGPU-first] Establish Desktop Duplication on the discrete GPU
// Only called by InitializeForWindow when m_isHybridGPU==true.
// Handles the case where a dGPU output has AttachedToDesktop==FALSE under fullscreen exclusive mode (DuplicateOutput directly, without skipping).
// On success: assigns m_d3dDevice/m_d3dContext/m_pDesktopAdapter/m_dxgiOutputDuplication and returns true.
// On failure: leaves existing members untouched and returns false so the caller falls through to the original monitor-matching logic.
bool DXGIScreenCapture::TryInitializeOnDiscreteGPU(HWND hWnd) {
    // 1. Create DXGI factory
    CComPtr<IDXGIFactory1> dxgiFactory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&dxgiFactory));
    if (FAILED(hr)) {
        DEBUG_LOG_FMT("[DXGICapture] TryInitializeOnDiscreteGPU: Failed to create DXGI factory: 0x%08X", (unsigned)hr);
        return false;
    }

    // 2. Iterate adapters, filter discrete GPUs (>1GB VRAM + not SOFTWARE + vendor in {NVIDIA, AMD})
    CComPtr<IDXGIAdapter1> dgpuAdapter;
    SIZE_T dgpuVideoMem = 0;
    std::string dgpuName;
    for (UINT adapterIndex = 0; ; ++adapterIndex) {
        CComPtr<IDXGIAdapter1> adapter;
        hr = dxgiFactory->EnumAdapters1(adapterIndex, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;

        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        // vendor: NVIDIA 0x10DE / AMD 0x1002
        const bool isDiscreteVendor = (desc.VendorId == 0x10DE || desc.VendorId == 0x1002);
        const SIZE_T vramMB = desc.DedicatedVideoMemory / (1024 * 1024);
        if (!isDiscreteVendor || vramMB <= 1024) continue;

        if (desc.DedicatedVideoMemory > dgpuVideoMem) {
            dgpuVideoMem = desc.DedicatedVideoMemory;
            dgpuAdapter = adapter;
            // Convert wide chars to narrow chars for logging
            char buf[256] = { 0 };
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, sizeof(buf), nullptr, nullptr);
            dgpuName = buf;
        }
    }

    if (!dgpuAdapter) {
        DEBUG_LOG_FMT("[DXGICapture] HybridGPU=1, no qualifying discrete GPU found (>1GB, NVIDIA/AMD)");
        return false;
    }

    DEBUG_LOG_FMT("[DXGICapture] HybridGPU=1, trying dGPU first: %s vram=%lluMB",
        dgpuName.c_str(), (unsigned long long)(dgpuVideoMem / (1024 * 1024)));

    // 3. Create the D3D11 device on the dGPU (parameters identical to the original InitializeForWindow logic)
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    CComPtr<ID3D11Device> tmpDevice;
    CComPtr<ID3D11DeviceContext> tmpContext;
    hr = D3D11CreateDevice(
        dgpuAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
        featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
        &tmpDevice, nullptr, &tmpContext);
    if (FAILED(hr)) {
        DEBUG_LOG_FMT("[DXGICapture] dGPU D3D11CreateDevice failed: 0x%08X", (unsigned)hr);
        return false;
    }

    // 4. Enumerate all outputs of the dGPU (including fullscreen-exclusive outputs with AttachedToDesktop==FALSE)
    //    Call DuplicateOutput directly instead of SetupOutputDuplication (which skips unattached outputs)
    std::vector<CComPtr<IDXGIOutputDuplication>> tmpDuplications;
    std::vector<HMONITOR> tmpMonitors;
    bool hasValidOutput = false;
    for (UINT outputIndex = 0; outputIndex < 5; ++outputIndex) {
        CComPtr<IDXGIOutput> dxgiOutput;
        hr = dgpuAdapter->EnumOutputs(outputIndex, &dxgiOutput);
        if (FAILED(hr)) break;

        DXGI_OUTPUT_DESC outputDesc;
        dxgiOutput->GetDesc(&outputDesc);

        CComPtr<IDXGIOutput1> output1;
        hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
        if (FAILED(hr)) continue;

        DEBUG_LOG_FMT("[DXGICapture] dGPU output[%u] AttachedToDesktop=%d, DuplicateOutput attempted",
            outputIndex, outputDesc.AttachedToDesktop ? 1 : 0);

        CComPtr<IDXGIOutputDuplication> duplication;
        hr = output1->DuplicateOutput(tmpDevice, &duplication);
        if (SUCCEEDED(hr) && duplication) {
            tmpDuplications.push_back(duplication);
            tmpMonitors.push_back(outputDesc.Monitor);
            hasValidOutput = true;
            DEBUG_LOG_FMT("[DXGICapture] dGPU output[%u] DuplicateOutput succeeded -> using dGPU capture", outputIndex);
        } else {
            DEBUG_LOG_FMT("[DXGICapture] dGPU output[%u] DuplicateOutput failed: 0x%08X", outputIndex, (unsigned)hr);
        }
    }

    if (!hasValidOutput) {
        DEBUG_LOG_FMT("[DXGICapture] dGPU has no usable output, giving up dGPU path");
        return false;
    }

    // 5. Success: commit to member variables (COM references of tmpDevice/tmpContext transferred)
    m_d3dDevice = tmpDevice;
    m_d3dContext = tmpContext;
    m_pDesktopAdapter = dgpuAdapter;
    m_dxgiOutputDuplication.assign(tmpDuplications.begin(), tmpDuplications.end());
    m_dxgiOutputDescMonitor.assign(tmpMonitors.begin(), tmpMonitors.end());
    return true;
}

bool DXGIScreenCapture::InitializeForWindow(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) {
        LogMessage("Invalid window handle");
        return false;
    }

    // Release existing resources so smart pointers are empty on reinitialization
    Release();

    // [Hybrid GPU rework] When m_isHybridGPU==true, skip the dGPU-first path and use the original monitor matching (reference implementation behavior)
    // Reason: under Optimus the dGPU has no outputs (EnumOutputs returns empty), so TryInitializeOnDiscreteGPU always fails.
    // The iGPU DXGI correctly captures dGPU-rendered content via DWM-composited frames (DWM performs the cross-adapter copy automatically).
    // The reference implementation works perfectly on the Shadow Knight 11; the key is NOT using dGPU-first and letting MonitorFromWindow naturally pick the iGPU.
    // When m_isHybridGPU==false this block is skipped and the original monitor-matching logic is unchanged line by line.
    // TryInitializeOnDiscreteGPU/SetHybridGPUMode are kept as dead code; they can be re-enabled for a future MUX direct mode.

    // Record window info
    m_isWindowCapture = true;
    m_targetWindow = hWnd;

    // Get the monitor the window is on
    HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    // Create DXGI factory
    CComPtr<IDXGIFactory1> dxgiFactory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&dxgiFactory));
    if (FAILED(hr)) {
        LogMessage("Failed to create DXGI factory: " + std::to_string(hr));
        return false;
    }

    // Iterate all adapters to find the one containing the target monitor
    CComPtr<IDXGIAdapter1> selectedAdapter;
    CComPtr<IDXGIAdapter1> fallbackAdapter;  // [Hybrid GPU optimization] fallback adapter when no match is found
    SIZE_T fallbackVideoMem = 0;
    bool foundAdapter = false;

    for (UINT adapterIndex = 0; ; ++adapterIndex) {
        CComPtr<IDXGIAdapter1> adapter;
        hr = dxgiFactory->EnumAdapters1(adapterIndex, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break; // No more adapters
        }
        if (FAILED(hr)) {
            continue;
        }
        // Get adapter info
        DXGI_ADAPTER_DESC1 adapterDesc;
        adapter->GetDesc1(&adapterDesc);
        std::wstring adapterName(adapterDesc.Description);
        std::string adapterNameUtf8(adapterName.begin(), adapterName.end());

        // [Hybrid GPU optimization] Skip SOFTWARE adapters (e.g. Microsoft Basic Render Driver)
        // They cannot provide hardware-accelerated Desktop Duplication
        if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            DEBUG_LOG_FMT("[DXGICapture] Skip SOFTWARE adapter: %s", adapterNameUtf8.c_str());
            continue;
        }

        LogMessage("Checking adapter: " + adapterNameUtf8);
        LogMessage("  VRAM: " + std::to_string(adapterDesc.DedicatedVideoMemory / (1024 * 1024)) + " MB" +
                   " Flags: 0x" + std::to_string(adapterDesc.Flags));
        // Check this adapter's outputs
        for (UINT outputIndex = 0; ; ++outputIndex) {
            CComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(outputIndex, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr)) {
                continue;
            }

            DXGI_OUTPUT_DESC outputDesc;
            output->GetDesc(&outputDesc);

            // Check whether this is the target monitor
            if (outputDesc.Monitor == hMonitor) {
                selectedAdapter = adapter;
                foundAdapter = true;
                LogMessage("Found matching adapter and output");
                break;
            }
        }
        if (foundAdapter) {
            break;
        }

        // [Hybrid GPU optimization] Record fallback adapter (largest VRAM after skipping SOFTWARE ones)
        if (adapterDesc.DedicatedVideoMemory > fallbackVideoMem) {
            fallbackVideoMem = adapterDesc.DedicatedVideoMemory;
            fallbackAdapter = adapter;
        }
    }
    if (!foundAdapter) {
        // [Hybrid GPU optimization] Fall back to the best non-SOFTWARE adapter when no match is found
        // On some hybrid-GPU laptops, during the GPU migration window the hMonitor returned by
        // MonitorFromWindow may not be stable yet, causing enumeration to fail. Falling back keeps
        // DXGI initialization from stalling; CaptureFrame validates frame contents afterwards.
        if (fallbackAdapter) {
            DXGI_ADAPTER_DESC1 fallbackDesc;
            fallbackAdapter->GetDesc1(&fallbackDesc);
            std::string fbName(fallbackDesc.Description, fallbackDesc.Description + wcslen(fallbackDesc.Description));
            DEBUG_LOG_FMT("[DXGICapture] No matching adapter for hMonitor, using fallback adapter: %s vram=%lluMB",
                fbName.c_str(), (unsigned long long)(fallbackDesc.DedicatedVideoMemory / (1024 * 1024)));
            selectedAdapter = fallbackAdapter;
            foundAdapter = true;
        } else {
            LogMessage("No adapter found that contains the target window's monitor");
            return false;
        }
    }

    // Create the D3D11 device
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    hr = D3D11CreateDevice(
        selectedAdapter,                    // Default adapter
        D3D_DRIVER_TYPE_UNKNOWN,   // Hardware rendering
        nullptr,                    // No software rasterizer
        flags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_d3dDevice,
        nullptr,                    // Don't care about the returned feature level
        &m_d3dContext
    );

    if (FAILED(hr)) {
        LogMessage("Failed to create D3D11 device: " + std::to_string(hr));
        return false;
    }

    m_pDesktopAdapter = selectedAdapter;// Save the selected adapter

    // Enumerate all outputs and set up duplication
    for (int i = 0; i < m_dxgiOutputDuplication.size(); i++) {
        m_dxgiOutputDuplication[i].Release();
    }
    m_dxgiOutputDuplication.resize(0);
    m_dxgiOutputDescMonitor.resize(0);

    bool hasValidOutput = false;
    for (int i = 0; i < 5; i++) {
        CComPtr<IDXGIOutput> dxgiOutput;
        hr = selectedAdapter->EnumOutputs(i, &dxgiOutput);
        if (FAILED(hr)) {
            if (i == 0) {
                LogMessage("Adapter has no outputs");
                LogMessage("Output enumeration out of range, index: " + std::to_string(i) + ", error: " + std::to_string(hr));
                return false;
            }
            else {
                LogMessage("Output enumeration out of range, index: " + std::to_string(i) + ", error: " + std::to_string(hr));
            }
            break; // No more outputs
        }

        CComPtr<IDXGIOutputDuplication> duplication;
        m_dxgiOutputDuplication.push_back(duplication);

        DXGI_OUTPUT_DESC outputDesc;
        dxgiOutput->GetDesc(&outputDesc);
        m_dxgiOutputDescMonitor.push_back(outputDesc.Monitor);

        if (SetupOutputDuplication(dxgiOutput, i)) {
            hasValidOutput = true;
        }

    }

    return hasValidOutput;
}

void DXGIScreenCapture::SetNewHandle(HWND& hWnd)
{
    m_targetWindow = hWnd;
}

int DXGIScreenCapture::GetMonitorIndex(HMONITOR& hm)
{
    for (int i = 0; i < m_dxgiOutputDescMonitor.size(); i++) {
        if (hm == m_dxgiOutputDescMonitor[i]) {
            return i;
        }
    }
    return -1;
}

RECT DXGIScreenCapture::GetWindowRect(HWND hWnd) {
    RECT windowRect;
    GetClientRect(hWnd, &windowRect);

    // Convert client coordinates to screen coordinates
    POINT topLeft = { windowRect.left, windowRect.top };
    POINT bottomRight = { windowRect.right, windowRect.bottom };
    ClientToScreen(hWnd, &topLeft);
    ClientToScreen(hWnd, &bottomRight);

    HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEX info;
    info.cbSize = sizeof(MONITORINFOEX);
    GetMonitorInfo(hMonitor, &info);
    RECT mRect = info.rcMonitor;

    int cxLogical = (mRect.right - mRect.left);
    // Get the monitor's physical width
    DEVMODE dm;
    dm.dmSize = sizeof(dm);
    dm.dmDriverExtra = 0;
    EnumDisplaySettings(info.szDevice, ENUM_CURRENT_SETTINGS, &dm);
    int cxPhysical = dm.dmPelsWidth;
    double ratio = cxPhysical * 1.0 / cxLogical;

    RECT screenRect = { (topLeft.x - mRect.left) * ratio, (topLeft.y - mRect.top)* ratio, (bottomRight.x - mRect.left)* ratio, (bottomRight.y - mRect.top)* ratio };
    return screenRect;
}

bool DXGIScreenCapture::SetupOutputDuplication(IDXGIOutput* output, int index) {
    if (!output) {
        LogMessage("SetupOutputDuplication: output is null");
        return false;
    }

    // Get the IDXGIOutput1 interface
    CComPtr<IDXGIOutput1> output1;
    HRESULT hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
    if (FAILED(hr)) {
        LogMessage("Failed to get IDXGIOutput1 interface: " + std::to_string(hr));
        return false;
    }

    DXGI_OUTPUT_DESC outputDesc;
    output->GetDesc(&outputDesc);

    std::wstring monitorName(outputDesc.DeviceName);
    std::string monitorNameStr(monitorName.begin(), monitorName.end());
    LogMessage("Setting up output [" + std::to_string(index) + "]: " + monitorNameStr +
        " Attached: " + (outputDesc.AttachedToDesktop ? "yes" : "no"));

    // Check whether the output is attached to the desktop
    if (!outputDesc.AttachedToDesktop) {
        LogMessage("Warning: output not attached to desktop, skipping");
        return false;
    }

    // Create the desktop duplication object
    hr = output1->DuplicateOutput(m_d3dDevice, &m_dxgiOutputDuplication[index]);
    if (FAILED(hr)) {
        std::string errorMsg = "Desktop duplication creation failed [index " + std::to_string(index) + "]: ";

        switch (hr) {
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
            errorMsg += "NOT_CURRENTLY_AVAILABLE (0x887A0022) - maximum duplication count exceeded or insufficient permission";
            break;
        case DXGI_ERROR_UNSUPPORTED:
            errorMsg += "UNSUPPORTED (0x887A0004) - GPU does not support this monitor or a driver issue";
            break;
        case E_ACCESSDENIED:
            errorMsg += "ACCESS_DENIED (0x80070005) - administrator privileges required";
            break;
        case DXGI_ERROR_SESSION_DISCONNECTED:
            errorMsg += "SESSION_DISCONNECTED (0x887A0028) - remote desktop session issue";
            break;
        case E_INVALIDARG:
            errorMsg += "INVALID_ARG (0x80070057) - invalid parameter";
            break;
        default:
            char hexStr[32];
            sprintf_s(hexStr, "0x%08X", static_cast<unsigned>(hr));
            errorMsg += "Unknown error: " + std::to_string(hr) + " (" + hexStr + ")";
            break;
        }

        LogMessage(errorMsg);

        // Provide suggestions
        if (hr == DXGI_ERROR_UNSUPPORTED) {
            LogMessage("Suggestion: 1) Update GPU driver 2) Check whether the correct GPU is used 3) Verify the monitor connection");
        }
        else if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            LogMessage("Suggestion: 1) Run as administrator 2) Close other screen capture programs");
        }

        return false;
    }

    // Get desktop duplication description
    DXGI_OUTDUPL_DESC outputDuplDesc;
    m_dxgiOutputDuplication[index]->GetDesc(&outputDuplDesc);

    LogMessage("Desktop duplication created successfully [" + std::to_string(index) + "] " +
        std::to_string(outputDuplDesc.ModeDesc.Width) + "x" +
        std::to_string(outputDuplDesc.ModeDesc.Height) + " @ " +
        std::to_string(outputDuplDesc.ModeDesc.RefreshRate.Numerator /
            outputDuplDesc.ModeDesc.RefreshRate.Denominator) + "Hz");

    return true;
}

bool DXGIScreenCapture::InitializeImageProcessing()
{
    // Skip if already initialized
    if (m_resizeShader && m_copyShader && m_bilinearSampler) {
        return true;
    }

    // Create compute shaders
    if (!CreateComputeShaders()) {
        LogMessage("Failed to create compute shaders");
        return false;
    }

    // Create bilinear sampler
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

    // Create constant buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(float) * 16; // Enough to store the transform matrix and other parameters
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

bool DXGIScreenCapture::ProcessImageWithShaders(ID3D11Texture2D* srcTexture, CComPtr<ID3D11Texture2D>& processedTexture, const ImageProcessConfig& imageProcessConfig)
{
    if (!srcTexture || !m_d3dDevice || !m_d3dContext) {
        return false;
    }

    // Check whether the device has been removed
    HRESULT deviceHR = m_d3dDevice->GetDeviceRemovedReason();
    if (FAILED(deviceHR)) {
        LogMessage("ProcessImageWithShaders: device removed, reason: " + std::to_string(deviceHR));
        m_needsReinitialization = true;
        return false;
    }

    // Get source texture description
    D3D11_TEXTURE2D_DESC srcDesc;
    srcTexture->GetDesc(&srcDesc);
    // Initialize image processing resources if not yet initialized
    if (!InitializeImageProcessing()) {
        LogMessage("Failed to initialize image processing resources");
        return false;
    }
    // Processing pipeline:
    // 1. Extract ROI region if enabled
    // 2. Resize if enabled
    // 3. Create canvas and copy if enabled
    CComPtr<ID3D11Texture2D> currentTexture = srcTexture;
    CComPtr<ID3D11ShaderResourceView> currentSRV;
   
    // Step 1: Extract ROI
    if (imageProcessConfig.enableROI) {
        RECT roiRect = imageProcessConfig.roiRect;

        // Keep the ROI within texture bounds
        roiRect.left = std::max(0L, roiRect.left);
        roiRect.top = std::max(0L, roiRect.top);
        roiRect.right = std::min((LONG)srcDesc.Width, roiRect.right);
        roiRect.bottom = std::min((LONG)srcDesc.Height, roiRect.bottom);

        if (roiRect.right > roiRect.left && roiRect.bottom > roiRect.top) {
            // Create ROI texture
            uint32_t roiWidth = roiRect.right - roiRect.left;
            uint32_t roiHeight = roiRect.bottom - roiRect.top;

            CComPtr<ID3D11Texture2D> roiTexture = GetCachedTexture(roiWidth, roiHeight);

            if (!roiTexture) {
                LogMessage("Failed to create ROI texture");
                return false;
            }

            // Copy ROI region to the new texture
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

            // Check whether the device has been removed
            HRESULT deviceHR = m_d3dDevice->GetDeviceRemovedReason();
            if (FAILED(deviceHR)) {
                LogMessage("Device check failed after ROI copy: " + std::to_string(deviceHR));
                if (deviceHR == DXGI_ERROR_DEVICE_REMOVED || deviceHR == DXGI_ERROR_DEVICE_RESET) {
                    LogMessage("Device removed or reset, marking for reinitialization");
                    m_needsReinitialization = true;
                }
                return false;
            }

            // Update current texture
            currentTexture = roiTexture;
        }
    }
    // Step 2: Resize
    if (imageProcessConfig.enableResize &&
        imageProcessConfig.resizeWidth > 0 &&
        imageProcessConfig.resizeHeight > 0) {

        // Get current texture description
        D3D11_TEXTURE2D_DESC currentDesc;
        currentTexture->GetDesc(&currentDesc);

        // Create a texture with the target size
        CComPtr<ID3D11Texture2D> resizedTexture = GetCachedTexture(imageProcessConfig.resizeWidth,
            imageProcessConfig.resizeHeight);

        if (!resizedTexture) {
            LogMessage("Failed to create resized texture");
            return false;
        }

        // Create shader resource view of the source texture
        if (!currentSRV) {
            bool ret = GetCachedSRV(currentTexture, currentSRV);
            if (!ret) {
                LogMessage("Failed to create shader resource view" );
                return false;
            }
        }

        // Create unordered access view of the target texture
        CComPtr<ID3D11UnorderedAccessView> resizedUAV;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = currentDesc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;

        bool ret = GetCachedUAV(resizedTexture, resizedUAV);
        if (!ret) {
            LogMessage("Failed to create unordered access view");
            return false;
        }

        // Update constant buffer
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        HRESULT hr = m_d3dContext->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        if (SUCCEEDED(hr)) {
            float* data = (float*)mappedResource.pData;

            // Input and output dimensions
            data[0] = (float)currentDesc.Width;
            data[1] = (float)currentDesc.Height;
            data[2] = (float)imageProcessConfig.resizeWidth;
            data[3] = (float)imageProcessConfig.resizeHeight;

            // Source rectangle (entire texture)
            data[4] = 0.0f;
            data[5] = 0.0f;
            data[6] = (float)currentDesc.Width;
            data[7] = (float)currentDesc.Height;

            // Destination rectangle (entire texture)
            data[8] = 0.0f;
            data[9] = 0.0f;
            data[10] = (float)imageProcessConfig.resizeWidth;
            data[11] = (float)imageProcessConfig.resizeHeight;

            m_d3dContext->Unmap(m_constantBuffer, 0);
            // Set shader resources
            m_d3dContext->CSSetShader(m_resizeShader, nullptr, 0);
            m_d3dContext->CSSetShaderResources(0, 1, &currentSRV.p);
            m_d3dContext->CSSetUnorderedAccessViews(0, 1, &resizedUAV.p, nullptr);
            m_d3dContext->CSSetSamplers(0, 1, &m_bilinearSampler.p);
            m_d3dContext->CSSetConstantBuffers(0, 1, &m_constantBuffer.p);

            // Dispatch the compute shader
            uint32_t threadGroupX = (imageProcessConfig.resizeWidth + 15) / 16;
            uint32_t threadGroupY = (imageProcessConfig.resizeHeight + 15) / 16;
            m_d3dContext->Dispatch(threadGroupX, threadGroupY, 1);

            // Clear shader resources
            ID3D11ShaderResourceView* nullSRV = nullptr;
            ID3D11UnorderedAccessView* nullUAV = nullptr;
            ID3D11SamplerState* nullSampler = nullptr;
            ID3D11Buffer* nullBuffer = nullptr;

            m_d3dContext->CSSetShaderResources(0, 1, &nullSRV);
            m_d3dContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
            m_d3dContext->CSSetSamplers(0, 1, &nullSampler);
            m_d3dContext->CSSetConstantBuffers(0, 1, &nullBuffer);

            // Update current texture
            currentTexture = resizedTexture;
            currentSRV.Release();
        }
    }
    
    // Step 3: Canvas composition
    if (imageProcessConfig.enableCanvas &&
        imageProcessConfig.canvasRect.right > imageProcessConfig.canvasRect.left &&
        imageProcessConfig.canvasRect.bottom > imageProcessConfig.canvasRect.top) 
    {

        // Get current texture description
        D3D11_TEXTURE2D_DESC currentDesc;
        currentTexture->GetDesc(&currentDesc);

        // Create canvas texture
        uint32_t canvasWidth = imageProcessConfig.canvasRect.right - imageProcessConfig.canvasRect.left;
        uint32_t canvasHeight = imageProcessConfig.canvasRect.bottom - imageProcessConfig.canvasRect.top;

        CComPtr<ID3D11Texture2D> canvasTexture = GetCachedTexture(canvasWidth, canvasHeight);

        if (!canvasTexture) {
            LogMessage("Failed to create canvas texture");
            return false;
        }

        // Clear the canvas to black
        float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        CComPtr<ID3D11UnorderedAccessView> canvasUAV;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = currentDesc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;

        bool ret = GetCachedUAV(canvasTexture, canvasUAV);
        if (!ret) {
            LogMessage("Failed to create canvas UAV");
            return false;
        }

        m_d3dContext->ClearUnorderedAccessViewFloat(canvasUAV, clearColor);

        // If a destination region exists, copy the current texture to that position on the canvas
        if (imageProcessConfig.destRect.right > imageProcessConfig.destRect.left &&
            imageProcessConfig.destRect.bottom > imageProcessConfig.destRect.top) {

            // Create shader resource view of the source texture
            if (!currentSRV) {

                bool ret = GetCachedSRV(currentTexture, currentSRV);
                if (!ret) {
                    LogMessage("Failed to create source texture SRV");
                    return false;
                }
            }

            // Update constant buffer
            D3D11_MAPPED_SUBRESOURCE mappedResource;
            HRESULT hr = m_d3dContext->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
            if (SUCCEEDED(hr)) {
                float* data = (float*)mappedResource.pData;

                // Input and output dimensions
                data[0] = (float)currentDesc.Width;
                data[1] = (float)currentDesc.Height;
                data[2] = (float)canvasWidth;
                data[3] = (float)canvasHeight;

                // Source rectangle (entire texture)
                data[4] = 0.0f;
                data[5] = 0.0f;
                data[6] = (float)currentDesc.Width;
                data[7] = (float)currentDesc.Height;

                // Destination rectangle
                data[8] = (float)(imageProcessConfig.destRect.left);
                data[9] = (float)(imageProcessConfig.destRect.top);
                data[10] = (float)(imageProcessConfig.destRect.right - imageProcessConfig.destRect.left);
                data[11] = (float)(imageProcessConfig.destRect.bottom - imageProcessConfig.destRect.top);

                m_d3dContext->Unmap(m_constantBuffer, 0);
            }

            // Set shader resources
            m_d3dContext->CSSetShader(m_copyShader, nullptr, 0);
            m_d3dContext->CSSetShaderResources(0, 1, &currentSRV.p);
            m_d3dContext->CSSetUnorderedAccessViews(0, 1, &canvasUAV.p, nullptr);
            m_d3dContext->CSSetConstantBuffers(0, 1, &m_constantBuffer.p);

            // Dispatch the compute shader
            uint32_t threadGroupX = (canvasWidth + 15) / 16;
            uint32_t threadGroupY = (canvasHeight + 15) / 16;
            m_d3dContext->Dispatch(threadGroupX, threadGroupY, 1);

            // Clear shader resources
            ID3D11ShaderResourceView* nullSRV = nullptr;
            ID3D11UnorderedAccessView* nullUAV = nullptr;
            ID3D11Buffer* nullBuffer = nullptr;

            m_d3dContext->CSSetShaderResources(0, 1, &nullSRV);
            m_d3dContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
            m_d3dContext->CSSetConstantBuffers(0, 1, &nullBuffer);
        }

        // Update current texture
        currentTexture = canvasTexture;
    }

    // Set the processed texture
    processedTexture = currentTexture;
    return true;
}

bool DXGIScreenCapture::ProcessImageWithShadersEx(
    ID3D11Texture2D* srcTexture1,
    ID3D11Texture2D* srcTexture2,
    CComPtr<ID3D11Texture2D>& processedTexture,
    const RECT& canvasRect,
    const RECT& roiRect1,
    const RECT& roiRect2)
{
    if (!srcTexture1 || !srcTexture2 || !m_d3dDevice || !m_d3dContext) {
        LogMessage("ProcessImageWithShadersEx: invalid input parameters");
        return false;
    }

    // Check whether the device has been removed
    HRESULT deviceHR = m_d3dDevice->GetDeviceRemovedReason();
    if (FAILED(deviceHR)) {
        LogMessage("ProcessImageWithShadersEx: device removed, reason: " + std::to_string(deviceHR));
        m_needsReinitialization = true;
        return false;
    }

    // Initialize image processing resources
    if (!InitializeImageProcessing()) {
        LogMessage("Failed to initialize image processing resources");
        return false;
    }

    // Create canvas texture
    uint32_t canvasWidth = canvasRect.right - canvasRect.left;
    uint32_t canvasHeight = canvasRect.bottom - canvasRect.top;

    // Get texture via the caching mechanism
    //CComPtr<ID3D11Texture2D> canvasTexture = GetCachedTexture(
    //    canvasWidth, canvasHeight,
    //    DXGI_FORMAT_B8G8R8A8_UNORM,
    //    D3D11_USAGE_DEFAULT,
    //    D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
    //);

    //if (!canvasTexture) {
    //    LogMessage("Failed to create canvas texture");
    //    return false;
    //}

    // Get texture via the caching mechanism
    processedTexture = GetCachedTexture(
        canvasWidth, canvasHeight,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
    );

    if (!processedTexture) {
        LogMessage("Failed to create canvas texture");
        return false;
    }

    // Create UAV
    //CComPtr<ID3D11ShaderResourceView> dummySRV;
    CComPtr<ID3D11UnorderedAccessView> canvasUAV;
    if (!GetCachedUAV(processedTexture, canvasUAV)) {
        LogMessage("Failed to create canvas UAV");
        return false;
    }

    // Clear the canvas to black
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_d3dContext->ClearUnorderedAccessViewFloat(canvasUAV, clearColor);



    // Process the first ROI
    if (roiRect1.right > roiRect1.left && roiRect1.bottom > roiRect1.top) {
        RECT destRect1 = { 0, 0, roiRect1.right - roiRect1.left, roiRect1.bottom - roiRect1.top };
        CopyROIToCanvas(srcTexture1, canvasUAV, roiRect1, destRect1, canvasWidth, canvasHeight);
    }

    // Process the second ROI
    if (roiRect2.right > roiRect2.left && roiRect2.bottom > roiRect2.top) {
        // Compute the second ROI's position on the canvas
        int roi1Width = roiRect1.right - roiRect1.left;
        RECT destRect2 = {
            0,  // Position can be adjusted as needed
            160,  // Placed below the first ROI
            160,
            320
        };
        CopyROIToCanvas(srcTexture2, canvasUAV, roiRect2, destRect2, canvasWidth, canvasHeight);
    }

    // Clear shader resources
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11Buffer* nullBuffer = nullptr;
    m_d3dContext->CSSetShaderResources(0, 1, &nullSRV);
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    m_d3dContext->CSSetConstantBuffers(0, 1, &nullBuffer);

    return true;
}

CComPtr<ID3D11Texture2D> DXGIScreenCapture::CreateTexture2D(uint32_t width, uint32_t height, DXGI_FORMAT format, D3D11_USAGE usage, UINT bindFlags)
{
    if (!m_d3dDevice) {
        LogMessage("CreateTexture2D: D3D device invalid");
        return nullptr;
    }

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
        LogMessage("Failed to create texture: " + std::to_string(hr));
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            LogMessage("Device removed or reset, marking for reinitialization");
            m_needsReinitialization = true;
        }
        return nullptr;
    }

    return texture;
}

bool DXGIScreenCapture::CreateComputeShaders()
{
    // Resize shader HLSL code
    const char* resizeShaderCode = R"(
        Texture2D<float4> InputTexture : register(t0);
        RWTexture2D<float4> OutputTexture : register(u0);
        SamplerState BilinearSampler : register(s0);
        
        cbuffer TransformParams : register(b0)
        {
            float2 InputSize;
            float2 OutputSize;
            float4 SrcRect;  // x, y, width, height
            float4 DstRect;  // x, y, width, height
        };
        
        [numthreads(16, 16, 1)]
        void main(uint3 DTid : SV_DispatchThreadID)
        {
            if (DTid.x >= OutputSize.x || DTid.y >= OutputSize.y)
                return;
                
            // Compute sampling coordinates in the input texture
            float2 uv = float2(DTid.xy) / OutputSize;
            float2 inputUV = float2(
                SrcRect.x + uv.x * SrcRect.z,
                SrcRect.y + uv.y * SrcRect.w
            ) / InputSize;
            
            // Sample and write to the output
            float4 color = InputTexture.SampleLevel(BilinearSampler, inputUV, 0);
            OutputTexture[DTid.xy] = color;
        }
    )";

    // Copy shader HLSL code
    const char* copyShaderCode = R"(
        Texture2D<float4> InputTexture : register(t0);
        RWTexture2D<float4> OutputTexture : register(u0);
        
        cbuffer CopyParams : register(b0)
        {
            float2 InputSize;
            float2 OutputSize;
            float4 SrcRect;  // x, y, width, height
            float4 DstRect;  // x, y, width, height
        };
        
        [numthreads(16, 16, 1)]
        void main(uint3 DTid : SV_DispatchThreadID)
        {
            if (DTid.x >= OutputSize.x || DTid.y >= OutputSize.y)
                return;
                
            // Check whether inside the destination region
            if (DTid.x >= DstRect.x && DTid.x < DstRect.x + DstRect.z &&
                DTid.y >= DstRect.y && DTid.y < DstRect.y + DstRect.w)
            {
                // Compute source texture coordinates
                uint2 srcCoord;
                srcCoord.x = (DTid.x - DstRect.x) * SrcRect.z / DstRect.z + SrcRect.x;
                srcCoord.y = (DTid.y - DstRect.y) * SrcRect.w / DstRect.w + SrcRect.y;
                
                // Ensure coordinates are within the source texture
                if (srcCoord.x < InputSize.x && srcCoord.y < InputSize.y)
                {
                    OutputTexture[DTid.xy] = InputTexture[srcCoord];
                }
                else
                {
                    OutputTexture[DTid.xy] = float4(0, 0, 0, 1); // black
                }
            }
            // Leave other canvas areas unchanged
        }
    )";

    // Compile the resize shader
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
        std::string errorMsg = "Failed to compile resize shader: ";
        if (errorBlob) {
            errorMsg += std::string((char*)errorBlob->GetBufferPointer(), errorBlob->GetBufferSize());
            errorBlob->Release();
        }
        LogMessage(errorMsg);
        if (resizeShaderBlob) resizeShaderBlob->Release();
        return false;
    }

    // Create the resize compute shader
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

    // Compile the copy shader
    ID3DBlob* copyShaderBlob = nullptr;

    hr = D3DCompile(
        copyShaderCode, strlen(copyShaderCode),
        "CopyShader", nullptr, nullptr,
        "main", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &copyShaderBlob, &errorBlob
    );

    if (FAILED(hr)) {
        std::string errorMsg = "Failed to compile copy shader: ";
        if (errorBlob) {
            errorMsg += std::string((char*)errorBlob->GetBufferPointer(), errorBlob->GetBufferSize());
            errorBlob->Release();
        }
        LogMessage(errorMsg);
        if (copyShaderBlob) copyShaderBlob->Release();
        return false;
    }

    // Create the copy compute shader
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

bool DXGIScreenCapture::GetFrame(cv::Mat& mat, int index)
{
    if (index < 0 || static_cast<size_t>(index) >= m_dxgiOutputDuplication.size()) {
        LogMessage("find no Output duplication (index out of range)");
        m_needsReinitialization = true;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_dxgiOutputDuplication[index]) {
            LogMessage("Output duplication pointer is null, scheduling reinitialization");
            m_needsReinitialization = true;
            return false;
        }

        if (!m_d3dDevice || !m_d3dContext) {
            LogMessage("D3D device/context is null, scheduling reinitialization");
            m_needsReinitialization = true;
            return false;
        }

        // Check whether the device has been removed
        HRESULT deviceHR = m_d3dDevice->GetDeviceRemovedReason();
        if (FAILED(deviceHR)) {
            LogMessage("Device removed, reason: " + std::to_string(deviceHR) + ", marking for reinitialization");
            m_needsReinitialization = true;
            return false;
        }

        // Check device context state
        D3D11_DEVICE_CONTEXT_TYPE contextType = m_d3dContext->GetType();
        if (contextType == D3D11_DEVICE_CONTEXT_DEFERRED) {
            LogMessage("Device context type abnormal, marking for reinitialization");
            m_needsReinitialization = true;
            return false;
        }
    }

    ReleaseFrame(index);

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    CComPtr<IDXGIResource> desktopResource;
    const UINT acquireTimeout = 100; // 100ms timeout

    HRESULT hr = S_OK;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_dxgiOutputDuplication[index]) {
            LogMessage("Output duplication became null before AcquireNextFrame");
            m_needsReinitialization = true;
            return false;
        }

        hr = m_dxgiOutputDuplication[index]->AcquireNextFrame(
            acquireTimeout,
            &frameInfo,
            &desktopResource
        );
    }

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;// Timed out, no new frame
    }
    else if (hr == DXGI_ERROR_ACCESS_LOST) {
        LogMessage("Access lost, marking for reinitialization");
        m_needsReinitialization = true;
        ReleaseFrame(index);
        return false;
    }
    else if (hr == E_INVALIDARG) {
        LogMessage("Invalid AcquireNextFrame parameters, marking for reinitialization");
        m_needsReinitialization = true;
        ReleaseFrame(index);
        return false;
    }
    else if (FAILED(hr)) {
        LogMessage("Failed to get next frame: " + std::to_string(hr));
        m_needsReinitialization = true;
        ReleaseFrame(index);
        return false;
    }

    if (!desktopResource) {
        LogMessage("AcquireNextFrame returned null desktop resource");
        ReleaseFrame(index);
        return false;
    }

    // Convert the resource to a texture
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&m_acquiredDesktopImage));
    if (FAILED(hr)) {
        LogMessage("Failed to get desktop texture");
        ReleaseFrame(index);
        return false;
    }

    // Process the frame data
    bool result = false;
    try {
        result = ProcessFrame(mat);
    }
    catch (const std::exception& e) {
        LogMessage("Exception while processing frame: " + std::string(e.what()));
        result = false;
        m_needsReinitialization = true;
    }
    catch (...) {
        LogMessage("Unknown exception while processing frame");
        result = false;
        m_needsReinitialization = true;
    }

    // Frame processed, resources can be released
    if (!result) {
        ReleaseFrame(index);
        LogMessage("Frame processing failed, marking for reinitialization");
        m_needsReinitialization = true;
    }
    return result;
}

void DXGIScreenCapture::ReleaseFrame(int index) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Release the acquired desktop image
    if (m_acquiredDesktopImage) {
        m_acquiredDesktopImage.Release();
    }

    // Release the frame
    if (index >= 0 && index < (int)m_dxgiOutputDuplication.size()
        && m_dxgiOutputDuplication[index]) {
        m_dxgiOutputDuplication[index]->ReleaseFrame();
    }
}

bool DXGIScreenCapture::Reset()
{
    LogMessage("Running full device reset flow...");
    std::lock_guard<std::mutex> lock(m_mutex);

    // 1. Release current frame resources
    if (m_acquiredDesktopImage) {
        m_acquiredDesktopImage.Release();
    }

    // 2. Release IDXGIOutputDuplication
    for (int i = 0; i < m_dxgiOutputDuplication.size(); i++) {
        if (m_dxgiOutputDuplication[i]) {
            m_dxgiOutputDuplication[i].Release();
        }
    }
    m_dxgiOutputDuplication.clear();
    m_dxgiOutputDescMonitor.clear();

    // 3. Release image processing resources (may depend on the device)
    if (m_resizeShader) m_resizeShader.Release();
    if (m_copyShader) m_copyShader.Release();
    if (m_bilinearSampler) m_bilinearSampler.Release();
    if (m_constantBuffer) m_constantBuffer.Release();
    if (m_outputUAV) m_outputUAV.Release();
    if (m_inputSRV) m_inputSRV.Release();

    // 4. Release temporary textures
    if (m_stagingTexture) {
        m_stagingTexture.Release();
    }

    // 5. Release the device context
    if (m_d3dContext) {
        m_d3dContext->ClearState();
        m_d3dContext.Release();
    }

    // 6. Release the device
    if (m_d3dDevice) {
        m_d3dDevice.Release();
    }

    // 7. Release the adapter
    if (m_pDesktopAdapter) {
        m_pDesktopAdapter.Release();
    }

    LogMessage("Device resources released, ready to reinitialize");

    // 8. If there is a target window, try to reinitialize
    if (m_targetWindow && IsWindow(m_targetWindow)) {
        LogMessage("Attempting to reinitialize window capture...");
        bool initResult = InitializeForWindow(m_targetWindow);
        if (initResult) {
            LogMessage("Device reset and reinitialization succeeded");
            m_recoveryBackoffMs = 10; // Reset the backoff counter
            m_needsReinitialization = false; // Clear the device-lost flag
            return true;
        } else {
            LogMessage("Reinitialization failed after device reset");
            return false;
        }
    } else {
        LogMessage("Target window invalid, cannot reinitialize");
        return false;
    }
}

bool DXGIScreenCapture::ReinitializeDuplication()
{
    LogMessage("Full reset triggered via ReinitializeDuplication");
    m_needsReinitialization = true;
    return false; // Let the external caller decide when to call Reset()
}

bool DXGIScreenCapture::ProcessFrame(FrameData& frameData) {

    CComPtr<ID3D11Texture2D> processedTexture;
    if (!ProcessFrameInner(processedTexture)) {
        return false;
    }
    // Create or update the staging texture for CPU reading
    D3D11_TEXTURE2D_DESC processedDesc;
    processedTexture->GetDesc(&processedDesc);
    if (!m_stagingTexture) {
        if (!CreateStagingTexture(processedDesc.Width, processedDesc.Height)) {
            LogMessage("Failed to create processed staging texture");
            return false;
        }
    }
    else {
        D3D11_TEXTURE2D_DESC texDesc;
        m_stagingTexture->GetDesc(&texDesc);
        if (texDesc.Width != processedDesc.Width ||
            texDesc.Height != processedDesc.Height) {
            m_stagingTexture.Release();
            if (!CreateStagingTexture(processedDesc.Width, processedDesc.Height)) {
                LogMessage("Failed to create processed staging texture");
                return false;
            }
        }
    }

    // Copy the processed texture to the staging texture
    m_d3dContext->CopyResource(m_stagingTexture, processedTexture);
    // Copy texture data to the buffer
    return CopyFrameToBuffer(m_stagingTexture, frameData);
}

// SEH-safe ProcessFrame wrapper
// In Debug mode the D3D11 SDK layer raises SEH exceptions when the device is lost.
// ProcessFrameImpl contains objects with destructors (CComPtr etc.), so __try/__except
// cannot be used directly; this wrapper holds no C++ objects and catches the SEH exception.
bool DXGIScreenCapture::ProcessFrame(cv::Mat& frameData)
{
    __try {
        return ProcessFrameImpl(frameData);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("[DXGI] SEH exception in ProcessFrame, device may be lost\n");
        m_needsReinitialization = true;
        return false;
    }
}

bool DXGIScreenCapture::ProcessFrameImpl(cv::Mat& frameData)
{
    CComPtr<ID3D11Texture2D> processedTexture;
    if (!ProcessFrameInner(processedTexture)) {
        return false;
    }
    if (!processedTexture)
        return false;

    D3D11_TEXTURE2D_DESC processedDesc;
    processedTexture->GetDesc(&processedDesc);

    bool needNewStagingTexture = false;
    if (!m_stagingTexture) {
        needNewStagingTexture = true;
    }
    else {
        D3D11_TEXTURE2D_DESC texDesc;
        m_stagingTexture->GetDesc(&texDesc);
        if (texDesc.Width != processedDesc.Width ||
            texDesc.Height != processedDesc.Height) {
            m_stagingTexture.Release();
            needNewStagingTexture = true;
        }
    }
    if (needNewStagingTexture) {
        if (!CreateStagingTexture(processedDesc.Width, processedDesc.Height)) {
            LogMessage("Failed to create processed staging texture");
            return false;
        }
    }

    if (!m_stagingTexture) {
        LogMessage("Staging texture creation failed");
        return false;
    }

    // Verify the device context is still valid
    if (!m_d3dContext) {
        LogMessage("ProcessFrame: D3D context invalid");
        m_needsReinitialization = true;
        return false;
    }

    m_d3dContext->CopyResource(m_stagingTexture, processedTexture);

    // Check whether the device has been removed
    HRESULT deviceHR = m_d3dDevice->GetDeviceRemovedReason();
    if (FAILED(deviceHR)) {
        LogMessage("Device check failed after CopyResource: " + std::to_string(deviceHR));
        if (deviceHR == DXGI_ERROR_DEVICE_REMOVED || deviceHR == DXGI_ERROR_DEVICE_RESET) {
            LogMessage("Device removed or reset, marking for reinitialization");
            m_needsReinitialization = true;
        }
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_d3dContext->Map(m_stagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
    if (FAILED(hr)) {
        LogMessage("Failed to map staging texture: " + std::to_string(hr));
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            LogMessage("Device removed or reset, marking for reinitialization");
            m_needsReinitialization = true;
        }
        return false;
    }

    const int rows = processedDesc.Height;
    const int cols = processedDesc.Width;
    cv::Mat bgra(rows, cols, CV_8UC4, mappedResource.pData, mappedResource.RowPitch);
    cv::cvtColor(bgra, frameData, cv::COLOR_BGRA2BGR);

    m_d3dContext->Unmap(m_stagingTexture, 0);

    return true;
}

bool DXGIScreenCapture::CreateStagingTexture(uint32_t width, uint32_t height) {
    if (!m_d3dDevice) {
        LogMessage("CreateStagingTexture: D3D device invalid");
        return false;
    }

    // Create a staging texture with CPU read access
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // Common BGRA format
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;  // Staging texture for CPU reading
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
    if (FAILED(hr)) {
        LogMessage("Failed to create staging texture: " + std::to_string(hr));
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            LogMessage("Device removed or reset, marking for reinitialization");
            m_needsReinitialization = true;
        }
        return false;
    }

    return true;
}

bool DXGIScreenCapture::CopyFrameToBuffer(ID3D11Texture2D* texture, FrameData& frameData) {
    if (!texture) {
        return false;
    }

    D3D11_TEXTURE2D_DESC texDesc;
    texture->GetDesc(&texDesc);

    // Map the texture to read pixel data
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_d3dContext->Map(texture, 0, D3D11_MAP_READ, 0, &mappedResource);
    if (FAILED(hr)) {
        LogMessage("Failed to map texture: " + std::to_string(hr));
        return false;
    }

    // Compute the required buffer size
    uint32_t width = texDesc.Width;
    uint32_t height = texDesc.Height;
    uint32_t stride = mappedResource.RowPitch;
    size_t bufferSize = stride * height;

    // If in window mode, update width and height
    /*if (m_isWindowCapture) {
        width = m_captureRect.right - m_captureRect.left;
        height = m_captureRect.bottom - m_captureRect.top;
        // stride stays the same, since the staging texture's stride is still the full row length
    }*/

    // Update or create the buffer
    if (frameData.buffer.size() != bufferSize) {
        frameData.buffer.resize(bufferSize);
    }

    // Copy pixel data (BGRA -> RGBA conversion happens here)
    const uint8_t* srcData = static_cast<const uint8_t*>(mappedResource.pData);
    uint8_t* dstData = frameData.buffer.data();

    // Row copy
    memcpy(dstData, srcData, bufferSize);

    // Unmap
    m_d3dContext->Unmap(texture, 0);

    // Set frame data properties
    frameData.width = width;
    frameData.height = height;
    frameData.stride = stride;

    return true;
}

void DXGIScreenCapture::SetFrameCallback(std::function<void(const FrameData&)> callback) {
    m_frameCallback = callback;
}

void DXGIScreenCapture::SetFrameCallback(std::function<void(const cv::Mat&)> callback) {
    m_frameCallback_New = callback;
}

void DXGIScreenCapture::StopCapture() {
    m_captureRunning = false;
    // Give the capture thread some time to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void DXGIScreenCapture::Release() {
    // Release resources in dependency order to avoid COM refcount anomalies
    // 1. Release device-dependent resources first
    for (int i = 0; i < m_dxgiOutputDuplication.size(); i++) {
        if (m_dxgiOutputDuplication[i]) {
            m_dxgiOutputDuplication[i].Release();
        }
    }
    m_dxgiOutputDuplication.clear();
    
    // Clean up the monitor handle list
    for (int i = 0; i < m_dxgiOutputDescMonitor.size(); i++) {
        m_dxgiOutputDescMonitor[i] = NULL;
    }
    m_dxgiOutputDescMonitor.clear();
    
    if (m_acquiredDesktopImage) {
        m_acquiredDesktopImage.Release();
    }
    
    if (m_stagingTexture) {
        m_stagingTexture.Release();
    }
    
    // 2. Release image processing resources
    if (m_resizeShader) m_resizeShader.Release();
    if (m_copyShader) m_copyShader.Release();
    if (m_bilinearSampler) m_bilinearSampler.Release();
    if (m_constantBuffer) m_constantBuffer.Release();
    if (m_outputUAV) m_outputUAV.Release();
    if (m_inputSRV) m_inputSRV.Release();
    
    // 3. Release the adapter
    if (m_pDesktopAdapter) {
        m_pDesktopAdapter.Release();
    }
    
    // 4. Finally release the device and context
    if (m_d3dContext) m_d3dContext.Release();
    if (m_d3dDevice) m_d3dDevice.Release();
}

void DXGIScreenCapture::LogMessage(const std::string& message) {
    std::string strTag = "[DXGIScreenCapture] ";
    std::string strMessage = strTag + message;
    DEBUG_LOG_FMT(strMessage.c_str());
}

void DXGIScreenCapture::SetImageProcessConfig(const ImageProcessConfig& config)
{
    m_imageProcessConfig = config;

    // If image processing is enabled, make sure the related resources are initialized
    if (config.enableResize || config.enableROI || config.enableCanvas) {
        InitializeImageProcessing();
    }
}

const DXGIScreenCapture::ImageProcessConfig& DXGIScreenCapture::GetImageProcessConfig() const
{
    return m_imageProcessConfig;
}

CComPtr<ID3D11Texture2D> DXGIScreenCapture::GetROI(ID3D11Texture2D* srcTexture, const RECT& roiRect)
{
    if (!srcTexture || !m_d3dDevice || !m_d3dContext) {
        return nullptr;
    }

    // Get source texture description
    D3D11_TEXTURE2D_DESC srcDesc;
    srcTexture->GetDesc(&srcDesc);

    // Keep the ROI within texture bounds
    RECT validRoi;
    validRoi.left = std::max(0L, roiRect.left);
    validRoi.top = std::max(0L, roiRect.top);
    validRoi.right = std::min((LONG)srcDesc.Width, roiRect.right);
    validRoi.bottom = std::min((LONG)srcDesc.Height, roiRect.bottom);

    // Check whether the ROI is valid
    if (validRoi.right <= validRoi.left || validRoi.bottom <= validRoi.top) {
        LogMessage("Invalid ROI region");
        return nullptr;
    }

    // Create ROI texture
    uint32_t roiWidth = validRoi.right - validRoi.left;
    uint32_t roiHeight = validRoi.bottom - validRoi.top;

    CComPtr<ID3D11Texture2D> roiTexture = CreateTexture2D(
        roiWidth, roiHeight,
        srcDesc.Format,
        D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
    );

    if (!roiTexture) {
        LogMessage("Failed to create ROI texture");
        return nullptr;
    }

    // Copy the ROI region to the new texture
    D3D11_BOX sourceBox;
    sourceBox.left = validRoi.left;
    sourceBox.top = validRoi.top;
    sourceBox.right = validRoi.right;
    sourceBox.bottom = validRoi.bottom;
    sourceBox.front = 0;
    sourceBox.back = 1;

    m_d3dContext->CopySubresourceRegion(
        roiTexture, 0, 0, 0, 0,
        srcTexture, 0, &sourceBox
    );

    // Check whether the device has been removed
    HRESULT deviceHR = m_d3dDevice->GetDeviceRemovedReason();
    if (FAILED(deviceHR)) {
        LogMessage("Device check failed after GetROI CopySubresourceRegion: " + std::to_string(deviceHR));
        if (deviceHR == DXGI_ERROR_DEVICE_REMOVED || deviceHR == DXGI_ERROR_DEVICE_RESET) {
            LogMessage("Device removed or reset, marking for reinitialization");
            m_needsReinitialization = true;
        }
        return nullptr;
    }

    return roiTexture;
}

// Copy the source texture to a specified position on the target canvas
bool DXGIScreenCapture::CopyToCanvas(ID3D11Texture2D* srcTexture, ID3D11Texture2D* dstTexture, const RECT& dstRect) {
    if (!srcTexture || !dstTexture || !m_d3dDevice || !m_d3dContext) {
        return false;
    }

    // Check whether the device has been removed
    HRESULT deviceHR = m_d3dDevice->GetDeviceRemovedReason();
    if (FAILED(deviceHR)) {
        LogMessage("CopyToCanvas: device removed, reason: " + std::to_string(deviceHR));
        m_needsReinitialization = true;
        return false;
    }

    // Get descriptions of the source and destination textures
    D3D11_TEXTURE2D_DESC srcDesc, dstDesc;
    srcTexture->GetDesc(&srcDesc);
    dstTexture->GetDesc(&dstDesc);

    // Keep the destination region within canvas bounds
    RECT validDstRect;
    validDstRect.left = std::max(0L, dstRect.left);
    validDstRect.top = std::max(0L, dstRect.top);
    validDstRect.right = std::min((LONG)dstDesc.Width, dstRect.right);
    validDstRect.bottom = std::min((LONG)dstDesc.Height, dstRect.bottom);

    // Check whether the destination region is valid
    if (validDstRect.right <= validDstRect.left || validDstRect.bottom <= validDstRect.top) {
        LogMessage("Invalid destination region");
        return false;
    }

    // Create shader resource view of the source texture
    CComPtr<ID3D11ShaderResourceView> srcSRV;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = srcDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    HRESULT hr = m_d3dDevice->CreateShaderResourceView(srcTexture, &srvDesc, &srcSRV);
    if (FAILED(hr)) {
        LogMessage("Failed to create source texture SRV: " + std::to_string(hr));
        return false;
    }

    // Create unordered access view of the destination texture
    CComPtr<ID3D11UnorderedAccessView> dstUAV;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = dstDesc.Format;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;

    hr = m_d3dDevice->CreateUnorderedAccessView(dstTexture, &uavDesc, &dstUAV);
    if (FAILED(hr)) {
        LogMessage("Failed to create destination texture UAV: " + std::to_string(hr));
        return false;
    }

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = m_d3dContext->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr)) {
        float* data = (float*)mappedResource.pData;

        // Input and output dimensions
        data[0] = (float)srcDesc.Width;
        data[1] = (float)srcDesc.Height;
        data[2] = (float)dstDesc.Width;
        data[3] = (float)dstDesc.Height;

        // Source rectangle (entire texture)
        data[4] = 0.0f;
        data[5] = 0.0f;
        data[6] = (float)srcDesc.Width;
        data[7] = (float)srcDesc.Height;

        // Destination rectangle
        data[8] = (float)validDstRect.left;
        data[9] = (float)validDstRect.top;
        data[10] = (float)(validDstRect.right - validDstRect.left);
        data[11] = (float)(validDstRect.bottom - validDstRect.top);

        m_d3dContext->Unmap(m_constantBuffer, 0);
    }

    // Set shader resources
    m_d3dContext->CSSetShader(m_copyShader, nullptr, 0);
    m_d3dContext->CSSetShaderResources(0, 1, &srcSRV.p);
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, &dstUAV.p, nullptr);
    m_d3dContext->CSSetConstantBuffers(0, 1, &m_constantBuffer.p);

    // Dispatch the compute shader
    uint32_t threadGroupX = (dstDesc.Width + 15) / 16;
    uint32_t threadGroupY = (dstDesc.Height + 15) / 16;
    m_d3dContext->Dispatch(threadGroupX, threadGroupY, 1);

    // Clear shader resources
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11Buffer* nullBuffer = nullptr;

    m_d3dContext->CSSetShaderResources(0, 1, &nullSRV);
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    m_d3dContext->CSSetConstantBuffers(0, 1, &nullBuffer);

    return true;
}

// Resize a texture
CComPtr<ID3D11Texture2D> DXGIScreenCapture::ResizeTexture(ID3D11Texture2D* srcTexture, uint32_t newWidth, uint32_t newHeight) {
    if (!srcTexture || !m_d3dDevice || !m_d3dContext || newWidth == 0 || newHeight == 0) {
        return nullptr;
    }

    // Check whether the device has been removed
    HRESULT deviceHR = m_d3dDevice->GetDeviceRemovedReason();
    if (FAILED(deviceHR)) {
        LogMessage("ResizeTexture: device removed, reason: " + std::to_string(deviceHR));
        m_needsReinitialization = true;
        return nullptr;
    }

    // Get source texture description
    D3D11_TEXTURE2D_DESC srcDesc;
    srcTexture->GetDesc(&srcDesc);

    // Create the destination texture
    CComPtr<ID3D11Texture2D> resizedTexture = CreateTexture2D(
        newWidth, newHeight,
        srcDesc.Format,
        D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
    );

    if (!resizedTexture) {
        LogMessage("Failed to create resized texture");
        return nullptr;
    }

    // Make sure image processing resources are initialized
    if (!InitializeImageProcessing()) {
        LogMessage("Failed to initialize image processing resources");
        return nullptr;
    }

    // Create shader resource view of the source texture
    CComPtr<ID3D11ShaderResourceView> srcSRV;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = srcDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    HRESULT hr = m_d3dDevice->CreateShaderResourceView(srcTexture, &srvDesc, &srcSRV);
    if (FAILED(hr)) {
        LogMessage("Failed to create source texture SRV: " + std::to_string(hr));
        return nullptr;
    }

    // Create unordered access view of the destination texture
    CComPtr<ID3D11UnorderedAccessView> dstUAV;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = srcDesc.Format;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;

    hr = m_d3dDevice->CreateUnorderedAccessView(resizedTexture, &uavDesc, &dstUAV);
    if (FAILED(hr)) {
        LogMessage("Failed to create destination texture UAV: " + std::to_string(hr));
        return nullptr;
    }

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = m_d3dContext->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr)) {
        float* data = (float*)mappedResource.pData;

        // Input and output dimensions
        data[0] = (float)srcDesc.Width;
        data[1] = (float)srcDesc.Height;
        data[2] = (float)newWidth;
        data[3] = (float)newHeight;

        // Source rectangle (entire texture)
        data[4] = 0.0f;
        data[5] = 0.0f;
        data[6] = (float)srcDesc.Width;
        data[7] = (float)srcDesc.Height;

        // Destination rectangle (entire texture)
        data[8] = 0.0f;
        data[9] = 0.0f;
        data[10] = (float)newWidth;
        data[11] = (float)newHeight;

        m_d3dContext->Unmap(m_constantBuffer, 0);
    }

    // Set shader resources
    m_d3dContext->CSSetShader(m_resizeShader, nullptr, 0);
    m_d3dContext->CSSetShaderResources(0, 1, &srcSRV.p);
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, &dstUAV.p, nullptr);
    m_d3dContext->CSSetSamplers(0, 1, &m_bilinearSampler.p);
    m_d3dContext->CSSetConstantBuffers(0, 1, &m_constantBuffer.p);

    // Dispatch the compute shader
    uint32_t threadGroupX = (newWidth + 15) / 16;
    uint32_t threadGroupY = (newHeight + 15) / 16;
    m_d3dContext->Dispatch(threadGroupX, threadGroupY, 1);

    // Clear shader resources
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11SamplerState* nullSampler = nullptr;
    ID3D11Buffer* nullBuffer = nullptr;

    m_d3dContext->CSSetShaderResources(0, 1, &nullSRV);
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    m_d3dContext->CSSetSamplers(0, 1, &nullSampler);
    m_d3dContext->CSSetConstantBuffers(0, 1, &nullBuffer);

    return resizedTexture;
}

// Update the constant buffer
bool DXGIScreenCapture::UpdateConstantBuffer(const RECT& srcRect, const RECT& dstRect) {
    if (!m_constantBuffer || !m_d3dContext) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_d3dContext->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (FAILED(hr)) {
        LogMessage("Failed to map constant buffer: " + std::to_string(hr));
        return false;
    }

    float* data = (float*)mappedResource.pData;

    // Source rectangle
    data[0] = (float)srcRect.left;
    data[1] = (float)srcRect.top;
    data[2] = (float)(srcRect.right - srcRect.left);
    data[3] = (float)(srcRect.bottom - srcRect.top);

    // Destination rectangle
    data[4] = (float)dstRect.left;
    data[5] = (float)dstRect.top;
    data[6] = (float)(dstRect.right - dstRect.left);
    data[7] = (float)(dstRect.bottom - dstRect.top);

    m_d3dContext->Unmap(m_constantBuffer, 0);
    return true;
}

void DXGIScreenCapture::Resize_W_H(const int srcH, const int srcW, const int nResizeWidth, const int nResizeHeight, int* newh, int* neww, int* top, int* left, float& hw_scale) {
    *newh = nResizeHeight;
    *neww = nResizeWidth;
    hw_scale = (float)srcH / srcW;// aspect ratio
    if (2 * srcH != srcW) {

        if (hw_scale > 0.5) {  // srch:srcw > 1:2, pad left and right
            *newh = nResizeHeight;
            *neww = int(*newh / hw_scale);                  // set/scale
            *left = int((nResizeWidth - *neww) * 0.5);  // compute padding distance
        }
        else {// wider than tall, pad top and bottom
            *neww = nResizeWidth;
            *newh = (int)(*neww) * hw_scale;
            *top = (int)(nResizeHeight - *newh) * 0.5;
        }
    }
    else {
        *top = 0;
        *left = 0;
        *newh = nResizeHeight;
        *neww = nResizeWidth;
    }
}

// SEH-safe CopySubresourceRegion wrapper
// In Debug mode the D3D11 SDK layer raises SEH exceptions (0x0000087D) when the device is lost.
// try/catch(...) cannot catch them (unless compiled with /EHa); __try/__except must be used.
HRESULT DXGIScreenCapture::SafeCopySubresourceRegion(ID3D11DeviceContext* ctx,
    ID3D11Texture2D* dst, UINT dstSubresource,
    UINT dstX, UINT dstY, UINT dstZ,
    ID3D11Texture2D* src, UINT srcSubresource,
    const D3D11_BOX* srcBox) {

    __try {
        ctx->CopySubresourceRegion(dst, dstSubresource, dstX, dstY, dstZ,
            src, srcSubresource, srcBox);
        return S_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("[DXGI] SEH exception in CopySubresourceRegion, device may be lost\n");
        return DXGI_ERROR_DEVICE_REMOVED;
    }
}

bool DXGIScreenCapture::ProcessFrameInner(CComPtr<ID3D11Texture2D>& processedTexture)
{
    ResetResourceCache();

    if (!m_acquiredDesktopImage) {
        LogMessage("ProcessFrameInner: no desktop image acquired");
        return false;
    }

    // Verify device and context state
    if (!m_d3dDevice || !m_d3dContext) {
        LogMessage("ProcessFrameInner: D3D device or context invalid");
        m_needsReinitialization = true;
        return false;
    }

    // Check whether the device has been removed
    HRESULT deviceHR = m_d3dDevice->GetDeviceRemovedReason();
    if (FAILED(deviceHR)) {
        LogMessage("ProcessFrameInner: device removed, reason: " + std::to_string(deviceHR));
        m_needsReinitialization = true;
        return false;
    }

    // Check device context state
    D3D11_DEVICE_CONTEXT_TYPE contextType = m_d3dContext->GetType();
    if (contextType == D3D11_DEVICE_CONTEXT_DEFERRED) {
        LogMessage("ProcessFrameInner: device context type abnormal, marking for reinitialization");
        m_needsReinitialization = true;
        return false;
    }

    D3D11_TEXTURE2D_DESC texDesc;
    m_acquiredDesktopImage->GetDesc(&texDesc);

    CComPtr<ID3D11Texture2D> sourceTexture;
    if (m_isWindowCapture && m_targetWindow && IsWindow(m_targetWindow)) {
        m_captureRect = GetWindowRect(m_targetWindow);
        D3D11_BOX sourceBox;
        sourceBox.left = m_captureRect.left;
        sourceBox.top = m_captureRect.top;
        sourceBox.right = m_captureRect.right;
        sourceBox.bottom = m_captureRect.bottom;
        sourceBox.front = 0;
        sourceBox.back = 1;

        // Keep the coordinates within texture bounds
        sourceBox.left = std::max(0U, sourceBox.left);
        sourceBox.top = std::max(0U, sourceBox.top);
        sourceBox.right = std::min(texDesc.Width, sourceBox.right);
        sourceBox.bottom = std::min(texDesc.Height, sourceBox.bottom);

        if (sourceBox.right <= sourceBox.left || sourceBox.bottom <= sourceBox.top) {
            std::cout << "texDesc.Width:" << texDesc.Width << " texDesc.Height:" << texDesc.Height << " tl:(" << m_captureRect.left << ","<< m_captureRect.top <<")  br:("<< m_captureRect.right<<"," << m_captureRect.bottom<<")" << std::endl;
            LogMessage("Window region invalid or off-screen");
            return false;
        }
        
        uint32_t windowWidth = sourceBox.right - sourceBox.left;
        uint32_t windowHeight = sourceBox.bottom - sourceBox.top;
        sourceTexture = GetCachedTexture(windowWidth, windowHeight);

        if (!sourceTexture) {
            LogMessage("Failed to create window texture");
            return false;
        }
        
        // Check whether the device has been removed
        HRESULT deviceHR = m_d3dDevice->GetDeviceRemovedReason();
        if (FAILED(deviceHR)) {
            LogMessage("Device check failed before ProcessFrameInner CopySubresourceRegion: " + std::to_string(deviceHR));
            if (deviceHR == DXGI_ERROR_DEVICE_REMOVED || deviceHR == DXGI_ERROR_DEVICE_RESET) {
                LogMessage("Device removed or reset, marking for reinitialization");
                m_needsReinitialization = true;
            }
            return false;
        }
        
        // Add error handling to catch device-removal errors (using the SEH-safe wrapper)
        HRESULT copyHR = SafeCopySubresourceRegion(
            m_d3dContext,
            sourceTexture, 0,
            0, 0, 0,
            m_acquiredDesktopImage, 0,
            &sourceBox
        );

        if (FAILED(copyHR)) {
            LogMessage("ProcessFrameInner CopySubresourceRegion failed: " + std::to_string(copyHR));
            m_needsReinitialization = true;
            return false;
        }
        
        // Check again whether the device has been removed
        deviceHR = m_d3dDevice->GetDeviceRemovedReason();
        if (FAILED(deviceHR)) {
            LogMessage("Device check failed after ProcessFrameInner CopySubresourceRegion: " + std::to_string(deviceHR));
            if (deviceHR == DXGI_ERROR_DEVICE_REMOVED || deviceHR == DXGI_ERROR_DEVICE_RESET) {
                LogMessage("Device removed or reset, marking for reinitialization");
                m_needsReinitialization = true;
            }
            return false;
        }
    }
    else {
        //m_d3dContext->CopyResource(sourceTexture, m_acquiredDesktopImage);
        sourceTexture = m_acquiredDesktopImage;
    }

    CComPtr<ID3D11Texture2D> resizeTexture;
    // Use the class member m_imageProcessConfig so externally applied configs take effect
    bool processResult = ProcessImageWithShaders(sourceTexture, resizeTexture, m_imageProcessConfig);

    if (!processResult || !resizeTexture) {
        resizeTexture = sourceTexture;// If processing failed, use the original texture
    }

    processedTexture = resizeTexture;
    CleanupResourceCache();
    return processResult;

    /*
    DXGIScreenCapture::ImageProcessConfig config;
    //1. Enable resize
    config.enableResize = true;
    config.resizeWidth = m_targetGameScreenSize.width;
    config.resizeHeight = m_targetGameScreenSize.height;

    CComPtr<ID3D11Texture2D> resizeTexture;
    bool processResult = ProcessImageWithShaders(sourceTexture, resizeTexture, config);
    if (!processResult || !resizeTexture) {
        resizeTexture = sourceTexture;// If processing failed, use the original texture
    }

    processedTexture = resizeTexture;
    CleanupResourceCache();
    return processResult;

    config.Reset();
    config.enableROI = true;
    config.roiRect = { m_targetCarPos.x,m_targetCarPos.y,m_targetCarPos.x + m_targetCarPos.width,m_targetCarPos.y + m_targetCarPos.height }; // left, top, right, bottom
    CComPtr<ID3D11Texture2D> roiCarTexture;
    processResult = ProcessImageWithShaders(resizeTexture, roiCarTexture, config);

    // Enable ROI extraction
    config.Reset();
    config.enableROI = true;
    config.roiRect = { m_targetWeaponPos.x,m_targetWeaponPos.y,m_targetWeaponPos.x + m_targetWeaponPos.width,m_targetWeaponPos.y + m_targetWeaponPos.height }; // left, top, right, bottom
    CComPtr<ID3D11Texture2D> roiWeaponTexture;
    processResult = ProcessImageWithShaders(resizeTexture, roiWeaponTexture, config);

    // ROI resize operation
    int nSrcW = m_targetWeaponPos.width;
    int nSrcH = m_targetWeaponPos.height;
    int newH = 0;
    int newW = 0;
    int top = 0;
    int left = 0;
    float fScale = 0.0;
    Resize_W_H(nSrcH, nSrcW, 320, 160, &newH, &newW, &top, &left, fScale);
    config.Reset();
    config.enableResize = true;
    config.resizeWidth = newW;
    config.resizeHeight = newH;
    CComPtr<ID3D11Texture2D> roiResizeWeaponTexture;
    processResult = ProcessImageWithShaders(roiWeaponTexture, roiResizeWeaponTexture, config);

    nSrcW = m_targetCarPos.width;
    nSrcH = m_targetCarPos.height;
    config.Reset();
    config.enableResize = true;
    config.resizeWidth = 160;
    config.resizeHeight = 160;
    CComPtr<ID3D11Texture2D> roiResizeCarTexture;
    processResult = ProcessImageWithShaders(roiCarTexture, roiResizeCarTexture, config);

    RECT canvasRect = { 0, 0, 320, 320 };
    RECT weaponRect = { left, top, left + newW, top + newH };
    RECT carRect = { 0,0, 160,160 };
    processResult = ProcessImageWithShadersEx(roiResizeWeaponTexture, roiResizeCarTexture, processedTexture, canvasRect, weaponRect, carRect);

    CleanupResourceCache();
    return processResult;*/
}

bool DXGIScreenCapture::CopyROIToCanvas(ID3D11Texture2D* srcTexture, ID3D11UnorderedAccessView* canvasUAV, const RECT& roiRect, const RECT& destRect, uint32_t canvasWidth, uint32_t canvasHeight)
{
    if (!srcTexture || !canvasUAV) return false;

    // Get source texture description
    D3D11_TEXTURE2D_DESC srcDesc;
    srcTexture->GetDesc(&srcDesc);

    // Create shader resource view of the source texture
    CComPtr<ID3D11ShaderResourceView> srcSRV;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = srcDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    bool ret = GetCachedSRV(srcTexture, srcSRV);
    if (!ret) {
        LogMessage("Failed to create source texture SRV" );
        return false;
    }

    //HRESULT hr = m_d3dDevice->CreateShaderResourceView(srcTexture, &srvDesc, &srcSRV);
    //if (FAILED(hr)) {
    //    LogMessage("Failed to create source texture SRV: " + std::to_string(hr));
    //    return false;
    //}

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_d3dContext->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr)) {
        float* data = (float*)mappedResource.pData;

        // Input and output dimensions
        data[0] = (float)srcDesc.Width;
        data[1] = (float)srcDesc.Height;
        data[2] = (float)canvasWidth;
        data[3] = (float)canvasHeight;

        // Source rectangle (ROI region)
        data[4] = (float)roiRect.left;
        data[5] = (float)roiRect.top;
        data[6] = (float)(roiRect.right - roiRect.left);
        data[7] = (float)(roiRect.bottom - roiRect.top);

        // Destination rectangle (position on the canvas)
        data[8] = (float)destRect.left;
        data[9] = (float)destRect.top;
        data[10] = (float)(destRect.right - destRect.left);
        data[11] = (float)(destRect.bottom - destRect.top);

        m_d3dContext->Unmap(m_constantBuffer, 0);
    }

    // Set shader resources
    m_d3dContext->CSSetShader(m_copyShader, nullptr, 0);
    m_d3dContext->CSSetShaderResources(0, 1, &srcSRV.p);
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, &canvasUAV, nullptr);
    m_d3dContext->CSSetConstantBuffers(0, 1, &m_constantBuffer.p);

    // Dispatch the compute shader
    uint32_t threadGroupX = (canvasWidth + 15) / 16;
    uint32_t threadGroupY = (canvasHeight + 15) / 16;
    m_d3dContext->Dispatch(threadGroupX, threadGroupY, 1);

    return true;
}

void DXGIScreenCapture::CleanupResourceCache()
{
    // Define cache size limits
    const size_t MAX_TEXTURE_CACHE_SIZE = 10;
    const size_t MAX_VIEW_CACHE_SIZE = 10;

    // If the texture cache is too large, remove unused entries
    if (m_textureCache.size() > MAX_TEXTURE_CACHE_SIZE) {
        auto newEnd = std::remove_if(m_textureCache.begin(), m_textureCache.end(),
            [](const TextureCacheEntry& entry) { return !entry.inUse; });

        // If still over the limit, remove the oldest entries
        if (std::distance(m_textureCache.begin(), newEnd) > MAX_TEXTURE_CACHE_SIZE) {
            newEnd = m_textureCache.begin() + MAX_TEXTURE_CACHE_SIZE;
        }

        m_textureCache.erase(newEnd, m_textureCache.end());
    }

    // If the view cache is too large, remove unused entries
    if (m_viewCache.size() > MAX_VIEW_CACHE_SIZE) {
        auto newEnd = std::remove_if(m_viewCache.begin(), m_viewCache.end(),
            [](const ViewCacheEntry& entry) { return !entry.inUse; });

        // If still over the limit, remove the oldest entries
        if (std::distance(m_viewCache.begin(), newEnd) > MAX_VIEW_CACHE_SIZE) {
            newEnd = m_viewCache.begin() + MAX_VIEW_CACHE_SIZE;
        }

        m_viewCache.erase(newEnd, m_viewCache.end());
    }
}



CComPtr<ID3D11Texture2D> DXGIScreenCapture::GetCachedTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, D3D11_USAGE usage, UINT bindFlags)
{
    // Find an available cached texture
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

    // No suitable cache entry found, create a new texture
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

bool DXGIScreenCapture::GetCachedViews(ID3D11Texture2D* texture, CComPtr<ID3D11ShaderResourceView>& srv, CComPtr<ID3D11UnorderedAccessView>& uav)
{
    if (!texture) return false;

    // Look up cached views
    for (auto& entry : m_viewCache) {
        if (!entry.inUse && entry.texture == texture) {
            entry.inUse = true;
            srv = entry.srv;
            uav = entry.uav;
            return true;
        }
    }

    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // Create SRV
    if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;

        HRESULT hr = m_d3dDevice->CreateShaderResourceView(texture, &srvDesc, &srv);
        if (FAILED(hr)) {
            LogMessage("Failed to create SRV: " + std::to_string(hr));
            return false;
        }
    }

    // Create UAV
    if (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = desc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;

        HRESULT hr = m_d3dDevice->CreateUnorderedAccessView(texture, &uavDesc, &uav);
        if (FAILED(hr)) {
            LogMessage("Failed to create UAV: " + std::to_string(hr));
            return false;
        }
    }

    // Add to cache
    ViewCacheEntry newEntry;
    newEntry.srv = srv;
    newEntry.uav = uav;
    newEntry.texture = texture;
    newEntry.inUse = true;
    m_viewCache.push_back(newEntry);

    return true;
}

void DXGIScreenCapture::ResetResourceCache()
{
    // Reset the in-use state of all cache entries
    for (auto& entry : m_textureCache) {
        entry.inUse = false;
    }

    for (auto& entry : m_viewCache) {
        entry.inUse = false;
    }

    //// If the cache is too large, consider releasing some resources
    //const size_t MAX_CACHE_SIZE = 20;
    //if (m_textureCache.size() > MAX_CACHE_SIZE) {
    //    // Remove unused textures beyond the limit
    //    m_textureCache.erase(
    //        std::remove_if(m_textureCache.begin() + MAX_CACHE_SIZE, m_textureCache.end(),
    //            [](const TextureCacheEntry& entry) { return !entry.inUse; }),
    //        m_textureCache.end());
    //}

    //if (m_viewCache.size() > MAX_CACHE_SIZE) {
    //    // Remove unused views beyond the limit
    //    m_viewCache.erase(
    //        std::remove_if(m_viewCache.begin() + MAX_CACHE_SIZE, m_viewCache.end(),
    //            [](const ViewCacheEntry& entry) { return !entry.inUse; }),
    //        m_viewCache.end());
    //}
}

bool DXGIScreenCapture::GetCachedUAV(ID3D11Texture2D* texture, CComPtr<ID3D11UnorderedAccessView>& uav)
{
    if (!texture) return false;

    // Look up a cached UAV
    for (auto& entry : m_viewCache) {
        if (!entry.inUse && entry.texture == texture && entry.uav) {
            entry.inUse = true;
            uav = entry.uav;
            return true;
        }
    }

    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // Make sure the texture supports UAVs
    if (!(desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) {
        LogMessage("Texture does not support UAV operations");
        return false;
    }

    // Create UAV
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = desc.Format;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;

    HRESULT hr = m_d3dDevice->CreateUnorderedAccessView(texture, &uavDesc, &uav);
    if (FAILED(hr)) {
        LogMessage("Failed to create UAV: " + std::to_string(hr));
        return false;
    }

    // Add to cache
    ViewCacheEntry newEntry;
    newEntry.srv = nullptr;
    newEntry.uav = uav;
    newEntry.texture = texture;
    newEntry.inUse = true;
    m_viewCache.push_back(newEntry);

    return true;
}

bool DXGIScreenCapture::GetCachedSRV(ID3D11Texture2D* texture, CComPtr<ID3D11ShaderResourceView>& srv)
{
    if (!texture) return false;

    // Look up a cached SRV
    for (auto& entry : m_viewCache) {
        if (!entry.inUse && entry.texture == texture && entry.srv) {
            entry.inUse = true;
            srv = entry.srv;
            return true;
        }
    }

    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // Make sure the texture supports SRVs
    if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        LogMessage("Texture does not support SRV operations");
        return false;
    }

    // Create SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    HRESULT hr = m_d3dDevice->CreateShaderResourceView(texture, &srvDesc, &srv);
    if (FAILED(hr)) {
        LogMessage("Failed to create SRV: " + std::to_string(hr));
        return false;
    }

    // Add to cache
    ViewCacheEntry newEntry;
    newEntry.srv = srv;
    newEntry.uav = nullptr;
    newEntry.texture = texture;
    newEntry.inUse = true;
    m_viewCache.push_back(newEntry);

    return true;
}
