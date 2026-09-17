#pragma once

#include "core/plugin_registry.h"
#include <string>
#include <vector>

namespace nskry {

struct PluginPackageResult {
    bool success = false;
    bool restartRequired = false;
    std::wstring pluginId;
    std::wstring message;
};

/// Handles package lifecycle only: verify, stage, install, update, and delayed
/// removal. It never loads plugin DLLs.
class PluginPackageManager {
public:
    explicit PluginPackageManager(PluginRegistry& registry);

    bool Initialize();
    bool ApplyPendingOperations(std::wstring* error = nullptr);

    /// Verifies and reads package metadata without installing or loading its DLL.
    bool InspectPackage(const std::wstring& packagePath, PluginManifest& manifest,
                        std::wstring* error = nullptr);
    PluginPackageResult InstallPackage(const std::wstring& packagePath, PluginSource source);
    PluginPackageResult Uninstall(const std::wstring& pluginId);

private:
    enum class PendingType { Update, Delete };
    struct PendingOperation {
        PendingType type = PendingType::Update;
        std::wstring pluginId;
        std::wstring stagedPath;
    };

    bool EnsureDirectories() const;
    bool ExtractToStaging(const std::wstring& packagePath, const std::wstring& destination, std::wstring* error) const;
    bool ValidateExtractedTree(const std::wstring& root, std::wstring* error) const;
    bool ReadPending(std::vector<PendingOperation>& operations) const;
    bool WritePending(const std::vector<PendingOperation>& operations) const;
    bool ApplyUpdate(const PendingOperation& operation, std::wstring* error);
    bool ApplyDelete(const PendingOperation& operation, std::wstring* error);
    std::wstring CreateStagingDirectory(const std::wstring& parent, const wchar_t* prefix) const;
    std::wstring TargetPathForId(const std::wstring& pluginId) const;

    PluginRegistry& m_registry;
    std::wstring m_stagingRoot;
    std::wstring m_packagesRoot;
    std::wstring m_updatesRoot;
    std::wstring m_rollbackRoot;
    std::wstring m_pendingPath;
};

} // namespace nskry
