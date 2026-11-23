#include "pch.h"
#include <windowsx.h>
#include "ui/ui_preferences.h"
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
constexpr int IDC_PLUGIN_SETTINGS = 128;
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
SettingsWindow::~SettingsWindow() { m_updateLifetime.reset();CancelPluginUpdate();m_inputPage.reset();m_jumpPage.reset();m_interactionPage.reset();if (m_hwnd) ::DestroyWindow(m_hwnd); if (m_font) ::DeleteObject(m_font); }

void SettingsWindow::Show(HWND owner) {
    std::call_once(s_classOnce, [] {
        WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr); wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); wc.lpszClassName = L"NskrySettings";
        ::RegisterClassExW(&wc);
    });
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    ::InitCommonControlsEx(&icc);
    m_hwnd = ::CreateWindowExW(WS_EX_APPWINDOW, L"NskrySettings", UiText(L"Nskry Settings", L"Nskry 设置"),
        WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, 720, 590, owner, nullptr,
        ::GetModuleHandleW(nullptr), this);
    if (!m_hwnd) return;
    ::ShowWindow(m_hwnd, SW_SHOW);
    ::SetForegroundWindow(m_hwnd);
}

bool SettingsWindow::HandleTab(const MSG &message) {
    if (!m_hwnd || message.message != WM_KEYDOWN || message.wParam != VK_TAB ||
        (message.hwnd != m_hwnd && !::IsChild(m_hwnd, message.hwnd)) ||
        ::GetKeyState(VK_CONTROL) < 0)
        return false;
    const auto focus = ::GetFocus();
    const auto next = ::GetNextDlgTabItem(m_hwnd, ::IsChild(m_hwnd, focus) ? focus : nullptr,
                                         ::GetKeyState(VK_SHIFT) < 0);
    if (!next) return false;
    if (next != m_apply) {
        RECT rect{}, client{};
        ::GetWindowRect(next, &rect);
        ::MapWindowPoints(HWND_DESKTOP, m_hwnd, reinterpret_cast<POINT*>(&rect), 2);
        ::GetClientRect(m_hwnd, &client);
        const UINT dpi = ::GetDpiForWindow(m_hwnd);
        if (::IsWindowVisible(m_footer)) client.bottom -= ::MulDiv(48, dpi ? dpi : 96, 96);
        const auto expose = [dpi](LONG start, LONG end, LONG viewport) {
            const LONG delta = start < 0 ? start : end > viewport ? end - viewport : 0;
            return ::MulDiv(delta, 96, dpi ? dpi : 96);
        };
        m_scrollX += expose(rect.left, rect.right, client.right);
        m_scrollY += expose(rect.top, rect.bottom, client.bottom);
        LayoutControls();
    }
    ::SetFocus(next);
    return true;
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
    case WM_NSKRY_SETTINGS_DIRTY:
        RefreshDirtyState();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_RUN || LOWORD(wp) == IDC_NOTIFY || LOWORD(wp) == IDC_THEME ||
            LOWORD(wp) == 104 || LOWORD(wp) == 105)
            RefreshDirtyState();
        if (LOWORD(wp) == IDC_PLUGIN_SETTINGS) {
            if (m_updateBusy) return 0;
            const auto i = static_cast<int>(SendMessageW(m_pluginList, LB_GETCURSEL, 0, 0));
            if (i >= 0 && i < static_cast<int>(m_plugins.size()) && m_services.showPluginSettings &&
                !m_services.showPluginSettings(m_plugins[i].id, m_hwnd))
                MessageBoxW(m_hwnd, UiText(L"Plugin settings could not be opened.", L"无法打开插件设置。"), L"Nskry", MB_ICONWARNING);
            return 0;
        }
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
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        m_palette = GetDesktopPalette();
        ::InvalidateRect(m_hwnd, nullptr, TRUE);
        return 0;
    case WM_ERASEBKGND: {
        RECT client{};
        ::GetClientRect(m_hwnd, &client);
        const auto dc = reinterpret_cast<HDC>(wp);
        ::SetDCBrushColor(dc, m_palette.window);
        ::FillRect(dc, &client, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto dc = reinterpret_cast<HDC>(wp);
        const bool field = msg != WM_CTLCOLORSTATIC || reinterpret_cast<HWND>(lp) == m_pluginDetails;
        const COLORREF color = field ? m_palette.panel : m_palette.window;
        ::SetBkColor(dc, color);
        ::SetTextColor(dc, m_palette.text);
        ::SetDCBrushColor(dc, color);
        return reinterpret_cast<LRESULT>(::GetStockObject(DC_BRUSH));
    }
    case WM_SIZE: LayoutControls(); return 0;
    case WM_DPICHANGED: {
        const auto& suggested = *reinterpret_cast<const RECT*>(lp);
        ::SetWindowPos(m_hwnd, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                       suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyUiFont();
        LayoutControls();
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {390, 360}; return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (msg == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT))
            m_scrollX += (msg == WM_MOUSEHWHEEL ? steps : -steps) * 48;
        else
            m_scrollY -= steps * 48;
        LayoutControls();
        return 0;
    }
    case WM_VSCROLL:
        if (!lp) { ScrollSettings(SB_VERT, LOWORD(wp)); return 0; } break;
    case WM_HSCROLL:
        if (!lp) { ScrollSettings(SB_HORZ, LOWORD(wp)); return 0; }
        for (int i = 0; i < 6; ++i)
            if (reinterpret_cast<HWND>(lp) == m_visualSliders[i]) OnVisualControl(149 + i);
        return 0;
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wp);
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count != 1) ::MessageBoxW(m_hwnd, UiText(L"Drop one .nskryplugin package at a time.", L"每次拖入一个 .nskryplugin 插件包。"), UiText(L"Nskry Settings", L"Nskry 设置"), MB_ICONINFORMATION);
        else {
            const UINT length = ::DragQueryFileW(drop, 0, nullptr, 0);
            std::vector<wchar_t> path(length + 1);
            ::DragQueryFileW(drop, 0, path.data(), static_cast<UINT>(path.size()));
            InstallThirdPartyPackage(path.data());
        }
        ::DragFinish(drop);
        return 0;
    }
    case WM_CLOSE:
        if (m_services.getSettings && ReadSettingsDraft() != m_appliedDraft) {
            const int choice = ::MessageBoxW(m_hwnd,
                UiText(L"Apply your changes before closing Settings?", L"关闭设置前要应用更改吗？"),
                UiText(L"Nskry Settings", L"Nskry 设置"), MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
            if (choice == IDCANCEL || (choice == IDYES && !ApplySettings()))
                return 0;
        }
        Close();
        return 0;
    case WM_DESTROY: m_hwnd = nullptr; if (!m_closed) Close(); return 0;
    }
    return ::DefWindowProcW(m_hwnd, msg, wp, lp);
}

