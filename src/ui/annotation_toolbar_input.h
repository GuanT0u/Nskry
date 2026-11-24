#pragma once
#include "ui/toolbar_accessibility.h"
#include "ui/annotation_toolbar_items.h"
#include "ui/toolbar_tooltip.h"
#include <windowsx.h>

namespace nskry {
class AnnotationToolbarInput {
public:
    void Update(HWND owner, HWND surface, const std::vector<AnnotationToolbarItem>& controls) {
        m_tooltip.Reset();
        m_targets.clear();
        std::vector<ToolbarAccessibility::Item> items;
        for (const auto& control : controls) {
            if (control.IsSeparator() || ::IsRectEmpty(&control.rect)) continue;
            m_targets.push_back(control.rect);
            std::wstring name;
            if (control.action == AnnotationAction::Width)
                name = UiText(L"Stroke width · ", L"线宽 · ") + std::to_wstring(control.widthVal) + L" px";
            else if (control.action == AnnotationAction::Color)
                name = UiText(L"Color · ", L"颜色 · ") + std::to_wstring(GetRValue(control.color)) + L", " +
                       std::to_wstring(GetGValue(control.color)) + L", " + std::to_wstring(GetBValue(control.color));
            else if (control.label)
                name = control.label;
            if (control.Icon() != EditorIcon::None)
                m_tooltip.Add(surface, surface, control.rect, name.c_str());
            items.push_back({std::move(name), control.rect, control.selected});
        }
        if (m_focus >= static_cast<int>(m_targets.size())) m_focus = -1;
        m_accessibility.Update(owner, surface, std::move(items), UiText(L"Annotation toolbar", L"标注工具栏"),
                               UiText(L"Activate", L"执行"));
        if (m_focus >= 0) m_accessibility.Focus(owner, m_focus + 1);
    }

    void Clear() {
        m_focus = -1;
        m_targets.clear();
        m_tooltip.Reset();
        m_accessibility.Detach();
    }

    bool Press(HWND owner, HWND surface, WPARAM key) {
        if (m_targets.empty() || ::GetKeyState(VK_CONTROL) < 0 || ::GetKeyState(VK_MENU) < 0)
            return false;
        if (key == VK_TAB || (m_focus >= 0 && (key == VK_LEFT || key == VK_RIGHT))) {
            const int step = key == VK_LEFT || (key == VK_TAB && ::GetKeyState(VK_SHIFT) < 0) ? -1 : 1;
            const int count = static_cast<int>(m_targets.size());
            if (m_focus < 0) m_focus = step > 0 ? count - 1 : 0;
            SetFocusIndex(owner, surface, (m_focus + step + count) % count);
            return true;
        }
        if (m_focus >= 0 && (key == VK_SPACE || key == VK_RETURN)) {
            Click(surface, m_focus);
            return true;
        }
        return false;
    }

    LRESULT GetObject(HWND owner, WPARAM wp, LPARAM lp) const {
        return m_accessibility.GetObject(owner, wp, lp);
    }

    void HandleObjectAction(HWND owner, HWND surface, UINT message, WPARAM wp, LPARAM lp) {
        if (!m_accessibility.Current(static_cast<UINT>(wp)) || lp <= 0 || static_cast<size_t>(lp) > m_targets.size())
            return;
        if (message == ToolbarAccessibility::InvokeMessage)
            Click(surface, static_cast<int>(lp - 1));
        else {
            ::SetFocus(owner);
            SetFocusIndex(owner, surface, static_cast<int>(lp - 1));
        }
    }

    void DrawFocus(HDC dc) const {
        if (m_focus < 0) return;
        RECT border = m_targets[m_focus];
        ::InflateRect(&border, -2, -2);
        ::DrawFocusRect(dc, &border);
    }

private:
    void SetFocusIndex(HWND owner, HWND surface, int index) {
        m_focus = index;
        m_accessibility.Focus(owner, index + 1);
        ::InvalidateRect(surface, nullptr, FALSE);
    }

    void Click(HWND surface, int index) {
        const auto &bounds = m_targets[index];
        const int x = (bounds.left + bounds.right) / 2, y = (bounds.top + bounds.bottom) / 2;
        ::SendMessageW(surface, WM_LBUTTONDOWN, 0, MAKELPARAM(x, y));
    }

    ToolbarTooltip m_tooltip;
    ToolbarAccessibility m_accessibility;
    std::vector<RECT> m_targets;
    int m_focus = -1;
};
} // namespace nskry
