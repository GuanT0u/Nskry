// Decoder characterization plus conservative spatial-recovery regression tests.
#include "ctc_decode.h"
#include "ocr_layout.h"
#include <iostream>
#include <algorithm>

bool Recovery(const std::wstring &source, const std::vector<int> &gaps, const std::wstring &expected,
              bool uncertain = false, bool uniformTiming = false, bool duplicateInk = false) {
    nskry::ocr::CtcText text;
    text.text = source;
    std::vector<int> starts;
    int width = 2;
    for (size_t i = 0; i < source.size(); ++i) {
        starts.push_back(width);
        width += 8 + (i < gaps.size() ? gaps[i] : 2);
    }
    std::vector<nskry::ocr::CtcInkBounds> ink;
    for (size_t i = 0; i < source.size(); ++i) {
        ink.push_back({float(starts[i]) / width, float(starts[i] + 8) / width});
        const float center = float(starts[i] + 4) / width;
        const float emission = uniformTiming ? float(i + 1) / (source.size() + 2) : center;
        const float left = i ? float(starts[i - 1] + 8 + starts[i]) * .5f / width : 0.f;
        const float right = i + 1 < source.size() ? float(starts[i] + 8 + starts[i + 1]) * .5f / width : 1.f;
        text.glyphs.push_back({i, 1, left, right, emission - .001f, emission + .001f, uncertain ? .5f : 1.f});
    }
    if (duplicateInk)
        ink[2] = ink[1];
    nskry::ocr::RecoverNumericSpacing(text, ink);
    std::wstring covered;
    for (const auto &glyph : text.glyphs)
        covered += text.text.substr(glyph.offset, glyph.length);
    return text.text == expected && covered == expected;
}

int main() {
    const std::vector<std::wstring> vocabulary{L"", L"2", L"5", L"9", L" "};
    auto decode = [&](const std::vector<size_t> &ids) {
        std::vector<float> values(ids.size() * vocabulary.size(), 0.f);
        for (size_t i = 0; i < ids.size(); ++i)
            values[i * vocabulary.size() + ids[i]] = 1.f;
        return nskry::ocr::DecodeCtc(values, ids.size(), vocabulary.size(), vocabulary, 1.f);
    };
    const auto blank = decode({1, 2, 0, 0, 0, 0, 0, 1, 3});
    const auto space = decode({1, 2, 4, 1, 3});
    bool ok = blank.text == L"2529" && space.text == L"25 29";
    // Long blank duration changes centers, but the midpoint cells still touch.
    ok &= blank.glyphs.size() == 4 && blank.glyphs[1].right == blank.glyphs[2].left;
    ok &= blank.glyphs[1].emissionRight < blank.glyphs[2].emissionLeft;
    ok &= Recovery(L"314159", {2, 10, 2, 10, 2}, L"31 41 59");
    ok &= Recovery(L"314159", {2, 10, 2, 10, 2}, L"314159", true);
    ok &= Recovery(L"314159", {2, 10, 2, 10, 2}, L"314159", false, true);
    ok &= Recovery(L"314159", {2, 10, 2, 10, 2}, L"314159", false, false, true);
    for (const auto *value : {L"1234567890", L"001122334455", L"1234.5678", L"2026-10-26", L"192.168.1.1",
                              L"build123456", L"12 34 56"})
        ok &= Recovery(value, std::vector<int>(wcslen(value), 2), value);
    ok &= Recovery(L"123", {2, 20}, L"123");                    // Too few samples to infer a baseline.
    ok &= Recovery(L"123456", {10, 10, 10, 10, 10}, L"123456"); // Uniform tracking is not grouping.
    ok &= Recovery(L"123456", {2, 12, 2, 2, 2}, L"123456");     // One anomalous gap is not sufficient.
    nskry::ocr::RecognizedLine line{{0, 0, 140, 30},
                                    L"2529",
                                    {{{0, 0, 20, 30}, L"2", 0, true},
                                     {{20, 0, 40, 30}, L"5", 0, true},
                                     {{55, 0, 75, 30}, L"2", 0, true},
                                     {{75, 0, 95, 30}, L"9", 0, true}}};
    ok &= nskry::ocr::BuildTextLayout({line}).text == L"2529";
    line.text = L"25 29";
    ok &= nskry::ocr::BuildTextLayout({line}).text == L"25 29";
    std::cout << "Current blank/space distinction, midpoint gap loss, exact-row preservation=" << ok << '\n';
    return ok ? 0 : 1;
}
