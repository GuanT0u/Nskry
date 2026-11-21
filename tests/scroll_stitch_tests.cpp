#include "scroll_composite.h"
#include "scroll_local_match.h"
#include <iostream>
#include <stdexcept>
using namespace nskry::scroll;
void Check(bool condition, const char *reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
Frame View(const Frame &document, int top, int height) {
    return {document.width,
            height,
            {document.pixels.begin() + top * document.width,
             document.pixels.begin() + (top + height) * document.width}};
}

Frame Noise(int width, int height) {
    Frame result{width, height, {}};
    result.pixels.reserve(size_t(width) * height);
    unsigned seed = 43791;
    for (int i = 0; i < width * height; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        result.pixels.push_back(seed & 0xffffff);
    }
    return result;
}

Frame BrowserView(const Frame &document, int top, int height, FixedBands fixed) {
    auto result = View(document, top, height);
    std::fill_n(result.pixels.begin(), size_t(fixed.top) * result.width, 0x102042u);
    std::fill_n(result.pixels.end() - size_t(fixed.bottom) * result.width,
                size_t(fixed.bottom) * result.width, 0xe0d7d4u);
    return result;
}

void CheckStickyBands(const Frame &doc) {
    constexpr int viewport = 256;
    constexpr FixedBands bands{56, 32};
    int absolute = 400, position = 0, minimum = absolute, maximum = absolute + viewport;
    Frame last = BrowserView(doc, absolute, viewport, bands), stitched = last;
    FixedBands learned{};
    bool haveAlignment = false;
    for (const int top : {405, 373, 320, 383, 449, 401, 400}) {
        auto next = BrowserView(doc, top, viewport, bands);
        const auto alignment = FindAlignment(last, next, 176, haveAlignment ? &learned : nullptr);
        Check(alignment.shift == top - absolute, "signed alignment with sticky header and footer");
        Check(alignment.fixed.top == bands.top && alignment.fixed.bottom == bands.bottom,
              "detect both sticky edges");
        learned = alignment.fixed;
        haveAlignment = true;
        const auto placement = PlaceFrame(position, alignment.shift, viewport, stitched.height);
        const int join = FindJoinRow(stitched, next, placement, learned);
        Check(join >= 0, "verified browser overlap");
        Extend(stitched, next, placement, learned, join);
        position = placement.top + placement.prepend;
        last = std::move(next);
        absolute = top;
        minimum = (std::min)(minimum, top);
        maximum = (std::max)(maximum, top + viewport);
        const auto expected = BrowserView(doc, minimum, maximum - minimum, bands);
        Check(stitched.height == expected.height && stitched.pixels == expected.pixels,
              "keep fixed header/footer once while extending or revisiting");
    }
}

void CheckRepeatedContent() {
    const auto tile = Noise(256, 128);
    Frame periodic{tile.width, 900, {}};
    for (int y = 0; y < periodic.height; ++y) {
        const auto begin = tile.pixels.begin() + (y % tile.height) * tile.width;
        periodic.pixels.insert(periodic.pixels.end(), begin, begin + tile.width);
    }
    Check(FindMotion(View(periodic, 256, 400), View(periodic, 288, 400), 280) == 0,
          "reject multiple exact alignments of fully periodic content");
    const auto detail = Noise(256, 48);
    std::copy(detail.pixels.begin(), detail.pixels.end(), periodic.pixels.begin() + 448 * periodic.width);
    const auto before = View(periodic, 256, 400);
    const auto after = View(periodic, 288, 400);
    Check(FindMotion(before, after, 280) == 32, "shorter reverse alias must not veto distinctive overlap");
    Check(FindMotion(after, before, 280) == -32, "same evidence supports reverse movement");
    auto changed = after;
    for (int y = 168; y < 176; ++y)
        for (int x = 0; x < changed.width; ++x)
            changed.pixels[y * changed.width + x] ^= 0x030303u;
    Check(FindMotion(before, changed, 280) == 32, "small page update must not prefer a short reverse alias");
}

Frame FeedView(const Frame &document, const Frame &rails, int top, int height, FixedBands bands) {
    auto center = BrowserView(document, top, height, bands);
    const int width = center.width + bands.left + bands.right;
    Frame frame{width, height, std::vector<unsigned>(size_t(width) * height)};
    for (int y = 0; y < height; ++y) {
        auto row = frame.pixels.begin() + y * width;
        auto rail = rails.pixels.begin() + (y % rails.height) * rails.width;
        std::copy_n(rail, bands.left, row);
        std::copy_n(center.pixels.begin() + y * center.width, center.width, row + bands.left);
        std::copy_n(rail + bands.left, bands.right, row + bands.left + center.width);
    }
    return frame;
}

void CheckSplitPane() {
    const auto doc = Noise(114, 1200), rails = Noise(301, 260);
    constexpr FixedBands bands{33, 78, 167, 134};
    constexpr int h = 260;
    int origin = 400, top = origin, pos = 0, low = origin, high = origin;
    Frame last = FeedView(doc, rails, top, h, bands), mosaic = last;
    FixedBands learned{};
    for (int target : {427, 395, 348, 415, 461, 411}) {
        auto next = FeedView(doc, rails, target, h, bands);
        auto alignment = FindAlignment(last, next, 160, top == origin ? nullptr : &learned);
        Check(alignment.shift == target - top, "track a narrow feed between two wider fixed sidebars");
        learned = alignment.fixed;
        Check(learned.left == bands.left && learned.right == bands.right, "resolve both side boundaries");
        Check(learned.top == bands.top && learned.bottom == bands.bottom, "resolve composer taller than 25%");
        auto placement = PlaceFrame(pos, alignment.shift, h, mosaic.height);
        int join = FindJoinRow(mosaic, next, placement, learned);
        Check(join >= 0, "find seam between chrome");
        Extend(mosaic, next, placement, learned, join);
        low = (std::min)(low, target);
        high = (std::max)(high, target);
        const auto expected = BrowserView(doc, low, high - low + h, bands);
        Check(mosaic.width == doc.width && mosaic.pixels == expected.pixels,
              "full window yields exactly the same rows as selecting only the center feed");
        pos = placement.top + placement.prepend;
        top = target;
        last = std::move(next);
    }
    const auto relayout = View(Noise(last.width, 600), 10, h);
    Check(!FindAlignment(last, relayout, 160, &learned).shift,
          "refuse an unrelated frame inside a locked pane");
    auto repaint = FeedView(doc, rails, top + 17, h, bands);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < 12; ++x)
            repaint.pixels[y * repaint.width + x] ^= 0xffffffu;
    const auto continued = FindAlignment(last, repaint, 160, &learned);
    Check(continued.shift == 17 && continued.fixed == learned, "ignore repaint outside the locked pane");
    auto scrollingThumb = FeedView(doc, rails, top + 17, h, bands);
    for (int y = 70; y < 150; ++y)
        std::fill_n(scrollingThumb.pixels.begin() + y * last.width, 6, 0xb4b4b4u);
    const auto chrome = FindAlignment(last, scrollingThumb, 160);
    Check(chrome.shift == 17 && chrome.fixed.left == bands.left,
          "a narrow changing scrollbar outside fixed chrome does not hide the feed");
}

