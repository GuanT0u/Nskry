#include "pch.h"
#include "ui/pin_window.h"
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

    m_font = ::CreateFontW(
        -12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_fontIcon = ::CreateFontW(
        -14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");

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
    CommitTextEdit();
    if (m_font)     { ::DeleteObject(m_font); m_font = nullptr; }
    if (m_fontIcon) { ::DeleteObject(m_fontIcon); m_fontIcon = nullptr; }
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
        m_hwnd = nullptr;
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
    if (!m_bitmap) return;

    HDC hdcScreen = ::GetDC(nullptr);
    HDC hdcSrc = ::CreateCompatibleDC(hdcScreen);
    HDC hdcDst = ::CreateCompatibleDC(hdcScreen);
    HBITMAP hbmpClone = ::CreateCompatibleBitmap(hdcScreen, m_width, m_height);

    HGDIOBJ oldSrc = ::SelectObject(hdcSrc, m_bitmap);
    HGDIOBJ oldDst = ::SelectObject(hdcDst, hbmpClone);
    ::BitBlt(hdcDst, 0, 0, m_width, m_height, hdcSrc, 0, 0, SRCCOPY);

    ::SelectObject(hdcSrc, oldSrc);
    ::SelectObject(hdcDst, oldDst);
    ::DeleteDC(hdcSrc);
    ::DeleteDC(hdcDst);
    ::ReleaseDC(nullptr, hdcScreen);

    if (::OpenClipboard(m_hwnd)) {
        ::EmptyClipboard();
        ::SetClipboardData(CF_BITMAP, hbmpClone);
        ::CloseClipboard();
    }
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
        HDC hdcMem = ::CreateCompatibleDC(hdcScreen);
        HGDIOBJ old = ::SelectObject(hdcMem, m_bitmap);
        RECT fullRect = { 0, 0, m_width, m_height };
        HBITMAP baked = m_annotationEngine.BakeToBitmap(hdcMem, fullRect, m_width, m_height);
        ::SelectObject(hdcMem, old);
        ::DeleteDC(hdcMem);
        ::ReleaseDC(nullptr, hdcScreen);

        if (baked) {
            ::DeleteObject(m_bitmap);
            m_bitmap = baked;
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

    int len = ::GetWindowTextLengthW(m_hTextEdit);
    if (len > 0) {
        std::vector<wchar_t> buf(len + 1);
        ::GetWindowTextW(m_hTextEdit, buf.data(), len + 1);
        m_annotationEngine.AddTextShape(m_textEditPos, buf.data());
    }

    ::DestroyWindow(m_hTextEdit);
    m_hTextEdit = nullptr;
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void PinWindow::BuildPinToolbar(int clientW, int clientH) {
    m_toolbarItems.clear();

    struct Def {
        ToolType tool;
        int      action;
        const wchar_t* label;
        int      width;
        bool     isSep;
    };

    Def defs[] = {
        { ToolType::Rect,    0, L"\x25AD", 26, false }, // ▭
        { ToolType::Ellipse, 0, L"\x25CB", 26, false }, // ○
        { ToolType::Arrow,   0, L"\x2794", 26, false }, // ➔
        { ToolType::Pen,     0, L"\x270E", 26, false }, // ✎
        { ToolType::Mosaic,  0, L"\x25A6", 26, false }, // ▦
        { ToolType::Text,    0, L"T",      26, false }, // T
        { ToolType::None,    0, nullptr,   4,  true  }, // Sep
        { ToolType::None,    3, L"\x21B6", 26, false }, // ↶ Undo
        { ToolType::None,    4, L"\x21B7", 26, false }, // ↷ Redo
        { ToolType::None,    0, nullptr,   4,  true  }, // Sep
        { ToolType::None,    1, L"\x2713", 32, false }, // ✓ Done
        { ToolType::None,    2, L"\x2715", 32, false }, // ✕ Cancel
    };

    constexpr int gap = 2;
    constexpr int barH = 26;

    int totalW = 0;
    for (const auto& d : defs) totalW += d.width + gap;
    totalW -= gap;

    int startX = (clientW - totalW) / 2;
    if (startX < 4) startX = 4;
    int startY = clientH - barH - 6;
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
}

void PinWindow::DrawPinToolbar(HDC hdc, int /*clientW*/, int /*clientH*/) {
    ::SetBkMode(hdc, TRANSPARENT);

    for (const auto& item : m_toolbarItems) {
        if (item.isSeparator) {
            int midX = (item.rect.left + item.rect.right) / 2;
            HPEN sepPen = ::CreatePen(PS_SOLID, 1, RGB(70, 70, 80));
            HGDIOBJ oldPen = ::SelectObject(hdc, sepPen);
            ::MoveToEx(hdc, midX, item.rect.top + 3, nullptr);
            ::LineTo(hdc, midX, item.rect.bottom - 3);
            ::SelectObject(hdc, oldPen);
            ::DeleteObject(sepPen);
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
            HFONT useFont = (item.tool != ToolType::None || item.action >= 3) ? m_fontIcon : m_font;
            HFONT oldFont = static_cast<HFONT>(::SelectObject(hdc, useFont));
            ::SetTextColor(hdc, RGB(240, 240, 245));
            RECT textRect = item.rect;
            ::DrawTextW(hdc, item.label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            ::SelectObject(hdc, oldFont);
        }
    }
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

    // 2. Annotation Canvas drawing
    if (m_annotationEngine.GetTool() != ToolType::None) {
        CommitTextEdit();

        RECT rc{}; ::GetClientRect(m_hwnd, &rc);
        int cw = rc.right; int ch = rc.bottom;
        if (cw <= 0 || ch <= 0) return;

        int bmpX = x * m_width / cw;
        int bmpY = y * m_height / ch;

        if (m_annotationEngine.GetTool() == ToolType::Text) {
            m_textEditPos = { bmpX, bmpY };
            m_hTextEdit = ::CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                x, y, 120, 24,
                m_hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(102)),
                ::GetModuleHandleW(nullptr), nullptr);
            ::SendMessageW(m_hTextEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
            ::SetFocus(m_hTextEdit);
        } else {
            m_isDrawing = true;
            m_annotationEngine.OnMouseDown({ bmpX, bmpY });
            ::SetCapture(m_hwnd);
            ::InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
}

void PinWindow::OnMouseMove(int x, int y) {
    if (m_isDrawing) {
        RECT rc{}; ::GetClientRect(m_hwnd, &rc);
        int cw = rc.right; int ch = rc.bottom;
        if (cw > 0 && ch > 0) {
            int bmpX = x * m_width / cw;
            int bmpY = y * m_height / ch;
            m_annotationEngine.OnMouseMove({ bmpX, bmpY });
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
        RECT rc{}; ::GetClientRect(m_hwnd, &rc);
        int cw = rc.right; int ch = rc.bottom;
        if (cw > 0 && ch > 0) {
            int bmpX = x * m_width / cw;
            int bmpY = y * m_height / ch;
            m_annotationEngine.OnMouseUp({ bmpX, bmpY });
        }
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void PinWindow::OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = ::BeginPaint(hwnd, &ps);

    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    int clientW = rc.right;
    int clientH = rc.bottom;

    HDC hdcMem = ::CreateCompatibleDC(hdc);
    HGDIOBJ old = ::SelectObject(hdcMem, m_bitmap);

    // 1. Draw base bitmap
    ::SetStretchBltMode(hdc, HALFTONE);
    ::SetBrushOrgEx(hdc, 0, 0, nullptr);

    ::StretchBlt(hdc, 0, 0, clientW, clientH,
                 hdcMem, 0, 0, m_width, m_height, SRCCOPY);

    // 2. Draw annotations in edit mode
    if (m_isEditing) {
        Gdiplus::Graphics g(hdc);
        g.ScaleTransform(
            static_cast<float>(clientW) / static_cast<float>(m_width),
            static_cast<float>(clientH) / static_cast<float>(m_height));
        m_annotationEngine.Draw(g, hdcMem, 0, 0, m_width, m_height);

        // Draw Pin Toolbar
        DrawPinToolbar(hdc, clientW, clientH);
    }

    ::SelectObject(hdcMem, old);
    ::DeleteDC(hdcMem);

    ::EndPaint(hwnd, &ps);
}

} // namespace nskry
