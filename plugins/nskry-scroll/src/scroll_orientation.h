#pragma once
#include "scroll_stitch.h"
#include <utility>

namespace nskry::scroll {
enum class ScrollAxis { Vertical, Horizontal };

inline bool SupportsAxis(int width, int height, ScrollAxis axis) noexcept {
    if (axis == ScrollAxis::Horizontal)
        std::swap(width, height);
    return width >= 64 && height >= 96;
}

// The stitcher stores the scroll axis as rows. Transposition preserves the
// sign of the offset: increasing screen X becomes increasing stitch Y.
inline void TransposePixels(const Frame &source, unsigned int *destination) noexcept {
    constexpr int tile = 32;
    for (int y = 0; y < source.height; y += tile) {
        for (int x = 0; x < source.width; x += tile) {
            for (int row = y; row < (std::min)(y + tile, source.height); ++row)
                for (int column = x; column < (std::min)(x + tile, source.width); ++column)
                    destination[size_t(column) * source.height + row] =
                        source.pixels[size_t(row) * source.width + column];
        }
    }
}

inline Frame OrientFrame(Frame frame, ScrollAxis axis) {
    if (axis == ScrollAxis::Vertical)
        return frame;
    Frame result{frame.height, frame.width, std::vector<unsigned int>(frame.pixels.size())};
    TransposePixels(frame, result.pixels.data());
    return result;
}
} // namespace nskry::scroll
