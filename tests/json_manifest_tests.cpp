#include <windows.h>

#include "core/json.h"
#include "core/plugin_manifest.h"
#include "update/semver.h"

#include <fstream>
#include <iostream>
#include <string>

namespace {

bool Expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAILED: " << message << '\n';
    return condition;
}

bool TestJsonParser() {
    nskry::json::Value root;
    std::wstring error;
    bool ok = Expect(nskry::json::Parse(
        LR"({"escaped":"line\n\u4f60\u597d","items":[true,null,42],"object":{"b":2,"a":1}})",
        root, &error), "valid JSON should parse");
    std::wstring escaped;
    ok &= Expect(root.GetString(L"escaped", escaped) && escaped == L"line\n\u4f60\u597d",
                 "escaped strings should decode");
    ok &= Expect(!nskry::json::Parse(LR"({"a":1,"a":2})", root, &error),
                 "duplicate keys should be rejected");
    ok &= Expect(!nskry::json::Parse(LR"({"a":1} trailing)", root, &error),
                 "trailing input should be rejected");
    ok &= Expect(!nskry::json::Parse(LR"({"a":"\ud800"})", root, &error),
                 "unpaired surrogates should be rejected");
    return ok;
}

bool TestManifestCompatibilityIsSeparateFromParsing() {
    wchar_t temporaryDirectory[MAX_PATH]{};
    wchar_t temporaryFile[MAX_PATH]{};
    if (!::GetTempPathW(MAX_PATH, temporaryDirectory) ||
        !::GetTempFileNameW(temporaryDirectory, L"nsk", 0, temporaryFile)) {
        return Expect(false, "temporary manifest path should be available");
    }

    const char manifest[] = R"({
        "runtime":{"api_version":1,"architecture":"x64","entry":"legacy.dll"},
        "version":"0.4.5",
        "name":"Legacy plugin",
        "id":"legacy-plugin",
        "manifest_version":1,
        "capabilities":["toolbar_action","post_capture"],
        "unknown_future_field":{"kept_compatible":true}
    })";
    {
        std::ofstream output(temporaryFile, std::ios::binary | std::ios::trunc);
        output.write(manifest, sizeof(manifest) - 1);
    }

    nskry::PluginManifest parsed;
    std::wstring error;
    const bool loaded = nskry::PluginManifest::LoadFromFile(temporaryFile, parsed, &error);
    ::DeleteFileW(temporaryFile);
    bool ok = Expect(loaded, "an older structurally valid manifest should remain discoverable");
    ok &= Expect(parsed.id == L"legacy-plugin" && parsed.toolbarAction && parsed.postCapture,
                 "manifest capabilities should be independent of object order");
    ok &= Expect(!parsed.IsCompatibleWithHost(&error),
                 "an older API plugin should be reported as incompatible at execution/install time");
    return ok;
}

bool TestSemVerDoesNotOverflow() {
    bool ok = Expect(nskry::SemVer::IsValid(L"1.2.3-999999999999999999999999999"),
                     "large numeric prerelease identifiers should be valid");
    ok &= Expect(nskry::SemVer::Compare(L"1.2.3-999999999999999999999999999", L"1.2.3-2") > 0,
                 "large numeric prerelease identifiers should compare without integer conversion");
    ok &= Expect(!nskry::SemVer::IsValid(L"1.2.3-01"),
                 "numeric prerelease identifiers with leading zeroes should be rejected");
    return ok;
}

} // namespace

int wmain() {
    const bool ok = TestJsonParser() && TestManifestCompatibilityIsSeparateFromParsing() &&
        TestSemVerDoesNotOverflow();
    if (ok) std::cout << "All core tests passed.\n";
    return ok ? 0 : 1;
}
