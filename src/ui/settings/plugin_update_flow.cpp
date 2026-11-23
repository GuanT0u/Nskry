#include "pch.h"
#include "ui/ui_preferences.h"
#include "ui/settings/settings_window.h"
namespace nskry {
namespace {
void SetText(HWND w, const std::wstring &text) {
    SetWindowTextW(w, text.c_str());
}
} // namespace
void SettingsWindow::CheckForPluginUpdates() {
    if (m_updateBusy) {
        CancelPluginUpdate();
        return;
    }
    if (!m_services.checkPluginUpdates)
        return;
    m_checkWarnings.clear();
    SetUpdateBusy(true);
    const auto generation = ++m_updateGeneration;
    const std::weak_ptr<int> lifetime = m_updateLifetime;
    if (!m_services.checkPluginUpdates([this, lifetime, generation](UpdateTaskResult result) {
            if (!lifetime.expired() && generation == m_updateGeneration)
                HandleUpdateCheck(std::move(result));
        })) {
        SetUpdateBusy(false);
        SetUpdateNotice(
            UiText(L"Cannot start update task; a previous cancellation may still be completing. Try again shortly.", L"无法开始更新。上一项任务可能仍在取消，请稍后重试。"));
    }
}
void SettingsWindow::SetUpdateBusy(bool busy) {
    m_updateBusy = busy;
    if (!m_hwnd)
        return;
    SetText(m_pluginCheckUpdates, busy ? UiText(L"Cancel update", L"取消更新") : UiText(L"Check for updates", L"检查更新"));
    for (HWND button : {m_pluginToggle, m_pluginUninstall, m_pluginInstall})
        EnableWindow(button, !busy);
    if (busy)
        SetUpdateNotice(
            UiText(L"Update task in progress. Capture and monitoring remain available. Click Cancel update to stop.", L"正在检查或准备更新。截图和画中画仍可用，可选择「取消更新」停止。") +
            (m_checkWarnings.empty() ? L"" : L"\r\nCheck warnings:\r\n" + m_checkWarnings));
}
void SettingsWindow::SetUpdateNotice(std::wstring notice) {
    m_updateNotice = std::move(notice);
    if (m_hwnd)
        RefreshPluginDetails();
}
void SettingsWindow::CancelPluginUpdate() {
    ++m_updateGeneration;
    const bool wasBusy = m_updateBusy;
    if (m_updateBusy && m_services.cancelPluginUpdate)
        m_services.cancelPluginUpdate();
    m_updates.clear();
    m_nextUpdate = 0;
    SetUpdateBusy(false);
    if (wasBusy && !m_closed && m_hwnd)
        SetUpdateNotice(UiText(L"Update cancelled. Temporary resource cleanup may still be completing.", L"已取消更新，可能仍在清理临时文件。"));
}
void SettingsWindow::HandleUpdateCheck(UpdateTaskResult result) {
    SetUpdateBusy(false);
    if (m_closed || result.cancelled)
        return;
    if (result.updates.empty()) {
        SetUpdateNotice(result.message.empty() ? UiText(L"No compatible newer versions were reported.", L"未找到兼容的新版本。") : result.message);
        return;
    }
    if (!result.message.empty())
        SetUpdateNotice(L"Some sources were not checked:\n" + result.message);
    m_checkWarnings = std::move(result.message);
    m_updates = std::move(result.updates);
    m_nextUpdate = 0;
    DownloadNextUpdate();
}
void SettingsWindow::DownloadNextUpdate() {
    const std::weak_ptr<int> lifetime = m_updateLifetime;
    while (m_nextUpdate < m_updates.size()) {
        const PluginUpdate update = m_updates[m_nextUpdate++];
        const std::wstring prompt =
            update.name + L"  " + update.currentVersion + L" → " + update.version + L"\nID: " + update.pluginId +
            L"\nSource: " +
            (update.source == PluginSource::Official ? UiText(L"Official catalog (publisher identity not yet verified)", L"官方目录（尚未验证发布者身份）")
                                                     : UiText(L"Publisher's feed (not an official certification)", L"发布者的更新源（未经官方认证）")) +
            L"\nPackage: " + update.packageUrl + (update.releaseNotes.empty() ? L"" : L"\n\n" + update.releaseNotes) +
            UiText(L"\n\nDownload and stage this update for the next restart?", L"\n\n下载更新并在下次重启时应用吗？") +
            (m_checkWarnings.empty() ? L"" : L"\n\nOther sources were not checked. See plugin details for warnings.");
        const auto answer =
            MessageBoxW(m_hwnd, prompt.c_str(), UiText(L"Plugin update available", L"发现插件更新"), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (lifetime.expired())
            return;
        if (answer != IDYES)
            continue;
        if (!m_services.downloadPluginUpdate)
            return;
        SetUpdateBusy(true);
        const auto generation = ++m_updateGeneration;
        std::wstring error;
        if (!m_services.downloadPluginUpdate(
                update,
                [this, lifetime, generation, update](UpdateTaskResult result) {
                    if (lifetime.expired() || generation != m_updateGeneration)
                        return;
                    if (result.cancelled) {
                        SetUpdateBusy(false);
                        return;
                    }
                    PluginPackageResult committed;
                    if (result.download && m_services.commitPluginUpdate) {
                        try {
                            committed = m_services.commitPluginUpdate(update, *result.download);
                        } catch (...) {
                            committed.message = UiText(L"Unable to commit the prepared update.", L"无法提交已准备的更新。");
                        }
                    } else
                        committed.message = result.message;
                    result.download.reset();
                    SetUpdateBusy(false);
                    RefreshPluginList();
                    SetUpdateNotice(committed.message +
                                    (m_checkWarnings.empty() ? L"" : L"\r\nCheck warnings:\r\n" + m_checkWarnings));
                    DownloadNextUpdate();
                },
                error)) {
            SetUpdateBusy(false);
            SetUpdateNotice(error.empty() ? UiText(L"Update task unavailable; try again shortly.", L"更新服务暂不可用，请稍后重试。") : error);
        }
        return;
    }
    m_updates.clear();
}
} // namespace nskry
