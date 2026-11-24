#pragma once
#include <initializer_list>
#include <memory>
#include "ui/annotation/annotation_shape.h"

namespace nskry {
enum class EditorIcon { None, Rectangle, Ellipse, Arrow, Pen, Mosaic, Text, Undo, Redo, Shapes, Line };

inline EditorIcon IconForTool(ToolType tool) {
    switch (tool) {
    case ToolType::Rect: return EditorIcon::Rectangle;
    case ToolType::Ellipse: return EditorIcon::Ellipse;
    case ToolType::Arrow: return EditorIcon::Arrow;
    case ToolType::Pen: return EditorIcon::Pen;
    case ToolType::Mosaic: return EditorIcon::Mosaic;
    case ToolType::Line: return EditorIcon::Line;
    case ToolType::Text: return EditorIcon::Text;
    default: return EditorIcon::None;
    }
}

// Draw the same 16-unit vector geometry at the device size of each toolbar.
inline void DrawEditorIcon(HDC dc, EditorIcon icon, int x, int y, int size, COLORREF color) {
    using namespace Gdiplus;
    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.TranslateTransform(static_cast<REAL>(x), static_cast<REAL>(y));
    const auto scale = static_cast<REAL>(size) / 16.0f;
    graphics.ScaleTransform(scale, scale);
    const Color ink(255, GetRValue(color), GetGValue(color), GetBValue(color));
    Pen stroke(ink, 1.5f);
    stroke.SetStartCap(LineCapRound);
    stroke.SetEndCap(LineCapRound);
    stroke.SetLineJoin(LineJoinRound);
    const auto lines = [&](std::initializer_list<PointF> points) {
        graphics.DrawLines(&stroke, points.begin(), static_cast<INT>(points.size()));
    };

    switch (icon) {
    case EditorIcon::Rectangle:
        graphics.DrawRectangle(&stroke, 2.5f, 3.5f, 11.0f, 9.0f);
        break;
    case EditorIcon::Ellipse:
        graphics.DrawEllipse(&stroke, 2.25f, 3.75f, 11.5f, 8.5f);
        break;
    case EditorIcon::Arrow:
        lines({{3, 13}, {13, 3}});
        lines({{7.5f, 3}, {13, 3}, {13, 8.5f}});
        break;
    case EditorIcon::Pen:
        lines({{2.5f, 13.5f}, {3.5f, 9.5f}, {10.5f, 2.5f}, {13.5f, 5.5f},
               {6.5f, 12.5f}, {2.5f, 13.5f}});
        lines({{8.75f, 4.25f}, {11.75f, 7.25f}});
        break;
    case EditorIcon::Mosaic: {
        const SolidBrush fill(ink);
        graphics.FillRectangle(&fill, 2, 2, 5, 5);
        graphics.DrawRectangle(&stroke, 9.75f, 2.75f, 3.5f, 3.5f);
        graphics.DrawRectangle(&stroke, 2.75f, 9.75f, 3.5f, 3.5f);
        graphics.FillRectangle(&fill, 9, 9, 5, 5);
        break;
    }
    case EditorIcon::Text:
        lines({{3, 5}, {3, 3}, {13, 3}, {13, 5}});
        lines({{8, 3}, {8, 13}});
        lines({{5.5f, 13}, {10.5f, 13}});
        break;
    case EditorIcon::Undo:
    case EditorIcon::Redo:
        if (icon == EditorIcon::Redo) {
            graphics.TranslateTransform(16, 0);
            graphics.ScaleTransform(-1, 1);
        }
        lines({{6, 3}, {2.5f, 6.5f}, {6, 10}});
        graphics.DrawBezier(&stroke, PointF(2.5f, 6.5f), PointF(8, 6.5f), PointF(13, 5.5f), PointF(13, 12.5f));
        break;
    case EditorIcon::Shapes:
        lines({{5, 10}, {2, 10}, {2, 2}, {10, 2}, {10, 5}});
        graphics.DrawEllipse(&stroke, 7.5f, 7.5f, 6.5f, 6.5f);
        break;
    case EditorIcon::Line:
        lines({{3, 13}, {13, 3}});
        break;
    case EditorIcon::None:
        break;
    }
}

inline void DrawCenteredEditorIcon(HDC dc, const RECT& bounds, EditorIcon icon, UINT dpi, COLORREF color) {
    const int size = ::MulDiv(16, dpi, 96);
    DrawEditorIcon(dc, icon, bounds.left + (bounds.right - bounds.left - size) / 2,
                   bounds.top + (bounds.bottom - bounds.top - size) / 2, size, color);
}
} // namespace nskry
