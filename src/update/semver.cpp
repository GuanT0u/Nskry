#include "pch.h"
#include "update/semver.h"

#include <regex>

namespace nskry {
namespace {
struct Parts { int major{}, minor{}, patch{}; std::wstring prerelease; };
bool Parse(const std::wstring& value, Parts& parts) {
    static const std::wregex pattern(LR"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z.-]+)?$)");
    std::wsmatch match;
    if (!std::regex_match(value, match, pattern)) return false;
    try { parts.major = std::stoi(match[1]); parts.minor = std::stoi(match[2]); parts.patch = std::stoi(match[3]); parts.prerelease = match[4]; return true; }
    catch (...) { return false; }
}
int CompareIdentifier(const std::wstring& left, const std::wstring& right) {
    const bool leftNumber = !left.empty() && std::all_of(left.begin(), left.end(), iswdigit);
    const bool rightNumber = !right.empty() && std::all_of(right.begin(), right.end(), iswdigit);
    if (leftNumber && rightNumber) { const int a = std::stoi(left), b = std::stoi(right); return a == b ? 0 : (a < b ? -1 : 1); }
    if (leftNumber != rightNumber) return leftNumber ? -1 : 1;
    return left == right ? 0 : (left < right ? -1 : 1);
}
}

bool SemVer::IsValid(const std::wstring& value) { Parts parts; return Parse(value, parts); }
int SemVer::Compare(const std::wstring& left, const std::wstring& right) {
    Parts a, b; if (!Parse(left, a) || !Parse(right, b)) return 0;
    for (const auto [x, y] : { std::pair{a.major, b.major}, {a.minor, b.minor}, {a.patch, b.patch} }) if (x != y) return x < y ? -1 : 1;
    if (a.prerelease.empty() || b.prerelease.empty()) return a.prerelease.empty() == b.prerelease.empty() ? 0 : (a.prerelease.empty() ? 1 : -1);
    std::wstringstream as(a.prerelease), bs(b.prerelease); std::wstring ai, bi;
    while (true) { const bool am = static_cast<bool>(std::getline(as, ai, L'.')); const bool bm = static_cast<bool>(std::getline(bs, bi, L'.')); if (!am || !bm) return am == bm ? 0 : (am ? 1 : -1); const int c = CompareIdentifier(ai, bi); if (c) return c; }
}
} // namespace nskry
