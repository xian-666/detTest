#pragma once
#include <opencv2/opencv.hpp>
#include <vector>
#include <filesystem>
#include <set>
#include <algorithm>
#include <string>
#include <iostream>
#include "common.hpp"

// Create a mosaic canvas from input images and scaled sizes. Safe: will ignore empty images.
inline cv::Mat createMosaicFromMats(const std::vector<cv::Mat>& images,
                                   const std::vector<cv::Size>& scaledSizes,
                                   const cv::Size& canvasSize,
                                   int topMargin)
{
    cv::Mat canvas(canvasSize, CV_8UC3, cv::Scalar(0, 0, 0));
    if (images.empty() || images.size() != scaledSizes.size()) {
        return canvas;
    }

    int currentY = topMargin;
    int currentX = 0;
    int rowHeight = 0;

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i].empty()) continue;

        cv::Mat resized;
        cv::resize(images[i], resized, scaledSizes[i]);

        if (currentX + resized.cols > canvasSize.width) {
            currentX = 0;
            currentY += rowHeight;
            rowHeight = 0;
        }

        // bounds check
        if (currentY + resized.rows > canvasSize.height) break;

        cv::Rect roi(currentX, currentY, resized.cols, resized.rows);
        if (roi.x + roi.width <= canvas.cols && roi.y + roi.height <= canvas.rows) {
            resized.copyTo(canvas(roi));
        }

        currentX += resized.cols;
        rowHeight = (std::max)(rowHeight, resized.rows);
    }

    return canvas;
}

// Create mosaic from a list of images with target total width and gap; used for ROI layout (big + smalls)
inline cv::Mat createMosaicImageWithGaps(const std::vector<cv::Mat>& images, int targetWidth, int gap)
{
    if (images.empty()) return cv::Mat();

    cv::Mat result;

    if (images.size() == 1) {
        double scale = static_cast<double>(targetWidth) / images[0].cols;
        cv::Size newSize(static_cast<int>(images[0].cols * scale), static_cast<int>(images[0].rows * scale));
        cv::resize(images[0], result, newSize);
        return result;
    }

    if (images.size() >= 4) {
        cv::Mat bigImg = images[0];
        double scaleBig = static_cast<double>(targetWidth) / bigImg.cols;
        cv::Size bigSize(targetWidth, static_cast<int>(bigImg.rows * scaleBig));
        cv::Mat resizedBig;
        cv::resize(bigImg, resizedBig, bigSize);

        std::vector<cv::Mat> smallImgs;
        for (int i = 1; i <= 3 && i < static_cast<int>(images.size()); ++i) {
            smallImgs.push_back(images[i]);
        }

        int numSmall = static_cast<int>(smallImgs.size());
        int totalGap = (numSmall - 1) * gap;
        int totalSmallWidth = targetWidth - totalGap;

        double totalRatio = 0.0;
        for (const auto& img : smallImgs) {
            totalRatio += static_cast<double>(img.cols) / img.rows;
        }
        if (totalRatio <= 0) return resizedBig;

        int smallHeight = static_cast<int>(totalSmallWidth / totalRatio);
        if (smallHeight <= 0) smallHeight = 1;

        std::vector<cv::Mat> resizedSmall;
        int maxHeight = 0;
        for (const auto& img : smallImgs) {
            double scale = static_cast<double>(smallHeight) / img.rows;
            cv::Size sz(static_cast<int>(img.cols * scale), smallHeight);
            cv::Mat tmp;
            cv::resize(img, tmp, sz);
            resizedSmall.push_back(tmp);
            maxHeight = (std::max)(maxHeight, sz.height);
        }

        cv::Mat smallRow = cv::Mat::zeros(maxHeight, targetWidth, resizedBig.type());
        int currentX = 0;
        for (size_t i = 0; i < resizedSmall.size(); ++i) {
            cv::Mat& s = resizedSmall[i];
            s.copyTo(smallRow(cv::Rect(currentX, 0, s.cols, s.rows)));
            currentX += s.cols + gap;
        }

        int finalHeight = resizedBig.rows + gap + smallRow.rows;
        result = cv::Mat::zeros(finalHeight, targetWidth, resizedBig.type());
        resizedBig.copyTo(result(cv::Rect(0, 0, resizedBig.cols, resizedBig.rows)));
        smallRow.copyTo(result(cv::Rect(0, resizedBig.rows + gap, targetWidth, smallRow.rows)));
    }

    return result;
}

