#include "pch.h"
#include "core/settings_manager.h"

#include <fstream>
#include <regex>

namespace nskry {
namespace {

const std::wstring kEmptyValue;

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch == L'\\' || ch == L'\"') escaped.push_back(L'\\');
        escaped.push_back(ch);
    }
    return escaped;
}

std::wstring GetLocalAppDataPath() {
    const DWORD required = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0) return {};
    std::wstring path(required, L'\0');
    ::GetEnvironmentVariableW(L"LOCALAPPDATA", path.data(), required);
    path.resize(required - 1);
    return path;
}

} // namespace

SettingsManager& SettingsManager::Instance() {
    static SettingsManager s_instance;
    return s_instance;
}

SettingsManager::SettingsManager() {
    const std::wstring localAppData = GetLocalAppDataPath();
    if (!localAppData.empty()) {
        m_configDirectory = localAppData + L"\\Nskry\\config";
        m_settingsPath = m_configDirectory + L"\\settings.json";
    }
    SetDefaults();
}

void SettingsManager::SetDefaults() {
    m_hotkeys.clear();
    m_hotkeys.emplace(L"core.capture", L"Ctrl+Alt+A");
    m_hotkeys.emplace(L"core.exit", L"Ctrl+Alt+Q");
    m_runAtStartup = false;
    m_notifications = true;
    m_theme = L"system";
    m_officialPluginCatalogUrl.clear();
}

bool SettingsManager::EnsureConfigDirectory() const {
    if (m_configDirectory.empty()) return false;

    const std::wstring localAppData = GetLocalAppDataPath();
    if (localAppData.empty()) return false;
    const std::wstring appDirectory = localAppData + L"\\Nskry";
    if (!::CreateDirectoryW(appDirectory.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) return false;
    if (!::CreateDirectoryW(m_configDirectory.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) return false;
    return true;
}

bool SettingsManager::Load() {
    SetDefaults();
    if (m_settingsPath.empty()) return false;

    std::wifstream input(m_settingsPath);
    if (!input) return Save();

    const std::wstring content((std::istreambuf_iterator<wchar_t>(input)), std::istreambuf_iterator<wchar_t>());
    const std::wregex hotkeysBlock(LR"("hotkeys"\s*:\s*\{([\s\S]*?)\})");
    std::wsmatch blockMatch;
    if (!std::regex_search(content, blockMatch, hotkeysBlock)) return false;

    const std::wregex item(LR"json("([^"\\]+)"\s*:\s*"([^"\\]*)")json");
    for (std::wsregex_iterator it(blockMatch[1].first, blockMatch[1].second, item), end; it != end; ++it) {
        m_hotkeys[(*it)[1].str()] = (*it)[2].str();
    }
    std::wsmatch match;
    if (std::regex_search(content, match, std::wregex(LR"json("run_at_startup"\s*:\s*(true|false))json"))) m_runAtStartup = match[1] == L"true";
    if (std::regex_search(content, match, std::wregex(LR"json("notifications"\s*:\s*(true|false))json"))) m_notifications = match[1] == L"true";
    if (std::regex_search(content, match, std::wregex(LR"json("theme"\s*:\s*"([^"\\]*)")json"))) m_theme = match[1].str();
    if (std::regex_search(content, match, std::wregex(LR"json("official_plugin_catalog_url"\s*:\s*"([^"\\]*)")json"))) m_officialPluginCatalogUrl = match[1].str();
    return true;
}

bool SettingsManager::Save() const {
    if (!EnsureConfigDirectory() || m_settingsPath.empty()) return false;

    std::wofstream output(m_settingsPath, std::ios::trunc);
    if (!output) return false;

    output << L"{\n"
           << L"  \"general\": {\n"
           << L"    \"run_at_startup\": " << (m_runAtStartup ? L"true" : L"false") << L",\n"
           << L"    \"notifications\": " << (m_notifications ? L"true" : L"false") << L",\n"
           << L"    \"theme\": \"" << JsonEscape(m_theme) << L"\"\n"
           << L"  },\n"
           << L"  \"capture\": {\n"
           << L"    \"default_save_directory\": \"\",\n"
           << L"    \"tray_double_click\": \"capture\"\n"
           << L"  },\n"
           << L"  \"updates\": {\n"
           << L"    \"check_automatically\": true,\n"
           << L"    \"include_prerelease\": false,\n"
           << L"    \"official_plugin_catalog_url\": \"" << JsonEscape(m_officialPluginCatalogUrl) << L"\"\n"
           << L"  },\n"
           << L"  \"hotkeys\": {\n";

    bool first = true;
    for (const auto& [commandId, shortcut] : m_hotkeys) {
        if (!first) output << L",\n";
        output << L"    \"" << JsonEscape(commandId) << L"\": \"" << JsonEscape(shortcut) << L"\"";
        first = false;
    }
    output << L"\n  }\n}\n";
    return static_cast<bool>(output);
}

const std::wstring& SettingsManager::GetHotkey(const std::wstring& commandId) const {
    const auto it = m_hotkeys.find(commandId);
    return it == m_hotkeys.end() ? kEmptyValue : it->second;
}

void SettingsManager::SetHotkey(const std::wstring& commandId, std::wstring shortcut) {
    if (commandId.empty()) return;
    m_hotkeys[commandId] = std::move(shortcut);
}

UserSettings SettingsManager::GetUserSettings() const {
    UserSettings result;
    result.runAtStartup = m_runAtStartup;
    result.notifications = m_notifications;
    result.theme = m_theme;
    result.captureShortcut = GetHotkey(L"core.capture");
    result.exitShortcut = GetHotkey(L"core.exit");
    return result;
}

void SettingsManager::SetUserSettings(const UserSettings& settings) {
    m_runAtStartup = settings.runAtStartup;
    m_notifications = settings.notifications;
    m_theme = settings.theme;
    SetHotkey(L"core.capture", settings.captureShortcut);
    SetHotkey(L"core.exit", settings.exitShortcut);
}

} // namespace nskry
