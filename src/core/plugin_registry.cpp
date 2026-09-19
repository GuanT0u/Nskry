#include "pch.h"
#include "core/json.h"
#include "core/plugin_registry.h"

#include <fstream>
#include <sstream>

namespace nskry {
namespace {

std::wstring GetLocalAppDataPath() {
    const DWORD required = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0) return {};
    std::wstring path(required, L'\0');
    ::GetEnvironmentVariableW(L"LOCALAPPDATA", path.data(), required);
    path.resize(required - 1);
    return path;
}

std::wstring RelativeInstallPath(const std::wstring& pluginRoot, const std::wstring& fullPath) {
    const std::wstring prefix = pluginRoot + L"\\";
    std::wstring relative = fullPath.rfind(prefix, 0) == 0
        ? L"plugins/" + fullPath.substr(prefix.size())
        : fullPath;
    std::replace(relative.begin(), relative.end(), L'\\', L'/');
    return relative;
}

std::wstring ResolveInstallPath(const std::wstring& appRoot, const std::wstring& path) {
    if (path.rfind(L"plugins/", 0) == 0) {
        std::wstring resolved = path;
        std::replace(resolved.begin(), resolved.end(), L'/', L'\\');
        return appRoot + L"\\" + resolved;
    }
    return path;
}

} // namespace

bool PluginRegistry::Initialize() {
    const std::wstring localAppData = GetLocalAppDataPath();
    if (localAppData.empty()) return false;
    m_appRoot = localAppData + L"\\Nskry";
    m_pluginRoot = m_appRoot + L"\\plugins";
    m_registryPath = m_appRoot + L"\\plugin-registry.json";
    return EnsureDirectories();
}

bool PluginRegistry::EnsureDirectories() const {
    if (m_appRoot.empty()) return false;
    if (!::CreateDirectoryW(m_appRoot.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) return false;
    if (!::CreateDirectoryW(m_pluginRoot.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) return false;
    return true;
}

bool PluginRegistry::Load() {
    if (!EnsureDirectories()) return false;
    m_plugins.clear();
    const bool loadedRegistry = LoadRegistryFile();
    const bool discovered = DiscoverPluginDirectories();
    if (!loadedRegistry || discovered) Save();
    return true;
}

bool PluginRegistry::LoadRegistryFile() {
    json::Value root;
    if (!json::ParseUtf8File(m_registryPath, root) || !root.IsObject()) return false;
    const json::Value* plugins = root.Find(L"plugins");
    const json::Value::Array* pluginArray = plugins ? plugins->AsArray() : nullptr;
    if (!pluginArray) return false;

    bool complete = true;
    for (const json::Value& item : *pluginArray) {
        std::wstring id, version, source, persistedPath;
        bool enabled = true;
        if (!item.IsObject() || !item.GetString(L"id", id) || !item.GetString(L"version", version) ||
            !item.GetBool(L"enabled", enabled) || !item.GetString(L"source_type", source) ||
            !item.GetString(L"install_path", persistedPath)) {
            complete = false;
            continue;
        }
        if (!PluginManifest::IsSafePluginId(id)) { complete = false; continue; }
        const std::wstring installPath = ResolveInstallPath(m_appRoot, persistedPath);
        PluginManifest manifest;
        if (!PluginManifest::LoadFromFile(installPath + L"\\manifest.json", manifest) || manifest.id != id) {
            complete = false;
            continue;
        }
        PluginRecord record;
        record.manifest = std::move(manifest);
        record.installPath = installPath;
        record.enabled = enabled;
        record.source = SourceFromString(source);
        if (!m_plugins.emplace(id, std::move(record)).second) complete = false;
    }
    return complete;
}

bool PluginRegistry::DiscoverPluginDirectories() {
    bool changed = false;
    WIN32_FIND_DATAW data{};
    const HANDLE find = ::FindFirstFileW((m_pluginRoot + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;

    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        const std::wstring installPath = m_pluginRoot + L"\\" + data.cFileName;
        PluginManifest manifest;
        if (!PluginManifest::LoadFromFile(installPath + L"\\manifest.json", manifest)) continue;
        if (m_plugins.contains(manifest.id)) continue;
        PluginRecord record;
        record.manifest = std::move(manifest);
        record.installPath = installPath;
        record.source = PluginSource::Local;
        m_plugins.emplace(record.manifest.id, std::move(record));
        changed = true;
    } while (::FindNextFileW(find, &data));

    ::FindClose(find);
    return changed;
}

bool PluginRegistry::Save() const {
    if (!EnsureDirectories()) return false;
    std::wostringstream output;
    output << L"{\n  \"schema\": 1,\n  \"plugins\": [";
    bool first = true;
    for (const auto& [id, record] : m_plugins) {
        if (!first) output << L',';
        output << L"\n    {\"id\": \"" << json::EscapeString(id)
               << L"\", \"version\": \"" << json::EscapeString(record.manifest.version)
               << L"\", \"enabled\": " << (record.enabled ? L"true" : L"false")
               << L", \"source_type\": \"" << json::EscapeString(SourceToString(record.source))
               << L"\", \"install_path\": \"" << json::EscapeString(RelativeInstallPath(m_pluginRoot, record.installPath)) << L"\"}";
        first = false;
    }
    output << L"\n  ]\n}\n";
    std::string bytes;
    if (!json::ToUtf8(output.str(), bytes)) return false;
    std::ofstream file(m_registryPath, std::ios::binary | std::ios::trunc);
    return file && static_cast<bool>(file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())));
}

const PluginRecord* PluginRegistry::Find(const std::wstring& pluginId) const {
    const auto it = m_plugins.find(pluginId);
    return it == m_plugins.end() ? nullptr : &it->second;
}

PluginRecord* PluginRegistry::Find(const std::wstring& pluginId) {
    const auto it = m_plugins.find(pluginId);
    return it == m_plugins.end() ? nullptr : &it->second;
}

std::vector<const PluginRecord*> PluginRegistry::GetAll() const {
    std::vector<const PluginRecord*> plugins;
    plugins.reserve(m_plugins.size());
    for (const auto& [id, record] : m_plugins) plugins.push_back(&record);
    return plugins;
}

bool PluginRegistry::UpsertInstalled(PluginManifest manifest, const std::wstring& installPath,
                                     PluginSource source, bool enabled) {
    if (!PluginManifest::IsSafePluginId(manifest.id) || installPath.empty()) return false;
    PluginRecord record;
    record.manifest = std::move(manifest);
    record.installPath = installPath;
    record.source = source;
    record.enabled = enabled;
    m_plugins[record.manifest.id] = std::move(record);
    return Save();
}

bool PluginRegistry::SetEnabled(const std::wstring& pluginId, bool enabled) {
    PluginRecord* record = Find(pluginId);
    if (!record) return false;
    record->enabled = enabled;
    return Save();
}

void PluginRegistry::SetLoaded(const std::wstring& pluginId, bool loaded) {
    if (PluginRecord* record = Find(pluginId)) record->loaded = loaded;
}

const wchar_t* PluginRegistry::SourceToString(PluginSource source) {
    switch (source) {
    case PluginSource::Official: return L"official";
    case PluginSource::ThirdParty: return L"third_party";
    case PluginSource::Legacy: return L"legacy";
    default: return L"local";
    }
}

PluginSource PluginRegistry::SourceFromString(const std::wstring& source) {
    if (source == L"official") return PluginSource::Official;
    if (source == L"third_party") return PluginSource::ThirdParty;
    if (source == L"legacy") return PluginSource::Legacy;
    return PluginSource::Local;
}

} // namespace nskry
