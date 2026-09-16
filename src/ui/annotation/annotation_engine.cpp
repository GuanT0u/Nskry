#include "pch.h"
#include "ui/annotation/annotation_engine.h"

namespace nskry {

void AnnotationEngine::OnMouseDown(POINT localPt) {
    if (m_currentTool == ToolType::None) return;

    m_isDrawing = true;
    m_mouseDownPt = localPt;
    m_redoStack.clear();

    switch (m_currentTool) {
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
    case ToolType::Pen:
        m_inProgressShape = std::make_unique<PenShape>(
            localPt, m_style.color, m_style.strokeWidth);
        break;
    case ToolType::Mosaic:
        m_inProgressShape = std::make_unique<MosaicShape>(
            localPt, localPt, 10);
        break;
    default:
        break;
    }
}

void AnnotationEngine::OnMouseMove(POINT localPt) {
    if (!m_isDrawing || !m_inProgressShape) return;

    switch (m_currentTool) {
    case ToolType::Rect:
        static_cast<RectShape*>(m_inProgressShape.get())->endPt = localPt;
        break;
    case ToolType::Ellipse:
        static_cast<EllipseShape*>(m_inProgressShape.get())->endPt = localPt;
        break;
    case ToolType::Arrow:
        static_cast<ArrowShape*>(m_inProgressShape.get())->endPt = localPt;
        break;
    case ToolType::Pen:
        static_cast<PenShape*>(m_inProgressShape.get())->AddPoint(localPt);
        break;
    case ToolType::Mosaic:
        static_cast<MosaicShape*>(m_inProgressShape.get())->endPt = localPt;
        break;
    default:
        break;
    }
}

void AnnotationEngine::OnMouseUp(POINT localPt) {
    if (!m_isDrawing) return;
    m_isDrawing = false;

    if (m_inProgressShape) {
        // Ensure final point is registered
        OnMouseMove(localPt);
        m_shapes.push_back(std::move(m_inProgressShape));
    }
}

void AnnotationEngine::AddTextShape(POINT localPt, const std::wstring& text) {
    if (text.empty()) return;
    m_shapes.push_back(std::make_unique<TextShape>(localPt, text, m_style.color, m_style.fontSize));
    m_redoStack.clear();
}

bool AnnotationEngine::Undo() {
    if (m_shapes.empty()) return false;
    m_redoStack.push_back(std::move(m_shapes.back()));
    m_shapes.pop_back();
    return true;
}

bool AnnotationEngine::Redo() {
    if (m_redoStack.empty()) return false;
    m_shapes.push_back(std::move(m_redoStack.back()));
    m_redoStack.pop_back();
    return true;
}

void AnnotationEngine::Clear() {
    m_shapes.clear();
    m_redoStack.clear();
    m_inProgressShape.reset();
    m_isDrawing = false;
}

void AnnotationEngine::Draw(Gdiplus::Graphics& g, HDC hdcBase, int baseOffsetX, int baseOffsetY, int baseW, int baseH) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

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

} // namespace nskry
