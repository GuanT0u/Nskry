#include "online_settings.h"

namespace nskry::translate {
namespace {
constexpr wchar_t Class[] = L"NskryTranslationOnlineSettingsV1";
HWND settings{};
struct Settings {
    Provider provider;
};
std::wstring Text(HWND hwnd, int id) {
    HWND edit = GetDlgItem(hwnd, id);
    std::wstring text(GetWindowTextLengthW(edit) + 1, 0);
    const auto length = GetWindowTextW(edit, text.data(), int(text.size()));
    text.resize(length);
    return text;
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto *state = reinterpret_cast<Settings *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    try {
        if (message == WM_NCCREATE) {
            state = static_cast<Settings *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(state));
        }
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
                        L"This provider's saved endpoint/model/key were removed. No request was sent.");
                } else
                    SetDlgItemTextW(hwnd, 20, error.c_str());
            }
            return 0;
        }
        if (message == WM_DESTROY)
            SetDlgItemTextW(hwnd, 12, L"");
        if (message == WM_NCDESTROY) {
            settings = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
    } catch (...) {
        SetDlgItemTextW(hwnd, 20, L"Could not change online settings. No network request was sent.");
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
    state = {provider};
    settings = CreateWindowExW(WS_EX_DLGMODALFRAME, Class,
                               provider == Provider::OpenAI ? L"Translation — OpenAI-compatible configuration"
                                                            : L"Translation — LibreTranslate configuration",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 700,
                               370, owner, nullptr, module, &state);
    if (!settings)
        return;
    auto child = [&](const wchar_t *type, const wchar_t *text, DWORD style, int id, int x, int y, int width,
                     int height) {
        return CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, settings,
                               reinterpret_cast<HMENU>(INT_PTR(id)), module, nullptr);
    };
    child(L"STATIC", L"Full HTTPS endpoint", 0, 0, 12, 12, 170, 22);
    child(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 10, 180, 10, 490, 25);
    child(L"STATIC", L"Model ID (OpenAI only)", 0, 0, 12, 48, 170, 22);
    const auto model = child(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 11, 180, 46, 490, 25);
    child(L"STATIC", L"API key (optional)", 0, 0, 12, 84, 170, 22);
    child(L"EDIT", L"", WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP, 12, 180, 82, 490, 25);
    const auto legacy = child(L"BUTTON", L"Use legacy max_tokens (instead of max_completion_tokens)",
                              BS_AUTOCHECKBOX | WS_TABSTOP, 13, 180, 118, 490, 24);
    child(L"STATIC",
          L"OpenAI: full /v1/chat/completions URL. LibreTranslate: full /translate URL.\r\n"
          L"Keys are protected for this Windows user (DPAPI). Empty key is allowed for your own gateway.\r\n"
          L"Saving does not upload text or test the endpoint. Each translation requires upload confirmation.",
          0, 0, 12, 150, 658, 65);
    child(L"STATIC", L"", 0, 20, 12, 218, 658, 45);
    child(L"BUTTON", L"Save configuration", WS_TABSTOP, 1, 12, 274, 155, 28);
    child(L"BUTTON", L"Cancel", WS_TABSTOP, 2, 177, 274, 90, 28);
    child(L"BUTTON", L"Forget saved configuration", WS_TABSTOP, 3, 440, 274, 230, 28);
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
    ShowWindow(settings, SW_SHOW);
}

void CloseOnlineSettings(HINSTANCE module) {
    if (settings)
        DestroyWindow(settings);
    UnregisterClassW(Class, module);
}
} // namespace nskry::translate
