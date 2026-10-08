#pragma once
#include <span>
#include <string>
#include <vector>

namespace nskry::ocr {
struct CtcGlyph {
    size_t offset{}, length{};
    float left{}, right{}; // In the unpadded, perspective-corrected crop.
};
struct CtcText {
    std::wstring text;
    float confidence{};
    std::vector<CtcGlyph> glyphs;
};
// Vocabulary is the model's class-label metadata, not a correction dictionary.
CtcText DecodeCtc(std::span<const float> probabilities, size_t steps, size_t classes,
                  const std::vector<std::wstring>& vocabulary, float paddedToContentRatio);
} // namespace nskry::ocr
