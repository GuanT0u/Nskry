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
int main() {
    try {
        Frame doc{160, 1400, {}};
        unsigned seed = 43791;
        for (int i = 0; i < doc.width * doc.height; ++i) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            doc.pixels.push_back(seed & 0xffffff);
        }
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
