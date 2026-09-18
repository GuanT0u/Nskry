#pragma once

#include "core/plugin_registry.h"
#include "sdk/nskry_plugin.h"
#include <string>
#include <unordered_map>

namespace nskry {

struct PluginEntry {
    HMODULE         hModule = nullptr;
    std::wstring    id;
    decltype(&nskry_plugin_execute)  fnExecute = nullptr;
    decltype(&nskry_plugin_shutdown) fnShutdown = nullptr;
};

struct PluginToolbarAction { std::wstring id; std::wstring label; };

/// Runtime-only plugin service. Installation metadata and enablement belong to
/// PluginRegistry; packages and updates will belong to later services.
class PluginManager {
public:
    static PluginManager& Instance();

    void Initialize(PluginRegistry& registry, const NskryHostContext& hostContext);
    void Shutdown();

    bool EnsureLoaded(const std::wstring& pluginId);
    bool Unload(const std::wstring& pluginId);
    bool ExecutePlugin(const std::wstring& pluginId, const NskryHostContext& hostContext);
    bool IsLoaded(const std::wstring& pluginId) const;
    std::vector<PluginToolbarAction> GetEnabledToolbarActions() const;

private:
    PluginManager() = default;
    ~PluginManager() { Shutdown(); }

    bool LoadPlugin(const PluginRecord& record);

    PluginRegistry* m_registry = nullptr;
    NskryHostContext m_hostContext{};
    std::unordered_map<std::wstring, PluginEntry> m_plugins;
    bool m_initialized = false;
};

} // namespace nskry