void CheckWeakDivider() {
    const auto doc = Noise(150, 700), rails = Noise(220, 260);
    constexpr FixedBands bands{0, 0, 130, 90};
    const auto view = [&](int top) {
        auto frame = FeedView(doc, rails, top, 260, bands);
        for (int y = 0; y < frame.height; ++y) {
            auto row = frame.pixels.begin() + y * frame.width;
            std::fill_n(row, 38, 0x070707u);
            std::fill(row + 38, row + 129, 0x303030u);
            if (y < 78 && y % 22 < 5)
                std::fill(row + 49, row + 115, 0xbbbbbbu);
            row[129] = 0x363636u;
            if ((top + y) % 81 >= 15)
                std::fill(row + 130, row + 280, 0x303030u);
            else {
                std::fill(row + 130, row + 146, 0x303030u);
                std::fill(row + 260, row + 280, 0x303030u);
            }
        }
        return frame;
    };
    const auto result = FindAlignment(view(300), view(332), 176);
    Check(result.shift == 32 && result.fixed.left == 130 && result.fixed.right == 90,
          "a weak internal divider excludes the entire static neighboring pane");
    for (bool mirror : {false, true}) {
        auto first = view(300), second = view(332);
        second.pixels[130 * second.width + 22] ^= 0xffffffu;
        if (mirror) {
            for (Frame *frame : {&first, &second})
                for (int y = 0; y < frame->height; ++y) {
                    auto row = frame->pixels.begin() + y * frame->width;
                    std::reverse(row, row + frame->width);
                }
        }
        const auto aligned = FindAlignment(first, second, 176);
        Check(aligned.shift == 32 && aligned.fixed.left == (mirror ? 90 : 130) &&
                  aligned.fixed.right == (mirror ? 130 : 90),
              "a single changed pixel cannot hide either mirrored pane divider");
    }
}

