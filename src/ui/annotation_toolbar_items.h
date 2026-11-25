#pragma once
#include "ui/annotation_toolbar_controls.h"
#include "ui/ui_preferences.h"
#include "ui/editor_icons.h"

namespace nskry {
enum class AnnotationAction { SelectTool, Separator, Done, Cancel, Undo, Redo, Width, Color, Copy, Save, ToggleShapes, ShapeMenu, MosaicStrength };

struct AnnotationToolbarItem {
    RECT rect{};
    ToolType tool = ToolType::None;
    AnnotationAction action = AnnotationAction::Separator;
    const wchar_t* label = nullptr;
    int widthVal = 0;
    COLORREF color = 0;
    bool hovered = false;
    bool selected = false;

    bool IsSeparator() const { return action == AnnotationAction::Separator; }
    EditorIcon Icon() const {
        if (action == AnnotationAction::ToggleShapes) return EditorIcon::Shapes;
        if (action == AnnotationAction::Undo) return EditorIcon::Undo;
        if (action == AnnotationAction::Redo) return EditorIcon::Redo;
        return IconForTool(tool);
    }
};

inline std::vector<AnnotationToolbarItem> BuildAnnotationToolbarItems(const AnnotationEngine& engine,
                                                                       bool pinDecision, size_t& mainCount) {
    using Action = AnnotationAction;
    std::vector<AnnotationToolbarItem> items{
        {{}, ToolType::None,    Action::ToggleShapes, UiText(L"Shapes", L"形状")},
        {{}, ToolType::Arrow,   Action::SelectTool, UiText(L"Arrow", L"箭头")},
        {{}, ToolType::Pen,     Action::SelectTool, UiText(L"Pen", L"画笔")},
        {{}, ToolType::Mosaic,  Action::SelectTool, UiText(L"Mosaic", L"马赛克")},
        {{}, ToolType::Text,    Action::SelectTool, UiText(L"Text", L"文本")},
        {},
        {{}, ToolType::None,    Action::Undo, UiText(L"Undo", L"撤销")},
        {{}, ToolType::None,    Action::Redo, UiText(L"Redo", L"重做")},
        {},
        {{}, ToolType::None,    Action::Done, pinDecision ? UiText(L"Pin", L"固定") : UiText(L"Done", L"完成")},
        {{}, ToolType::None,    Action::Cancel, UiText(L"Cancel", L"取消")},
    };
    if (pinDecision) {
        items.push_back({{}, ToolType::None, Action::Copy, UiText(L"Copy", L"复制")});
        items.push_back({{}, ToolType::None, Action::Save, UiText(L"Save", L"保存")});
    }
    for (auto& item : items)
        item.selected = item.action == Action::ToggleShapes ? IsShapeTool(engine.GetTool())
            : item.action == Action::SelectTool && engine.GetTool() == item.tool;

    mainCount = items.size();
    if (engine.GetTool() == ToolType::None) return items;
    if (engine.GetTool() == ToolType::Mosaic) {
        items.push_back({{}, ToolType::None, Action::MosaicStrength, UiText(L"Mosaic strength", L"马赛克强度")});
        return items;
    }

    constexpr int widths[] = {2, 4, 8};
    constexpr const wchar_t* labels[] = {L"2", L"4", L"8"};
    for (size_t i = 0; i < std::size(widths); ++i) {
        AnnotationToolbarItem item{{}, ToolType::None, Action::Width, labels[i], widths[i]};
        item.selected = engine.GetStrokeWidth() == widths[i];
        items.push_back(item);
    }
    if (IsShapeTool(engine.GetTool()))
        items.push_back({{}, engine.GetTool(), Action::ShapeMenu, ChangeShapeLabel(engine.GetTool())});
    constexpr COLORREF colors[] = {RGB(255,59,48), RGB(255,149,0), RGB(255,204,0), RGB(52,199,89),
                                   RGB(0,122,255), RGB(175,82,222), RGB(240,240,245), RGB(30,30,35)};
    for (COLORREF color : colors) {
        AnnotationToolbarItem item{{}, ToolType::None, Action::Color, nullptr, 0, color};
        item.selected = engine.GetColor() == color;
        items.push_back(item);
    }
    return items;
}

inline std::vector<int> MeasureAnnotationToolbarItems(HWND window, HFONT font,
                                                       const std::vector<AnnotationToolbarItem>& items,
                                                       UINT dpi) {
    std::vector<int> widths;
    widths.reserve(items.size());
    HDC dc = ::GetDC(window);
    HGDIOBJ oldFont = dc && font ? ::SelectObject(dc, font) : nullptr;
    for (const auto& item : items) {
        const int minimum = ::MulDiv(item.IsSeparator() ? 4 : 32, dpi, 96);
        SIZE text{};
        if (item.action == AnnotationAction::MosaicStrength)
            widths.push_back(::MulDiv(kMosaicSliderWidth, dpi, 96));
        else if (item.Icon() == EditorIcon::None && dc && item.label &&
                 ::GetTextExtentPoint32W(dc, item.label, static_cast<int>(wcslen(item.label)), &text))
            widths.push_back((std::max)(minimum, static_cast<int>(text.cx) + ::MulDiv(16, dpi, 96)));
        else
            widths.push_back(minimum);
    }
    if (oldFont) ::SelectObject(dc, oldFont);
    if (dc) ::ReleaseDC(window, dc);
    return widths;
}
} // namespace nskry
