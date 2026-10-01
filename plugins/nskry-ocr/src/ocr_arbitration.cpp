#include "ocr_arbitration.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace nskry::ocr {

std::wstring NormalizeForComparison(const std::wstring& text) {
    std::wstring normalized;
    bool pendingSpace = false;
    const auto isHan = [](wchar_t ch) { return ch >= 0x3400 && ch <= 0x9fff; };
    for (wchar_t ch : text) {
        if (iswspace(ch)) { pendingSpace = !normalized.empty(); continue; }
        if (pendingSpace && !(isHan(ch) && !normalized.empty() && isHan(normalized.back())))
            normalized.push_back(L' ');
        pendingSpace = false;
        normalized.push_back(ch);
    }
    return normalized;
}

bool SameSpatialLine(const RECT& a, const RECT& b) {
    const int ah = a.bottom - a.top, bh = b.bottom - b.top;
    if (ah <= 0 || bh <= 0) return false;
    const int yOverlap = (std::min)(a.bottom, b.bottom) - (std::max)(a.top, b.top);
    const int centerDistance = std::abs((a.top + a.bottom) - (b.top + b.bottom));
    if (yOverlap <= 0 || yOverlap * 2 < (std::min)(ah, bh) ||
        centerDistance > (std::max)(ah, bh) * 2) return false;
    const int xGap = (std::max)(0L, (std::max)(a.left, b.left) - (std::min)(a.right, b.right));
    return xGap <= (std::max)(ah, bh) * 2;
}

std::vector<AlignedLineGroup> AlignLines(const std::vector<std::vector<RecognizedLine>>& passes) {
    std::vector<AlignedLineGroup> groups;
    for (size_t pass = 0; pass < passes.size() && pass < 3; ++pass) {
        for (const auto& line : passes[pass]) {
            size_t best = groups.size();
            double bestScore = -1.0;
            for (size_t index = 0; index < groups.size(); ++index) {
                const auto& group = groups[index];
                if (group.candidates[pass]) continue;
                for (const RecognizedLine* existing : group.candidates) {
                    if (!existing || !SameSpatialLine(existing->rect, line.rect)) continue;
                    const int overlapX = (std::max)(0L, (std::min)(existing->rect.right, line.rect.right) -
                        (std::max)(existing->rect.left, line.rect.left));
                    const int overlapY = (std::max)(0L, (std::min)(existing->rect.bottom, line.rect.bottom) -
                        (std::max)(existing->rect.top, line.rect.top));
                    const int centerDistance = std::abs(existing->rect.top + existing->rect.bottom -
                        line.rect.top - line.rect.bottom);
                    const double score = overlapX * 2.0 + overlapY - centerDistance;
                    if (score > bestScore) { bestScore = score; best = index; }
                }
            }
            if (best == groups.size()) groups.push_back({});
            groups[best].candidates[pass] = &line;
        }
    }
    return groups;
}

namespace {

double BaseQuality(const RecognizedLine& line) {
    int visible = 0, useful = 0, suspicious = 0;
    for (wchar_t ch : line.text) {
        if (iswspace(ch)) continue;
        ++visible;
        if ((ch >= 0x3400 && ch <= 0x9fff) || iswalnum(ch)) ++useful;
        if (ch == 0xfffd || ch == 0x25a1 || ch == L'?') ++suspicious;
    }
    if (!visible) return 0.0;
    const double ratio = static_cast<double>(useful) / visible;
    const double width = (std::max)(1L, line.rect.right - line.rect.left);
    const double coverage = (std::min)(1.0, visible / (width / 16.0));
    return 50.0 * ratio + 20.0 * coverage + (std::min)(10.0, visible / 2.0) - 10.0 * suspicious;
}

void RebuildText(RecognizedLine& line) {
    std::stable_sort(line.words.begin(), line.words.end(), [](const WordBox& a, const WordBox& b) {
        return a.rect.left < b.rect.left;
    });
    line.text.clear();
    for (size_t i = 0; i < line.words.size(); ++i) {
        if (i) {
            const auto& before = line.words[i - 1];
            const auto& after = line.words[i];
            const bool hanAdjacent = !before.text.empty() && !after.text.empty() &&
                before.text.back() >= 0x3400 && before.text.back() <= 0x9fff &&
                after.text.front() >= 0x3400 && after.text.front() <= 0x9fff;
            if (!hanAdjacent && after.rect.left - before.rect.right >
                (std::max)(2L, (line.rect.bottom - line.rect.top) / 5)) line.text.push_back(L' ');
        }
        line.text += line.words[i].text;
    }
}

bool ValidSupplement(const WordBox& word) {
    if (word.text.empty() || word.rect.right <= word.rect.left || word.rect.bottom <= word.rect.top) return false;
    for (wchar_t ch : word.text)
        if ((ch >= 0x3400 && ch <= 0x9fff) || iswalnum(ch)) return true;
    return false;
}

int CoveredArea(const WordBox& candidate, const RecognizedLine& primary) {
    int covered = 0;
    for (const auto& existing : primary.words) {
        const int dx = (std::max)(0L, (std::min)(candidate.rect.right, existing.rect.right) -
            (std::max)(candidate.rect.left, existing.rect.left));
        const int dy = (std::max)(0L, (std::min)(candidate.rect.bottom, existing.rect.bottom) -
            (std::max)(candidate.rect.top, existing.rect.top));
        covered += dx * dy;
    }
    return covered;
}

} // namespace

