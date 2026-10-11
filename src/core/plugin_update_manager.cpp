#include "pch.h"
#include "core/plugin_update_manager.h"
#include "update/downloader.h"
#include "update/semver.h"

namespace nskry {
namespace {
bool Dir(const std::wstring& path) {
    return ::CreateDirectoryW(path.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS;
}
}
PluginUpdateManager::PluginUpdateManager(PluginRegistry& registry, PluginPackageManager& packages)
    : m_registry(registry), m_packages(packages) {}

bool PluginUpdateManager::ReadFeed(const std::wstring& url, PluginSource source, const std::wstring& requestedId,
    std::vector<PluginUpdate>& updates, std::wstring* error) const {
    std::wstring text;
    if (!IsPluginHttpsUrl(url)) { if (error) *error = L"Plugin update feed must use a valid HTTPS URL."; return false; }
    if (!Downloader::DownloadTextHttps(url, text, error)) return false;
    return ParsePluginUpdateFeed(text, m_registry.GetAll(), source, requestedId, updates, error);
}

std::vector<PluginUpdate> PluginUpdateManager::CheckForUpdates(const std::wstring& officialCatalogUrl, std::wstring* error) {
    std::vector<PluginUpdate> updates;
    std::wstring warnings;
    const auto note = [&](const std::wstring& message) {
        if (!warnings.empty()) warnings += L"\n";
        warnings += message;
    };
    bool needsOfficialCatalog = false;
    for (const auto* record : m_registry.GetAll()) {
        if (record->source == PluginSource::Official) { needsOfficialCatalog = true; continue; }
        if (record->manifest.updateUrl.empty()) { note(record->manifest.name + L": no publisher update feed; not checked."); continue; }
        std::wstring feedError;
        if (!ReadFeed(record->manifest.updateUrl, record->source, record->manifest.id, updates, &feedError))
            note(record->manifest.name + L": " + feedError);
    }
    if (needsOfficialCatalog) {
        if (officialCatalogUrl.empty()) note(L"Official plugin catalog is not configured; official updates were not checked.");
        else {
            std::wstring feedError;
            if (!ReadFeed(officialCatalogUrl, PluginSource::Official, L"", updates, &feedError)) note(L"Official catalog: " + feedError);
        }
    }
    if (error) *error = std::move(warnings);
    return updates;
}

PluginPackageResult PluginUpdateManager::DownloadAndStage(const PluginUpdate& update) {
    PluginPackageResult result;
    const auto* installed = m_registry.Find(update.pluginId);
    if (!installed || installed->source != update.source || installed->manifest.version != update.currentVersion ||
        !PluginManifest::IsSafePluginId(update.pluginId) || !SemVer::IsValid(update.version) ||
        SemVer::Compare(update.version, installed->manifest.version) <= 0 || !IsPluginHttpsUrl(update.packageUrl)) {
        result.message = L"The update is invalid or installed plugin metadata changed. Check for updates again.";
        return result;
    }
    const std::wstring cacheRoot = m_registry.AppRoot() + L"\\cache";
    const std::wstring downloadRoot = cacheRoot + L"\\downloads";
    if (m_registry.AppRoot().empty() || !Dir(cacheRoot) || !Dir(downloadRoot)) {
        result.message = L"Update download cache is unavailable."; return result;
    }
    const std::wstring destination = downloadRoot + L"\\" + update.pluginId + L"-" + update.version + L".nskryplugin";
    std::wstring failure;
    PluginManifest package;
    if (!Downloader::DownloadFileHttps(update.packageUrl, destination, &failure) ||
        !Downloader::VerifySha256File(destination, update.sha256, &failure) ||
        !m_packages.InspectPackage(destination, package, &failure) || !MatchesPluginUpdate(package, update, &failure)) {
        ::DeleteFileW(destination.c_str()); result.message = failure; return result;
    }
    result = m_packages.InstallPackage(destination, installed->source);
    // Installation copied/staged it; don't retain a second model/runtime archive.
    ::DeleteFileW(destination.c_str());
    return result;
}
} // namespace nskry
