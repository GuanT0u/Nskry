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
    if (const auto *extensions = root.Find(L"extensions")) {
        if (!extensions->IsObject() ||
            (extensions->Find(L"image_recognition") &&
             (!extensions->GetUnsigned(L"image_recognition", result.imageRecognitionApi) || result.imageRecognitionApi != 1)) ||
            (extensions->Find(L"image_recognition_client") &&
             (!extensions->GetUnsigned(L"image_recognition_client", result.imageRecognitionClientApi) || result.imageRecognitionClientApi != 1)) ||
            (extensions->Find(L"text_translation") &&
             (!extensions->GetUnsigned(L"text_translation", result.textTranslationApi) || result.textTranslationApi != 1)) ||
            (extensions->Find(L"text_translation_client") &&
             (!extensions->GetUnsigned(L"text_translation_client", result.textTranslationClientApi) || result.textTranslationClientApi != 1)) ||
            (extensions->Find(L"settings_ui") &&
             (!extensions->GetUnsigned(L"settings_ui", result.settingsUiApi) || result.settingsUiApi != 1)) ||
            (extensions->Find(L"capture_actions") &&
             (!extensions->GetUnsigned(L"capture_actions", result.actionApi) || result.actionApi != 1)) ||
            (extensions->Find(L"commands") &&
             (!extensions->GetUnsigned(L"commands", result.commandApi) || result.commandApi != 1)) ||
            (extensions->Find(L"region_capture") &&
             (!extensions->GetUnsigned(L"region_capture", result.captureApi) ||
              (result.captureApi != 1 && result.captureApi != 2)))) {
            SetError(error, L"Unsupported optional extension version.");
            return false;
        }
    }
    if (const auto *actions = root.Find(L"capture_actions")) {
        const auto *array = actions->AsArray();
        if (!result.actionApi || !array || array->size() > 8) {
            SetError(error, L"Invalid capture actions extension.");
            return false;
        }
        for (const auto &item : *array) {
            PluginManifest::CaptureAction action;
            if (!item.GetString(L"id", action.id) || !IsSafePluginId(action.id) ||
                !item.GetString(L"label", action.label) || action.label.empty() || action.label.size() > 40 ||
                std::any_of(result.captureActions.begin(), result.captureActions.end(),
                            [&](const auto &a) { return a.id == action.id; })) {
                SetError(error, L"Invalid or duplicate capture action.");
                return false;
            }
            result.captureActions.push_back(std::move(action));
        }
    }
    // API 2 has no cross-plugin service/dependency resolver. Do not silently
    // accept a declared prerequisite that the host cannot satisfy.
    if (const auto* required = root.Find(L"requires_plugins")) {
        const auto* array = required->AsArray();
        if (!array || !array->empty()) {
            SetError(error, L"Cross-plugin prerequisites are not supported by API 2. Use core host services or private packaged libraries.");
            return false;
        }
    }
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
        std::wstring type;
        if (!update->IsObject() || !OptionalString(*update, L"url", result.updateUrl) ||
            !OptionalString(*update, L"type", type) || (!type.empty() && type != L"manifest")) {
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
            if (*name == L"post_capture") result.postCapture = true;
        }
    }

    if(const auto* shortcuts=root.Find(L"shortcuts")) {
        const auto* array=shortcuts->AsArray();
        if(!array || array->size()>32) {SetError(error,L"At most 32 shortcut descriptors are allowed.");return false;}
        std::vector<std::wstring> seen;
        for(const auto& value:*array) {
            ShortcutDefinition row;std::wstring local,scope;
            if(!value.IsObject() || !value.GetString(L"id",local) || !IsSafePluginId(local) ||
               !value.GetString(L"label",row.label) || row.label.empty() || row.label.size()>160 ||
               !value.GetString(L"scope",scope) || !OptionalString(value,L"default",row.defaultShortcut) ||
               std::find(seen.begin(),seen.end(),local)!=seen.end()) {SetError(error,L"Invalid or duplicate shortcut descriptor.");return false;}
            seen.push_back(local);
            if(scope==L"capture" && local==L"launch" && result.toolbarAction) row.kind=input::Kind::Global;
            else if(scope==L"command" && local!=L"launch" && result.commandApi==1) {
                row.kind=input::Kind::Global;
                row.pluginCommand=true;
            }
            else if(scope==L"key")row.kind=input::Kind::Key;
            else if(scope==L"hold")row.kind=input::Kind::Hold;
            else if(scope==L"wheel")row.kind=input::Kind::Wheel;
            else {SetError(error,L"Unsupported shortcut scope; capture is only for launch toolbar actions.");return false;}
            if(const auto* jump=value.Find(L"general_jump")) {
                if(!jump->AsBool()) {SetError(error,L"general_jump must be boolean.");return false;}
                row.generalJump=*jump->AsBool();
            }
            input::Chord chord;
            if(!row.defaultShortcut.empty() && !input::Parse(row.defaultShortcut,row.kind,chord)) {
                SetError(error,L"Invalid default shortcut.");return false;
            }
            row.id=L"plugin."+result.id+L"."+local;row.pluginId=result.id;row.pluginName=result.name;
            result.shortcuts.push_back(std::move(row));
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
