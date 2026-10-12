#pragma once

#include <string>
#include <unordered_map>
#include "core/window_visual_settings.h"
#include "core/input_settings.h"

namespace nskry {

struct UserSettings {
    bool runAtStartup = false;
    bool notifications = true;
    bool showStartupHint = true;
    bool hideMonitorOnSourceFocus = false;
    WindowVisualSettings visuals;
    InteractionStyle interaction;
    std::unordered_map<std::wstring,std::wstring> shortcuts;
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
    const std::wstring& GetShortcutValue(const ShortcutDefinition& definition) const;
    void SetHotkey(const std::wstring& commandId, std::wstring shortcut);
    UserSettings GetUserSettings() const;
    void SetUserSettings(const UserSettings& settings);
    const std::wstring& GetOfficialPluginCatalogUrl() const { return m_officialPluginCatalogUrl; }
    const std::wstring& GetOfficialPluginsPageUrl() const { return m_officialPluginsPageUrl; }
    const std::wstring& SettingsPath() const { return m_settingsPath; }
    const InteractionStyle& Interaction()const{return m_interaction;}

private:
    SettingsManager();

    void SetDefaults();
    bool EnsureConfigDirectory() const;

    std::wstring m_configDirectory;
    std::wstring m_settingsPath;
    std::unordered_map<std::wstring, std::wstring> m_hotkeys;
    bool m_runAtStartup = false;
    bool m_notifications = true;
    bool m_showStartupHint = true;
    bool m_hideMonitorOnSourceFocus = false;
    WindowVisualSettings m_visuals;
    InteractionStyle m_interaction;
    std::wstring m_theme = L"system";
    // Empty until the official site goes live. HTTPS/hash aren't publisher
    // signatures; catalog authentication is a separate planned release gate.
    std::wstring m_officialPluginCatalogUrl;
    std::wstring m_officialPluginsPageUrl;
};

} // namespace nskry
