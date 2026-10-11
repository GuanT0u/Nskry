#include "pch.h"
#include "core/plugin_update_policy.h"
#include "core/json.h"
#include "update/semver.h"
#include "sdk/nskry_plugin.h"
#include <winhttp.h>
#include <set>

namespace nskry {
bool IsPluginHttpsUrl(const std::wstring& url) {
    if (url.empty() || url.find_first_of(L"\r\n\t ") != std::wstring::npos) return false;
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    return ::WinHttpCrackUrl(url.c_str(), 0, 0, &parts) && parts.nScheme == INTERNET_SCHEME_HTTPS &&
        parts.dwHostNameLength && !parts.dwUserNameLength && !parts.dwPasswordLength;
}
bool ParsePluginUpdateFeed(const std::wstring& text,
    const std::vector<const PluginRecord*>& installed, PluginSource feedSource,
    const std::wstring& requestedId, std::vector<PluginUpdate>& updates, std::wstring* error) {
    const auto fail = [&](const wchar_t* message) { if (error) *error = message; return false; };
    json::Value root;
    if (!json::Parse(text, root, error) || !root.IsObject()) return fail(L"Invalid plugin update feed.");
    if (root.Find(L"schema")) {
        unsigned int schema{};
        if (!root.GetUnsigned(L"schema", schema) || schema != 1) return fail(L"Unsupported plugin update feed schema.");
    }
    if ((feedSource == PluginSource::Official) != requestedId.empty())
        return fail(L"Invalid plugin update source scope.");
    std::vector<const json::Value*> entries;
    if (const auto* plugins = root.Find(L"plugins")) {
        const auto* array = plugins->AsArray();
        if (!array || array->size() > 2000) return fail(L"Invalid plugin catalog entries.");
        for (const auto& item : *array) entries.push_back(&item);
    } else entries.push_back(&root);
    std::vector<PluginUpdate> result;
    std::set<std::wstring> seen;
    for (const auto* item : entries) {
        std::wstring id;
        if (!item->IsObject() || !item->GetString(L"id", id)) return fail(L"Update entry requires an id.");
        if (!requestedId.empty() && id != requestedId) continue;
        const auto found = std::find_if(installed.begin(), installed.end(), [&](const auto* p) { return p && p->manifest.id == id; });
        if (found == installed.end()) continue;
        const PluginRecord& record = **found;
        if ((record.source == PluginSource::Official) != (feedSource == PluginSource::Official)) continue;
        if (!seen.insert(id).second) return fail(L"Duplicate plugin id in update feed.");
        PluginUpdate update;
        update.pluginId = id; update.source = record.source;
        update.name = record.manifest.name; update.currentVersion = record.manifest.version;
        if (!PluginManifest::IsSafePluginId(id) || !item->GetString(L"latest_version", update.version) ||
            !SemVer::IsValid(update.version) || !item->GetString(L"package_url", update.packageUrl) ||
            !IsPluginHttpsUrl(update.packageUrl) || !item->GetString(L"sha256", update.sha256) ||
            update.sha256.size() != 64 || !std::all_of(update.sha256.begin(), update.sha256.end(), [](wchar_t c) {
                return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
            })) return fail(L"Update entry requires a valid version, HTTPS package URL and SHA-256.");
        for (const auto key : {L"min_nskry", L"max_nskry"}) {
            if (const auto* member = item->Find(key); member && !member->IsNull()) {
                const auto* version = member->AsString();
                if (!version || !SemVer::IsValid(*version)) return fail(L"Invalid update compatibility range.");
            }
        }
        std::wstring min, max;
        item->GetString(L"min_nskry", min); item->GetString(L"max_nskry", max);
        if ((!min.empty() && SemVer::Compare(NSKRY_VERSION, min) < 0) ||
            (!max.empty() && SemVer::Compare(NSKRY_VERSION, max) > 0)) continue;
        if (const auto* notes = item->Find(L"release_notes")) {
            const auto* value = notes->AsString();
            if (!value) return fail(L"Update release notes must be text.");
            update.releaseNotes = *value;
        }
        if (SemVer::IsValid(record.manifest.version) && SemVer::Compare(update.version, record.manifest.version) > 0)
            result.push_back(std::move(update));
    }
    updates.insert(updates.end(), std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
    return true;
}
bool MatchesPluginUpdate(const PluginManifest& package, const PluginUpdate& update, std::wstring* error) {
    if (package.id != update.pluginId || package.version != update.version) {
        if (error) *error = L"Downloaded package id/version does not match the selected update.";
        return false;
    }
    return package.IsCompatibleWithHost(error);
}
} // namespace nskry
