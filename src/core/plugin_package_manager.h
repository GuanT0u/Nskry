#pragma once

#include "core/plugin_registry.h"
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace nskry {

struct PluginPackageResult {
    bool success = false;
    bool restartRequired = false;
    std::wstring pluginId;
    std::wstring message;
};
struct PreparedPluginPackage {
    PluginManifest manifest;
    std::wstring path, parent;
    ~PreparedPluginPackage();
};

/// Handles package lifecycle only: verify, stage, install, update, and delayed
/// removal. It never loads plugin DLLs.
class PluginPackageManager {
  public:
    explicit PluginPackageManager(PluginRegistry &registry);

    bool Initialize();
    bool ApplyPendingOperations(std::wstring *error = nullptr);

    /// Verifies and reads package metadata without installing or loading its DLL.
    bool InspectPackage(const std::wstring &packagePath, PluginManifest &manifest, std::wstring *error = nullptr);
    PluginPackageResult InstallPackage(const std::wstring &packagePath, PluginSource source);
    // Value-only preparation may run on a worker; commit remains UI-thread-only.
    static std::shared_ptr<PreparedPluginPackage> PreparePackage(const std::wstring &packagePath,
                                                                 const std::wstring &appRoot, std::wstring *error,
                                                                 std::stop_token stop = {});
    PluginPackageResult InstallPrepared(const std::shared_ptr<PreparedPluginPackage> &package, PluginSource source);
    PluginPackageResult Uninstall(const std::wstring &pluginId);

  private:
    enum class PendingType {
        Update,
        Delete
    };
    struct PendingOperation {
        PendingType type = PendingType::Update;
        std::wstring pluginId;
        std::wstring stagedPath;
    };

    bool EnsureDirectories() const;
    static bool ExtractToStaging(const std::wstring &packagePath, const std::wstring &destination, std::wstring *error,
                                 std::stop_token stop = {});
    static bool ValidateExtractedTree(const std::wstring &root, std::wstring *error);
    bool ReadPending(std::vector<PendingOperation> &operations) const;
    bool WritePending(const std::vector<PendingOperation> &operations) const;
    bool ApplyUpdate(const PendingOperation &operation, std::wstring *error);
    bool ApplyDelete(const PendingOperation &operation, std::wstring *error);
    static std::wstring CreateStagingDirectory(const std::wstring &parent, const wchar_t *prefix);
    std::wstring TargetPathForId(const std::wstring &pluginId) const;

    PluginRegistry &m_registry;
    std::wstring m_stagingRoot;
    std::wstring m_packagesRoot;
    std::wstring m_updatesRoot;
    std::wstring m_rollbackRoot;
    std::wstring m_pendingPath;
};

} // namespace nskry
