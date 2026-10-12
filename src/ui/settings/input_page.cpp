#include "ui/settings/input_page.h"
#include "pch.h"
#include <commctrl.h>
#include <windowsx.h>
namespace nskry {
namespace {
std::wstring Text(HWND w) {
    std::wstring s(GetWindowTextLengthW(w) + 1, L'\0');
    GetWindowTextW(w, s.data(), int(s.size()));
    s.pop_back();
    return s;
}
UINT RecordedKey(WPARAM key, LPARAM flags) {
    if (key == VK_CONTROL)
        return flags & (1LL << 24) ? VK_RCONTROL : VK_LCONTROL;
    if (key == VK_MENU)
        return flags & (1LL << 24) ? VK_RMENU : VK_LMENU;
    if (key == VK_SHIFT) {
        const auto physical = MapVirtualKeyW(UINT((flags >> 16) & 255), MAPVK_VSC_TO_VK_EX);
        return physical ? physical : VK_LSHIFT;
    }
    return UINT(key);
}
} // namespace
InputPage::InputPage(HWND parent, std::vector<ShortcutDefinition> definitions, bool jumps,
                     std::function<void(bool)> recording, std::function<void(const std::wstring &)> navigate)
    : m_jumps(jumps), m_recordState(std::move(recording)), m_navigate(std::move(navigate)) {
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSW c{};
        c.lpfnWndProc = Proc;
        c.hInstance = GetModuleHandleW(nullptr);
        c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        c.hbrBackground = HBRUSH(COLOR_WINDOW + 1);
        c.lpszClassName = L"NskryInputPage";
        RegisterClassW(&c);
    });
    m_hwnd = CreateWindowExW(0, L"NskryInputPage", L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 10, 10, parent,
                             nullptr, GetModuleHandleW(nullptr), this);
    auto create = [&](const wchar_t *cls, const std::wstring &text, DWORD style, int id) {
        HWND w = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, m_hwnd,
                                 HMENU(INT_PTR(id)), nullptr, nullptr);
        SendMessageW(w, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        return w;
    };
    int y = 8;
    for (int section = 0; section < 3; ++section) {
        m_headings.push_back({create(L"STATIC",
                                     section == 0   ? L"General / Core"
                                     : section == 1 ? L"Official plugin profiles"
                                                    : L"Third-party plugins",
                                     0, 0),
                              y});
        y += 32;
        std::wstring last;
        for (auto &definition : definitions) {
            const int group = definition.pluginId.empty() ? 0 : definition.official ? 1 : 2;
            if (group != section || (jumps && !definition.generalJump))
                continue;
            if (!definition.pluginId.empty() && last != definition.pluginId) {
                last = definition.pluginId;
                m_headings.push_back({create(L"STATIC", definition.pluginName, 0, 0), y});
                y += 26;
            }
            Row row;
            row.definition = definition;
            row.y = y;
            m_rows.push_back(std::move(row));
            y += 52;
        }
        y += 10;
    }
    m_height = y;
    Layout();
}
InputPage::~InputPage() {
    StopRecording();
    if (m_hwnd)
        DestroyWindow(m_hwnd);
}
void InputPage::CreateRow(size_t i) {
    auto &row = m_rows[i];
    auto create = [&](const wchar_t *cls, const std::wstring &text, DWORD flags, int id) {
        HWND w = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | flags, 0, 0, 10, 10, m_hwnd,
                                 HMENU(INT_PTR(id)), nullptr, nullptr);
        SendMessageW(w, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        return w;
    };
    row.label = create(L"STATIC", row.definition.label, 0, 0);
    if (!m_values.contains(row.definition.id))
        m_values[row.definition.id] = row.definition.defaultShortcut;
    const auto &value = m_values[row.definition.id];
    row.display = create(L"STATIC", value.empty() ? L"Not set" : value, SS_CENTERIMAGE, 0);
    row.record = create(L"BUTTON", m_jumps ? L"Set " + row.definition.label + L" shortcut" : L"Record",
                        BS_PUSHBUTTON | WS_TABSTOP, 10000);
    SetWindowLongPtrW(row.record, GWLP_USERDATA, LONG_PTR(i + 1));
    if (!m_jumps) {
        row.edit = create(L"EDIT", value, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 30000);
        ShowWindow(row.edit, SW_HIDE);
        SetWindowSubclass(row.edit, EditProc, i, DWORD_PTR(this));
        row.type = create(L"BUTTON", L"Type", BS_PUSHBUTTON | WS_TABSTOP, 10001);
        row.remove = create(L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, 10002);
        SetWindowLongPtrW(row.type, GWLP_USERDATA, LONG_PTR(i + 1));
        SetWindowLongPtrW(row.remove, GWLP_USERDATA, LONG_PTR(i + 1));
    }
}
void InputPage::ReleaseRow(size_t i) {
    TypeDone(i);
    auto &r = m_rows[i];
    for (HWND w : {r.label, r.display, r.edit, r.record, r.type, r.remove})
        if (w)
            DestroyWindow(w);
    r.label = r.display = r.edit = r.record = r.type = r.remove = nullptr;
}
void InputPage::Load(const UserSettings &settings) {
    StopRecording();
    m_values = settings.shortcuts;
    for (size_t i = 0; i < m_rows.size(); ++i) {
        auto &row = m_rows[i];
        if (!m_values.contains(row.definition.id))
            m_values[row.definition.id] = row.definition.defaultShortcut;
        const auto &value = m_values[row.definition.id];
        SetWindowTextW(row.display, value.empty() ? L"Not set" : value.c_str());
        SetWindowTextW(row.edit, value.c_str());
        ShowWindow(row.edit, SW_HIDE);
        if (!m_jumps)
            ShowWindow(row.display, SW_SHOW);
    }
}
void InputPage::Store(UserSettings &settings) {
    StopRecording();
    for (size_t i = 0; i < m_rows.size(); ++i)
        TypeDone(i);
    settings.shortcuts = m_values;
    settings.captureShortcut = m_values[L"core.capture"];
    settings.exitShortcut = m_values[L"core.exit"];
}
void InputPage::TypeDone(size_t i) {
    if (i >= m_rows.size())
        return;
    auto &row = m_rows[i];
    if (row.edit && (GetWindowLongPtrW(row.edit, GWL_STYLE) & WS_VISIBLE)) {
        m_values[row.definition.id] = Text(row.edit);
        SetWindowTextW(row.display,
                       m_values[row.definition.id].empty() ? L"Not set" : m_values[row.definition.id].c_str());
        ShowWindow(row.edit, SW_HIDE);
        ShowWindow(row.display, SW_SHOW);
    }
}
void InputPage::StopRecording(bool accept) {
    if (m_recording < 0)
        return;
    const auto i = size_t(m_recording);
    m_recording = -1;
    auto &row = m_rows[i];
    if (accept)
        m_values[row.definition.id] = m_recorder.Text(row.definition.kind == input::Kind::Wheel);
    const auto &value = m_values[row.definition.id];
    SetWindowTextW(row.display, value.empty() ? L"Not set" : value.c_str());
    SetWindowTextW(row.record, L"Record");
    if (m_recordState)
        m_recordState(false);
    m_recorder.Reset();
}
void InputPage::BeginRecord(size_t i) {
    if (m_recording == int(i)) {
        StopRecording();
        return;
    }
    StopRecording();
    for (size_t j = 0; j < m_rows.size(); ++j)
        TypeDone(j);
    m_recording = int(i);
    m_recorder.Reset();
    SetWindowTextW(m_rows[i].record, L"Cancel");
    SetWindowTextW(m_rows[i].display, L"Press keys...");
    if (m_recordState)
        m_recordState(true);
    SetFocus(m_hwnd);
}
void InputPage::Layout() {
    RECT r{};
    GetClientRect(m_hwnd, &r);
    const int w = r.right;
    SCROLLINFO si{
        sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, (std::max)(0, m_height - 1), UINT((std::max)(0L, r.bottom)),
        m_scroll};
    SetScrollInfo(m_hwnd, SB_VERT, &si, TRUE);
    for (auto h : m_headings)
        SetWindowPos(h.hwnd, nullptr, 8, h.y - m_scroll, (std::max)(1, w - 16), 24, SWP_NOZORDER | SWP_NOACTIVATE);
    for (size_t i = 0; i < m_rows.size(); ++i) {
        auto &row = m_rows[i];
        const bool visible = row.y + 52 >= m_scroll && row.y <= m_scroll + r.bottom + 52;
        if (!visible) {
            if (row.label)
                ReleaseRow(i);
            continue;
        }
        if (!row.label)
            CreateRow(i);
        const int y = row.y - m_scroll;
        if (m_jumps) {
            ShowWindow(row.label, SW_HIDE);
            ShowWindow(row.display, SW_HIDE);
            SetWindowPos(row.record, nullptr, 16, y, (std::max)(1, w - 32), 34, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            const int labelW = (std::max)(140, w - 470), valueX = labelW + 14, valueW = 170,
                      buttonsX = valueX + valueW + 6;
            SetWindowPos(row.label, nullptr, 8, y, labelW, 44, SWP_NOZORDER | SWP_NOACTIVATE);
            for (HWND c : {row.display, row.edit})
                SetWindowPos(c, nullptr, valueX, y, valueW, 30, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(row.record, nullptr, buttonsX, y, 96, 30, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(row.type, nullptr, buttonsX + 100, y, 64, 30, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(row.remove, nullptr, buttonsX + 168, y, 66, 30, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}
void InputPage::Scroll(int offset) {
    StopRecording();
    RECT r{};
    GetClientRect(m_hwnd, &r);
    m_scroll = std::clamp(offset, 0, (std::max)(0, m_height - int(r.bottom)));
    Layout();
}
void InputPage::Jump(const std::wstring &id) {
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].definition.id == id) {
            Scroll(m_rows[i].y - 16);
            if (m_flashRow >= 0 && m_rows[m_flashRow].label)
                InvalidateRect(m_rows[m_flashRow].label, nullptr, TRUE);
            m_flashRow = int(i);
            m_flashPhase = 0;
            SetTimer(m_hwnd, 1, 220, nullptr);
            InvalidateRect(m_rows[i].label, nullptr, TRUE);
            return;
        }
}
LRESULT CALLBACK InputPage::Proc(HWND w, UINT m, WPARAM p, LPARAM l) {
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(w, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
    auto self = reinterpret_cast<InputPage *>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (self && m == WM_NCCREATE)
        self->m_hwnd = w;
    if (self && m == WM_NCDESTROY) {
        self->StopRecording();
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        self->m_hwnd = nullptr;
        return DefWindowProcW(w, m, p, l);
    }
    return self ? self->Handle(m, p, l) : DefWindowProcW(w, m, p, l);
}
LRESULT CALLBACK InputPage::EditProc(HWND w, UINT m, WPARAM p, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    auto self = reinterpret_cast<InputPage *>(ref);
    if (m == WM_KEYDOWN && p == VK_RETURN) {
        self->TypeDone(id);
        SetFocus(self->m_hwnd);
        return 0;
    }
    if (m == WM_KILLFOCUS)
        self->TypeDone(id);
    if (m == WM_NCDESTROY)
        RemoveWindowSubclass(w, EditProc, id);
    return DefSubclassProc(w, m, p, l);
}
LRESULT InputPage::Handle(UINT m, WPARAM p, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        Scroll(m_scroll);
        return 0;
    case WM_MOUSEWHEEL:
        if (m_recording >= 0 && m_rows[m_recording].definition.kind == input::Kind::Wheel) {
            StopRecording(true);
            return 0;
        }
        Scroll(m_scroll - GET_WHEEL_DELTA_WPARAM(p) / WHEEL_DELTA * 100);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO s{sizeof(s), SIF_ALL};
        GetScrollInfo(m_hwnd, SB_VERT, &s);
        int pos = m_scroll;
        switch (LOWORD(p)) {
        case SB_LINEUP:
            pos -= 40;
            break;
        case SB_LINEDOWN:
            pos += 40;
            break;
        case SB_PAGEUP:
            pos -= int(s.nPage);
            break;
        case SB_PAGEDOWN:
            pos += int(s.nPage);
            break;
        case SB_THUMBTRACK:
            pos = s.nTrackPos;
            break;
        }
        Scroll(pos);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (m_recording >= 0) {
            m_recorder.Down(RecordedKey(p, l));
            SetWindowTextW(m_rows[m_recording].display,
                           m_recorder.Text(m_rows[m_recording].definition.kind == input::Kind::Wheel).c_str());
            return 0;
        }
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (m_recording >= 0) {
            if (m_recorder.Up(RecordedKey(p, l)))
                StopRecording(true);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        StopRecording();
        break;
    case WM_SHOWWINDOW:
        if (!p)
            StopRecording();
        break;
    case WM_COMMAND: {
        // WM_COMMAND carries only a 16-bit control ID. Store the full row index on
        // the sender instead, so a large plugin catalog cannot alias another row.
        const HWND sender = HWND(l);
        const int id = LOWORD(p), action = id - 10000;
        const LONG_PTR stored = sender ? GetWindowLongPtrW(sender, GWLP_USERDATA) : 0;
        const size_t index = stored > 0 ? size_t(stored - 1) : m_rows.size();
        if (id >= 10000 && id <= 10002 && index < m_rows.size() && HIWORD(p) == BN_CLICKED &&
            GetParent(sender) == m_hwnd &&
            sender == (action == 0   ? m_rows[index].record
                       : action == 1 ? m_rows[index].type
                                     : m_rows[index].remove)) {
            if (m_jumps) {
                if (m_navigate)
                    m_navigate(m_rows[index].definition.id);
                return 0;
            }
            if (action == 0)
                BeginRecord(index);
            else {
                StopRecording();
                auto &row = m_rows[index];
                if (action == 1) {
                    SetWindowTextW(row.edit, m_values[row.definition.id].c_str());
                    ShowWindow(row.display, SW_HIDE);
                    ShowWindow(row.edit, SW_SHOW);
                    SetFocus(row.edit);
                    SendMessageW(row.edit, EM_SETSEL, 0, -1);
                }
                if (action == 2) {
                    ShowWindow(row.edit, SW_HIDE);
                    m_values[row.definition.id].clear();
                    SetWindowTextW(row.display, L"Not set");
                    ShowWindow(row.display, SW_SHOW);
                }
            }
            return 0;
        }
        break;
    }
    case WM_TIMER:
        if (p == 1) {
            const HWND label = m_flashRow >= 0 ? m_rows[m_flashRow].label : nullptr;
            ++m_flashPhase;
            if (m_flashPhase >= 4) {
                KillTimer(m_hwnd, 1);
                m_flashRow = -1;
            }
            if (label)
                InvalidateRect(label, nullptr, TRUE);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        if (m_flashRow >= 0 && (m_flashPhase % 2) == 0 && HWND(l) == m_rows[m_flashRow].label) {
            SetTextColor(HDC(p), RGB(0, 65, 150));
            SetBkColor(HDC(p), RGB(222, 237, 255));
            SetDCBrushColor(HDC(p), RGB(222, 237, 255));
            return LRESULT(GetStockObject(DC_BRUSH));
        }
        break;
    }
    return DefWindowProcW(m_hwnd, m, p, l);
}
} // namespace nskry
