#include "pch.h"
#include "ui/settings/settings_window.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string_view>

namespace nskry {
namespace {
constexpr int IDC_TAB = 100, IDC_RUN = 101, IDC_NOTIFY = 102, IDC_THEME = 103;
constexpr int IDC_PLUGIN_LIST = 120, IDC_PLUGIN_TOGGLE = 121;
constexpr int IDC_PLUGIN_UNINSTALL = 122, IDC_PLUGIN_INSTALL = 123, IDC_PLUGIN_CHECK_UPDATES = 124, IDC_APPLY = 130;
constexpr int IDC_PLUGIN_DETAILS = 125, IDC_OFFICIAL_PLUGINS = 126, IDC_PLUGIN_POLICY = 127;
constexpr const wchar_t* kThirdPartyPolicy =
    L"Official plugins are developed by Nskry's maintainer or explicitly adopted/certified developers or teams.\n\n"
    L"Other plugins are independent third-party software. Listing, installation, HTTPS or a hash does not certify their safety. "
    L"Native DLL plugins run with the same user permissions as Nskry; they are not sandboxed. They may access files, network and captures.\n\n"
    L"Check the publisher, source code, license and privacy policy before installing. The publisher is responsible for its plugin. "
    L"To the extent permitted by applicable law, Nskry's maintainers provide no warranty for third-party plugins. "
    L"This notice does not waive statutory rights, override GPL rights, or exclude liability that cannot legally be excluded.\n\n"
    L"An 'Official source' label currently records installation provenance, not a cryptographically verified publisher identity.";

void SetText(HWND hwnd, const std::wstring& text) { ::SetWindowTextW(hwnd, text.c_str()); }
void ShowControl(HWND hwnd, bool show) { if (hwnd) ::ShowWindow(hwnd, show ? SW_SHOW : SW_HIDE); }
}

SettingsWindow::SettingsWindow(SettingsWindowServices services, std::function<void()> onClosed)
    : m_services(std::move(services)), m_onClosed(std::move(onClosed)) {}
SettingsWindow::~SettingsWindow() { m_updateLifetime.reset();CancelPluginUpdate();m_inputPage.reset();m_jumpPage.reset();m_interactionPage.reset();if (m_hwnd) ::DestroyWindow(m_hwnd); }

void SettingsWindow::Show(HWND owner) {
    std::call_once(s_classOnce, [] {
        WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr); wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); wc.lpszClassName = L"NskrySettings";
        ::RegisterClassExW(&wc);
    });
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    ::InitCommonControlsEx(&icc);
    m_hwnd = ::CreateWindowExW(WS_EX_APPWINDOW, L"NskrySettings", L"Nskry Settings",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 720, 590, owner, nullptr,
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
    case WM_ACTIVATEAPP:if(!wp && m_inputPage)m_inputPage->StopRecording();break;
    case WM_CREATE: CreateControls(); ::DragAcceptFiles(m_hwnd, TRUE); RefreshFromServices(); return 0;
    case WM_NOTIFY: if (reinterpret_cast<NMHDR*>(lp)->idFrom == IDC_TAB && reinterpret_cast<NMHDR*>(lp)->code == TCN_SELCHANGE) SelectPage(TabCtrl_GetCurSel(m_tab)); return 0;
    case WM_COMMAND:
        if(m_updateBusy && (LOWORD(wp)==IDC_PLUGIN_TOGGLE || LOWORD(wp)==IDC_PLUGIN_UNINSTALL || LOWORD(wp)==IDC_PLUGIN_INSTALL))return 0;
        if (LOWORD(wp) >= 140 && LOWORD(wp) <= 154) { OnVisualControl(LOWORD(wp)); return 0; }
        if (LOWORD(wp) == IDC_APPLY) ApplySettings();
        else if (LOWORD(wp) == IDC_PLUGIN_TOGGLE) ToggleSelectedPlugin();
        else if (LOWORD(wp) == IDC_PLUGIN_UNINSTALL) UninstallSelectedPlugin();
        else if (LOWORD(wp) == IDC_PLUGIN_INSTALL) BrowseAndInstallPlugin();
        else if (LOWORD(wp) == IDC_PLUGIN_CHECK_UPDATES) CheckForPluginUpdates();
        else if (LOWORD(wp) == IDC_OFFICIAL_PLUGINS) OpenOfficialPluginsPage();
        else if (LOWORD(wp) == IDC_PLUGIN_POLICY) ShowThirdPartyPolicy();
        else if (LOWORD(wp) == IDC_PLUGIN_LIST && HIWORD(wp) == LBN_SELCHANGE) RefreshPluginDetails();
        return 0;
    case WM_SIZE: LayoutControls(); return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {720, 590}; return 0;
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
    case WM_HSCROLL:
        for (int i = 0; i < 6; ++i) if (reinterpret_cast<HWND>(lp) == m_visualSliders[i]) { OnVisualControl(149 + i); break; }
        return 0;
    case WM_DESTROY: m_hwnd = nullptr; if (!m_closed) Close(); return 0;
    }
    return ::DefWindowProcW(m_hwnd, msg, wp, lp);
}