void CheckFloatingControl() {
    const auto doc = Noise(232, 1000);
    const auto paint = [](Frame &frame) {
        for (int y = frame.height - 38; y < frame.height - 22; ++y)
            std::fill_n(frame.pixels.begin() + y * frame.width + frame.width - 45, 28, 0xffffffu);
    };
    Frame last = View(doc, 300, 260);
    paint(last);
    Frame mosaic = last;
    int top = 300, position = 0, low = top, high = top;
    for (int target : {323, 353, 396, 327, 279, 250, 316, 387, 430}) {
        auto next = View(doc, target, 260);
        paint(next);
        const auto alignment = FindAlignment(last, next, 170);
        Check(alignment.shift == target - top, "floating button permits foreground alignment");
        auto placement = PlaceFrame(position, alignment.shift, next.height, mosaic.height);
        int join = FindJoinRow(mosaic, next, placement, alignment.fixed);
        Check(join >= 0, "clean overlap away from the floating button");
        Extend(mosaic, next, placement, alignment.fixed, join);
        low = (std::min)(low, target);
        high = (std::max)(high, target);
        auto expected = View(doc, low, high - low + 260);
        paint(expected);
        Check(mosaic.pixels == expected.pixels, "floating button retained once, including after reversal");
        position = placement.top + placement.prepend;
        top = target;
        last = std::move(next);
    }
}

void CheckWallpaper() {
    constexpr int width = 300, height = 260;
    const auto pixel = [](unsigned index) {
        index ^= index >> 16;
        index *= 0x85ebca6bu;
        index ^= index >> 13;
        index *= 0xc2b2ae35u;
        return (index ^ (index >> 16)) & 0xffffffu;
    };
    const auto foreground = [](int y, int x) {
        const int left = (y / 77) % 2 ? 0 : width - 160;
        return y % 77 < 34 && x >= left && x < left + 160;
    };
    auto view = [&](int top) {
        Frame out{width, height, std::vector<unsigned>(width * height)};
        for (int y = 0; y < out.height; ++y)
            for (int x = 0; x < out.width; ++x)
                out.pixels[y * width + x] = foreground(y + top, x) ? pixel(100000 + (y + top) * width + x)
                                                                  : pixel(y * width + x);
        return out;
    };
    const auto first = view(50), second = view(83);
    const auto motion = FindAlignment(first, second, 160);
    Check(motion.shift == 33 && motion.fixed == FixedBands{} && motion.fixedTexture,
          "moving foreground can establish a shift despite stationary wallpaper");
    Check(HasFixedTexture(first, second, {}, 33), "co-located wallpaper contradicts a true message shift");
    Check(FindJoinRow(first, second, PlaceFrame(0, 33, 260, 260), {}) < 0,
          "a known true displacement alone cannot justify a seam through fixed wallpaper");
    const auto p = PlaceFrame(0, motion.shift, 260, 260);
    const int join = FindJoinRow(first, second, p, motion.fixed, &first, motion.shift);
    Check(join >= 0, "allow a seam when only stationary wallpaper differs");
    auto mosaic = first;
    Extend(mosaic, second, p, {}, join);
    for (int y = 0; y < mosaic.height; ++y)
        for (int x = 0; x < mosaic.width; ++x)
            if (foreground(y + 50, x))
                Check(mosaic.pixels[y * mosaic.width + x] == pixel(100000 + (y + 50) * width + x),
                      "every foreground pixel remains intact across a wallpaper seam");
    auto obscured = second;
    for (int y = 0; y < second.height; ++y)
        obscured.pixels[y * second.width + second.width / 2] ^= 0xffffffu;
    Check(FindJoinRow(first, obscured, p, {}, &first, 33) < 0,
          "refuse an overlap with unresolved foreground changes");
    Frame margin = Noise(width, 700);
    for (int y = 0; y < margin.height; ++y)
        std::fill_n(margin.pixels.begin() + y * margin.width, 47, 0x202020u);
    auto aligned = FindAlignment(View(margin, 50, 260), View(margin, 83, 260), 160);
    Check(aligned.shift == 33 && aligned.fixed.left == 0, "blank margins are not proven chrome");
}

