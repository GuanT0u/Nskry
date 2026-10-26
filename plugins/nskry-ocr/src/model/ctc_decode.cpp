#include "ctc_decode.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nskry::ocr {
CtcText DecodeCtc(std::span<const float> probabilities, size_t steps, size_t classes,
                  const std::vector<std::wstring> &vocabulary, float paddedToContentRatio) {
    if (!steps || !classes || classes != vocabulary.size() || steps > probabilities.size() / classes ||
        probabilities.size() != steps * classes || !std::isfinite(paddedToContentRatio) ||
        paddedToContentRatio < 1)
        throw std::invalid_argument("Invalid recognition tensor");
    struct Token {
        size_t id{}, begin{}, end{};
        float score{};
    };
    std::vector<Token> tokens;
    size_t previous = 0;
    for (size_t step = 0; step < steps; ++step) {
        const auto row = probabilities.subspan(step * classes, classes);
        const auto best = std::max_element(row.begin(), row.end());
        if (!std::isfinite(*best) || *best < 0 || *best > 1)
            throw std::invalid_argument("Invalid recognition probability");
        const size_t id = static_cast<size_t>(best - row.begin());
        if (id && id != previous)
            tokens.push_back({id, step, step + 1, *best});
        else if (id && !tokens.empty())
            tokens.back().end = step + 1;
        previous = id; // Blank resets duplicate removal, e.g. '人人'.
    }
    CtcText result;
    float score = 0;
    std::vector<float> centers;
    for (const auto &token : tokens) {
        centers.push_back((token.begin + token.end) * .5f / static_cast<float>(steps) * paddedToContentRatio);
        result.glyphs.push_back({result.text.size(), vocabulary[token.id].size(), 0, 0,
                                 token.begin / static_cast<float>(steps) * paddedToContentRatio,
                                 token.end / static_cast<float>(steps) * paddedToContentRatio, token.score});
        result.uncertain |= token.score < .55f;
        result.text += vocabulary[token.id];
        score += token.score;
    }
    result.confidence = tokens.empty() ? 0 : score / static_cast<float>(tokens.size());
    for (size_t i = 0; i < centers.size(); ++i) {
        const float half = centers.size() == 1 ? .5f
                           : i == 0            ? (centers[1] - centers[0]) * .5f
                                               : (centers[i] - centers[i - 1]) * .5f;
        result.glyphs[i].left =
            std::clamp(i ? (centers[i - 1] + centers[i]) * .5f : centers[i] - half, 0.f, 1.f);
        result.glyphs[i].right =
            std::clamp(i + 1 < centers.size() ? (centers[i] + centers[i + 1]) * .5f : centers[i] + half,
                       result.glyphs[i].left, 1.f);
    }
    return result;
}

