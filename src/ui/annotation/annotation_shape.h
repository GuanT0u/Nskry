#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

namespace nskry {

enum class ToolType {
    None,
    Rect,
    Ellipse,
    Arrow,
    Pen,
    Mosaic,
    Text
};

struct ToolStyle {
    COLORREF color       = RGB(255, 59, 48); // Default highlight red
    int      strokeWidth = 3;                // 2, 4, 8
    bool     filled      = false;
    int      fontSize    = 16;
};

/// Base class for all vector annotation shapes
class AnnotationShape {
public:
    virtual ~AnnotationShape() = default;
    virtual void Draw(Gdiplus::Graphics& g, HDC hdcBase, int baseOffsetX, int baseOffsetY, int baseW, int baseH) = 0;
};

// ============================================================================
// RectShape
// ============================================================================
class RectShape : public AnnotationShape {
public:
    POINT     startPt{};
    POINT     endPt{};
    COLORREF  color{};
    int       strokeWidth = 3;
    bool      filled = false;

    RectShape(POINT start, POINT end, COLORREF col, int width, bool isFilled)
        : startPt(start), endPt(end), color(col), strokeWidth(width), filled(isFilled) {}

    void Draw(Gdiplus::Graphics& g, HDC /*hdcBase*/, int /*baseOffsetX*/, int /*baseOffsetY*/, int /*baseW*/, int /*baseH*/) override {
        int x = (std::min)(startPt.x, endPt.x);
        int y = (std::min)(startPt.y, endPt.y);
        int w = std::abs(endPt.x - startPt.x);
        int h = std::abs(endPt.y - startPt.y);
        if (w <= 0 || h <= 0) return;

        Gdiplus::Color gdiColor(255, GetRValue(color), GetGValue(color), GetBValue(color));
        if (filled) {
            Gdiplus::SolidBrush brush(gdiColor);
            g.FillRectangle(&brush, x, y, w, h);
        } else {
            Gdiplus::Pen pen(gdiColor, static_cast<Gdiplus::REAL>(strokeWidth));
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            g.DrawRectangle(&pen, x, y, w, h);
        }
    }
};

// ============================================================================
// EllipseShape
// ============================================================================
class EllipseShape : public AnnotationShape {
public:
    POINT     startPt{};
    POINT     endPt{};
    COLORREF  color{};
    int       strokeWidth = 3;
    bool      filled = false;

    EllipseShape(POINT start, POINT end, COLORREF col, int width, bool isFilled)
        : startPt(start), endPt(end), color(col), strokeWidth(width), filled(isFilled) {}

    void Draw(Gdiplus::Graphics& g, HDC /*hdcBase*/, int /*baseOffsetX*/, int /*baseOffsetY*/, int /*baseW*/, int /*baseH*/) override {
        int x = (std::min)(startPt.x, endPt.x);
        int y = (std::min)(startPt.y, endPt.y);
        int w = std::abs(endPt.x - startPt.x);
        int h = std::abs(endPt.y - startPt.y);
        if (w <= 0 || h <= 0) return;

        Gdiplus::Color gdiColor(255, GetRValue(color), GetGValue(color), GetBValue(color));
        if (filled) {
            Gdiplus::SolidBrush brush(gdiColor);
            g.FillEllipse(&brush, x, y, w, h);
        } else {
            Gdiplus::Pen pen(gdiColor, static_cast<Gdiplus::REAL>(strokeWidth));
            g.DrawEllipse(&pen, x, y, w, h);
        }
    }
};

// ============================================================================
// ArrowShape
// ============================================================================
class ArrowShape : public AnnotationShape {
public:
    POINT    startPt{};
    POINT    endPt{};
    COLORREF color{};
    int      strokeWidth = 3;

    ArrowShape(POINT start, POINT end, COLORREF col, int width)
        : startPt(start), endPt(end), color(col), strokeWidth(width) {}

    void Draw(Gdiplus::Graphics& g, HDC /*hdcBase*/, int /*baseOffsetX*/, int /*baseOffsetY*/, int /*baseW*/, int /*baseH*/) override {
        double dx = static_cast<double>(endPt.x - startPt.x);
        double dy = static_cast<double>(endPt.y - startPt.y);
        double len = std::sqrt(dx * dx + dy * dy);
        if (len < 4.0) return;

        Gdiplus::Color gdiColor(255, GetRValue(color), GetGValue(color), GetBValue(color));
        Gdiplus::Pen pen(gdiColor, static_cast<Gdiplus::REAL>(strokeWidth));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);

        // Arrow head dimensions
        double headLen = (std::max)(12.0, static_cast<double>(strokeWidth) * 4.5);
        if (headLen > len * 0.7) headLen = len * 0.7;
        double angle = std::atan2(dy, dx);
        constexpr double kHeadAngle = 0.45; // ~26 degrees

        // Arrow head triangle base points
        double x1 = endPt.x - headLen * std::cos(angle - kHeadAngle);
        double y1 = endPt.y - headLen * std::sin(angle - kHeadAngle);
        double x2 = endPt.x - headLen * std::cos(angle + kHeadAngle);
        double y2 = endPt.y - headLen * std::sin(angle + kHeadAngle);