void CheckBridge() {
    const auto doc = Noise(180, 1000);
    const auto first = View(doc, 200, 260), middle = View(doc, 296, 260), last = View(doc, 392, 260);
    Check(!FindAlignment(first, last, 176).shift, "direct search deliberately limits the advance");
    const int shift = BridgeAlignment(first, middle, last, 176, nullptr).shift;
    Check(shift == 192, "an intermediate observation recovers the combined displacement");
    Check(BridgeAlignment(first, last, middle, 176, nullptr).shift == 0 &&
              BridgeAlignment(first, middle, View(doc, 250, 260), 176, nullptr).shift == 0,
          "do not bridge a missing first step or reverse direction");
    auto mosaic = first;
    const auto placement = PlaceFrame(0, shift, last.height, mosaic.height);
    const int join = FindJoinRow(mosaic, last, placement, {});
    Check(join >= 0, "a bridged displacement still overlaps the prior mosaic");
    Extend(mosaic, last, placement, {}, join);
    Check(mosaic.pixels == View(doc, 200, 452).pixels, "bridging preserves every row in order");
}

void CheckWallpaperPath() {
    constexpr int width = 180, height = 180, shift = 8;
    const auto first = Noise(width, height);
    auto next = first;
    // Two disjoint areas of re-rendered foreground make every full row
    // unsafe. The fixed background between them still connects both sides.
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if ((x < 34 && y < 94) || (x >= 146 && y >= 86))
                next.pixels[y * width + x] ^= 0xffffffu;
    for (int delta : {shift, -shift}) {
        const auto &previous = delta > 0 ? first : next;
        const auto &current = delta > 0 ? next : first;
        const auto placement = PlaceFrame(0, delta, height, height);
        Check(FindJoinRow(previous, current, placement, {}, &previous, delta) < 0,
              "no straight seam through staggered foreground");
        const auto path = FindWallpaperJoinPath(previous, previous, current, placement, {}, delta);
        Check(path.size() == width && path.front() > path.back(), "find a connected seam around foreground");
        auto mosaic = previous;
        Extend(mosaic, current, placement, {}, -1, path);
        Check(mosaic.height == height + shift, "connected cut adds exactly the new rows");
        for (int y = 0; y < mosaic.height; ++y)
            for (int x = 0; x < width; ++x) {
                const int currentY = y - placement.top - placement.prepend;
                const bool useCurrent = delta > 0 ? currentY >= path[x] : currentY < path[x];
                const unsigned expected = useCurrent ? current.pixels[currentY * width + x]
                                                    : previous.pixels[(y - placement.prepend) * width + x];
                Check(mosaic.pixels[y * width + x] == expected, "merge exactly the pixels on each side of the cut");
            }
    }
    auto blocked = next;
    for (int y = 0; y < height; ++y)
        blocked.pixels[y * width + width / 2] ^= 0xffffffu;
    Check(FindWallpaperJoinPath(first, first, blocked, PlaceFrame(0, shift, height, height), {}, shift).empty(),
          "refuse a full-height obstruction instead of cutting through it");
}

