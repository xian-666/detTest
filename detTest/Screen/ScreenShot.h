#pragma once
#include <Windows.h>
#include <opencv2/opencv.hpp>

class Screenshot
{
private:
    Screenshot();

public:
    Screenshot(const Screenshot&) = delete;
    Screenshot& operator=(const Screenshot&) = delete;
    static Screenshot& getInstance() {
        static Screenshot screenShot;
        return screenShot;
    }
    ~Screenshot() {
        SelectObject(m_compatibleDC, m_hOldBitmap);
        DeleteObject(m_hBitmap);
        DeleteObject(m_hClientBitmap);
        DeleteDC(m_compatibleDC);
        ReleaseDC(NULL, m_screenDC);
        delete[] m_pPixels;
    }
    cv::Mat getScreenshot(HWND hWnd);
    void getScreenShot(cv::Mat& mat, HWND hWnd); // capture screenshot for the given HWND
    bool getScreenshotSize(std::string& strScreenSize);



    bool isTopMost(HWND gameh);
private:
    double getZoom(HWND hWnd);
    int m_nWidth;
    int m_nHeight;
    int m_nClientWidth;
    int m_nClientHeight;
    int m_nLastClientWidth;
    int m_nLastClientHeight;
    int m_nClientX;
    int m_nClientY;
    bool m_bIsFullScreen;
    HDC m_screenDC;
    HDC m_compatibleDC;
    HBITMAP m_hBitmap;
    HBITMAP m_hClientBitmap;
    HBITMAP m_hOldBitmap;
    LPVOID m_screenshotData = nullptr;
    BYTE* m_pPixels = nullptr;
    std::vector<std::string> m_vctScreenSize;
    HWND m_hGame;

};


