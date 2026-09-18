#pragma once

#include <string>

namespace nskry {

enum class PluginLoadPolicy { OnDemand, Startup };

/// Metadata that is safe to read before loading a third-party DLL.
struct PluginManifest {
    int manifestVersion = 0;
    std::wstring id;
    std::wstring name;
    std::wstring version;
    std::wstring author;
    std::wstring description;
    std::wstring entry;
    std::wstring architecture;
    unsigned int apiVersion = 0;
    PluginLoadPolicy loadPolicy = PluginLoadPolicy::OnDemand;
    std::wstring nskryMinVersion;
    std::wstring nskryMaxVersion;
    std::wstring homepage;
    std::wstring repository;
    std::wstring updateUrl;
    bool toolbarAction = false;

    static bool LoadFromFile(const std::wstring& path, PluginManifest& manifest, std::wstring* error = nullptr);
    static bool IsSafePluginId(const std::wstring& id);
    static bool IsSafeEntryName(const std::wstring& entry);

    bool IsCompatibleWithHost(std::wstring* error = nullptr) const;
};

} // namespace nskry
