#pragma once
#include "core/settings_manager.h"
namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <commctrl.h>
#include <gdiplus.h>
namespace nskry {
inline bool Shortcut(const wchar_t *id, UINT key, input::Kind kind = input::Kind::Key) {
    struct Cached {
        std::wstring text;
        input::Chord chord;
        input::Kind kind = input::Kind::Key;
        bool valid{};
    };
    static std::unordered_map<std::wstring, Cached> cache;
    auto [it, created] = cache.try_emplace(id);
    auto &compiled = it->second;
    const auto &text = SettingsManager::Instance().GetHotkey(id);
    if (created || compiled.text != text || compiled.kind != kind) {
        compiled.text = text;
        compiled.kind = kind;
        compiled.valid = input::Parse(text, kind, compiled.chord);
    }
    return compiled.valid && input::Matches(compiled.chord, key, kind);
}
enum class ButtonKeyContext {
    Annotation,
    ImageEditor
};
inline LRESULT CALLBACK CoreKeyRelay(HWND w, UINT m, WPARAM p, LPARAM l, UINT_PTR id, DWORD_PTR parent) {
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        const bool editor = id == UINT_PTR(ButtonKeyContext::ImageEditor);
        for (const auto *shortcutId :
             editor ? std::initializer_list<const wchar_t *>{L"core.editor.cancel", L"core.editor.confirm"}
                    : std::initializer_list<const wchar_t *>{L"core.cancel", L"core.undo", L"core.redo"})
            if (Shortcut(shortcutId, UINT(p))) {
                SendMessageW(HWND(parent), m, p, l);
                return 0;
            }
    }
    if (m == WM_NCDESTROY)
        RemoveWindowSubclass(w, CoreKeyRelay, id);
    return DefSubclassProc(w, m, p, l);
}
inline void RelayCoreButtons(HWND parent, ButtonKeyContext context = ButtonKeyContext::Annotation) {
    struct Context {
        HWND parent;
        ButtonKeyContext keys;
    } relay{parent, context};
    EnumChildWindows(
        parent,
        [](HWND w, LPARAM data) -> BOOL {
            const auto &relay = *reinterpret_cast<const Context *>(data);
            wchar_t type[24]{};
            GetClassNameW(w, type, 24);
            if (wcscmp(type, L"Button") == 0 || wcscmp(type, L"BUTTON") == 0)
                SetWindowSubclass(w, CoreKeyRelay, UINT_PTR(relay.keys), DWORD_PTR(relay.parent));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&relay));
}
inline LRESULT CALLBACK TextKeyRelay(HWND w, UINT m, WPARAM p, LPARAM l, UINT_PTR id, DWORD_PTR parent) {
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        if (Shortcut(L"core.text.accept", UINT(p)) || Shortcut(L"core.text.cancel", UINT(p))) {
            const bool accept = Shortcut(L"core.text.accept", UINT(p));
            // Clear first for cancellation; the parent's existing focus-loss
            // commit path must not accidentally add canceled text.
            if (!accept)
                SetWindowTextW(w, L"");
            SetFocus(HWND(parent));
            return 0;
        }
    }
    if (m == WM_NCDESTROY)
        RemoveWindowSubclass(w, TextKeyRelay, id);
    return DefSubclassProc(w, m, p, l);
}
inline HCURSOR InteractionCursor(size_t index) {
    struct Cache {
        std::array<CursorChoice, 5> choices{};
        std::array<HCURSOR, 5> cursors{};
        ~Cache() {
            for (auto c : cursors)
                if (c)
                    DestroyCursor(c);
        }
    };
    static Cache cache;
    const auto &choices = SettingsManager::Instance().Interaction().cursors;
    if (index >= choices.size())
        return LoadCursorW(nullptr, IDC_ARROW);
    const auto &choice = choices[index];
    if (cache.choices[index] != choice) {
        if (cache.cursors[index])
            DestroyCursor(cache.cursors[index]);
        cache.cursors[index] = nullptr;
        cache.choices[index] = choice;
        if (choice.style == CursorStyle::Custom && !choice.file.empty())
            cache.cursors[index] = LoadCursorFromFileW(choice.file.c_str());
    }
    if (cache.cursors[index])
        return cache.cursors[index];
    const wchar_t *system[] = {IDC_ARROW, IDC_CROSS, IDC_HAND, IDC_SIZEALL, IDC_SIZEWE, IDC_SIZENS, IDC_ARROW};
    return LoadCursorW(nullptr, system[int(choice.style)]);
}
inline Gdiplus::Color VisualColor(COLORREF c, int opacity) {
    return {BYTE(opacity * 255 / 100), GetRValue(c), GetGValue(c), GetBValue(c)};
}
inline void SelectionBorder(HDC dc, RECT rect) {
    const auto &s = SettingsManager::Instance().Interaction();
    Gdiplus::Graphics g(dc);
    Gdiplus::Pen pen(VisualColor(s.borderColor, s.borderOpacity), Gdiplus::REAL(s.borderWidth));
    pen.SetDashStyle(s.dashed ? Gdiplus::DashStyleDash : Gdiplus::DashStyleSolid);
    pen.SetAlignment(Gdiplus::PenAlignmentInset);
    g.DrawRectangle(&pen, Gdiplus::Rect(int(rect.left), int(rect.top), int((std::max)(0L, rect.right - rect.left - 1)),
                                        int((std::max)(0L, rect.bottom - rect.top - 1))));
}
inline void CutPreview(HDC dc, RECT strip, bool columns) {
    const auto &s = SettingsManager::Instance().Interaction();
    Gdiplus::Graphics g(dc);
    const int left = int(strip.left), top = int(strip.top), right = int(strip.right), bottom = int(strip.bottom);
    const Gdiplus::Rect rectangle(left, top, right - left, bottom - top);
    if (s.cutStyle < 2) {
        Gdiplus::SolidBrush b(VisualColor(s.cutColor, s.cutOpacity));
        g.FillRectangle(&b, rectangle);
    }
    RECT clip{};
    GetClipBox(dc, &clip);
    RECT visible{};
    IntersectRect(&visible, &strip, &clip);
    if (s.cutStyle == 1) {
        Gdiplus::Pen grid(VisualColor(s.cutColor, (std::max)(s.cutOpacity, 50)), 1);
        const auto state = g.Save();
        g.SetClip(Gdiplus::Rect(strip.left, strip.top, strip.right - strip.left, strip.bottom - strip.top));
        for (int x = visible.left; x < visible.right; x += 12)
            g.DrawLine(&grid, x, int(visible.top), x, int(visible.bottom));
        for (int y = visible.top; y < visible.bottom; y += 12)
            g.DrawLine(&grid, int(visible.left), y, int(visible.right), y);
        g.Restore(state); // Grid clipping must not suppress the right/bottom cut edge.
    }
    if (s.cutStyle == 3) {
        Gdiplus::HatchBrush b(Gdiplus::HatchStyleDiagonalCross, VisualColor(s.cutColor, 100),
                              Gdiplus::Color(0, 0, 0, 0));
        g.FillRectangle(&b, rectangle);
    }
    if (s.cutEdges || s.cutStyle == 2 || s.cutStyle == 3) {
        Gdiplus::Pen p(VisualColor(s.borderColor, s.borderOpacity), Gdiplus::REAL(s.borderWidth));
        p.SetDashStyle(s.dashed ? Gdiplus::DashStyleDash : Gdiplus::DashStyleSolid);
        if (columns) {
            g.DrawLine(&p, left, top, left, bottom);
            g.DrawLine(&p, right, top, right, bottom);
        } else {
            g.DrawLine(&p, left, top, right, top);
            g.DrawLine(&p, left, bottom, right, bottom);
        }
    }
}
} // namespace nskry
