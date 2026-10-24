#include "ocr_layout.h"
#include <algorithm>
#include <cmath>

namespace nskry::ocr {
namespace {
int Height(const RECT &r) { return (std::max)(1L, r.bottom - r.top); }
int Width(const RECT &r) { return (std::max)(1L, r.right - r.left); }
bool Vertical(const RecognizedLine &line) {
    if (line.text.size() < 2 || Height(line.rect) <= 1.8 * Width(line.rect))
        return false;
    if (line.words.size() < 2)
        return true;
    const auto &a = line.words.front().rect;
    const auto &b = line.words.back().rect;
    return std::abs((b.top + b.bottom) - (a.top + a.bottom)) >
           std::abs((b.left + b.right) - (a.left + a.right));
}
RECT Unite(const RECT &a, const RECT &b) {
    return {(std::min)(a.left, b.left), (std::min)(a.top, b.top), (std::max)(a.right, b.right),
            (std::max)(a.bottom, b.bottom)};
}
bool SameRow(const RECT &a, const RECT &b) {
    const double h = (std::min)(Height(a), Height(b));
    const double large = (std::max)(Height(a), Height(b));
    const double overlap = (std::min)(a.bottom, b.bottom) - (std::max)(a.top, b.top);
    // Commas/full stops often have a much shorter, lower bounding box.
    return overlap >= .55 * h &&
           std::abs((a.top + a.bottom) - (b.top + b.bottom)) <= 1.05 * large;
}
bool Near(const RECT &a, const RECT &b) {
    const double h = (std::max)(double((std::min)(Height(a), Height(b))),
                                .5 * (std::max)(Height(a), Height(b)));
    const double gap = b.left - a.right;
    return SameRow(a, b) && gap >= -.25 * (std::max)(Height(a), Height(b)) && gap <= 1.5 * h;
}
bool Han(wchar_t c) { return (c >= 0x3400 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff); }
bool Space(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }
bool Contains(const wchar_t *set, wchar_t c) {
    for (; *set; ++set)
        if (*set == c)
            return true;
    return false;
}
bool NeedSpace(const std::wstring &a, const std::wstring &b, bool wrapped, double gap, int height) {
    if (a.empty() || b.empty() || Space(a.back()) || Space(b.front()))
        return false;
    if (Contains(L",.;:!?，。；：！？、）)]}》」』”’", b.front()) || Contains(L"（([{《「『“‘", a.back()))
        return false;
    if (Han(a.back()) && Han(b.front()))
        return false;
    if (Han(a.back()) || Han(b.front()))
        return !wrapped && gap > .35 * height;
    return wrapped || gap > .12 * height;
}
bool Bullet(const std::wstring &text) {
    size_t i = text.find_first_not_of(L" \t");
    if (i == text.npos)
        return false;
    if (Contains(L"•●▪◦", text[i]))
        return true;
    if (Contains(L"-*", text[i]))
        return i + 1 < text.size() && Space(text[i + 1]);
    const size_t begin = i;
    while (i < text.size() && text[i] >= L'0' && text[i] <= L'9' && i - begin < 3)
        ++i;
    return i > begin && i < text.size() &&
           (text[i] == L'、' || (Contains(L".)", text[i]) && i + 1 < text.size() && Space(text[i + 1])));
}
void AppendWord(RecognizedLine &line, WordBox word) {
    if (line.words.empty())
        line.rect = word.rect;
    else
        line.rect = Unite(line.rect, word.rect);
    if (!line.text.empty() && !word.joinPrevious &&
        NeedSpace(line.text, word.text, false,
                  line.words.empty() ? 0 : word.rect.left - line.words.back().rect.right, Height(word.rect)))
        line.text += L' ';
    line.text += word.text;
    line.words.push_back(std::move(word));
}
} // namespace

TextLayout BuildTextLayout(const std::vector<RecognizedLine> &recognized) {
    std::vector<RecognizedLine> fragments;
    for (const auto &line : recognized) {
        if (line.text.empty())
            continue;
        // Rotated OCR maps glyphs back into a vertical image column. Do not
        // sort/split those glyphs using horizontal spacing rules.
        if (Vertical(line)) {
            fragments.push_back(line);
            continue;
        }
        if (line.words.empty()) {
            fragments.push_back(line);
            continue;
        }
        auto words = line.words;
        std::stable_sort(words.begin(), words.end(),
                         [](const auto &a, const auto &b) { return a.rect.left < b.rect.left; });
        std::vector<RecognizedLine> pieces;
        RecognizedLine piece;
        for (auto word : words) {
            if (word.text.empty())
                continue;
            if (!piece.words.empty() && !Near(piece.words.back().rect, word.rect)) {
                pieces.push_back(std::move(piece));
                piece = {};
            }
            AppendWord(piece, std::move(word));
        }
        if (!piece.words.empty())
            pieces.push_back(std::move(piece));
        if (pieces.empty()) {
            fragments.push_back({line.rect, line.text, {}});
            continue;
        }
        // Preserve the engine's exact punctuation/spaces unless geometry forced
        // us to split a row that spans separate columns/controls.
        if (pieces.size() == 1)
            pieces.front().text = line.text;
        for (auto &part : pieces)
            fragments.push_back(std::move(part));
    }
    std::stable_sort(fragments.begin(), fragments.end(), [](const auto &a, const auto &b) {
        return a.rect.top != b.rect.top ? a.rect.top < b.rect.top : a.rect.left < b.rect.left;
    });
    struct Row {
        RECT anchor;
        std::vector<RecognizedLine> spans;
        bool vertical{};
    };
    std::vector<Row> rows;
    for (auto &fragment : fragments) {
        Row *row = nullptr;
        const bool vertical = Vertical(fragment);
        // Splitting engine rows can create more spans than raw line count.
        // Bound candidate search even for pathological/overlapping geometry.
        size_t examined = 0;
        for (auto it = rows.rbegin(); !vertical && it != rows.rend() && examined < 64; ++it, ++examined) {
            if (it->vertical)
                continue;
            if (fragment.rect.top - it->anchor.bottom > 2 * Height(it->anchor))
                break;
            if (SameRow(it->anchor, fragment.rect)) {
                row = &*it;
                break;
            }
        }
        if (!row) {
            rows.push_back({fragment.rect, {}, vertical});
            row = &rows.back();
        }
        row->anchor = Unite(row->anchor, fragment.rect);
        row->spans.push_back(std::move(fragment));
    }
    TextLayout output;
    std::vector<bool> isolated;
    for (auto &row : rows) {
        std::stable_sort(row.spans.begin(), row.spans.end(),
                         [](const auto &a, const auto &b) { return a.rect.left < b.rect.left; });
        std::vector<RecognizedLine> spans;
        for (auto &span : row.spans) {
            if (!spans.empty() && !row.vertical && Near(spans.back().rect, span.rect)) {
                auto &last = spans.back();
                const bool separator =
                    NeedSpace(last.text, span.text, false, span.rect.left - last.rect.right,
                              (std::min)(Height(last.rect), Height(span.rect)));
                if (separator)
                    last.text += L' ';
                last.text += span.text;
                if (!span.words.empty())
                    span.words.front().joinPrevious = !separator;
                last.words.insert(last.words.end(), span.words.begin(), span.words.end());
                last.rect = Unite(last.rect, span.rect);
            } else
                spans.push_back(std::move(span));
        }
        const bool columns = spans.size() > 1;
        for (auto &span : spans) {
            output.lines.push_back(std::move(span));
            isolated.push_back(columns);
        }
    }
    size_t paragraphStart = 0;
    for (size_t i = 0; i < output.lines.size(); ++i) {
        auto &line = output.lines[i];
        for (auto &word : line.words)
            word.line = i;
        bool continuation = false;
        if (i && !isolated[i] && !isolated[i - 1] && !Bullet(line.text) && !Vertical(line) &&
            !Vertical(output.lines[i - 1])) {
            const auto &previous = output.lines[i - 1];
            const auto &first = output.lines[paragraphStart];
            const double h = (std::min)(Height(previous.rect), Height(line.rect));
            const double gap = line.rect.top - previous.rect.bottom;
            const bool comparable = (std::max)(Height(previous.rect), Height(line.rect)) <= 1.45 * h;
            const double indent = line.rect.left - first.rect.left;
            const bool aligned =
                std::abs(indent) <= .65 * h || (Bullet(first.text) && indent >= 0 && indent <= 2.2 * h);
            const bool heading = Width(previous.rect) < .7 * Width(line.rect) &&
                                 line.rect.right - previous.rect.right > 1.5 * h;
            continuation = comparable && aligned && gap >= -.15 * h && gap <= .85 * h && !heading;
        }
        if (i && !continuation)
            output.text += L"\r\n\r\n";
        if (continuation && NeedSpace(output.lines[i - 1].text, line.text, true, 0, Height(line.rect)))
            output.text += L' ';
        if (!continuation)
            paragraphStart = i;
        output.text += line.text;
    }
    return output;
}

std::vector<RECT> SelectionRects(const std::vector<WordBox> &words, const std::vector<size_t> &indices) {
    std::vector<RECT> rects;
    size_t previous = size_t(-1);
    for (const auto index : indices) {
        if (index >= words.size())
            continue;
        if (!rects.empty() && previous != size_t(-1) && index == previous + 1 &&
            words[index].line == words[previous].line && Near(words[previous].rect, words[index].rect))
            rects.back() = Unite(rects.back(), words[index].rect);
        else
            rects.push_back(words[index].rect);
        previous = index;
    }
    return rects;
}
} // namespace nskry::ocr
