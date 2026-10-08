#include "ctc_decode.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nskry::ocr {
CtcText DecodeCtc(std::span<const float> probabilities, size_t steps, size_t classes,
                  const std::vector<std::wstring>& vocabulary, float paddedToContentRatio) {
    if (!steps || !classes || classes != vocabulary.size() || steps > probabilities.size() / classes ||
        probabilities.size() != steps * classes || !std::isfinite(paddedToContentRatio) || paddedToContentRatio < 1)
        throw std::invalid_argument("Invalid recognition tensor");
    struct Token { size_t id{}, begin{}, end{}; float score{}; };
    std::vector<Token> tokens;
    size_t previous = 0;
    for (size_t step = 0; step < steps; ++step) {
        const auto row = probabilities.subspan(step * classes, classes);
        const auto best = std::max_element(row.begin(), row.end());
        if (!std::isfinite(*best) || *best < 0 || *best > 1) throw std::invalid_argument("Invalid recognition probability");
        const size_t id = static_cast<size_t>(best - row.begin());
        if (id && id != previous) tokens.push_back({id, step, step + 1, *best});
        else if (id && !tokens.empty()) tokens.back().end = step + 1;
        previous = id; // Blank resets duplicate removal, e.g. '人人'.
    }
    CtcText result;
    float score = 0;
    std::vector<float> centers;
    for (const auto& token : tokens) {
        centers.push_back((token.begin + token.end) * .5f / static_cast<float>(steps) * paddedToContentRatio);
        result.glyphs.push_back({result.text.size(), vocabulary[token.id].size()});
        result.text += vocabulary[token.id];
        score += token.score;
    }
    result.confidence = tokens.empty() ? 0 : score / static_cast<float>(tokens.size());
    for (size_t i = 0; i < centers.size(); ++i) {
        const float half = centers.size() == 1 ? .5f : i == 0 ? (centers[1] - centers[0]) * .5f :
            (centers[i] - centers[i - 1]) * .5f;
        result.glyphs[i].left = std::clamp(i ? (centers[i - 1] + centers[i]) * .5f : centers[i] - half, 0.f, 1.f);
        result.glyphs[i].right = std::clamp(i + 1 < centers.size() ? (centers[i] + centers[i + 1]) * .5f : centers[i] + half,
                                         result.glyphs[i].left, 1.f);
    }
    return result;
}
} // namespace nskry::ocr
