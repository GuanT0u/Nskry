#include "pch.h"
#include "core/json.h"
#include "core/plugin_update_manager.h"
#include "update/downloader.h"
#include "update/semver.h"

namespace nskry { namespace {
bool Dir(const std::wstring& path) { return ::CreateDirectoryW(path.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS; }
} PluginUpdateManager::PluginUpdateManager(PluginRegistry& registry, PluginPackageManager& packages) : m_registry(registry), m_packages(packages) {}
bool PluginUpdateManager::ReadFeed(const std::wstring& url, PluginSource source, std::vector<PluginUpdate>& updates, std::wstring* error) const {
    std::wstring text;
    if (!Downloader::DownloadTextHttps(url, text, error)) return false;

    json::Value root;
    std::wstring parseError;
    if (!json::Parse(text, root, &parseError) || !root.IsObject()) {
        if (error) *error = parseError.empty() ? L"Plugin update feed root must be an object."
                                              : L"Invalid plugin update feed: " + parseError;
        return false;
    }

    std::vector<const json::Value*> entries;
    if (const json::Value* plugins = root.Find(L"plugins")) {
        const json::Value::Array* array = plugins->AsArray();
        if (!array) {
            if (error) *error = L"Plugin update feed 'plugins' field must be an array.";
            return false;
        }
        entries.reserve(array->size());
        for (const json::Value& item : *array) entries.push_back(&item);
    } else {
        // A per-plugin update URL serves one update object directly.
        entries.push_back(&root);
    }

    for (const json::Value* item : entries) {
        PluginUpdate update;
        if (!item->IsObject() || !item->GetString(L"id", update.pluginId) ||
            !item->GetString(L"latest_version", update.version) ||
            !item->GetString(L"package_url", update.packageUrl) || !item->GetString(L"sha256", update.sha256)) continue;
        const PluginRecord* installed = m_registry.Find(update.pluginId);
        if (!installed || !SemVer::IsValid(update.version) || !SemVer::IsValid(installed->manifest.version) ||
            SemVer::Compare(update.version, installed->manifest.version) <= 0) continue;
        update.name = installed->manifest.name;
        update.currentVersion = installed->manifest.version;
        update.source = source;
        if (const json::Value* notes = item->Find(L"release_notes")) {
            const std::wstring* value = notes->AsString();
            if (!value) continue;
            update.releaseNotes = *value;
        }
        updates.push_back(std::move(update));
    }
    return true;
}
std::vector<PluginUpdate> PluginUpdateManager::CheckForUpdates(const std::wstring& officialCatalogUrl, std::wstring* error) { std::vector<PluginUpdate> updates; std::wstring firstError; if (!officialCatalogUrl.empty() && !ReadFeed(officialCatalogUrl, PluginSource::Official, updates, &firstError)) {} for (const PluginRecord* record : m_registry.GetAll()) if (!record->manifest.updateUrl.empty() && !ReadFeed(record->manifest.updateUrl, record->source, updates, &firstError)) {} if (error && !firstError.empty()) *error = firstError; return updates; }
PluginPackageResult PluginUpdateManager::DownloadAndStage(const PluginUpdate& update) { PluginPackageResult result; if (update.packageUrl.rfind(L"https://", 0) != 0) { result.message = L"Update package URL must use HTTPS."; return result; } const std::wstring cacheRoot = m_registry.AppRoot() + L"\\cache"; const std::wstring downloadRoot = cacheRoot + L"\\downloads"; if (m_registry.AppRoot().empty() || !Dir(cacheRoot) || !Dir(downloadRoot)) { result.message = L"Update download cache is unavailable."; return result; } const std::wstring destination = downloadRoot + L"\\" + update.pluginId + L"-" + update.version + L".nskryplugin"; std::wstring error; if (!Downloader::DownloadFileHttps(update.packageUrl, destination, &error) || !Downloader::VerifySha256File(destination, update.sha256, &error)) { ::DeleteFileW(destination.c_str()); result.message = error; return result; } result = m_packages.InstallPackage(destination, update.source); if (!result.success) ::DeleteFileW(destination.c_str()); return result; }
} // namespace nskry
