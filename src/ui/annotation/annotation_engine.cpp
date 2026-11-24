#include "pch.h"
#include "ui/annotation/annotation_engine.h"
#include <utility>

namespace nskry {

AnnotationEngine AnnotationEngine::Clone() const {
    AnnotationEngine copy;
    copy.m_currentTool = m_currentTool;
    copy.m_style = m_style;
    for (const auto& shape : m_shapes) copy.m_shapes.push_back(shape->Clone());
    const auto cloneHistory = [](const auto& source, auto& destination) {
        for (const auto& change : source)
            destination.push_back({change.index, change.alternate ? change.alternate->Clone() : nullptr});
    };
    cloneHistory(m_undoStack, copy.m_undoStack);
    cloneHistory(m_redoStack, copy.m_redoStack);
    return copy;
}

void AnnotationEngine::OnMouseDown(POINT localPt) {
    if (m_currentTool == ToolType::None) return;

    m_isDrawing = true;
    m_drawingTool = m_currentTool;

    switch (m_drawingTool) {
    case ToolType::Rect:
        m_inProgressShape = std::make_unique<RectShape>(
            localPt, localPt, m_style.color, m_style.strokeWidth, m_style.filled);
        break;
    case ToolType::Ellipse:
        m_inProgressShape = std::make_unique<EllipseShape>(
            localPt, localPt, m_style.color, m_style.strokeWidth, m_style.filled);
        break;
    case ToolType::Arrow:
        m_inProgressShape = std::make_unique<ArrowShape>(
            localPt, localPt, m_style.color, m_style.strokeWidth);
        break;
    case ToolType::Line:
        m_inProgressShape = std::make_unique<LineShape>(localPt, localPt, m_style.color, m_style.strokeWidth);
        break;
    case ToolType::Pen:
        m_inProgressShape = std::make_unique<PenShape>(
            localPt, m_style.color, m_style.strokeWidth);
        break;
    case ToolType::Mosaic:
        m_inProgressShape = std::make_unique<MosaicShape>(
            localPt, localPt, m_style.mosaicBlockSize);
        break;
    default:
        break;
    }
}

void AnnotationEngine::SetTool(ToolType tool) {
    if (tool != m_currentTool) ClearSelection();
    m_currentTool = tool;
}

bool AnnotationEngine::BeginPointerEdit(POINT point, int tolerance, bool drawOver) {
    if (drawOver && m_currentTool != ToolType::None) {
        ClearSelection();
        return false;
    }
    if (m_selected) {
        const auto& shape = *m_shapes[*m_selected];
        const auto handles = shape.Handles();
        for (int i = 0; i < shape.HandleCount(); ++i) {
            if (std::abs(point.x - handles[i].x) <= tolerance && std::abs(point.y - handles[i].y) <= tolerance) {
                m_pointerEdit = PointerEdit{point, i, shape.Clone()};
                return true;
            }
        }
    }
    for (size_t i = m_shapes.size(); i > 0; --i) {
        const auto& shape = *m_shapes[i - 1];
        if (shape.HitTest(point, tolerance)) {
            m_selected = i - 1;
            m_pointerEdit = PointerEdit{point, -1, shape.Clone()};
            return true;
        }
    }
    m_selected.reset();
    return false;
}

void AnnotationEngine::OnMouseMove(POINT localPt) {
    if (m_pointerEdit) {
        const auto& drag = *m_pointerEdit;
        m_shapes[*m_selected]->EditFrom(*drag.before, drag.handle,
                                       {localPt.x - drag.start.x, localPt.y - drag.start.y});
        return;
    }
    if (!m_isDrawing || !m_inProgressShape) return;

    if (m_drawingTool == ToolType::Pen)
        static_cast<PenShape*>(m_inProgressShape.get())->AddPoint(localPt);
    else
        static_cast<BoundedAnnotation*>(m_inProgressShape.get())->endPt = localPt;
}

void AnnotationEngine::OnMouseUp(POINT localPt) {
    if (m_pointerEdit) {
        const auto start = m_pointerEdit->start;
        if (start.x != localPt.x || start.y != localPt.y) {
            OnMouseMove(localPt);
            RecordChange(*m_selected, std::move(m_pointerEdit->before));
        } else {
            m_shapes[*m_selected] = std::move(m_pointerEdit->before);
        }
        m_pointerEdit.reset();
        return;
    }
    if (!m_isDrawing) return;
    OnMouseMove(localPt);
    m_isDrawing = false;
    if (m_inProgressShape) AddShape(std::move(m_inProgressShape));
}

void AnnotationEngine::CancelPointer() {
    if (m_pointerEdit) m_shapes[*m_selected] = std::move(m_pointerEdit->before);
    m_pointerEdit.reset();
    m_inProgressShape.reset();
    m_isDrawing = false;
}

bool AnnotationEngine::ClearSelection() {
    CancelPointer();
    return std::exchange(m_selected, std::nullopt).has_value();
}

std::optional<AnnotationEngine::TextTarget> AnnotationEngine::SelectTextAt(POINT point, int tolerance) {
    for (size_t i = m_shapes.size(); i > 0; --i) {
        const auto* text = dynamic_cast<const TextShape*>(m_shapes[i - 1].get());
        if (!text || !text->HitTest(point, tolerance)) continue;
        m_selected = i - 1;
        return TextTarget{i - 1, text->pt, text->text};
    }
    return std::nullopt;
}

void AnnotationEngine::CommitText(POINT localPt, std::optional<size_t> target, std::wstring text) {
    if (text.empty()) return;
    if (!target) {
        AddShape(std::make_unique<TextShape>(localPt, std::move(text), m_style.color, m_style.fontSize));
        return;
    }
    if (*target >= m_shapes.size()) return;
    auto* shape = dynamic_cast<TextShape*>(m_shapes[*target].get());
    if (!shape || shape->text == text) return;
    auto before = shape->Clone();
    shape->SetText(std::move(text));
    RecordChange(*target, std::move(before));
}

void AnnotationEngine::AddShape(std::unique_ptr<AnnotationShape> shape) {
    const auto index = m_shapes.size();
    m_shapes.push_back(std::move(shape));
    RecordChange(index, nullptr);
}

void AnnotationEngine::RecordChange(size_t index, std::unique_ptr<AnnotationShape> alternate) {
    m_undoStack.push_back({index, std::move(alternate)});
    m_redoStack.clear();
}

bool AnnotationEngine::ApplyHistory(std::vector<Change>& from, std::vector<Change>& to) {
    if (from.empty()) return false;
    ClearSelection();
    auto& change = from.back();
    if (change.index == m_shapes.size()) {
        m_shapes.push_back(std::move(change.alternate));
    } else if (!change.alternate) {
        change.alternate = std::move(m_shapes.back());
        m_shapes.pop_back();
    } else {
        change.alternate.swap(m_shapes[change.index]);
        m_selected = change.index;
    }
    to.push_back(std::move(change));
    from.pop_back();
    return true;
}

bool AnnotationEngine::Undo() { return ApplyHistory(m_undoStack, m_redoStack); }
bool AnnotationEngine::Redo() { return ApplyHistory(m_redoStack, m_undoStack); }

void AnnotationEngine::Clear() {
    ClearSelection();
    m_shapes.clear();
    m_undoStack.clear();
    m_redoStack.clear();
}

int AnnotationEngine::HandleRadius(RECT viewport, int width, int height, UINT dpi) {
    const int radius = ::MulDiv(5, dpi ? dpi : 96, 96);
    const int screenWidth = (std::max)(1L, viewport.right - viewport.left);
    const int screenHeight = (std::max)(1L, viewport.bottom - viewport.top);
    return (std::max)({2, ::MulDiv(radius, width, screenWidth), ::MulDiv(radius, height, screenHeight)});
}

void AnnotationEngine::DrawSelection(Gdiplus::Graphics& graphics, int radius) const {
    if (!m_selected || radius <= 0) return;
    using namespace Gdiplus;
    const auto& shape = *m_shapes[*m_selected];
    const auto bounds = shape.Bounds();
    const auto handles = shape.Handles();
    const REAL width = (std::max)(1.0f, radius * 0.25f);
    const Color outline(255, 0, 89, 164);
    Pen border(outline, width);
    Pen shadow(Color(255, 255, 255, 255), width * 2.5f);
    if (shape.HandleCount() > 2) {
        const auto w = (std::max)(1L, bounds.right - bounds.left);
        const auto h = (std::max)(1L, bounds.bottom - bounds.top);
        graphics.DrawRectangle(&shadow, REAL(bounds.left), REAL(bounds.top), REAL(w), REAL(h));
        border.SetDashStyle(DashStyleDash);
        graphics.DrawRectangle(&border, REAL(bounds.left), REAL(bounds.top), REAL(w), REAL(h));
        border.SetDashStyle(DashStyleSolid);
    }
    const SolidBrush fill(Color(255, 255, 255, 255));
    for (int i = 0; i < shape.HandleCount(); ++i) {
        const RectF handle(REAL(handles[i].x - radius), REAL(handles[i].y - radius), REAL(2 * radius), REAL(2 * radius));
        graphics.FillRectangle(&fill, handle);
        graphics.DrawRectangle(&border, handle);
    }
}

void AnnotationEngine::Draw(Gdiplus::Graphics& g, HDC hdcBase, int baseOffsetX, int baseOffsetY, int baseW, int baseH) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    for (const auto& shape : m_shapes) {
        shape->Draw(g, hdcBase, baseOffsetX, baseOffsetY, baseW, baseH);
    }
    if (m_inProgressShape) {
        m_inProgressShape->Draw(g, hdcBase, baseOffsetX, baseOffsetY, baseW, baseH);
    }
}

