#pragma once

#include <string>

namespace nskry {

/// Strict enough for update feeds: major.minor.patch with optional prerelease.
class SemVer {
public:
    static bool IsValid(const std::wstring& value);
    static int Compare(const std::wstring& left, const std::wstring& right);
};

} // namespace nskry
