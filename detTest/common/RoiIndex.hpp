#pragma once
// RoiIndex.hpp
//
// Logical index of each ROI fed to the classification model.
//
// Historically this enum lived in StrategyController.h, next to the game
// strategy logic. detTest keeps only capture + inference, so the ROI identity
// (which is a property of the model input layout, not of the game) was moved
// here to keep it available without pulling in any game layer.
//
// The model still is the HoK HONOR model, so the ROI order is unchanged:
// index N corresponds to the N-th Region produced by
// GameInferenceSystem::generateDynamicROIs().

#include <cstddef>
#include <string>

enum class ButtonIndex : size_t {
    LsRs   = 0,
    LB_1   = 1,
    RB_1   = 2,
    LT_1   = 3,
    RT_1   = 4,
    LB_2   = 5,
    LT_2   = 6,
    RT_2   = 7,
    COUNT  = 8
};

inline const char* RoiIndexName(std::size_t index) {
    switch (static_cast<ButtonIndex>(index)) {
        case ButtonIndex::LsRs: return "LsRs";
        case ButtonIndex::LB_1: return "LB_1";
        case ButtonIndex::RB_1: return "RB_1";
        case ButtonIndex::LT_1: return "LT_1";
        case ButtonIndex::RT_1: return "RT_1";
        case ButtonIndex::LB_2: return "LB_2";
        case ButtonIndex::LT_2: return "LT_2";
        case ButtonIndex::RT_2: return "RT_2";
        default:                return "ROI?";
    }
}
