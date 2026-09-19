#include "pch.h"
#include "ui/settings/settings_window.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

namespace nskry {
namespace {
constexpr int IDC_TAB = 100, IDC_RUN = 101, IDC_NOTIFY = 102, IDC_THEME = 103;
constexpr int IDC_CAPTURE = 110, IDC_EXIT = 111, IDC_PLUGIN_LIST = 120, IDC_PLUGIN_TOGGLE = 121;
constexpr int IDC_PLUGIN_UNINSTALL = 122, IDC_PLUGIN_INSTALL = 123, IDC_PLUGIN_CHECK_UPDATES = 124, IDC_APPLY = 130;
constexpr UINT WM_SETTINGS_CLOSED = WM_APP + 42;

void SetText(HWND hwnd, const std::wstring& text) { ::SetWindowTextW(hwnd, text.c_str()); }
std::wstring GetText(HWND hwnd) { wchar_t buf[256]{}; ::GetWindowTextW(hwnd, buf, 256); return buf; }
void ShowControl(HWND hwnd, bool show) { if (hwnd) ::ShowWindow(hwnd, show ? SW_SHOW : SW_HIDE); }
}

SettingsWindow::SettingsWindow(SettingsWindowServices services, std::function<void()> onClosed)
    : m_services(std::move(services)), m_onClosed(std::move(onClosed)) {}
SettingsWindow::~SettingsWindow() { if (m_hwnd) ::DestroyWindow(m_hwnd); }

void SettingsWindow::Show(HWND owner) {
    std::call_once(s_classOnce, [] {
        WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr); wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); wc.lpszClassName = L"NskrySettings";
        ::RegisterClassExW(&wc);
    });
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES };
    ::InitCommonControlsEx(&icc);
    m_hwnd = ::CreateWindowExW(WS_EX_APPWINDOW, L"NskrySettings", L"Nskry Settings",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 720, 500, owner, nullptr,
        ::GetModuleHandleW(nullptr), this);
    ::ShowWindow(m_hwnd, SW_SHOW);
    ::SetForegroundWindow(m_hwnd);
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<SettingsWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = reinterpret_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->m_hwnd = hwnd;
    }
    return self ? self->HandleMessage(msg, wp, lp) : ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SettingsWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: CreateControls(); ::DragAcceptFiles(m_hwnd, TRUE); RefreshFromServices(); return 0;
    case WM_NOTIFY: if (reinterpret_cast<NMHDR*>(lp)->idFrom == IDC_TAB && reinterpret_cast<NMHDR*>(lp)->code == TCN_SELCHANGE) SelectPage(TabCtrl_GetCurSel(m_tab)); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_APPLY) ApplySettings();
        else if (LOWORD(wp) == IDC_PLUGIN_TOGGLE) ToggleSelectedPlugin();
        else if (LOWORD(wp) == IDC_PLUGIN_UNINSTALL) UninstallSelectedPlugin();
        else if (LOWORD(wp) == IDC_PLUGIN_INSTALL) BrowseAndInstallPlugin();
        else if (LOWORD(wp) == IDC_PLUGIN_CHECK_UPDATES) CheckForPluginUpdates();
        return 0;
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wp);
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count != 1) ::MessageBoxW(m_hwnd, L"Drop one .nskryplugin package at a time.", L"Nskry Settings", MB_ICONINFORMATION);
        else {
            const UINT length = ::DragQueryFileW(drop, 0, nullptr, 0);
            std::vector<wchar_t> path(length + 1);
            ::DragQueryFileW(drop, 0, path.data(), static_cast<UINT>(path.size()));
            InstallThirdPartyPackage(path.data());
        }
        ::DragFinish(drop);
        return 0;
    }
    case WM_CLOSE: Close(); return 0;
    case WM_DESTROY: m_hwnd = nullptr; if (!m_closed) Close(); return 0;
    }
    return ::DefWindowProcW(m_hwnd, msg, wp, lp);
}

