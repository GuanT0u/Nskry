#pragma once
#include "scroll_stitch.h"
#include <cstring>

namespace nskry::scroll {
struct Placement {
    int top{}, prepend{}, append{};
};
inline Placement PlaceFrame(int currentTop, int displacement, int viewportHeight, int stitchedHeight) {
    const int top = currentTop + displacement;
    return {top, (std::max)(0, -top), (std::max)(0, top + viewportHeight - stitchedHeight)};
}

inline bool WallpaperJoinPixel(unsigned mosaic, const Frame &previous, const Frame &next,
                               int x, int y, int shift) {
    const size_t here = size_t(y) * next.width + x;
    if (PixelError(mosaic, next.pixels[here]) <= 3)
        return true;
    const size_t prior = size_t(y + shift) * next.width + x;
    return PixelError(mosaic, previous.pixels[prior]) <= 3 &&
           PixelError(previous.pixels[prior], next.pixels[prior]) <= 3 &&
           PixelError(previous.pixels[here], next.pixels[here]) <= 3;
}

// Compare against the actual mosaic: after a reversal its edge may have
// come from a different visit. Only splice inside a verified overlap.
inline int FindJoinRow(const Frame &stitched, const Frame &next, Placement p, FixedBands f,
                       const Frame *wallpaperReference = nullptr, int shift = 0) {
    if (!p.append && !p.prepend)
        return 0;
    const int begin = (std::max)(f.top, f.top - p.top);
    const int end = (std::min)(next.height - f.bottom, stitched.height - f.bottom - p.top);
    const int width = next.width - f.left - f.right;
    const int offset = stitched.width == next.width ? f.left : 0;
    const int middle = begin + (end - begin) / 2;
    const int radius = (end - begin) / 4;
    const int xStep = wallpaperReference ? 1 : (std::max)(1, width / 300);
    if (end - begin < 8)
        return -1;
    // Stay away from the viewport edges where jump buttons, scrollbars and
    // composer shadows live. Test a neighborhood, not just one blank row.
    for (int i = 0; i <= 48; ++i) {
        // Try the central region first. A local overlay can block it even
        // when the remaining overlap still contains a clean five-row band.
        const int y = std::clamp(middle + (i % 2 ? 1 : -1) * ((i + 1) / 2) * radius / 16, begin + 2, end - 3);
        bool clean = true;
        for (int dy = -2; dy <= 2 && clean; ++dy) {
            const int row = y + dy, oldRow = row + shift;
            const auto *old = stitched.pixels.data() + size_t(p.top + y + dy) * stitched.width + offset;
            const auto *fresh = next.pixels.data() + size_t(y + dy) * next.width + f.left;
            if (wallpaperReference && (oldRow < f.top || oldRow >= next.height - f.bottom)) {
                clean = false;
                break;
            }
            int misses = 0, samples = 0, run = 0;
            for (int x = (dy + 2) % xStep; x < width; x += xStep) {
                if (wallpaperReference) {
                    // Both exposed pixels must belong to screen-stationary
                    // content. The mosaic may have come from an older visit.
                    if (!WallpaperJoinPixel(old[x], *wallpaperReference, next, f.left + x, row, shift)) {
                        clean = false;
                        break;
                    }
                    continue;
                }
                const bool different = PixelError(old[x], fresh[x]) > 18;
                misses += different;
                run = different ? run + 1 : 0;
                ++samples;
                if (run >= 3) {
                    clean = false;
                    break;
                }
            }
            if (misses * 30 > samples)
                clean = false;
        }
        if (clean)
            return y;
    }
    return -1;
}

// Find a connected cut through verified pixels when no entire row is safe.
// A bounded central band keeps both the search and storage proportional to width.
inline std::vector<int> FindWallpaperJoinPath(const Frame &stitched, const Frame &previous,
                                              const Frame &next, Placement p, FixedBands f, int shift) {
    if (!p.append && !p.prepend)
        return {};
    const int begin = (std::max)({f.top, f.top - p.top, f.top - shift});
    const int end = (std::min)({next.height - f.bottom, stitched.height - f.bottom - p.top,
                                next.height - f.bottom - shift});
    if (end - begin < 8)
        return {};
    const int middle = begin + (end - begin) / 2;
    const int first = (std::max)(begin + 2, middle - 32);
    const int last = (std::min)(end - 3, middle + 32);
    const int rows = last - first + 1, width = next.width - f.left - f.right;
    const int offset = stitched.width == next.width ? f.left : 0;
    constexpr signed char unreachable = 2;
    std::vector<signed char> parents(size_t(width) * rows, unreachable);
    for (int x = 0; x < width; ++x) {
        auto *current = parents.data() + size_t(x) * rows;
        bool reachable = false;
        for (int r = 0; r < rows; ++r) {
            for (int step : {0, -1, 1}) {
                const int prior = r + step;
                if (x && (prior < 0 || prior >= rows || (current - rows)[prior] == unreachable))
                    continue;
                const int cut = first + r;
                bool safe = true;
                for (int y = cut - 2; y <= cut + 2 && safe; ++y) {
                    const unsigned pixel = stitched.pixels[size_t(y + p.top) * stitched.width + offset + x];
                    safe = WallpaperJoinPixel(pixel, previous, next, x + f.left, y, shift);
                }
                if (safe) {
                    current[r] = static_cast<signed char>(step);
                    reachable = true;
                }
                break;
            }
        }
        if (!reachable)
            return {};
    }
    const auto *tail = parents.data() + size_t(width - 1) * rows;
    int row = 0;
    for (int r = 1; r < rows; ++r)
        if (tail[r] != unreachable &&
            (tail[row] == unreachable || std::abs(first + r - middle) < std::abs(first + row - middle)))
            row = r;
    std::vector<int> path(width);
    for (int x = width - 1; x >= 0; --x) {
        path[x] = first + row;
        row += parents[size_t(x) * rows + row];
    }
    return path;
}

// Reserve/budget checks and seam validation precede this operation. Compact
// the first frame in place so matching can keep full frames without another
// viewport-sized buffer. Every output path (preview, edit, copy) uses this image.
inline void Extend(Frame &stitched, const Frame &next, Placement p, FixedBands f, int join,
                   const std::vector<int> &path = {}) {
    const int width = next.width - f.left - f.right;
    if (width != stitched.width) {
        for (int y = 0; y < stitched.height; ++y)
            std::memmove(stitched.pixels.data() + size_t(y) * width,
                         stitched.pixels.data() + size_t(y) * stitched.width + f.left,
                         size_t(width) * sizeof(unsigned));
        stitched.width = width;
        stitched.pixels.resize(size_t(width) * stitched.height);
    }
    const int added = p.prepend + p.append;
    if (!added)
        return;
    const int oldHeight = stitched.height;
    stitched.height += added;
    stitched.pixels.resize(size_t(stitched.height) * width);
    const int insertAt = p.prepend ? f.top : oldHeight - f.bottom;
    auto *data = stitched.pixels.data();
    std::memmove(data + size_t(insertAt + added) * width, data + size_t(insertAt) * width,
                 size_t(oldHeight - insertAt) * width * sizeof(unsigned));
    // A translucent bar and a floating control may overlap. Use the same
    // source frame for the newly exposed end and its chrome.
    const int low = path.empty() ? join : *std::min_element(path.begin(), path.end());
    const int high = path.empty() ? join : *std::max_element(path.begin(), path.end());
    const int first = p.prepend ? 0 : low;
    const int last = p.prepend ? high : next.height;
    for (int y = first; y < last; ++y) {
        const auto *source = next.pixels.data() + size_t(y) * next.width + f.left;
        auto *destination = data + size_t(p.top + p.prepend + y) * width;
        if (y < low || y >= high) {
            std::copy_n(source, width, destination);
        } else {
            for (int x = 0; x < width; ++x)
                if (p.prepend ? y < path[x] : y >= path[x])
                    destination[x] = source[x];
        }
    }
}
} // namespace nskry::scroll
