#include "ocr_layout.h"
#include <iostream>
using namespace nskry::ocr;
RecognizedLine Line(const wchar_t *text, LONG x, LONG y, LONG right, LONG bottom) {
    RECT rect{x, y, right, bottom};
    return {rect, text, {{rect, text, 0, false}}};
}
bool Check(bool value, const char *message) {
    if (!value)
        std::cerr << "Failed: " << message << '\n';
    return value;
}
int main() {
    bool ok = true;
    auto punctuation = BuildTextLayout(
        {Line(L"world", 70, 10, 120, 30), Line(L",", 61, 26, 64, 30), Line(L"Hello", 10, 10, 60, 30)});
    ok &= Check(punctuation.lines.size() == 1 && punctuation.text == L"Hello, world",
                "nearby fragments and small punctuation share one visual row");
    auto chinese = BuildTextLayout(
        {Line(L"你好", 10, 10, 50, 30), Line(L"，", 51, 26, 54, 30), Line(L"世界", 60, 10, 100, 30)});
    ok &= Check(chinese.lines.size() == 1 && chinese.text == L"你好，世界",
                "Chinese punctuation does not create a row or extra spaces");
    auto tinyPeriod = BuildTextLayout({Line(L"Hello",10,10,60,30),Line(L".",64,28,66,30)});
    ok &= Check(tinyPeriod.lines.size()==1 && tinyPeriod.text==L"Hello.",
        "a two-pixel full stop at the baseline is still part of the same visual line");
    auto columns = BuildTextLayout({Line(L"left", 10, 10, 80, 30), Line(L"right", 300, 10, 380, 30)});
    ok &= Check(columns.lines.size() == 2 && columns.text == L"left\r\n\r\nright",
                "far same-baseline columns are not continuous text");
    auto wide = Line(L"left right", 10, 10, 380, 30);
    wide.words = {{{10, 10, 80, 30}, L"left", 0}, {{300, 10, 380, 30}, L"right", 0}};
    auto split = BuildTextLayout({wide});
    ok &= Check(split.lines.size() == 2 && split.text == columns.text,
                "engine row spanning far controls is split");
    auto paragraphs =
        BuildTextLayout({Line(L"这是第一行，", 10, 10, 190, 30), Line(L"下一行继续。", 10, 37, 185, 57),
                         Line(L"另一个段落。", 10, 90, 180, 110)});
    ok &= Check(paragraphs.lines.size() == 3 &&
                    paragraphs.text == L"这是第一行，下一行继续。\r\n\r\n另一个段落。",
                "close lines join a paragraph while large vertical gap separates paragraphs");
    ok &= Check(paragraphs.lines[0].rect.bottom == 30 && paragraphs.lines[1].rect.top == 37,
                "paragraph continuity never stretches a highlight into another row");
    auto english = BuildTextLayout(
        {Line(L"One sentence.", 10, 10, 200, 30), Line(L"Another sentence.", 10, 36, 200, 56)});
    ok &= Check(english.text == L"One sentence. Another sentence.",
                "sentence punctuation alone does not split a paragraph");
    auto bullets = BuildTextLayout({Line(L"• 第一项", 10, 10, 200, 30), Line(L"续行", 30, 36, 100, 56),
                                    Line(L"• 第二项", 10, 62, 190, 82)});
    ok &= Check(bullets.text == L"• 第一项续行\r\n\r\n• 第二项",
                "bullet indentation continues but a new bullet starts its own paragraph");
    auto heading = BuildTextLayout(
        {Line(L"Heading", 10, 10, 80, 30), Line(L"Body paragraph follows.", 10, 36, 300, 56)});
    ok &= Check(heading.text == L"Heading\r\n\r\nBody paragraph follows.",
                "short heading does not merge into a long following body");
    auto separate =
        BuildTextLayout({Line(L"left block", 10, 10, 200, 30), Line(L"independent", 300, 36, 490, 56)});
    ok &= Check(separate.text.find(L"\r\n\r\n") != std::wstring::npos,
                "large horizontal shift is not a wrapped paragraph");
    const std::vector<WordBox> glyphs{{{0, 0, 10, 20}, L"你", 0, true},
                                      {{10, 0, 20, 20}, L"好", 0, true},
                                      {{0, 30, 10, 50}, L"啊", 1, true}};
    auto selection = SelectionRects(glyphs, {0, 1, 2, 999});
    ok &= Check(selection.size() == 2 && selection[0].right == 20 && selection[0].bottom == 20 &&
                    selection[1].top == 30,
                "selected character boxes merge only inside their own row");
    auto partial = SelectionRects(glyphs, {1});
    ok &= Check(partial.size() == 1 && partial[0].left == 10, "partial selection keeps character precision");
    ok &= Check(BuildTextLayout({}).text.empty(), "empty recognition stays empty");
    auto vertical = Line(L"Nskry", 10, 10, 25, 110);
    vertical.words = {{{10, 10, 25, 30}, L"N", 0, true},
                      {{10, 30, 25, 50}, L"s", 0, true},
                      {{10, 50, 25, 70}, L"k", 0, true},
                      {{10, 70, 25, 90}, L"r", 0, true},
                      {{10, 90, 25, 110}, L"y", 0, true}};
    auto rotated = BuildTextLayout({vertical});
    ok &= Check(
        rotated.lines.size() == 1 && rotated.text == L"Nskry" && rotated.lines[0].words.size() == 5,
        "rotated vertical glyphs retain engine text/order instead of being fragmented by horizontal rules");
    std::vector<RecognizedLine> crowded(2000, vertical);
    auto crowdedLayout = BuildTextLayout(crowded);
    ok &= Check(crowdedLayout.lines.size() == crowded.size(),
                "many vertical spans remain separate with bounded candidate search");
    std::cout << "Presentation rows/paragraphs, punctuation, spacing, columns and selection=" << ok << '\n';
    return ok ? 0 : 1;
}