void CheckSparseTranslucentForeground() {
    constexpr int width = 320, height = 260, advance = 27;
    const auto texture = [](unsigned x, unsigned y, unsigned seed) {
        unsigned value = x * 73856093u ^ y * 19349663u ^ seed;
        value ^= value >> 16;
        value *= 0x85ebca6bu;
        value ^= value >> 13;
        return value % 210 + 20;
    };
    const auto view = [&](int top) {
        Frame frame{width, height, std::vector<unsigned>(width * height)};
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const unsigned wallpaper = texture(x, y, 5101);
                const bool foreground = x >= 60 && x < 188 && (y + top) % 160 < 48;
                const unsigned value = foreground ? (texture(x, y + top, 84691) * 3 + wallpaper) / 4
                                                  : wallpaper;
                frame.pixels[y * width + x] = value * 0x010101u;
            }
        return frame;
    };
    int joined = 0;
    for (int top : {114, 132, 167, 188, 211, 234}) {
        const auto first = view(top), second = view(top + advance);
        const auto match = FindAlignment(first, second, 176);
        if (!match.shift)
            continue;
        Check(match.shift == advance && match.fixed == FixedBands{} && match.fixedTexture,
              "focused sampling must not crop the wallpaper margin or accept an incorrect shift");
        const auto placement = PlaceFrame(0, advance, height, height);
        joined += FindJoinRow(first, second, placement, {}, &first, advance) >= 0;
    }
    Check(joined >= 3, "recover sparse translucent foreground with a verifiable join");
}

void CheckComposerResize() {
    const auto doc = Noise(220, 1000);
    const auto view = [&](int offset, int composer) {
        auto frame = BrowserView(doc, offset, 260, {0, composer});
        std::fill_n(frame.pixels.begin() + (260 - composer) * frame.width, 2 * frame.width, 0x161616u);
        return frame;
    };
    const auto a = view(200, 50), b = view(230, 50);
    const auto locked = FindAlignment(a, b, 160);
    Check(locked.shift == 30 && locked.fixed.bottom == 50, "identify fixed input bar with divider");
    for (int composer : {28, 90})
        Check(FindAlignment(b, view(260, composer), 160, &locked.fixed).issue ==
                  Alignment::Issue::LayoutChanged,
              "reject changed composer geometry before writing a strip");
}

void CheckEndpointChrome() {
    const auto doc = Noise(200, 800);
    constexpr FixedBands bands{24, 30};
    constexpr int height = 240;
    auto mosaic = BrowserView(doc, 200, height, bands);
    auto down = BrowserView(doc, 240, height, bands);
    const auto footer = size_t(bands.bottom) * down.width;
    std::fill(down.pixels.end() - footer, down.pixels.end(), 0x123456u);
    auto p = PlaceFrame(0, 40, height, height);
    int join = FindJoinRow(mosaic, down, p, bands);
    Check(join >= 0, "find overlap before changing the end chrome");
    Extend(mosaic, down, p, bands, join);
    Check(std::equal(down.pixels.end() - footer, down.pixels.end(), mosaic.pixels.end() - footer),
          "bottom chrome comes from the frame supplying the new endpoint");
    auto up = BrowserView(doc, 160, height, bands);
    const auto header = size_t(bands.top) * up.width;
    std::fill_n(up.pixels.begin(), header, 0x654321u);
    p = PlaceFrame(40, -80, height, mosaic.height);
    join = FindJoinRow(mosaic, up, p, bands);
    Check(join >= 0, "find overlap after reversing");
    Extend(mosaic, up, p, bands, join);
    Check(std::equal(up.pixels.begin(), up.pixels.begin() + header, mosaic.pixels.begin()) &&
              std::equal(down.pixels.end() - footer, down.pixels.end(), mosaic.pixels.end() - footer),
          "prepend replaces the header and preserves the far footer");
}

