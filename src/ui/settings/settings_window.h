#pragma once

#include "core/plugin_package_manager.h"
#include "core/plugin_update_manager.h"
#include "core/settings_manager.h"
#include <functional>
#include <string>
#include <vector>

namespace nskry {

struct SettingsPluginItem {
    std::wstring id, name, version, author, status;
    bool enabled = true;
};

/// Process-boundary-friendly service contract. The window owns no core state.
struct SettingsWindowServices {
    std::function<UserSettings()> getSettings;
    std::function<bool(const UserSettings&, std::wstring&)> applySettings;
    std::function<std::vector<SettingsPluginItem>()> getPlugins;
    std::function<bool(const std::wstring&, bool)> setPluginEnabled;
    std::function<bool(const std::wstring&, std::wstring&)> uninstallPlugin;
    std::function<bool(const std::wstring&, PluginManifest&, std::wstring&)> inspectPluginPackage;
    std::function<PluginPackageResult(const std::wstring&)> installThirdPartyPlugin;
    std::function<std::vector<PluginUpdate>(std::wstring&)> checkPluginUpdates;
    std::function<PluginPackageResult(const PluginUpdate&)> downloadPluginUpdate;
};

class SettingsWindow {
public:
    SettingsWindow(SettingsWindowServices services, std::function<void()> onClosed);
    ~SettingsWindow();
    void Show(HWND owner);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void CreateControls();
    void SelectPage(int page);
    void RefreshFromServices();
    void RefreshPluginList();
    void ApplySettings();
    void ToggleSelectedPlugin();
    void UninstallSelectedPlugin();
    void BrowseAndInstallPlugin();
    void InstallThirdPartyPackage(const std::wstring& packagePath);
    void CheckForPluginUpdates();
    void Close();

    SettingsWindowServices m_services;
    std::function<void()> m_onClosed;
    HWND m_hwnd{};
    HWND m_tab{};
    HWND m_runAtStartup{}, m_notifications{}, m_theme{};
    HWND m_captureHotkey{}, m_exitHotkey{};
    HWND m_pluginList{}, m_pluginToggle{}, m_pluginUninstall{}, m_pluginInstall{}, m_pluginCheckUpdates{};
    HWND m_aboutText{};
    HWND m_apply{};
    int m_page = 0;
    bool m_closed = false;
    std::vector<SettingsPluginItem> m_plugins;
    std::vector<HWND> m_generalDecor, m_hotkeyDecor;
    static inline std::once_flag s_classOnce;
};

} // namespace nskry
