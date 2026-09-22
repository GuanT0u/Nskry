#include "pch.h"
#include "ui/long_image_crop_window.h"
#include "ui/precision_loupe.h"

#include <algorithm>
#include <windowsx.h>

namespace nskry {
namespace {
constexpr int kApplyId = 1;
constexpr int kCancelId = 2;
constexpr int kZoomInId = 3;
constexpr int kZoomOutId = 4;
constexpr int kImageActionId = 5;
constexpr int kCanvasMargin = 14;
constexpr int kControlsHeight = 62;
constexpr int kMinimumCropHeight = 24;
}

LongImageCropWindow::LongImageCropWindow(HBITMAP bitmap, int width, int height,
                                         ApplyCallback apply, CloseCallback closed,
                                         ImageActions imageActions)
    : m_bitmap(bitmap), m_width(width), m_height(height),
      m_cropBottom(height), m_apply(std::move(apply)), m_closed(std::move(closed)),
      m_imageActions(std::move(imageActions)) {
    if (!m_bitmap || m_width <= 0 || m_height < kMinimumCropHeight) return;
    BITMAP bitmapInfo{};
    if (::GetObjectW(m_bitmap, sizeof(bitmapInfo), &bitmapInfo) != sizeof(bitmapInfo) ||
        bitmapInfo.bmWidth <= 0 || bitmapInfo.bmHeight == 0) {
        return;
    }
    m_width = (std::min)(m_width, static_cast<int>(bitmapInfo.bmWidth));
    const int bitmapHeight = bitmapInfo.bmHeight < 0 ? -bitmapInfo.bmHeight : bitmapInfo.bmHeight;
    m_height = (std::min)(m_height, bitmapHeight);
    if (m_width <= 0 || m_height < kMinimumCropHeight) return;
    m_cropBottom = m_height;
    m_zoomFocusY = m_height / 2;

    std::call_once(s_classOnce, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
        wc.lpszClassName = kClassName;
        ::RegisterClassExW(&wc);
    });

    POINT cursor{};
    ::GetCursorPos(&cursor);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    RECT work{ 0, 0, 1024, 768 };
    const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    if (monitor && ::GetMonitorInfoW(monitor, &monitorInfo)) work = monitorInfo.rcWork;
    const int workW = (std::max)(1, static_cast<int>(work.right - work.left));
    const int workH = (std::max)(1, static_cast<int>(work.bottom - work.top));
    const int windowW = (std::min)(660, workW);
    const int windowH = (std::min)(760, workH);

    m_hwnd = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName,
        L"Long Screenshot - Adjust height", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        work.left + (workW - windowW) / 2, work.top + (workH - windowH) / 2,
        windowW, windowH, nullptr, nullptr,
        ::GetModuleHandleW(nullptr), this);
}

LongImageCropWindow::~LongImageCropWindow() {
    m_destroying = true;
    if (m_hwnd) ::DestroyWindow(m_hwnd);
    if (m_bitmap) ::DeleteObject(m_bitmap);
}

bool LongImageCropWindow::Show(HWND owner) {
    if (!m_hwnd) return false;
    if (owner) ::SetWindowLongPtrW(m_hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    LayoutControls();
    ::ShowWindow(m_hwnd, SW_SHOW);
    ::SetForegroundWindow(m_hwnd);
    return true;
}

LRESULT CALLBACK LongImageCropWindow::WndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<LongImageCropWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCDESTROY && self) {
        self->m_hwnd = nullptr;
        self->m_applyButton = nullptr;
        self->m_cancelButton = nullptr;
        self->m_zoomInButton = nullptr;
        self->m_zoomOutButton = nullptr;
        self->m_imageActionButton = nullptr;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        auto closed = self->m_destroying ? CloseCallback{} : std::move(self->m_closed);
        const LRESULT result = ::DefWindowProcW(hwnd, message, wp, lp);
        if (closed) {
            try { closed(); } catch (...) { /* Never unwind through Win32. */ }
        }
        return result;
    }
    return self ? self->HandleMessage(hwnd, message, wp, lp) : ::DefWindowProcW(hwnd, message, wp, lp);
}

