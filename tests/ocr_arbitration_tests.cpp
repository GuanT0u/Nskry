#include "ocr_arbitration.h"

#include <algorithm>
#include <iostream>

using namespace nskry::ocr;

namespace {

RecognizedLine Line(std::initializer_list<WordBox> words) {
    RecognizedLine line;
    line.words = words;
    bool first = true;
    for (const auto& word : line.words) {
        if (!line.text.empty()) line.text += L' ';
        line.text += word.text;
        if (first) { line.rect = word.rect; first = false; }
        else {
            line.rect.left = (std::min)(line.rect.left, word.rect.left);
            line.rect.top = (std::min)(line.rect.top, word.rect.top);
            line.rect.right = (std::max)(line.rect.right, word.rect.right);
            line.rect.bottom = (std::max)(line.rect.bottom, word.rect.bottom);
        }
    }
    return line;
}

WordBox Word(int left, int right, const wchar_t* text) {
    return { { left, 10, right, 30 }, text, 0 };
}

bool Check(bool condition, const wchar_t* label) {
    if (!condition) std::wcerr << L"FAILED: " << label << L'\n';
    return condition;
}

} // namespace

int wmain() {
    bool okay = true;
    okay &= Check(NormalizeForComparison(L"  此电脑 \t  build  ") == L"此电脑 build", L"normalization");
    okay &= Check(NormalizeForComparison(L"此 电 脑") == L"此电脑", L"CJK segmentation normalization");
    okay &= Check(SameSpatialLine({ 100, 100, 151, 120 }, { 98, 99, 153, 121 }), L"line alignment tolerance");
    okay &= Check(!SameSpatialLine({ 100, 100, 151, 120 }, { 400, 100, 450, 120 }), L"separate columns");

    {
        std::vector<std::vector<RecognizedLine>> passes{
            { Line({ Word(10, 70, L"比申，月囟") }) },
            { Line({ Word(10, 70, L"此电脑") }) },
            { Line({ Word(10, 70, L"此电脑") }) }
        };
        const auto groups = AlignLines(passes);
        const auto decisions = Arbitrate(groups);
        okay &= Check(groups.size() == 1 && decisions.size() == 1 &&
            decisions[0].line.text == L"此电脑" && decisions[0].conflictReplacement,
            L"two-vote conflict replacement");
        okay &= Check(decisions[0].line.words.size() == 1, L"conflict cannot duplicate");
        okay &= Check(HasLocalDisagreement(groups), L"local conflict detection");
    }

    {
        std::vector<std::vector<RecognizedLine>> passes{
            { Line({ Word(0, 40, L"名称"), Word(60, 130, L"修改日期"), Word(210, 250, L"大小") }) },
            { Line({ Word(0, 40, L"名称"), Word(60, 130, L"修改日期"),
                     Word(150, 190, L"类别"), Word(210, 250, L"大小") }) }
        };
        const auto groups = AlignLines(passes);
        const auto decisions = Arbitrate(groups);
        okay &= Check(HasCoverageDifference(groups), L"two-Han coverage difference");
        okay &= Check(decisions.size() == 1 && decisions[0].line.text.find(L"类别") != std::wstring::npos,
            L"missing region supplement");
    }

    {
        std::vector<std::vector<RecognizedLine>> passes{
            { Line({ Word(0, 40, L"name"), Word(110, 150, L"size") }) },
            { Line({ Word(0, 40, L"name"), Word(55, 95, L"build"), Word(110, 150, L"size") }) }
        };
        const auto decisions = Arbitrate(AlignLines(passes));
        okay &= Check(decisions.size() == 1 && decisions[0].line.text.find(L"build") != std::wstring::npos,
            L"English UI token recovery");
    }

    {
        const auto plausible = Line({ Word(0, 40, L"类别") });
        const auto squeezed = Line({ Word(0, 10, L"类别") });
        okay &= Check(GeometryPenalty(squeezed) > GeometryPenalty(plausible), L"weak CJK geometry");
    }
    return okay ? 0 : 1;
}
