#include "core/plugin_manager.h"
#include "pch.h"
#include <stdexcept>

namespace nskry {

PluginManager &PluginManager::Instance() {
    static PluginManager s_instance;
    return s_instance;
}

void PluginManager::Initialize(PluginRegistry &registry, const NskryHostContext &hostContext) {
    if (m_initialized)
        return;
    m_registry = &registry;
    m_hostContext = hostContext;
    m_initialized = true;

    // Startup is deliberately opt-in. All normal plugins remain installed and
    // enabled, but consume no DLL/runtime memory until their command is used.
    for (const PluginRecord *record : m_registry->GetAll()) {
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

bool PluginManager::EnsureLoaded(const std::wstring &pluginId) {
    if (!m_initialized || !m_registry || pluginId.empty())
        return false;
    const PluginRecord *record = m_registry->Find(pluginId);
    if (!record || !record->enabled || !record->manifest.IsCompatibleWithHost())
        return false;
    // Existing windows may still hold an action snapshot after disable. A
    // loaded DLL is retained for safe shutdown, not permission to execute it.
    if (m_plugins.contains(pluginId))
        return true;
    return LoadPlugin(*record);
}

bool PluginManager::LoadPlugin(const PluginRecord &record) {
    const std::wstring dllPath = record.installPath + L"\\" + record.manifest.entry;
    const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    HMODULE module = ::LoadLibraryExW(dllPath.c_str(), nullptr, flags);
    if (!module)
        return false;

    const auto fnInfo = reinterpret_cast<decltype(&nskry_plugin_info)>(::GetProcAddress(module, "nskry_plugin_info"));
    const auto fnInit = reinterpret_cast<decltype(&nskry_plugin_init)>(::GetProcAddress(module, "nskry_plugin_init"));
    const auto fnExecute =
        reinterpret_cast<decltype(&nskry_plugin_execute)>(::GetProcAddress(module, "nskry_plugin_execute"));
    const auto fnShutdown =
        reinterpret_cast<decltype(&nskry_plugin_shutdown)>(::GetProcAddress(module, "nskry_plugin_shutdown"));
    const auto configureInput =
        reinterpret_cast<NskryConfigureInputV1>(::GetProcAddress(module, "nskry_plugin_configure_input_v1"));
    const auto configureInputV2 =
        reinterpret_cast<NskryConfigureInputV2>(::GetProcAddress(module, "nskry_plugin_configure_input_v2"));
    const bool localInput = std::any_of(record.manifest.shortcuts.begin(), record.manifest.shortcuts.end(),
                                        [](const ShortcutDefinition &row) { return row.kind != input::Kind::Global; });
    if (localInput && !configureInput && !configureInputV2) {
        ::FreeLibrary(module);
        return false;
    }
    if (!fnInfo || !fnInit || !fnExecute || !fnShutdown) {
        ::FreeLibrary(module);
        return false;
    }

    // This check is runtime ABI validation, not metadata discovery: all data
    // displayed before load still comes exclusively from manifest.json.
    const NskryPluginInfo *runtimeInfo = nullptr;
    bool initialized = false;
    bool initAttempted = false;
    try {
        runtimeInfo = fnInfo();
        initialized = runtimeInfo && runtimeInfo->structSize >= sizeof(NskryPluginInfo) &&
                      runtimeInfo->apiVersion == NSKRY_PLUGIN_API_VERSION && runtimeInfo->id && runtimeInfo->version &&
                      record.manifest.apiVersion == runtimeInfo->apiVersion && record.manifest.id == runtimeInfo->id &&
                      record.manifest.version == runtimeInfo->version;
        if (initialized) {
            initAttempted = true;
            initialized = fnInit(&m_hostContext) != 0;
        }
    } catch (...) {
        initialized = false;
    }
    if (!initialized) {
        // A partially initialized plugin may already own windows/threads.
        // Give it its cleanup contract before releasing the DLL reference.
        if (initAttempted) {
            try {
                fnShutdown();
            } catch (...) {
            }
        }
        ::FreeLibrary(module);
        return false;
    }

    PluginEntry entry;
    entry.hModule = module;
    entry.id = record.manifest.id;
    entry.fnExecute = fnExecute;
    entry.fnShutdown = fnShutdown;
    entry.fnConfigureInput = configureInput;
    entry.fnConfigureInputV2 = configureInputV2;
    try {
        entry.inputReport = ConfigureInput(entry, record);
    } catch (...) {
        try {
            fnShutdown();
        } catch (...) {
        }
        ::FreeLibrary(module);
        return false;
    }
    m_plugins.emplace(entry.id, std::move(entry));
    m_registry->SetLoaded(record.manifest.id, true);
    return true;
}

bool PluginManager::Unload(const std::wstring &pluginId) {
    const auto it = m_plugins.find(pluginId);
    if (it == m_plugins.end())
        return false;
    if (it->second.fnShutdown) {
        try {
            it->second.fnShutdown();
        } catch (...) {
        }
    }
    if (it->second.hModule)
        ::FreeLibrary(it->second.hModule);
    m_plugins.erase(it);
    if (m_registry)
        m_registry->SetLoaded(pluginId, false);
    return true;
}

bool PluginManager::ExecutePlugin(const std::wstring &pluginId, const NskryHostContext &hostContext) {
    if (!EnsureLoaded(pluginId))
        return false;
    const auto it = m_plugins.find(pluginId);
    if (it == m_plugins.end() || !it->second.fnExecute)
        return false;
    try {
        it->second.fnExecute(&hostContext);
        return true;
    } catch (...) {
        return false;
    }
}

bool PluginManager::IsLoaded(const std::wstring &pluginId) const {
    return m_plugins.contains(pluginId);
}

std::vector<ShortcutDefinition> PluginManager::GetShortcutDefinitions() const {
    std::vector<ShortcutDefinition> out;
    if (!m_registry)
        return out;
    for (const auto *record : m_registry->GetAll()) {
        // Editing metadata is safe even for disabled/incompatible packages.
        // EnsureLoaded/action enumeration remains the execution compatibility gate.
        auto rows = record->manifest.shortcuts;
        if (rows.empty() && record->manifest.toolbarAction)
            rows.push_back({L"plugin." + record->manifest.id + L".launch", L"Capture and run " + record->manifest.name,
                            L"", record->manifest.id, record->manifest.name, input::Kind::Global, false, true});
        for (auto &row : rows) {
            row.official = record->source == PluginSource::Official;
            out.push_back(std::move(row));
        }
    }
    return out;
}
PluginEntry::InputReport PluginManager::ConfigureInput(PluginEntry &entry, const PluginRecord &record) {
    using S = PluginEntry::InputReport::Status;
    PluginEntry::InputReport report{record.manifest.id, L"Not confirmed: plugin provides no acknowledgment.",
                                    S::Unconfirmed};
    if (!entry.fnConfigureInput && !entry.fnConfigureInputV2)
        return report;
    const auto &rows = record.manifest.shortcuts;
    std::vector<std::wstring> values;
    std::vector<NskryInputBindingV1> bindings;
    values.reserve(rows.size());
    bindings.reserve(rows.size());
    for (const auto &row : rows)
        values.push_back(row.kind == input::Kind::Global
                             ? L""
                             : (m_shortcutProvider ? m_shortcutProvider(row) : row.defaultShortcut));
    const auto prefix = L"plugin." + record.manifest.id + L".";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind == input::Kind::Global)
            continue; // Launch belongs to the host, not plugin-local input.
        if (!rows[i].id.starts_with(prefix))
            throw std::invalid_argument("Invalid plugin shortcut namespace");
        bindings.push_back({rows[i].id.c_str() + prefix.size(), values[i].c_str()});
    }
    NskryInputConfigV1 config{sizeof(config), 1, uint32_t(bindings.size()), bindings.data()};
    if (entry.fnConfigureInputV2) {
        wchar_t error[512]{};
        NskryInputResultV2 result{sizeof(result), 2, error, 512};
        const auto status = entry.fnConfigureInputV2(&config, &result);
        error[511] = 0;
        report.message = error;
        switch (status) {
        case NSKRY_INPUT_APPLIED:
            report.status = S::Applied;
            report.message = L"Confirmed applied by plugin.";
            break;
        case NSKRY_INPUT_UNSUPPORTED:
            report.status = S::Unsupported;
            if (report.message.empty())
                report.message = L"Plugin does not support this input configuration.";
            break;
        case NSKRY_INPUT_FAILED:
            report.status = S::Failed;
            if (report.message.empty())
                report.message = L"Plugin rejected input configuration.";
            break;
        default:
            report.status = S::Failed;
            report.message = L"Plugin returned an invalid input acknowledgment.";
            break;
        }
    } else
        entry.fnConfigureInput(&config);
    return report;
}
std::vector<PluginEntry::InputReport> PluginManager::RefreshInputConfiguration() {
    std::vector<PluginEntry::InputReport> reports;
    if (!m_registry)
        return reports;
    for (auto &[id, entry] : m_plugins)
        if (const auto *record = m_registry->Find(id)) {
            try {
                entry.inputReport = ConfigureInput(entry, *record);
            } catch (...) {
                entry.inputReport = {id, L"Plugin input configuration threw an exception.",
                                     PluginEntry::InputReport::Status::Failed};
            }
            reports.push_back(entry.inputReport);
        }
    return reports;
}
PluginEntry::InputReport PluginManager::GetInputReport(const std::wstring &id) const {
    auto entry = m_plugins.find(id);
    return entry == m_plugins.end() ? PluginEntry::InputReport{id, L"Not loaded: confirmation has not been queried.",
                                                               PluginEntry::InputReport::Status::NotLoaded}
                                    : entry->second.inputReport;
}

std::vector<PluginToolbarAction> PluginManager::GetEnabledToolbarActions() const {
    std::vector<PluginToolbarAction> actions;
    if (!m_registry)
        return actions;
    for (const PluginRecord *record : m_registry->GetAll()) {
        if (record->enabled && record->manifest.toolbarAction && record->manifest.IsCompatibleWithHost())
            actions.push_back({record->manifest.id, record->manifest.name});
    }
    return actions;
}

std::vector<PluginImageAction> PluginManager::GetEnabledImageActions() const {
    std::vector<PluginImageAction> actions;
    if (!m_registry)
        return actions;
    for (const PluginRecord *record : m_registry->GetAll()) {
        if (record->enabled && record->manifest.postCapture && record->manifest.IsCompatibleWithHost())
            actions.push_back({record->manifest.id, record->manifest.name});
    }
    return actions;
}

} // namespace nskry
