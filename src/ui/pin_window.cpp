#include "pch.h"
#include "ui/pin_window.h"
#include <algorithm>
#include <windowsx.h>
#include <commdlg.h>

namespace nskry {

// PNG encoder helper for saving
static int GetPngEncoderClsidHelper(CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    auto pInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
    if (!pInfo) return -1;

    Gdiplus::GetImageEncoders(num, size, pInfo);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(pInfo[i].MimeType, L"image/png") == 0) {
            *pClsid = pInfo[i].Clsid;
            free(pInfo);
            return static_cast<int>(i);
        }
    }
    free(pInfo);
    return -1;
}

PinWindow::PinWindow(HBITMAP bitmap, int width, int height, CloseCallback closed)
    : m_bitmap(bitmap), m_width(width), m_height(height), m_closed(std::move(closed))
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

    m_font = ::CreateFontW(
        -12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    const DWORD style   = WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;

    if (!m_bitmap || m_width <= 0 || m_height <= 0) return;

    POINT cursor{};
    ::GetCursorPos(&cursor);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    RECT workArea{ 0, 0, 1024, 768 };
    if (monitor && ::GetMonitorInfoW(monitor, &monitorInfo)) workArea = monitorInfo.rcWork;

    RECT nonClient{ 0, 0, 0, 0 };
    if (!::AdjustWindowRectEx(&nonClient, style, FALSE, exStyle)) return;
    const int nonClientW = nonClient.right - nonClient.left;
    const int nonClientH = nonClient.bottom - nonClient.top;
    const int workW = (std::max)(1, static_cast<int>(workArea.right - workArea.left));
    const int workH = (std::max)(1, static_cast<int>(workArea.bottom - workArea.top));
    const int maxClientW = (std::max)(1, (std::min)(800, workW - nonClientW));
    const int maxClientH = (std::max)(1, workH - nonClientH);
    const double scale = (std::min)({ 1.0,
        static_cast<double>(maxClientW) / m_width,
        static_cast<double>(maxClientH) / m_height });

    constexpr int kToolbarClientWidth = 428;
    int winW = (std::max)(1, static_cast<int>(m_width * scale));
    int winH = (std::max)(60, static_cast<int>(m_height * scale));
    winW = (std::min)((std::max)(winW, (std::min)(kToolbarClientWidth, maxClientW)), maxClientW);
    winH = (std::min)(winH, maxClientH);

    const int outerW = winW + nonClientW;
    const int outerH = winH + nonClientH;
    const int x = workArea.left + (workW - outerW) / 2;
    const int y = workArea.top + (workH - outerH) / 2;

    m_hwnd = ::CreateWindowExW(
        exStyle, kClassName, L"Nskry Pin",
        style,
        x, y, outerW, outerH,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
}

PinWindow::~PinWindow() {
    m_destroying = true;
    CommitTextEdit();
    if (m_font)     { ::DeleteObject(m_font); m_font = nullptr; }
    if (m_hwnd) {
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_bitmap) {
        ::DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }
}

bool PinWindow::Show() {
    if (!m_hwnd) return false;
    ::ShowWindow(m_hwnd, SW_SHOWNA);
    ::UpdateWindow(m_hwnd);
    return true;
}

bool PinWindow::ShowInEditMode() {
    if (!Show()) return false;
    if (!m_isEditing) EnterEditMode();
    if (m_hwnd) ::SetForegroundWindow(m_hwnd);
    return m_hwnd != nullptr;
}

LRESULT CALLBACK PinWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto self = reinterpret_cast<PinWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCDESTROY && self) {
        self->m_hwnd = nullptr;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        auto closed = self->m_destroying ? CloseCallback{} : std::move(self->m_closed);
        const LRESULT result = ::DefWindowProcW(hwnd, msg, wp, lp);
        if (closed) {
            try { closed(self); } catch (...) { /* Never unwind through Win32. */ }
        }
        return result;
    }
    if (self) return self->HandleMessage(hwnd, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PinWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: {
        LRESULT hit = ::DefWindowProcW(hwnd, msg, wp, lp);
        if (hit == HTCLIENT) {
            // When editing, keep HTCLIENT so mouse events go to client drawing
            if (m_isEditing) return HTCLIENT;
            return HTCAPTION; // Draggable client area
        }
        return hit;
    }

    case WM_SIZING: {
        if (m_height == 0) break;
        // Free resize for edge drags
        if (wp == WMSZ_LEFT || wp == WMSZ_RIGHT || wp == WMSZ_TOP || wp == WMSZ_BOTTOM) {
            return TRUE;
        }

        // Corner drag — lock aspect ratio
        RECT* r = reinterpret_cast<RECT*>(lp);
        const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_EXSTYLE));

        RECT adj = {0, 0, 0, 0};
        ::AdjustWindowRectEx(&adj, style, FALSE, exStyle);
        int bw = adj.right - adj.left;
        int bh = adj.bottom - adj.top;

        float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);

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

    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
        if (limits) {
            // The primary toolbar is 420 px wide; keep every action reachable.
            limits->ptMinTrackSize.x = (std::max)(limits->ptMinTrackSize.x, 440L);
            limits->ptMinTrackSize.y = (std::max)(limits->ptMinTrackSize.y, 120L);
        }
        return 0;
    }

    case WM_NCRBUTTONUP:
        if (wp == HTCAPTION) {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ShowContextMenu(pt.x, pt.y);
            return 0;
        }
        break;

    case WM_NCLBUTTONDBLCLK:
        if (wp == HTCAPTION) {
            EnterEditMode();
            return 0;
        }
        break;

    case WM_CONTEXTMENU: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt.x == -1 && pt.y == -1) {
            RECT rc{}; ::GetWindowRect(hwnd, &rc);
            pt = { rc.left + 20, rc.top + 20 };
        }
        ShowContextMenu(pt.x, pt.y);
        return 0;
    }

    case WM_RBUTTONUP: {
        if (m_isEditing) {
            if (m_annotationEngine.GetTool() != ToolType::None) {
                m_annotationEngine.SetTool(ToolType::None);
                RECT rc{}; ::GetClientRect(hwnd, &rc);
                BuildPinToolbar(rc.right, rc.bottom);
                ::InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                FinishEdit(false);
            }
            return 0;
        }
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ::ClientToScreen(hwnd, &pt);
        ShowContextMenu(pt.x, pt.y);
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        if (!m_isEditing) {
            EnterEditMode();
            return 0;
        }
        break;

    case WM_LBUTTONDOWN:
        if (m_isEditing) {
            OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
        if (m_isEditing) {
            OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (m_isEditing) {
            OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (m_isEditing) {
            if (wp == VK_ESCAPE) {
                FinishEdit(false);
                return 0;
            }
            if (::GetKeyState(VK_CONTROL) & 0x8000) {
                if (wp == 'Z') {
                    CommitTextEdit();
                    m_annotationEngine.Undo();
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (wp == 'Y') {
                    CommitTextEdit();
                    m_annotationEngine.Redo();
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
        }
        break;

    case WM_COMMAND:
        if (HIWORD(wp) == EN_KILLFOCUS && reinterpret_cast<HWND>(lp) == m_hTextEdit) {
            CommitTextEdit();
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (m_isEditing) {
            POINT pt;
            ::GetCursorPos(&pt);
            ::ScreenToClient(hwnd, &pt);
            if (::PtInRect(&m_toolbarBounds, pt)) {
                ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32649))); // IDC_HAND
                return TRUE;
            } else if (m_annotationEngine.GetTool() != ToolType::None) {
                ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32515))); // IDC_CROSS
                return TRUE;
            }
        }
        break;

    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_SIZE: {
        RECT rc{}; ::GetClientRect(hwnd, &rc);
        if (m_isEditing) BuildPinToolbar(rc.right, rc.bottom);
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================================
// Context Menu
// ============================================================================

void PinWindow::ShowContextMenu(int screenX, int screenY) {
    HMENU hMenu = ::CreatePopupMenu();
    ::InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, 201, L"\x270E \x6807\x6CE8\x7F16\x8F91 (Edit)");     // ✎ 标注编辑
    ::InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    ::InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_STRING, 202, L"\x2398 \x590D\x5236 (Copy)");             // ⎘ 复制
    ::InsertMenuW(hMenu, 3, MF_BYPOSITION | MF_STRING, 203, L"\x2193 \x4FDD\x5B58 (Save PNG)");         // ↓ 保存
    ::InsertMenuW(hMenu, 4, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    ::InsertMenuW(hMenu, 5, MF_BYPOSITION | MF_STRING, 204, L"\x2715 \x5173\x95ED (Close)");            // ✕ 关闭

    ::SetForegroundWindow(m_hwnd);
    int cmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, screenX, screenY, 0, m_hwnd, nullptr);
    ::DestroyMenu(hMenu);

    switch (cmd) {
    case 201: EnterEditMode(); break;
    case 202: CopyToClipboard(); break;
    case 203: SaveToFile(); break;
    case 204: ::DestroyWindow(m_hwnd); break;
    }
}

void PinWindow::CopyToClipboard() {
    if (!m_bitmap || m_width <= 0 || m_height <= 0) return;

    HDC hdcScreen = ::GetDC(nullptr);
    if (!hdcScreen) return;
    HDC hdcSrc = ::CreateCompatibleDC(hdcScreen);
    HDC hdcDst = ::CreateCompatibleDC(hdcScreen);
    HBITMAP hbmpClone = ::CreateCompatibleBitmap(hdcScreen, m_width, m_height);
    HGDIOBJ oldSrc = nullptr;
    HGDIOBJ oldDst = nullptr;
    bool copied = false;
    if (hdcSrc && hdcDst && hbmpClone) {
        oldSrc = ::SelectObject(hdcSrc, m_bitmap);
        oldDst = ::SelectObject(hdcDst, hbmpClone);
        if (oldSrc && oldSrc != HGDI_ERROR && oldDst && oldDst != HGDI_ERROR) {
            copied = ::BitBlt(hdcDst, 0, 0, m_width, m_height, hdcSrc, 0, 0, SRCCOPY) != FALSE;
        }
    }
    if (oldSrc && oldSrc != HGDI_ERROR) ::SelectObject(hdcSrc, oldSrc);
    if (oldDst && oldDst != HGDI_ERROR) ::SelectObject(hdcDst, oldDst);
    if (hdcSrc) ::DeleteDC(hdcSrc);
    if (hdcDst) ::DeleteDC(hdcDst);
    ::ReleaseDC(nullptr, hdcScreen);

    if (!copied) {
        if (hbmpClone) ::DeleteObject(hbmpClone);
        return;
    }

    bool clipboardOwnsBitmap = false;
    if (::OpenClipboard(m_hwnd)) {
        if (::EmptyClipboard() && ::SetClipboardData(CF_BITMAP, hbmpClone)) clipboardOwnsBitmap = true;
        ::CloseClipboard();
    }
    if (!clipboardOwnsBitmap) ::DeleteObject(hbmpClone);
}

void PinWindow::SaveToFile() {
    if (!m_bitmap) return;

    wchar_t szFile[MAX_PATH] = L"pinned_screenshot.png";
    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = m_hwnd;
    ofn.lpstrFilter  = L"PNG Files\0*.png\0All Files\0*.*\0";
    ofn.lpstrFile    = szFile;
    ofn.nMaxFile     = MAX_PATH;
    ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt  = L"png";
    ofn.lpstrTitle   = L"Nskry \u2014 Save Pinned Screenshot";

    if (!::GetSaveFileNameW(&ofn)) return;

    CLSID clsid;
    if (GetPngEncoderClsidHelper(&clsid) < 0) {
        ::MessageBoxW(m_hwnd, L"PNG encoder not found.", L"Nskry", MB_ICONERROR);
        return;
    }

    Gdiplus::Bitmap bmp(m_bitmap, nullptr);
    Gdiplus::Status st = bmp.Save(szFile, &clsid);
    if (st != Gdiplus::Ok) {
        ::MessageBoxW(m_hwnd, L"Failed to save PNG file.", L"Nskry", MB_ICONERROR);
    }
}

// ============================================================================
// In-place Annotation Editing
// ============================================================================

void PinWindow::EnterEditMode() {
    m_isEditing = true;
    m_annotationEngine.Clear();
    m_annotationEngine.SetTool(ToolType::Pen);

    RECT rc{}; ::GetClientRect(m_hwnd, &rc);
    BuildPinToolbar(rc.right, rc.bottom);
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void PinWindow::FinishEdit(bool apply) {
    CommitTextEdit();

    if (apply && !m_annotationEngine.IsEmpty()) {
        HDC hdcScreen = ::GetDC(nullptr);
        HDC hdcMem = hdcScreen ? ::CreateCompatibleDC(hdcScreen) : nullptr;
        HBITMAP baked = nullptr;
        HGDIOBJ old = nullptr;
        if (hdcMem) {
            old = ::SelectObject(hdcMem, m_bitmap);
            if (old && old != HGDI_ERROR) {
                RECT fullRect = { 0, 0, m_width, m_height };
                baked = m_annotationEngine.BakeToBitmap(hdcMem, fullRect, m_width, m_height);
                ::SelectObject(hdcMem, old);
            }
            ::DeleteDC(hdcMem);
        }
        if (hdcScreen) ::ReleaseDC(nullptr, hdcScreen);

        if (baked) {
            ::DeleteObject(m_bitmap);
            m_bitmap = baked;
        } else {
            ::MessageBoxW(m_hwnd, L"Failed to apply annotations.", L"Nskry", MB_ICONERROR);
            return;
        }
    }

    m_annotationEngine.Clear();
    m_annotationEngine.SetTool(ToolType::None);
    m_isEditing = false;
    m_toolbarItems.clear();
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void PinWindow::CommitTextEdit() {
    if (!m_hTextEdit) return;

    // Clear the member before DestroyWindow: destroying the edit control emits
    // EN_KILLFOCUS and can otherwise re-enter this method with the same HWND.
    HWND edit = m_hTextEdit;
    m_hTextEdit = nullptr;
    int len = ::GetWindowTextLengthW(edit);
    if (len > 0) {
        std::vector<wchar_t> buf(len + 1);
        ::GetWindowTextW(edit, buf.data(), len + 1);
        m_annotationEngine.AddTextShape(m_textEditPos, buf.data());
    }

    if (::IsWindow(edit)) ::DestroyWindow(edit);
    if (m_hwnd) ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void PinWindow::BuildPinToolbar(int clientW, int clientH) {
    m_toolbarItems.clear();

    const bool hasSub = (m_annotationEngine.GetTool() != ToolType::None);

    struct Def {
        ToolType tool;
        int      action;
        const wchar_t* label;
        int      width;
        bool     isSep;
    };

    Def defs[] = {
        { ToolType::Rect,    0, L"矩形",   36, false },
        { ToolType::Ellipse, 0, L"圆形",   36, false },
        { ToolType::Arrow,   0, L"箭头",   36, false },
        { ToolType::Pen,     0, L"画笔",   36, false },
        { ToolType::Mosaic,  0, L"马赛克", 46, false },
        { ToolType::Text,    0, L"文本",   36, false },
        { ToolType::None,    0, nullptr,    4, true  },
        { ToolType::None,    3, L"撤销",   36, false },
        { ToolType::None,    4, L"重做",   36, false },
        { ToolType::None,    0, nullptr,    4, true  },
        { ToolType::None,    1, L"✓ 完成", 46, false },
        { ToolType::None,    2, L"✕ 取消", 46, false },
    };

    constexpr int gap = 2;
    constexpr int barH = 26;
    constexpr int subItemH = 24;

    int totalW = 0;
    for (const auto& d : defs) totalW += d.width + gap;
    totalW -= gap;

    int startX = (clientW - totalW) / 2;
    if (startX < 4) startX = 4;
    int startY = clientH - barH - 6;
    if (hasSub) {
        startY = clientH - barH - subItemH - 10;
    }
    if (startY < 4) startY = 4;

    int curX = startX;
    for (const auto& d : defs) {
        PinToolItem item{};
        item.rect        = { curX, startY, curX + d.width, startY + barH };
        item.tool        = d.tool;
        item.action      = d.action;
        item.label       = d.label;
        item.isSeparator = d.isSep;
        item.selected    = (d.tool != ToolType::None && m_annotationEngine.GetTool() == d.tool);
        m_toolbarItems.push_back(item);
        curX += d.width + gap;
    }

    if (hasSub) {
        int subY = startY + barH + 4;
        const int subBarW = 310;
        int subStartX = (clientW - subBarW) / 2;
        if (subStartX < 4) subStartX = 4;

        int sx = subStartX;
        int widths[] = { 2, 4, 8 };
        const wchar_t* wLabels[] = { L"● 2", L"● 4", L"● 8" };
        for (int i = 0; i < 3; i++) {
            PinToolItem item{};
            item.rect     = { sx, subY, sx + 32, subY + subItemH };
            item.action   = 5; // Width
            item.widthVal = widths[i];
            item.label    = wLabels[i];
            item.selected = (m_annotationEngine.GetStrokeWidth() == widths[i]);
            m_toolbarItems.push_back(item);
            sx += 32 + gap;
        }

        PinToolItem sep{};
        sep.rect        = { sx, subY, sx + 4, subY + subItemH };
        sep.isSeparator = true;
        m_toolbarItems.push_back(sep);
        sx += 4 + gap;

        COLORREF colors[] = {
            RGB(255, 59, 48),   // Red
            RGB(255, 149, 0),  // Orange
            RGB(255, 204, 0),  // Yellow
            RGB(52, 199, 89),  // Green
            RGB(0, 122, 255),  // Blue
            RGB(175, 82, 222), // Purple
            RGB(240, 240, 245),// White
            RGB(30, 30, 35)    // Dark
        };

        for (int i = 0; i < 8; i++) {
            PinToolItem item{};
            item.rect          = { sx, subY, sx + 22, subY + subItemH };
            item.action        = 6; // Color
            item.color         = colors[i];
            item.isColorChoice = true;
            item.selected      = (m_annotationEngine.GetColor() == colors[i]);
            m_toolbarItems.push_back(item);
            sx += 22 + gap;
        }
    }

    if (!m_toolbarItems.empty()) {
        m_toolbarBounds = m_toolbarItems.front().rect;
        for (const auto& it : m_toolbarItems) {
            if (it.rect.left < m_toolbarBounds.left)     m_toolbarBounds.left   = it.rect.left;
            if (it.rect.top < m_toolbarBounds.top)       m_toolbarBounds.top    = it.rect.top;
            if (it.rect.right > m_toolbarBounds.right)   m_toolbarBounds.right  = it.rect.right;
            if (it.rect.bottom > m_toolbarBounds.bottom) m_toolbarBounds.bottom = it.rect.bottom;
        }
        ::InflateRect(&m_toolbarBounds, 2, 2);
    } else {
        m_toolbarBounds = {};
    }
}

void PinWindow::DrawPinToolbar(HDC hdc, int /*clientW*/, int /*clientH*/) {
    ::SetBkMode(hdc, TRANSPARENT);

    for (const auto& item : m_toolbarItems) {
        if (item.isSeparator) {
            int midX = (item.rect.left + item.rect.right) / 2;
            HPEN sepPen = ::CreatePen(PS_SOLID, 1, RGB(70, 70, 80));
            HGDIOBJ oldPen = sepPen ? ::SelectObject(hdc, sepPen) : nullptr;
            if (oldPen && oldPen != HGDI_ERROR) {
                ::MoveToEx(hdc, midX, item.rect.top + 3, nullptr);
                ::LineTo(hdc, midX, item.rect.bottom - 3);
                ::SelectObject(hdc, oldPen);
            }
            if (sepPen) ::DeleteObject(sepPen);
            continue;
        }

        if (item.isColorChoice) {
            COLORREF bg = item.hovered ? RGB(60, 60, 70) : RGB(40, 40, 48);
            HBRUSH bgBrush = ::CreateSolidBrush(bg);
            ::FillRect(hdc, &item.rect, bgBrush);
            ::DeleteObject(bgBrush);

            int cx = (item.rect.left + item.rect.right) / 2;
            int cy = (item.rect.top + item.rect.bottom) / 2;
            int r = 6;

            HBRUSH colBrush = ::CreateSolidBrush(item.color);
            HPEN borderPen = ::CreatePen(PS_SOLID, 1, item.selected ? RGB(255, 255, 255) : RGB(80, 80, 90));
            HGDIOBJ oldBrush = colBrush ? ::SelectObject(hdc, colBrush) : nullptr;
            HGDIOBJ oldPen   = borderPen ? ::SelectObject(hdc, borderPen) : nullptr;

            if (oldBrush && oldBrush != HGDI_ERROR && oldPen && oldPen != HGDI_ERROR) {
                ::Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
            }

            if (item.selected) {
                HPEN ringPen = ::CreatePen(PS_SOLID, 2, RGB(0, 150, 255));
                HGDIOBJ previousPen = ringPen ? ::SelectObject(hdc, ringPen) : nullptr;
                HGDIOBJ previousBrush = ::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
                if (previousPen && previousPen != HGDI_ERROR && previousBrush && previousBrush != HGDI_ERROR) {
                    ::Ellipse(hdc, cx - r - 2, cy - r - 2, cx + r + 3, cy + r + 3);
                }
                if (previousBrush && previousBrush != HGDI_ERROR) ::SelectObject(hdc, previousBrush);
                if (previousPen && previousPen != HGDI_ERROR) ::SelectObject(hdc, previousPen);
                if (ringPen) ::DeleteObject(ringPen);
            }

            if (oldBrush && oldBrush != HGDI_ERROR) ::SelectObject(hdc, oldBrush);
            if (oldPen && oldPen != HGDI_ERROR) ::SelectObject(hdc, oldPen);
            if (colBrush) ::DeleteObject(colBrush);
            if (borderPen) ::DeleteObject(borderPen);
            continue;
        }

        COLORREF bg;
        if (item.selected) {
            bg = RGB(25, 118, 210);
        } else if (item.action == 1 && item.hovered) { // Done
            bg = RGB(46, 125, 50);
        } else if (item.action == 2 && item.hovered) { // Cancel
            bg = RGB(198, 40, 40);
        } else if (item.hovered) {
            bg = RGB(65, 65, 75);
        } else {
            bg = RGB(40, 40, 48);
        }

        HBRUSH bgBrush = ::CreateSolidBrush(bg);
        ::FillRect(hdc, &item.rect, bgBrush);
        ::DeleteObject(bgBrush);

        HBRUSH borderBrush = ::CreateSolidBrush(item.selected ? RGB(70, 160, 245) : (item.hovered ? RGB(90, 90, 100) : RGB(60, 60, 70)));
        ::FrameRect(hdc, &item.rect, borderBrush);
        ::DeleteObject(borderBrush);

        if (item.label) {
            HFONT oldFont = static_cast<HFONT>(::SelectObject(hdc, m_font));
            ::SetTextColor(hdc, RGB(240, 240, 245));
            RECT textRect = item.rect;
            ::DrawTextW(hdc, item.label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            ::SelectObject(hdc, oldFont);
        }
    }
}

RECT PinWindow::ImageRect() const {
    if (!m_hwnd || m_width <= 0 || m_height <= 0) return {};
    RECT client{};
    if (!::GetClientRect(m_hwnd, &client)) return {};
    const int clientW = client.right - client.left;
    const int clientH = client.bottom - client.top;
    if (clientW <= 0 || clientH <= 0) return {};

    const double scale = (std::min)(
        static_cast<double>(clientW) / m_width,
        static_cast<double>(clientH) / m_height);
    const int imageW = (std::max)(1, static_cast<int>(m_width * scale));
    const int imageH = (std::max)(1, static_cast<int>(m_height * scale));
    const int left = (clientW - imageW) / 2;
    const int top = (clientH - imageH) / 2;
    return { left, top, left + imageW, top + imageH };
}

bool PinWindow::ClientPointToBitmap(int x, int y, POINT& bitmapPoint, bool clampToImage) const {
    const RECT image = ImageRect();
    POINT clientPoint{ x, y };
    if (image.right <= image.left || image.bottom <= image.top) {
        return false;
    }
    if (!::PtInRect(&image, clientPoint)) {
        if (!clampToImage) return false;
        x = (std::clamp)(x, static_cast<int>(image.left), static_cast<int>(image.right - 1));
        y = (std::clamp)(y, static_cast<int>(image.top), static_cast<int>(image.bottom - 1));
    }

    const int imageW = image.right - image.left;
    const int imageH = image.bottom - image.top;
    bitmapPoint.x = (std::clamp)(static_cast<int>(
        static_cast<long long>(x - image.left) * m_width / imageW), 0, m_width - 1);
    bitmapPoint.y = (std::clamp)(static_cast<int>(
        static_cast<long long>(y - image.top) * m_height / imageH), 0, m_height - 1);
    return true;
}

void PinWindow::OnLButtonDown(int x, int y) {
    POINT pt{ x, y };

    // 1. Check toolbar click
    for (const auto& item : m_toolbarItems) {
        if (item.isSeparator) continue;
        if (::PtInRect(&item.rect, pt)) {
            CommitTextEdit();

            if (item.action == 1) { FinishEdit(true);  return; } // Done
            if (item.action == 2) { FinishEdit(false); return; } // Cancel
            if (item.action == 3) { m_annotationEngine.Undo(); ::InvalidateRect(m_hwnd, nullptr, FALSE); return; }
            if (item.action == 4) { m_annotationEngine.Redo(); ::InvalidateRect(m_hwnd, nullptr, FALSE); return; }

            if (item.action == 5) { // Width
                m_annotationEngine.SetStrokeWidth(item.widthVal);
                m_annotationEngine.SetFontSize(item.widthVal == 2 ? 14 : (item.widthVal == 4 ? 18 : 26));
                RECT rc{}; ::GetClientRect(m_hwnd, &rc);
                BuildPinToolbar(rc.right, rc.bottom);
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }

            if (item.action == 6) { // Color
                m_annotationEngine.SetColor(item.color);
                RECT rc{}; ::GetClientRect(m_hwnd, &rc);
                BuildPinToolbar(rc.right, rc.bottom);
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }

            if (item.tool != ToolType::None) {
                if (m_annotationEngine.GetTool() == item.tool) {
                    m_annotationEngine.SetTool(ToolType::None);
                } else {
                    m_annotationEngine.SetTool(item.tool);
                }
                RECT rc{}; ::GetClientRect(m_hwnd, &rc);
                BuildPinToolbar(rc.right, rc.bottom);
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
        }
    }

    // Ignore clicks in toolbar area that missed a button
    if (::PtInRect(&m_toolbarBounds, pt)) return;

    // 2. Annotation Canvas drawing
    if (m_annotationEngine.GetTool() != ToolType::None) {
        CommitTextEdit();

        POINT bitmapPoint{};
        if (!ClientPointToBitmap(x, y, bitmapPoint)) return;

        if (m_annotationEngine.GetTool() == ToolType::Text) {
            m_textEditPos = bitmapPoint;
            RECT client{};
            ::GetClientRect(m_hwnd, &client);
            const int clientW = static_cast<int>(client.right - client.left);
            const int clientH = static_cast<int>(client.bottom - client.top);
            const int editW = (std::min)(120, (std::max)(1, clientW));
            const int editX = (std::clamp)(x, 0, (std::max)(0, clientW - editW));
            const int editY = (std::clamp)(y, 0, (std::max)(0, clientH - 24));
            m_hTextEdit = ::CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                editX, editY, editW, 24,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(102)),
                ::GetModuleHandleW(nullptr), nullptr);
            if (m_hTextEdit) {
                if (m_font) ::SendMessageW(m_hTextEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
                ::SetFocus(m_hTextEdit);
            }
        } else {
            m_isDrawing = true;
            m_annotationEngine.OnMouseDown(bitmapPoint);
            ::SetCapture(m_hwnd);
            ::InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
}

void PinWindow::OnMouseMove(int x, int y) {
    if (m_isDrawing) {
        POINT bitmapPoint{};
        if (ClientPointToBitmap(x, y, bitmapPoint, true)) {
            m_annotationEngine.OnMouseMove(bitmapPoint);
            ::InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }

    // Update toolbar item hover
    bool needRepaint = false;
    for (auto& item : m_toolbarItems) {
        if (item.isSeparator) continue;
        POINT pt{ x, y };
        bool inside = ::PtInRect(&item.rect, pt);
        if (inside != item.hovered) {
            item.hovered = inside;
            needRepaint = true;
        }
    }
    if (needRepaint) ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void PinWindow::OnLButtonUp(int x, int y) {
    if (m_isDrawing) {
        ::ReleaseCapture();
        m_isDrawing = false;
        POINT bitmapPoint{};
        if (ClientPointToBitmap(x, y, bitmapPoint, true)) {
            m_annotationEngine.OnMouseUp(bitmapPoint);
        }
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void PinWindow::OnPaint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = ::BeginPaint(hwnd, &ps);
    if (!hdc) return;

    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    int clientW = rc.right;
    int clientH = rc.bottom;

    ::FillRect(hdc, &rc, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
    const RECT image = ImageRect();
    HDC hdcMem = (m_bitmap && image.right > image.left && image.bottom > image.top)
        ? ::CreateCompatibleDC(hdc) : nullptr;
    HGDIOBJ old = hdcMem ? ::SelectObject(hdcMem, m_bitmap) : nullptr;
    const bool sourceReady = old && old != HGDI_ERROR;

    // 1. Draw base bitmap
    if (sourceReady) {
        ::SetStretchBltMode(hdc, HALFTONE);
        ::SetBrushOrgEx(hdc, 0, 0, nullptr);
        ::StretchBlt(hdc, image.left, image.top, image.right - image.left, image.bottom - image.top,
                     hdcMem, 0, 0, m_width, m_height, SRCCOPY);
    }

    // 2. Draw annotations in edit mode
    if (m_isEditing && sourceReady) {
        Gdiplus::Graphics g(hdc);
        Gdiplus::Matrix transform(
            static_cast<Gdiplus::REAL>(image.right - image.left) / m_width, 0.0f,
            0.0f, static_cast<Gdiplus::REAL>(image.bottom - image.top) / m_height,
            static_cast<Gdiplus::REAL>(image.left), static_cast<Gdiplus::REAL>(image.top));
        g.SetTransform(&transform);
        m_annotationEngine.Draw(g, hdcMem, 0, 0, m_width, m_height);
    }

    // Draw the toolbar even when the source bitmap could not be selected, so
    // users can still cancel editing or close the window.
    if (m_isEditing) DrawPinToolbar(hdc, clientW, clientH);

    if (sourceReady) ::SelectObject(hdcMem, old);
    if (hdcMem) ::DeleteDC(hdcMem);

    ::EndPaint(hwnd, &ps);
}

} // namespace nskry
