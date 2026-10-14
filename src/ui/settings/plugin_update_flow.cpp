#include "pch.h"
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
            L"Cannot start update task; a previous cancellation may still be completing. Try again shortly.");
    }
}
void SettingsWindow::SetUpdateBusy(bool busy) {
    m_updateBusy = busy;
    if (!m_hwnd)
        return;
    SetText(m_pluginCheckUpdates, busy ? L"Cancel update" : L"Check for updates");
    for (HWND button : {m_pluginToggle, m_pluginUninstall, m_pluginInstall})
        EnableWindow(button, !busy);
    if (busy)
        SetUpdateNotice(
            L"Update task in progress. Capture and monitoring remain available. Click Cancel update to stop." +
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
        SetUpdateNotice(L"Update cancelled. Temporary resource cleanup may still be completing.");
}
void SettingsWindow::HandleUpdateCheck(UpdateTaskResult result) {
    SetUpdateBusy(false);
    if (m_closed || result.cancelled)
        return;
    if (result.updates.empty()) {
        SetUpdateNotice(result.message.empty() ? L"No compatible newer versions were reported." : result.message);
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
            (update.source == PluginSource::Official ? L"Official catalog (publisher identity not yet verified)"
                                                     : L"Publisher's feed (not an official certification)") +
            L"\nPackage: " + update.packageUrl + (update.releaseNotes.empty() ? L"" : L"\n\n" + update.releaseNotes) +
            L"\n\nDownload and stage this update for the next restart?" +
            (m_checkWarnings.empty() ? L"" : L"\n\nOther sources were not checked. See plugin details for warnings.");
        const auto answer =
            MessageBoxW(m_hwnd, prompt.c_str(), L"Plugin update available", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
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
                            committed.message = L"Unable to commit the prepared update.";
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
            SetUpdateNotice(error.empty() ? L"Update task unavailable; try again shortly." : error);
        }
        return;
    }
    m_updates.clear();
}
} // namespace nskry
