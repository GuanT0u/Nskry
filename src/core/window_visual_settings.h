#pragma once
#include <algorithm>
#include <cmath>

namespace nskry {
enum class VisualPreset { Win11, MacOS, Custom };
enum class CornerMode { System, Fixed, Percent };
struct WindowVisualSettings {
    bool shadowEnabled = false, cornersEnabled = false;
    VisualPreset shadowPreset = VisualPreset::MacOS, cornerPreset = VisualPreset::Win11;
    int shadowOpacity = 22, shadowBlur = 6, shadowRange = 18, shadowOffset = 3;
    CornerMode cornerMode = CornerMode::System;
    int cornerRadius = 8, cornerPercent = 5;
    bool operator==(const WindowVisualSettings&) const = default;
};
inline void SetShadowPreset(WindowVisualSettings& s, VisualPreset preset) {
    s.shadowPreset = preset;
    if (preset == VisualPreset::Win11) { s.shadowOpacity = 18; s.shadowBlur = 4; s.shadowRange = 12; s.shadowOffset = 2; }
    if (preset == VisualPreset::MacOS) { s.shadowOpacity = 22; s.shadowBlur = 6; s.shadowRange = 18; s.shadowOffset = 3; }
}
inline void SetCornerPreset(WindowVisualSettings& s, VisualPreset preset) {
    s.cornerPreset = preset;
    if (preset == VisualPreset::Win11) { s.cornerMode = CornerMode::System; s.cornerRadius = 8; }
    if (preset == VisualPreset::MacOS) { s.cornerMode = CornerMode::Fixed; s.cornerRadius = 10; }
    if (preset == VisualPreset::Custom && s.cornerMode == CornerMode::System) s.cornerMode = CornerMode::Fixed;
}
inline WindowVisualSettings NormalizeVisualSettings(WindowVisualSettings s) {
    s.shadowOpacity = std::clamp(s.shadowOpacity, 0, 50); s.shadowBlur = std::clamp(s.shadowBlur, 0, 8);
    s.shadowRange = std::clamp(s.shadowRange, 0, 24); s.shadowOffset = std::clamp(s.shadowOffset, 0, 6);
    s.cornerRadius = std::clamp(s.cornerRadius, 0, 24); s.cornerPercent = std::clamp(s.cornerPercent, 0, 15);
    return s;
}
inline int WindowCornerRadius(const WindowVisualSettings& s, int width, int height, unsigned dpi) {
    if (!s.cornersEnabled || width <= 0 || height <= 0) return 0;
    const int value = s.cornerMode == CornerMode::Percent ? static_cast<int>(std::min(width, height) * (s.cornerPercent / 100.0))
        : static_cast<int>(std::lround(s.cornerRadius * (dpi / 96.0)));
    return std::clamp(value, 0, std::min(width, height) / 2);
}
}