void CheckResampledText() {
    Frame document{384, 540, std::vector<unsigned>(384 * 540, 0x212327u)};
    const auto noise = Noise(document.width, document.height);
    for (int y = 0; y < document.height; ++y) {
        const int line = y / 24;
        for (int x = 12; x < 354 - line * 53 % 64; ++x) {
            const int glyph = x / 8;
            if (y % 24 < 13 && x % 8 < 6 &&
                (noise.pixels[(line * 24) * document.width + glyph] & (1u << ((y % 13 + x % 8 * 5) % 24))))
                document.pixels[y * document.width + x] = 0xe2e5e9u;
        }
    }
    constexpr int height = 260, advance = 31;
    const auto previous = View(document, 120, height);
    const auto exact = View(document, 120 + advance, height + 1);
    auto next = View(document, 120 + advance, height);
    for (size_t i = 0; i < next.pixels.size(); ++i)
        next.pixels[i] =
            ((exact.pixels[i] & 0xfefefeu) >> 1) + ((exact.pixels[i + document.width] & 0xfefefeu) >> 1);
    const auto alignment = FindAlignment(previous, next, 170);
    Check(alignment.shift == advance || alignment.shift == advance + 1,
          "local motion survives a change in sampling of thin glyphs");
    const auto p = PlaceFrame(0, alignment.shift, height, height);
    Check(FindJoinRow(previous, next, p, alignment.fixed) >= 0,
          "resampled glyphs still have a safe join between text lines");
}

int main() {
    try {
        const auto doc = Noise(160, 1400);
        for (const int shift : {1, 5, 13, 41, 65, 129}) {
            const auto before = View(doc, 500, 600);
            const auto after = View(doc, 500 + shift, 600);
            Check(FindMotion(before, after, 420) == shift, "every positive pixel offset is searched");
            Check(FindMotion(after, before, 420) == -shift, "every negative pixel offset is searched");
        }
        auto sparse = Noise(256, 700);
        for (int y = 0; y < sparse.height; ++y)
            for (int x = 8; x < sparse.width; x += 24)
                sparse.pixels[y * sparse.width + x] = 0xffffff;
        const auto sparseBefore = View(sparse, 120, 256);
        const auto sparseAfter = View(sparse, 160, 256);
        Check(!SameFrame(sparseBefore, sparseAfter), "movement between former sample columns is visible");
        Check(FindMotion(sparseBefore, sparseAfter, 176) == 40, "align content between sample columns");
        CheckStickyBands(doc);
        CheckRepeatedContent();
        CheckSplitPane();
        CheckWeakDivider();
        CheckFloatingControl();
        CheckWallpaper();
        CheckWallpaperPath();
        CheckSparseTranslucentForeground();
        CheckBridge();
        CheckComposerResize();
        CheckEndpointChrome();
        CheckResampledText();
        Frame last = View(doc, 500, 256), stitched = last;
        int position = 0, absolute = 500, minimum = 500, maximum = 756;
        // Reversals within the existing image, new top/bottom, and repeated revisits.
        for (int top : {540, 508, 460, 420, 460, 540, 580, 540, 420, 380, 500, 620}) {
            Frame next = View(doc, top, 256);
            const int motion = FindMotion(last, next, 176);
            Check(motion == top - absolute, "signed alignment");
            const auto placement = PlaceFrame(position, motion, next.height, stitched.height);
            const int join = FindJoinRow(stitched, next, placement, {});
            Check(join >= 0, "verified document overlap");
            Extend(stitched, next, placement, {}, join);
            position = placement.top + placement.prepend;
            last = std::move(next);
            absolute = top;
            minimum = (std::min)(minimum, top);
            maximum = (std::max)(maximum, top + 256);
            Check(stitched.height == maximum - minimum, "no duplicate rows on reversal");
            Check(stitched.pixels == View(doc, minimum, maximum - minimum).pixels, "exact document order");
        }
        Check(FindMotion(last, last, 176) == 0, "stationary frame");
        Check(FindMotion(last, View(doc, 1000, 256), 176) == 0, "reject missing overlap");
        Frame blank{160, 256, std::vector<unsigned>(160 * 256, 0)};
        Check(FindMotion(blank, blank, 176) == 0, "reject textureless frames");
        Frame repeated = blank;
        for (int y = 0; y < repeated.height; ++y)
            for (int x = 0; x < repeated.width; ++x)
                repeated.pixels[y * repeated.width + x] = ((y / 4) % 2) ? 0xffffff : 0;
        Check(FindMotion(repeated, repeated, 176) == 0, "reject ambiguous repeating patterns");
        Check(FindMotion(last, blank, 176) == 0, "reject unrelated image");
        Check(FindMotion(last, Frame{}, 176) == 0, "invalid input");
        std::cout << "Bidirectional stitching, revisits and rejection tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