void SettingsWindow::CreateControls() {
    m_tab = ::CreateWindowExW(0, WC_TABCONTROLW, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        12, 12, 680, 390, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TAB)), nullptr, nullptr);
    const wchar_t* tabs[] = { L"General", L"Hotkeys", L"Plugins", L"About" };
    for (const auto* tab : tabs) { TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(tab); TabCtrl_InsertItem(m_tab, TabCtrl_GetItemCount(m_tab), &item); }
    auto control = [&](DWORD style, const wchar_t* text, int id, int x, int y, int w, int h) {
        return ::CreateWindowExW(0, L"BUTTON", text, WS_CHILD | style, x, y, w, h, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr); };
    m_generalDecor.push_back(control(WS_VISIBLE | BS_GROUPBOX, L"General", 0, 32, 55, 630, 190));
    m_runAtStartup = control(WS_VISIBLE | BS_AUTOCHECKBOX, L"Start Nskry when I sign in", IDC_RUN, 52, 90, 300, 24);
    m_notifications = control(WS_VISIBLE | BS_AUTOCHECKBOX, L"Show notifications", IDC_NOTIFY, 52, 120, 300, 24);
    m_generalDecor.push_back(::CreateWindowExW(0, L"STATIC", L"Theme:", WS_CHILD | WS_VISIBLE, 52, 158, 90, 22, m_hwnd, nullptr, nullptr, nullptr));
    m_theme = ::CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 145, 154, 150, 150, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_THEME)), nullptr, nullptr);
    ::SendMessageW(m_theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"System")); ::SendMessageW(m_theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Light")); ::SendMessageW(m_theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dark"));
    m_hotkeyDecor.push_back(::CreateWindowExW(0, L"STATIC", L"Capture shortcut:", WS_CHILD, 52, 80, 140, 22, m_hwnd, nullptr, nullptr, nullptr));
    m_captureHotkey = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | ES_AUTOHSCROLL, 205, 76, 210, 25, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CAPTURE)), nullptr, nullptr);
    m_hotkeyDecor.push_back(::CreateWindowExW(0, L"STATIC", L"Exit shortcut:", WS_CHILD, 52, 122, 140, 22, m_hwnd, nullptr, nullptr, nullptr));
    m_exitHotkey = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | ES_AUTOHSCROLL, 205, 118, 210, 25, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXIT)), nullptr, nullptr);
    m_pluginList = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr, WS_CHILD | LBS_NOTIFY | WS_VSCROLL, 32, 58, 630, 240, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PLUGIN_LIST)), nullptr, nullptr);
    m_pluginToggle = control(BS_PUSHBUTTON, L"Enable / Disable", IDC_PLUGIN_TOGGLE, 32, 315, 130, 28);
    m_pluginUninstall = control(BS_PUSHBUTTON, L"Uninstall", IDC_PLUGIN_UNINSTALL, 175, 315, 100, 28);
    m_pluginInstall = control(BS_PUSHBUTTON, L"Install third-party plugin...", IDC_PLUGIN_INSTALL, 288, 315, 190, 28);
    m_pluginCheckUpdates = control(BS_PUSHBUTTON, L"Check for updates", IDC_PLUGIN_CHECK_UPDATES, 492, 315, 170, 28);
    m_aboutText = ::CreateWindowExW(0, L"STATIC", L"Nskry\n\nLightweight Windows capture and PiP utility.\n\nVersion 0.5.0\nPlugin API version 2", WS_CHILD | SS_LEFT,
        52, 72, 520, 180, m_hwnd, nullptr, nullptr, nullptr);
    m_apply = control(WS_VISIBLE | BS_DEFPUSHBUTTON, L"Apply", IDC_APPLY, 580, 420, 110, 30);
    SelectPage(0);
}

void SettingsWindow::SelectPage(int page) {
    m_page = page; if (m_tab) TabCtrl_SetCurSel(m_tab, page);
    const bool general = page == 0, hotkeys = page == 1, plugins = page == 2, about = page == 3;
    ShowControl(m_runAtStartup, general); ShowControl(m_notifications, general); ShowControl(m_theme, general);
    ShowControl(m_captureHotkey, hotkeys); ShowControl(m_exitHotkey, hotkeys);
    ShowControl(m_pluginList, plugins); ShowControl(m_pluginToggle, plugins); ShowControl(m_pluginUninstall, plugins); ShowControl(m_pluginInstall, plugins); ShowControl(m_pluginCheckUpdates, plugins);
    ShowControl(m_apply, general || hotkeys); if (plugins) RefreshPluginList();
    for (HWND hwnd : m_generalDecor) ShowControl(hwnd, general);
    for (HWND hwnd : m_hotkeyDecor) ShowControl(hwnd, hotkeys);
    ShowControl(m_aboutText, about);
}

void SettingsWindow::RefreshFromServices() {
    if (!m_services.getSettings) return; const UserSettings settings = m_services.getSettings();
    ::SendMessageW(m_runAtStartup, BM_SETCHECK, settings.runAtStartup ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(m_notifications, BM_SETCHECK, settings.notifications ? BST_CHECKED : BST_UNCHECKED, 0);
    SetText(m_captureHotkey, settings.captureShortcut); SetText(m_exitHotkey, settings.exitShortcut);
    const int selection = settings.theme == L"light" ? 1 : settings.theme == L"dark" ? 2 : 0; ::SendMessageW(m_theme, CB_SETCURSEL, selection, 0);
}

void SettingsWindow::RefreshPluginList() {
    ::SendMessageW(m_pluginList, LB_RESETCONTENT, 0, 0); m_plugins = m_services.getPlugins ? m_services.getPlugins() : std::vector<SettingsPluginItem>{};
    for (const auto& p : m_plugins) { const std::wstring text = p.name + L"  v" + p.version + L"  — " + p.status; ::SendMessageW(m_pluginList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())); }
}

