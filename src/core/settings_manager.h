#pragma once

#include <string>
#include <unordered_map>

namespace nskry {

struct UserSettings {
    bool runAtStartup = false;
    bool notifications = true;
    std::wstring theme = L"system";
    std::wstring captureShortcut = L"Ctrl+Alt+A";
    std::wstring exitShortcut = L"Ctrl+Alt+Q";
};

/// Minimal, always-lightweight application settings store.  Stage A persists
/// hotkeys now and leaves the remaining sections ready for the Settings UI.
class SettingsManager {
public:
    static SettingsManager& Instance();

    bool Load();
    bool Save() const;

    const std::wstring& GetHotkey(const std::wstring& commandId) const;
    void SetHotkey(const std::wstring& commandId, std::wstring shortcut);
    UserSettings GetUserSettings() const;
    void SetUserSettings(const UserSettings& settings);
    const std::wstring& GetOfficialPluginCatalogUrl() const { return m_officialPluginCatalogUrl; }
    const std::wstring& SettingsPath() const { return m_settingsPath; }

private:
    SettingsManager();

    void SetDefaults();
    bool EnsureConfigDirectory() const;

    std::wstring m_configDirectory;
    std::wstring m_settingsPath;
    std::unordered_map<std::wstring, std::wstring> m_hotkeys;
    bool m_runAtStartup = false;
    bool m_notifications = true;
    std::wstring m_theme = L"system";
    // Empty by default: distributors may configure a signed HTTPS catalog without
    // baking a mutable endpoint into the executable.
    std::wstring m_officialPluginCatalogUrl;
};

} // namespace nskry