HBITMAP AnnotationEngine::BakeToBitmap(HDC hdcBase, RECT baseLocalRect, int outW, int outH) {
    if (!hdcBase || outW <= 0 || outH <= 0) return nullptr;

    HDC hdcScreen = ::GetDC(nullptr);
    HDC hdcMem    = ::CreateCompatibleDC(hdcScreen);
    HBITMAP hbmp  = ::CreateCompatibleBitmap(hdcScreen, outW, outH);
    HGDIOBJ old   = ::SelectObject(hdcMem, hbmp);

    // 1. Copy base image to output bitmap
    ::BitBlt(hdcMem, 0, 0, outW, outH,
             hdcBase, baseLocalRect.left, baseLocalRect.top, SRCCOPY);

    // 2. Render all annotations on top
    {
        Gdiplus::Graphics g(hdcMem);
        Draw(g, hdcBase, baseLocalRect.left, baseLocalRect.top, baseLocalRect.left + outW, baseLocalRect.top + outH);
    }

    ::SelectObject(hdcMem, old);
    ::DeleteDC(hdcMem);
    ::ReleaseDC(nullptr, hdcScreen);

    return hbmp;
}

std::vector<uint32_t> AnnotationEngine::RenderOverlayRgba(HDC hdcBase, int baseOffsetX, int baseOffsetY, int outW, int outH, int selectionRadius) {
    if (IsEmpty() || outW <= 0 || outH <= 0) return {};

    std::vector<uint32_t> pixels(outW * outH, 0); // 0 = fully transparent ARGB

    Gdiplus::Bitmap bmp(outW, outH, outW * 4, PixelFormat32bppARGB, reinterpret_cast<BYTE*>(pixels.data()));
    Gdiplus::Graphics g(&bmp);

    Draw(g, hdcBase, baseOffsetX, baseOffsetY, baseOffsetX + outW, baseOffsetY + outH);

    DrawSelection(g, selectionRadius);
    return pixels;
}

} // namespace nskry
