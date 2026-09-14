#include "pch.h"
#include "ui/selection_window.h"
#include <windowsx.h>
#pragma comment(lib, "msimg32.lib")

namespace nskry {

// ============================================================================
// Construction
// ============================================================================

SelectionWindow::SelectionWindow(CompletionCallback onComplete)
    : m_onComplete(std::move(onComplete))
{
    m_windows = WindowEnumerator::GetAllVisibleWindows();

    m_vX = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_vY = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    m_vW = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_vH = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

    TakeSnapshot();

    m_font = ::CreateFontW(
        -13, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    std::call_once(s_classOnce, [] {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance      = ::GetModuleHandleW(nullptr);
        wc.lpszClassName  = kClassName;
        wc.hCursor        = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32515));
        ::RegisterClassExW(&wc);
    });

    m_hwnd = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName, L"Nskry Selection", WS_POPUP,
        m_vX, m_vY, m_vW, m_vH,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
}

SelectionWindow::~SelectionWindow() {
    Close();
    CleanupGdi();
    if (m_font) { ::DeleteObject(m_font); m_font = nullptr; }
    // Note: m_capturedBitmap ownership is passed to the caller via FinishWithAction
}

void SelectionWindow::TakeSnapshot() {
    HDC hdcScreen = ::GetDC(nullptr);

    m_hdcSnapshot = ::CreateCompatibleDC(hdcScreen);
    m_bmpSnapshot = ::CreateCompatibleBitmap(hdcScreen, m_vW, m_vH);
    ::SelectObject(m_hdcSnapshot, m_bmpSnapshot);
    ::BitBlt(m_hdcSnapshot, 0, 0, m_vW, m_vH, hdcScreen, m_vX, m_vY, SRCCOPY);

    m_hdcBlack = ::CreateCompatibleDC(hdcScreen);
    m_bmpBlack = ::CreateCompatibleBitmap(hdcScreen, m_vW, m_vH);
    ::SelectObject(m_hdcBlack, m_bmpBlack);
    RECT r{ 0, 0, m_vW, m_vH };
    HBRUSH blackBrush = ::CreateSolidBrush(RGB(0, 0, 0));
    ::FillRect(m_hdcBlack, &r, blackBrush);
    ::DeleteObject(blackBrush);

    ::ReleaseDC(nullptr, hdcScreen);
}

void SelectionWindow::CleanupGdi() {
    if (m_hdcSnapshot) { ::DeleteDC(m_hdcSnapshot); m_hdcSnapshot = nullptr; }
    if (m_bmpSnapshot) { ::DeleteObject(m_bmpSnapshot); m_bmpSnapshot = nullptr; }
    if (m_hdcBlack)    { ::DeleteDC(m_hdcBlack); m_hdcBlack = nullptr; }
    if (m_bmpBlack)    { ::DeleteObject(m_bmpBlack); m_bmpBlack = nullptr; }
}

void SelectionWindow::Show() {
    if (m_hwnd) {
        ::SetWindowPos(m_hwnd, HWND_TOPMOST, m_vX, m_vY, m_vW, m_vH, SWP_SHOWWINDOW);
        ::ShowWindow(m_hwnd, SW_SHOWNA);
        ::UpdateWindow(m_hwnd);
        ::SetForegroundWindow(m_hwnd);
        ::SetFocus(m_hwnd);
    }
}

