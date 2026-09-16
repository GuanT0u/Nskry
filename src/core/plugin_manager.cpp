#include "pch.h"
#include "core/plugin_manager.h"
#include <shlwapi.h>

namespace nskry {

PluginManager& PluginManager::Instance() {
    static PluginManager s_instance;
    return s_instance;
}

void PluginManager::Initialize(const NskryHostContext* hostCtx) {
    if (m_initialized) return;
    m_initialized = true;

    // Determine executable directory
    wchar_t exePath[MAX_PATH] = { 0 };
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    ::PathRemoveFileSpecW(exePath);

    std::wstring pluginsDir = exePath;
    pluginsDir += L"\\plugins";

    ScanAndLoad(pluginsDir, hostCtx);
}

void PluginManager::Shutdown() {
    if (!m_initialized) return;

    // Shutdown and free in reverse order
    for (auto it = m_plugins.rbegin(); it != m_plugins.rend(); ++it) {
        if (it->fnShutdown) {
            it->fnShutdown();
        }
        if (it->hModule) {
            ::FreeLibrary(it->hModule);
        }
    }
    m_plugins.clear();
    m_initialized = false;
}

void PluginManager::ScanAndLoad(const std::wstring& pluginsDir, const NskryHostContext* hostCtx) {
    std::wstring searchPattern = pluginsDir + L"\\*.dll";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW(searchPattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::wstring fullPath = pluginsDir + L"\\" + fd.cFileName;
        HMODULE hMod = ::LoadLibraryW(fullPath.c_str());
        if (!hMod) continue;

        auto pInfo     = reinterpret_cast<decltype(&nskry_plugin_info)>(::GetProcAddress(hMod, "nskry_plugin_info"));
        auto pInit     = reinterpret_cast<decltype(&nskry_plugin_init)>(::GetProcAddress(hMod, "nskry_plugin_init"));
        auto pExecute  = reinterpret_cast<decltype(&nskry_plugin_execute)>(::GetProcAddress(hMod, "nskry_plugin_execute"));
        auto pShutdown = reinterpret_cast<decltype(&nskry_plugin_shutdown)>(::GetProcAddress(hMod, "nskry_plugin_shutdown"));

        if (!pInfo || !pInit || !pExecute || !pShutdown) {
            ::FreeLibrary(hMod);
            continue;
        }

        const NskryPluginInfo* info = pInfo();
        if (!info || !pInit(hostCtx)) {
            ::FreeLibrary(hMod);
            continue;
        }

        PluginEntry entry;
        entry.hModule    = hMod;
        entry.info       = *info;
        entry.fnInfo     = pInfo;
        entry.fnInit     = pInit;
        entry.fnExecute  = pExecute;
        entry.fnShutdown = pShutdown;

        m_plugins.push_back(std::move(entry));

    } while (::FindNextFileW(hFind, &fd));

    ::FindClose(hFind);
}

void PluginManager::ExecutePlugin(const wchar_t* pluginId, const NskryHostContext* hostCtx) {
    if (!pluginId || !hostCtx) return;

    for (auto& plugin : m_plugins) {
        if (plugin.info.id && wcscmp(plugin.info.id, pluginId) == 0) {
            if (plugin.fnExecute) {
                plugin.fnExecute(hostCtx);
            }
            break;
        }
    }
}

} // namespace nskry