        // Draw main line slightly shortened to fit head
        double shaftEndRatio = (len - headLen * 0.6) / len;
        int shaftEndX = static_cast<int>(startPt.x + dx * shaftEndRatio);
        int shaftEndY = static_cast<int>(startPt.y + dy * shaftEndRatio);
        g.DrawLine(&pen, startPt.x, startPt.y, shaftEndX, shaftEndY);

        // Draw filled arrow head
        Gdiplus::PointF arrowPoints[3] = {
            { static_cast<Gdiplus::REAL>(endPt.x), static_cast<Gdiplus::REAL>(endPt.y) },
            { static_cast<Gdiplus::REAL>(x1), static_cast<Gdiplus::REAL>(y1) },
            { static_cast<Gdiplus::REAL>(x2), static_cast<Gdiplus::REAL>(y2) }
        };
        Gdiplus::SolidBrush brush(gdiColor);
        g.FillPolygon(&brush, arrowPoints, 3);
    }
};

// ============================================================================
// PenShape (Smooth freehand curve)
// ============================================================================
class PenShape : public AnnotationShape {
public:
    std::vector<POINT> points;
    COLORREF           color{};
    int                strokeWidth = 3;

    PenShape(POINT firstPt, COLORREF col, int width)
        : color(col), strokeWidth(width) {
        points.push_back(firstPt);
    }

    void AddPoint(POINT pt) {
        points.push_back(pt);
    }

    void Draw(Gdiplus::Graphics& g, HDC /*hdcBase*/, int /*baseOffsetX*/, int /*baseOffsetY*/, int /*baseW*/, int /*baseH*/) override {
        if (points.empty()) return;

        Gdiplus::Color gdiColor(255, GetRValue(color), GetGValue(color), GetBValue(color));
        Gdiplus::Pen pen(gdiColor, static_cast<Gdiplus::REAL>(strokeWidth));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);

        if (points.size() == 1) {
            Gdiplus::SolidBrush brush(gdiColor);
            float r = static_cast<float>(strokeWidth) / 2.0f;
            g.FillEllipse(&brush, points[0].x - r, points[0].y - r, r * 2, r * 2);
            return;
        }

        std::vector<Gdiplus::Point> gdiPoints;
        gdiPoints.reserve(points.size());
        for (const auto& pt : points) {
            gdiPoints.emplace_back(pt.x, pt.y);
        }

        g.DrawLines(&pen, gdiPoints.data(), static_cast<INT>(gdiPoints.size()));
    }
};

// ============================================================================
// MosaicShape (High performance pixelation)
// ============================================================================
class MosaicShape : public AnnotationShape {
public:
    POINT startPt{};
    POINT endPt{};
    int   blockSize = 10; // Pixel block size

    MosaicShape(POINT start, POINT end, int block = 10)
        : startPt(start), endPt(end), blockSize(block) {}

    void Draw(Gdiplus::Graphics& g, HDC hdcBase, int baseOffsetX, int baseOffsetY, int baseW, int baseH) override {
        if (!hdcBase || baseW <= 0 || baseH <= 0) return;

        int x = (std::min)(startPt.x, endPt.x);
        int y = (std::min)(startPt.y, endPt.y);
        int w = std::abs(endPt.x - startPt.x);
        int h = std::abs(endPt.y - startPt.y);
        if (w <= 0 || h <= 0) return;

        int bs = (blockSize < 4) ? 4 : blockSize;

        // Pixelate block by block using sampled color from hdcBase
        for (int curY = y; curY < y + h; curY += bs) {
            int curH = (std::min)(bs, (y + h) - curY);
            for (int curX = x; curX < x + w; curX += bs) {
                int curW = (std::min)(bs, (x + w) - curX);
                int sampleX = baseOffsetX + curX + curW / 2;
                int sampleY = baseOffsetY + curY + curH / 2;
                if (sampleX >= baseW) sampleX = baseW - 1;
                if (sampleY >= baseH) sampleY = baseH - 1;

                COLORREF c = ::GetPixel(hdcBase, sampleX, sampleY);
                Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
                g.FillRectangle(&brush, curX, curY, curW, curH);
            }
        }
    }
};

// ============================================================================
// TextShape
// ============================================================================
class TextShape : public AnnotationShape {
public:
    POINT        pt{};
    std::wstring text;
    COLORREF     color{};
    int          fontSize = 16;

    TextShape(POINT p, std::wstring txt, COLORREF col, int size)
        : pt(p), text(std::move(txt)), color(col), fontSize(size) {}

    void Draw(Gdiplus::Graphics& g, HDC /*hdcBase*/, int /*baseOffsetX*/, int /*baseOffsetY*/, int /*baseW*/, int /*baseH*/) override {
        if (text.empty()) return;

        Gdiplus::Font font(L"Segoe UI", static_cast<Gdiplus::REAL>(fontSize), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));

        // Subtle shadow for legibility
        Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(160, 0, 0, 0));
        Gdiplus::PointF shadowPt(static_cast<Gdiplus::REAL>(pt.x + 1), static_cast<Gdiplus::REAL>(pt.y + 1));
        g.DrawString(text.c_str(), -1, &font, shadowPt, &shadowBrush);

        Gdiplus::PointF origin(static_cast<Gdiplus::REAL>(pt.x), static_cast<Gdiplus::REAL>(pt.y));
        g.DrawString(text.c_str(), -1, &font, origin, &brush);
    }
};

} // namespace nskry
