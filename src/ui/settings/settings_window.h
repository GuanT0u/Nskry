#pragma once

#include "core/plugin_package_manager.h"
#include "core/plugin_update_manager.h"
#include "core/settings_manager.h"
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <memory>
#include "ui/settings/input_page.h"
#include "ui/settings/interaction_page.h"

namespace nskry {

struct SettingsPluginItem {
    std::wstring id, name, version, author, status;
    std::wstring description, homepage, repository, updateUrl, loadPolicy, minVersion, maxVersion;
    PluginSource source = PluginSource::Local;
    bool enabled = true;
    bool hasSettings = false;
};

/// Process-boundary-friendly service contract. The window owns no core state.
struct SettingsWindowServices {
    std::function<UserSettings()> getSettings;
    // False: failure text; true with nonempty text: applied with warnings.
    std::function<bool(const UserSettings&, std::wstring&)> applySettings;
    std::function<std::vector<SettingsPluginItem>()> getPlugins;
    std::function<bool(const std::wstring&, HWND)> showPluginSettings;
    std::function<bool(const std::wstring&, bool)> setPluginEnabled;
    std::function<bool(const std::wstring&, std::wstring&)> uninstallPlugin;
    std::function<bool(const std::wstring&, PluginManifest&, std::wstring&)> inspectPluginPackage;
    std::function<PluginPackageResult(const std::wstring&)> installThirdPartyPlugin;
    std::function<bool(UpdateTask::Completion)> checkPluginUpdates;
    std::function<bool(const PluginUpdate&, UpdateTask::Completion, std::wstring&)> downloadPluginUpdate;
    std::function<PluginPackageResult(const PluginUpdate&, const DownloadedUpdate&)> commitPluginUpdate;
    std::function<void()> cancelPluginUpdate;
    std::function<std::wstring()> getOfficialPluginsPageUrl;
    std::function<std::vector<ShortcutDefinition>()> getShortcuts;
    std::function<void(bool)> setRecording;
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
    void RefreshPluginDetails();
    void LayoutControls();
    void OpenOfficialPluginsPage();
    void ShowThirdPartyPolicy();
    void ApplySettings();
    void ToggleSelectedPlugin();
    void UninstallSelectedPlugin();
    void BrowseAndInstallPlugin();
    void InstallThirdPartyPackage(const std::wstring& packagePath);
    void CheckForPluginUpdates();
    void HandleUpdateCheck(UpdateTaskResult result);
    void DownloadNextUpdate();
    void CancelPluginUpdate();
    void SetUpdateBusy(bool busy);
    void SetUpdateNotice(std::wstring notice);
    void RefreshVisualControls();
    void OnVisualControl(int id);
    void Close();

    SettingsWindowServices m_services;
    std::function<void()> m_onClosed;
    HWND m_hwnd{};
    HWND m_tab{};
    HWND m_runAtStartup{}, m_notifications{}, m_theme{};
    HWND m_hideMonitor{};
    HWND m_shadowEnabled{}, m_shadowPreset{}, m_cornersEnabled{}, m_cornerPreset{}, m_cornerMode{};
    HWND m_visualSliders[6]{}, m_visualValues[6]{};
    std::vector<HWND> m_visualControls;
    WindowVisualSettings m_visualDraft;
    std::unique_ptr<InputPage> m_inputPage,m_jumpPage;
    std::unique_ptr<InteractionPage> m_interactionPage;
    HWND m_pluginList{}, m_pluginToggle{}, m_pluginUninstall{}, m_pluginInstall{}, m_pluginCheckUpdates{};
    HWND m_pluginDetails{}, m_officialPlugins{}, m_thirdPartyPolicy{};
    HWND m_pluginSettings{};
    HWND m_aboutText{};
    HWND m_apply{};
    int m_page = 0;
    bool m_closed = false;
    bool m_updateBusy=false;
    std::wstring m_updateNotice,m_checkWarnings;
    std::vector<PluginUpdate> m_updates;
    size_t m_nextUpdate=0;
    std::shared_ptr<int> m_updateLifetime=std::make_shared<int>(0);
    unsigned m_updateGeneration=0;
    std::vector<SettingsPluginItem> m_plugins;
    std::vector<HWND> m_generalDecor, m_hotkeyDecor;
    static inline std::once_flag s_classOnce;
};

} // namespace nskry
