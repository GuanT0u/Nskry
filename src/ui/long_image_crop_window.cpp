#include "pch.h"
#include "ui/long_image_crop_window.h"

#include <algorithm>
#include <windowsx.h>

namespace nskry {
namespace {
constexpr int kApplyId = 1;
constexpr int kCancelId = 2;
constexpr int kCanvasMargin = 14;
constexpr int kControlsHeight = 62;
constexpr int kMinimumCropHeight = 24;
}

LongImageCropWindow::LongImageCropWindow(HBITMAP bitmap, int width, int height,
                                         ApplyCallback apply, CloseCallback closed)
    : m_bitmap(bitmap), m_width(width), m_height(height),
      m_cropBottom(height), m_apply(std::move(apply)), m_closed(std::move(closed)) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_SIZEWE);
    wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
    wc.lpszClassName = kClassName;
    static const ATOM ignored = ::RegisterClassExW(&wc);
    (void)ignored;

    m_hwnd = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName,
        L"Long Screenshot - Adjust height", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 760, nullptr, nullptr,
        ::GetModuleHandleW(nullptr), this);
}

LongImageCropWindow::~LongImageCropWindow() {
    if (m_hwnd) ::DestroyWindow(m_hwnd);
    if (m_bitmap) ::DeleteObject(m_bitmap);
}

void LongImageCropWindow::Show(HWND owner) {
    if (!m_hwnd) return;
    if (owner) ::SetWindowLongPtrW(m_hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    LayoutControls();
    ::ShowWindow(m_hwnd, SW_SHOW);
    ::SetForegroundWindow(m_hwnd);
}

LRESULT CALLBACK LongImageCropWindow::WndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<LongImageCropWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(hwnd, message, wp, lp) : ::DefWindowProcW(hwnd, message, wp, lp);
}

LRESULT LongImageCropWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        m_applyButton = ::CreateWindowExW(0, L"BUTTON", L"Open annotation editor", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            0, 0, 160, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyId)), nullptr, nullptr);
        m_cancelButton = ::CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            0, 0, 86, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)), nullptr, nullptr);
        LayoutControls();
        return 0;
    case WM_SIZE:
        LayoutControls();
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_LBUTTONDOWN: {
        const RECT image = ImageRect();
        const int y = GET_Y_LPARAM(lp);
        const int topY = image.top + static_cast<int>((static_cast<double>(m_cropTop) / m_height) * (image.bottom - image.top));
        const int bottomY = image.top + static_cast<int>((static_cast<double>(m_cropBottom) / m_height) * (image.bottom - image.top));
        if (GET_X_LPARAM(lp) >= image.left - 12 && GET_X_LPARAM(lp) <= image.right + 12 && abs(y - topY) <= 12) m_drag = DragHandle::Top;
        else if (GET_X_LPARAM(lp) >= image.left - 12 && GET_X_LPARAM(lp) <= image.right + 12 && abs(y - bottomY) <= 12) m_drag = DragHandle::Bottom;
        if (m_drag != DragHandle::None) ::SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (m_drag != DragHandle::None) {
            const int sourceY = SourceYFromClientY(GET_Y_LPARAM(lp));
            if (m_drag == DragHandle::Top) m_cropTop = (std::clamp)(sourceY, 0, m_cropBottom - kMinimumCropHeight);
            else m_cropBottom = (std::clamp)(sourceY, m_cropTop + kMinimumCropHeight, m_height);
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (m_drag != DragHandle::None) { m_drag = DragHandle::None; ::ReleaseCapture(); }
        return 0;
    case WM_SETCURSOR:
        if (m_drag != DragHandle::None) { ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS)); return TRUE; }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == kApplyId) { ApplyCrop(); return 0; }
        if (LOWORD(wp) == kCancelId) { ::DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE:
        ::DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        m_hwnd = nullptr;
        if (m_closed) m_closed();
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, wp, lp);
}

void LongImageCropWindow::LayoutControls() {
    if (!m_hwnd || !m_applyButton || !m_cancelButton) return;
    RECT client{}; ::GetClientRect(m_hwnd, &client);
    const int y = client.bottom - 43;
    ::SetWindowPos(m_cancelButton, nullptr, client.right - 100, y, 86, 30, SWP_NOZORDER);
    ::SetWindowPos(m_applyButton, nullptr, client.right - 270, y, 160, 30, SWP_NOZORDER);
}

