#pragma once

#include "ui/annotation/annotation_shape.h"
#include <memory>
#include <optional>
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

    AnnotationEngine Clone() const;
    void RestoreFrom(const AnnotationEngine& other) { *this = other.Clone(); }

    // Tool & Style Configuration
    void     SetTool(ToolType tool);
    ToolType GetTool() const               { return m_currentTool; }
    void     SetColor(COLORREF color)      { m_style.color = color; }
    COLORREF GetColor() const              { return m_style.color; }
    void     SetStrokeWidth(int width)     { m_style.strokeWidth = width; }
    int      GetStrokeWidth() const        { return m_style.strokeWidth; }
    void     SetFilled(bool filled)        { m_style.filled = filled; }
    bool     IsFilled() const              { return m_style.filled; }
    void     SetFontSize(int size)         { m_style.fontSize = size; }
    int      GetFontSize() const           { return m_style.fontSize; }

    static constexpr int MinMosaicBlockSize = 6;
    static constexpr int MaxMosaicBlockSize = 48;
    void SetMosaicBlockSize(int size) { m_style.mosaicBlockSize = std::clamp(size, MinMosaicBlockSize, MaxMosaicBlockSize); }
    int GetMosaicBlockSize() const { return m_style.mosaicBlockSize; }

    const ToolStyle& GetStyle() const      { return m_style; }

    // Mouse Interactions (coordinates in selection/target local space)
    void OnMouseDown(POINT localPt);
    void OnMouseMove(POINT localPt);
    void OnMouseUp(POINT localPt);

    bool BeginPointerEdit(POINT localPt, int tolerance, bool drawOver = false);
    void CancelPointer();
    bool ClearSelection();
    bool HasSelection() const { return m_selected.has_value(); }

    struct TextTarget { size_t index; POINT position; std::wstring text; };
    std::optional<TextTarget> SelectTextAt(POINT localPt, int tolerance);
    void CommitText(POINT localPt, std::optional<size_t> target, std::wstring text);

    void DrawSelection(Gdiplus::Graphics& graphics, int radius) const;
    static int HandleRadius(RECT viewport, int width, int height, UINT dpi);

    // History
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !m_undoStack.empty(); }
    bool CanRedo() const { return !m_redoStack.empty(); }
    void Clear();
    bool IsEmpty() const { return m_shapes.empty() && !m_inProgressShape; }

    // Drawing onto a Graphics context
    void Draw(Gdiplus::Graphics& g, HDC hdcBase, int baseOffsetX, int baseOffsetY, int baseW, int baseH);

    // Bake annotations with a base snapshot into a new HBITMAP
    // baseLocalRect: the rectangle in hdcBase corresponding to the cropped region
    HBITMAP BakeToBitmap(HDC hdcBase, RECT baseLocalRect, int outW, int outH);

    // Renders only the annotations onto an ARGB 32bpp transparent buffer for PiP video overlay
    std::vector<uint32_t> RenderOverlayRgba(HDC hdcBase, int baseOffsetX, int baseOffsetY, int outW, int outH, int selectionRadius = 0);

private:
    ToolType  m_currentTool = ToolType::None;
    ToolStyle m_style{};

    std::vector<std::unique_ptr<AnnotationShape>> m_shapes;
    struct Change {
        size_t index;
        std::unique_ptr<AnnotationShape> alternate;
    };
    void AddShape(std::unique_ptr<AnnotationShape> shape);
    void RecordChange(size_t index, std::unique_ptr<AnnotationShape> alternate);
    bool ApplyHistory(std::vector<Change>& from, std::vector<Change>& to);

    std::vector<Change> m_undoStack, m_redoStack;
    std::unique_ptr<AnnotationShape> m_inProgressShape;
    std::optional<size_t> m_selected;
    struct PointerEdit {
        POINT start;
        int handle;
        std::unique_ptr<AnnotationShape> before;
    };
    std::optional<PointerEdit> m_pointerEdit;
    ToolType m_drawingTool = ToolType::None;

    bool  m_isDrawing = false;
};

} // namespace nskry
