#include "pch.h"
#include "core/json.h"
#include "core/settings_manager.h"

#include <fstream>
#include <sstream>

namespace nskry {
namespace {

const std::wstring kEmptyValue;

bool ReadOptionalString(const json::Value& object, std::wstring_view key, std::wstring& value) {
    const json::Value* member = object.Find(key);
    return !member || (member->AsString() && object.GetString(key, value));
}

bool ReadOptionalBool(const json::Value& object, std::wstring_view key, bool& value) {
    const json::Value* member = object.Find(key);
    return !member || (member->AsBool() && object.GetBool(key, value));
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

    if (::GetFileAttributesW(m_settingsPath.c_str()) == INVALID_FILE_ATTRIBUTES) return Save();

    json::Value root;
    if (!json::ParseUtf8File(m_settingsPath, root) || !root.IsObject()) return false;

    const json::Value* hotkeys = root.Find(L"hotkeys");
    const json::Value::Object* hotkeyObject = hotkeys ? hotkeys->AsObject() : nullptr;
    if (!hotkeyObject) return false;
    for (const auto& [commandId, shortcutValue] : *hotkeyObject) {
        const std::wstring* shortcut = shortcutValue.AsString();
        if (!shortcut || commandId.empty()) return false;
        m_hotkeys[commandId] = *shortcut;
    }

    const json::Value* general = root.Find(L"general");
    if (general && !general->IsObject()) return false;
    const json::Value& generalObject = general ? *general : root; // Accept the early flat settings shape.
    if (!ReadOptionalBool(generalObject, L"run_at_startup", m_runAtStartup) ||
        !ReadOptionalBool(generalObject, L"notifications", m_notifications) ||
        !ReadOptionalString(generalObject, L"theme", m_theme)) return false;

    const json::Value* updates = root.Find(L"updates");
    if (updates && !updates->IsObject()) return false;
    const json::Value& updatesObject = updates ? *updates : root;
    if (!ReadOptionalString(updatesObject, L"official_plugin_catalog_url", m_officialPluginCatalogUrl)) return false;
    return true;
}

bool SettingsManager::Save() const {
    if (!EnsureConfigDirectory() || m_settingsPath.empty()) return false;

    std::wostringstream output;
    output << L"{\n"
           << L"  \"general\": {\n"
           << L"    \"run_at_startup\": " << (m_runAtStartup ? L"true" : L"false") << L",\n"
           << L"    \"notifications\": " << (m_notifications ? L"true" : L"false") << L",\n"
           << L"    \"theme\": \"" << json::EscapeString(m_theme) << L"\"\n"
           << L"  },\n"
           << L"  \"capture\": {\n"
           << L"    \"default_save_directory\": \"\",\n"
           << L"    \"tray_double_click\": \"capture\"\n"
           << L"  },\n"
           << L"  \"updates\": {\n"
           << L"    \"check_automatically\": true,\n"
           << L"    \"include_prerelease\": false,\n"
           << L"    \"official_plugin_catalog_url\": \"" << json::EscapeString(m_officialPluginCatalogUrl) << L"\"\n"
           << L"  },\n"
           << L"  \"hotkeys\": {\n";

    bool first = true;
    for (const auto& [commandId, shortcut] : m_hotkeys) {
        if (!first) output << L",\n";
        output << L"    \"" << json::EscapeString(commandId) << L"\": \"" << json::EscapeString(shortcut) << L"\"";
        first = false;
    }
    output << L"\n  }\n}\n";
    std::string bytes;
    if (!json::ToUtf8(output.str(), bytes)) return false;
    std::ofstream file(m_settingsPath, std::ios::binary | std::ios::trunc);
    return file && static_cast<bool>(file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())));
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