// Folder-based mosaic creation (originally in DXGIScreenshot)
inline bool createMosaicImageWithGaps(const std::vector<std::string>& inFolders,
    const std::string& outFolder,
    const std::vector<cv::Size>& ScaledSizes,
    const Canvas& canvas,
    int gap, int topMargin,
    const std::string& archiveFolder = "")
{
    if (inFolders.empty()) {
        std::cerr << "输入文件夹列表为空。" << std::endl;
        return false;
    }

    const size_t numFolders = inFolders.size();
    if (ScaledSizes.size() != numFolders) {
        std::cerr << "ScaledSizes 数量 (" << ScaledSizes.size()
            << ") 与文件夹数量 (" << numFolders << ") 不匹配。" << std::endl;
        return false;
    }

    auto isImageFile = [](const std::filesystem::path& p) {
        static const std::set<std::string> validExts = { ".png", ".jpg", ".jpeg", ".bmp", ".tiff" };
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return validExts.count(ext) > 0;
    };

    std::vector<std::vector<std::string>> folderFiles(numFolders);
    size_t minImageCount = SIZE_MAX;

    for (size_t i = 0; i < numFolders; ++i) {
        const std::string& folder = inFolders[i];
        if (!std::filesystem::exists(folder)) {
            std::cerr << "文件夹不存在: " << folder << std::endl;
            return false;
        }

        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (entry.is_regular_file() && isImageFile(entry.path())) {
                folderFiles[i].push_back(entry.path().string());
            }
        }

        std::sort(folderFiles[i].begin(), folderFiles[i].end());
        if (folderFiles[i].empty()) {
            std::cerr << "文件夹无有效图像: " << folder << std::endl;
            return false;
        }

        minImageCount = (std::min)(minImageCount, folderFiles[i].size());
    }

    size_t totalBatches = minImageCount;
    std::cout << "各文件夹图像数量:";
    for (size_t i = 0; i < numFolders; ++i) {
        std::cout << " [" << folderFiles[i].size() << "]";
    }
    std::cout << " → 按最小值 " << totalBatches << " 批进行拼接。" << std::endl;

    if (totalBatches == 0) {
        std::cerr << "所有文件夹均无图像。" << std::endl;
        return false;
    }

    std::filesystem::create_directories(outFolder);
    if (!archiveFolder.empty()) {
        std::filesystem::create_directories(archiveFolder);
    }

    std::vector<std::filesystem::path> archiveSubDirs;
    if (!archiveFolder.empty()) {
        std::filesystem::create_directories(archiveFolder);
        for (size_t i = 0; i < numFolders; ++i) {
            std::filesystem::path baseName = std::filesystem::path(inFolders[i]).filename();
            std::filesystem::path subDir = std::filesystem::path(archiveFolder) / baseName;
            std::filesystem::create_directories(subDir);
            archiveSubDirs.push_back(subDir);
        }
    }

    for (size_t batchIdx = 0; batchIdx < totalBatches; ++batchIdx) {
        std::vector<cv::Mat> images;
        struct BatchItem { std::string path; size_t folderIndex; };
        std::vector<BatchItem> currentBatchItems;
        bool ok = true;

        for (size_t f = 0; f < numFolders; ++f) {
            const std::string& imagePath = folderFiles[f][batchIdx];
            cv::Mat img = cv::imread(imagePath);
            if (img.empty()) { std::cerr << "无法读取图像: " << imagePath << std::endl; ok = false; break; }
            images.push_back(img);
            currentBatchItems.push_back({ imagePath, f });
        }

        if (!ok) { std::cerr << "跳过第 " << batchIdx << " 批（加载失败）。" << std::endl; continue; }

        const int CANVAS_W = canvas.width;
        const int CANVAS_H = canvas.height;
        cv::Mat mosaic(CANVAS_H, CANVAS_W, CV_8UC3, cv::Scalar(0,0,0));

        int x = 0, y = topMargin, currentRowHeight = 0;
        for (size_t i = 0; i < images.size(); ++i) {
            cv::Size target = ScaledSizes[i];
            if (target.width <= 0 || target.height <= 0) continue;

            cv::Mat resized;
            cv::resize(images[i], resized, target);

            if (x + target.width > CANVAS_W) {
                y += currentRowHeight + gap;
                x = 0;
                currentRowHeight = 0;
            }
            currentRowHeight = (std::max)(currentRowHeight, target.height);

            if (y + target.height > CANVAS_H) {
                std::cerr << "第 " << batchIdx << " 批：图像 " << i << " 超出画布底部，跳过后续。" << std::endl;
                break;
            }

            int copyW = (std::min)(target.width, CANVAS_W - x);
            int copyH = (std::min)(target.height, CANVAS_H - y);
            if (copyW > 0 && copyH > 0) {
                cv::Rect dst(x, y, copyW, copyH);
                cv::Rect src(0, 0, copyW, copyH);
                resized(src).copyTo(mosaic(dst));
            }

            x += target.width + gap;
        }

        if (!common::saveMosaic(mosaic, outFolder)) continue;

        if (!archiveFolder.empty()) {
            for (const auto& item : currentBatchItems) {
                std::filesystem::path src(item.path);
                std::filesystem::path dst = archiveSubDirs[item.folderIndex] / src.filename();
                try {
                    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
                    std::filesystem::remove(src);
                    std::cout << "已归档: " << dst.string() << std::endl;
                } catch (const std::filesystem::filesystem_error& e) {
                    std::cerr << "归档失败 (" << item.path << " → " << dst << "): " << e.what() << std::endl;
                }
            }
        }
    }

    std::cout << "拼接完成！共生成 " << totalBatches << " 张图像到: " << outFolder << std::endl;
    if (!archiveFolder.empty()) std::cout << "原始图像已归档至: " << archiveFolder << std::endl;
    return true;
}