void SelectionWindow::Close() {
    if (m_hwnd) {
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

// ============================================================================
// WndProc dispatch
// ============================================================================

LRESULT CALLBACK SelectionWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto self = reinterpret_cast<SelectionWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMessage(hwnd, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SelectionWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE:   OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONDOWN: OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONUP:   OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;

    case WM_RBUTTONUP:
        FinishWithAction(SelectionAction::Cancel);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { FinishWithAction(SelectionAction::Cancel); return 0; }
        break;

    case WM_SETCURSOR:
        if (m_state == State::Selected || m_state == State::Adjusting) {
            POINT pt;
            ::GetCursorPos(&pt);
            ::ScreenToClient(hwnd, &pt);
            HitZone zone = (m_state == State::Adjusting) ? m_activeZone : HitTest(pt.x, pt.y);
            UpdateCursor(zone);
            return TRUE;
        }
        break;

    case WM_PAINT:       OnPaint(hwnd); return 0;
    case WM_ERASEBKGND:  return 1;
    case WM_DESTROY:     m_hwnd = nullptr; return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================================
// Hit Testing & Cursors
// ============================================================================

SelectionWindow::HitZone SelectionWindow::HitTest(int x, int y) {
    POINT pt{ x, y };

    // 1. Toolbar
    for (const auto& btn : m_toolbarButtons) {
        if (::PtInRect(&btn.rect, pt)) return HitZone::Toolbar;
    }

    // 2. Adjust handles
    const int grip = 6;
    RECT r = m_finalRect;
    
    // Corners
    RECT tl = { r.left - grip, r.top - grip, r.left + grip, r.top + grip };
    RECT tr = { r.right - grip, r.top - grip, r.right + grip, r.top + grip };
    RECT bl = { r.left - grip, r.bottom - grip, r.left + grip, r.bottom + grip };
    RECT br = { r.right - grip, r.bottom - grip, r.right + grip, r.bottom + grip };
    if (::PtInRect(&tl, pt)) return HitZone::TopLeft;
    if (::PtInRect(&tr, pt)) return HitZone::TopRight;
    if (::PtInRect(&bl, pt)) return HitZone::BottomLeft;
    if (::PtInRect(&br, pt)) return HitZone::BottomRight;

    // Edges
    RECT top = { r.left, r.top - grip, r.right, r.top + grip };
    RECT bot = { r.left, r.bottom - grip, r.right, r.bottom + grip };
    RECT lft = { r.left - grip, r.top, r.left + grip, r.bottom };
    RECT rgt = { r.right - grip, r.top, r.right + grip, r.bottom };
    if (::PtInRect(&top, pt)) return HitZone::Top;
    if (::PtInRect(&bot, pt)) return HitZone::Bottom;
    if (::PtInRect(&lft, pt)) return HitZone::Left;
    if (::PtInRect(&rgt, pt)) return HitZone::Right;

    // Inside
    if (::PtInRect(&r, pt)) return HitZone::Inside;

    return HitZone::None;
}

void SelectionWindow::UpdateCursor(HitZone zone) {
    LPCWSTR id = IDC_CROSS;
    switch (zone) {
    case HitZone::TopLeft:
    case HitZone::BottomRight: id = IDC_SIZENWSE; break;
    case HitZone::TopRight:
    case HitZone::BottomLeft:  id = IDC_SIZENESW; break;
    case HitZone::Top:
    case HitZone::Bottom:      id = IDC_SIZENS; break;
    case HitZone::Left:
    case HitZone::Right:       id = IDC_SIZEWE; break;
    case HitZone::Inside:      id = IDC_SIZEALL; break;
    case HitZone::Toolbar:     id = IDC_HAND; break;
    case HitZone::None:        id = IDC_CROSS; break;
    }
    ::SetCursor(::LoadCursorW(nullptr, id));
}

// ============================================================================
// Mouse handling
// ============================================================================

void SelectionWindow::OnMouseMove(int x, int y) {
    if (m_state == State::Hovering) {
        int sx = x + m_vX, sy = y + m_vY;
        POINT pt{ sx, sy };
        bool found = false;

        for (const auto& wi : m_windows) {
            if (wi.contains(pt)) {
                if (m_targetHwnd != wi.hwnd) {
                    m_targetHwnd   = wi.hwnd;
                    m_targetBounds = wi.bounds;
                    m_hoveredRect  = wi.bounds;
                    ::OffsetRect(&m_hoveredRect, -m_vX, -m_vY);
                    ::InvalidateRect(m_hwnd, nullptr, FALSE);
                }
                found = true;
                break;
            }
        }
        if (!found && m_targetHwnd) {
            m_targetHwnd   = nullptr;
            m_hoveredRect  = {};
            ::InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
    else if (m_state == State::Dragging) {
        m_dragRect.left   = (std::min)(static_cast<int>(m_dragStart.x), x);
        m_dragRect.top    = (std::min)(static_cast<int>(m_dragStart.y), y);
        m_dragRect.right  = (std::max)(static_cast<int>(m_dragStart.x), x);
        m_dragRect.bottom = (std::max)(static_cast<int>(m_dragStart.y), y);
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    else if (m_state == State::Selected) {
        bool needRepaint = false;
        for (auto& btn : m_toolbarButtons) {
            POINT pt{ x, y };
            bool inside = ::PtInRect(&btn.rect, pt);
            if (inside != btn.hovered) {
                btn.hovered = inside;
                needRepaint = true;
            }
        }
        if (needRepaint)
            ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    else if (m_state == State::Adjusting) {
        int dx = x - m_adjustStartPt.x;
        int dy = y - m_adjustStartPt.y;
        RECT r = m_adjustStartRect;

        switch (m_activeZone) {
        case HitZone::Inside:      ::OffsetRect(&r, dx, dy); break;
        case HitZone::Left:        r.left += dx; break;
        case HitZone::Right:       r.right += dx; break;
        case HitZone::Top:         r.top += dy; break;
        case HitZone::Bottom:      r.bottom += dy; break;
        case HitZone::TopLeft:     r.left += dx; r.top += dy; break;
        case HitZone::TopRight:    r.right += dx; r.top += dy; break;
        case HitZone::BottomLeft:  r.left += dx; r.bottom += dy; break;
        case HitZone::BottomRight: r.right += dx; r.bottom += dy; break;
        default: break;
        }

        // Normalize if flipped
        if (r.left > r.right) std::swap(r.left, r.right);
        if (r.top > r.bottom) std::swap(r.top, r.bottom);

        // Clamp to screen
        r.left   = (std::max)(0, static_cast<int>(r.left));
        r.top    = (std::max)(0, static_cast<int>(r.top));
        r.right  = (std::min)(m_vW, static_cast<int>(r.right));
        r.bottom = (std::min)(m_vH, static_cast<int>(r.bottom));

        m_finalRect = r;
        BuildToolbar(m_finalRect);
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void SelectionWindow::OnLButtonDown(int x, int y) {
    if (m_state == State::Hovering && m_targetHwnd) {
        m_state     = State::Dragging;
        m_dragStart = { x, y };
        m_dragRect  = { x, y, x, y };
        ::SetCapture(m_hwnd);
    }
    else if (m_state == State::Selected) {
        HitZone zone = HitTest(x, y);
        if (zone == HitZone::Toolbar) {
            for (const auto& btn : m_toolbarButtons) {
                POINT pt{ x, y };
                if (::PtInRect(&btn.rect, pt)) {
                    FinishWithAction(btn.action);
                    return;
                }
            }
        }
        else if (zone != HitZone::None) {
            // Start adjusting
            m_state = State::Adjusting;
            m_activeZone = zone;
            m_adjustStartPt = { x, y };
            m_adjustStartRect = m_finalRect;
            ::SetCapture(m_hwnd);
        }
        else {
            // Clicked outside, reset to hovering
            m_state = State::Hovering;
            m_toolbarButtons.clear();
            ::InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
}

void SelectionWindow::OnLButtonUp(int x, int y) {
    if (m_state == State::Dragging) {
        ::ReleaseCapture();

        RECT activeRect = m_dragRect;
        if (activeRect.right - activeRect.left < 5 && activeRect.bottom - activeRect.top < 5)
            activeRect = m_hoveredRect;

        if (activeRect.right <= activeRect.left || activeRect.bottom <= activeRect.top) {
            m_state = State::Hovering;
            return;
        }

        m_finalRect = activeRect;
        m_state     = State::Selected;
        BuildToolbar(m_finalRect);
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    else if (m_state == State::Adjusting) {
        ::ReleaseCapture();
        m_state = State::Selected;
        // Toolbar is already rebuilt during MouseMove
    }
}

// ============================================================================
// Toolbar
// ============================================================================

void SelectionWindow::BuildToolbar(RECT selRect) {
    m_toolbarButtons.clear();

    struct BtnDef { const wchar_t* label; SelectionAction action; };
    BtnDef defs[] = {
        { L"\x2398 \x590D\x5236", SelectionAction::Copy },   // ⎘ 复制
        { L"\x2193 \x4FDD\x5B58", SelectionAction::Save },   // ↓ 保存
        { L"\x2197 \x56FA\x5B9A", SelectionAction::Pin  },   // ↗ 固定
        { L"\x25B6 \x76D1\x63A7", SelectionAction::PiP  },   // ▶ 监控
    };

    constexpr int btnW = 72, btnH = 30, gap = 4;
    constexpr int count = 4;
    const int totalW = count * btnW + (count - 1) * gap;

    // Center horizontally relative to selection
    int centerX = (selRect.left + selRect.right) / 2;
    int startX  = centerX - totalW / 2;
    if (startX < 4) startX = 4;
    if (startX + totalW > m_vW - 4) startX = m_vW - 4 - totalW;

    // Position below selection with 8px gap
    int y = selRect.bottom + 8;
    // If below screen bottom, place above selection
    if (y + btnH > m_vH - 4)
        y = selRect.top - btnH - 8;
    if (y < 4) y = 4;

    for (int i = 0; i < count; i++) {
        ToolbarButton btn;
        btn.rect   = { startX + i * (btnW + gap), y,
                        startX + i * (btnW + gap) + btnW, y + btnH };
        btn.label   = defs[i].label;
        btn.action  = defs[i].action;
        btn.hovered = false;
        m_toolbarButtons.push_back(btn);
    }
}

void SelectionWindow::DrawToolbar(HDC hdc) {
    HFONT oldFont = static_cast<HFONT>(::SelectObject(hdc, m_font));
    ::SetBkMode(hdc, TRANSPARENT);

    for (const auto& btn : m_toolbarButtons) {
        // Background
        COLORREF bgColor = btn.hovered ? RGB(45, 120, 215) : RGB(50, 50, 58);
        HBRUSH bgBrush = ::CreateSolidBrush(bgColor);
        // Draw a rounded-ish rectangle (using FillRect for simplicity)
        ::FillRect(hdc, &btn.rect, bgBrush);
        ::DeleteObject(bgBrush);

        // Border
        HBRUSH borderBrush = ::CreateSolidBrush(btn.hovered ? RGB(80, 160, 240) : RGB(75, 75, 85));
        ::FrameRect(hdc, &btn.rect, borderBrush);
        ::DeleteObject(borderBrush);

        // Label text
        ::SetTextColor(hdc, RGB(240, 240, 245));
        RECT textRect = btn.rect;
        ::DrawTextW(hdc, btn.label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    ::SelectObject(hdc, oldFont);
}

void SelectionWindow::DrawInfoBox(HDC hdc, RECT selRect) {
    int w = selRect.right - selRect.left;
    int h = selRect.bottom - selRect.top;

    wchar_t text[64];
    ::_snwprintf_s(text, _TRUNCATE, L"%d \u00D7 %d", w, h);   // "W × H"

    HFONT oldFont = static_cast<HFONT>(::SelectObject(hdc, m_font));
    ::SetBkMode(hdc, TRANSPARENT);

    SIZE sz{};
    ::GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);

    int px = selRect.left;
    int py = selRect.top - sz.cy - 6;
    if (py < 2) py = selRect.top + 2;

    RECT bgRect = { px, py, px + sz.cx + 12, py + sz.cy + 4 };

    HBRUSH bg = ::CreateSolidBrush(RGB(30, 30, 35));
    ::FillRect(hdc, &bgRect, bg);
    ::DeleteObject(bg);

    ::SetTextColor(hdc, RGB(200, 200, 210));
    ::TextOutW(hdc, px + 6, py + 2, text, static_cast<int>(wcslen(text)));

    ::SelectObject(hdc, oldFont);
}

// ============================================================================
// Bitmap capture from snapshot
// ============================================================================

HBITMAP SelectionWindow::CaptureSelectedRegion(RECT localRect) {
    int w = localRect.right  - localRect.left;
    int h = localRect.bottom - localRect.top;
    if (w <= 0 || h <= 0) return nullptr;

    HDC hdcScreen = ::GetDC(nullptr);
    HDC hdcMem    = ::CreateCompatibleDC(hdcScreen);
    HBITMAP hbmp  = ::CreateCompatibleBitmap(hdcScreen, w, h);
    HGDIOBJ old   = ::SelectObject(hdcMem, hbmp);

    ::BitBlt(hdcMem, 0, 0, w, h,
             m_hdcSnapshot, localRect.left, localRect.top, SRCCOPY);

    ::SelectObject(hdcMem, old);
    ::DeleteDC(hdcMem);
    ::ReleaseDC(nullptr, hdcScreen);
    return hbmp;
}

// ============================================================================
// Finish and invoke callback
// ============================================================================

void SelectionWindow::FinishWithAction(SelectionAction action) {
    SelectionResult result{};

    if (action != SelectionAction::Cancel) {
        // Capture the final region on demand
        m_capturedBitmap = CaptureSelectedRegion(m_finalRect);

        result.targetHwnd  = m_targetHwnd;
        result.bitmap      = m_capturedBitmap;
        result.bitmapWidth = m_finalRect.right  - m_finalRect.left;
        result.bitmapHeight= m_finalRect.bottom - m_finalRect.top;

        // Compute crop region relative to the target window
        int screenLeft = m_finalRect.left + m_vX;
        int screenTop  = m_finalRect.top  + m_vY;
        result.crop.x      = screenLeft - m_targetBounds.left;
        result.crop.y      = screenTop  - m_targetBounds.top;
        result.crop.width  = result.bitmapWidth;
        result.crop.height = result.bitmapHeight;

        m_capturedBitmap = nullptr;  // ownership transferred to caller
    } else {
        // Cancel — clean up bitmap if any
        if (m_capturedBitmap) {
            ::DeleteObject(m_capturedBitmap);
            m_capturedBitmap = nullptr;
        }
    }

    Close();

    if (m_onComplete)
        m_onComplete(action, result);
}

// ============================================================================
// Painting
// ============================================================================

void SelectionWindow::OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdcPaint = ::BeginPaint(hwnd, &ps);

    // Double buffering
    HDC hdcBack     = ::CreateCompatibleDC(hdcPaint);
    HBITMAP hbmBack = ::CreateCompatibleBitmap(hdcPaint, m_vW, m_vH);
    HGDIOBJ oldBack = ::SelectObject(hdcBack, hbmBack);

    // 1. Draw original snapshot
    ::BitBlt(hdcBack, 0, 0, m_vW, m_vH, m_hdcSnapshot, 0, 0, SRCCOPY);

    // 2. Overlay dark mask
    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = 120;
    ::AlphaBlend(hdcBack, 0, 0, m_vW, m_vH, m_hdcBlack, 0, 0, m_vW, m_vH, bf);

    // 3. Determine the active rectangle
    RECT activeRect{};
    if (m_state == State::Selected || m_state == State::Adjusting)
        activeRect = m_finalRect;
    else if (m_state == State::Dragging)
        activeRect = m_dragRect;
    else
        activeRect = m_hoveredRect;

    // 4. Clear out the highlighted area (punch through the mask)
    if (activeRect.right > activeRect.left && activeRect.bottom > activeRect.top) {
        ::BitBlt(hdcBack,
            activeRect.left, activeRect.top,
            activeRect.right - activeRect.left, activeRect.bottom - activeRect.top,
            m_hdcSnapshot, activeRect.left, activeRect.top, SRCCOPY);

        // Border (2px blue)
        HBRUSH borderBrush = ::CreateSolidBrush(RGB(20, 150, 255));
        ::FrameRect(hdcBack, &activeRect, borderBrush);
        RECT inner = activeRect;
        ::InflateRect(&inner, -1, -1);
        ::FrameRect(hdcBack, &inner, borderBrush);
        ::DeleteObject(borderBrush);

        // Size info
        DrawInfoBox(hdcBack, activeRect);
    }

    // 5. Draw toolbar if in Selected or Adjusting state
    if (m_state == State::Selected || m_state == State::Adjusting)
        DrawToolbar(hdcBack);

    // 6. Blit to screen
    ::BitBlt(hdcPaint, 0, 0, m_vW, m_vH, hdcBack, 0, 0, SRCCOPY);

    ::SelectObject(hdcBack, oldBack);
    ::DeleteObject(hbmBack);
    ::DeleteDC(hdcBack);

    ::EndPaint(hwnd, &ps);
}

} // namespace nskry
