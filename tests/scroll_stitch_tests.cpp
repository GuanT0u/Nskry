#include "scroll_stitch.h"
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
        Extend(stitched, next, placement, learned);
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
        Frame last = View(doc, 500, 256), stitched = last;
        int position = 0, absolute = 500, minimum = 500, maximum = 756;
        // Reversals within the existing image, new top/bottom, and repeated revisits.
        for (int top : {540, 508, 460, 420, 460, 540, 580, 540, 420, 380, 500, 620}) {
            Frame next = View(doc, top, 256);
            const int motion = FindMotion(last, next, 176);
            Check(motion == top - absolute, "signed alignment");
            const auto placement = PlaceFrame(position, motion, next.height, stitched.height);
            Extend(stitched, next, placement);
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
