#pragma once
#include "ocr_arbitration.h"

namespace nskry::ocr {
struct TextLayout {
    std::vector<RecognizedLine> lines; // Visual rows/nearby spans, never paragraph-height rectangles.
    std::wstring text;                 // Logical paragraphs. The native text pane soft-wraps these.
};
// Presentation only: never modifies pass candidates, recognition or arbitration.
TextLayout BuildTextLayout(const std::vector<RecognizedLine> &recognized);
// Selection indices are in reading order. Keeps character hit testing while
// painting a single contiguous selection overlay per visual line/span.
std::vector<RECT> SelectionRects(const std::vector<WordBox> &words, const std::vector<size_t> &indices);
} // namespace nskry::ocr
