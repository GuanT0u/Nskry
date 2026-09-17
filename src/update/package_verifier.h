#pragma once

#include <string>
#include <vector>

namespace nskry {

struct VerifiedPackage {
    std::vector<std::wstring> entries;
    bool hasManifest = false;
};

/// Validates the ZIP container before anything is extracted. Extraction is
/// intentionally separate so PackageManager can keep all writes in staging.
class PackageVerifier {
public:
    static bool VerifyNskryPluginArchive(const std::wstring& archivePath,
                                         VerifiedPackage& package,
                                         std::wstring* error = nullptr);
};

} // namespace nskry