void SettingsWindow::CreateControls() {
    m_tab = ::CreateWindowExW(0, WC_TABCONTROLW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | TCS_BUTTONS,
        12, 12, 680, 32, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TAB)), nullptr, nullptr);
    const wchar_t* tabs[] = { UiText(L"General", L"常规"), UiText(L"Hotkeys", L"快捷键"), UiText(L"Plugins", L"插件"), UiText(L"About", L"关于"), UiText(L"Window effects", L"窗口效果"), UiText(L"Input appearance", L"输入外观") };
    for (const auto* tab : tabs) { TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(tab); TabCtrl_InsertItem(m_tab, TabCtrl_GetItemCount(m_tab), &item); }
    auto control = [&](DWORD style, const wchar_t* text, int id, int x, int y, int w, int h) {
        return ::CreateWindowExW(0, L"BUTTON", text, WS_CHILD | style | ((style & BS_TYPEMASK) == BS_GROUPBOX ? 0 : WS_TABSTOP), x, y, w, h, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr); };
    m_generalDecor.push_back(control(WS_VISIBLE | BS_GROUPBOX, UiText(L"General", L"常规"), 0, 32, 55, 630, 190));
    m_runAtStartup = control(WS_VISIBLE | BS_AUTOCHECKBOX, UiText(L"Start Nskry when I sign in", L"登录 Windows 时启动 Nskry"), IDC_RUN, 52, 90, 300, 24);
    m_notifications = control(WS_VISIBLE | BS_AUTOCHECKBOX, UiText(L"Show notifications", L"显示通知"), IDC_NOTIFY, 52, 120, 300, 24);
    m_hideMonitor = control(WS_VISIBLE | BS_AUTOCHECKBOX, UiText(L"Hide monitor while its source window is focused", L"来源窗口聚焦时自动隐藏画中画"), 104, 52, 194, 500, 24);
    m_generalDecor.push_back(::CreateWindowExW(0, L"STATIC", UiText(L"Theme:", L"主题"), WS_CHILD | WS_VISIBLE, 52, 158, 90, 22, m_hwnd, nullptr, nullptr, nullptr));
    m_theme = ::CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 145, 154, 150, 150, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_THEME)), nullptr, nullptr);
    ::SendMessageW(m_theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(UiText(L"System", L"跟随系统"))); ::SendMessageW(m_theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(UiText(L"Light", L"浅色"))); ::SendMessageW(m_theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(UiText(L"Dark", L"深色")));
    m_generalDecor.push_back(::CreateWindowExW(0, L"STATIC", L"Language / 语言", WS_CHILD | WS_VISIBLE,
        335, 158, 115, 22, m_hwnd, nullptr, nullptr, nullptr));
    m_language = ::CreateWindowExW(0, WC_COMBOBOXW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 452, 154, 170, 150,
        m_hwnd, reinterpret_cast<HMENU>(105), nullptr, nullptr);
    for (const auto* label : {L"System / 跟随系统", L"简体中文", L"English"})
        ::SendMessageW(m_language, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    auto definitions = m_services.getShortcuts ? m_services.getShortcuts() : CoreShortcuts();
    if (DesktopUiChinese()) {
        constexpr std::pair<std::wstring_view, const wchar_t*> labels[] = {
            {L"core.capture", L"截图"}, {L"core.exit", L"退出"}, {L"core.monitor", L"框选画中画区域"},
            {L"core.cancel", L"取消框选或标注"}, {L"core.undo", L"撤销标注"}, {L"core.redo", L"重做标注"},
            {L"core.copy", L"复制选区"}, {L"core.save", L"保存选区"}, {L"core.text.accept", L"确认文字标注"},
            {L"core.text.cancel", L"取消文字标注"}, {L"core.editor.cancel", L"取消编辑或退出工具"},
            {L"core.editor.confirm", L"确认切除或文字识别选区"}, {L"core.precision", L"按住以精细调整"},
            {L"core.editor.zoom", L"编辑器缩放"}, {L"core.editor.pan_x", L"编辑器水平平移"},
            {L"core.editor.pan_y", L"编辑器垂直平移"},
        };
        for (auto &definition : definitions) {
            const auto match = std::find_if(std::begin(labels), std::end(labels),
                [&](const auto &item) { return item.first == definition.id; });
            if (match != std::end(labels)) definition.label = match->second;
        }
    }
    m_hotkeyDecor.push_back(::CreateWindowExW(0,L"STATIC",
        UiText(L"Release all keys to finish recording. You can also type a shortcut such as Ctrl+Alt+A. Hold shortcuts accept Alt or Space. Wheel shortcuts can use Shift+Wheel.",
               L"松开所有按键以完成录制，也可输入 Ctrl+Alt+A 等组合。按住类快捷键可用 Alt 或空格，滚轮类可用 Shift+Wheel。"),
        WS_CHILD,32,58,630,44,m_hwnd,nullptr,nullptr,nullptr));
    m_inputPage=std::make_unique<InputPage>(m_hwnd,definitions,false,m_services.setRecording,nullptr);
    m_jumpPage=std::make_unique<InputPage>(m_hwnd,definitions,true,nullptr,[this](const std::wstring& id){SelectPage(1);m_inputPage->Jump(id);});
    m_interactionPage=std::make_unique<InteractionPage>(m_hwnd);
    m_pluginList = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr, WS_CHILD | LBS_NOTIFY | WS_VSCROLL, 32, 58, 630, 240, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PLUGIN_LIST)), nullptr, nullptr);
    m_pluginToggle = control(BS_PUSHBUTTON, UiText(L"Enable / Disable", L"启用或禁用"), IDC_PLUGIN_TOGGLE, 32, 315, 130, 28);
    m_pluginUninstall = control(BS_PUSHBUTTON, UiText(L"Uninstall", L"卸载"), IDC_PLUGIN_UNINSTALL, 175, 315, 100, 28);
    m_pluginInstall = control(BS_PUSHBUTTON, UiText(L"Install third-party plugin...", L"安装第三方插件…"), IDC_PLUGIN_INSTALL, 288, 315, 190, 28);
    m_pluginCheckUpdates = control(BS_PUSHBUTTON, UiText(L"Check for updates", L"检查更新"), IDC_PLUGIN_CHECK_UPDATES, 492, 315, 170, 28);
    m_pluginDetails = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", UiText(L"Select a plugin to view its information.", L"选择插件以查看详情。"),
        WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        32, 250, 630, 130, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PLUGIN_DETAILS)), nullptr, nullptr);
    m_officialPlugins = control(BS_PUSHBUTTON, UiText(L"Browse official plugins...", L"浏览官方插件…"), IDC_OFFICIAL_PLUGINS, 32, 420, 210, 28);
    m_thirdPartyPolicy = control(BS_PUSHBUTTON, UiText(L"Third-party plugin policy...", L"第三方插件政策…"), IDC_PLUGIN_POLICY, 255, 420, 220, 28);
    m_pluginSettings = control(BS_PUSHBUTTON, UiText(L"Plugin settings...", L"插件设置…"), IDC_PLUGIN_SETTINGS, 488, 420, 174, 28);
    m_aboutText = ::CreateWindowExW(0, L"STATIC", UiText(
        L"Nskry\n\nLightweight Windows capture and picture in picture.\n\nVersion " NSKRY_VERSION L"\nPlugin API version 2",
        L"Nskry\n\n轻量 Windows 截图与画中画工具。\n\n版本 " NSKRY_VERSION L"\n插件 API 版本 2"), WS_CHILD | SS_LEFT,
        52, 72, 520, 180, m_hwnd, nullptr, nullptr, nullptr);
    m_footer = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_WHITERECT, 0, 0, 0, 0,
        m_hwnd, nullptr, nullptr, nullptr);
    m_dirty = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
        0, 0, 0, 0, m_hwnd, nullptr, nullptr, nullptr);
    m_apply = control(WS_VISIBLE | BS_DEFPUSHBUTTON, UiText(L"Apply", L"应用"), IDC_APPLY, 580, 420, 110, 32);
    auto visual = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
        HWND item = ::CreateWindowExW(0, type, text, WS_CHILD | style, x, y, w, h, m_hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        m_visualControls.push_back(item); return item;
    };
    auto preset = [&](int id, int y) {
        HWND combo = visual(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, id, 340, y, 230, 140);
        for (const auto* text : {UiText(L"Win11 style", L"Windows 11 风格"), UiText(L"macOS style (approx.)", L"近似 macOS 风格"), UiText(L"Custom", L"自定义")})
            ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        return combo;
    };
    m_shadowEnabled = visual(L"BUTTON", UiText(L"Shadow", L"阴影"), BS_AUTOCHECKBOX, 140, 50, 64, 200, 24);
    m_shadowPreset = preset(141, 62);
    m_cornersEnabled = visual(L"BUTTON", UiText(L"Rounded corners", L"圆角"), BS_AUTOCHECKBOX, 142, 50, 251, 250, 24);
    m_cornerPreset = preset(143, 249);
    m_cornerMode = visual(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 144, 340, 285, 230, 140);
    for (const auto* text : {UiText(L"Win11 system preference", L"Windows 11 系统设置"), UiText(L"Fixed radius (DIP)", L"固定半径（DIP）"), UiText(L"Percentage of short edge", L"短边比例")})
        ::SendMessageW(m_cornerMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    visual(L"STATIC", UiText(L"Corner size rule:", L"圆角尺寸规则"), 0, 0, 50, 289, 240, 24);
    const wchar_t* labels[] = {UiText(L"Opacity (%)", L"不透明度（%）"), UiText(L"Blur sigma (DIP)", L"模糊强度（DIP）"), UiText(L"Range (DIP)", L"范围（DIP）"), UiText(L"Offset Y (DIP)", L"纵向偏移（DIP）"), UiText(L"Radius (DIP; system ref.)", L"半径（DIP，系统参考值）"), UiText(L"Short-edge percentage", L"短边比例")};
    const int maximum[] = {50, 8, 24, 6, 24, 15};
    for (int i = 0; i < 6; ++i) {
        const int y = i < 4 ? 100 + i * 35 : 324 + (i - 4) * 35;
        visual(L"STATIC", labels[i], 0, 0, 50, y + 4, 200, 24);
        m_visualSliders[i] = visual(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 149 + i, 255, y, 285, 30);
        ::SendMessageW(m_visualSliders[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, maximum[i]));
        m_visualValues[i] = visual(L"STATIC", L"", 0, 0, 556, y + 4, 60, 24);
    }
    visual(L"STATIC", UiText(
           L"Effects apply to the window appearance. Copy, Save and OCR use the original image.\nSelect Apply to update existing and future windows.",
           L"窗口效果仅用于显示。复制、保存和文字识别使用原图。\n选择「应用」以更新现有及之后打开的窗口。"),
           0, 0, 50, 404, 600, 62);
    SelectPage(0);
    m_controlBounds.clear();
    for (HWND child = ::GetWindow(m_hwnd, GW_CHILD); child; child = ::GetWindow(child, GW_HWNDNEXT)) {
        RECT bounds{};
        wchar_t cls[32]{};
        ::GetClassNameW(child, cls, ARRAYSIZE(cls));
        if (_wcsicmp(cls, WC_COMBOBOXW) ||
            !::SendMessageW(child, CB_GETDROPPEDCONTROLRECT, 0, reinterpret_cast<LPARAM>(&bounds)))
            ::GetWindowRect(child, &bounds);
        ::MapWindowPoints(HWND_DESKTOP, m_hwnd, reinterpret_cast<POINT*>(&bounds), 2);
        m_controlBounds.push_back({child, bounds});
    }
    ApplyUiFont();
    LayoutControls();
}