LRESULT LongImageCropWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        m_applyButton = ::CreateWindowExW(0, L"BUTTON", L"Open annotation editor", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            0, 0, 160, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyId)), ::GetModuleHandleW(nullptr), nullptr);
        m_cancelButton = ::CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            0, 0, 86, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)), ::GetModuleHandleW(nullptr), nullptr);
        m_zoomOutButton = ::CreateWindowExW(0, L"BUTTON", L"Zoom -", WS_CHILD | WS_VISIBLE,
            0, 0, 64, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kZoomOutId)), ::GetModuleHandleW(nullptr), nullptr);
        m_zoomInButton = ::CreateWindowExW(0, L"BUTTON", L"Zoom +", WS_CHILD | WS_VISIBLE,
            0, 0, 64, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kZoomInId)), ::GetModuleHandleW(nullptr), nullptr);
        if (!m_imageActions.empty()) {
            const std::wstring label = m_imageActions.size() == 1
                ? L"Select " + m_imageActions.front().label + L" area" : L"Select image area";
            m_imageActionButton = ::CreateWindowExW(0, L"BUTTON", label.c_str(), WS_CHILD | WS_VISIBLE,
                0, 0, 118, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kImageActionId)), ::GetModuleHandleW(nullptr), nullptr);
        }
        if (!m_applyButton || !m_cancelButton || !m_zoomOutButton || !m_zoomInButton ||
            (!m_imageActions.empty() && !m_imageActionButton)) return -1;
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
        if (m_height <= 0 || image.bottom <= image.top) return 0;
        if (m_selectingOcr) {
            const POINT point{ GET_X_LPARAM(lp), y };
            if (::PtInRect(&image, point)) {
                m_drag = DragHandle::OcrArea;
                m_ocrDragStart = SourcePointFromClient(point);
                m_ocrSelection = { m_ocrDragStart.x, m_ocrDragStart.y,
                                   m_ocrDragStart.x + 1, m_ocrDragStart.y + 1 };
                m_dragLastPoint = point;
                ::SetCapture(hwnd);
            }
            return 0;
        }
        const int topY = image.top + static_cast<int>((static_cast<double>(m_cropTop) / m_height) * (image.bottom - image.top));
        const int bottomY = image.top + static_cast<int>((static_cast<double>(m_cropBottom) / m_height) * (image.bottom - image.top));
        if (GET_X_LPARAM(lp) >= image.left - 12 && GET_X_LPARAM(lp) <= image.right + 12 && abs(y - topY) <= 12) m_drag = DragHandle::Top;
        else if (GET_X_LPARAM(lp) >= image.left - 12 && GET_X_LPARAM(lp) <= image.right + 12 && abs(y - bottomY) <= 12) m_drag = DragHandle::Bottom;
        if (m_drag != DragHandle::None) {
            m_dragLastPoint = { GET_X_LPARAM(lp), y };
            m_dragRemainderY = 0.0;
            m_zoomFocusY = m_drag == DragHandle::Top ? m_cropTop : m_cropBottom;
            ::SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (m_drag != DragHandle::None) {
            const int pointerX = GET_X_LPARAM(lp);
            const int pointerY = GET_Y_LPARAM(lp);
            if (m_drag == DragHandle::OcrArea) {
                const POINT current = SourcePointFromClient({ pointerX, pointerY });
                m_ocrSelection.left = (std::min)(m_ocrDragStart.x, current.x);
                m_ocrSelection.top = (std::min)(m_ocrDragStart.y, current.y);
                m_ocrSelection.right = (std::min)(m_width, static_cast<int>((std::max)(m_ocrDragStart.x, current.x)) + 1);
                m_ocrSelection.bottom = (std::min)(m_height, static_cast<int>((std::max)(m_ocrDragStart.y, current.y)) + 1);
                m_dragLastPoint = { pointerX, pointerY };
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const bool precision = (::GetKeyState(VK_MENU) & 0x8000) != 0;
            int sourceY = SourceYFromClientY(pointerY);
            if (precision) {
                const RECT image = ImageRect();
                const double sourcePerPixel = image.bottom > image.top
                    ? static_cast<double>(m_height) / (image.bottom - image.top) : 0.0;
                const double scaled = m_dragRemainderY + (pointerY - m_dragLastPoint.y) * sourcePerPixel * 0.125;
                const int step = static_cast<int>(std::trunc(scaled));
                m_dragRemainderY = scaled - step;
                sourceY = (m_drag == DragHandle::Top ? m_cropTop : m_cropBottom) + step;
            } else {
                m_dragRemainderY = 0.0;
            }
            if (m_drag == DragHandle::Top) m_cropTop = (std::clamp)(sourceY, 0, m_cropBottom - kMinimumCropHeight);
            else m_cropBottom = (std::clamp)(sourceY, m_cropTop + kMinimumCropHeight, m_height);
            m_zoomFocusY = m_drag == DragHandle::Top ? m_cropTop : m_cropBottom;
            m_dragLastPoint = { pointerX, pointerY };
            if (precision) {
                // The actual pointer follows the damped edge. This keeps the
                // loupe's centre cross, the cursor, and the edited boundary
                // visually aligned instead of showing the unscaled raw input.
                const RECT image = ImageRect();
                const int edgeY = image.top + static_cast<int>(
                    (static_cast<double>(m_drag == DragHandle::Top ? m_cropTop : m_cropBottom) / m_height) *
                    (image.bottom - image.top));
                POINT screenPoint{ pointerX, edgeY };
                ::ClientToScreen(hwnd, &screenPoint);
                ::SetCursorPos(screenPoint.x, screenPoint.y);
                m_dragLastPoint.y = edgeY;
            }
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (m_drag != DragHandle::None) { m_drag = DragHandle::None; m_dragRemainderY = 0.0; ::ReleaseCapture(); }
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        m_drag = DragHandle::None;
        return 0;
    case WM_MOUSEWHEEL:
        ScrollView(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_SETCURSOR: {
        if (LOWORD(lp) != HTCLIENT) break;
        POINT point{};
        ::GetCursorPos(&point);
        ::ScreenToClient(hwnd, &point);
        const RECT image = ImageRect();
        if (m_height > 0 && image.bottom > image.top &&
            point.x >= image.left - 12 && point.x <= image.right + 12) {
            const int topY = image.top + static_cast<int>((static_cast<double>(m_cropTop) / m_height) * (image.bottom - image.top));
            const int bottomY = image.top + static_cast<int>((static_cast<double>(m_cropBottom) / m_height) * (image.bottom - image.top));
            if (m_drag != DragHandle::None || abs(point.y - topY) <= 12 || abs(point.y - bottomY) <= 12) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
        }
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == kApplyId) { ApplyCrop(); return 0; }
        if (LOWORD(wp) == kCancelId) { ::DestroyWindow(hwnd); return 0; }
        if (LOWORD(wp) == kZoomInId) { m_zoom = (std::min)(16.0, m_zoom * 1.4); m_zoomFocusY = (m_cropTop + m_cropBottom) / 2; CenterViewOn(m_zoomFocusY); ::InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (LOWORD(wp) == kZoomOutId) { m_zoom = (std::max)(1.0, m_zoom / 1.4); m_zoomFocusY = (m_cropTop + m_cropBottom) / 2; CenterViewOn(m_zoomFocusY); ::InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (LOWORD(wp) == kImageActionId) {
            if (!m_selectingOcr) {
                m_selectingOcr = true;
                m_ocrSelection = { 0, m_cropTop, m_width, m_cropBottom };
                ::SetWindowTextW(m_imageActionButton,
                    m_imageActions.size() == 1 ? m_imageActions.front().label.c_str() : L"Run image action");
                ::InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                if (m_imageActions.size() == 1) {
                    RunImageAction(0);
                } else {
                    HMENU menu = ::CreatePopupMenu();
                    for (size_t index = 0; menu && index < m_imageActions.size() && index < 100; ++index)
                        ::AppendMenuW(menu, MF_STRING, 600 + static_cast<UINT>(index), m_imageActions[index].label.c_str());
                    RECT button{};
                    ::GetWindowRect(m_imageActionButton, &button);
                    const int command = menu ? ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                        button.left, button.bottom, 0, hwnd, nullptr) : 0;
                    if (menu) ::DestroyMenu(menu);
                    if (command >= 600 && command < 600 + static_cast<int>(m_imageActions.size()))
                        RunImageAction(static_cast<size_t>(command - 600));
                }
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        ::DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, wp, lp);
}

void LongImageCropWindow::LayoutControls() {
    if (!m_hwnd || !m_applyButton || !m_cancelButton || !m_zoomInButton || !m_zoomOutButton) return;
    RECT client{};
    if (!::GetClientRect(m_hwnd, &client)) return;
    const int y = (std::max)(0, static_cast<int>(client.bottom - 43));
    const int cancelX = (std::max)(0, static_cast<int>(client.right - 100));
    const int applyX = (std::max)(0, cancelX - 170);
    const int imageActionX = (std::max)(152, applyX - 126);
    ::SetWindowPos(m_zoomOutButton, nullptr, 12, y, 64, 30, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(m_zoomInButton, nullptr, 82, y, 64, 30, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(m_cancelButton, nullptr, cancelX, y, 86, 30, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(m_applyButton, nullptr, applyX, y, 160, 30, SWP_NOZORDER | SWP_NOACTIVATE);
    if (m_imageActionButton) ::SetWindowPos(m_imageActionButton, nullptr, imageActionX, y, 118, 30, SWP_NOZORDER | SWP_NOACTIVATE);
}

RECT LongImageCropWindow::ImageRect() const {
    if (!m_hwnd || m_width <= 0 || m_height <= 0) return {};
    RECT client{}; ::GetClientRect(m_hwnd, &client);
    const int availableWidth = (std::max)(1, static_cast<int>(client.right) - 2 * kCanvasMargin);
    const int availableHeight = (std::max)(1, static_cast<int>(client.bottom) - kControlsHeight - 2 * kCanvasMargin);
    const double fitScale = (std::min)(static_cast<double>(availableWidth) / m_width, static_cast<double>(availableHeight) / m_height);
    const double scale = fitScale * m_zoom;
    const int width = (std::max)(1, static_cast<int>(m_width * scale));
    const int height = (std::max)(1, static_cast<int>(m_height * scale));
    const int left = (client.right - width) / 2;
    int top = kCanvasMargin + (availableHeight - height) / 2;
    if (height > availableHeight) {
        const double visibleSourceHeight = static_cast<double>(availableHeight) / scale;
        const double maximumTop = (std::max)(0.0, static_cast<double>(m_height) - visibleSourceHeight);
        const double viewTop = (std::clamp)(m_viewTopY, 0.0, maximumTop);
        top = kCanvasMargin - static_cast<int>(viewTop * scale);
    }
    return { left, top, left + width, top + height };
}

void LongImageCropWindow::CenterViewOn(int sourceY) {
    if (!m_hwnd || m_width <= 0 || m_height <= 0) return;
    RECT client{};
    if (!::GetClientRect(m_hwnd, &client)) return;
    const int availableWidth = (std::max)(1, static_cast<int>(client.right) - 2 * kCanvasMargin);
    const int availableHeight = (std::max)(1, static_cast<int>(client.bottom) - kControlsHeight - 2 * kCanvasMargin);
    const double scale = (std::min)(static_cast<double>(availableWidth) / m_width,
                                    static_cast<double>(availableHeight) / m_height) * m_zoom;
    if (scale <= 0.0 || m_height * scale <= availableHeight) { m_viewTopY = 0.0; return; }
    const double visibleSourceHeight = static_cast<double>(availableHeight) / scale;
    const double maximumTop = (std::max)(0.0, static_cast<double>(m_height) - visibleSourceHeight);
    m_viewTopY = (std::clamp)(static_cast<double>(sourceY) - visibleSourceHeight / 2.0, 0.0, maximumTop);
}

void LongImageCropWindow::ScrollView(int wheelDelta) {
    if (m_zoom <= 1.0 || wheelDelta == 0 || !m_hwnd) return;
    RECT client{};
    if (!::GetClientRect(m_hwnd, &client)) return;
    const int availableWidth = (std::max)(1, static_cast<int>(client.right) - 2 * kCanvasMargin);
    const int availableHeight = (std::max)(1, static_cast<int>(client.bottom) - kControlsHeight - 2 * kCanvasMargin);
    const double scale = (std::min)(static_cast<double>(availableWidth) / m_width,
                                    static_cast<double>(availableHeight) / m_height) * m_zoom;
    if (scale <= 0.0 || m_height * scale <= availableHeight) return;
    const double visibleSourceHeight = static_cast<double>(availableHeight) / scale;
    const double maximumTop = (std::max)(0.0, static_cast<double>(m_height) - visibleSourceHeight);
    m_viewTopY = (std::clamp)(m_viewTopY - (static_cast<double>(wheelDelta) / WHEEL_DELTA) * visibleSourceHeight * 0.20,
                              0.0, maximumTop);
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

int LongImageCropWindow::SourceYFromClientY(int y) const {
    const RECT image = ImageRect();
    if (image.bottom <= image.top) return 0;
    const double ratio = static_cast<double>(y - image.top) / (image.bottom - image.top);
    return static_cast<int>(ratio * m_height);
}

POINT LongImageCropWindow::SourcePointFromClient(POINT point) const {
    const RECT image = ImageRect();
    if (image.right <= image.left || image.bottom <= image.top || m_width <= 0 || m_height <= 0) return {};
    return {
        (std::clamp)(static_cast<int>(static_cast<double>(point.x - image.left) * m_width / (image.right - image.left)), 0, m_width - 1),
        (std::clamp)(SourceYFromClientY(point.y), 0, m_height - 1)
    };
}

void LongImageCropWindow::Paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC paintDc = ::BeginPaint(hwnd, &ps);
    if (!paintDc) return;
    RECT client{};
    ::GetClientRect(hwnd, &client);
    HDC dc = ::CreateCompatibleDC(paintDc);
    HBITMAP back = dc ? ::CreateCompatibleBitmap(paintDc, client.right - client.left, client.bottom - client.top) : nullptr;
    HGDIOBJ oldBack = back ? ::SelectObject(dc, back) : nullptr;
    if (!dc || !back || !oldBack || oldBack == HGDI_ERROR) {
        if (back) ::DeleteObject(back);
        if (dc) ::DeleteDC(dc);
        ::EndPaint(hwnd, &ps);
        return;
    }
    HBRUSH background = ::CreateSolidBrush(RGB(30, 31, 36));
    ::FillRect(dc, &client, background ? background : static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
    if (background) ::DeleteObject(background);
    const RECT image = ImageRect();
    if (m_bitmap && image.right > image.left && image.bottom > image.top) {
        HDC memory = ::CreateCompatibleDC(dc);
        HGDIOBJ old = memory ? ::SelectObject(memory, m_bitmap) : nullptr;
        if (old && old != HGDI_ERROR) {
            ::SetStretchBltMode(dc, HALFTONE);
            ::SetBrushOrgEx(dc, 0, 0, nullptr);
            ::StretchBlt(dc, image.left, image.top, image.right - image.left, image.bottom - image.top,
                memory, 0, 0, m_width, m_height, SRCCOPY);
        }

        const int topY = image.top + static_cast<int>((static_cast<double>(m_cropTop) / m_height) * (image.bottom - image.top));
        const int bottomY = image.top + static_cast<int>((static_cast<double>(m_cropBottom) / m_height) * (image.bottom - image.top));
        HBRUSH shade = ::CreateSolidBrush(RGB(18, 18, 22));
        RECT topShade{ image.left, image.top, image.right, topY };
        RECT bottomShade{ image.left, bottomY, image.right, image.bottom };
        if (shade) {
            ::FillRect(dc, &topShade, shade);
            ::FillRect(dc, &bottomShade, shade);
            ::DeleteObject(shade);
        }
        HPEN pen = ::CreatePen(PS_SOLID, 2, RGB(0, 174, 255));
        HGDIOBJ oldPen = pen ? ::SelectObject(dc, pen) : nullptr;
        if (oldPen && oldPen != HGDI_ERROR) {
            ::MoveToEx(dc, image.left - 8, topY, nullptr); ::LineTo(dc, image.right + 8, topY);
            ::MoveToEx(dc, image.left - 8, bottomY, nullptr); ::LineTo(dc, image.right + 8, bottomY);
            ::SelectObject(dc, oldPen);
        }
        if (pen) ::DeleteObject(pen);

        if (m_selectingOcr && m_ocrSelection.right > m_ocrSelection.left && m_ocrSelection.bottom > m_ocrSelection.top) {
            const int left = image.left + static_cast<int>(static_cast<double>(m_ocrSelection.left) / m_width * (image.right - image.left));
            const int right = image.left + static_cast<int>(static_cast<double>(m_ocrSelection.right) / m_width * (image.right - image.left));
            const int selectionTop = image.top + static_cast<int>(static_cast<double>(m_ocrSelection.top) / m_height * (image.bottom - image.top));
            const int selectionBottom = image.top + static_cast<int>(static_cast<double>(m_ocrSelection.bottom) / m_height * (image.bottom - image.top));
            HPEN selectionPen = ::CreatePen(PS_SOLID, 2, RGB(255, 191, 0));
            HGDIOBJ oldSelectionPen = selectionPen ? ::SelectObject(dc, selectionPen) : nullptr;
            HGDIOBJ oldSelectionBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
            ::Rectangle(dc, left, selectionTop, right, selectionBottom);
            if (oldSelectionPen && oldSelectionPen != HGDI_ERROR) ::SelectObject(dc, oldSelectionPen);
            ::SelectObject(dc, oldSelectionBrush);
            if (selectionPen) ::DeleteObject(selectionPen);
        }

        if (m_drag != DragHandle::None && memory && old && old != HGDI_ERROR) {
            // Inspect the pixel physically under the cursor, not the centre
            // of the long image. This matches the selection-overlay loupe.
            const int sourceX = (std::clamp)(static_cast<int>(
                static_cast<double>(m_dragLastPoint.x - image.left) * m_width / (image.right - image.left)),
                0, m_width - 1);
            const int sourceY = (std::clamp)(SourceYFromClientY(m_dragLastPoint.y), 0, m_height - 1);
            wchar_t coordinate[80]{};
            swprintf_s(coordinate, L"Image: %d, %d%s", sourceX, sourceY,
                       (::GetKeyState(VK_MENU) & 0x8000) ? L"  (Alt precision)" : L"");
            DrawPrecisionLoupe(dc, memory, RECT{ 0, 0, m_width, m_height }, POINT{ sourceX, sourceY },
                               m_dragLastPoint, client, coordinate);
        }
        if (memory && old && old != HGDI_ERROR) ::SelectObject(memory, old);
        if (memory) ::DeleteDC(memory);
    }
    ::SetBkMode(dc, TRANSPARENT); ::SetTextColor(dc, RGB(235, 235, 235));
    const wchar_t* help = m_selectingOcr
        ? L"Drag a gold rectangle around the target content, then run the selected image action"
        : L"Drag blue lines to crop · Zoom +/- for detail · Hold Alt for precision";
    ::TextOutW(dc, kCanvasMargin, client.bottom - 38, help, static_cast<int>(wcslen(help)));
    ::BitBlt(paintDc, 0, 0, client.right - client.left, client.bottom - client.top, dc, 0, 0, SRCCOPY);
    ::SelectObject(dc, oldBack);
    ::DeleteObject(back);
    ::DeleteDC(dc);
    ::EndPaint(hwnd, &ps);
}

HBITMAP LongImageCropWindow::CreateCroppedBitmap() const {
    return CreateRegionBitmap({ 0, m_cropTop, m_width, m_cropBottom });
}

HBITMAP LongImageCropWindow::CreateRegionBitmap(RECT sourceRegion) const {
    const int cropWidth = sourceRegion.right - sourceRegion.left;
    const int cropHeight = sourceRegion.bottom - sourceRegion.top;
    if (!m_bitmap || m_width <= 0 || cropWidth <= 0 || cropHeight <= 0 ||
        sourceRegion.left < 0 || sourceRegion.top < 0 || sourceRegion.right > m_width || sourceRegion.bottom > m_height) return nullptr;
    HDC screen = ::GetDC(nullptr);
    if (!screen) return nullptr;
    HDC source = ::CreateCompatibleDC(screen);
    HDC target = ::CreateCompatibleDC(screen);
    if (!source || !target) {
        if (source) ::DeleteDC(source);
        if (target) ::DeleteDC(target);
        ::ReleaseDC(nullptr, screen);
        return nullptr;
    }
    HBITMAP result = ::CreateCompatibleBitmap(screen, cropWidth, cropHeight);
    if (!result) { ::DeleteDC(source); ::DeleteDC(target); ::ReleaseDC(nullptr, screen); return nullptr; }
    HGDIOBJ oldSource = ::SelectObject(source, m_bitmap);
    HGDIOBJ oldTarget = ::SelectObject(target, result);
    const bool selected = oldSource && oldSource != HGDI_ERROR && oldTarget && oldTarget != HGDI_ERROR;
    const bool copied = selected &&
        ::BitBlt(target, 0, 0, cropWidth, cropHeight, source, sourceRegion.left, sourceRegion.top, SRCCOPY) != FALSE;
    if (oldSource && oldSource != HGDI_ERROR) ::SelectObject(source, oldSource);
    if (oldTarget && oldTarget != HGDI_ERROR) ::SelectObject(target, oldTarget);
    ::DeleteDC(source); ::DeleteDC(target); ::ReleaseDC(nullptr, screen);
    if (!copied) { ::DeleteObject(result); return nullptr; }
    return result;
}

void LongImageCropWindow::ApplyCrop() {
    HBITMAP cropped = CreateCroppedBitmap();
    if (!cropped) {
        ::MessageBoxW(m_hwnd, L"Failed to crop the screenshot.", L"Nskry", MB_ICONERROR);
        return;
    }
    if (m_apply) {
        try {
            m_apply(cropped, m_width, m_cropBottom - m_cropTop);
        } catch (...) {
            // Ownership transfers when the callback is invoked; it remains the
            // callback's responsibility even when it reports failure by throwing.
            ::MessageBoxW(m_hwnd, L"Failed to open the annotation editor.", L"Nskry", MB_ICONERROR);
            return;
        }
    } else {
        ::DeleteObject(cropped);
    }
    if (m_hwnd) ::DestroyWindow(m_hwnd);
}

void LongImageCropWindow::RunImageAction(size_t actionIndex) {
    if (actionIndex >= m_imageActions.size()) return;
    HBITMAP cropped = CreateRegionBitmap(m_ocrSelection);
    if (!cropped) {
        ::MessageBoxW(m_hwnd, L"Failed to prepare the selected screenshot area.", L"Nskry", MB_ICONERROR);
        return;
    }
    RECT region{};
    if (m_hwnd) ::GetWindowRect(m_hwnd, &region);
    const ImageAction& action = m_imageActions[actionIndex];
    try {
        if (action.invoke) action.invoke(cropped, m_ocrSelection.right - m_ocrSelection.left,
                                         m_ocrSelection.bottom - m_ocrSelection.top, region, m_hwnd);
    } catch (...) {
        ::DeleteObject(cropped);
        throw;
    }
    ::DeleteObject(cropped);
}

} // namespace nskry