void SettingsWindow::CreateControls() {
    m_tab = ::CreateWindowExW(0, WC_TABCONTROLW, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        12, 12, 680, 390, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TAB)), nullptr, nullptr);
    const wchar_t* tabs[] = { L"General", L"Hotkeys", L"Plugins", L"About", L"Window effects", L"Input appearance" };
    for (const auto* tab : tabs) { TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(tab); TabCtrl_InsertItem(m_tab, TabCtrl_GetItemCount(m_tab), &item); }
    auto control = [&](DWORD style, const wchar_t* text, int id, int x, int y, int w, int h) {
        return ::CreateWindowExW(0, L"BUTTON", text, WS_CHILD | style, x, y, w, h, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr); };
    m_generalDecor.push_back(control(WS_VISIBLE | BS_GROUPBOX, L"General", 0, 32, 55, 630, 190));
    m_runAtStartup = control(WS_VISIBLE | BS_AUTOCHECKBOX, L"Start Nskry when I sign in", IDC_RUN, 52, 90, 300, 24);
    m_notifications = control(WS_VISIBLE | BS_AUTOCHECKBOX, L"Show notifications", IDC_NOTIFY, 52, 120, 300, 24);
    m_hideMonitor = control(WS_VISIBLE | BS_AUTOCHECKBOX, L"Hide monitor while its source window is focused", 104, 52, 194, 500, 24);
    m_generalDecor.push_back(::CreateWindowExW(0, L"STATIC", L"Theme:", WS_CHILD | WS_VISIBLE, 52, 158, 90, 22, m_hwnd, nullptr, nullptr, nullptr));
    m_theme = ::CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 145, 154, 150, 150, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_THEME)), nullptr, nullptr);
    ::SendMessageW(m_theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"System")); ::SendMessageW(m_theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Light")); ::SendMessageW(m_theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dark"));
    auto definitions=m_services.getShortcuts?m_services.getShortcuts():CoreShortcuts();
    m_hotkeyDecor.push_back(::CreateWindowExW(0,L"STATIC",
        L"Record until all keys are released; Type uses names such as Ctrl+Alt+A. Hold rows accept Alt/Space; wheel rows accept Shift+Wheel. Windows-reserved combinations are not intercepted.",
        WS_CHILD,32,58,630,44,m_hwnd,nullptr,nullptr,nullptr));
    m_inputPage=std::make_unique<InputPage>(m_hwnd,definitions,false,m_services.setRecording,nullptr);
    m_jumpPage=std::make_unique<InputPage>(m_hwnd,definitions,true,nullptr,[this](const std::wstring& id){SelectPage(1);m_inputPage->Jump(id);});
    m_interactionPage=std::make_unique<InteractionPage>(m_hwnd);
    m_pluginList = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr, WS_CHILD | LBS_NOTIFY | WS_VSCROLL, 32, 58, 630, 240, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PLUGIN_LIST)), nullptr, nullptr);
    m_pluginToggle = control(BS_PUSHBUTTON, L"Enable / Disable", IDC_PLUGIN_TOGGLE, 32, 315, 130, 28);
    m_pluginUninstall = control(BS_PUSHBUTTON, L"Uninstall", IDC_PLUGIN_UNINSTALL, 175, 315, 100, 28);
    m_pluginInstall = control(BS_PUSHBUTTON, L"Install third-party plugin...", IDC_PLUGIN_INSTALL, 288, 315, 190, 28);
    m_pluginCheckUpdates = control(BS_PUSHBUTTON, L"Check for updates", IDC_PLUGIN_CHECK_UPDATES, 492, 315, 170, 28);
    m_pluginDetails = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"Select a plugin to view its information.",
        WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        32, 250, 630, 130, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PLUGIN_DETAILS)), nullptr, nullptr);
    m_officialPlugins = control(BS_PUSHBUTTON, L"Browse official plugins...", IDC_OFFICIAL_PLUGINS, 32, 420, 210, 28);
    m_thirdPartyPolicy = control(BS_PUSHBUTTON, L"Third-party plugin policy...", IDC_PLUGIN_POLICY, 255, 420, 220, 28);
    m_aboutText = ::CreateWindowExW(0, L"STATIC", L"Nskry\n\nLightweight Windows capture and PiP utility.\n\nVersion 0.5.0\nPlugin API version 2", WS_CHILD | SS_LEFT,
        52, 72, 520, 180, m_hwnd, nullptr, nullptr, nullptr);
    m_apply = control(WS_VISIBLE | BS_DEFPUSHBUTTON, L"Apply", IDC_APPLY, 580, 420, 110, 30);
    auto visual = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
        HWND item = ::CreateWindowExW(0, type, text, WS_CHILD | style, x, y, w, h, m_hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        m_visualControls.push_back(item); return item;
    };
    auto preset = [&](int id, int y) {
        HWND combo = visual(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, id, 340, y, 230, 140);
        for (const auto* text : {L"Win11 style", L"macOS style (approx.)", L"Custom"})
            ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        return combo;
    };
    m_shadowEnabled = visual(L"BUTTON", L"Shadow", BS_AUTOCHECKBOX, 140, 50, 64, 200, 24);
    m_shadowPreset = preset(141, 62);
    m_cornersEnabled = visual(L"BUTTON", L"Rounded corners", BS_AUTOCHECKBOX, 142, 50, 251, 250, 24);
    m_cornerPreset = preset(143, 249);
    m_cornerMode = visual(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 144, 340, 285, 230, 140);
    for (const auto* text : {L"Win11 system preference", L"Fixed radius (DIP)", L"Percentage of short edge"})
        ::SendMessageW(m_cornerMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    visual(L"STATIC", L"Corner size rule:", 0, 0, 50, 289, 240, 24);
    const wchar_t* labels[] = {L"Opacity (%)", L"Blur sigma (DIP)", L"Range (DIP)", L"Offset Y (DIP)", L"Radius (DIP; system ref.)", L"Short-edge percentage"};
    const int maximum[] = {50, 8, 24, 6, 24, 15};
    for (int i = 0; i < 6; ++i) {
        const int y = i < 4 ? 100 + i * 35 : 324 + (i - 4) * 35;
        visual(L"STATIC", labels[i], 0, 0, 50, y + 4, 200, 24);
        m_visualSliders[i] = visual(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 149 + i, 255, y, 285, 30);
        ::SendMessageW(m_visualSliders[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, maximum[i]));
        m_visualValues[i] = visual(L"STATIC", L"", 0, 0, 556, y + 4, 60, 24);
    }
    visual(L"STATIC", L"Effects are display-only; Copy/Save/OCR keep original pixels.\nWin11 radius is a system preference, not an exact editable value.\nChanges apply to existing and future windows only after Apply.",
           0, 0, 50, 404, 600, 62);
    SelectPage(0);
    LayoutControls();
}

