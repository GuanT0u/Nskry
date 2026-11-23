#include "ui/ui_preferences.h"
#include "ui/settings/interaction_page.h"
#include "pch.h"
#include <commctrl.h>
#include <commdlg.h>
namespace nskry {
InteractionPage::InteractionPage(HWND parent) {
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSW c{};
        c.lpfnWndProc = Proc;
        c.hInstance = GetModuleHandleW(nullptr);
        c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        c.hbrBackground = HBRUSH(COLOR_WINDOW + 1);
        c.lpszClassName = L"NskryInteractionPage";
        RegisterClassW(&c);
    });
    m_hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"NskryInteractionPage", L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 10, 10,
                             parent, nullptr, nullptr, this);
    auto control = [&](const wchar_t *cls, const wchar_t *text, int id, int x, int y, int w, int h, DWORD flags = 0) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | flags, x, y, w, h, m_hwnd, HMENU(INT_PTR(id)),
                                 nullptr, nullptr);
        SendMessageW(c, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        m_items.push_back({c, x, y, w, h});
        return c;
    };
    auto combo = [&](int id, int y, std::initializer_list<const wchar_t *> values) {
        auto c = control(WC_COMBOBOXW, L"", id, 260, y, 300, 200, CBS_DROPDOWNLIST | WS_TABSTOP);
        for (auto v : values)
            SendMessageW(c, CB_ADDSTRING, 0, LPARAM(v));
        return c;
    };
    auto slider = [&](int id, int y, int min, int max) {
        auto c = control(TRACKBAR_CLASSW, L"", id, 260, y, 270, 30, TBS_HORZ | WS_TABSTOP);
        SendMessageW(c, TBM_SETRANGE, TRUE, MAKELPARAM(min, max));
        control(L"STATIC", L"", id + 100, 544, y, 55, 25);
    };
    control(L"STATIC", UiText(L"Selection border (display only)", L"选区边线（仅界面显示）"), 0, 8, 8, 580, 24);
    control(L"STATIC", UiText(L"Line style", L"线型"), 0, 16, 40, 220, 24);
    combo(1, 36, {UiText(L"Solid", L"实线"), UiText(L"Dashed", L"虚线")});
    control(L"STATIC", UiText(L"Width (1-6 px)", L"宽度（1–6 px）"), 0, 16, 80, 220, 24);
    slider(2, 76, 1, 6);
    control(L"STATIC", UiText(L"Opacity (%)", L"不透明度（%）"), 0, 16, 120, 220, 24);
    slider(3, 116, 0, 100);
    control(L"BUTTON", UiText(L"Border color...", L"边线颜色…"), 4, 16, 156, 230, 30, BS_PUSHBUTTON | WS_TABSTOP);
    combo(5, 156, {UiText(L"Blue", L"蓝色"), UiText(L"Amber", L"琥珀色"), UiText(L"Red", L"红色"), UiText(L"White", L"白色"), UiText(L"Black", L"黑色"), UiText(L"Custom", L"自定义")});
    control(L"STATIC", UiText(L"Cut preview (never exported)", L"切除预览（仅界面显示）"), 0, 8, 206, 580, 24);
    control(L"STATIC", UiText(L"Preview style", L"预览样式"), 0, 16, 240, 220, 24);
    combo(6, 236, {UiText(L"Translucent mask", L"半透明蒙层"), UiText(L"Mask + grid", L"蒙层和网格"), UiText(L"Cut edges only", L"仅显示切除边线"), UiText(L"Legacy hatch", L"交叉纹理")});
    control(L"STATIC", UiText(L"Mask opacity (%)", L"蒙层不透明度（%）"), 0, 16, 280, 220, 24);
    slider(7, 276, 0, 100);
    control(L"BUTTON", UiText(L"Mask color...", L"蒙层颜色…"), 8, 16, 316, 230, 30, BS_PUSHBUTTON | WS_TABSTOP);
    combo(9, 316, {UiText(L"Red", L"红色"), UiText(L"Black", L"黑色"), UiText(L"Custom", L"自定义")});
    control(L"BUTTON", UiText(L"Also show cut edges", L"同时显示切除边线"), 10, 16, 354, 400, 24, BS_AUTOCHECKBOX | WS_TABSTOP);
    control(L"STATIC", UiText(L"Cursor by interaction state", L"各操作的光标"), 0, 8, 398, 580, 24);
    const wchar_t *names[] = {UiText(L"Selection / dragging", L"框选／拖动"), UiText(L"Selection move", L"移动选区"), UiText(L"Cut rows", L"切除行"), UiText(L"Cut columns", L"切除列"), UiText(L"Image pan", L"平移图片")};
    for (int i = 0; i < 5; ++i) {
        int y = 434 + i * 84;
        control(L"STATIC", names[i], 0, 16, y, 220, 24);
        m_cursor[i] = combo(
            20 + i, y,
            {UiText(L"Arrow", L"箭头"), UiText(L"Cross", L"十字"), UiText(L"Hand", L"手形"), UiText(L"Move", L"移动"), UiText(L"Horizontal resize", L"横向调整大小"), UiText(L"Vertical resize", L"竖向调整大小"), UiText(L"Custom .cur / .ani", L"自定义 .cur／.ani")});
        control(L"BUTTON", UiText(L"Choose cursor...", L"选择光标…"), 30 + i, 16, y + 34, 220, 30, BS_PUSHBUTTON | WS_TABSTOP);
        m_files[i] = control(L"STATIC", L"", 0, 260, y + 38, 330, 30);
    }
    control(L"BUTTON", UiText(L"Reset input appearance", L"重置输入外观"), 50, 16, 846, 260, 30, BS_PUSHBUTTON | WS_TABSTOP);
    Load({});
}
InteractionPage::~InteractionPage() {
    if (m_hwnd)
        DestroyWindow(m_hwnd);
}
void InteractionPage::Load(InteractionStyle s) {
    m_style = NormalizeInteractionStyle(s);
    SendDlgItemMessageW(m_hwnd, 1, CB_SETCURSEL, s.dashed ? 1 : 0, 0);
    for (auto [id, value] : {std::pair{2, s.borderWidth}, {3, s.borderOpacity}, {7, s.cutOpacity}}) {
        SendDlgItemMessageW(m_hwnd, id, TBM_SETPOS, TRUE, value);
        SetDlgItemTextW(m_hwnd, id + 100, std::to_wstring(value).c_str());
    }
    const COLORREF colors[] = {RGB(20, 150, 255), RGB(255, 180, 30), RGB(220, 50, 50), RGB(255, 255, 255),
                               RGB(0, 0, 0)};
    int preset = 5;
    for (int i = 0; i < 5; ++i)
        if (s.borderColor == colors[i])
            preset = i;
    SendDlgItemMessageW(m_hwnd, 5, CB_SETCURSEL, preset, 0);
    SendDlgItemMessageW(m_hwnd, 9, CB_SETCURSEL,
                        s.cutColor == RGB(190, 30, 30) ? 0
                        : s.cutColor == RGB(0, 0, 0)   ? 1
                                                       : 2,
                        0);
    SendDlgItemMessageW(m_hwnd, 6, CB_SETCURSEL, s.cutStyle, 0);
    SendDlgItemMessageW(m_hwnd, 10, BM_SETCHECK, s.cutEdges || s.cutStyle == 2 ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(GetDlgItem(m_hwnd, 10), s.cutStyle != 2);
    for (size_t i = 0; i < m_cursor.size(); ++i) {
        SendMessageW(m_cursor[i], CB_SETCURSEL, int(s.cursors[i].style), 0);
        SetWindowTextW(m_files[i], s.cursors[i].file.c_str());
    }
}
InteractionStyle InteractionPage::Value() {
    m_style.dashed = SendDlgItemMessageW(m_hwnd, 1, CB_GETCURSEL, 0, 0) == 1;
    m_style.borderWidth = int(SendDlgItemMessageW(m_hwnd, 2, TBM_GETPOS, 0, 0));
    m_style.borderOpacity = int(SendDlgItemMessageW(m_hwnd, 3, TBM_GETPOS, 0, 0));
    m_style.cutOpacity = int(SendDlgItemMessageW(m_hwnd, 7, TBM_GETPOS, 0, 0));
    m_style.cutStyle = int(SendDlgItemMessageW(m_hwnd, 6, CB_GETCURSEL, 0, 0));
    m_style.cutEdges = SendDlgItemMessageW(m_hwnd, 10, BM_GETCHECK, 0, 0) == BST_CHECKED;
    for (size_t i = 0; i < m_cursor.size(); ++i)
        m_style.cursors[i].style = CursorStyle(SendMessageW(m_cursor[i], CB_GETCURSEL, 0, 0));
    return NormalizeInteractionStyle(m_style);
}
void InteractionPage::Layout() {
    RECT r{};
    GetClientRect(m_hwnd, &r);
    const UINT dpi = ::GetDpiForWindow(m_hwnd);
    const int units = dpi ? dpi : 96;
    const auto move = [units](HWND control, HWND, int x, int y, int w, int h, UINT flags) {
        const auto scale = [units](int n) { return ::MulDiv(n, units, 96); };
        ::SetWindowPos(control, nullptr, scale(x), scale(y), scale(w), scale(h), flags);
    };
    r.right = ::MulDiv(r.right, 96, units);
    r.bottom = ::MulDiv(r.bottom, 96, units);
    SCROLLINFO s{sizeof(s), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 879, UINT(r.bottom), m_scroll};
    SetScrollInfo(m_hwnd, SB_VERT, &s, TRUE);
    for (auto i : m_items)
        move(i.hwnd, nullptr, i.x, i.y - m_scroll, i.w, i.h, SWP_NOZORDER | SWP_NOACTIVATE);
}
void InteractionPage::Scroll(int pos) {
    RECT r{};
    GetClientRect(m_hwnd, &r);
    const UINT dpi = ::GetDpiForWindow(m_hwnd);
    const int units = dpi ? dpi : 96;
    r.right = ::MulDiv(r.right, 96, units);
    r.bottom = ::MulDiv(r.bottom, 96, units);
    m_scroll = std::clamp(pos, 0, (std::max)(0, 880 - int(r.bottom)));
    Layout();
}
LRESULT CALLBACK InteractionPage::Proc(HWND w, UINT m, WPARAM p, LPARAM l) {
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(w, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
    auto self = reinterpret_cast<InteractionPage *>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (self && m == WM_NCCREATE)
        self->m_hwnd = w;
    if (self && m == WM_NCDESTROY) {
        self->m_hwnd = nullptr;
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        return DefWindowProcW(w, m, p, l);
    }
    return self ? self->Handle(m, p, l) : DefWindowProcW(w, m, p, l);
}
LRESULT InteractionPage::Handle(UINT m, WPARAM p, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        Scroll(m_scroll);
        return 0;
    case WM_MOUSEWHEEL:
        Scroll(m_scroll - GET_WHEEL_DELTA_WPARAM(p) / WHEEL_DELTA * 100);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO s{sizeof(s), SIF_ALL};
        GetScrollInfo(m_hwnd, SB_VERT, &s);
        int v = m_scroll;
        switch (LOWORD(p)) {
        case SB_LINEUP:
            v -= 35;
            break;
        case SB_LINEDOWN:
            v += 35;
            break;
        case SB_PAGEUP:
            v -= int(s.nPage);
            break;
        case SB_PAGEDOWN:
            v += int(s.nPage);
            break;
        case SB_THUMBTRACK:
            v = s.nTrackPos;
            break;
        }
        Scroll(v);
        return 0;
    }
    case WM_HSCROLL: {
        ::PostMessageW(::GetParent(m_hwnd), WM_NSKRY_SETTINGS_DIRTY, 0, 0);
        int id = GetDlgCtrlID(HWND(l));
        SetDlgItemTextW(m_hwnd, id + 100, std::to_wstring(SendMessageW(HWND(l), TBM_GETPOS, 0, 0)).c_str());
        return 0;
    }
    case WM_COMMAND: {
        ::PostMessageW(::GetParent(m_hwnd), WM_NSKRY_SETTINGS_DIRTY, 0, 0);
        int id = LOWORD(p);
        if (id == 50) {
            Load({});
            return 0;
        }
        if (id == 4 || id == 8) {
            Value();
            COLORREF colors[16]{};
            CHOOSECOLORW c{sizeof(c)};
            c.hwndOwner = m_hwnd;
            c.lpCustColors = colors;
            c.Flags = CC_FULLOPEN | CC_RGBINIT;
            c.rgbResult = id == 4 ? m_style.borderColor : m_style.cutColor;
            if (ChooseColorW(&c)) {
                if (id == 4)
                    m_style.borderColor = c.rgbResult;
                else
                    m_style.cutColor = c.rgbResult;
                SendDlgItemMessageW(m_hwnd, id == 4 ? 5 : 9, CB_SETCURSEL, id == 4 ? 5 : 2, 0);
            }
            return 0;
        }
        if (HIWORD(p) == CBN_SELCHANGE && (id == 5 || id == 9)) {
            int choice = int(SendDlgItemMessageW(m_hwnd, id, CB_GETCURSEL, 0, 0));
            const COLORREF colors[] = {RGB(20, 150, 255), RGB(255, 180, 30), RGB(220, 50, 50), RGB(255, 255, 255),
                                       RGB(0, 0, 0)};
            if (id == 5 && choice < 5)
                m_style.borderColor = colors[choice];
            if (id == 9 && choice < 2)
                m_style.cutColor = choice ? RGB(0, 0, 0) : RGB(190, 30, 30);
            if ((id == 5 && choice == 5) || (id == 9 && choice == 2))
                SendMessageW(m_hwnd, WM_COMMAND, id == 5 ? 4 : 8, 0);
            return 0;
        }
        if (HIWORD(p) == CBN_SELCHANGE && id == 6) {
            const auto style = SendDlgItemMessageW(m_hwnd, 6, CB_GETCURSEL, 0, 0);
            EnableWindow(GetDlgItem(m_hwnd, 10), style != 2);
            if (style == 2)
                SendDlgItemMessageW(m_hwnd, 10, BM_SETCHECK, BST_CHECKED, 0);
            return 0;
        }
        if (id >= 30 && id < 35) {
            wchar_t path[32768]{};
            OPENFILENAMEW f{sizeof(f)};
            f.hwndOwner = m_hwnd;
            f.lpstrFile = path;
            f.nMaxFile = 32768;
            f.lpstrFilter = L"Cursor files (*.cur;*.ani)\0*.cur;*.ani\0\0";
            f.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&f)) {
                HCURSOR cursor = LoadCursorFromFileW(path);
                if (!cursor) {
                    MessageBoxW(m_hwnd, UiText(L"This cursor could not be loaded.", L"无法加载该光标。"), L"Nskry", MB_ICONERROR);
                    return 0;
                }
                DestroyCursor(cursor);
                m_style.cursors[id - 30].file = path;
                SendMessageW(m_cursor[id - 30], CB_SETCURSEL, 6, 0);
                SetWindowTextW(m_files[id - 30], path);
            }
            return 0;
        }
        break;
    }
    }
    return DefWindowProcW(m_hwnd, m, p, l);
}
} // namespace nskry
