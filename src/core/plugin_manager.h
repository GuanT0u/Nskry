#pragma once

#include "core/plugin_registry.h"
#include "sdk/nskry_plugin.h"
#include "sdk/nskry_capture.h"
#include "sdk/nskry_commands.h"
#include "sdk/nskry_actions.h"
#include <functional>
#include <string>
#include <unordered_map>

namespace nskry {

struct PluginEntry {
    HMODULE hModule = nullptr;
    std::wstring id;
    decltype(&nskry_plugin_execute) fnExecute = nullptr;
    decltype(&nskry_plugin_shutdown) fnShutdown = nullptr;
    NskryConfigureInputV1 fnConfigureInput = nullptr;
    NskryConfigureInputV2 fnConfigureInputV2 = nullptr;
    NskryCommandHandlerV1 fnCommand = nullptr;
    NskryExecuteCaptureActionV1 fnCaptureAction = nullptr;
    NskryShowSettingsV1 fnShowSettings = nullptr;
    struct InputReport {
        enum class Status {
            NotLoaded,
            Unconfirmed,
            Applied,
            Failed,
            Unsupported
        };
        std::wstring pluginId, message;
        Status status = Status::Unconfirmed;
    } inputReport;
};

struct PluginToolbarAction {
    std::wstring id;
    std::wstring label;
    std::wstring actionId;
};
struct PluginImageAction {
    std::wstring id;
    std::wstring label;
};

/// Runtime-only plugin service. Installation metadata and enablement belong to
/// PluginRegistry; package/update lifecycle belongs to separate services.
class PluginManager {
  public:
    static PluginManager &Instance();

    void Initialize(PluginRegistry &registry, const NskryHostContext &hostContext);
    void Shutdown();

    bool EnsureLoaded(const std::wstring &pluginId);
    bool Unload(const std::wstring &pluginId);
    bool ExecutePlugin(const std::wstring &pluginId, const NskryHostContext &hostContext,
                       const std::wstring &actionId = {});
    // Does not load idle plugins. Cleanup commands remain usable after disable.
    bool ExecuteCommand(const std::wstring& pluginId, const std::wstring& localId, std::wstring& error);
    bool ShowPluginSettings(const std::wstring &pluginId, HWND owner);
    void SetCaptureService(const NskryCaptureServiceV1& service) { m_captureService = service; }
    void SetCaptureServiceV2(const NskryCaptureServiceV2& service) { m_captureServiceV2 = service; }
    bool IsLoaded(const std::wstring &pluginId) const;
    std::vector<PluginToolbarAction> GetEnabledToolbarActions() const;
    std::vector<PluginImageAction> GetEnabledImageActions() const;
    std::vector<ShortcutDefinition> GetShortcutDefinitions() const;
    void SetShortcutProvider(std::function<std::wstring(const ShortcutDefinition &)> provider) {
        m_shortcutProvider = std::move(provider);
    }
    // Failed IDs are reported to the user; configuration cannot force a native DLL to comply.
    std::vector<PluginEntry::InputReport> RefreshInputConfiguration();
    PluginEntry::InputReport GetInputReport(const std::wstring &id) const;

  private:
    PluginManager() = default;
    ~PluginManager() {
        Shutdown();
    }

    bool LoadPlugin(const PluginRecord &record);
    PluginEntry::InputReport ConfigureInput(PluginEntry &entry, const PluginRecord &record);

    PluginRegistry *m_registry = nullptr;
    NskryHostContext m_hostContext{};
    NskryCaptureServiceV1 m_captureService{};
    NskryCaptureServiceV2 m_captureServiceV2{};
    std::unordered_map<std::wstring, PluginEntry> m_plugins;
    bool m_initialized = false;
    std::function<std::wstring(const ShortcutDefinition &)> m_shortcutProvider;
};

} // namespace nskry
