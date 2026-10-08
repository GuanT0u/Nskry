#include "ocr_wire.h"
#include "ctc_decode.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace nskry::ocr;
bool Check(bool value, const char* name) { if (!value) std::cerr << "FAIL: " << name << '\n'; return value; }
int main() {
    bool okay = true;
    wire::Input input; input.width = 64; input.height = 32; input.pixelBytes = 64 * 32 * 4;
    okay &= Check(wire::ValidInput(input), "valid input");
    input.width = 0; okay &= Check(!wire::ValidInput(input), "zero width");
    input.width = UINT32_MAX; okay &= Check(!wire::ValidInput(input), "overflow/large input");
    input.width = 64; input.pixelBytes--; okay &= Check(!wire::ValidInput(input), "truncated pixels");
    wire::Reply value;
    value.lines.push_back({{1, 2, 60, 20}, .97f, L"文件夹 build\U0001f600",
        {{{1, 2, 10, 20}, 0, 1}, {{10, 2, 20, 20}, 1, 1}, {{20, 2, 30, 20}, 2, 1},
         {{30, 2, 50, 20}, 4, 5}, {{50, 2, 60, 20}, 9, 2}}});
    // UTF-16: three Han, one space, five letters, then one supplementary glyph.
    auto bytes = wire::Encode(value); wire::Reply decoded;
    okay &= Check(wire::Decode(bytes, 64, 32, decoded) && decoded.lines[0].text == value.lines[0].text,
                  "Unicode round trip with glyphs");
    auto bad = bytes; bad.pop_back(); okay &= Check(!wire::Decode(bad, 64, 32, decoded), "truncated response");
    bad = bytes; bad.push_back(0); okay &= Check(!wire::Decode(bad, 64, 32, decoded), "trailing data");
    bad = bytes; bad[0] = 0; okay &= Check(!wire::Decode(bad, 64, 32, decoded), "bad magic");
    value.lines[0].confidence = std::numeric_limits<float>::quiet_NaN();
    okay &= Check(!wire::Decode(wire::Encode(value), 64, 32, decoded), "NaN confidence");
    value.lines[0].confidence = .97f; value.lines[0].rect.right = 100;
    okay &= Check(!wire::Decode(wire::Encode(value), 64, 32, decoded), "coordinates outside original");
    value.lines[0].rect.right = 60; value.lines[0].glyphs.back().length = 1;
    okay &= Check(!wire::Decode(wire::Encode(value), 64, 32, decoded), "split UTF16 surrogate glyph");
    value = {}; value.error = L"模型缺失";
    okay &= Check(wire::Decode(wire::Encode(value), 64, 32, decoded) && decoded.error == value.error, "error reply");
    value = {}; okay &= Check(wire::Decode(wire::Encode(value), 64, 32, decoded) && decoded.lines.empty(), "empty success");

    const std::vector<std::wstring> vocabulary{L"", L"文", L"件", L"\U0001f600"};
    const auto tensor = [&](std::initializer_list<int> ids) {
        std::vector<float> data(ids.size() * vocabulary.size(), .01f); size_t row = 0;
        for (int id : ids) data[row++ * vocabulary.size() + id] = .97f;
        return data;
    };
    const auto data = tensor({0, 1, 1, 0, 1, 2, 0});
    const auto text = DecodeCtc(data, 7, 4, vocabulary, 1);
    okay &= Check(text.text == L"文文件" && text.glyphs.size() == 3, "blank resets duplicate Han tokens");
    okay &= Check(text.glyphs[0].right <= text.glyphs[1].left && text.glyphs.back().right <= 1,
                  "ordered glyph geometry");
    const auto surrogate = DecodeCtc(tensor({0, 3, 3, 0}), 4, 4, vocabulary, 1);
    okay &= Check(surrogate.text == L"\U0001f600" && surrogate.glyphs[0].length == 2, "supplementary character class");
    okay &= Check(DecodeCtc(tensor({0, 0}), 2, 4, vocabulary, 1).text.empty(), "all blank");
    const auto padded = DecodeCtc(data, 7, 4, vocabulary, 4);
    okay &= Check(padded.glyphs.back().right <= 1 && padded.text == text.text, "padding does not correct or drop text");
    bool threw = false;
    try { DecodeCtc(data, 8, 4, vocabulary, 1); } catch (const std::invalid_argument&) { threw = true; }
    okay &= Check(threw, "reject tensor shape mismatch");
    return okay ? 0 : 1;
}
