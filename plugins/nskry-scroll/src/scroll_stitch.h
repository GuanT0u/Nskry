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

inline int FrameDifference(const Frame &left, const Frame &right) {
    if (!ValidFrame(left) || !ValidFrame(right) || left.width != right.width || left.height != right.height)
        return 255;
    unsigned long long total = 0;
    int samples = 0;
    for (int y = 8; y < left.height - 8; y += 24)
        for (int x = 8; x < left.width - 8; x += 24) {
            const unsigned int a = left.pixels[static_cast<size_t>(y) * left.width + x];
            const unsigned int b = right.pixels[static_cast<size_t>(y) * right.width + x];
            total += abs(Channel(a, 16) - Channel(b, 16)) + abs(Channel(a, 8) - Channel(b, 8)) +
                     abs(Channel(a, 0) - Channel(b, 0));
            ++samples;
        }
    return samples ? static_cast<int>(total / static_cast<unsigned long long>(samples * 3)) : 255;
}

struct AlignmentScore {
    int shift{};
    double error{(std::numeric_limits<double>::infinity)()};
    int samples{};
};

inline AlignmentScore ScoreShift(const Frame &previous, const Frame &current, int shift, int xStep,
                                 int yStep) noexcept {
    AlignmentScore result;
    result.shift = shift;
    const int topMargin = (std::max)(8, (std::min)(24, previous.height / 8));
    const int bottomMargin = (std::max)(8, (std::min)(16, previous.height / 12));
    const int yEnd = previous.height - shift - bottomMargin;
    if (yEnd <= topMargin || previous.width <= 24)
        return result;
    unsigned long long cost = 0;
    int samples = 0, possible = 0;
    for (int y = topMargin; y < yEnd; y += yStep)
        for (int x = 12; x < previous.width - 12; x += xStep) {
            ++possible;
            const size_t oldIndex = static_cast<size_t>(y + shift) * previous.width + x;
            const size_t newIndex = static_cast<size_t>(y) * current.width + x;
            const unsigned int a = previous.pixels[oldIndex], b = current.pixels[newIndex];
            const int edgeA =
                std::abs(Luma(a) - Luma(previous.pixels[oldIndex - 2])) +
                std::abs(Luma(a) - Luma(previous.pixels[oldIndex - static_cast<size_t>(previous.width) * 2]));
            const int edgeB =
                std::abs(Luma(b) - Luma(current.pixels[newIndex - 2])) +
                std::abs(Luma(b) - Luma(current.pixels[newIndex - static_cast<size_t>(current.width) * 2]));
            if ((std::max)(edgeA, edgeB) < 20)
                continue;
            cost += static_cast<unsigned long long>(std::abs(Channel(a, 0) - Channel(b, 0)));
            cost += static_cast<unsigned long long>(std::abs(Channel(a, 8) - Channel(b, 8)));
            cost += static_cast<unsigned long long>(std::abs(Channel(a, 16) - Channel(b, 16)));
            ++samples;
        }
    const int minimumSamples = (std::max)(12, possible / 40);
    if (samples >= minimumSamples)
        result.error = static_cast<double>(cost) / static_cast<double>(samples * 3);
    result.samples = samples;
    return result;
}

