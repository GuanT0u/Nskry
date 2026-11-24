#include "pch.h"
#include "ui/floating_actions.h"
#include "ui/toolbar_layout.h"
#include <oleacc.h>
#include "ui/ui_preferences.h"
#include "ui/interaction_visuals.h"
#include "ui/pin_window.h"
#include <algorithm>
#include <windowsx.h>
#include <commdlg.h>

namespace nskry {

namespace {
constexpr UINT_PTR kQuickHideTimer = 0x51A;
constexpr int kQuickEditId = 7101;
constexpr int kQuickImageActionId = 7102;
constexpr int kQuickMinimizeId = 7103;
constexpr int kQuickCloseId = 7104;
}

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

PinWindow::PinWindow(HBITMAP bitmap, int width, int height, CloseCallback closed,
              ImageActions imageActions, RECT initialScreenRect, ImageEditorCallback editor, AnnotationEngine annotations)
    : m_bitmap(bitmap), m_width(width), m_height(height), m_annotationEngine(std::move(annotations)), m_closed(std::move(closed)),
      m_imageActions(std::move(imageActions)), m_editor(std::move(editor)),
      m_hasInitialScreenRect(initialScreenRect.right > initialScreenRect.left && initialScreenRect.bottom > initialScreenRect.top)
{
    std::call_once(s_classOnce, [] {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DBLCLKS;
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

    const DWORD style   = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_APPWINDOW;

    if (!m_bitmap || m_width <= 0 || m_height <= 0) return;

    POINT cursor{};
    ::GetCursorPos(&cursor);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    RECT workArea{ 0, 0, 1024, 768 };
    if (monitor && ::GetMonitorInfoW(monitor, &monitorInfo)) workArea = monitorInfo.rcWork;

    const int nonClientW = 0;
    const int nonClientH = 0;
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

    const int outerW = m_hasInitialScreenRect ? initialScreenRect.right - initialScreenRect.left : winW + nonClientW;
    const int outerH = m_hasInitialScreenRect ? initialScreenRect.bottom - initialScreenRect.top : winH + nonClientH;
    const int x = m_hasInitialScreenRect ? initialScreenRect.left : workArea.left + (workW - outerW) / 2;
    const int y = m_hasInitialScreenRect ? initialScreenRect.top : workArea.top + (workH - outerH) / 2;

    m_hwnd = ::CreateWindowExW(
        exStyle, kClassName, UiText(L"Nskry — Pinned image", L"Nskry — 固定截图"),
        style,
        x, y, outerW, outerH,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
}

PinWindow::~PinWindow() {
    m_visualStyle.reset();
    m_destroying = true;
    CommitTextEdit();
    if (m_font)     { ::DeleteObject(m_font); m_font = nullptr; }
    if (m_hwnd) {
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_quickFont) { ::DeleteObject(m_quickFont); m_quickFont = nullptr; }
    if (m_bitmap) {
        ::DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }
}

bool PinWindow::Show() {
    if (!m_hwnd) return false;
    ::ShowWindow(m_hwnd, SW_SHOWNA);
    ::UpdateWindow(m_hwnd);
    POINT cursor{};
    RECT bounds{};
    if (::GetCursorPos(&cursor) && ::GetWindowRect(m_hwnd, &bounds) && ::PtInRect(&bounds, cursor))
        ShowQuickActions(true);
    return true;
}

bool PinWindow::ShowInEditMode(bool requirePinDecision) {
    if (!Show()) return false;
    m_requiresPinDecision = requirePinDecision;
    if (!m_isEditing) EnterEditMode();
    if (m_hwnd) ::SetForegroundWindow(m_hwnd);
    return m_hwnd != nullptr;
}

LRESULT CALLBACK PinWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* created = static_cast<PinWindow*>(cs->lpCreateParams);
        created->m_hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
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
    case WM_HSCROLL:
        if (m_mosaicSlider.Read(lp, m_annotationEngine)) return 0;
        break;
    case WM_GETOBJECT:
        if (m_isEditing && static_cast<LONG>(lp) == OBJID_CLIENT)
            return m_toolbarInput.GetObject(hwnd, wp, lp);
        break;
    case ToolbarAccessibility::InvokeMessage:
    case ToolbarAccessibility::FocusMessage:
        if (m_isEditing) m_toolbarInput.HandleObjectAction(hwnd, hwnd, msg, wp, lp);
        return 0;

    case WM_NSKRY_REFRESH_UI:
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        m_palette = GetDesktopPalette();
        ::SetWindowTextW(hwnd, UiText(L"Nskry — Pinned image", L"Nskry — 固定截图"));
        if (m_isEditing) {
            RECT bounds{}; ::GetClientRect(hwnd, &bounds);
            BuildPinToolbar(bounds.right, bounds.bottom);
        }
        ::InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_ENTERSIZEMOVE: if (m_visualStyle) m_visualStyle->BeginSizeMove(); break;
    case WM_EXITSIZEMOVE: if (m_visualStyle) m_visualStyle->EndSizeMove(); break;
    case WM_WINDOWPOSCHANGED: if (m_visualStyle) m_visualStyle->Sync(); break;
    case WM_DPICHANGED:
        LayoutQuickActions();
        if (m_visualStyle) m_visualStyle->Sync();
        break;
    case WM_CREATE:
        {
            const COLORREF noBorder = DWMWA_COLOR_NONE;
            ::DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
        }
        CreateQuickActions(hwnd);
        return 0;

    case WM_NCHITTEST: {
        POINT point{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT window{}; ::GetWindowRect(hwnd, &window);
        if (!m_isEditing && !m_quickActionsVisible && !::IsIconic(hwnd) && ::PtInRect(&window, point))
            ShowQuickActions(true);
        const int border = (std::max)(6, ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, ::GetDpiForWindow(hwnd)));
        const bool left = point.x < window.left + border;
        const bool right = point.x >= window.right - border;
        const bool top = point.y < window.top + border;
        const bool bottom = point.y >= window.bottom - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        POINT clientPoint = point; ::ScreenToClient(hwnd, &clientPoint);
        if (IsQuickActionPoint(clientPoint) || m_isEditing) return HTCLIENT;
        return HTCAPTION;
    }

    case WM_NCCALCSIZE:
        return 0; // Keep the window borderless; resize hit targets are handled above.

    case WM_NCACTIVATE:
        return TRUE; // Do not let DefWindowProc redraw the thick frame on focus changes.

    case WM_NCPAINT:
        return 0;

    case WM_SIZING: {
        if (m_height == 0) break;
        // Free resize for edge drags
        if (wp == WMSZ_LEFT || wp == WMSZ_RIGHT || wp == WMSZ_TOP || wp == WMSZ_BOTTOM) {
            return TRUE;
        }

        // Corner drag — lock aspect ratio
        RECT* r = reinterpret_cast<RECT*>(lp);
        const int bw = 0;
        const int bh = 0;

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

        finalCW = (std::max)(1, finalCW);
        finalCH = (std::max)(1, finalCH);
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
            // Never enlarge a small screenshot to fit annotation controls.
            limits->ptMinTrackSize.x = 1;
            limits->ptMinTrackSize.y = 1;
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
        } else {
            const POINT client{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            POINT image{};
            CommitTextEdit();
            if (!::PtInRect(&m_toolbarBounds, client) && ClientPointToBitmap(client.x, client.y, image)) {
                const int tolerance = AnnotationEngine::HandleRadius(ImageRect(), m_width, m_height, ::GetDpiForWindow(hwnd));
                if (auto text = m_annotationEngine.SelectTextAt(image, tolerance / 2)) {
                    const auto origin = text->position;
                    const auto bounds = ImageRect();
                    OpenTextEdit({bounds.left + ::MulDiv(origin.x, bounds.right - bounds.left, m_width),
                                  bounds.top + ::MulDiv(origin.y, bounds.bottom - bounds.top, m_height)},
                                 origin, std::move(text));
                    return 0;
                }
            }
            OnLButtonDown(client.x, client.y);
            return 0;
        }

    case WM_LBUTTONDOWN:
        if (m_isEditing) {
            OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        break;
        break;

    case WM_MOUSEMOVE:
        if (m_isEditing) {
            OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        ShowQuickActions(true);
        break;

    case WM_NCMOUSEMOVE:
        if (!m_isEditing) ShowQuickActions(true);
        break;

    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        if (m_isDrawing) {
            m_isDrawing = false;
            m_annotationEngine.CancelPointer();
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_LBUTTONUP:
        if (m_isEditing) {
            OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        break;

    case WM_KEYDOWN:case WM_SYSKEYDOWN:
        if (!m_isEditing && IsFloatingContextKey(wp))
            return ::SendMessageW(hwnd, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(hwnd), -1);
        if (m_isEditing) {
            if (Shortcut(L"core.cancel",UINT(wp))) {
                FinishEdit(false);
                return 0;
            }
            {
                if (Shortcut(L"core.undo",UINT(wp))) {
                    CommitTextEdit();
                    m_annotationEngine.Undo();
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (Shortcut(L"core.redo",UINT(wp))) {
                    CommitTextEdit();
                    m_annotationEngine.Redo();
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (msg == WM_KEYDOWN && !m_hTextEdit && m_toolbarInput.Press(hwnd, hwnd, wp))
                return 0;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kQuickEditId: EnterEditMode(); return 0;
        case kQuickImageActionId: RunQuickImageAction(); return 0;
        case kQuickMinimizeId: ShowQuickActions(false); ::ShowWindow(hwnd, SW_MINIMIZE); return 0;
        case kQuickCloseId: ::DestroyWindow(hwnd); return 0;
        default: break;
        }
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
        LayoutQuickActions();
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (wp == kQuickHideTimer && m_quickActionsVisible && !m_isEditing) {
            POINT cursor{};
            RECT window{};
            ::GetCursorPos(&cursor);
            ::GetWindowRect(hwnd, &window);
            if (!::PtInRect(&window, cursor)) ShowQuickActions(false);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        m_toolbarInput.Clear();
        m_visualStyle.reset();
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void PinWindow::CreateQuickActions(HWND parent) {
    m_quickFont = ::CreateFontW(-::MulDiv(16, ::GetDpiForWindow(parent), 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const wchar_t* labels[] = { L"\x270E", L"\x2026", L"\x2212", L"\x00D7" };
    const int ids[] = { kQuickEditId, kQuickImageActionId, kQuickMinimizeId, kQuickCloseId };
    for (size_t index = 0; index < 4; ++index) {
        m_quickButtons[index] = ::CreateWindowExW(WS_EX_LAYERED, L"BUTTON", labels[index],
            WS_CHILD | BS_PUSHBUTTON, 0, 0, 16, 14, parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[index])), ::GetModuleHandleW(nullptr), nullptr);
        if (!m_quickButtons[index])
            m_quickButtons[index] = ::CreateWindowExW(0, L"BUTTON", labels[index],
                WS_CHILD | BS_PUSHBUTTON, 0, 0, 16, 14, parent,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[index])), ::GetModuleHandleW(nullptr), nullptr);
        if (m_quickButtons[index]) {
            if ((::GetWindowLongPtrW(m_quickButtons[index], GWL_EXSTYLE) & WS_EX_LAYERED) != 0 &&
                !::SetLayeredWindowAttributes(m_quickButtons[index], 0, DesktopHighContrast() ? 255 : 235, LWA_ALPHA))
                ::SetWindowLongPtrW(m_quickButtons[index], GWL_EXSTYLE, 0);
            if (m_quickFont) ::SendMessageW(m_quickButtons[index], WM_SETFONT, reinterpret_cast<WPARAM>(m_quickFont), TRUE);
        }
    }
    LayoutQuickActions();
}

void PinWindow::LayoutQuickActions() {
    if (m_hwnd && !LayoutFloatingActions(m_hwnd, m_quickButtons, !m_imageActions.empty()))
        ShowQuickActions(false);
}

bool PinWindow::IsQuickActionPoint(POINT clientPoint) const {
    for (HWND button : m_quickButtons) {
        if (!button || !::IsWindowVisible(button)) continue;
        RECT rect{}; ::GetWindowRect(button, &rect);
        ::MapWindowPoints(HWND_DESKTOP, m_hwnd, reinterpret_cast<POINT*>(&rect), 2);
        if (::PtInRect(&rect, clientPoint)) return true;
    }
    return false;
}

void PinWindow::ShowQuickActions(bool show) {
    if (m_isEditing || (show && !LayoutFloatingActions(m_hwnd, m_quickButtons, !m_imageActions.empty())))
        show = false;
    bool visibilityMatches = m_quickActionsVisible == show;
    for (size_t i = 0; i < std::size(m_quickButtons); ++i)
        if (m_quickButtons[i] && (::IsWindowVisible(m_quickButtons[i]) != FALSE) !=
            (show && (i != 1 || !m_imageActions.empty()))) visibilityMatches = false;
    if (visibilityMatches) return;
    m_quickActionsVisible = show;
    for (size_t i = 0; i < std::size(m_quickButtons); ++i) if (m_quickButtons[i])
        ::ShowWindow(m_quickButtons[i], show && (i != 1 || !m_imageActions.empty()) ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show) ::SetTimer(m_hwnd, kQuickHideTimer, 120, nullptr);
    else if (m_hwnd) ::KillTimer(m_hwnd, kQuickHideTimer);
}

void PinWindow::RunQuickImageAction() {
    const auto index = ChooseImageAction(m_hwnd, m_imageActions);
    if (!index) return;
    InvokeImageAction(m_imageActions[*index]);
}

void PinWindow::InvokeImageAction(const ImageAction& action) {
    if (!action.invoke) return;
    const auto invoke = action.invoke;
    const int width = m_width, height = m_height;
    const HWND owner = m_hwnd;
    RECT region{};
    ::GetWindowRect(owner, &region);
    const bool baked = !m_annotationEngine.IsEmpty();
    HBITMAP image = baked ? BakeEditedBitmap() : m_bitmap;
    if (!image) return;
    struct OwnedBitmap {
        HBITMAP handle;
        ~OwnedBitmap() { if (handle) ::DeleteObject(handle); }
    } cleanup{baked ? image : nullptr};
    invoke(image, width, height, region, owner);
}

// ============================================================================
// Context Menu
// ============================================================================

void PinWindow::ShowContextMenu(int screenX, int screenY) {
    HMENU hMenu = ::CreatePopupMenu();
    ::InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, 201, UiText(L"Edit annotations", L"编辑标注"));     // ✎ 标注编辑
    ::InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    int position = 2;
    if (m_editor) ::InsertMenuW(hMenu, position++, MF_BYPOSITION | MF_STRING, 205, UiText(L"Crop or cut strips", L"裁剪或切除区段"));
    for (size_t index = 0; index < m_imageActions.size() && index < 100; ++index) {
        const std::wstring item = m_imageActions[index].label;
        ::InsertMenuW(hMenu, position++, MF_BYPOSITION | MF_STRING, 220 + static_cast<UINT>(index), item.c_str());
    }
    if (!m_imageActions.empty()) ::InsertMenuW(hMenu, position++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    ::InsertMenuW(hMenu, position++, MF_BYPOSITION | MF_STRING, 202, UiText(L"Copy image", L"复制图片"));             // ⎘ 复制
    ::InsertMenuW(hMenu, position++, MF_BYPOSITION | MF_STRING, 203, UiText(L"Save PNG", L"保存 PNG"));         // ↓ 保存
    ::InsertMenuW(hMenu, position++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    ::InsertMenuW(hMenu, position++, MF_BYPOSITION | MF_STRING, 210, UiText(L"Minimize", L"最小化"));
    ::InsertMenuW(hMenu, position, MF_BYPOSITION | MF_STRING, 204, UiText(L"Close", L"关闭"));            // ✕ 关闭

    ::SetForegroundWindow(m_hwnd);
    int cmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, screenX, screenY, 0, m_hwnd, nullptr);
    ::DestroyMenu(hMenu);

    switch (cmd) {
    case 201: EnterEditMode(); break;
    case 202: CopyToClipboard(); break;
    case 203: SaveToFile(); break;
    case 210: ShowQuickActions(false); ::ShowWindow(m_hwnd, SW_MINIMIZE); break;
    case 204: ::DestroyWindow(m_hwnd); break;
    case 205: OpenStructureEditor(); break;
    default:
        if (cmd >= 220 && cmd < 220 + static_cast<int>(m_imageActions.size())) {
            InvokeImageAction(m_imageActions[cmd - 220]);
        }
        break;
    }
}

void PinWindow::OpenStructureEditor() {
    if (!m_editor) return;
    CommitTextEdit();
    HBITMAP copy = m_annotationEngine.IsEmpty()
        ? static_cast<HBITMAP>(::CopyImage(m_bitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION)) : BakeEditedBitmap();
    if (copy) m_editor(copy, m_width, m_height);
}
bool PinWindow::SetVisualSettings(WindowVisualSettings settings) {
    RelayCoreButtons(m_hwnd);
    if (!m_hwnd) return false;
    if (!m_visualStyle) m_visualStyle = std::make_unique<WindowVisualStyle>(m_hwnd);
    return m_visualStyle->SetSettings(settings);
}

bool PinWindow::CopyToClipboard(HBITMAP bitmap) {
    if (!bitmap && !m_annotationEngine.IsEmpty()) {
        const auto annotated = BakeEditedBitmap();
        if (!annotated) return false;
        const bool copied = CopyToClipboard(annotated);
        ::DeleteObject(annotated);
        return copied;
    }
    if (!bitmap) bitmap = m_bitmap;
    if (!bitmap || m_width <= 0 || m_height <= 0) return false;

    HDC hdcScreen = ::GetDC(nullptr);
    if (!hdcScreen) return false;
    HDC hdcSrc = ::CreateCompatibleDC(hdcScreen);
    HDC hdcDst = ::CreateCompatibleDC(hdcScreen);
    HBITMAP hbmpClone = ::CreateCompatibleBitmap(hdcScreen, m_width, m_height);
    HGDIOBJ oldSrc = nullptr;
    HGDIOBJ oldDst = nullptr;
    bool copied = false;
    if (hdcSrc && hdcDst && hbmpClone) {
        oldSrc = ::SelectObject(hdcSrc, bitmap);
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
        return false;
    }

    bool clipboardOwnsBitmap = false;
    if (::OpenClipboard(m_hwnd)) {
        if (::EmptyClipboard() && ::SetClipboardData(CF_BITMAP, hbmpClone)) clipboardOwnsBitmap = true;
        ::CloseClipboard();
    }
    if (!clipboardOwnsBitmap) ::DeleteObject(hbmpClone);
    return clipboardOwnsBitmap;
}

bool PinWindow::SaveToFile(HBITMAP bitmap) {
    if (!bitmap && !m_annotationEngine.IsEmpty()) {
        const auto annotated = BakeEditedBitmap();
        if (!annotated) return false;
        const bool saved = SaveToFile(annotated);
        ::DeleteObject(annotated);
        return saved;
    }
    if (!bitmap) bitmap = m_bitmap;
    if (!bitmap) return false;

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

    if (!::GetSaveFileNameW(&ofn)) return false;

    CLSID clsid;
    if (GetPngEncoderClsidHelper(&clsid) < 0) {
        ::MessageBoxW(m_hwnd, UiText(L"PNG encoder is unavailable.", L"PNG 编码器不可用。"), L"Nskry", MB_ICONERROR);
        return false;
    }

    Gdiplus::Bitmap bmp(bitmap, nullptr);
    Gdiplus::Status st = bmp.Save(szFile, &clsid);
    if (st != Gdiplus::Ok) {
        ::MessageBoxW(m_hwnd, UiText(L"Could not save the PNG file.", L"无法保存 PNG 文件。"), L"Nskry", MB_ICONERROR);
        return false;
    }
    return true;
}

// ============================================================================
// In-place Annotation Editing
// ============================================================================

void PinWindow::EnterEditMode() {
    ShowQuickActions(false);
    m_isEditing = true;
    m_editBackup = m_annotationEngine.Clone();
    m_annotationEngine.SetTool(ToolType::Pen);

    RECT bounds{};
    MONITORINFO monitor{sizeof(monitor)};
    if (::GetWindowRect(m_hwnd, &bounds) &&
        ::GetMonitorInfoW(::MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const UINT dpi = ::GetDpiForWindow(m_hwnd);
        const auto &work = monitor.rcWork;
        const LONG width = (std::min)(work.right - work.left,
            (std::max)(bounds.right - bounds.left, static_cast<LONG>(::MulDiv(340, dpi ? dpi : 96, 96))));
        const LONG height = (std::min)(work.bottom - work.top,
            (std::max)(bounds.bottom - bounds.top, static_cast<LONG>(::MulDiv(220, dpi ? dpi : 96, 96))));
        if (width > bounds.right - bounds.left || height > bounds.bottom - bounds.top) {
            m_editOriginalBounds = bounds;
            const LONG x = std::clamp(bounds.left, work.left, work.right - width);
            const LONG y = std::clamp(bounds.top, work.top, work.bottom - height);
            ::SetWindowPos(m_hwnd, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
            ::GetWindowRect(m_hwnd, &m_editExpandedBounds);
        }
    }

    RECT rc{}; ::GetClientRect(m_hwnd, &rc);
    BuildPinToolbar(rc.right, rc.bottom);
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void PinWindow::FinishEdit(bool apply) {
    CommitTextEdit();

    if (!apply) m_annotationEngine = std::move(m_editBackup);
    m_editBackup.Clear();
    m_annotationEngine.ClearSelection();
    m_annotationEngine.SetTool(ToolType::None);
    m_isEditing = false;
    m_mosaicSlider.Hide();
    m_toolbarInput.Clear();
    m_toolbarItems.clear();
    if (!::IsRectEmpty(&m_editExpandedBounds)) {
        RECT current{};
        if (::GetWindowRect(m_hwnd, &current) && ::EqualRect(&current, &m_editExpandedBounds)) {
            const auto &old = m_editOriginalBounds;
            ::SetWindowPos(m_hwnd, nullptr, old.left, old.top, old.right - old.left, old.bottom - old.top,
                           SWP_NOACTIVATE | SWP_NOZORDER);
        }
        m_editOriginalBounds = m_editExpandedBounds = {};
    }
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
    if (m_requiresPinDecision) {
        if (apply) m_requiresPinDecision = false;
        else if (m_hwnd) ::DestroyWindow(m_hwnd);
    }
}

HBITMAP PinWindow::BakeEditedBitmap() {
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

    return baked;
}

void PinWindow::ExportEditedBitmap(bool save) {
    CommitTextEdit();
    HBITMAP rendered = m_annotationEngine.IsEmpty() ? m_bitmap : BakeEditedBitmap();
    if (!rendered) {
        ::MessageBoxW(m_hwnd, L"Failed to prepare the annotated screenshot.", L"Nskry", MB_ICONERROR);
        return;
    }
    const bool exported = save ? SaveToFile(rendered) : CopyToClipboard(rendered);
    if (rendered != m_bitmap) ::DeleteObject(rendered);
    if (exported && m_requiresPinDecision && m_hwnd) ::DestroyWindow(m_hwnd);
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
        m_annotationEngine.CommitText(m_textEditPos, m_textEditTarget, buf.data());
    }

    if (::IsWindow(edit)) ::DestroyWindow(edit);
    if (m_hwnd) ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void PinWindow::BuildPinToolbar(int clientW, int clientH) {
    size_t mainCount = 0;
    m_toolbarItems = BuildAnnotationToolbarItems(m_annotationEngine, m_requiresPinDecision, mainCount);
    const UINT windowDpi = ::GetDpiForWindow(m_hwnd);
    const UINT dpi = windowDpi ? windowDpi : 96;
    const auto widths = MeasureAnnotationToolbarItems(m_hwnd, m_font, m_toolbarItems, dpi);
    const auto layout = LayoutSelectionToolbar(widths, {0, clientH, clientW, clientH},
                                                 {0, 0, clientW, clientH}, mainCount, dpi);
    for (size_t i = 0; i < layout.size(); ++i) m_toolbarItems[i].rect = layout[i];

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
    if (m_isEditing && m_hwnd) {
        m_toolbarInput.Update(m_hwnd, m_hwnd, m_toolbarItems);
        if (m_annotationEngine.GetTool() == ToolType::Mosaic)
            m_mosaicSlider.Update(m_hwnd, m_hwnd, m_toolbarItems.back().rect, dpi, m_annotationEngine);
        else
            m_mosaicSlider.Hide();
    }
}

void PinWindow::DrawPinToolbar(HDC hdc, int /*clientW*/, int /*clientH*/) {
    const auto &palette = m_palette;
    const UINT windowDpi = ::GetDpiForWindow(m_hwnd);
    const UINT dpi = windowDpi ? windowDpi : 96;
    ::SetBkMode(hdc, TRANSPARENT);

    for (const auto& item : m_toolbarItems) {
        if (item.IsSeparator()) {
            int midX = (item.rect.left + item.rect.right) / 2;
            HPEN sepPen = ::CreatePen(PS_SOLID, 1, palette.border);
            HGDIOBJ oldPen = sepPen ? ::SelectObject(hdc, sepPen) : nullptr;
            if (oldPen && oldPen != HGDI_ERROR) {
                ::MoveToEx(hdc, midX, item.rect.top + 3, nullptr);
                ::LineTo(hdc, midX, item.rect.bottom - 3);
                ::SelectObject(hdc, oldPen);
            }
            if (sepPen) ::DeleteObject(sepPen);
            continue;
        }

        if (item.action == AnnotationAction::Color) {
            COLORREF bg = item.hovered ? palette.hover : palette.panel;
            HBRUSH bgBrush = ::CreateSolidBrush(bg);
            ::FillRect(hdc, &item.rect, bgBrush);
            ::DeleteObject(bgBrush);

            int cx = (item.rect.left + item.rect.right) / 2;
            int cy = (item.rect.top + item.rect.bottom) / 2;
            int r = 6;

            HBRUSH colBrush = ::CreateSolidBrush(item.color);
            HPEN borderPen = ::CreatePen(PS_SOLID, 1, item.selected ? palette.accent : palette.border);
            HGDIOBJ oldBrush = colBrush ? ::SelectObject(hdc, colBrush) : nullptr;
            HGDIOBJ oldPen   = borderPen ? ::SelectObject(hdc, borderPen) : nullptr;

            if (oldBrush && oldBrush != HGDI_ERROR && oldPen && oldPen != HGDI_ERROR) {
                ::Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
            }

            if (item.selected) {
                HPEN ringPen = ::CreatePen(PS_SOLID, 2, palette.accent);
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

        const bool primary = item.action == AnnotationAction::Done;
        const auto bg = item.selected || primary ? palette.accent : (item.hovered ? palette.hover : palette.panel);

        HBRUSH bgBrush = ::CreateSolidBrush(bg);
        ::FillRect(hdc, &item.rect, bgBrush);
        ::DeleteObject(bgBrush);

        HBRUSH borderBrush = ::CreateSolidBrush(item.selected ? palette.accent : palette.border);
        ::FrameRect(hdc, &item.rect, borderBrush);
        ::DeleteObject(borderBrush);

        if (item.label) {
            HFONT oldFont = static_cast<HFONT>(::SelectObject(hdc, m_font));
            const auto foreground = item.selected || primary ? palette.onAccent : palette.text;
            if (item.Icon() != EditorIcon::None)
                DrawCenteredEditorIcon(hdc, item.rect, item.Icon(), dpi, foreground);
            else if (item.action == AnnotationAction::MosaicStrength)
                MosaicStrengthSlider::DrawLabel(hdc, item.rect, dpi, foreground);
            else {
                RECT bounds = item.rect;
                ::SetTextColor(hdc, foreground);
                ::DrawTextW(hdc, item.label, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            ::SelectObject(hdc, oldFont);
        }
    }
    m_toolbarInput.DrawFocus(hdc);
}

RECT PinWindow::ImageRect() const {
    if (!m_hwnd || m_width <= 0 || m_height <= 0) return {};
    RECT client{};
    if (!::GetClientRect(m_hwnd, &client)) return {};
    const int clientW = client.right - client.left;
    const int clientH = client.bottom - client.top;
    if (clientW <= 0 || clientH <= 0) return {};

    // Match the monitor window: corner resizing preserves the source aspect
    // ratio through WM_SIZING, while dragging a single edge deliberately
    // stretches that axis to fill the client area.
    return { 0, 0, clientW, clientH };
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
        if (item.IsSeparator()) continue;
        if (::PtInRect(&item.rect, pt)) {
            CommitTextEdit();

            if (item.action == AnnotationAction::Done) { FinishEdit(true); return; }
            if (item.action == AnnotationAction::Cancel) { FinishEdit(false); return; }
            if (item.action == AnnotationAction::Undo) { m_annotationEngine.Undo(); ::InvalidateRect(m_hwnd, nullptr, FALSE); return; }
            if (item.action == AnnotationAction::Redo) { m_annotationEngine.Redo(); ::InvalidateRect(m_hwnd, nullptr, FALSE); return; }
            if (item.action == AnnotationAction::Copy) { ExportEditedBitmap(false); return; }
            if (item.action == AnnotationAction::Save) { ExportEditedBitmap(true); return; }

            if (item.action == AnnotationAction::Width) { // Width
                m_annotationEngine.SetStrokeWidth(item.widthVal);
                m_annotationEngine.SetFontSize(item.widthVal == 2 ? 14 : (item.widthVal == 4 ? 18 : 26));
                RECT rc{}; ::GetClientRect(m_hwnd, &rc);
                BuildPinToolbar(rc.right, rc.bottom);
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }

            if (item.action == AnnotationAction::Color) { // Color
                m_annotationEngine.SetColor(item.color);
                RECT rc{}; ::GetClientRect(m_hwnd, &rc);
                BuildPinToolbar(rc.right, rc.bottom);
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }

            if (item.action == AnnotationAction::MosaicStrength) {
                m_mosaicSlider.Focus();
                return;
            }

            if (item.action == AnnotationAction::ShapeMenu) {
                const auto shape = ChooseAnnotationShape(m_hwnd, m_hwnd, item.rect, m_annotationEngine.GetTool());
                if (shape) {
                    m_annotationEngine.SetTool(*shape);
                    RECT rc{}; ::GetClientRect(m_hwnd, &rc);
                    BuildPinToolbar(rc.right, rc.bottom);
                    ::InvalidateRect(m_hwnd, nullptr, FALSE);
                }
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

    CommitTextEdit();
    POINT bitmapPoint{};
    const int tolerance = AnnotationEngine::HandleRadius(ImageRect(), m_width, m_height, ::GetDpiForWindow(m_hwnd));
    if (!ClientPointToBitmap(x, y, bitmapPoint, true)) return;
    if (m_annotationEngine.BeginPointerEdit(bitmapPoint, tolerance, ::GetKeyState(VK_MENU) < 0)) {
        m_isDrawing = true;
        ::SetCapture(m_hwnd);
    } else if (ClientPointToBitmap(x, y, bitmapPoint)) {
        if (m_annotationEngine.GetTool() == ToolType::Text) {
            OpenTextEdit({x, y}, bitmapPoint);
        } else if (m_annotationEngine.GetTool() != ToolType::None) {
            m_isDrawing = true;
            m_annotationEngine.OnMouseDown(bitmapPoint);
            ::SetCapture(m_hwnd);
        }
    }
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void PinWindow::OpenTextEdit(POINT clientPoint, POINT image, std::optional<AnnotationEngine::TextTarget> target) {
    m_textEditPos = image;
    m_textEditTarget = target ? std::optional{target->index} : std::nullopt;
    RECT client{};
    ::GetClientRect(m_hwnd, &client);
    const int clientW = static_cast<int>(client.right - client.left);
    const int clientH = static_cast<int>(client.bottom - client.top);
    const int editW = (std::min)(target ? 220 : 120, (std::max)(1, clientW));
    const int editX = (std::clamp)(int(clientPoint.x), 0, (std::max)(0, clientW - editW));
    const int editY = (std::clamp)(int(clientPoint.y), 0, (std::max)(0, clientH - 24));
    m_hTextEdit = ::CreateWindowExW(
        0, L"EDIT", target ? target->text.c_str() : L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        editX, editY, editW, 24,
        m_hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(102)),
        ::GetModuleHandleW(nullptr), nullptr);
    if (m_hTextEdit) {
        if (m_font) ::SendMessageW(m_hTextEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        ::SetWindowSubclass(m_hTextEdit,TextKeyRelay,1,DWORD_PTR(m_hwnd));
        if (target) ::SendMessageW(m_hTextEdit, EM_SETSEL, 0, -1);
        ::SetFocus(m_hTextEdit);
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
        if (item.IsSeparator()) continue;
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
        m_isDrawing = false;
        POINT bitmapPoint{};
        if (ClientPointToBitmap(x, y, bitmapPoint, true)) {
            m_annotationEngine.OnMouseUp(bitmapPoint);
        } else {
            m_annotationEngine.CancelPointer();
        }
        ::ReleaseCapture();
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void PinWindow::OnPaint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC paintDc = ::BeginPaint(hwnd, &ps);
    if (!paintDc) return;

    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    int clientW = rc.right;
    int clientH = rc.bottom;
    HDC hdc = ::CreateCompatibleDC(paintDc);
    HBITMAP back = hdc ? ::CreateCompatibleBitmap(paintDc, clientW, clientH) : nullptr;
    HGDIOBJ oldBack = back ? ::SelectObject(hdc, back) : nullptr;
    if (!hdc || !back || !oldBack || oldBack == HGDI_ERROR) {
        if (back) ::DeleteObject(back);
        if (hdc) ::DeleteDC(hdc);
        ::EndPaint(hwnd, &ps);
        return;
    }

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

    if (sourceReady && !m_annotationEngine.IsEmpty()) {
        Gdiplus::Graphics g(hdc);
        Gdiplus::Matrix transform(
            static_cast<Gdiplus::REAL>(image.right - image.left) / m_width, 0.0f,
            0.0f, static_cast<Gdiplus::REAL>(image.bottom - image.top) / m_height,
            static_cast<Gdiplus::REAL>(image.left), static_cast<Gdiplus::REAL>(image.top));
        g.SetTransform(&transform);
        m_annotationEngine.Draw(g, hdcMem, 0, 0, m_width, m_height);
        if (m_isEditing)
            m_annotationEngine.DrawSelection(g, AnnotationEngine::HandleRadius(image, m_width, m_height, ::GetDpiForWindow(hwnd)));
    }

    // Draw the toolbar even when the source bitmap could not be selected, so
    // users can still cancel editing or close the window.
    if (m_isEditing) DrawPinToolbar(hdc, clientW, clientH);

    if (sourceReady) ::SelectObject(hdcMem, old);
    if (hdcMem) ::DeleteDC(hdcMem);

    ::BitBlt(paintDc, 0, 0, clientW, clientH, hdc, 0, 0, SRCCOPY);
    ::SelectObject(hdc, oldBack);
    ::DeleteObject(back);
    ::DeleteDC(hdc);
    ::EndPaint(hwnd, &ps);
}

} // namespace nskry
