#pragma once
#include "ui/annotation/annotation_engine.h"
#include "ui/ui_preferences.h"
#include <commctrl.h>
#include <optional>

namespace nskry {
inline bool IsShapeTool(ToolType tool) {
    return tool == ToolType::Rect || tool == ToolType::Ellipse || tool == ToolType::Line;
}

inline std::optional<ToolType> ChooseAnnotationShape(HWND owner, HWND surface, RECT bounds, ToolType current) {
    const auto menu = ::CreatePopupMenu();
    if (!menu) return std::nullopt;
    const ToolType tools[] = {ToolType::Rect, ToolType::Ellipse, ToolType::Line};
    const wchar_t* names[] = {UiText(L"Rectangle", L"矩形"), UiText(L"Ellipse", L"椭圆"), UiText(L"Line", L"直线")};
    for (size_t i = 0; i < std::size(tools); ++i)
        ::AppendMenuW(menu, MF_STRING | (current == tools[i] ? MF_CHECKED : 0), i + 1, names[i]);
    POINT anchor{bounds.left, bounds.bottom};
    ::ClientToScreen(surface, &anchor);
    ::OffsetRect(&bounds, anchor.x - bounds.left, anchor.y - bounds.bottom);
    TPMPARAMS params{sizeof(params), bounds};
    const auto choice = ::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_VERTICAL,
                                          anchor.x, anchor.y, owner, &params);
    ::DestroyMenu(menu);
    return choice && choice <= std::size(tools) ? std::optional{tools[choice - 1]} : std::nullopt;
}

inline constexpr int kMosaicSliderWidth = 168;
inline const wchar_t* MosaicStrengthLabel() { return UiText(L"Strength", L"强度"); }

class MosaicStrengthSlider {
public:
    void Update(HWND surface, HWND keyboardOwner, const RECT& bounds, UINT dpi, const AnnotationEngine& engine) {
        if (!m_control) {
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
            ::InitCommonControlsEx(&controls);
            m_control = ::CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
                WS_CHILD | WS_TABSTOP | TBS_NOTICKS | TBS_TOOLTIPS | TBS_TRANSPARENTBKGND,
                0, 0, 0, 0, surface, nullptr, ::GetModuleHandleW(nullptr), nullptr);
            if (!m_control) return;
            ::SetWindowSubclass(m_control, KeyRelay, 1, reinterpret_cast<DWORD_PTR>(keyboardOwner));
            ::SendMessageW(m_control, TBM_SETRANGE, FALSE,
                MAKELPARAM(AnnotationEngine::MinMosaicBlockSize, AnnotationEngine::MaxMosaicBlockSize));
        }
        ::SetWindowTextW(m_control, UiText(L"Mosaic strength", L"马赛克强度"));
        ::SendMessageW(m_control, TBM_SETPOS, TRUE, engine.GetMosaicBlockSize());
        const int left = (std::min)(bounds.left + ::MulDiv(58, dpi, 96), bounds.right);
        ::SetWindowPos(m_control, HWND_TOP, left, bounds.top, bounds.right - left,
                       bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    bool Read(LPARAM source, AnnotationEngine& engine) const {
        if (!m_control || reinterpret_cast<HWND>(source) != m_control) return false;
        engine.SetMosaicBlockSize(static_cast<int>(::SendMessageW(m_control, TBM_GETPOS, 0, 0)));
        return true;
    }

    void Hide() { if (m_control) ::ShowWindow(m_control, SW_HIDE); }
    void Focus() { if (m_control) ::SetFocus(m_control); }

    static void DrawLabel(HDC dc, RECT bounds, UINT dpi, COLORREF color) {
        bounds.left += ::MulDiv(5, dpi, 96);
        bounds.right = (std::min)(bounds.right, bounds.left + ::MulDiv(53, dpi, 96));
        ::SetTextColor(dc, color);
        ::DrawTextW(dc, MosaicStrengthLabel(), -1, &bounds, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

private:
    static LRESULT CALLBACK KeyRelay(HWND control, UINT message, WPARAM key, LPARAM data, UINT_PTR id, DWORD_PTR owner) {
        if (message == WM_KEYDOWN && (key == VK_TAB || key == VK_ESCAPE)) {
            const auto window = reinterpret_cast<HWND>(owner);
            ::SetFocus(window);
            ::SendMessageW(window, message, key, data);
            return 0;
        }
        if (message == WM_NCDESTROY) ::RemoveWindowSubclass(control, KeyRelay, id);
        return ::DefSubclassProc(control, message, key, data);
    }

    HWND m_control{};
};
} // namespace nskry