double CandidateScore(const AlignedLineGroup& group, int pass) {
    if (pass < 0 || pass >= 3 || !group.candidates[pass]) return -1e9;
    const auto& line = *group.candidates[pass];
    double score = BaseQuality(line) - GeometryPenalty(line);
    const auto normalized = NormalizeForComparison(line.text);
    for (int other = 0; other < 3; ++other) {
        if (other == pass || !group.candidates[other]) continue;
        if (!normalized.empty() && normalized == NormalizeForComparison(group.candidates[other]->text))
            score += 35.0;
    }
    return score;
}

double GeometryPenalty(const RecognizedLine& line) {
    double penalty = 0.0;
    for (const auto& word : line.words) {
        if (word.text.size() < 2 ||
            !std::all_of(word.text.begin(), word.text.end(), [](wchar_t ch) {
                return ch >= 0x3400 && ch <= 0x9fff;
            })) continue;
        const double height = word.rect.bottom - word.rect.top;
        if (height <= 0.0) continue;
        const double ratio = (word.rect.right - word.rect.left) / (height * word.text.size());
        if (ratio < 0.43 || ratio > 1.9) penalty += 4.0;
    }
    return (std::min)(8.0, penalty); // Never override strong cross-pass agreement.
}

bool HasSuspiciousGeometry(const std::vector<AlignedLineGroup>& groups) {
    for (const auto& group : groups)
        for (const auto* line : group.candidates)
            if (line && GeometryPenalty(*line) > 0.0) return true;
    return false;
}

bool HasLocalDisagreement(const std::vector<AlignedLineGroup>& groups) {
    for (const auto& group : groups) {
        if (!group.candidates[0] || !group.candidates[1]) continue;
        if (NormalizeForComparison(group.candidates[0]->text) !=
            NormalizeForComparison(group.candidates[1]->text)) return true;
    }
    return false;
}

bool HasCoverageDifference(const std::vector<AlignedLineGroup>& groups) {
    for (const auto& group : groups) {
        if (!group.candidates[1]) continue;
        if (!group.candidates[0]) return true;
        for (const auto& word : group.candidates[1]->words) {
            if (!ValidSupplement(word)) continue;
            const int area = (word.rect.right - word.rect.left) * (word.rect.bottom - word.rect.top);
            if (area > 0 && CoveredArea(word, *group.candidates[0]) * 3 < area) return true;
        }
    }
    return false;
}

std::vector<ArbitrationDecision> Arbitrate(const std::vector<AlignedLineGroup>& groups) {
    std::vector<ArbitrationDecision> decisions;
    decisions.reserve(groups.size());
    for (const auto& group : groups) {
        int selected = -1;
        double best = -1e9;
        for (int pass = 0; pass < 3; ++pass) {
            const double score = CandidateScore(group, pass);
            if (score > best + 1e-6) { best = score; selected = pass; }
        }
        if (selected < 0) continue;
        ArbitrationDecision decision;
        decision.line = *group.candidates[selected];
        decision.primaryPass = selected;
        for (int pass = 0; pass < 3; ++pass)
            if (group.candidates[pass] &&
                NormalizeForComparison(group.candidates[pass]->text) ==
                    NormalizeForComparison(decision.line.text)) ++decision.agreementCount;
        decision.conflictReplacement = selected != 0 && group.candidates[0] &&
            NormalizeForComparison(group.candidates[0]->text) != NormalizeForComparison(decision.line.text);
        decision.reason = decision.agreementCount > 1 ? L"cross-pass text agreement" : L"base text quality";
        // Only add OCR-observed words in genuinely uncovered X/Y regions. A
        // different reading of an overlapping region is a conflict, not a suffix.
        for (int pass = 0; pass < 3; ++pass) {
            if (pass == selected || !group.candidates[pass]) continue;
            for (const auto& word : group.candidates[pass]->words) {
                if (!ValidSupplement(word)) continue;
                const int area = (word.rect.right - word.rect.left) * (word.rect.bottom - word.rect.top);
                if (CoveredArea(word, decision.line) * 3 >= area) continue;
                decision.line.words.push_back(word);
                decision.line.rect.left = (std::min)(decision.line.rect.left, word.rect.left);
                decision.line.rect.top = (std::min)(decision.line.rect.top, word.rect.top);
                decision.line.rect.right = (std::max)(decision.line.rect.right, word.rect.right);
                decision.line.rect.bottom = (std::max)(decision.line.rect.bottom, word.rect.bottom);
                ++decision.recoveredWords;
            }
        }
        if (decision.recoveredWords) {
            decision.reason += L"; uncovered OCR word supplement";
        }
        RebuildText(decision.line);
        decisions.push_back(std::move(decision));
    }
    return decisions;
}

} // namespace nskry::ocr
