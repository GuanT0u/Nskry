#pragma once

#include "ui/annotation/annotation_shape.h"
#include <memory>
#include <vector>

namespace nskry {

class AnnotationEngine {
public:
    AnnotationEngine() = default;
    ~AnnotationEngine() = default;

    AnnotationEngine(AnnotationEngine&&) noexcept            = default;
    AnnotationEngine& operator=(AnnotationEngine&&) noexcept = default;
    AnnotationEngine(const AnnotationEngine&)                = delete;
    AnnotationEngine& operator=(const AnnotationEngine&)     = delete;

    AnnotationEngine Clone() const {
        AnnotationEngine copy;
        copy.m_currentTool = m_currentTool;
        copy.m_style       = m_style;
        for (const auto& s : m_shapes) {
            if (s) copy.m_shapes.push_back(s->Clone());
        }
        return copy;
    }

    void RestoreFrom(const AnnotationEngine& other) {
        m_currentTool = other.m_currentTool;
        m_style       = other.m_style;
        m_shapes.clear();
        m_redoStack.clear();
        m_inProgressShape.reset();
        for (const auto& s : other.m_shapes) {
            if (s) m_shapes.push_back(s->Clone());
        }
    }

    // Tool & Style Configuration
    void     SetTool(ToolType tool)        { m_currentTool = tool; }
    ToolType GetTool() const               { return m_currentTool; }
    void     SetColor(COLORREF color)      { m_style.color = color; }
    COLORREF GetColor() const              { return m_style.color; }
    void     SetStrokeWidth(int width)     { m_style.strokeWidth = width; }
    int      GetStrokeWidth() const        { return m_style.strokeWidth; }
    void     SetFilled(bool filled)        { m_style.filled = filled; }
    bool     IsFilled() const              { return m_style.filled; }
    void     SetFontSize(int size)         { m_style.fontSize = size; }
    int      GetFontSize() const           { return m_style.fontSize; }

    const ToolStyle& GetStyle() const      { return m_style; }

    // Mouse Interactions (coordinates in selection/target local space)
    void OnMouseDown(POINT localPt);
    void OnMouseMove(POINT localPt);
    void OnMouseUp(POINT localPt);

    // Text annotation commit
    void AddTextShape(POINT localPt, const std::wstring& text);

    // History
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !m_shapes.empty(); }
    bool CanRedo() const { return !m_redoStack.empty(); }
    void Clear();
    bool IsEmpty() const { return m_shapes.empty() && !m_inProgressShape; }

    // Drawing onto a Graphics context
    void Draw(Gdiplus::Graphics& g, HDC hdcBase, int baseOffsetX, int baseOffsetY, int baseW, int baseH);

    // Bake annotations with a base snapshot into a new HBITMAP
    // baseLocalRect: the rectangle in hdcBase corresponding to the cropped region
    HBITMAP BakeToBitmap(HDC hdcBase, RECT baseLocalRect, int outW, int outH);

    // Renders only the annotations onto an ARGB 32bpp transparent buffer for PiP video overlay
    std::vector<uint32_t> RenderOverlayRgba(HDC hdcBase, int baseOffsetX, int baseOffsetY, int outW, int outH);

private:
    ToolType  m_currentTool = ToolType::None;
    ToolStyle m_style{};

    std::vector<std::unique_ptr<AnnotationShape>> m_shapes;
    std::vector<std::unique_ptr<AnnotationShape>> m_redoStack;
    std::unique_ptr<AnnotationShape>              m_inProgressShape;

    POINT m_mouseDownPt{};
    bool  m_isDrawing = false;
};

} // namespace nskry
