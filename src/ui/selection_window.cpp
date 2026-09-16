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
        -13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_fontIcon = ::CreateFontW(
        -15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");

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
    CommitTextEdit();
    Close();
    CleanupGdi();
    if (m_font)     { ::DeleteObject(m_font); m_font = nullptr; }
    if (m_fontIcon) { ::DeleteObject(m_fontIcon); m_fontIcon = nullptr; }
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
        if (m_annotationEngine.GetTool() != ToolType::None) {
            CommitTextEdit();
            m_annotationEngine.SetTool(ToolType::None);
            BuildToolbar(m_finalRect);
            ::InvalidateRect(m_hwnd, nullptr, FALSE);
            return 0;
        }
        FinishWithAction(SelectionAction::Cancel);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (m_annotationEngine.GetTool() != ToolType::None) {
                CommitTextEdit();
                m_annotationEngine.SetTool(ToolType::None);
                BuildToolbar(m_finalRect);
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }
            FinishWithAction(SelectionAction::Cancel);
            return 0;
        }
        if (::GetKeyState(VK_CONTROL) & 0x8000) {
            if (wp == 'Z') {
                CommitTextEdit();
                m_annotationEngine.Undo();
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == 'Y') {
                CommitTextEdit();
                m_annotationEngine.Redo();
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == 'C') {
                FinishWithAction(SelectionAction::Copy);
                return 0;
            }
            if (wp == 'S') {
                FinishWithAction(SelectionAction::Save);
                return 0;
            }
        }
        break;

    case WM_SETCURSOR:
        if (m_state == State::Selected || m_state == State::Adjusting || m_state == State::Annotating) {
            POINT pt;
            ::GetCursorPos(&pt);
            ::ScreenToClient(hwnd, &pt);
            HitZone zone = (m_state == State::Adjusting) ? m_activeZone : HitTest(pt.x, pt.y);
            UpdateCursor(zone);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (HIWORD(wp) == EN_KILLFOCUS && reinterpret_cast<HWND>(lp) == m_hTextEdit) {
            CommitTextEdit();
            return 0;
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

    // 1. Toolbar items
    for (const auto& item : m_toolbarItems) {
        if (!item.isSeparator && ::PtInRect(&item.rect, pt)) return HitZone::Toolbar;
    }

    // 2. If annotation tool is active, inside area is canvas for drawing
    if (m_annotationEngine.GetTool() != ToolType::None) {
        if (::PtInRect(&m_finalRect, pt)) return HitZone::Inside;
    }

    // 3. Selection adjustment handles
    const int grip = 6;
    RECT r = m_finalRect;

    RECT tl = { r.left - grip, r.top - grip, r.left + grip, r.top + grip };
    RECT tr = { r.right - grip, r.top - grip, r.right + grip, r.top + grip };
    RECT bl = { r.left - grip, r.bottom - grip, r.left + grip, r.bottom + grip };
    RECT br = { r.right - grip, r.bottom - grip, r.right + grip, r.bottom + grip };
    if (::PtInRect(&tl, pt)) return HitZone::TopLeft;
    if (::PtInRect(&tr, pt)) return HitZone::TopRight;
    if (::PtInRect(&bl, pt)) return HitZone::BottomLeft;
    if (::PtInRect(&br, pt)) return HitZone::BottomRight;

    RECT top = { r.left, r.top - grip, r.right, r.top + grip };
    RECT bot = { r.left, r.bottom - grip, r.right, r.bottom + grip };
    RECT lft = { r.left - grip, r.top, r.left + grip, r.bottom };
    RECT rgt = { r.right - grip, r.top, r.right + grip, r.bottom };
    if (::PtInRect(&top, pt)) return HitZone::Top;
    if (::PtInRect(&bot, pt)) return HitZone::Bottom;
    if (::PtInRect(&lft, pt)) return HitZone::Left;
    if (::PtInRect(&rgt, pt)) return HitZone::Right;

    if (::PtInRect(&r, pt)) return HitZone::Inside;

    return HitZone::None;
}

void SelectionWindow::UpdateCursor(HitZone zone) {
    if (m_state == State::Annotating || (m_annotationEngine.GetTool() != ToolType::None && zone == HitZone::Inside)) {
        ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32515))); // IDC_CROSS
        return;
    }

    LPCWSTR id = MAKEINTRESOURCEW(32515);
    switch (zone) {
    case HitZone::TopLeft:
    case HitZone::BottomRight: id = MAKEINTRESOURCEW(32642); break; // IDC_SIZENWSE
    case HitZone::TopRight:
    case HitZone::BottomLeft:  id = MAKEINTRESOURCEW(32643); break; // IDC_SIZENESW
    case HitZone::Top:
    case HitZone::Bottom:      id = MAKEINTRESOURCEW(32645); break; // IDC_SIZENS
    case HitZone::Left:
    case HitZone::Right:       id = MAKEINTRESOURCEW(32644); break; // IDC_SIZEWE
    case HitZone::Inside:      id = MAKEINTRESOURCEW(32646); break; // IDC_SIZEALL
    case HitZone::Toolbar:     id = MAKEINTRESOURCEW(32649); break; // IDC_HAND
    case HitZone::None:        id = MAKEINTRESOURCEW(32515); break;
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
        for (auto& item : m_toolbarItems) {
            if (item.isSeparator) continue;
            POINT pt{ x, y };
            bool inside = ::PtInRect(&item.rect, pt);
            if (inside != item.hovered) {
                item.hovered = inside;
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

        if (r.left > r.right) std::swap(r.left, r.right);
        if (r.top > r.bottom) std::swap(r.top, r.bottom);

        r.left   = (std::max)(0, static_cast<int>(r.left));
        r.top    = (std::max)(0, static_cast<int>(r.top));
        r.right  = (std::min)(m_vW, static_cast<int>(r.right));
        r.bottom = (std::min)(m_vH, static_cast<int>(r.bottom));

        m_finalRect = r;
        BuildToolbar(m_finalRect);
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    else if (m_state == State::Annotating) {
        POINT localPt{ x - m_finalRect.left, y - m_finalRect.top };
        m_annotationEngine.OnMouseMove(localPt);
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
        POINT pt{ x, y };

        // 1. Check toolbar click
        for (const auto& item : m_toolbarItems) {
            if (item.isSeparator) continue;
            if (::PtInRect(&item.rect, pt)) {
                CommitTextEdit();

                switch (item.type) {
                case ToolbarItemType::Action:
                    FinishWithAction(item.action);
                    return;

                case ToolbarItemType::ToolToggle:
                    if (m_annotationEngine.GetTool() == item.tool) {
                        m_annotationEngine.SetTool(ToolType::None);
                    } else {
                        m_annotationEngine.SetTool(item.tool);
                    }
                    BuildToolbar(m_finalRect);
                    ::InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;

                case ToolbarItemType::Undo:
                    m_annotationEngine.Undo();
                    ::InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;

                case ToolbarItemType::Redo:
                    m_annotationEngine.Redo();
                    ::InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;

                case ToolbarItemType::WidthChoice:
                    m_annotationEngine.SetStrokeWidth(item.widthVal);
                    BuildToolbar(m_finalRect);
                    ::InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;

                case ToolbarItemType::ColorChoice:
                    m_annotationEngine.SetColor(item.color);
                    BuildToolbar(m_finalRect);
                    ::InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;
                }
            }
        }

        // 2. Check canvas annotation or adjustment
        if (m_annotationEngine.GetTool() != ToolType::None && ::PtInRect(&m_finalRect, pt)) {
            CommitTextEdit();

            if (m_annotationEngine.GetTool() == ToolType::Text) {
                // In-place text input
                m_textEditPos = { x - m_finalRect.left, y - m_finalRect.top };
                m_hTextEdit = ::CreateWindowExW(
                    0, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                    x, y, 140, 26,
                    m_hwnd,
                    reinterpret_cast<HMENU>(static_cast<UINT_PTR>(101)),
                    ::GetModuleHandleW(nullptr), nullptr);
                ::SendMessageW(m_hTextEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
                ::SetFocus(m_hTextEdit);
            } else {
                m_state = State::Annotating;
                POINT localPt{ x - m_finalRect.left, y - m_finalRect.top };
                m_annotationEngine.OnMouseDown(localPt);
                ::SetCapture(m_hwnd);
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return;
        }

        // 3. Selection handle adjustment
        HitZone zone = HitTest(x, y);
        if (zone != HitZone::None && zone != HitZone::Toolbar) {
            CommitTextEdit();
            m_state = State::Adjusting;
            m_activeZone = zone;
            m_adjustStartPt = { x, y };
            m_adjustStartRect = m_finalRect;
            ::SetCapture(m_hwnd);
        } else {
            // Clicked outside — reset selection to hovering
            CommitTextEdit();
            m_annotationEngine.Clear();
            m_annotationEngine.SetTool(ToolType::None);
            m_state = State::Hovering;
            m_toolbarItems.clear();
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
        BuildToolbar(m_finalRect);
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    else if (m_state == State::Annotating) {
        ::ReleaseCapture();
        POINT localPt{ x - m_finalRect.left, y - m_finalRect.top };
        m_annotationEngine.OnMouseUp(localPt);
        m_state = State::Selected;
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void SelectionWindow::CommitTextEdit() {
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

// ============================================================================
// Toolbar Construction & Painting
// ============================================================================

void SelectionWindow::BuildToolbar(RECT selRect) {
    m_toolbarItems.clear();

    const bool hasSub = (m_annotationEngine.GetTool() != ToolType::None);

    // Button definitions for the main bar
    struct MainBtnDef {
        ToolbarItemType type;
        const wchar_t*  label;
        ToolType        tool;
        SelectionAction action;
        int             width;
        bool            isSep;
    };

    MainBtnDef mainDefs[] = {
        // Annotation Tools
        { ToolbarItemType::ToolToggle, L"矩形",   ToolType::Rect,    SelectionAction::Cancel, 38, false },
        { ToolbarItemType::ToolToggle, L"圆形",   ToolType::Ellipse, SelectionAction::Cancel, 38, false },
        { ToolbarItemType::ToolToggle, L"箭头",   ToolType::Arrow,   SelectionAction::Cancel, 38, false },
        { ToolbarItemType::ToolToggle, L"画笔",   ToolType::Pen,     SelectionAction::Cancel, 38, false },
        { ToolbarItemType::ToolToggle, L"马赛克", ToolType::Mosaic,  SelectionAction::Cancel, 48, false },
        { ToolbarItemType::ToolToggle, L"文本",   ToolType::Text,    SelectionAction::Cancel, 38, false },

        // Separator
        { ToolbarItemType::Action,     nullptr,   ToolType::None,    SelectionAction::Cancel, 6,  true  },

        // Edit History
        { ToolbarItemType::Undo,       L"撤销",   ToolType::None,    SelectionAction::Cancel, 38, false },
        { ToolbarItemType::Redo,       L"重做",   ToolType::None,    SelectionAction::Cancel, 38, false },

        // Separator
        { ToolbarItemType::Action,     nullptr,   ToolType::None,    SelectionAction::Cancel, 6,  true  },

        // Actions
        { ToolbarItemType::Action,     L"⎘ 复制", ToolType::None,    SelectionAction::Copy, 56, false },
        { ToolbarItemType::Action,     L"↓ 保存", ToolType::None,    SelectionAction::Save, 56, false },
        { ToolbarItemType::Action,     L"↗ 固定", ToolType::None,    SelectionAction::Pin,  56, false },
        { ToolbarItemType::Action,     L"▶ 监控", ToolType::None,    SelectionAction::PiP,  56, false },
    };

    constexpr int gap = 3;
    constexpr int barH = 32;

    int totalMainW = 0;
    for (const auto& def : mainDefs) totalMainW += def.width + gap;
    totalMainW -= gap;

    // Center horizontally relative to selection
    int centerX = (selRect.left + selRect.right) / 2;
    int startX  = centerX - totalMainW / 2;
    if (startX < 4) startX = 4;
    if (startX + totalMainW > m_vW - 4) startX = m_vW - 4 - totalMainW;

    // Y positioning: default below selection
    int mainY = selRect.bottom + 8;
    int subY  = mainY + barH + 4;

    if (mainY + barH + (hasSub ? 34 : 0) > m_vH - 4) {
        // Not enough room below: place above
        if (hasSub) {
            subY  = selRect.top - 28 - 8;
            mainY = subY - barH - 4;
        } else {
            mainY = selRect.top - barH - 8;
        }
    }
    if (mainY < 4) mainY = 4;

    // Populate Main Bar
    int curX = startX;
    for (const auto& def : mainDefs) {
        ToolbarItem item{};
        item.rect        = { curX, mainY, curX + def.width, mainY + barH };
        item.type        = def.type;
        item.label       = def.label;
        item.tool        = def.tool;
        item.action      = def.action;
        item.isSeparator = def.isSep;
        item.selected    = (def.type == ToolbarItemType::ToolToggle && m_annotationEngine.GetTool() == def.tool);

        m_toolbarItems.push_back(item);
        curX += def.width + gap;
    }

    // Populate Secondary Sub-bar (if tool is active)
    if (hasSub) {
        const int subItemH = 26;
        const int subBarW = 320;
        int subStartX = centerX - subBarW / 2;
        if (subStartX < 4) subStartX = 4;
        if (subStartX + subBarW > m_vW - 4) subStartX = m_vW - 4 - subBarW;

        int sx = subStartX;

        // 3 stroke widths
        int widths[] = { 2, 4, 8 };
        const wchar_t* wLabels[] = { L"\x25CF 2", L"\x25CF 4", L"\x25CF 8" };
        for (int i = 0; i < 3; i++) {
            ToolbarItem item{};
            item.rect     = { sx, subY, sx + 34, subY + subItemH };
            item.type     = ToolbarItemType::WidthChoice;
            item.widthVal = widths[i];
            item.label    = wLabels[i];
            item.selected = (m_annotationEngine.GetStrokeWidth() == widths[i]);
            m_toolbarItems.push_back(item);
            sx += 34 + gap;
        }

        // Separator
        ToolbarItem sep{};
        sep.rect        = { sx, subY, sx + 6, subY + subItemH };
        sep.isSeparator = true;
        m_toolbarItems.push_back(sep);
        sx += 6 + gap;

        // 8 Palette Colors
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
            ToolbarItem item{};
            item.rect     = { sx, subY, sx + 22, subY + subItemH };
            item.type     = ToolbarItemType::ColorChoice;
            item.color    = colors[i];
            item.selected = (m_annotationEngine.GetColor() == colors[i]);
            m_toolbarItems.push_back(item);
            sx += 22 + gap;
        }
    }
}

void SelectionWindow::DrawToolbar(HDC hdc) {
    ::SetBkMode(hdc, TRANSPARENT);

    for (const auto& item : m_toolbarItems) {
        if (item.isSeparator) {
            // Separator vertical line
            int midX = (item.rect.left + item.rect.right) / 2;
            HPEN sepPen = ::CreatePen(PS_SOLID, 1, RGB(70, 70, 80));
            HGDIOBJ oldPen = ::SelectObject(hdc, sepPen);
            ::MoveToEx(hdc, midX, item.rect.top + 4, nullptr);
            ::LineTo(hdc, midX, item.rect.bottom - 4);
            ::SelectObject(hdc, oldPen);
            ::DeleteObject(sepPen);
            continue;
        }

        if (item.type == ToolbarItemType::ColorChoice) {
            // Palette color circle/swatch
            COLORREF bg = item.hovered ? RGB(60, 60, 70) : RGB(45, 45, 52);
            HBRUSH bgBrush = ::CreateSolidBrush(bg);
            ::FillRect(hdc, &item.rect, bgBrush);
            ::DeleteObject(bgBrush);

            // Color circle inside
            int cx = (item.rect.left + item.rect.right) / 2;
            int cy = (item.rect.top + item.rect.bottom) / 2;
            int r  = 7;

            HBRUSH colBrush = ::CreateSolidBrush(item.color);
            HPEN borderPen  = ::CreatePen(PS_SOLID, 1, item.selected ? RGB(255, 255, 255) : RGB(80, 80, 90));
            HGDIOBJ oldBrush = ::SelectObject(hdc, colBrush);
            HGDIOBJ oldPen   = ::SelectObject(hdc, borderPen);

            ::Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);

            // Selected outline ring
            if (item.selected) {
                HPEN ringPen = ::CreatePen(PS_SOLID, 2, RGB(0, 150, 255));
                ::SelectObject(hdc, ringPen);
                ::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
                ::Ellipse(hdc, cx - r - 2, cy - r - 2, cx + r + 3, cy + r + 3);
                ::DeleteObject(ringPen);
            }

            ::SelectObject(hdc, oldBrush);
            ::SelectObject(hdc, oldPen);
            ::DeleteObject(colBrush);
            ::DeleteObject(borderPen);
            continue;
        }

        // Standard Button
        COLORREF bgColor;
        if (item.selected) {
            bgColor = RGB(25, 118, 210); // Active blue
        } else if (item.hovered) {
            bgColor = RGB(65, 65, 75);
        } else {
            bgColor = RGB(45, 45, 52);
        }

        HBRUSH bgBrush = ::CreateSolidBrush(bgColor);
        ::FillRect(hdc, &item.rect, bgBrush);
        ::DeleteObject(bgBrush);

        HBRUSH borderBrush = ::CreateSolidBrush(item.selected ? RGB(70, 160, 245) : (item.hovered ? RGB(85, 85, 95) : RGB(65, 65, 75)));
        ::FrameRect(hdc, &item.rect, borderBrush);
        ::DeleteObject(borderBrush);

        if (item.label) {
            HFONT oldFont = static_cast<HFONT>(::SelectObject(hdc, m_font));

            ::SetTextColor(hdc, item.selected ? RGB(255, 255, 255) : RGB(235, 235, 240));
            RECT textRect = item.rect;
            ::DrawTextW(hdc, item.label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            ::SelectObject(hdc, oldFont);
        }
    }
}

void SelectionWindow::DrawInfoBox(HDC hdc, RECT selRect) {
    int w = selRect.right - selRect.left;
    int h = selRect.bottom - selRect.top;

    wchar_t text[64];
    ::_snwprintf_s(text, _TRUNCATE, L"%d \u00D7 %d", w, h);

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
        CommitTextEdit();

        // If there are annotations, bake them onto the bitmap; otherwise normal capture
        if (m_annotationEngine.IsEmpty()) {
            m_capturedBitmap = CaptureSelectedRegion(m_finalRect);
        } else {
            m_capturedBitmap = m_annotationEngine.BakeToBitmap(
                m_hdcSnapshot, m_finalRect,
                m_finalRect.right - m_finalRect.left,
                m_finalRect.bottom - m_finalRect.top);
        }

        result.targetHwnd  = m_targetHwnd;
        result.bitmap      = m_capturedBitmap;
        result.bitmapWidth = m_finalRect.right  - m_finalRect.left;
        result.bitmapHeight= m_finalRect.bottom - m_finalRect.top;

        int screenLeft = m_finalRect.left + m_vX;
        int screenTop  = m_finalRect.top  + m_vY;
        result.crop.x      = screenLeft - m_targetBounds.left;
        result.crop.y      = screenTop  - m_targetBounds.top;
        result.crop.width  = result.bitmapWidth;
        result.crop.height = result.bitmapHeight;

        // If PiP action and we have annotations, extract the overlay
        if (action == SelectionAction::PiP && !m_annotationEngine.IsEmpty()) {
            result.overlayPixels = m_annotationEngine.RenderOverlayRgba(
                m_hdcSnapshot, m_finalRect.left, m_finalRect.top,
                result.crop.width, result.crop.height);
        }

        m_capturedBitmap = nullptr; // transferred to caller
    } else {
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

    // 3. Determine active rectangle
    RECT activeRect{};
    if (m_state == State::Selected || m_state == State::Adjusting || m_state == State::Annotating)
        activeRect = m_finalRect;
    else if (m_state == State::Dragging)
        activeRect = m_dragRect;
    else
        activeRect = m_hoveredRect;

    // 4. Punch through the mask
    if (activeRect.right > activeRect.left && activeRect.bottom > activeRect.top) {
        ::BitBlt(hdcBack,
            activeRect.left, activeRect.top,
            activeRect.right - activeRect.left, activeRect.bottom - activeRect.top,
            m_hdcSnapshot, activeRect.left, activeRect.top, SRCCOPY);

        // 4.1 Render annotations in activeRect
        if (m_state == State::Selected || m_state == State::Adjusting || m_state == State::Annotating) {
            Gdiplus::Graphics g(hdcBack);
            g.TranslateTransform(
                static_cast<Gdiplus::REAL>(activeRect.left),
                static_cast<Gdiplus::REAL>(activeRect.top));
            m_annotationEngine.Draw(g, m_hdcSnapshot, activeRect.left, activeRect.top, m_vW, m_vH);
        }

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

    // 5. Draw toolbar if in Selected, Adjusting, or Annotating state
    if (m_state == State::Selected || m_state == State::Adjusting || m_state == State::Annotating) {
        DrawToolbar(hdcBack);
    }

    // 6. Blit to screen
    ::BitBlt(hdcPaint, 0, 0, m_vW, m_vH, hdcBack, 0, 0, SRCCOPY);

    ::SelectObject(hdcBack, oldBack);
    ::DeleteObject(hbmBack);
    ::DeleteDC(hdcBack);

    ::EndPaint(hwnd, &ps);
}

} // namespace nskry
