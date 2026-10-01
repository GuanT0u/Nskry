#pragma once

#include <windows.h>

#include <array>
#include <string>
#include <vector>

namespace nskry::ocr {

struct WordBox {
    RECT rect{};
    std::wstring text;
    size_t line{};
};

struct RecognizedLine {
    RECT rect{};
    std::wstring text;
    std::vector<WordBox> words;
};

struct AlignedLineGroup {
    std::array<const RecognizedLine*, 3> candidates{};
};

struct ArbitrationDecision {
    RecognizedLine line;
    int primaryPass{};
    int agreementCount{};
    int recoveredWords{};
    bool conflictReplacement{};
    std::wstring reason;
};

std::wstring NormalizeForComparison(const std::wstring& text);
bool SameSpatialLine(const RECT& a, const RECT& b);
std::vector<AlignedLineGroup> AlignLines(const std::vector<std::vector<RecognizedLine>>& passes);
double GeometryPenalty(const RecognizedLine& line);
double CandidateScore(const AlignedLineGroup& group, int pass);
bool HasLocalDisagreement(const std::vector<AlignedLineGroup>& groups);
bool HasCoverageDifference(const std::vector<AlignedLineGroup>& groups);
bool HasSuspiciousGeometry(const std::vector<AlignedLineGroup>& groups);
std::vector<ArbitrationDecision> Arbitrate(const std::vector<AlignedLineGroup>& groups);

} // namespace nskry::ocr
