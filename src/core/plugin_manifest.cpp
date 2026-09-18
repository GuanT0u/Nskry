#include "pch.h"
#include "core/plugin_manifest.h"
#include "sdk/nskry_plugin.h"

#include <fstream>
#include <regex>

namespace nskry {
namespace {

bool ReadUtf8File(const std::wstring& path, std::wstring& content) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.empty()) { content.clear(); return true; }
    const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (required <= 0) return false;
    content.resize(required);
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), content.data(), required) > 0;
}

bool FindString(const std::wstring& json, const wchar_t* key, std::wstring& value) {
    const std::wregex pattern(std::wstring(LR"json(")json") + key + LR"json("\s*:\s*"([^"\\]*)")json");
    std::wsmatch match;
    if (!std::regex_search(json, match, pattern)) return false;
    value = match[1].str();
    return true;
}

bool FindUInt(const std::wstring& json, const wchar_t* key, unsigned int& value) {
    const std::wregex pattern(std::wstring(LR"json(")json") + key + LR"json("\s*:\s*(\d+))json");
    std::wsmatch match;
    if (!std::regex_search(json, match, pattern)) return false;
    try { value = static_cast<unsigned int>(std::stoul(match[1].str())); return true; }
    catch (const std::exception&) { return false; }
}

bool FindObject(const std::wstring& json, const wchar_t* key, std::wstring& object) {
    const std::wregex pattern(std::wstring(LR"json(")json") + key + LR"json("\s*:\s*\{([\s\S]*?)\})json");
    std::wsmatch match;
    if (!std::regex_search(json, match, pattern)) return false;
    object = match[1].str();
    return true;
}

int CompareVersion(const std::wstring& left, const std::wstring& right) {
    std::wstringstream leftStream(left), rightStream(right);
    std::wstring leftPart, rightPart;
    while (true) {
        const bool leftMore = static_cast<bool>(std::getline(leftStream, leftPart, L'.'));
        const bool rightMore = static_cast<bool>(std::getline(rightStream, rightPart, L'.'));
        if (!leftMore && !rightMore) return 0;
        int leftNumber = 0, rightNumber = 0;
        try { if (leftMore) leftNumber = std::stoi(leftPart); } catch (const std::exception&) { return 0; }
        try { if (rightMore) rightNumber = std::stoi(rightPart); } catch (const std::exception&) { return 0; }
        if (leftNumber != rightNumber) return leftNumber < rightNumber ? -1 : 1;
    }
}

void SetError(std::wstring* error, const wchar_t* message) {
    if (error) *error = message;
}

} // namespace

bool PluginManifest::LoadFromFile(const std::wstring& path, PluginManifest& manifest, std::wstring* error) {
    std::wstring json;
    if (!ReadUtf8File(path, json)) { SetError(error, L"Unable to read manifest.json as UTF-8."); return false; }

    PluginManifest result;
    unsigned int manifestVersion = 0;
    if (!FindUInt(json, L"manifest_version", manifestVersion) || manifestVersion != 1 ||
        !FindString(json, L"id", result.id) || !FindString(json, L"name", result.name) ||
        !FindString(json, L"version", result.version)) {
        SetError(error, L"Manifest is missing required top-level metadata.");
        return false;
    }
    result.manifestVersion = static_cast<int>(manifestVersion);
    FindString(json, L"author", result.author);
    FindString(json, L"description", result.description);
    FindString(json, L"homepage", result.homepage);
    FindString(json, L"repository", result.repository);

    std::wstring runtime;
    if (!FindObject(json, L"runtime", runtime) || !FindString(runtime, L"entry", result.entry) ||
        !FindString(runtime, L"architecture", result.architecture) || !FindUInt(runtime, L"api_version", result.apiVersion)) {
        SetError(error, L"Manifest runtime metadata is incomplete.");
        return false;
    }
    std::wstring loadPolicy;
    if (FindString(runtime, L"load_policy", loadPolicy) && loadPolicy == L"startup") result.loadPolicy = PluginLoadPolicy::Startup;
    else if (!loadPolicy.empty() && loadPolicy != L"on_demand") { SetError(error, L"Unsupported plugin load policy."); return false; }

    std::wstring compatibility;
    if (FindObject(json, L"compatibility", compatibility)) {
        FindString(compatibility, L"nskry_min", result.nskryMinVersion);
        FindString(compatibility, L"nskry_max", result.nskryMaxVersion);
    }
    std::wstring update;
    if (FindObject(json, L"update", update)) FindString(update, L"url", result.updateUrl);
    result.toolbarAction = std::regex_search(json, std::wregex(LR"json("toolbar_action")json"));

    if (!IsSafePluginId(result.id) || !IsSafeEntryName(result.entry)) {
        SetError(error, L"Plugin id or entry file name is unsafe.");
        return false;
    }
    if (!result.IsCompatibleWithHost(error)) return false;
    manifest = std::move(result);
    return true;
}

bool PluginManifest::IsSafePluginId(const std::wstring& id) {
    if (id.empty() || id.size() > 128) return false;
    for (const wchar_t ch : id) {
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
              (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'-' || ch == L'_')) return false;
    }
    return id != L"." && id != L"..";
}

bool PluginManifest::IsSafeEntryName(const std::wstring& entry) {
    return !entry.empty() && entry.find(L"..") == std::wstring::npos &&
        entry.find_first_of(L"\\/:") == std::wstring::npos;
}

bool PluginManifest::IsCompatibleWithHost(std::wstring* error) const {
    if (architecture != L"x64") { SetError(error, L"Plugin architecture must be x64."); return false; }
    if (apiVersion != NSKRY_PLUGIN_API_VERSION) { SetError(error, L"Plugin API version is not supported by this Nskry build."); return false; }
    if (!nskryMinVersion.empty() && CompareVersion(NSKRY_VERSION, nskryMinVersion) < 0) {
        SetError(error, L"Plugin requires a newer Nskry version."); return false;
    }
    if (!nskryMaxVersion.empty() && CompareVersion(NSKRY_VERSION, nskryMaxVersion) > 0) {
        SetError(error, L"Plugin does not support this Nskry version."); return false;
    }
    return true;
}

} // namespace nskry
