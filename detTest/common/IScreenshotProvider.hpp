#pragma once
#include <opencv2/opencv.hpp>
#include <string>

struct Canvas {
	int width, height;

	Canvas(int w, int h) : width(w), height(h) { }
};

struct Region {
	int x, y, width, height;
	std::string name;

	Region(int x, int y, int width, int height, const std::string& name = "")
		: x(x), y(y), width(width), height(height), name(name) { }
};

class IScreenshotProvider {
public:
    virtual ~IScreenshotProvider() = default;
    virtual bool initialize(int monitorIndex = 0) = 0;
    virtual cv::Mat getScreenshot() = 0;
    virtual std::string getName() const = 0;
};