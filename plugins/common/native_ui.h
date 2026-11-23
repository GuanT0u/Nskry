#pragma once
#include "nskry_ui_preferences.h"
#include <commctrl.h>

namespace nskry::plugin_ui {
struct NativeColors {
    COLORREF window{}, panel{}, text{}, border{};
};

inline NativeColors Colors(NskryUiTheme theme) noexcept {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON))
        return {::GetSysColor(COLOR_WINDOW), ::GetSysColor(COLOR_WINDOW),
                ::GetSysColor(COLOR_WINDOWTEXT), ::GetSysColor(COLOR_WINDOWTEXT)};
    return theme == NSKRY_UI_THEME_DARK
        ? NativeColors{RGB(32,32,32), RGB(43,43,43), RGB(245,245,245), RGB(131,131,131)}
        : NativeColors{RGB(247,247,247), RGB(255,255,255), RGB(32,32,32), RGB(115,115,115)};
}

inline HBRUSH Brush(HDC dc, COLORREF color) noexcept {
    ::SetDCBrushColor(dc, color);
    return static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
}

inline LRESULT Background(HWND window, WPARAM dc, NskryUiTheme theme) {
    RECT bounds{};
    ::GetClientRect(window, &bounds);
    const auto canvas = reinterpret_cast<HDC>(dc);
    ::FillRect(canvas, &bounds, Brush(canvas, Colors(theme).window));
    return TRUE;
}

inline LRESULT ControlColors(WPARAM dc, NskryUiTheme theme, bool field = false) {
    const auto canvas = reinterpret_cast<HDC>(dc);
    const auto colors = Colors(theme);
    const auto background = field ? colors.panel : colors.window;
    ::SetBkColor(canvas, background);
    ::SetTextColor(canvas, colors.text);
    return reinterpret_cast<LRESULT>(Brush(canvas, background));
}

inline HFONT CreateMessageFont(HWND window) {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    const UINT dpi = ::GetDpiForWindow(window);
    return ::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi ? dpi : 96)
        ? ::CreateFontIndirectW(&metrics.lfMessageFont) : nullptr;
}
inline LRESULT CALLBACK TabRelay(HWND window, UINT message, WPARAM key, LPARAM data,
                                 UINT_PTR id, DWORD_PTR dialog) {
    if (message == WM_KEYDOWN && key == VK_TAB && ::GetKeyState(VK_CONTROL) >= 0) {
        const auto parent = reinterpret_cast<HWND>(dialog);
        const auto focus = ::GetFocus();
        const auto next = ::GetNextDlgTabItem(parent, ::IsChild(parent, focus) ? focus : nullptr,
                                             ::GetKeyState(VK_SHIFT) < 0);
        if (next) {
            ::SetFocus(next);
            return 0;
        }
    }
    if (message == WM_NCDESTROY) ::RemoveWindowSubclass(window, TabRelay, id);
    return ::DefSubclassProc(window, message, key, data);
}

inline void AttachTabNavigation(HWND window) {
    if (!window) return;
    constexpr UINT_PTR id = 0x198;
    const auto dialog = reinterpret_cast<DWORD_PTR>(window);
    ::SetWindowSubclass(window, TabRelay, id, dialog);
    ::EnumChildWindows(window, [](HWND child, LPARAM parent) -> BOOL {
        ::SetWindowSubclass(child, TabRelay, id, static_cast<DWORD_PTR>(parent));
        return TRUE;
    }, dialog);
}
} // namespace nskry::plugin_ui
