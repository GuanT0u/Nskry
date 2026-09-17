#include "pch.h"
#include "core/plugin_update_manager.h"
#include "update/downloader.h"
#include "update/semver.h"

#include <regex>

namespace nskry { namespace {
bool Field(const std::wstring& json, const wchar_t* key, std::wstring& value) {
    std::wsmatch match;
    const std::wstring pattern = L"\\\"" + std::wstring(key) + L"\\\"\\s*:\\s*\\\"([^\\\"\\\\]*)\\\"";
    if (!std::regex_search(json, match, std::wregex(pattern))) return false;
    value = match[1].str();
    return true;
}
bool Dir(const std::wstring& path) { return ::CreateDirectoryW(path.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS; }
} PluginUpdateManager::PluginUpdateManager(PluginRegistry& registry, PluginPackageManager& packages) : m_registry(registry), m_packages(packages) {}
bool PluginUpdateManager::ReadFeed(const std::wstring& url, PluginSource source, std::vector<PluginUpdate>& updates, std::wstring* error) const { std::wstring json; if (!Downloader::DownloadTextHttps(url, json, error)) return false; const std::wregex object(LR"(\{([^{}]*)\})"); for (std::wsregex_iterator it(json.begin(), json.end(), object), end; it != end; ++it) { const std::wstring item = (*it)[1]; PluginUpdate update; if (!Field(item, L"id", update.pluginId) || !Field(item, L"latest_version", update.version) || !Field(item, L"package_url", update.packageUrl) || !Field(item, L"sha256", update.sha256)) continue; const PluginRecord* installed = m_registry.Find(update.pluginId); if (!installed || !SemVer::IsValid(update.version) || !SemVer::IsValid(installed->manifest.version) || SemVer::Compare(update.version, installed->manifest.version) <= 0) continue; update.name = installed->manifest.name; update.currentVersion = installed->manifest.version; update.source = source; Field(item, L"release_notes", update.releaseNotes); updates.push_back(std::move(update)); } return true; }
std::vector<PluginUpdate> PluginUpdateManager::CheckForUpdates(const std::wstring& officialCatalogUrl, std::wstring* error) { std::vector<PluginUpdate> updates; std::wstring firstError; if (!officialCatalogUrl.empty() && !ReadFeed(officialCatalogUrl, PluginSource::Official, updates, &firstError)) {} for (const PluginRecord* record : m_registry.GetAll()) if (!record->manifest.updateUrl.empty() && !ReadFeed(record->manifest.updateUrl, record->source, updates, &firstError)) {} if (error && !firstError.empty()) *error = firstError; return updates; }
PluginPackageResult PluginUpdateManager::DownloadAndStage(const PluginUpdate& update) { PluginPackageResult result; if (update.packageUrl.rfind(L"https://", 0) != 0) { result.message = L"Update package URL must use HTTPS."; return result; } const std::wstring cacheRoot = m_registry.AppRoot() + L"\\cache"; const std::wstring downloadRoot = cacheRoot + L"\\downloads"; if (m_registry.AppRoot().empty() || !Dir(cacheRoot) || !Dir(downloadRoot)) { result.message = L"Update download cache is unavailable."; return result; } const std::wstring destination = downloadRoot + L"\\" + update.pluginId + L"-" + update.version + L".nskryplugin"; std::wstring error; if (!Downloader::DownloadFileHttps(update.packageUrl, destination, &error) || !Downloader::VerifySha256File(destination, update.sha256, &error)) { ::DeleteFileW(destination.c_str()); result.message = error; return result; } result = m_packages.InstallPackage(destination, update.source); if (!result.success) ::DeleteFileW(destination.c_str()); return result; }
} // namespace nskry