void SettingsWindow::LayoutControls() {
    if (!m_tab) return;
    RECT client{}; ::GetClientRect(m_hwnd, &client);
    const auto move = [](HWND control, int x, int y, int w, int h) {
        if (control) ::SetWindowPos(control, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    const int width = (std::max)(630, static_cast<int>(client.right) - 64);
    const int listHeight = (std::max)(100, static_cast<int>(client.bottom) - 358);
    const int detailsY = 58 + listHeight + 8, buttonsY = detailsY + 150;
    move(m_tab, 12, 12, client.right - 24, client.bottom - 82);
    move(m_pluginList, 32, 58, width, listHeight);
    move(m_pluginDetails, 32, detailsY, width, 140);
    move(m_pluginToggle, 32, buttonsY, 130, 28);
    move(m_pluginUninstall, 175, buttonsY, 100, 28);
    move(m_pluginInstall, 288, buttonsY, 190, 28);
    move(m_pluginCheckUpdates, 492, buttonsY, 170, 28);
    move(m_officialPlugins, 32, buttonsY + 38, 210, 28);
    move(m_thirdPartyPolicy, 255, buttonsY + 38, 220, 28);
    move(m_apply, client.right - 134, client.bottom - 54, 110, 30);
    if(m_inputPage)move(m_inputPage->Hwnd(),32,108,width,client.bottom-178);
    if(m_jumpPage)move(m_jumpPage->Hwnd(),32,258,width,client.bottom-328);
    if(m_interactionPage)move(m_interactionPage->Hwnd(),32,58,width,client.bottom-128);
}

void SettingsWindow::SelectPage(int page) {
    m_page = page; if (m_tab) TabCtrl_SetCurSel(m_tab, page);
    const bool general = page == 0, hotkeys = page == 1, plugins = page == 2, about = page == 3;
    ShowControl(m_runAtStartup, general); ShowControl(m_notifications, general); ShowControl(m_theme, general);
    ShowControl(m_hideMonitor, general);
    if(m_inputPage)ShowControl(m_inputPage->Hwnd(),hotkeys);
    if(m_jumpPage)ShowControl(m_jumpPage->Hwnd(),general);
    if(m_interactionPage)ShowControl(m_interactionPage->Hwnd(),page==5);
    ShowControl(m_pluginList, plugins); ShowControl(m_pluginToggle, plugins); ShowControl(m_pluginUninstall, plugins); ShowControl(m_pluginInstall, plugins); ShowControl(m_pluginCheckUpdates, plugins);
    ShowControl(m_apply, general || hotkeys || page == 4 || page==5); if (plugins) RefreshPluginList();
    for (auto control : m_visualControls) ShowControl(control, page == 4);
    ShowControl(m_pluginDetails, plugins); ShowControl(m_officialPlugins, plugins); ShowControl(m_thirdPartyPolicy, plugins);
    for (HWND hwnd : m_generalDecor) ShowControl(hwnd, general);
    for (HWND hwnd : m_hotkeyDecor) ShowControl(hwnd, hotkeys);
    ShowControl(m_aboutText, about);
}

void SettingsWindow::RefreshFromServices() {
    if (!m_services.getSettings) return; const UserSettings settings = m_services.getSettings();
    ::SendMessageW(m_runAtStartup, BM_SETCHECK, settings.runAtStartup ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(m_notifications, BM_SETCHECK, settings.notifications ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(m_hideMonitor, BM_SETCHECK, settings.hideMonitorOnSourceFocus ? BST_CHECKED : BST_UNCHECKED, 0);
    if(m_inputPage)m_inputPage->Load(settings);
    if(m_jumpPage)m_jumpPage->Load(settings);
    if(m_interactionPage)m_interactionPage->Load(settings.interaction);
    const int selection = settings.theme == L"light" ? 1 : settings.theme == L"dark" ? 2 : 0; ::SendMessageW(m_theme, CB_SETCURSEL, selection, 0);
    m_visualDraft = settings.visuals; RefreshVisualControls();
}

void SettingsWindow::RefreshPluginList() {
    const int oldSelection = static_cast<int>(::SendMessageW(m_pluginList, LB_GETCURSEL, 0, 0));
    const std::wstring selectedId = oldSelection >= 0 && oldSelection < static_cast<int>(m_plugins.size()) ? m_plugins[oldSelection].id : L"";
    ::SendMessageW(m_pluginList, LB_RESETCONTENT, 0, 0); m_plugins = m_services.getPlugins ? m_services.getPlugins() : std::vector<SettingsPluginItem>{};
    for (const auto& p : m_plugins) { const std::wstring text = p.name + L"  v" + p.version + L"  — " + p.status; ::SendMessageW(m_pluginList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())); }
    int selection = m_plugins.empty() ? -1 : 0;
    for (size_t i = 0; i < m_plugins.size(); ++i) if (m_plugins[i].id == selectedId) selection = static_cast<int>(i);
    ::SendMessageW(m_pluginList, LB_SETCURSEL, selection, 0);
    RefreshPluginDetails();
}

void SettingsWindow::RefreshPluginDetails() {
    const int selected = static_cast<int>(::SendMessageW(m_pluginList, LB_GETCURSEL, 0, 0));
    const bool valid = selected >= 0 && selected < static_cast<int>(m_plugins.size());
    ::EnableWindow(m_pluginToggle, valid && !m_updateBusy); ::EnableWindow(m_pluginUninstall, valid && !m_updateBusy);
    const auto notice=m_updateNotice.empty()?L"":m_updateNotice+L"\r\n\r\n";
    if (!valid) { SetText(m_pluginDetails, notice+L"No plugin selected. Use Browse official plugins or install a local package."); return; }
    const auto& plugin = m_plugins[selected];
    const wchar_t* source = plugin.source == PluginSource::Official ? L"Official source (identity not verified)" :
        plugin.source == PluginSource::ThirdParty ? L"Third-party" : plugin.source == PluginSource::Legacy ? L"Legacy" : L"Local / unverified";
    SetText(m_pluginDetails, notice+plugin.name + L"  v" + plugin.version + L"\r\n" + plugin.description +
        L"\r\nID: " + plugin.id + L"  |  Author: " + plugin.author + L"\r\nSource: " + source +
        L"\r\nState: " + plugin.status + L"  |  Load policy: " + plugin.loadPolicy +
        L"\r\nHost compatibility: " + (plugin.minVersion.empty() ? L"unspecified" : plugin.minVersion) +
        L" — " + (plugin.maxVersion.empty() ? L"no maximum" : plugin.maxVersion) +
        L"\r\nHomepage: " + plugin.homepage + L"\r\nRepository: " + plugin.repository +
        L"\r\nUpdate feed: " + (plugin.updateUrl.empty() ? L"not configured" : plugin.updateUrl));
}

void SettingsWindow::OpenOfficialPluginsPage() {
    const std::wstring url = m_services.getOfficialPluginsPageUrl ? m_services.getOfficialPluginsPageUrl() : L"";
    if (url.empty()) {
        ::MessageBoxW(m_hwnd, L"The official plugin website is not online/configured yet.\n\nNo network request was made.",
            L"Official plugins", MB_OK | MB_ICONINFORMATION); return;
    }
    if (!IsPluginHttpsUrl(url)) {
        ::MessageBoxW(m_hwnd, L"The official plugin page must be a valid HTTPS URL.", L"Official plugins", MB_OK | MB_ICONERROR); return;
    }
    if (reinterpret_cast<INT_PTR>(::ShellExecuteW(m_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        ::MessageBoxW(m_hwnd, L"Could not open the plugin website in your browser.", L"Official plugins", MB_OK | MB_ICONERROR);
}
void SettingsWindow::ShowThirdPartyPolicy() {
    ::MessageBoxW(m_hwnd, kThirdPartyPolicy, L"Third-party plugin policy", MB_OK | MB_ICONINFORMATION);
}

void SettingsWindow::ApplySettings() {
    if (!m_services.getSettings || !m_services.applySettings) return; UserSettings settings = m_services.getSettings();
    settings.runAtStartup = ::SendMessageW(m_runAtStartup, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.notifications = ::SendMessageW(m_notifications, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.hideMonitorOnSourceFocus = ::SendMessageW(m_hideMonitor, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.visuals = m_visualDraft;
    if(m_inputPage)m_inputPage->Store(settings);
    if(m_interactionPage)settings.interaction=m_interactionPage->Value();
    const int theme = static_cast<int>(::SendMessageW(m_theme, CB_GETCURSEL, 0, 0)); settings.theme = theme == 1 ? L"light" : theme == 2 ? L"dark" : L"system";
    std::wstring error; if (!m_services.applySettings(settings, error)) ::MessageBoxW(m_hwnd, error.c_str(), L"Nskry Settings", MB_ICONERROR);
    else {
        RefreshFromServices();
        const auto message=L"Settings applied."+error;
        ::MessageBoxW(m_hwnd, message.c_str(), L"Nskry Settings", error.empty()?MB_ICONINFORMATION:MB_ICONWARNING);
    }
}

void SettingsWindow::RefreshVisualControls() {
    ::SendMessageW(m_shadowEnabled, BM_SETCHECK, m_visualDraft.shadowEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(m_cornersEnabled, BM_SETCHECK, m_visualDraft.cornersEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(m_shadowPreset, CB_SETCURSEL, static_cast<int>(m_visualDraft.shadowPreset), 0);
    ::SendMessageW(m_cornerPreset, CB_SETCURSEL, static_cast<int>(m_visualDraft.cornerPreset), 0);
    ::SendMessageW(m_cornerMode, CB_SETCURSEL, static_cast<int>(m_visualDraft.cornerMode), 0);
    const int values[] = {m_visualDraft.shadowOpacity, m_visualDraft.shadowBlur, m_visualDraft.shadowRange,
                         m_visualDraft.shadowOffset, m_visualDraft.cornerRadius, m_visualDraft.cornerPercent};
    for (int i = 0; i < 6; ++i) {
        ::SendMessageW(m_visualSliders[i], TBM_SETPOS, TRUE, values[i]);
        ::SetWindowTextW(m_visualValues[i], std::to_wstring(values[i]).c_str());
        ::EnableWindow(m_visualSliders[i], i < 4 ? m_visualDraft.shadowEnabled :
            m_visualDraft.cornersEnabled && (i == 4 ? m_visualDraft.cornerMode == CornerMode::Fixed : m_visualDraft.cornerMode == CornerMode::Percent));
    }
    ::EnableWindow(m_shadowPreset, m_visualDraft.shadowEnabled);
    ::EnableWindow(m_cornerPreset, m_visualDraft.cornersEnabled);
    ::EnableWindow(m_cornerMode, m_visualDraft.cornersEnabled);
}
void SettingsWindow::OnVisualControl(int id) {
    if (id == 140) m_visualDraft.shadowEnabled = ::SendMessageW(m_shadowEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (id == 142) m_visualDraft.cornersEnabled = ::SendMessageW(m_cornersEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (id == 141) SetShadowPreset(m_visualDraft, static_cast<VisualPreset>(::SendMessageW(m_shadowPreset, CB_GETCURSEL, 0, 0)));
    if (id == 143) SetCornerPreset(m_visualDraft, static_cast<VisualPreset>(::SendMessageW(m_cornerPreset, CB_GETCURSEL, 0, 0)));
    if (id == 144) {
        m_visualDraft.cornerMode = static_cast<CornerMode>(::SendMessageW(m_cornerMode, CB_GETCURSEL, 0, 0));
        m_visualDraft.cornerPreset = m_visualDraft.cornerMode == CornerMode::System ? VisualPreset::Win11 : VisualPreset::Custom;
    }
    if (id >= 149 && id <= 154) {
        const int i = id - 149;
        int* values[] = {&m_visualDraft.shadowOpacity, &m_visualDraft.shadowBlur, &m_visualDraft.shadowRange,
                        &m_visualDraft.shadowOffset, &m_visualDraft.cornerRadius, &m_visualDraft.cornerPercent};
        *values[i] = static_cast<int>(::SendMessageW(m_visualSliders[i], TBM_GETPOS, 0, 0));
        if (i < 4) m_visualDraft.shadowPreset = VisualPreset::Custom;
        else m_visualDraft.cornerPreset = VisualPreset::Custom;
    }
    RefreshVisualControls(); // Value-only preview; no file write or capture re-render.
}

void SettingsWindow::ToggleSelectedPlugin() {
    const int i = static_cast<int>(::SendMessageW(m_pluginList, LB_GETCURSEL, 0, 0));
    if (i < 0 || i >= static_cast<int>(m_plugins.size()) || !m_services.setPluginEnabled) return;
    if (!m_services.setPluginEnabled(m_plugins[i].id, !m_plugins[i].enabled))
        ::MessageBoxW(m_hwnd, L"Could not save plugin enablement. The previous state is retained.", L"Nskry Settings", MB_OK | MB_ICONERROR);
    RefreshPluginList();
}
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
    if(m_updateBusy)return;
    constexpr std::wstring_view extension = L".nskryplugin";
    if (packagePath.size() < extension.size() ||
        _wcsicmp(packagePath.c_str() + packagePath.size() - extension.size(), extension.data()) != 0) {
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
        L"\nDescription: " + manifest.description +
        L"\nLoad policy: " + (manifest.loadPolicy == PluginLoadPolicy::Startup ?
            L"STARTUP - can run whenever Nskry starts" : L"On demand") +
        L"\n\n" + kThirdPartyPolicy + L"\n\nInstall '" + manifest.name + L"'?";
    if (::MessageBoxW(m_hwnd, warning.c_str(), L"Install third-party plugin?", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    const PluginPackageResult result = m_services.installThirdPartyPlugin(packagePath);
    ::MessageBoxW(m_hwnd, result.message.c_str(), result.success ? L"Nskry Settings" : L"Plugin installation failed",
        result.success ? MB_ICONINFORMATION : MB_ICONERROR);
    if (result.success) RefreshPluginList();
}
void SettingsWindow::Close() { if (m_closed) return; m_closed = true;m_updateLifetime.reset();CancelPluginUpdate(); if (m_hwnd) ::DestroyWindow(m_hwnd); if (m_onClosed) m_onClosed(); }

} // namespace nskry
