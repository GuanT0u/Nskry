#include "pch.h"
#include "core/plugin_registry.h"

#include <fstream>
#include <regex>

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

bool ReadUtf8File(const std::wstring& path, std::wstring& content) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.empty()) { content.clear(); return true; }
    const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (required <= 0) return false;
    content.resize(required);
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), content.data(), required) > 0;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(required, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

bool FindString(const std::wstring& json, const wchar_t* key, std::wstring& value) {
    const std::wregex pattern(std::wstring(LR"json(")json") + key + LR"json("\s*:\s*"([^"\\]*)")json");
    std::wsmatch match;
    if (!std::regex_search(json, match, pattern)) return false;
    value = match[1].str();
    return true;
}

bool FindBool(const std::wstring& json, const wchar_t* key, bool& value) {
    const std::wregex pattern(std::wstring(LR"json(")json") + key + LR"json("\s*:\s*(true|false))json");
    std::wsmatch match;
    if (!std::regex_search(json, match, pattern)) return false;
    value = match[1].str() == L"true";
    return true;
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
    std::wstring json;
    if (!ReadUtf8File(m_registryPath, json)) return false;

    bool complete = true;
    const std::wregex item(LR"json(\{\s*"id"\s*:\s*"([^"\\]+)"\s*,\s*"version"\s*:\s*"([^"\\]+)"\s*,\s*"enabled"\s*:\s*(true|false)\s*,\s*"source_type"\s*:\s*"([^"\\]+)"\s*,\s*"install_path"\s*:\s*"([^"\\]+)"\s*\})json");
    for (std::wsregex_iterator it(json.begin(), json.end(), item), end; it != end; ++it) {
        const std::wstring id = (*it)[1].str();
        if (!PluginManifest::IsSafePluginId(id)) { complete = false; continue; }
        const std::wstring installPath = ResolveInstallPath(m_appRoot, (*it)[5].str());
        PluginManifest manifest;
        if (!PluginManifest::LoadFromFile(installPath + L"\\manifest.json", manifest) || manifest.id != id) {
            complete = false;
            continue;
        }
        PluginRecord record;
        record.manifest = std::move(manifest);
        record.installPath = installPath;
        record.enabled = (*it)[3].str() == L"true";
        record.source = SourceFromString((*it)[4].str());
        m_plugins.emplace(id, std::move(record));
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
    std::ofstream output(m_registryPath, std::ios::binary | std::ios::trunc);
    if (!output) return false;

    output << "{\n  \"schema\": 1,\n  \"plugins\": [";
    bool first = true;
    for (const auto& [id, record] : m_plugins) {
        if (!first) output << ',';
        output << "\n    {\"id\": \"" << ToUtf8(id)
               << "\", \"version\": \"" << ToUtf8(record.manifest.version)
               << "\", \"enabled\": " << (record.enabled ? "true" : "false")
               << ", \"source_type\": \"" << ToUtf8(SourceToString(record.source))
               << "\", \"install_path\": \"" << ToUtf8(RelativeInstallPath(m_pluginRoot, record.installPath)) << "\"}";
        first = false;
    }
    output << "\n  ]\n}\n";
    return static_cast<bool>(output);
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