void RecoverNumericSpacing(CtcText &text, std::span<const CtcInkBounds> ink) {
    if (ink.size() != text.glyphs.size() || text.glyphs.size() < 4)
        return;
    struct Measure {
        float left{}, right{}, width{};
        bool valid{};
    };
    std::vector<Measure> bounds(text.glyphs.size());
    std::vector<bool> digits;
    for (size_t i = 0; i < text.glyphs.size(); ++i) {
        const auto &g = text.glyphs[i];
        const bool digit = g.length == 1 && g.offset < text.text.size() && text.text[g.offset] >= L'0' &&
                           text.text[g.offset] <= L'9';
        digits.push_back(digit);
        if (!digit)
            continue;
        if (g.score < .8f || !std::isfinite(ink[i].left) || !std::isfinite(ink[i].right) || ink[i].left < 0 ||
            ink[i].right > 1 || ink[i].right <= ink[i].left)
            return;
        // One-to-one, ordered alignment is required. Duplicated/missing glyph
        // assignments would otherwise invent a low inter-digit baseline.
        if (i && digits[i - 1] && ink[i - 1].right > ink[i].left)
            return;
    }
    // Short runs cannot establish their own spacing baseline. In particular,
    // do not split already separated two-digit groups or decimal suffixes.
    for (size_t first = 0; first < digits.size();) {
        if (!digits[first]) {
            ++first;
            continue;
        }
        size_t last = first + 1;
        while (last < digits.size() && digits[last])
            ++last;
        if (last - first < 4)
            std::fill(digits.begin() + first, digits.begin() + last, false);
        first = last;
    }
    std::vector<float> gaps, widths, pitches, inkPitches;
    for (size_t i = 0; i < bounds.size(); ++i) {
        if (!digits[i])
            continue;
        const auto &g = text.glyphs[i];
        const float first = ink[i].left, last = ink[i].right;
        if (!std::isfinite(first) || !std::isfinite(last) || first < 0 || last > 1 || last <= first)
            continue;
        bounds[i] = {first, last, last - first, true};
        widths.push_back(last - first);
        if (i && digits[i - 1] && bounds[i - 1].valid) {
            gaps.push_back(std::max(0.f, bounds[i].left - bounds[i - 1].right));
            const auto &previous = text.glyphs[i - 1];
            pitches.push_back(
                (g.emissionLeft + g.emissionRight - previous.emissionLeft - previous.emissionRight) * .5f);
            inkPitches.push_back(
                (bounds[i].left + bounds[i].right - bounds[i - 1].left - bounds[i - 1].right) * .5f);
        }
    }
    if (gaps.size() < 3 || widths.empty())
        return;
    std::sort(gaps.begin(), gaps.end());
    std::sort(widths.begin(), widths.end());
    std::sort(pitches.begin(), pitches.end());
    std::sort(inkPitches.begin(), inkPitches.end());
    const float ordinaryGap = gaps[(gaps.size() - 1) / 4];
    const float ordinaryPitch = pitches[(pitches.size() - 1) / 4];
    const float ordinaryInkPitch = inkPitches[(inkPitches.size() - 1) / 4];
    const float width = widths[widths.size() / 2];
    // Relative to the actual ink/font scale, not fixed pixels or group length.
    const float threshold = std::max({width * .35f, ordinaryGap * 1.8f, ordinaryGap + width * .18f});
    std::vector<bool> separators(text.glyphs.size(), false);
    for (size_t i = 1; i < text.glyphs.size(); ++i) {
        if (!digits[i] || !digits[i - 1] || !bounds[i].valid || !bounds[i - 1].valid)
            continue;
        const auto &g = text.glyphs[i];
        const auto &previous = text.glyphs[i - 1];
        const float pitch =
            (g.emissionLeft + g.emissionRight - previous.emissionLeft - previous.emissionRight) * .5f;
        const float inkPitch =
            (bounds[i].left + bounds[i].right - bounds[i - 1].left - bounds[i - 1].right) * .5f;
        separators[i] = bounds[i].left - bounds[i - 1].right >= threshold && pitch > ordinaryPitch * 1.2f &&
                        inkPitch > ordinaryInkPitch * 1.3f;
    }
    // One anomalous glyph/pitch (e.g. a narrow '1') is insufficient evidence
    // of grouping. Ambiguous single separators are deliberately left alone.
    if (std::count(separators.begin(), separators.end(), true) < 2)
        return;
    std::wstring output;
    std::vector<CtcGlyph> glyphs;
    for (size_t i = 0; i < text.glyphs.size(); ++i) {
        auto g = text.glyphs[i];
        if (separators[i]) {
            const float left = bounds[i - 1].right, right = bounds[i].left;
            glyphs.back().right = left;
            g.left = right;
            glyphs.push_back({output.size(), 1, left, right, left, right, 1.f});
            output += L' ';
        }
        const auto source = text.text.substr(g.offset, g.length);
        g.offset = output.size();
        output += source;
        glyphs.push_back(g);
    }
    text.text = std::move(output);
    text.glyphs = std::move(glyphs);
}
} // namespace nskry::ocr
