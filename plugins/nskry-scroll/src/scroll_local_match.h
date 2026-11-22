#pragma once
#include "scroll_stitch.h"
#include <array>

namespace nskry::scroll {
// Small scrolls on a fixed background often preserve the glyphs exactly.
// Use only changing edges whose cross of five pixels has a unique old position.
inline int FindMovingEdges(const Frame &a, const Frame &b, FixedBands f, int limit) {
    if (limit < 1)
        return 0;
    const int width = a.width - f.left - f.right, height = a.height - f.top - f.bottom;
    struct Vote { int count{}; unsigned rows{}; };
    std::vector<Vote> votes(2 * limit + 1);
    int voters = 0;
    constexpr int grid = 9;
    for (int band = 0; band < grid; ++band) {
        for (int col = 0; col < grid; ++col) {
            int best = 0, anchorY = 0, anchorX = 0;
            for (int y = f.top + height * band / grid + 1; y < f.top + height * (band + 1) / grid - 1;
                 y += (std::max)(1, height / 180)) {
                for (int x = f.left + width * col / grid + 1; x < f.left + width * (col + 1) / grid - 1;
                     x += (std::max)(1, width / 240)) {
                    const auto i = size_t(y) * b.width + x;
                    const int edge = PixelError(b.pixels[i - b.width], b.pixels[i + b.width]);
                    const int quality = (std::min)(edge, PixelError(a.pixels[i], b.pixels[i]));
                    if (quality > best) {
                        best = quality;
                        anchorY = y;
                        anchorX = x;
                    }
                }
            }
            if (best < 48)
                continue;
            const auto i = size_t(anchorY) * b.width + anchorX;
            int displacement = 0;
            for (int dy = (std::max)(-limit, f.top + 1 - anchorY);
                 dy <= (std::min)(limit, b.height - f.bottom - 2 - anchorY); ++dy) {
                const auto j = size_t(anchorY + dy) * a.width + anchorX;
                if (PixelError(a.pixels[j], b.pixels[i]) > 3)
                    continue;
                if (PixelError(a.pixels[j - 1], b.pixels[i - 1]) > 3 ||
                    PixelError(a.pixels[j + 1], b.pixels[i + 1]) > 3 ||
                    PixelError(a.pixels[j - a.width], b.pixels[i - b.width]) > 3 ||
                    PixelError(a.pixels[j + a.width], b.pixels[i + b.width]) > 3)
                    continue;
                if (displacement) {
                    displacement = 0;
                    break;
                }
                displacement = dy;
            }
            if (displacement) {
                auto &vote = votes[displacement + limit];
                ++vote.count;
                vote.rows |= 1u << band;
                ++voters;
            }
        }
    }
    for (int shift = -limit; shift <= limit; ++shift) {
        const auto &vote = votes[shift + limit];
        if (vote.count >= 3 && vote.count * 3 >= voters * 2 && std::popcount(vote.rows) >= 2 &&
            HasFixedTexture(a, b, f, shift, TextureCheck::VerifiedMovingEdges))
            return shift;
    }
    return 0;
}

// Sample a lightly filtered luminance image at a bounded number of columns.
// Keep every row so narrow text strokes cannot fall between shift candidates.
inline int FindLocalShift(const Frame &a, const Frame &b, FixedBands f, int limit,
                          bool focusOnChanges = false) {
    const FixedBands wallpaperArea = f;
    if (focusOnChanges) {
        // Narrow the sampling grid, not the output. A sparse moving object
        // can leave a large stationary margin that is still part of its pane.
        int left = a.width - f.right, right = f.left;
        const int step = (std::max)(1, a.height / 128);
        for (int x = f.left; x < a.width - f.right; ++x) {
            int changed = 0;
            for (int y = f.top; y < a.height - f.bottom && changed < 3; y += step) {
                const size_t i = size_t(y) * a.width + x;
                changed += PixelError(a.pixels[i], b.pixels[i]) > 18;
            }
            if (changed == 3) {
                left = (std::min)(left, x);
                right = x + 1;
            }
        }
        if (right <= left || (right - left) * 4 >= (a.width - f.left - f.right) * 3)
            return 0;
        f.left = (std::max)(f.left, left - 8);
        f.right = (std::max)(f.right, a.width - right - 8);
    }
    const int width = a.width - f.left - f.right, height = a.height - f.top - f.bottom;
    if (height > 4320 || height < 48 || width < 24)
        return 0;
    const int step = (std::max)(2, (width + 159) / 160);
    const int columns = (width + step - 1) / step;
    const auto sample = [&](const Frame &frame) {
        std::vector<unsigned char> rows(size_t(columns) * height);
        for (int y = 0; y < height; ++y) {
            const auto *row = frame.pixels.data() + size_t(y + f.top) * frame.width + f.left;
            const int above = y ? frame.width : 0;
            const int below = y + 1 < height ? frame.width : 0;
            for (int col = 0; col < columns; ++col) {
                const int x = col * step;
                rows[size_t(y) * columns + col] = static_cast<unsigned char>(
                    (Luma(row[x - above]) + 2 * Luma(row[x]) + Luma(row[x + below])) / 4);
            }
        }
        return rows;
    };
    const auto old = sample(a), next = sample(b);
    const auto voteForShift = [&](bool suppressWallpaper) {
        struct Vote {
            int shift, row, column;
        };
        std::vector<Vote> votes;
        constexpr int tileHeight = 16;
        const int tileWidth = std::clamp(columns / 8, 4, 14);
        const int grid = suppressWallpaper ? 7 : 5;
        for (int band = 0; band < grid; ++band) {
            const int centerY = (height - tileHeight) * band / (grid - 1);
            for (int lane = 0; lane < grid; ++lane) {
                const int center = (columns - tileWidth) * lane / (grid - 1);
                int x = center, y = centerY, energy = 0;
                // Prefer changing texture to stationary wallpaper. A short
                // vertical search also finds messages between the grid rows.
                for (int dy : {0, -tileHeight / 2, tileHeight / 2, -tileHeight, tileHeight}) {
                    const int row = std::clamp(centerY + dy, 0, height - tileHeight);
                    for (int col = (std::max)(0, center - columns / 12);
                         col <= (std::min)(columns - tileWidth, center + columns / 12); col += 2) {
                        int texture = 0;
                        for (int ty = 2; ty < tileHeight; ty += 2)
                            for (int dx = 0; dx < tileWidth; ++dx) {
                                const size_t i = size_t(row + ty) * columns + col + dx;
                                const int edge = std::abs(next[i] - next[i - 2 * columns]);
                                const bool changed = std::abs(next[i] - old[i]) > 5;
                                texture += suppressWallpaper ? (changed ? edge : 0) : edge * (changed ? 3 : 1);
                            }
                        if (texture > energy) {
                            energy = texture;
                            x = col;
                            y = row;
                        }
                    }
                }
                if (energy < tileWidth * (suppressWallpaper ? 10 : 30))
                    continue;
                const int first = (std::max)(-limit, -y), last = (std::min)(limit, height - tileHeight - y);
                std::vector<double> scores(size_t(last - first + 1), -1.0);
                int best = 0;
                for (int shift = first; shift <= last; ++shift) {
                    int sumOld = 0, sumNew = 0, squareOld = 0, squareNew = 0, cross = 0, count = 0;
                    for (int dy = 0; dy < tileHeight; dy += 2) {
                        const size_t i = size_t(y + dy) * columns + x;
                        const size_t j = size_t(y + dy + shift) * columns + x;
                        for (int dx = 0; dx < tileWidth; ++dx) {
                            // The other frame at the same screen coordinate is
                            // a proxy for the wallpaper exposed by scrolling.
                            const int u = old[j + dx] - (suppressWallpaper ? next[j + dx] / 2 : 0);
                            const int v = next[i + dx] - (suppressWallpaper ? old[i + dx] / 2 : 0);
                            sumOld += u;
                            sumNew += v;
                            squareOld += u * u;
                            squareNew += v * v;
                            cross += u * v;
                            ++count;
                        }
                    }
                    const double varianceOld = double(count) * squareOld - double(sumOld) * sumOld;
                    const double varianceNew = double(count) * squareNew - double(sumNew) * sumNew;
                    if (varianceOld > 0 && varianceNew > 0)
                        scores[shift - first] = (double(count) * cross - double(sumOld) * sumNew) /
                                                std::sqrt(varianceOld * varianceNew);
                    if (scores[shift - first] > scores[best - first])
                        best = shift;
                }
                const double peak = scores[best - first];
                if (peak < (suppressWallpaper ? 0.86 : 0.92))
                    continue;
                bool unique = true;
                for (int shift = first; shift <= last && unique; ++shift)
                    if (std::abs(shift - best) > 2 && scores[shift - first] > peak - 0.06)
                        unique = false;
                if (unique)
                    votes.push_back({best, y, x});
            }
        }
        int best = 0, support = 0, exactSupport = 0, movingVotes = 0;
        for (const auto &vote : votes)
            movingVotes += vote.shift != 0;
        for (const auto &candidate : votes) {
            if (!candidate.shift)
                continue;
            int count = 0, exact = 0;
            std::array<const Vote *, 49> independent{};
            bool differentRow = false;
            for (const auto &vote : votes) {
                if (!vote.shift || std::abs(vote.shift - candidate.shift) > 1)
                    continue;
                const auto end = independent.begin() + count;
                const auto begin = suppressWallpaper || !count ? independent.begin() : end - 1;
                if (std::any_of(begin, end, [&](const Vote *other) {
                        return std::abs(vote.row - other->row) < tileHeight / 2 &&
                               std::abs(vote.column - other->column) < tileWidth / 2;
                    }))
                    continue;
                independent[count++] = &vote;
                exact += vote.shift == candidate.shift;
                differentRow |= std::abs(vote.row - candidate.row) >= tileHeight;
            }
            if (differentRow && (count > support || (count == support && exact > exactSupport))) {
                best = candidate.shift;
                support = count;
                exactSupport = exact;
            }
        }
        if (support < 3 || support * 3 < movingVotes * 2)
            return 0;
        if ((!focusOnChanges && !suppressWallpaper && size_t(support) * 3 >= votes.size() * 2) ||
            HasFixedTexture(a, b, wallpaperArea, best))
            return best;
        return 0;
    };
    const int shift = voteForShift(false);
    return shift ? shift : voteForShift(true);
}

// Translucent chrome changes with the content underneath it, but a broad,
// high-contrast separator can stay at the same screen coordinate.
inline int FindStableDivider(const Frame &a, const Frame &b, FixedBands f, bool bottom) {
    const int step = (std::max)(1, (a.width - f.left - f.right) / 96);
    for (int depth = 8; depth <= a.height / 3; ++depth) {
        const int y = bottom ? a.height - depth : depth;
        const int other = bottom ? y + 1 : y - 1;
        const auto *old = a.pixels.data() + size_t(y) * a.width;
        const auto *next = b.pixels.data() + size_t(y) * a.width;
        const int delta = (other - y) * a.width;
        const unsigned divider = old[(f.left + a.width - f.right) / 2];
        int support = 0, uniform = 0, count = 0;
        for (int x = f.left; x < a.width - f.right; x += step) {
            ++count;
            uniform += PixelError(divider, old[x]) <= 24;
            if (PixelError(old[x], next[x]) <= 9 && PixelError(old[x], old[x + delta]) >= 60 &&
                PixelError(next[x], next[x + delta]) >= 60)
                ++support;
        }
        if (support * 5 >= count * 4 && uniform * 5 >= count * 3)
            return depth;
    }
    return 0;
}

inline Alignment EstimateAlignment(const Frame &a, const Frame &b, int maximumAdvance,
                                   const FixedBands *knownFixed) {
    const auto integer = FindIntegerAlignment(a, b, maximumAdvance, knownFixed);
    if (integer.shift || integer.issue == Alignment::Issue::LayoutChanged)
        return integer;
    FixedBands f = integer.fixed;
    if (!knownFixed) {
        if (!f.top)
            f.top = FindStableDivider(a, b, f, false);
        if (!f.bottom)
            f.bottom = FindStableDivider(a, b, f, true);
    }
    const auto searchLimit = [&](FixedBands area) {
        const int height = a.height - area.top - area.bottom;
        return (std::min)(maximumAdvance, height - (std::max)(48, height * 30 / 100));
    };
    const auto verifyDividers = [&](FixedBands area) {
        const auto confirmedEdge = [&](int depth, bool bottom) {
            if (!depth || DetectFixedEdge(a, b, bottom, area, true) == depth)
                return depth;
            return (std::min)(depth, FindStableDivider(a, b, area, bottom));
        };
        area.top = confirmedEdge(area.top, false);
        area.bottom = confirmedEdge(area.bottom, true);
        if (area.left && DetectFixedSide(a, b, false, true) != area.left)
            area.left = 0;
        if (area.right && DetectFixedSide(a, b, true, true) != area.right)
            area.right = 0;
        return area;
    };
    const int limit = searchLimit(f);
    if (limit < 1)
        return integer;
    int shift = FindMovingEdges(a, b, f, limit);
    const bool verifiedEdges = shift != 0;
    if (!shift)
        shift = FindLocalShift(a, b, f, limit);
    if (!shift)
        shift = FindLocalShift(a, b, f, limit, true);
    if (!shift && !knownFixed) {
        // Sparse foreground can make wallpaper at the boundary look like
        // fixed chrome. The focused retry requires independent wallpaper
        // evidence, so keep only crop boundaries backed by a divider.
        const auto wider = verifyDividers(f);
        if (wider != f) {
            f = wider;
            shift = FindLocalShift(a, b, f, searchLimit(f), true);
        }
    }
    if (!shift)
        return integer;
    const bool wallpaper = verifiedEdges || HasFixedTexture(a, b, f, shift);
    if (wallpaper && !knownFixed)
        f = verifyDividers(f);
    return {shift, f, Alignment::Issue::NoMatch, wallpaper};
}

// A mostly empty sidebar candidate can be a sparse column of the document.
// Look for changing edges at both ends of the proposed motion. A single
// blinking caret or a stationary rule does not provide this evidence.
inline bool SideMovesWithContent(const Frame &a, const Frame &b, Alignment match, int left, int right) {
    if (right <= left)
        return false;
    const int begin = match.fixed.top + (std::max)(0, -match.shift);
    const int end = b.height - match.fixed.bottom - (std::max)(0, match.shift);
    const int xStep = (std::max)(1, (right - left) / 256);
    const int yStep = (std::max)(1, (end - begin) / 720);
    const size_t above = size_t(yStep) * b.width;
    const auto changedEdge = [&](size_t k) {
        return PixelError(a.pixels[k], b.pixels[k]) > 24 ||
               PixelError(a.pixels[k - above], b.pixels[k - above]) > 24;
    };
    int firstRow = -1;
    for (int y = begin + yStep; y < end; y += yStep) {
        int columns = 0;
        for (int x = left; x < right; x += xStep) {
            const size_t i = size_t(y) * b.width + x;
            const size_t j = size_t(y + match.shift) * a.width + x;
            if (PixelError(b.pixels[i], b.pixels[i - above]) < 32 ||
                PixelError(a.pixels[j], b.pixels[i]) > 3 ||
                PixelError(a.pixels[j - above], b.pixels[i - above]) > 3 ||
                !changedEdge(i) || !changedEdge(j))
                continue;
            if (++columns < 2)
                continue;
            if (firstRow >= 0 && y - firstRow >= 2)
                return true;
            if (firstRow < 0)
                firstRow = y;
            break;
        }
    }
    return false;
}

inline Alignment FindAlignment(const Frame &a, const Frame &b, int maximumAdvance,
                               const FixedBands *knownFixed = nullptr) {
    if (!ValidFrame(a) || !ValidFrame(b) || a.width != b.width || a.height != b.height ||
        a.width < 24 || SameFrame(a, b))
        return {};
    auto match = EstimateAlignment(a, b, maximumAdvance, knownFixed);
    if (!match.shift)
        return match;
    const bool keepLeft = SideMovesWithContent(a, b, match, 0, match.fixed.left);
    const bool keepRight = SideMovesWithContent(a, b, match, a.width - match.fixed.right, a.width);
    if (keepLeft || keepRight) {
        if (knownFixed)
            return {0, *knownFixed, Alignment::Issue::LayoutChanged};
        if (keepLeft)
            match.fixed.left = 0;
        if (keepRight)
            match.fixed.right = 0;
        // A wider first frame must validate its seam against all retained pixels.
        match.fixedTexture = match.fixedTexture || HasFixedTexture(a, b, match.fixed, match.shift);
    }
    return match;
}

inline int FindMotion(const Frame &a, const Frame &b, int maximumAdvance) {
    return FindAlignment(a, b, maximumAdvance).shift;
}

// The intermediate observation supplies a displacement, never pixels.
// The caller must still verify the combined placement against the mosaic.
inline Alignment BridgeAlignment(const Frame &last, const Frame &probe, const Frame &next,
                                 int limit, const FixedBands *knownFixed) {
    const auto first = FindAlignment(last, probe, limit, knownFixed);
    if (!first.shift)
        return {};
    const auto second = FindAlignment(probe, next, limit, &first.fixed);
    if (!second.shift || (first.shift > 0) != (second.shift > 0))
        return {};
    const int shift = first.shift + second.shift;
    const int overlap = next.height - first.fixed.top - first.fixed.bottom - std::abs(shift);
    if (overlap < 48)
        return {};
    return {shift, first.fixed, Alignment::Issue::NoMatch,
            first.fixedTexture || second.fixedTexture || HasFixedTexture(last, next, first.fixed, shift)};
}
} // namespace nskry::scroll
