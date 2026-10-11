#pragma once

#include "core/plugin_package_manager.h"
#include "core/plugin_update_policy.h"
#include <string>
#include <vector>

namespace nskry {
class PluginUpdateManager {
public:
    PluginUpdateManager(PluginRegistry& registry, PluginPackageManager& packages);
    std::vector<PluginUpdate> CheckForUpdates(const std::wstring& officialCatalogUrl, std::wstring* error = nullptr);
    PluginPackageResult DownloadAndStage(const PluginUpdate& update);
private:
    bool ReadFeed(const std::wstring& url, PluginSource source, const std::wstring& requestedId,
                  std::vector<PluginUpdate>& updates, std::wstring* error) const;
    PluginRegistry& m_registry; PluginPackageManager& m_packages;
};
} // namespace nskry
