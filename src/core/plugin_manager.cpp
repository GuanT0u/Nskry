#include "pch.h"
#include "core/plugin_manager.h"

namespace nskry {

PluginManager& PluginManager::Instance() {
    static PluginManager s_instance;
    return s_instance;
}

void PluginManager::Initialize(PluginRegistry& registry, const NskryHostContext& hostContext) {
    if (m_initialized) return;
    m_registry = &registry;
    m_hostContext = hostContext;
    m_initialized = true;

    // Startup is deliberately opt-in. All normal plugins remain installed and
    // enabled, but consume no DLL/runtime memory until their command is used.
    for (const PluginRecord* record : m_registry->GetAll()) {
        if (record->enabled && record->manifest.loadPolicy == PluginLoadPolicy::Startup) {
            EnsureLoaded(record->manifest.id);
        }
    }
}

void PluginManager::Shutdown() {
    for (auto it = m_plugins.begin(); it != m_plugins.end();) {
        const std::wstring id = it->first;
        ++it;
        Unload(id);
    }
    m_plugins.clear();
    m_registry = nullptr;
    m_hostContext = {};
    m_initialized = false;
}

bool PluginManager::EnsureLoaded(const std::wstring& pluginId) {
    if (!m_initialized || !m_registry || pluginId.empty()) return false;
    if (m_plugins.contains(pluginId)) return true;

    const PluginRecord* record = m_registry->Find(pluginId);
    if (!record || !record->enabled || !record->manifest.IsCompatibleWithHost()) return false;
    return LoadPlugin(*record);
}

bool PluginManager::LoadPlugin(const PluginRecord& record) {
    const std::wstring dllPath = record.installPath + L"\\" + record.manifest.entry;
    const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    HMODULE module = ::LoadLibraryExW(dllPath.c_str(), nullptr, flags);
    if (!module) return false;

    const auto fnInfo = reinterpret_cast<decltype(&nskry_plugin_info)>(::GetProcAddress(module, "nskry_plugin_info"));
    const auto fnInit = reinterpret_cast<decltype(&nskry_plugin_init)>(::GetProcAddress(module, "nskry_plugin_init"));
    const auto fnExecute = reinterpret_cast<decltype(&nskry_plugin_execute)>(::GetProcAddress(module, "nskry_plugin_execute"));
    const auto fnShutdown = reinterpret_cast<decltype(&nskry_plugin_shutdown)>(::GetProcAddress(module, "nskry_plugin_shutdown"));
    if (!fnInfo || !fnInit || !fnExecute || !fnShutdown) {
        ::FreeLibrary(module);
        return false;
    }

    // This check is runtime ABI validation, not metadata discovery: all data
    // displayed before load still comes exclusively from manifest.json.
    const NskryPluginInfo* runtimeInfo = nullptr;
    bool initialized = false;
    try {
        runtimeInfo = fnInfo();
        initialized = runtimeInfo && runtimeInfo->structSize >= sizeof(NskryPluginInfo) &&
            runtimeInfo->apiVersion == NSKRY_PLUGIN_API_VERSION && runtimeInfo->id && runtimeInfo->version &&
            record.manifest.apiVersion == runtimeInfo->apiVersion && record.manifest.id == runtimeInfo->id &&
            record.manifest.version == runtimeInfo->version && fnInit(&m_hostContext) != 0;
    } catch (...) {
        initialized = false;
    }
    if (!initialized) {
        ::FreeLibrary(module);
        return false;
    }

    PluginEntry entry;
    entry.hModule = module;
    entry.id = record.manifest.id;
    entry.fnExecute = fnExecute;
    entry.fnShutdown = fnShutdown;
    m_plugins.emplace(entry.id, std::move(entry));
    m_registry->SetLoaded(record.manifest.id, true);
    return true;
}

bool PluginManager::Unload(const std::wstring& pluginId) {
    const auto it = m_plugins.find(pluginId);
    if (it == m_plugins.end()) return false;
    if (it->second.fnShutdown) {
        try { it->second.fnShutdown(); } catch (...) {}
    }
    if (it->second.hModule) ::FreeLibrary(it->second.hModule);
    m_plugins.erase(it);
    if (m_registry) m_registry->SetLoaded(pluginId, false);
    return true;
}

bool PluginManager::ExecutePlugin(const std::wstring& pluginId, const NskryHostContext& hostContext) {
    if (!EnsureLoaded(pluginId)) return false;
    const auto it = m_plugins.find(pluginId);
    if (it == m_plugins.end() || !it->second.fnExecute) return false;
    try {
        it->second.fnExecute(&hostContext);
        return true;
    } catch (...) {
        return false;
    }
}

bool PluginManager::IsLoaded(const std::wstring& pluginId) const {
    return m_plugins.contains(pluginId);
}

std::vector<PluginToolbarAction> PluginManager::GetEnabledToolbarActions() const {
    std::vector<PluginToolbarAction> actions;
    if (!m_registry) return actions;
    for (const PluginRecord* record : m_registry->GetAll()) {
        if (record->enabled && record->manifest.toolbarAction && record->manifest.IsCompatibleWithHost())
            actions.push_back({ record->manifest.id, record->manifest.name });
    }
    return actions;
}

} // namespace nskry
