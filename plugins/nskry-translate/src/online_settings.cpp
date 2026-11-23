#include "online_settings.h"
#include "../../common/native_ui.h"
#include <vector>
#include <algorithm>

namespace nskry::translate {
namespace {
constexpr wchar_t Class[] = L"NskryTranslationOnlineSettingsV1";
HWND settings{};
HFONT settingsFont{};
struct Settings {
    Provider provider;
    std::vector<std::pair<HWND, RECT>> controls;
};
std::wstring Text(HWND hwnd, int id) {
    HWND edit = GetDlgItem(hwnd, id);
    std::wstring text(GetWindowTextLengthW(edit) + 1, 0);
    const auto length = GetWindowTextW(edit, text.data(), int(text.size()));
    text.resize(length);
    return text;
}
void Layout(HWND hwnd, Settings &state) {
    const UINT windowDpi = ::GetDpiForWindow(hwnd), dpi = windowDpi ? windowDpi : 96;
    const auto scale = [dpi](int value) { return ::MulDiv(value, dpi, 96); };
    RECT client{};
    ::GetClientRect(hwnd, &client);
    const int available = ::MulDiv(client.right, 96, dpi);
    const auto horizontal = [available](int x) { return ::MulDiv(x, available, 684); };
    HFONT font = plugin_ui::CreateMessageFont(hwnd);
    for (const auto &[control, bounds] : state.controls) {
        if (font) ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        ::SetWindowPos(control, nullptr, scale(horizontal(bounds.left)), scale(bounds.top),
                       scale(horizontal(bounds.right - bounds.left)), scale(bounds.bottom - bounds.top),
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (font) {
        if (settingsFont) ::DeleteObject(settingsFont);
        settingsFont = font;
    }
}

LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto *state = reinterpret_cast<Settings *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    try {
        if (message == WM_NCCREATE) {
            state = static_cast<Settings *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(state));
        }
        if (message == WM_DPICHANGED && state) {
            const auto &bounds = *reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(hwnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                           bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(hwnd, *state);
            return 0;
        }
        if (message == WM_ERASEBKGND)
            return plugin_ui::Background(hwnd, wp, plugin_ui::preferences.theme);
        if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT)
            return plugin_ui::ControlColors(wp, plugin_ui::preferences.theme, message == WM_CTLCOLOREDIT);
        if (message == WM_COMMAND && state) {
            const auto id = LOWORD(wp);
            if (id == 1) {
                OnlineConfig config;
                config.endpoint = Text(hwnd, 10);
                config.model = Text(hwnd, 11);
                config.key = Text(hwnd, 12);
                config.legacyTokenLimit = SendDlgItemMessageW(hwnd, 13, BM_GETCHECK, 0, 0) == BST_CHECKED;
                std::wstring error;
                if (!SaveOnlineConfig(state->provider, config, error))
                    SetDlgItemTextW(hwnd, 20, error.c_str());
                else {
                    SetDlgItemTextW(hwnd, 12, L""); // Do not retain a credential in an idle EDIT buffer.
                    DestroyWindow(hwnd);
                }
            } else if (id == 2)
                DestroyWindow(hwnd);
            else if (id == 3) {
                std::wstring error;
                if (ForgetOnlineConfig(state->provider, error)) {
                    SetDlgItemTextW(hwnd, 10, L"");
                    SetDlgItemTextW(hwnd, 11, L"");
                    SetDlgItemTextW(hwnd, 12, L"");
                    SetDlgItemTextW(
                        hwnd, 20,
                        plugin_ui::Text(L"This provider's saved endpoint/model/key were removed. No request was sent.", L"已移除此服务的端点、模型和密钥，未发送请求。"));
                } else
                    SetDlgItemTextW(hwnd, 20, error.c_str());
            }
            return 0;
        }
        if (message == WM_DESTROY)
            SetDlgItemTextW(hwnd, 12, L"");
        if (message == WM_NCDESTROY) {
            settings = nullptr;
            if (settingsFont) { ::DeleteObject(settingsFont); settingsFont = nullptr; }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
    } catch (...) {
        SetDlgItemTextW(hwnd, 20, plugin_ui::Text(L"Could not change online settings. No network request was sent.", L"无法更新在线设置，未发送网络请求。"));
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
} // namespace

void ShowOnlineSettings(HINSTANCE module, HWND owner, Provider provider) {
    if (settings) {
        ShowWindow(settings, SW_RESTORE);
        SetForegroundWindow(settings);
        return;
    }
    if (provider != Provider::OpenAI && provider != Provider::LibreTranslate)
        return;
    WNDCLASSW wc{};
    wc.hInstance = module;
    wc.lpfnWndProc = Procedure;
    wc.lpszClassName = Class;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    // State has static lifetime for this single, modeless settings window. It
    // contains no text, credentials, callbacks or worker-owned objects.
    static Settings state;
    state = {provider, {}};
    const UINT ownerDpi = owner ? ::GetDpiForWindow(owner) : ::GetDpiForSystem();
    const auto scale = [ownerDpi](int n) { return ::MulDiv(n, ownerDpi ? ownerDpi : 96, 96); };
    MONITORINFO monitor{sizeof(monitor)};
    const bool workArea = ::GetMonitorInfoW(::MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = workArea ? (std::min)(scale(700), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left)) : scale(700);
    const int height = workArea ? (std::min)(scale(370), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top)) : scale(370);
    settings = CreateWindowExW(WS_EX_DLGMODALFRAME, Class,
                               provider == Provider::OpenAI ? plugin_ui::Text(L"Translation — OpenAI-compatible configuration", L"翻译 — OpenAI 兼容设置")
                                                            : plugin_ui::Text(L"Translation — LibreTranslate configuration", L"翻译 — LibreTranslate 设置"),
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, width,
                               height, owner, nullptr, module, &state);
    if (!settings)
        return;
    auto child = [&](const wchar_t *type, const wchar_t *text, DWORD style, int id, int x, int y, int width,
                     int height) {
        HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, settings,
                                      reinterpret_cast<HMENU>(INT_PTR(id)), module, nullptr);
        if (control) state.controls.emplace_back(control, RECT{x, y, x + width, y + height});
        return control;
    };
    child(L"STATIC", plugin_ui::Text(L"Full HTTPS endpoint", L"完整 HTTPS 端点"), 0, 0, 12, 12, 170, 22);
    child(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 10, 180, 10, 490, 25);
    child(L"STATIC", plugin_ui::Text(L"Model ID (OpenAI only)", L"模型 ID（仅 OpenAI）"), 0, 0, 12, 48, 170, 22);
    const auto model = child(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 11, 180, 46, 490, 25);
    child(L"STATIC", plugin_ui::Text(L"API key (optional)", L"API 密钥（可选）"), 0, 0, 12, 84, 170, 22);
    child(L"EDIT", L"", WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP, 12, 180, 82, 490, 25);
    const auto legacy = child(L"BUTTON", plugin_ui::Text(L"Use legacy max_tokens (instead of max_completion_tokens)", L"使用旧版 max_tokens 参数"),
                              BS_AUTOCHECKBOX | WS_TABSTOP, 13, 180, 118, 490, 24);
    child(L"STATIC",
          plugin_ui::Text(
              L"OpenAI requires the full /v1/chat/completions URL. LibreTranslate requires the full /translate URL.\r\n"
              L"Keys are protected for this Windows user. For your own gateway you can leave the key blank.\r\n"
              L"Saving only changes local settings. Each translation asks before sending text.",
              L"OpenAI 需填写完整的 /v1/chat/completions 地址，LibreTranslate 需填写完整的 /translate 地址。\r\n"
              L"密钥由 Windows 针对当前用户保护。使用自己的网关时可留空。\r\n"
              L"保存只修改本地设置。每次翻译前都会询问是否发送文字。"),
          0, 0, 12, 150, 658, 65);
    child(L"STATIC", L"", 0, 20, 12, 218, 658, 45);
    child(L"BUTTON", plugin_ui::Text(L"Save configuration", L"保存设置"), WS_TABSTOP, 1, 12, 274, 155, 28);
    child(L"BUTTON", plugin_ui::Text(L"Cancel", L"取消"), WS_TABSTOP, 2, 177, 274, 90, 28);
    child(L"BUTTON", plugin_ui::Text(L"Forget saved configuration", L"清除已保存的设置"), WS_TABSTOP, 3, 440, 274, 230, 28);
    SendDlgItemMessageW(settings, 10, EM_SETLIMITTEXT, 2048, 0);
    SendDlgItemMessageW(settings, 11, EM_SETLIMITTEXT, 160, 0);
    SendDlgItemMessageW(settings, 12, EM_SETLIMITTEXT, 4096, 0);
    OnlineConfig config;
    std::wstring error;
    if (LoadOnlineConfig(provider, config, error)) {
        SetDlgItemTextW(settings, 10, config.endpoint.c_str());
        SetDlgItemTextW(settings, 11, config.model.c_str());
        SetDlgItemTextW(settings, 12, config.key.c_str());
        SendMessageW(legacy, BM_SETCHECK, config.legacyTokenLimit ? BST_CHECKED : BST_UNCHECKED, 0);
    } else
        SetDlgItemTextW(settings, 20, error.c_str());
    EnableWindow(model, provider == Provider::OpenAI);
    EnableWindow(legacy, provider == Provider::OpenAI);
    Layout(settings, state);
    plugin_ui::AttachTabNavigation(settings);
    ShowWindow(settings, SW_SHOW);
}

void CloseOnlineSettings(HINSTANCE module) {
    if (settings)
        DestroyWindow(settings);
    UnregisterClassW(Class, module);
}
} // namespace nskry::translate
