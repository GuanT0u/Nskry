#pragma once

#include "core/plugin_manifest.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace nskry {

enum class PluginSource { Official, ThirdParty, Local, Legacy };

struct PluginRecord {
    PluginManifest manifest;
    std::wstring installPath;
    PluginSource source = PluginSource::Local;
    bool enabled = true;
    bool loaded = false; // Runtime state only; never persisted.
};

/// Owns installed-plugin metadata and persistence. It never loads a plugin DLL.
class PluginRegistry {
public:
    bool Initialize();
    bool Load();
    bool Save() const;

    const PluginRecord* Find(const std::wstring& pluginId) const;
    PluginRecord* Find(const std::wstring& pluginId);
    std::vector<const PluginRecord*> GetAll() const;
    bool UpsertInstalled(PluginManifest manifest, const std::wstring& installPath,
                         PluginSource source, bool enabled = true);
    bool SetEnabled(const std::wstring& pluginId, bool enabled);
    void SetLoaded(const std::wstring& pluginId, bool loaded);

    const std::wstring& PluginRoot() const { return m_pluginRoot; }
    const std::wstring& RegistryPath() const { return m_registryPath; }
    const std::wstring& AppRoot() const { return m_appRoot; }

private:
    bool EnsureDirectories() const;
    bool LoadRegistryFile();
    bool DiscoverPluginDirectories();
    static const wchar_t* SourceToString(PluginSource source);
    static PluginSource SourceFromString(const std::wstring& source);

    std::wstring m_appRoot;
    std::wstring m_pluginRoot;
    std::wstring m_registryPath;
    std::unordered_map<std::wstring, PluginRecord> m_plugins;
};

} // namespace nskry
