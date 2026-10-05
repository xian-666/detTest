#pragma once

#include <opencv2/opencv.hpp>
#include <vector>
#include <string>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>

#ifdef _WIN32
#include <direct.h>
#include <Windows.h>
#else
#include <sys/stat.h>
#endif

namespace ImageSaver
{

    namespace fs = std::filesystem;

#ifdef _WIN32
    inline bool SaveImageWithChinesePath(const std::string& filePath, const cv::Mat& img, const std::vector<int>& params = {}) {
        std::wstring wfilePath = std::wstring(filePath.begin(), filePath.end());
        FILE* f = _wfopen(wfilePath.c_str(), L"wb");
        if (!f) {
            return false;
        }
        std::vector<int> compression_params = params.empty() ? std::vector<int>{cv::IMWRITE_JPEG_QUALITY, 95} : params;
        std::vector<unsigned char> buf;
        bool encodeSuccess = cv::imencode(".jpg", img, buf, compression_params);
        if (!encodeSuccess) {
            fclose(f);
            return false;
        }
        size_t written = fwrite(buf.data(), 1, buf.size(), f);
        fclose(f);
        return written == buf.size();
    }
#endif

    inline bool SaveROIsToSubfolders(const std::vector<cv::Mat>& rois,
        const std::vector<std::string>& roiNames,
        const std::string& baseSavePath) {
        if (rois.size() != roiNames.size()) {
            return false;
        }

        if (!fs::exists(baseSavePath)) {
            if (!fs::create_directory(baseSavePath)) {
                return false;
            }
        }

        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
        ss << '_' << std::setfill('0') << std::setw(3) << ms.count();
        std::string timestamp = ss.str();

        for (size_t i = 0; i < rois.size(); ++i) {
            const cv::Mat& roi = rois[i];
            const std::string& name = roiNames[i];

            if (roi.empty()) {
                continue;
            }

            std::string subfolderPath = baseSavePath + "/" + name;
            if (!fs::exists(subfolderPath)) {
                if (!fs::create_directory(subfolderPath)) {
                    continue;
                }
            }

            std::string filePath = subfolderPath + "/" + name + "_" + timestamp + ".jpg";

#ifdef _WIN32
            bool success = SaveImageWithChinesePath(filePath, roi, { cv::IMWRITE_JPEG_QUALITY, 95 });
#else
            bool success = cv::imwrite(filePath, roi, { cv::IMWRITE_JPEG_QUALITY, 95 });
#endif
            if (success) {
                std::cout << "[ImageSaver] Saved: " << filePath << std::endl;
            }
            else {
                std::cerr << "[ImageSaver] Error: Failed to save image: " << filePath << std::endl;
            }
        }

        return true;
    }

}