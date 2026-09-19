#include "pch.h"
#include "core/json.h"
#include "core/plugin_manifest.h"
#include "sdk/nskry_plugin.h"
#include "update/semver.h"

namespace nskry {
namespace {

bool OptionalString(const json::Value& object, std::wstring_view key, std::wstring& value) {
    const json::Value* member = object.Find(key);
    if (!member || member->IsNull()) return true;
    return object.GetString(key, value);
}

void SetError(std::wstring* error, const wchar_t* message) {
    if (error) *error = message;
}

} // namespace

bool PluginManifest::LoadFromFile(const std::wstring& path, PluginManifest& manifest, std::wstring* error) {
    json::Value root;
    std::wstring parseError;
    if (!json::ParseUtf8File(path, root, &parseError) || !root.IsObject()) {
        if (error) *error = parseError.empty() ? L"Manifest root must be a JSON object."
                                              : L"Invalid manifest.json: " + parseError;
        return false;
    }

    PluginManifest result;
    unsigned int manifestVersion = 0;
    if (!root.GetUnsigned(L"manifest_version", manifestVersion) || manifestVersion != 1 ||
        !root.GetString(L"id", result.id) || !root.GetString(L"name", result.name) ||
        !root.GetString(L"version", result.version)) {
        SetError(error, L"Manifest is missing required top-level metadata.");
        return false;
    }
    result.manifestVersion = static_cast<int>(manifestVersion);
    if (!OptionalString(root, L"author", result.author) ||
        !OptionalString(root, L"description", result.description) ||
        !OptionalString(root, L"homepage", result.homepage) ||
        !OptionalString(root, L"repository", result.repository)) {
        SetError(error, L"Manifest contains invalid optional metadata.");
        return false;
    }

    const json::Value* runtime = root.Find(L"runtime");
    if (!runtime || !runtime->IsObject() || !runtime->GetString(L"entry", result.entry) ||
        !runtime->GetString(L"architecture", result.architecture) ||
        !runtime->GetUnsigned(L"api_version", result.apiVersion)) {
        SetError(error, L"Manifest runtime metadata is incomplete.");
        return false;
    }
    std::wstring loadPolicy;
    if (!OptionalString(*runtime, L"load_policy", loadPolicy)) {
        SetError(error, L"Manifest load policy must be a string.");
        return false;
    }
    if (loadPolicy == L"startup") result.loadPolicy = PluginLoadPolicy::Startup;
    else if (!loadPolicy.empty() && loadPolicy != L"on_demand") {
        SetError(error, L"Unsupported plugin load policy.");
        return false;
    }

    if (const json::Value* compatibility = root.Find(L"compatibility")) {
        if (!compatibility->IsObject() ||
            !OptionalString(*compatibility, L"nskry_min", result.nskryMinVersion) ||
            !OptionalString(*compatibility, L"nskry_max", result.nskryMaxVersion)) {
            SetError(error, L"Manifest compatibility metadata is invalid.");
            return false;
        }
    }
    if (const json::Value* source = root.Find(L"source")) {
        if (!source->IsObject() || !OptionalString(*source, L"homepage", result.homepage) ||
            !OptionalString(*source, L"repository", result.repository)) {
            SetError(error, L"Manifest source metadata is invalid.");
            return false;
        }
    }
    if (const json::Value* update = root.Find(L"update")) {
        if (!update->IsObject() || !OptionalString(*update, L"url", result.updateUrl)) {
            SetError(error, L"Manifest update metadata is invalid.");
            return false;
        }
    }
    if (const json::Value* capabilities = root.Find(L"capabilities")) {
        const json::Value::Array* array = capabilities->AsArray();
        if (!array) {
            SetError(error, L"Manifest capabilities must be an array.");
            return false;
        }
        for (const json::Value& capability : *array) {
            const std::wstring* name = capability.AsString();
            if (!name) {
                SetError(error, L"Manifest capabilities must contain only strings.");
                return false;
            }
            if (*name == L"toolbar_action") result.toolbarAction = true;
        }
    }

    if (!IsSafePluginId(result.id) || !IsSafeEntryName(result.entry)) {
        SetError(error, L"Plugin id or entry file name is unsafe.");
        return false;
    }
    if (!SemVer::IsValid(result.version) ||
        (!result.nskryMinVersion.empty() && !SemVer::IsValid(result.nskryMinVersion)) ||
        (!result.nskryMaxVersion.empty() && !SemVer::IsValid(result.nskryMaxVersion))) {
        SetError(error, L"Plugin version or compatibility range is not valid SemVer.");
        return false;
    }
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
    if (!nskryMinVersion.empty() && SemVer::Compare(NSKRY_VERSION, nskryMinVersion) < 0) {
        SetError(error, L"Plugin requires a newer Nskry version."); return false;
    }
    if (!nskryMaxVersion.empty() && SemVer::Compare(NSKRY_VERSION, nskryMaxVersion) > 0) {
        SetError(error, L"Plugin does not support this Nskry version."); return false;
    }
    return true;
}

} // namespace nskry
