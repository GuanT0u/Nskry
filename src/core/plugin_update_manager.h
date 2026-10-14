#pragma once

#include "core/plugin_package_manager.h"
#include "core/plugin_update_policy.h"
#include "core/update_task.h"
#include <string>
#include <vector>

namespace nskry {
class PluginUpdateManager {
  public:
    PluginUpdateManager(PluginRegistry &registry, PluginPackageManager &packages);
    // Synchronous compatibility/headless helpers. UI must use Work + UpdateTask.
    std::vector<PluginUpdate> CheckForUpdates(const std::wstring &officialCatalogUrl, std::wstring *error = nullptr);
    PluginPackageResult DownloadAndStage(const PluginUpdate &update);
    UpdateTask::Work MakeCheckWork(const std::wstring &officialCatalogUrl) const;
    UpdateTask::Work MakeDownloadWork(const PluginUpdate &update, std::wstring &error) const;
    PluginPackageResult CommitDownload(const PluginUpdate &update, const DownloadedUpdate &download);

  private:
    bool ValidUpdate(const PluginUpdate &update) const;
    PluginRegistry &m_registry;
    PluginPackageManager &m_packages;
};
} // namespace nskry
