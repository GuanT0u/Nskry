#include <windows.h>

#include "core/json.h"
#include "core/plugin_manifest.h"
#include "core/plugin_update_policy.h"
#include "update/semver.h"

#include <fstream>
#include <iostream>
#include <limits>
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
    nskry::json::Value existing, changes, roundTrip;
    ok &= Expect(nskry::json::Parse(LR"({"general":{"notifications":false,"future":42},"future":[true,null,"中文"]})",existing) &&
                 nskry::json::Parse(LR"({"general":{"notifications":true}})",changes), "merge inputs parse");
    std::wstring serialized;
    const auto merged=nskry::json::MergeObjects(existing,changes);
    ok &= Expect(nskry::json::Serialize(merged,serialized) && nskry::json::Parse(serialized,roundTrip),"merged JSON round trip");
    bool notifications=false;unsigned future=0;
    const auto* general=roundTrip.Find(L"general");
    const auto* extension=roundTrip.Find(L"future");
    ok &= Expect(general && general->GetBool(L"notifications",notifications) && notifications &&
                 general->GetUnsigned(L"future",future) && future==42 && extension && extension->AsArray() &&
                 extension->AsArray()->size()==3,"overlay preserves unknown nested fields and arrays");
    ok &= Expect(!nskry::json::Serialize(nskry::json::Value(std::numeric_limits<double>::infinity()),serialized),
                 "non-finite JSON output rejected");
    nskry::json::Value deep;
    for(int i=0;i<130;++i)deep=nskry::json::Value(nskry::json::Value::Array{std::move(deep)});
    ok &= Expect(!nskry::json::Serialize(deep,serialized) && serialized.empty(),"serializer bounds nesting");
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

bool TestManifestDependencyContract() {
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"nsk", 0, path)) return false;
    const auto load = [&](const std::string& extra) {
        const std::string text = R"({"manifest_version":1,"id":"org.test","name":"Test","version":"1.0.0","runtime":{"entry":"test.dll","architecture":"x64","api_version":2})" + extra + "}";
        { std::ofstream file(path, std::ios::binary | std::ios::trunc); file << text; }
        nskry::PluginManifest manifest;
        return nskry::PluginManifest::LoadFromFile(path, manifest);
    };
    bool ok = Expect(load(""), "minimal API 2 manifest accepted");
    ok &= Expect(load(",\"requires_plugins\":[]"), "empty prerequisites accepted");
    ok &= Expect(load(R"(,"extensions":{"commands":1,"region_capture":1},"shortcuts":[{"id":"stop","label":"Stop","scope":"command","default":"Ctrl+Alt+R"}])"),"optional versioned command/capture metadata accepted");
    ok &= Expect(!load(R"(,"extensions":{"commands":2})"),"unknown command version rejected before DLL load");
    ok &= Expect(load(R"(,"extensions":{"text_translation":1,"text_translation_client":1})"),
        "optional host-mediated text handoff metadata accepted");
    ok &= Expect(!load(R"(,"extensions":{"text_translation":2})"), "unknown text provider version rejected");
    ok &= Expect(!load(R"(,"extensions":{"text_translation_client":true})"), "invalid text client version rejected");
    ok &= Expect(load(R"(,"extensions":{"region_capture":2,"capture_actions":1,"settings_ui":1},"capture_actions":[{"id":"gif","label":"Record GIF"}])"),
                 "capture v2 and extra metadata actions accepted without loading DLL");
    ok &= Expect(!load(R"(,"extensions":{"region_capture":3})"), "unknown capture version rejected");
    ok &= Expect(!load(R"(,"capture_actions":[{"id":"gif","label":"GIF"}])"), "actions require explicit version");
    ok &= Expect(!load(R"(,"extensions":{"capture_actions":1},"capture_actions":[{"id":"gif","label":"GIF"},{"id":"gif","label":"Again"}])"),
                 "duplicate extra actions rejected");
    ok &= Expect(!load(R"(,"shortcuts":[{"id":"stop","label":"Stop","scope":"command"}])"),"command scope needs explicit extension negotiation");
    ok &= Expect(!load(",\"requires_plugins\":[\"nskry-scroll\"]"), "unsupported cross-plugin dependency rejected");
    ok &= Expect(!load(",\"requires_plugins\":true"), "malformed dependency field rejected");
    ok &= Expect(load(R"(,"shortcuts":[{"id":"copy","label":"Copy text","scope":"key","default":"Ctrl+C","general_jump":true}])"),"plugin shortcut metadata accepted without loading DLL");
    ok &= Expect(!load(R"(,"shortcuts":[{"id":"copy","label":"Copy","scope":"key","default":"A+B"}])"),"invalid default chord rejected");
    ok &= Expect(!load(R"(,"shortcuts":[{"id":"core.capture","label":"bad","scope":"global","default":"Ctrl+A"}])"),"plugin cannot declare arbitrary core/global command");
    ok &= Expect(!load(R"(,"shortcuts":[{"id":"copy","label":"Copy","scope":"key"},{"id":"copy","label":"Copy 2","scope":"key"}])"),"duplicate shortcut descriptors rejected");
    ok &= Expect(load(",\"update\":{\"type\":\"manifest\",\"url\":\"https://publisher.test/update.json\"}"), "manifest feed supported");
    ok &= Expect(!load(",\"update\":{\"type\":\"future-provider\"}"), "unsupported provider does not silently pretend to work");
    DeleteFileW(path);
    return ok;
}

