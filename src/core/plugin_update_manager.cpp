#include "core/plugin_update_manager.h"
#include "pch.h"
#include "update/downloader.h"
#include "update/semver.h"
#include <chrono>
namespace nskry {
PluginUpdateManager::PluginUpdateManager(PluginRegistry &registry, PluginPackageManager &packages)
    : m_registry(registry), m_packages(packages) {
}
UpdateTask::Work PluginUpdateManager::MakeCheckWork(const std::wstring &url) const {
    std::vector<PluginRecord> snapshot;
    for (const auto *record : m_registry.GetAll())
        snapshot.push_back(*record);
    return [snapshot = std::move(snapshot), url](std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        UpdateTaskResult result;
        std::vector<const PluginRecord *> installed;
        for (const auto &record : snapshot)
            installed.push_back(&record);
        auto note = [&](const std::wstring &text) {
            if (!result.message.empty())
                result.message += L"\n";
            result.message += text;
        };
        auto read = [&](const std::wstring &address, PluginSource source, const std::wstring &id,
                        const std::wstring &name) {
            std::wstring text, error;
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            if (left <= 0) {
                note(L"Update check timed out.");
                return;
            }
            if (!IsPluginHttpsUrl(address) ||
                !Downloader::DownloadTextHttps(address, text, &error, stop, uint32_t(left)) ||
                !ParsePluginUpdateFeed(text, installed, source, id, result.updates, &error))
                note(name + L": " + (error.empty() ? L"Invalid HTTPS feed." : error));
        };
        bool official = false;
        for (const auto &record : snapshot) {
            if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline)
                break;
            if (record.source == PluginSource::Official) {
                official = true;
                continue;
            }
            if (record.manifest.updateUrl.empty())
                note(record.manifest.name + L": no publisher feed; not checked.");
            else
                read(record.manifest.updateUrl, record.source, record.manifest.id, record.manifest.name);
        }
        if (official && !stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            if (url.empty())
                note(L"Official catalog is not configured; official updates were not checked.");
            else
                read(url, PluginSource::Official, L"", L"Official catalog");
        }
        if (!stop.stop_requested() && std::chrono::steady_clock::now() >= deadline)
            note(L"Update check deadline reached; remaining sources were not checked.");
        result.cancelled = stop.stop_requested();
        return result;
    };
}
bool PluginUpdateManager::ValidUpdate(const PluginUpdate &update) const {
    const auto *record = m_registry.Find(update.pluginId);
    return record && record->source == update.source && record->manifest.version == update.currentVersion &&
           PluginManifest::IsSafePluginId(update.pluginId) && SemVer::IsValid(update.version) &&
           SemVer::Compare(update.version, record->manifest.version) > 0 && IsPluginHttpsUrl(update.packageUrl);
}
UpdateTask::Work PluginUpdateManager::MakeDownloadWork(const PluginUpdate &update, std::wstring &error) const {
    error.clear();
    if (!ValidUpdate(update)) {
        error = L"Installed metadata changed or update is invalid. Check again.";
        return {};
    }
    const auto root = m_registry.AppRoot() + L"\\cache";
    const auto downloads = root + L"\\downloads";
    auto dir = [](const std::wstring &path) {
        return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    };
    if (m_registry.AppRoot().empty() || !dir(root) || !dir(downloads)) {
        error = L"Download cache unavailable.";
        return {};
    }
    static unsigned sequence = 0;
    const auto path = downloads + L"\\update-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence) + L".nskryplugin";
    return [update, path, appRoot = m_registry.AppRoot()](std::stop_token stop) {
        UpdateTaskResult result;
        auto artifact = std::make_shared<DownloadedUpdate>();
        artifact->path.reserve(path.size());
        if (Downloader::DownloadFileHttps(update.packageUrl, path, &result.message, stop)) {
            artifact->path = path;
            if (Downloader::VerifySha256File(path, update.sha256, &result.message, stop) && !stop.stop_requested()) {
                artifact->package = PluginPackageManager::PreparePackage(path, appRoot, &result.message, stop);
                if (artifact->package && MatchesPluginUpdate(artifact->package->manifest, update, &result.message) &&
                    !stop.stop_requested())
                    result.download = std::move(artifact);
            }
        }
        result.cancelled = stop.stop_requested();
        return result;
    };
}
PluginPackageResult PluginUpdateManager::CommitDownload(const PluginUpdate &update, const DownloadedUpdate &download) {
    PluginPackageResult result;
    if (!ValidUpdate(update)) {
        result.message = L"Installed metadata changed during download; check for updates again.";
        return result;
    }
    // Main-thread-only disk commit. No network and no background Registry access.
    if (!download.package || !MatchesPluginUpdate(download.package->manifest, update, &result.message))
        return result;
    return m_packages.InstallPrepared(download.package, update.source);
}
std::vector<PluginUpdate> PluginUpdateManager::CheckForUpdates(const std::wstring &url, std::wstring *error) {
    auto result = MakeCheckWork(url)({});
    if (error)
        *error = result.message;
    return result.updates;
}
PluginPackageResult PluginUpdateManager::DownloadAndStage(const PluginUpdate &update) {
    std::wstring error;
    auto work = MakeDownloadWork(update, error);
    if (!work) {
        PluginPackageResult result;
        result.message = error;
        return result;
    }
    auto result = work({});
    if (result.download)
        return CommitDownload(update, *result.download);
    PluginPackageResult failure;
    failure.message = result.message;
    return failure;
}
} // namespace nskry
