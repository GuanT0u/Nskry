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
    int top{}, bottom{};
};

inline int DetectFixedEdge(const Frame &previous, const Frame &current, bool bottom) {
    const int width = previous.width;
    const int step = (std::max)(2, width / 128);
    const auto row = [&](const Frame &frame, int distance) {
        return frame.pixels.data() + size_t(bottom ? frame.height - 1 - distance : distance) * width;
    };
    int boundary = 0;
    int lastDivider = 0;
    bool hasDetail = false;
    const int limit = previous.height / 4;
    for (; boundary <= limit; ++boundary) {
        const auto *a = row(previous, boundary), *b = row(current, boundary);
        const auto *prior = boundary ? row(previous, boundary - 1) : a;
        const auto *priorCurrent = boundary ? row(current, boundary - 1) : b;
        int changed = 0, details = 0, divider = 0, samples = 0;
        for (int x = 4; x < width - 4; x += step) {
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
        for (int x = 4 + phase; x < width - 4; x += xStep) {
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
};

inline Alignment FindAlignment(const Frame &previous, const Frame &current, int maximumAdvance,
                               const FixedBands *knownFixed = nullptr) {
    if (!ValidFrame(previous) || !ValidFrame(current) || previous.width != current.width ||
        previous.height != current.height || previous.width < 24 || SameFrame(previous, current))
        return {};
    const FixedBands fixed = knownFixed ? *knownFixed
                                        : FixedBands{DetectFixedEdge(previous, current, false),
                                                     DetectFixedEdge(previous, current, true)};
    const int content = previous.height - fixed.top - fixed.bottom;
    const int minOverlap = (std::max)(48, content * 30 / 100);
    const int maxShift = (std::min)(maximumAdvance, content - minOverlap);
    if (maxShift < 1)
        return {};

    // Test every pixel displacement at low spatial density. Subsampling
    // offsets themselves loses the true match on sharp text and thin lines.
    const int coarseX = (std::max)(8, previous.width / 32);
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
    const int fineX = (std::max)(4, previous.width / 96);
    const int fineY = (std::max)(3, content / 128);
    std::vector<AlignmentScore> fine;
    fine.reserve(seeds);
    for (size_t i = 0; i < seeds && std::isfinite(coarse[i].error); ++i)
        fine.push_back(ScoreShift(previous, current, coarse[i].shift, fixed, fineX, fineY, false));
    if (fine.empty())
        return {};
    std::sort(fine.begin(), fine.end(), byError);
    const auto &lowestError = fine.front();
    if (!(lowestError.error < 12.0))
        return {};
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
        return {};
    for (const auto &other : fine) {
        if (other.error > best->error + 0.8)
            break;
        const bool reverse = opposite(*best, other);
        if (!reverse && std::abs(other.shift - best->shift) < 4)
            continue;
        // An opposite-direction alias supported by a much smaller slice is
        // weaker evidence. Distinct repeats in the same direction still veto.
        if (!reverse || !strongerOverlap(*best, other))
            return {};
    }
    return {best->shift, fixed};
}

inline int FindMotion(const Frame &previous, const Frame &current, int maximumAdvance) {
    return FindAlignment(previous, current, maximumAdvance).shift;
}
struct Placement {
    int top{}, prepend{}, append{};
};
inline Placement PlaceFrame(int currentTop, int displacement, int viewportHeight, int stitchedHeight) {
    const int top = currentTop + displacement;
    return {top, (std::max)(0, -top), (std::max)(0, top + viewportHeight - stitchedHeight)};
}
inline void Extend(Frame &stitched, const Frame &next, const Placement &placement, FixedBands fixed = {}) {
    const auto width = size_t(next.width);
    const auto before = size_t(placement.prepend) * width;
    const auto after = size_t(placement.append) * width;
    if (before) {
        const auto head = size_t(fixed.top) * width;
        stitched.pixels.insert(stitched.pixels.begin() + head, next.pixels.begin() + head,
                               next.pixels.begin() + head + before);
    }
    if (after) {
        const auto foot = size_t(fixed.bottom) * width;
        const auto end = next.pixels.end() - foot;
        stitched.pixels.insert(stitched.pixels.end() - foot, end - after, end);
    }
    stitched.height += placement.prepend + placement.append;
}
} // namespace nskry::scroll