void SettingsWindow::ApplySettings() {
    if (!m_services.getSettings || !m_services.applySettings) return; UserSettings settings = m_services.getSettings();
    settings.runAtStartup = ::SendMessageW(m_runAtStartup, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.notifications = ::SendMessageW(m_notifications, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.captureShortcut = GetText(m_captureHotkey); settings.exitShortcut = GetText(m_exitHotkey);
    const int theme = static_cast<int>(::SendMessageW(m_theme, CB_GETCURSEL, 0, 0)); settings.theme = theme == 1 ? L"light" : theme == 2 ? L"dark" : L"system";
    std::wstring error; if (!m_services.applySettings(settings, error)) ::MessageBoxW(m_hwnd, error.c_str(), L"Nskry Settings", MB_ICONERROR);
    else ::MessageBoxW(m_hwnd, L"Settings applied.", L"Nskry Settings", MB_ICONINFORMATION);
}

void SettingsWindow::ToggleSelectedPlugin() { const int i = static_cast<int>(::SendMessageW(m_pluginList, LB_GETCURSEL, 0, 0)); if (i >= 0 && i < static_cast<int>(m_plugins.size()) && m_services.setPluginEnabled) { m_services.setPluginEnabled(m_plugins[i].id, !m_plugins[i].enabled); RefreshPluginList(); } }
void SettingsWindow::UninstallSelectedPlugin() { const int i = static_cast<int>(::SendMessageW(m_pluginList, LB_GETCURSEL, 0, 0)); if (i < 0 || i >= static_cast<int>(m_plugins.size()) || !m_services.uninstallPlugin) return; if (::MessageBoxW(m_hwnd, L"Remove this plugin when Nskry restarts?", L"Nskry Settings", MB_YESNO | MB_ICONWARNING) == IDYES) { std::wstring error; if (!m_services.uninstallPlugin(m_plugins[i].id, error)) ::MessageBoxW(m_hwnd, error.c_str(), L"Nskry Settings", MB_ICONERROR); RefreshPluginList(); } }

void SettingsWindow::BrowseAndInstallPlugin() {
    std::vector<wchar_t> path(32768);
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = m_hwnd;
    dialog.lpstrFilter = L"Nskry plugin packages (*.nskryplugin)\0*.nskryplugin\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = path.data(); dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (::GetOpenFileNameW(&dialog)) InstallThirdPartyPackage(path.data());
}

void SettingsWindow::InstallThirdPartyPackage(const std::wstring& packagePath) {
    if (packagePath.size() < 13 || _wcsicmp(packagePath.c_str() + packagePath.size() - 13, L".nskryplugin") != 0) {
        ::MessageBoxW(m_hwnd, L"Choose a .nskryplugin package.", L"Nskry Settings", MB_ICONERROR);
        return;
    }
    if (!m_services.inspectPluginPackage || !m_services.installThirdPartyPlugin) return;
    PluginManifest manifest;
    std::wstring error;
    if (!m_services.inspectPluginPackage(packagePath, manifest, error)) {
        ::MessageBoxW(m_hwnd, error.c_str(), L"Plugin package rejected", MB_ICONERROR);
        return;
    }
    const std::wstring source = !manifest.homepage.empty() ? manifest.homepage :
        (!manifest.repository.empty() ? manifest.repository : L"Local package selected by you");
    const std::wstring warning = L"Third-party native plugin\n\nAuthor: " + manifest.author +
        L"\nSource: " + source + L"\nVersion: " + manifest.version +
        L"\n\nThis plugin contains native code and will run with the same permissions as Nskry.\n\nInstall '" + manifest.name + L"'?";
    if (::MessageBoxW(m_hwnd, warning.c_str(), L"Install third-party plugin?", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    const PluginPackageResult result = m_services.installThirdPartyPlugin(packagePath);
    ::MessageBoxW(m_hwnd, result.message.c_str(), result.success ? L"Nskry Settings" : L"Plugin installation failed",
        result.success ? MB_ICONINFORMATION : MB_ICONERROR);
    if (result.success) RefreshPluginList();
}
void SettingsWindow::CheckForPluginUpdates() {
    if (!m_services.checkPluginUpdates || !m_services.downloadPluginUpdate) return;
    std::wstring error;
    const std::vector<PluginUpdate> updates = m_services.checkPluginUpdates(error);
    if (updates.empty()) {
        const std::wstring message = error.empty() ? L"All installed plugins are up to date." : L"No updates were found.\n\n" + error;
        ::MessageBoxW(m_hwnd, message.c_str(), L"Plugin updates", error.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
        return;
    }
    for (const PluginUpdate& update : updates) {
        const std::wstring prompt = update.name + L"  " + update.currentVersion + L" → " + update.version +
            (update.releaseNotes.empty() ? L"" : L"\n\n" + update.releaseNotes) + L"\n\nDownload and stage this update for the next restart?";
        if (::MessageBoxW(m_hwnd, prompt.c_str(), L"Plugin update available", MB_YESNO | MB_ICONQUESTION) != IDYES) continue;
        const PluginPackageResult result = m_services.downloadPluginUpdate(update);
        ::MessageBoxW(m_hwnd, result.message.c_str(), result.success ? L"Plugin update" : L"Plugin update failed", result.success ? MB_ICONINFORMATION : MB_ICONERROR);
    }
    RefreshPluginList();
}
void SettingsWindow::Close() { if (m_closed) return; m_closed = true; if (m_hwnd) ::DestroyWindow(m_hwnd); if (m_onClosed) m_onClosed(); }

} // namespace nskry
