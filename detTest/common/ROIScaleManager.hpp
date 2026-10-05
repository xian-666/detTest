#pragma once

#include <string>
#include <unordered_map>
#include <cmath>
#include <algorithm>

namespace roi {

/**
 * @brief ROI configuration stored as relative ratios from reference resolution (1920x1080)
 */
struct ROIRatio {
    double x_ratio;  // x / reference_width
    double y_ratio;  // y / reference_height
    double w_ratio;  // w / reference_width
    double h_ratio;  // h / reference_height
};

/**
 * @brief Absolute ROI coordinates with padding
 */
struct AbsoluteROI {
    int x;
    int y;
    int width;
    int height;
    std::string name;
};

/**
 * @brief Aspect ratio category with specific Y-axis offset percentage
 */
struct AspectConfig {
    std::string name;
    double target_aspect;
    double tolerance;
    double y_offset_percent;  // Y-axis offset as percentage of height (e.g., 0.06 for 6%)
};

/**
 * @brief ROI Scale Manager - Pure mathematical scaling based on render resolution
 * 
 * This manager replaces the JSON-based lookup approach with dynamic mathematical scaling.
 * It maintains backward compatibility while supporting all resolutions through linear scaling
 * and aspect-ratio-specific offsets.
 */
class ROIScaleManager {
public:
    static constexpr int REFERENCE_WIDTH = 1920;
    static constexpr int REFERENCE_HEIGHT = 1080;
    static constexpr double DEFAULT_TOLERANCE = 0.01;
    static constexpr int PADDING = 5;

    /**
     * @brief Get singleton instance
     */
    static ROIScaleManager& GetInstance() {
        static ROIScaleManager instance;
        return instance;
    }

    /**
     * @brief Calculate absolute ROI coordinates for given resolution
     * 
     * @param roi_name Name of the ROI (e.g., "Switch", "LsRs")
     * @param current_width Current render width
     * @param current_height Current render height
     * @param padding Whether to apply 5px padding (default: true)
     * @return AbsoluteROI Absolute coordinates and dimensions
     */
    AbsoluteROI GetROI(const std::string& roi_name, int current_width, int current_height, bool padding = true) {
        if (current_height == 0) {
            return AbsoluteROI{0, 0, 10, 10, roi_name};  // Fallback
        }

        // Get aspect ratio configuration
        const auto& aspect_config = GetAspectConfig(current_width, current_height);

        // Get ROI ratio configuration
        auto it = roi_ratios_.find(roi_name);
        if (it == roi_ratios_.end()) {
            return AbsoluteROI{0, 0, 10, 10, roi_name};  // Fallback for unknown ROI
        }

        const auto& ratio = it->second;

        // Calculate scale factors
        double scale_x = static_cast<double>(current_width) / REFERENCE_WIDTH;
        double scale_y = static_cast<double>(current_height) / REFERENCE_HEIGHT;

        // Calculate base coordinates with linear scaling
        double x = ratio.x_ratio * current_width;
        double y = ratio.y_ratio * current_height;
        double w = ratio.w_ratio * current_width;
        double h = ratio.h_ratio * current_height;

        // Apply aspect-ratio-specific Y-axis offset
        double y_offset = current_height * aspect_config.y_offset_percent;
        y += y_offset;

        // Convert to integers
        int x_int = static_cast<int>(std::round(x));
        int y_int = static_cast<int>(std::round(y));
        int w_int = static_cast<int>(std::round(w));
        int h_int = static_cast<int>(std::round(h));

        // Apply padding if requested
        if (padding) {
            x_int = std::max(0, x_int - PADDING);
            y_int = std::max(0, y_int - PADDING);
            w_int = std::min(current_width - x_int, w_int + 2 * PADDING);
            h_int = std::min(current_height - y_int, h_int + 2 * PADDING);
        }

        // Ensure minimum size
        w_int = std::max(10, w_int);
        h_int = std::max(10, h_int);

        return AbsoluteROI{x_int, y_int, w_int, h_int, roi_name};
    }

    /**
     * @brief Get all ROI names available
     */
    std::vector<std::string> GetROINames() const {
        std::vector<std::string> names;
        names.reserve(roi_ratios_.size());
        for (const auto& pair : roi_ratios_) {
            names.push_back(pair.first);
        }
        return names;
    }

private:
    ROIScaleManager() {
        InitializeROIRatios();
        InitializeAspectConfigs();
    }

    /**
     * @brief Initialize ROI ratios from reference resolution (1920x1080)
     * Data from 21:9 screen, 1920x1080 render resolution
     */
    void InitializeROIRatios() {
        roi_ratios_["LsRs"]   = {1747.0/1920.0, 950.0/1080.0, 93.0/1920.0, 93.0/1080.0};
        roi_ratios_["LB_1"]   = {1529.0/1920.0, 949.0/1080.0, 57.0/1920.0, 57.0/1080.0};
        roi_ratios_["RB_1"]   = {1644.0/1920.0, 949.0/1080.0, 57.0/1920.0, 57.0/1080.0};
        roi_ratios_["LT_1"]   = {1529.0/1920.0, 1014.0/1080.0, 57.0/1920.0, 57.0/1080.0};
        roi_ratios_["RT_1"]   = {1644.0/1920.0, 1014.0/1080.0, 57.0/1920.0, 57.0/1080.0};
        roi_ratios_["LB_2"]   = {1497.0/1920.0, 981.0/1080.0, 57.0/1920.0, 57.0/1080.0};
        roi_ratios_["LT_2"]   = {1577.0/1920.0, 981.0/1080.0, 57.0/1920.0, 57.0/1080.0};
        roi_ratios_["RT_2"]   = {1656.0/1920.0, 981.0/1080.0, 57.0/1920.0, 57.0/1080.0};
    }

    /**
     * @brief Initialize aspect ratio configurations with Y-axis offsets
     * Based on measured data analysis
     */
    void InitializeAspectConfigs() {
        aspect_configs_["21:9"]  = {"21:9",  2.333333, 0.02, 0.00};
        aspect_configs_["16:10"] = {"16:10", 1.6,      0.02, 0.023};
        aspect_configs_["16:9"]  = {"16:9",  1.777778, 0.02, 0.00};
        aspect_configs_["5:3"]   = {"5:3",   1.666667, 0.02, 0.00};
        aspect_configs_["25:16"] = {"25:16", 1.5625,   0.02, 0.00};
        aspect_configs_["5:4"]   = {"5:4",   1.25,     0.02, 0.00};
        aspect_configs_["4:3"]   = {"4:3",   1.333333, 0.02, 0.06};
    }

    /**
     * @brief Get aspect configuration based on current resolution
     */
    AspectConfig GetAspectConfig(int width, int height) const {
        if (height == 0) {
            return aspect_configs_.at("16:9");  // Fallback
        }

        double actual_aspect = static_cast<double>(width) / static_cast<double>(height);

        // Find closest match within tolerance
        for (const auto& config : aspect_configs_) {
            double diff = std::abs(actual_aspect - config.second.target_aspect);
            if (diff < config.second.tolerance) {
                return config.second;
            }
        }

        // Default to 16:9 if no match
        return aspect_configs_.at("16:9");
    }

    std::unordered_map<std::string, ROIRatio> roi_ratios_;
    std::unordered_map<std::string, AspectConfig> aspect_configs_;
};

} // namespace roi
