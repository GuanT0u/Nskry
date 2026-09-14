#include "pch.h"
#include "ui/pin_window.h"

namespace nskry {

PinWindow::PinWindow(HBITMAP bitmap, int width, int height)
    : m_bitmap(bitmap), m_width(width), m_height(height)
{
    std::call_once(s_classOnce, [] {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance      = ::GetModuleHandleW(nullptr);
        wc.lpszClassName  = kClassName;
        wc.hbrBackground  = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
        wc.hCursor        = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        ::RegisterClassExW(&wc);
    });

    // Initial window size: clamp to reasonable screen fraction
    constexpr int kMaxInitW = 800;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    int winW = width, winH = height;
    if (winW > kMaxInitW) {
        winW = kMaxInitW;
        winH = static_cast<int>(winW / aspect);
    }
    if (winH < 60) {
        winH = 60;
        winW = static_cast<int>(winH * aspect);
    }

    const DWORD style   = WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;

    RECT rc = { 0, 0, winW, winH };
    ::AdjustWindowRectEx(&rc, style, FALSE, exStyle);

    m_hwnd = ::CreateWindowExW(
        exStyle, kClassName, L"Nskry Pin",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
}

PinWindow::~PinWindow() {
    if (m_hwnd) {
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_bitmap) {
        ::DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }
}

void PinWindow::Show() {
    if (m_hwnd) {
        ::ShowWindow(m_hwnd, SW_SHOWNA);
        ::UpdateWindow(m_hwnd);
    }
}

LRESULT CALLBACK PinWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }

    auto self = reinterpret_cast<PinWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCHITTEST: {
        LRESULT hit = ::DefWindowProcW(hwnd, msg, wp, lp);
        if (hit == HTCLIENT) return HTCAPTION;   // Draggable client area
        return hit;
    }

    case WM_SIZING: {
        if (!self || self->m_height == 0) break;
        // Free resize for edge drags
        if (wp == WMSZ_LEFT || wp == WMSZ_RIGHT || wp == WMSZ_TOP || wp == WMSZ_BOTTOM) {
            return TRUE;
        }

        // Corner drag — lock aspect ratio, follow cursor naturally
        RECT* r = reinterpret_cast<RECT*>(lp);
        const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_EXSTYLE));

        RECT adj = {0, 0, 0, 0};
        ::AdjustWindowRectEx(&adj, style, FALSE, exStyle);
        int bw = adj.right - adj.left;
        int bh = adj.bottom - adj.top;

        float aspect = static_cast<float>(self->m_width) / static_cast<float>(self->m_height);

        int proposedCW = (r->right - r->left) - bw;
        int proposedCH = (r->bottom - r->top) - bh;
        if (proposedCW < 1) proposedCW = 1;
        if (proposedCH < 1) proposedCH = 1;

        int cwFromH = static_cast<int>(proposedCH * aspect);
        int chFromW = static_cast<int>(proposedCW / aspect);

        int finalCW, finalCH;
        if (cwFromH > proposedCW) {
            finalCW = proposedCW;
            finalCH = chFromW;
        } else {
            finalCW = cwFromH;
            finalCH = proposedCH;
        }

        bool anchorRight  = (wp == WMSZ_TOPLEFT  || wp == WMSZ_BOTTOMLEFT);
        bool anchorBottom = (wp == WMSZ_TOPLEFT  || wp == WMSZ_TOPRIGHT);

        if (anchorRight) r->left   = r->right  - finalCW - bw;
        else             r->right  = r->left   + finalCW + bw;

        if (anchorBottom) r->top   = r->bottom - finalCH - bh;
        else              r->bottom = r->top   + finalCH + bh;

        return TRUE;
    }

    case WM_PAINT:
        if (self) { self->OnPaint(hwnd); return 0; }
        break;

    case WM_SIZE:
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        if (self) self->m_hwnd = nullptr;
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void PinWindow::OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = ::BeginPaint(hwnd, &ps);

    RECT rc{};
    ::GetClientRect(hwnd, &rc);

    HDC hdcMem = ::CreateCompatibleDC(hdc);
    HGDIOBJ old = ::SelectObject(hdcMem, m_bitmap);

    // High-quality stretch
    ::SetStretchBltMode(hdc, HALFTONE);
    ::SetBrushOrgEx(hdc, 0, 0, nullptr);

    ::StretchBlt(hdc, 0, 0, rc.right, rc.bottom,
                 hdcMem, 0, 0, m_width, m_height, SRCCOPY);

    ::SelectObject(hdcMem, old);
    ::DeleteDC(hdcMem);

    ::EndPaint(hwnd, &ps);
}

} // namespace nskry