bool TestUpdatePolicy() {
    using namespace nskry;
    PluginRecord official, third, other;
    official.manifest.id = L"nskry-ocr"; official.manifest.version = L"1.0.0"; official.source = PluginSource::Official;
    third.manifest.id = L"org.test.plugin"; third.manifest.version = L"1.0.0"; third.source = PluginSource::ThirdParty;
    other.manifest.id = L"org.test.other"; other.manifest.version = L"1.0.0"; other.source = PluginSource::Local;
    const std::vector<const PluginRecord*> installed{&official, &third, &other};
    const auto entry = [](const wchar_t* id, const wchar_t* version = L"1.1.0") {
        return L"{\"id\":\"" + std::wstring(id) + L"\",\"latest_version\":\"" + version +
            L"\",\"package_url\":\"https://publisher.test/p.nskryplugin\",\"sha256\":\"" + std::wstring(64, L'a') + L"\"}";
    };
    const std::wstring catalog = L"{\"plugins\":[" + entry(L"nskry-ocr") + L"," + entry(L"org.test.plugin") + L"," + entry(L"org.test.other") + L"]}";
    std::vector<PluginUpdate> updates;
    std::wstring error;
    bool ok = Expect(ParsePluginUpdateFeed(catalog, installed, PluginSource::Official, L"", updates, &error) &&
        updates.size() == 1 && updates[0].pluginId == L"nskry-ocr", "official catalog cannot promote/update third-party IDs");
    updates.clear();
    ok &= Expect(ParsePluginUpdateFeed(catalog, installed, PluginSource::ThirdParty, L"org.test.plugin", updates, &error) &&
        updates.size() == 1 && updates[0].pluginId == L"org.test.plugin", "publisher feed must be restricted to its own ID");
    updates.clear();
    ok &= Expect(ParsePluginUpdateFeed(entry(L"nskry-ocr"), installed, PluginSource::ThirdParty, L"nskry-ocr", updates, &error) &&
        updates.empty(), "publisher feed cannot update official records");
    ok &= Expect(!ParsePluginUpdateFeed(L"{\"plugins\":[" + entry(L"org.test.plugin") + L"," + entry(L"org.test.plugin") + L"]}",
        installed, PluginSource::ThirdParty, L"org.test.plugin", updates, &error) && updates.empty(), "ambiguous duplicate updates rejected atomically");
    ok &= Expect(ParsePluginUpdateFeed(entry(L"org.test.plugin", L"0.9.0"), installed, PluginSource::ThirdParty, L"org.test.plugin", updates, &error) &&
        updates.empty(), "downgrades do not become updates");
    auto invalid = entry(L"org.test.plugin"); invalid.replace(invalid.find(L"https://"), 8, L"http://");
    ok &= Expect(!ParsePluginUpdateFeed(invalid, installed, PluginSource::ThirdParty, L"org.test.plugin", updates, &error), "HTTP packages rejected before download");
    invalid = entry(L"org.test.plugin"); invalid.replace(invalid.find(std::wstring(64, L'a')), 64, L"bad");
    ok &= Expect(!ParsePluginUpdateFeed(invalid, installed, PluginSource::ThirdParty, L"org.test.plugin", updates, &error), "invalid SHA rejected before download");
    ok &= Expect(IsPluginHttpsUrl(L"https://publisher.test/plugins") && !IsPluginHttpsUrl(L"file:///C:/x") &&
        !IsPluginHttpsUrl(L"https://user:secret@publisher.test/plugins") && !IsPluginHttpsUrl(L"https://"), "browser/feed URL policy excludes non-HTTPS and credentials");
    PluginUpdate selected; selected.pluginId = L"org.test.plugin"; selected.version = L"1.1.0";
    PluginManifest package;
    package.id = selected.pluginId; package.version = selected.version; package.architecture = L"x64"; package.apiVersion = 2;
    ok &= Expect(MatchesPluginUpdate(package, selected, &error), "matching package accepted");
    package.id = L"nskry-ocr";
    ok &= Expect(!MatchesPluginUpdate(package, selected, &error), "different downloaded package ID rejected");
    package.id = selected.pluginId; package.version = L"1.2.0";
    ok &= Expect(!MatchesPluginUpdate(package, selected, &error), "different downloaded package version rejected");
    package.version = selected.version; package.apiVersion = 999;
    ok &= Expect(!MatchesPluginUpdate(package, selected, &error), "incompatible downloaded package rejected");
    return ok;
}

int wmain() {
    const bool ok = TestJsonParser() && TestManifestCompatibilityIsSeparateFromParsing() &&
        TestSemVerDoesNotOverflow() && TestManifestDependencyContract() && TestUpdatePolicy();
    if (ok) std::cout << "All core tests passed.\n";
    return ok ? 0 : 1;
}