RECT LongImageCropWindow::ImageRect() const {
    if (!m_hwnd || m_width <= 0 || m_height <= 0) return {};
    RECT client{}; ::GetClientRect(m_hwnd, &client);
    const int availableWidth = (std::max)(1, static_cast<int>(client.right) - 2 * kCanvasMargin);
    const int availableHeight = (std::max)(1, static_cast<int>(client.bottom) - kControlsHeight - 2 * kCanvasMargin);
    const double scale = (std::min)(static_cast<double>(availableWidth) / m_width, static_cast<double>(availableHeight) / m_height);
    const int width = (std::max)(1, static_cast<int>(m_width * scale));
    const int height = (std::max)(1, static_cast<int>(m_height * scale));
    return { (client.right - width) / 2, kCanvasMargin, (client.right + width) / 2, kCanvasMargin + height };
}

int LongImageCropWindow::SourceYFromClientY(int y) const {
    const RECT image = ImageRect();
    if (image.bottom <= image.top) return 0;
    const double ratio = static_cast<double>(y - image.top) / (image.bottom - image.top);
    return static_cast<int>(ratio * m_height);
}

void LongImageCropWindow::Paint(HWND hwnd) {
    PAINTSTRUCT ps{}; HDC dc = ::BeginPaint(hwnd, &ps);
    RECT client{}; ::GetClientRect(hwnd, &client);
    HBRUSH background = ::CreateSolidBrush(RGB(30, 31, 36)); ::FillRect(dc, &client, background); ::DeleteObject(background);
    const RECT image = ImageRect();
    if (m_bitmap && image.right > image.left && image.bottom > image.top) {
        HDC memory = ::CreateCompatibleDC(dc); HGDIOBJ old = ::SelectObject(memory, m_bitmap);
        ::SetStretchBltMode(dc, HALFTONE);
        ::StretchBlt(dc, image.left, image.top, image.right - image.left, image.bottom - image.top,
            memory, 0, 0, m_width, m_height, SRCCOPY);
        ::SelectObject(memory, old); ::DeleteDC(memory);

        const int topY = image.top + static_cast<int>((static_cast<double>(m_cropTop) / m_height) * (image.bottom - image.top));
        const int bottomY = image.top + static_cast<int>((static_cast<double>(m_cropBottom) / m_height) * (image.bottom - image.top));
        HBRUSH shade = ::CreateSolidBrush(RGB(18, 18, 22));
        RECT topShade{ image.left, image.top, image.right, topY };
        RECT bottomShade{ image.left, bottomY, image.right, image.bottom };
        ::FillRect(dc, &topShade, shade); ::FillRect(dc, &bottomShade, shade); ::DeleteObject(shade);
        HPEN pen = ::CreatePen(PS_SOLID, 2, RGB(0, 174, 255)); HGDIOBJ oldPen = ::SelectObject(dc, pen);
        ::MoveToEx(dc, image.left - 8, topY, nullptr); ::LineTo(dc, image.right + 8, topY);
        ::MoveToEx(dc, image.left - 8, bottomY, nullptr); ::LineTo(dc, image.right + 8, bottomY);
        ::SelectObject(dc, oldPen); ::DeleteObject(pen);
    }
    ::SetBkMode(dc, TRANSPARENT); ::SetTextColor(dc, RGB(235, 235, 235));
    const wchar_t* help = L"Drag the blue lines to remove extra height, then open the annotation toolbar.";
    ::TextOutW(dc, kCanvasMargin, client.bottom - 38, help, static_cast<int>(wcslen(help)));
    ::EndPaint(hwnd, &ps);
}

HBITMAP LongImageCropWindow::CreateCroppedBitmap() const {
    const int cropHeight = m_cropBottom - m_cropTop;
    if (!m_bitmap || cropHeight <= 0) return nullptr;
    HDC screen = ::GetDC(nullptr); HDC source = ::CreateCompatibleDC(screen); HDC target = ::CreateCompatibleDC(screen);
    HBITMAP result = ::CreateCompatibleBitmap(screen, m_width, cropHeight);
    if (!result) { ::DeleteDC(source); ::DeleteDC(target); ::ReleaseDC(nullptr, screen); return nullptr; }
    HGDIOBJ oldSource = ::SelectObject(source, m_bitmap); HGDIOBJ oldTarget = ::SelectObject(target, result);
    const bool copied = ::BitBlt(target, 0, 0, m_width, cropHeight, source, 0, m_cropTop, SRCCOPY) != FALSE;
    ::SelectObject(source, oldSource); ::SelectObject(target, oldTarget);
    ::DeleteDC(source); ::DeleteDC(target); ::ReleaseDC(nullptr, screen);
    if (!copied) { ::DeleteObject(result); return nullptr; }
    return result;
}

void LongImageCropWindow::ApplyCrop() {
    HBITMAP cropped = CreateCroppedBitmap();
    if (!cropped) return;
    if (m_apply) m_apply(cropped, m_width, m_cropBottom - m_cropTop);
    else ::DeleteObject(cropped);
    ::DestroyWindow(m_hwnd);
}

} // namespace nskry
