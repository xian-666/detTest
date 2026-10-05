#include "Screenshot.h"
#include "../common/DebugFileLog.h"
#include <string>

Screenshot::Screenshot()
{
    m_nClientWidth = 0;
    m_nClientHeight = 0;
    m_nClientX = 0;
    m_nClientY = 0;
    m_nLastClientHeight = 0;
    m_nLastClientWidth = 0;

    m_screenDC = GetDC(NULL);
    m_compatibleDC = CreateCompatibleDC(m_screenDC);
    m_hBitmap = CreateCompatibleBitmap(m_screenDC, 1, 1);
    m_hOldBitmap = (HBITMAP)SelectObject(m_compatibleDC, m_hBitmap);
}

cv::Mat Screenshot::getScreenshot(HWND gameh)
{
    RECT b1Rect;
    ::GetClientRect(gameh, &b1Rect);
    double dZoom = getZoom(gameh);
    m_nClientWidth = (b1Rect.right - b1Rect.left) * dZoom;
    m_nClientHeight = (b1Rect.bottom - b1Rect.top) * dZoom;
    POINT pt; 
    pt.x = 0; pt.y = 0;
    ::ClientToScreen(gameh, &pt);
    m_nClientX = pt.x * dZoom;
    m_nClientY = pt.y * dZoom;

    int nWidth = GetSystemMetrics(SM_CXSCREEN) * dZoom;
    int nHeight = GetSystemMetrics(SM_CYSCREEN) * dZoom;

    if (pt.x < 0
        || pt.y < 0
        || pt.x > nWidth
        || pt.y > nHeight
        || m_nClientWidth <= 0
        || m_nClientHeight <= 0) {
        DEBUG_LOG_FMT("detTest: error positon - %d %d %d %d %d %d", pt.x, pt.y, nWidth, nHeight, m_nClientWidth, m_nClientHeight);
        return cv::Mat();
    }
    
    if ((m_nClientX + m_nClientWidth) > nWidth
        || (m_nClientY + m_nClientHeight) > nHeight) {
        std::cout << "error image size" << std::endl;
        DEBUG_LOG_FMT("detTest: error image size");
        return cv::Mat();
    }

    if (!m_screenshotData) {
        m_screenshotData = new char[nWidth * nHeight * 4];
    }

    if (m_hBitmap) {
        DeleteObject(m_hBitmap);
    }
    m_hBitmap = CreateCompatibleBitmap(m_screenDC, nWidth, nHeight);
    SelectObject(m_compatibleDC, m_hBitmap);

    BitBlt(m_compatibleDC, 0, 0, nWidth, nHeight, m_screenDC, 0, 0, SRCCOPY);
    GetBitmapBits(m_hBitmap, nWidth * nHeight * 4, m_screenshotData);
    cv::Mat screenshot(nHeight, nWidth, CV_8UC4, m_screenshotData);
    cv::cvtColor(screenshot, screenshot, cv::COLOR_BGRA2BGR);
    return screenshot(cv::Rect(m_nClientX, m_nClientY, m_nClientWidth, m_nClientHeight));
    
}

void Screenshot::getScreenShot(cv::Mat& srcImg, HWND gameh)
{
    RECT b1Rect;
    ::GetClientRect(gameh, &b1Rect);
    double dZoom = getZoom(gameh);
    m_nClientWidth = (b1Rect.right - b1Rect.left) * dZoom;
    m_nClientHeight = (b1Rect.bottom - b1Rect.top) * dZoom;
    POINT pt;
    pt.x = 0; pt.y = 0;
    ::ClientToScreen(gameh, &pt);
    m_nClientX = pt.x * dZoom;
    m_nClientY = pt.y * dZoom;

    int nWidth = GetSystemMetrics(SM_CXSCREEN) * dZoom;
    int nHeight = GetSystemMetrics(SM_CYSCREEN) * dZoom;

    if (pt.x < 0
        || pt.y < 0
        || pt.x > nWidth
        || pt.y > nHeight
        || m_nClientWidth <= 0
        || m_nClientHeight <= 0) {
        DEBUG_LOG_FMT("detTest: error positon - %d %d %d %d %d %d", pt.x, pt.y, nWidth, nHeight, m_nClientWidth, m_nClientHeight);
        return ;
    }

    if ((m_nClientX + m_nClientWidth) > nWidth
        || (m_nClientY + m_nClientHeight) > nHeight) {
        std::cout << "error image size" << std::endl;
        DEBUG_LOG_FMT("detTest: error image size");
        return ;
    }
    if (m_nLastClientHeight != m_nClientHeight || m_nLastClientWidth != m_nClientWidth) {
        if (m_hClientBitmap) {
            DeleteObject(m_hClientBitmap);
        }
        m_hClientBitmap = CreateCompatibleBitmap(m_screenDC, m_nClientWidth, m_nClientHeight);       
        if (m_pPixels) {
            delete[] m_pPixels;
            m_pPixels = nullptr;
        }
        
        SelectObject(m_compatibleDC, m_hClientBitmap);
        m_nLastClientHeight = m_nClientHeight;
        m_nLastClientWidth = m_nClientWidth;
        m_pPixels = new BYTE[m_nClientHeight * m_nClientWidth * 4];
    }

    BitBlt(m_compatibleDC, 0, 0, m_nClientWidth, m_nClientHeight, m_screenDC, m_nClientX, m_nClientY, SRCCOPY);
    if (m_hClientBitmap && m_pPixels) {
        GetBitmapBits(m_hClientBitmap, m_nClientHeight * m_nClientWidth * 4, m_pPixels);
        srcImg.create(m_nClientHeight, m_nClientWidth, CV_8UC4);
        memcpy(srcImg.data, m_pPixels, m_nClientHeight * m_nClientWidth * 4);
        cv::cvtColor(srcImg, srcImg, cv::COLOR_BGRA2BGR);
    }
}

bool Screenshot::getScreenshotSize(std::string& strScreenSize)
{
    if (0 == m_nClientWidth || 0 == m_nClientHeight) {
        return false;
    }
    else {
        std::string strWidth = std::to_string(m_nClientWidth);
        std::string strHeight = std::to_string(m_nClientHeight);
        strScreenSize = strWidth + "x" + strHeight;
        return true;
    }
}

bool Screenshot::isTopMost(HWND gameh)
{
    return GetWindowLong(gameh, GWL_EXSTYLE)& WS_EX_TOPMOST;
}



double Screenshot::getZoom(HWND hWnd)
{
    HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);

    MONITORINFOEX monitorInfo;
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfo(hMonitor, &monitorInfo);
    int cxLogical = (monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left);

    DEVMODE dm;
    dm.dmSize = sizeof(dm);
    dm.dmDriverExtra = 0;
    EnumDisplaySettings(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &dm);
    int cxPhysical = dm.dmPelsWidth;

    return cxPhysical * 1.0 / cxLogical;
}
