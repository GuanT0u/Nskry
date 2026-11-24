#pragma once
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <string>
#include <vector>

namespace nskry {
class ToolbarTooltip {
public:
    ~ToolbarTooltip() {
        Reset();
        if (::IsWindow(m_window)) ::DestroyWindow(m_window);
        if (m_font) ::DeleteObject(m_font);
    }

    void Reset() {
        Hide();
        for (const auto& entry : m_entries) ::RemoveWindowSubclass(entry.target, Relay, Id());
        m_entries.clear();
    }

    void Add(HWND owner, HWND target, const RECT& bounds, const wchar_t* text, bool entireWindow = false) {
        Hide(); // Adding tools can reallocate the list of hit targets.
        m_entries.push_back({owner, target, bounds, entireWindow, text});
        ::SetWindowSubclass(target, Relay, Id(), reinterpret_cast<DWORD_PTR>(this));
    }

    void Hide() {
        if (!m_hovered) return;
        ::KillTimer(m_hovered->target, Id());
        if (::IsWindow(m_window)) ::ShowWindow(m_window, SW_HIDE);
        m_hovered = nullptr;
    }

private:
    struct Entry {
        HWND owner, target;
        RECT bounds;
        bool entireWindow;
        std::wstring text;
    };

    UINT_PTR Id() const { return reinterpret_cast<UINT_PTR>(this); }

    void Hover(HWND target, POINT point) {
        const auto next = std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& entry) {
            return entry.target == target && (entry.entireWindow || ::PtInRect(&entry.bounds, point));
        });
        const auto* hovered = next == m_entries.end() ? nullptr : &*next;
        m_position = point;
        if (hovered == m_hovered) return;
        Hide();
        if (hovered) {
            m_hovered = hovered;
            TRACKMOUSEEVENT leave{sizeof(leave), TME_LEAVE, target, 0};
            ::TrackMouseEvent(&leave);
            ::SetTimer(target, Id(), 450, nullptr);
        }
    }

    bool EnsurePopup(HWND owner) {
        if (::IsWindow(m_window)) return true;
        static constexpr wchar_t className[] = L"NskryToolbarTooltip";
        const auto module = ::GetModuleHandleW(nullptr);
        static const bool registered = [module] {
            WNDCLASSW windowClass{};
            windowClass.hInstance = module;
            windowClass.lpfnWndProc = PopupProc;
            windowClass.lpszClassName = className;
            return ::RegisterClassW(&windowClass) != 0;
        }();
        if (!registered) return false;
        m_window = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
            className, nullptr, WS_POPUP, 0, 0, 0, 0, owner, nullptr, module, this);
        return m_window != nullptr;
    }

    void Show(HWND target) {
        ::KillTimer(target, Id());
        if (!m_hovered || m_hovered->target != target || !::IsWindowVisible(target) || ::GetCapture())
            return;
        if (!EnsurePopup(m_hovered->owner)) return;

        UINT dpi = ::GetDpiForWindow(target);
        if (!dpi) dpi = 96;
        const auto dip = [dpi](int n) { return ::MulDiv(n, dpi, 96); };
        if (dpi != m_dpi) {
            if (auto font = ::CreateFontW(-dip(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI")) {
                if (m_font) ::DeleteObject(m_font);
                m_font = font;
                m_dpi = dpi;
            }
        }
        HDC dc = ::GetDC(m_window);
        if (!dc) return;
        const auto old = m_font ? ::SelectObject(dc, m_font) : nullptr;
        SIZE text{};
        ::GetTextExtentPoint32W(dc, m_hovered->text.c_str(), static_cast<int>(m_hovered->text.size()), &text);
        if (old) ::SelectObject(dc, old);
        ::ReleaseDC(m_window, dc);

        POINT anchor = m_position;
        ::ClientToScreen(target, &anchor);
        const int width = text.cx + dip(16), height = text.cy + dip(10);
        POINT origin{anchor.x + dip(8), anchor.y + dip(18)};
        MONITORINFO monitor{sizeof(monitor)};
        if (::GetMonitorInfoW(::MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const auto& work = monitor.rcWork;
            origin.x = std::clamp(origin.x, work.left, (std::max)(work.left, work.right - width));
            if (origin.y + height > work.bottom) origin.y = anchor.y - height - dip(8);
            origin.y = std::clamp(origin.y, work.top, (std::max)(work.top, work.bottom - height));
        }
        ::SetWindowTextW(m_window, m_hovered->text.c_str());
        ::SetWindowPos(m_window, HWND_TOPMOST, origin.x, origin.y, width, height,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ::InvalidateRect(m_window, nullptr, FALSE);
    }

    static LRESULT CALLBACK PopupProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_PAINT) {
            const auto* self = reinterpret_cast<const ToolbarTooltip*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(window, &paint);
            RECT bounds{};
            ::GetClientRect(window, &bounds);
            ::FillRect(dc, &bounds, ::GetSysColorBrush(COLOR_INFOBK));
            ::FrameRect(dc, &bounds, ::GetSysColorBrush(COLOR_INFOTEXT));
            if (self && self->m_hovered) {
                const auto old = self->m_font ? ::SelectObject(dc, self->m_font) : nullptr;
                ::SetTextColor(dc, ::GetSysColor(COLOR_INFOTEXT));
                ::SetBkMode(dc, TRANSPARENT);
                ::DrawTextW(dc, self->m_hovered->text.c_str(), -1, &bounds,
                            DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
                if (old) ::SelectObject(dc, old);
            }
            ::EndPaint(window, &paint);
            return 0;
        }
        return ::DefWindowProcW(window, message, wp, lp);
    }

    static LRESULT CALLBACK Relay(HWND target, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
        auto& self = *reinterpret_cast<ToolbarTooltip*>(data);
        switch (message) {
        case WM_MOUSEMOVE:
            self.Hover(target, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            break;
        case WM_TIMER:
            if (wp == id) { self.Show(target); return 0; }
            break;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MOUSELEAVE: case WM_SHOWWINDOW:
            if (self.m_hovered && self.m_hovered->target == target) self.Hide();
            break;
        case WM_NCDESTROY:
            if (self.m_hovered && self.m_hovered->target == target) self.Hide();
            ::RemoveWindowSubclass(target, Relay, id);
            break;
        }
        return ::DefSubclassProc(target, message, wp, lp);
    }

    HWND m_window{};
    HFONT m_font{};
    UINT m_dpi{};
    std::vector<Entry> m_entries;
    const Entry* m_hovered{};
    POINT m_position{};
};
} // namespace nskry