inline int FindAdvance(const Frame &previous, const Frame &current, int maximumAdvance) {
    if (!ValidFrame(previous) || !ValidFrame(current) || previous.width != current.width ||
        previous.height != current.height)
        return 0;
    constexpr int minShift = 4;
    // Keep a meaningful overlap for confidence scoring, but allow a normal
    // trackpad gesture to move most of the viewport between samples. Texture-
    // poor or repetitive pages are still rejected by the score/ambiguity test.
    const int minimumOverlap = (std::max)(48, previous.height * 30 / 100);
    const int maxShift = (std::min)(maximumAdvance, previous.height - minimumOverlap);
    if (maxShift < minShift)
        return 0;

    const int coarseStep = maxShift > 240 ? 8 : (maxShift > 80 ? 4 : 2);
    const int coarseX = previous.width > 1600 ? 32 : (previous.width > 700 ? 24 : 12);
    const int coarseY = previous.height > 1200 ? 24 : (previous.height > 500 ? 16 : 8);
    std::vector<AlignmentScore> coarse;
    coarse.reserve(static_cast<size_t>((maxShift - minShift) / coarseStep + 2));
    for (int shift = minShift; shift <= maxShift; shift += coarseStep)
        coarse.push_back(ScoreShift(previous, current, shift, coarseX, coarseY));
    if (coarse.empty() || coarse.back().shift != maxShift)
        coarse.push_back(ScoreShift(previous, current, maxShift, coarseX, coarseY));
    std::sort(coarse.begin(), coarse.end(),
              [](const AlignmentScore &a, const AlignmentScore &b) { return a.error < b.error; });

    std::vector<int> candidates;
    const size_t seedCount = (std::min<size_t>)(4, coarse.size());
    for (size_t i = 0; i < seedCount && std::isfinite(coarse[i].error); ++i) {
        const int first = (std::max)(minShift, coarse[i].shift - coarseStep);
        const int last = (std::min)(maxShift, coarse[i].shift + coarseStep);
        for (int shift = first; shift <= last; ++shift)
            if (std::find(candidates.begin(), candidates.end(), shift) == candidates.end())
                candidates.push_back(shift);
    }
    if (candidates.empty())
        return 0;

    std::vector<AlignmentScore> fine;
    fine.reserve(candidates.size());
    const int fineX = previous.width > 2400 ? 16 : 12;
    const int fineY = previous.height > 1600 ? 10 : 7;
    for (const int shift : candidates)
        fine.push_back(ScoreShift(previous, current, shift, fineX, fineY));
    std::sort(fine.begin(), fine.end(),
              [](const AlignmentScore &a, const AlignmentScore &b) { return a.error < b.error; });
    if (fine.empty() || !std::isfinite(fine.front().error) || fine.front().error >= 12.0)
        return 0;

    const AlignmentScore &best = fine.front();
    double secondError = (std::numeric_limits<double>::infinity)();
    for (const AlignmentScore &score : fine) {
        if (std::abs(score.shift - best.shift) >= 4 && std::isfinite(score.error)) {
            secondError = score.error;
            break;
        }
    }
    // Repetitive pages can produce several equally plausible offsets. Reject
    // those rather than appending at an arbitrary position.
    if (std::isfinite(secondError) && secondError - best.error < 0.8 && best.error > secondError * 0.92)
        return 0;
    return best.shift;
}

// Signed displacement in document coordinates. Reverse matching uses the same
// confidence/overlap checks without reversing or copying the frame buffers.
inline int FindMotion(const Frame &previous, const Frame &current, int maximumAdvance) {
    const int down = FindAdvance(previous, current, maximumAdvance);
    const int up = FindAdvance(current, previous, maximumAdvance);
    if (down && up) {
        const double a = ScoreShift(previous, current, down, 12, 7).error;
        const double b = ScoreShift(current, previous, up, 12, 7).error;
        if (std::abs(a - b) < 0.8)
            return 0;
        return a < b ? down : -up;
    }
    return down ? down : -up;
}
struct Placement {
    int top{}, prepend{}, append{};
};
inline Placement PlaceFrame(int currentTop, int displacement, int viewportHeight, int stitchedHeight) {
    const int top = currentTop + displacement;
    return {top, (std::max)(0, -top), (std::max)(0, top + viewportHeight - stitchedHeight)};
}
inline void Extend(Frame &stitched, const Frame &next, const Placement &placement) {
    const size_t before = static_cast<size_t>(placement.prepend) * next.width;
    const size_t after = static_cast<size_t>(placement.append) * next.width;
    if (before)
        stitched.pixels.insert(stitched.pixels.begin(), next.pixels.begin(), next.pixels.begin() + before);
    if (after)
        stitched.pixels.insert(stitched.pixels.end(), next.pixels.end() - after, next.pixels.end());
    stitched.height += placement.prepend + placement.append;
}
} // namespace nskry::scroll
