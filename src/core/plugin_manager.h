#pragma once

#include "sdk/nskry_plugin.h"
#include <vector>
#include <string>
#include <memory>

namespace nskry {

struct PluginEntry {
    HMODULE         hModule = nullptr;
    NskryPluginInfo info{};
    decltype(&nskry_plugin_info)     fnInfo = nullptr;
    decltype(&nskry_plugin_init)     fnInit = nullptr;
    decltype(&nskry_plugin_execute)  fnExecute = nullptr;
    decltype(&nskry_plugin_shutdown) fnShutdown = nullptr;
};

class PluginManager {
public:
    static PluginManager& Instance();

    void Initialize(const NskryHostContext* hostCtx);
    void Shutdown();

    const std::vector<PluginEntry>& GetPlugins() const { return m_plugins; }
    void ExecutePlugin(const wchar_t* pluginId, const NskryHostContext* hostCtx);

private:
    PluginManager() = default;
    ~PluginManager() { Shutdown(); }

    void ScanAndLoad(const std::wstring& pluginsDir, const NskryHostContext* hostCtx);

    std::vector<PluginEntry> m_plugins;
    bool m_initialized = false;
};

} // namespace nskry
