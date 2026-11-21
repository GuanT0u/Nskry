#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <cstddef>

namespace nskry::scroll {
constexpr size_t kMaxFrameBytes = 128ull * 1024 * 1024;
struct Frame {
    int width{}, height{};
    std::vector<unsigned int> pixels;
};
inline bool PixelCount(int width, int height, size_t &count, size_t maximumBytes = kMaxFrameBytes) noexcept {
    if (width <= 0 || height <= 0)
        return false;
    const size_t w = static_cast<size_t>(width), h = static_cast<size_t>(height);
    if (w > (std::numeric_limits<size_t>::max)() / h)
        return false;
    count = w * h;
    return count <= maximumBytes / sizeof(unsigned int);
}

inline bool ValidFrame(const Frame &frame, size_t maximumBytes = kMaxFrameBytes) noexcept {
    size_t count = 0;
    return PixelCount(frame.width, frame.height, count, maximumBytes) && frame.pixels.size() == count;
}

inline int Channel(unsigned int pixel, int shift) { return static_cast<int>((pixel >> shift) & 255); }
inline int Luma(unsigned int pixel) {
    return (Channel(pixel, 16) * 3 + Channel(pixel, 8) * 6 + Channel(pixel, 0)) / 10;
}

inline bool SameFrame(const Frame &left, const Frame &right) {
    return left.width == right.width && left.height == right.height &&
           left.pixels.size() == right.pixels.size() &&
           (left.pixels == right.pixels ||
            std::equal(left.pixels.begin(), left.pixels.end(), right.pixels.begin(),
                       [](unsigned a, unsigned b) { return ((a ^ b) & 0xffffffu) == 0; }));
}

inline int PixelError(unsigned a, unsigned b) {
    return std::abs(Channel(a, 0) - Channel(b, 0)) + std::abs(Channel(a, 8) - Channel(b, 8)) +
           std::abs(Channel(a, 16) - Channel(b, 16));
}

struct FixedBands {
    int top{}, bottom{}, left{}, right{};
    bool operator==(const FixedBands &) const = default;
};

// A blank margin alone says nothing about the width of later messages.
// Require a persistent divider or an abrupt transition from textured static
// chrome to a column that changes throughout its height.
inline int DetectFixedSide(const Frame &previous, const Frame &current, bool right, bool dividerOnly = false) {
    const int width = previous.width, height = previous.height;
    const int step = (std::max)(1, height / 96);
    int divider = 0;
    int staticStart = 0;
    bool sparseRepaint = false;
    bool textured = false;
    for (int d = 0; d < width - 24; ++d) {
        const int x = right ? width - 1 - d : d;
        int changed = 0, brighter = 0, darker = 0, detail = 0, samples = 0;
        // Include the outer quarters without relying on the viewport's edge.
        for (int y = height / 8; y < height - height / 8; y += step) {
            const size_t i = size_t(y) * width + x;
            const size_t prior = d == 0 ? i : (right ? i + 1 : i - 1);
            const auto a = previous.pixels[i], b = current.pixels[i];
            changed += PixelError(a, b) > 12;
            if (PixelError(a, b) <= 12 && PixelError(previous.pixels[prior], current.pixels[prior]) <= 12) {
                const int edgeA = Luma(a) - Luma(previous.pixels[prior]);
                const int edgeB = Luma(b) - Luma(current.pixels[prior]);
                brighter += edgeA >= 5 && edgeB >= 5;
                darker += edgeA <= -5 && edgeB <= -5;
            }
            detail += y > 0 && PixelError(a, previous.pixels[i - width]) > 24;
            ++samples;
        }
        // A blinking indicator or hover repaint can change a few rows
        // without moving the neighboring pane.
        if (changed > (std::max)(1, samples / 24)) {
            // A changing scrollbar at the extreme edge can sit outside an
            // otherwise stationary sidebar. Look past at most one narrow strip.
            if (d < (std::min)(20, width / 16) && d - staticStart < 4) {
                staticStart = d + 1;
                textured = false;
                divider = 0;
                continue;
            }
            if (d - staticStart < 4)
                return 0;
            if (divider && d - divider <= 32)
                return divider;
            if (dividerOnly || sparseRepaint)
                return 0;
            if (changed * 2 >= samples && textured)
                return d;
            return divider;
        }
        sparseRepaint |= changed > 0;
        if (d >= 4 && (std::max)(brighter, darker) * 4 >= samples * 3)
            divider = d;
        textured |= detail >= 3;
    }
    return 0;
}

inline int DetectFixedEdge(const Frame &previous, const Frame &current, bool bottom, FixedBands sides) {
    const int width = previous.width;
    const int step = (std::max)(2, (width - sides.left - sides.right) / 128);
    const auto row = [&](const Frame &frame, int distance) {
        return frame.pixels.data() + size_t(bottom ? frame.height - 1 - distance : distance) * width;
    };
    int boundary = 0;
    int lastDivider = 0;
    bool hasDetail = false;
    const int limit = previous.height / 2;
    for (; boundary <= limit; ++boundary) {
        const auto *a = row(previous, boundary), *b = row(current, boundary);
        const auto *prior = boundary ? row(previous, boundary - 1) : a;
        const auto *priorCurrent = boundary ? row(current, boundary - 1) : b;
        int changed = 0, details = 0, divider = 0, samples = 0;
        for (int x = sides.left + 4; x < width - sides.right - 4; x += step) {
            changed += PixelError(a[x], b[x]) > 18;
            details += PixelError(a[x], a[x - 2]) > 48 || PixelError(a[x], prior[x]) > 48;
            divider += PixelError(a[x], prior[x]) > 72 && PixelError(b[x], priorCurrent[x]) > 72;
            ++samples;
        }
        // A separator across most of the viewport can mark the real edge
        // even if the scrolling area starts with identical blank rows.
        if (boundary >= 8 && divider * 4 >= samples * 3)
            lastDivider = boundary;
        if (changed > 0)
            break;
        hasDetail = hasDetail || details >= 2;
    }
    if (boundary < 8 || boundary > limit)
        return 0;
    if (lastDivider && boundary - lastDivider <= 16)
        return lastDivider;
    return hasDetail ? boundary : 0;
}

struct AlignmentScore {
    int shift{};
    double error{(std::numeric_limits<double>::infinity)()};
    int samples{}, overlap{};
};

inline AlignmentScore ScoreShift(const Frame &previous, const Frame &current, int shift, FixedBands fixed,
                                 int xStep, int yStep, bool coarse) noexcept {
    AlignmentScore result;
    result.shift = shift;
    const int width = previous.width;
    const int content = previous.height - fixed.top - fixed.bottom;
    result.overlap = content - std::abs(shift);
    const int oldTop = fixed.top + (std::max)(shift, 0);
    const int newTop = fixed.top + (std::max)(-shift, 0);
    unsigned long long cost = 0;
    int possible = 0;
    for (int y = 2; y < result.overlap - 2; y += yStep) {
        const auto *oldRow = previous.pixels.data() + size_t(y + oldTop) * width;
        const auto *newRow = current.pixels.data() + size_t(y + newTop) * width;
        const int phase = (y / yStep) % (std::max)(1, xStep / 2);
        for (int x = fixed.left + 4 + phase; x < width - fixed.right - 4; x += xStep) {
            ++possible;
            const unsigned a = oldRow[x], b = newRow[x];
            const int lumaA = Luma(a), lumaB = Luma(b);
            const int edgeA =
                std::abs(lumaA - Luma(oldRow[x - 2])) + std::abs(lumaA - Luma(oldRow[x - width * 2]));
            const int edgeB =
                std::abs(lumaB - Luma(newRow[x - 2])) + std::abs(lumaB - Luma(newRow[x - width * 2]));
            if ((std::max)(edgeA, edgeB) < 20)
                continue;
            cost += PixelError(a, b);
            ++result.samples;
        }
    }
    if (result.samples >= (std::max)(coarse ? 4 : 12, possible / 80))
        result.error = double(cost) / (result.samples * 3);
    return result;
}

struct Alignment {
    int shift{};
    FixedBands fixed;
    enum class Issue { NoMatch, LayoutChanged, MixedMotion } issue{};
    bool fixedTexture{};
};

inline bool RegionChanged(const Frame &a, const Frame &b, int left, int right, int top, int bottom) {
    if (left >= right || top >= bottom)
        return false;
    int changed = 0, samples = 0;
    for (int y = top; y < bottom; y += (std::max)(1, (bottom - top) / 64))
        for (int x = left; x < right; x += (std::max)(1, (right - left) / 48)) {
            const size_t i = size_t(y) * a.width + x;
            changed += PixelError(a.pixels[i], b.pixels[i]) > 24;
            ++samples;
        }
    return changed * 20 > samples;
}

// Track a visible rule just inside a known toolbar/composer. If it vanishes
// the bar may have expanded into the feed; matching the feed alone would
// still produce a plausible shift with the obsolete crop.
inline bool DividerDisappeared(const Frame &a, const Frame &b, FixedBands f, bool bottom) {
    const int depth = bottom ? f.bottom : f.top;
    if (!depth)
        return false;
    const int xStep = (std::max)(1, (a.width - f.left - f.right) / 80);
    for (int d = (std::max)(1, depth - 3); d <= depth; ++d) {
        const int y = bottom ? a.height - d : d;
        int oldEdges = 0, newEdges = 0, uniform = 0, unchanged = 0, samples = 0;
        const size_t anchor = size_t(bottom ? y : y - 1) * a.width + f.left;
        for (int x = f.left; x < a.width - f.right; x += xStep) {
            const size_t i = size_t(y) * a.width + x;
            const size_t chrome = bottom ? i : i - a.width;
            oldEdges += PixelError(a.pixels[i], a.pixels[i - a.width]) > 48;
            newEdges += PixelError(b.pixels[i], b.pixels[i - a.width]) > 48;
            uniform += PixelError(a.pixels[anchor], a.pixels[chrome]) <= 9;
            unchanged += PixelError(a.pixels[chrome], b.pixels[chrome]) <= 18;
            ++samples;
        }
        if (oldEdges * 5 >= samples * 4 &&
            (newEdges * 5 < samples || (uniform * 5 >= samples * 4 && unchanged * 5 < samples)))
            return true;
    }
    return false;
}

inline bool SideDividerDisappeared(const Frame &a, const Frame &b, FixedBands f, int x) {
    if (x <= 0 || x >= a.width)
        return false;
    const int step = (std::max)(1, a.height / 80);
    int oldEdges = 0, newEdges = 0, samples = 0;
    for (int y = f.top; y < a.height - f.bottom; y += step) {
        const size_t i = size_t(y) * a.width + x;
        oldEdges += PixelError(a.pixels[i], a.pixels[i - 1]) > 24;
        newEdges += PixelError(b.pixels[i], b.pixels[i - 1]) > 24;
        ++samples;
    }
    return samples > 0 && oldEdges * 5 >= samples * 4 && newEdges * 5 < samples;
}

// A displacement can fit the foreground while an unrelated wallpaper stays
// at screen coordinates. A central seam alone cannot validate an entire layer.
inline bool HasFixedTexture(const Frame &a, const Frame &b, FixedBands f, int shift) {
    const int first = f.top + (std::max)(0, -shift);
    const int end = a.height - f.bottom - (std::max)(0, shift);
    int conflicting = 0, samples = 0;
    for (int y = first; y < end; y += (std::max)(1, (end - first) / 48)) {
        for (int x = f.left; x < a.width - f.right; x += (std::max)(1, (a.width - f.left - f.right) / 80)) {
            const size_t i = size_t(y) * a.width + x, j = size_t(y + shift) * a.width + x;
            ++samples;
            if (PixelError(a.pixels[i], b.pixels[i]) <= 3 && PixelError(a.pixels[j], b.pixels[j]) <= 3 &&
                PixelError(a.pixels[j], b.pixels[i]) > 18)
                ++conflicting;
        }
    }
    return conflicting * 8 > samples;
}

inline Alignment FindIntegerAlignment(const Frame &previous, const Frame &current, int maximumAdvance,
                                      const FixedBands *knownFixed = nullptr) {
    if (!ValidFrame(previous) || !ValidFrame(current) || previous.width != current.width ||
        previous.height != current.height || previous.width < 24 || SameFrame(previous, current))
        return {};
    FixedBands fixed{};
    if (knownFixed) {
        fixed = *knownFixed;
        if (DividerDisappeared(previous, current, fixed, false) ||
            DividerDisappeared(previous, current, fixed, true) ||
            SideDividerDisappeared(previous, current, fixed, fixed.left) ||
            SideDividerDisappeared(previous, current, fixed, previous.width - fixed.right))
            return {0, fixed, Alignment::Issue::LayoutChanged};
    } else {
        fixed.left = DetectFixedSide(previous, current, false);
        fixed.right = DetectFixedSide(previous, current, true);
        // The fast side probe looks through the middle of the viewport. A
        // conversation may have new bubbles only near the top or bottom.
        if (RegionChanged(previous, current, 0, fixed.left, 0, previous.height))
            fixed.left = 0;
        if (RegionChanged(previous, current, previous.width - fixed.right, previous.width, 0, previous.height))
            fixed.right = 0;
        if (previous.width - fixed.left - fixed.right < 24)
            return {};
        fixed.top = DetectFixedEdge(previous, current, false, fixed);
        fixed.bottom = DetectFixedEdge(previous, current, true, fixed);
    }
    const int width = previous.width - fixed.left - fixed.right;
    const int content = previous.height - fixed.top - fixed.bottom;
    const int minOverlap = (std::max)(48, content * 30 / 100);
    const int maxShift = (std::min)(maximumAdvance, content - minOverlap);
    if (maxShift < 1)
        return {0, fixed};

    // Test every pixel displacement at low spatial density. Subsampling
    // offsets themselves loses the true match on sharp text and thin lines.
    const int coarseX = (std::max)(8, width / 32);
    const int coarseY = (std::max)(8, content / 28);
    constexpr size_t seedCount = 16;
    std::vector<AlignmentScore> coarse;
    coarse.reserve(2 * size_t(maxShift));
    for (int shift = -maxShift; shift <= maxShift; ++shift) {
        if (shift)
            coarse.push_back(ScoreShift(previous, current, shift, fixed, coarseX, coarseY, true));
    }
    const auto byError = [](const AlignmentScore &a, const AlignmentScore &b) {
        return a.error < b.error || (a.error == b.error && a.overlap > b.overlap);
    };
    const size_t seeds = (std::min)(seedCount, coarse.size());
    std::partial_sort(coarse.begin(), coarse.begin() + seeds, coarse.end(), byError);
    const int fineX = (std::max)(4, width / 96);
    const int fineY = (std::max)(3, content / 128);
    std::vector<AlignmentScore> fine;
    fine.reserve(seeds);
    for (size_t i = 0; i < seeds && std::isfinite(coarse[i].error); ++i)
        fine.push_back(ScoreShift(previous, current, coarse[i].shift, fixed, fineX, fineY, false));
    if (fine.empty())
        return {0, fixed};
    std::sort(fine.begin(), fine.end(), byError);
    const auto &lowestError = fine.front();
    if (!(lowestError.error < 12.0))
        return {0, fixed};
    const auto strongerOverlap = [](const AlignmentScore &a, const AlignmentScore &b) {
        return a.overlap * 2 >= b.overlap * 3 && a.samples * 2 >= b.samples * 3;
    };
    const auto opposite = [](const AlignmentScore &a, const AlignmentScore &b) {
        return (a.shift < 0) != (b.shift < 0);
    };
    const AlignmentScore *best = &lowestError;
    for (const auto &candidate : fine) {
        if (candidate.error > lowestError.error + 0.8 || candidate.error >= 12.0)
            break;
        if (opposite(candidate, *best) && strongerOverlap(candidate, *best))
            best = &candidate;
    }
    const auto stationary = ScoreShift(previous, current, 0, fixed, fineX, fineY, false);
    if (stationary.error <= best->error + 0.5)
        return {0, fixed};
    for (const auto &other : fine) {
        if (other.error > best->error + 0.8)
            break;
        const bool reverse = opposite(*best, other);
        if (!reverse && std::abs(other.shift - best->shift) < 4)
            continue;
        // An opposite-direction alias supported by a much smaller slice is
        // weaker evidence. Distinct repeats in the same direction still veto.
        if (!reverse || !strongerOverlap(*best, other))
            return {0, fixed};
    }
    if (HasFixedTexture(previous, current, fixed, best->shift))
        return {0, fixed, Alignment::Issue::MixedMotion, true};
    return {best->shift, fixed};
}

} // namespace nskry::scroll