void SettingsWindow::ScrollSettings(int bar, int request) {
    SCROLLINFO info{sizeof(info), SIF_ALL};
    if (!::GetScrollInfo(m_hwnd, bar, &info)) return;
    int &position = bar == SB_HORZ ? m_scrollX : m_scrollY;
    switch (request) {
    case SB_LINEUP: position -= 32; break;
    case SB_LINEDOWN: position += 32; break;
    case SB_PAGEUP: position -= static_cast<int>(info.nPage) / 2; break;
    case SB_PAGEDOWN: position += static_cast<int>(info.nPage) / 2; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK: position = info.nTrackPos; break;
    case SB_TOP: position = 0; break;
    case SB_BOTTOM: position = info.nMax; break;
    default: return;
    }
    LayoutControls();
}

void SettingsWindow::ApplyUiFont() {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    const UINT dpi = ::GetDpiForWindow(m_hwnd);
    if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
        return;
    HFONT font = ::CreateFontIndirectW(&metrics.lfMessageFont);
    if (!font) return;
    ::EnumChildWindows(m_hwnd, [](HWND child, LPARAM value) -> BOOL {
        ::SendMessageW(child, WM_SETFONT, value, TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));
    if (m_font) ::DeleteObject(m_font);
    m_font = font;
}

void SettingsWindow::LayoutControls() {
    if (!m_tab) return;
    RECT client{}; ::GetClientRect(m_hwnd, &client);
    const UINT dpi = ::GetDpiForWindow(m_hwnd);
    const auto scale = [dpi](int n) { return ::MulDiv(n, dpi ? dpi : 96, 96); };
    const int viewWidth = ::MulDiv(client.right, 96, dpi ? dpi : 96);
    const int viewHeight = ::MulDiv(client.bottom, 96, dpi ? dpi : 96);
    const int contentWidth = (std::max)(704, viewWidth), contentHeight = (std::max)(538, viewHeight);
    const auto setScroll = [&](int bar, int content, int view, int &position) {
        position = std::clamp(position, 0, content - view);
        SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL,
                          0, content - 1, static_cast<UINT>(view), position};
        ::SetScrollInfo(m_hwnd, bar, &scroll, TRUE);
    };
    setScroll(SB_HORZ, contentWidth, viewWidth, m_scrollX);
    setScroll(SB_VERT, contentHeight, viewHeight, m_scrollY);
    client.right = contentWidth;
    client.bottom = contentHeight;
    const auto move = [&](HWND control, int x, int y, int w, int h) {
        if (control) ::SetWindowPos(control, nullptr, scale(x - m_scrollX), scale(y - m_scrollY),
            scale(w), scale(h), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    for (const auto &entry : m_controlBounds) {
        const HWND child = entry.first;
        if (child == m_apply || child == m_footer || child == m_dirty || child == m_tab || child == m_pluginList ||
            child == m_pluginDetails || child == m_pluginToggle || child == m_pluginUninstall ||
            child == m_pluginInstall || child == m_pluginCheckUpdates || child == m_officialPlugins ||
            child == m_thirdPartyPolicy || child == m_pluginSettings ||
            (m_inputPage && child == m_inputPage->Hwnd()) || (m_jumpPage && child == m_jumpPage->Hwnd()) ||
            (m_interactionPage && child == m_interactionPage->Hwnd()))
            continue;
        const auto &bounds = entry.second;
        move(child, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
    }
    const int width = (std::max)(630, static_cast<int>(client.right) - 64);
    const int listHeight = (std::max)(100, static_cast<int>(client.bottom) - 358);
    const int detailsY = 58 + listHeight + 8, buttonsY = detailsY + 150;
    move(m_tab, 12, 12, client.right - 24, 32);
    move(m_pluginList, 32, 58, width, listHeight);
    move(m_pluginDetails, 32, detailsY, width, 140);
    move(m_pluginToggle, 32, buttonsY, 130, 28);
    move(m_pluginUninstall, 175, buttonsY, 100, 28);
    move(m_pluginInstall, 288, buttonsY, 190, 28);
    move(m_pluginCheckUpdates, 492, buttonsY, 170, 28);
    move(m_officialPlugins, 32, buttonsY + 38, 210, 28);
    move(m_thirdPartyPolicy, 255, buttonsY + 38, 220, 28);
    move(m_pluginSettings, 488, buttonsY + 38, 174, 28);
    ::SetWindowPos(m_footer, HWND_TOP, 0, scale(viewHeight - 48), scale(viewWidth), scale(48), SWP_NOACTIVATE);
    ::SetWindowPos(m_dirty, HWND_TOP, scale(16), scale(viewHeight - 40), scale((std::max)(0, viewWidth - 158)),
                   scale(32), SWP_NOACTIVATE);
    ::SetWindowPos(m_apply, HWND_TOP, scale(viewWidth - 126), scale(viewHeight - 40), scale(110), scale(32), SWP_NOACTIVATE);
    if(m_inputPage)move(m_inputPage->Hwnd(),32,108,width,client.bottom-178);
    if(m_jumpPage)move(m_jumpPage->Hwnd(),32,258,width,client.bottom-328);
    if(m_interactionPage)move(m_interactionPage->Hwnd(),32,58,width,client.bottom-128);
}

void SettingsWindow::SelectPage(int page) {
    m_page = page; if (m_tab) TabCtrl_SetCurSel(m_tab, page);
    const bool general = page == 0, hotkeys = page == 1, plugins = page == 2, about = page == 3;
    ShowControl(m_runAtStartup, general); ShowControl(m_notifications, general); ShowControl(m_theme, general); ShowControl(m_language, general);
    ShowControl(m_hideMonitor, general);
    if(m_inputPage)ShowControl(m_inputPage->Hwnd(),hotkeys);
    if(m_jumpPage)ShowControl(m_jumpPage->Hwnd(),general);
    if(m_interactionPage)ShowControl(m_interactionPage->Hwnd(),page==5);
    ShowControl(m_pluginList, plugins); ShowControl(m_pluginToggle, plugins); ShowControl(m_pluginUninstall, plugins); ShowControl(m_pluginInstall, plugins); ShowControl(m_pluginCheckUpdates, plugins);
    if (plugins) RefreshPluginList();
    RefreshDirtyState();
    for (auto control : m_visualControls) ShowControl(control, page == 4);
    ShowControl(m_pluginDetails, plugins); ShowControl(m_officialPlugins, plugins); ShowControl(m_thirdPartyPolicy, plugins);
    ShowControl(m_pluginSettings, plugins);
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
    ::SendMessageW(m_language, CB_SETCURSEL, settings.language == L"zh-CN" ? 1 : settings.language == L"en" ? 2 : 0, 0);
    m_visualDraft = settings.visuals; RefreshVisualControls();
    m_appliedDraft = ReadSettingsDraft();
    m_hasDraft = true;
    RefreshDirtyState();
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
    EnableWindow(m_pluginSettings, valid && !m_updateBusy && m_plugins[selected].enabled && m_plugins[selected].hasSettings);
    ::EnableWindow(m_pluginToggle, valid && !m_updateBusy); ::EnableWindow(m_pluginUninstall, valid && !m_updateBusy);
    const auto notice=m_updateNotice.empty()?L"":m_updateNotice+L"\r\n\r\n";
    if (!valid) { SetText(m_pluginDetails, notice+UiText(L"No plugin selected. Use Browse official plugins or install a local package.", L"尚未选择插件。可浏览官方插件或安装本地插件包。")); return; }
    const auto& plugin = m_plugins[selected];
    const wchar_t* source = plugin.source == PluginSource::Official ? UiText(L"Official source (identity not verified)", L"官方来源（身份未经验证）") :
        plugin.source == PluginSource::ThirdParty ? UiText(L"Third-party", L"第三方") : plugin.source == PluginSource::Legacy ? UiText(L"Legacy", L"旧版安装") : UiText(L"Local / unverified", L"本地安装／未经验证");
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
            UiText(L"Official plugins", L"官方插件"), MB_OK | MB_ICONINFORMATION); return;
    }
    if (!IsPluginHttpsUrl(url)) {
        ::MessageBoxW(m_hwnd, UiText(L"The official plugin page must be a valid HTTPS URL.", L"官方插件页面需要有效的 HTTPS 地址。"), UiText(L"Official plugins", L"官方插件"), MB_OK | MB_ICONERROR); return;
    }
    if (reinterpret_cast<INT_PTR>(::ShellExecuteW(m_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        ::MessageBoxW(m_hwnd, UiText(L"Could not open the plugin website in your browser.", L"无法在浏览器中打开插件网站。"), UiText(L"Official plugins", L"官方插件"), MB_OK | MB_ICONERROR);
}
void SettingsWindow::ShowThirdPartyPolicy() {
    ::MessageBoxW(m_hwnd, kThirdPartyPolicy, UiText(L"Third-party plugin policy", L"第三方插件政策"), MB_OK | MB_ICONINFORMATION);
}

UserSettings SettingsWindow::ReadSettingsDraft(bool commit) const {
    UserSettings settings = m_services.getSettings();
    settings.runAtStartup = ::SendMessageW(m_runAtStartup, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.notifications = ::SendMessageW(m_notifications, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.hideMonitorOnSourceFocus = ::SendMessageW(m_hideMonitor, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.visuals = m_visualDraft;
    if (m_inputPage) {
        if (commit) m_inputPage->Store(settings);
        else m_inputPage->Draft(settings);
    }
    if(m_interactionPage)settings.interaction=m_interactionPage->Value();
    const int theme = static_cast<int>(::SendMessageW(m_theme, CB_GETCURSEL, 0, 0)); settings.theme = theme == 1 ? L"light" : theme == 2 ? L"dark" : L"system";
    const auto language = ::SendMessageW(m_language, CB_GETCURSEL, 0, 0);
    settings.language = language == 1 ? L"zh-CN" : language == 2 ? L"en" : L"system";
    return settings;
}

void SettingsWindow::RefreshDirtyState() {
    if (!m_apply) return;
    const bool changed = m_hasDraft && ReadSettingsDraft() != m_appliedDraft;
    const bool editablePage = m_page == 0 || m_page == 1 || m_page == 4 || m_page == 5;
    ShowControl(m_footer, editablePage || changed);
    ShowControl(m_dirty, changed);
    ShowControl(m_apply, editablePage || changed);
    SetText(m_dirty, UiText(L"Unsaved changes", L"有未应用的更改"));
    ::EnableWindow(m_apply, changed);
}

bool SettingsWindow::ApplySettings() {
    if (!m_services.getSettings || !m_services.applySettings) return false;
    const auto settings = ReadSettingsDraft(true);
    const auto before = m_services.getSettings();
    std::wstring error;
    if (!m_services.applySettings(settings, error)) {
        ::MessageBoxW(m_hwnd, error.c_str(), UiText(L"Nskry Settings", L"Nskry 设置"), MB_ICONERROR);
        return false;
    }
    if (before.language != settings.language) {
        const int page = m_page;
        m_hasDraft = false;
        m_controlBounds.clear();
        m_inputPage.reset(); m_jumpPage.reset(); m_interactionPage.reset();
        while (HWND child = ::GetWindow(m_hwnd, GW_CHILD)) ::DestroyWindow(child);
        m_generalDecor.clear(); m_hotkeyDecor.clear(); m_visualControls.clear();
        m_scrollX = m_scrollY = 0;
        ::SetWindowTextW(m_hwnd, UiText(L"Nskry Settings", L"Nskry 设置"));
        CreateControls();
        SelectPage(page);
        ::SetFocus(m_apply);
    }
    RefreshFromServices();
    m_palette = GetDesktopPalette();
    ::InvalidateRect(m_hwnd, nullptr, TRUE);
    if (!error.empty())
        ::MessageBoxW(m_hwnd, error.c_str(), UiText(L"Nskry Settings", L"Nskry 设置"), MB_ICONWARNING);
    return true;
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
    RefreshVisualControls();
    RefreshDirtyState();
}

void SettingsWindow::ToggleSelectedPlugin() {
    const int i = static_cast<int>(::SendMessageW(m_pluginList, LB_GETCURSEL, 0, 0));
    if (i < 0 || i >= static_cast<int>(m_plugins.size()) || !m_services.setPluginEnabled) return;
    if (!m_services.setPluginEnabled(m_plugins[i].id, !m_plugins[i].enabled))
        ::MessageBoxW(m_hwnd, UiText(L"Could not save plugin enablement. The previous state is retained.", L"无法保存插件启用状态，已保留原设置。"), UiText(L"Nskry Settings", L"Nskry 设置"), MB_OK | MB_ICONERROR);
    RefreshPluginList();
}
void SettingsWindow::UninstallSelectedPlugin() { const int i = static_cast<int>(::SendMessageW(m_pluginList, LB_GETCURSEL, 0, 0)); if (i < 0 || i >= static_cast<int>(m_plugins.size()) || !m_services.uninstallPlugin) return; if (::MessageBoxW(m_hwnd, UiText(L"Remove this plugin when Nskry restarts?", L"在 Nskry 重启时卸载此插件？"), UiText(L"Nskry Settings", L"Nskry 设置"), MB_YESNO | MB_ICONWARNING) == IDYES) { std::wstring error; if (!m_services.uninstallPlugin(m_plugins[i].id, error)) ::MessageBoxW(m_hwnd, error.c_str(), UiText(L"Nskry Settings", L"Nskry 设置"), MB_ICONERROR); RefreshPluginList(); } }

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
        ::MessageBoxW(m_hwnd, UiText(L"Choose a .nskryplugin package.", L"请选择 .nskryplugin 插件包。"), UiText(L"Nskry Settings", L"Nskry 设置"), MB_ICONERROR);
        return;
    }
    if (!m_services.inspectPluginPackage || !m_services.installThirdPartyPlugin) return;
    PluginManifest manifest;
    std::wstring error;
    if (!m_services.inspectPluginPackage(packagePath, manifest, error)) {
        ::MessageBoxW(m_hwnd, error.c_str(), UiText(L"Plugin package rejected", L"插件包未通过验证"), MB_ICONERROR);
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
    if (::MessageBoxW(m_hwnd, warning.c_str(), UiText(L"Install third-party plugin?", L"安装这个第三方插件？"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    const PluginPackageResult result = m_services.installThirdPartyPlugin(packagePath);
    ::MessageBoxW(m_hwnd, result.message.c_str(), result.success ? UiText(L"Nskry Settings", L"Nskry 设置") : UiText(L"Plugin installation failed", L"插件安装失败"),
        result.success ? MB_ICONINFORMATION : MB_ICONERROR);
    if (result.success) RefreshPluginList();
}
void SettingsWindow::Close() { if (m_closed) return; m_closed = true;m_updateLifetime.reset();CancelPluginUpdate(); if (m_hwnd) ::DestroyWindow(m_hwnd); if (m_onClosed) m_onClosed(); }

} // namespace nskry
