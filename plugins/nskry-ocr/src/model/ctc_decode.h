#pragma once
#include <span>
#include <cstdint>
#include <string>
#include <vector>

namespace nskry::ocr {
struct CtcGlyph {
    size_t offset{}, length{};
    float left{}, right{}; // In the unpadded, perspective-corrected crop.
    // Midpoint display cells are NOT ink bounds. Preserve emission evidence.
    float emissionLeft{}, emissionRight{}, score{};
};
struct CtcText {
    std::wstring text;
    float confidence{};
    std::vector<CtcGlyph> glyphs;
    bool uncertain{}; // Local model score, not a calibrated correctness probability.
};
// Vocabulary is the model's class-label metadata, not a correction dictionary.
CtcText DecodeCtc(std::span<const float> probabilities, size_t steps, size_t classes,
                  const std::vector<std::wstring> &vocabulary, float paddedToContentRatio);
// Requires both pixel whitespace and relative CTC spacing; never uses digit
// count or a dictionary to guess groups. No additional model inference.
struct CtcInkBounds {
    float left{}, right{};
}; // Normalized whole-component bounds, NOT midpoint cells.
void RecoverNumericSpacing(CtcText &text, std::span<const CtcInkBounds> ink);
} // namespace nskry::ocr
