#pragma once

#include <windows.h>
#include <vector>
#include <array>
#include <memory>
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
    Text,
    Line
};

struct ToolStyle {
    COLORREF color       = RGB(255, 59, 48); // Default highlight red
    int      strokeWidth = 3;                // 2, 4, 8
    bool     filled      = false;
    int      fontSize    = 16;
    int      mosaicBlockSize = 12;
};

inline RECT AnnotationBounds(POINT a, POINT b) {
    return {(std::min)(a.x, b.x), (std::min)(a.y, b.y), (std::max)(a.x, b.x), (std::max)(a.y, b.y)};
}

inline bool NearAnnotationSegment(POINT point, POINT a, POINT b, int tolerance) {
    const double x = b.x - a.x, y = b.y - a.y;
    const double length = x * x + y * y;
    const double t = length ? std::clamp(((point.x - a.x) * x + (point.y - a.y) * y) / length, 0.0, 1.0) : 0;
    const double dx = point.x - a.x - t * x, dy = point.y - a.y - t * y;
    return dx * dx + dy * dy <= double(tolerance) * tolerance;
}

class AnnotationShape {
public:
    virtual ~AnnotationShape() = default;
    virtual void Draw(Gdiplus::Graphics& g, HDC hdcBase, int baseOffsetX, int baseOffsetY, int baseW, int baseH) = 0;
    virtual std::unique_ptr<AnnotationShape> Clone() const = 0;
    virtual RECT Bounds() const = 0;
    virtual void SetBoundsFrom(const AnnotationShape& before, RECT bounds) = 0;

    virtual bool HitTest(POINT point, int tolerance) const {
        auto bounds = Bounds();
        ::InflateRect(&bounds, tolerance, tolerance);
        return ::PtInRect(&bounds, point) != FALSE;
    }

    virtual std::array<POINT, 4> Handles() const {
        const auto [left, top, right, bottom] = Bounds();
        return {{{left, top}, {right, top}, {left, bottom}, {right, bottom}}};
    }
    virtual int HandleCount() const { return 4; }

    // Handle -1 moves the entire object. The original geometry is reused for
    // every drag update so rounding never accumulates.
    virtual void EditFrom(const AnnotationShape& before, int handle, POINT delta) {
        auto bounds = before.Bounds();
        if (handle < 0) {
            ::OffsetRect(&bounds, delta.x, delta.y);
        } else {
            if (handle & 1) bounds.right = (std::max)(bounds.left + 4, bounds.right + delta.x);
            else bounds.left = (std::min)(bounds.right - 4, bounds.left + delta.x);
            if (handle & 2) bounds.bottom = (std::max)(bounds.top + 4, bounds.bottom + delta.y);
            else bounds.top = (std::min)(bounds.bottom - 4, bounds.top + delta.y);
        }
        SetBoundsFrom(before, bounds);
    }
};

class BoundedAnnotation : public AnnotationShape {
public:
    BoundedAnnotation(POINT start, POINT end) : startPt(start), endPt(end) {}
    RECT Bounds() const override { return AnnotationBounds(startPt, endPt); }
    void SetBoundsFrom(const AnnotationShape&, RECT bounds) override {
        startPt = {bounds.left, bounds.top};
        endPt = {bounds.right, bounds.bottom};
    }
    POINT startPt{}, endPt{};
};

class SegmentAnnotation : public BoundedAnnotation {
public:
    using BoundedAnnotation::BoundedAnnotation;
    bool HitTest(POINT point, int tolerance) const override {
        return NearAnnotationSegment(point, startPt, endPt, tolerance);
    }
    std::array<POINT, 4> Handles() const override { return {{startPt, endPt}}; }
    int HandleCount() const override { return 2; }
    void EditFrom(const AnnotationShape& before, int handle, POINT delta) override {
        const auto& segment = static_cast<const SegmentAnnotation&>(before);
        startPt = segment.startPt;
        endPt = segment.endPt;
        if (handle != 1) { startPt.x += delta.x; startPt.y += delta.y; }
        if (handle != 0) { endPt.x += delta.x; endPt.y += delta.y; }
    }
};

// ============================================================================
// RectShape
// ============================================================================
class RectShape : public BoundedAnnotation {
public:
    COLORREF  color{};
    int       strokeWidth = 3;
    bool      filled = false;

    RectShape(POINT start, POINT end, COLORREF col, int width, bool isFilled)
        : BoundedAnnotation(start, end), color(col), strokeWidth(width), filled(isFilled) {}

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

    std::unique_ptr<AnnotationShape> Clone() const override {
        return std::make_unique<RectShape>(*this);
    }
};

// ============================================================================
// EllipseShape
// ============================================================================
class EllipseShape : public BoundedAnnotation {
public:
    COLORREF  color{};
    int       strokeWidth = 3;
    bool      filled = false;

    EllipseShape(POINT start, POINT end, COLORREF col, int width, bool isFilled)
        : BoundedAnnotation(start, end), color(col), strokeWidth(width), filled(isFilled) {}

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

    bool HitTest(POINT point, int tolerance) const override {
        const auto bounds = Bounds();
        const double rx = (bounds.right - bounds.left) / 2.0, ry = (bounds.bottom - bounds.top) / 2.0;
        const double x = point.x - bounds.left - rx, y = point.y - bounds.top - ry;
        const double nx = x / (rx + tolerance), ny = y / (ry + tolerance);
        return nx * nx + ny * ny <= 1;
    }

    std::unique_ptr<AnnotationShape> Clone() const override {
        return std::make_unique<EllipseShape>(*this);
    }
};

// ============================================================================
// ArrowShape
// ============================================================================
class ArrowShape : public SegmentAnnotation {
public:
    COLORREF color{};
    int      strokeWidth = 3;

    ArrowShape(POINT start, POINT end, COLORREF col, int width)
        : SegmentAnnotation(start, end), color(col), strokeWidth(width) {}

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

    std::unique_ptr<AnnotationShape> Clone() const override {
        return std::make_unique<ArrowShape>(*this);
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

    RECT Bounds() const override {
        auto bounds = AnnotationBounds(points.front(), points.front());
        for (const auto point : points) {
            bounds.left = (std::min)(bounds.left, point.x);
            bounds.top = (std::min)(bounds.top, point.y);
            bounds.right = (std::max)(bounds.right, point.x);
            bounds.bottom = (std::max)(bounds.bottom, point.y);
        }
        return bounds;
    }
    bool HitTest(POINT point, int tolerance) const override {
        if (!AnnotationShape::HitTest(point, tolerance + strokeWidth / 2)) return false;
        for (size_t i = 0; i < points.size(); ++i)
            if (NearAnnotationSegment(point, points[i ? i - 1 : 0], points[i], tolerance + strokeWidth / 2))
                return true;
        return false;
    }
    void SetBoundsFrom(const AnnotationShape& before, RECT bounds) override {
        const auto& original = static_cast<const PenShape&>(before);
        const auto old = original.Bounds();
        for (size_t i = 0; i < points.size(); ++i) {
            const auto p = original.points[i];
            points[i] = {bounds.left + ::MulDiv(p.x - old.left, bounds.right - bounds.left, (std::max)(1L, old.right - old.left)),
                         bounds.top + ::MulDiv(p.y - old.top, bounds.bottom - bounds.top, (std::max)(1L, old.bottom - old.top))};
        }
    }

    std::unique_ptr<AnnotationShape> Clone() const override {
        return std::make_unique<PenShape>(*this);
    }
};

class LineShape : public SegmentAnnotation {
public:
    LineShape(POINT start, POINT end, COLORREF color, int width)
        : SegmentAnnotation(start, end), m_color(color), m_width(width) {}

    void Draw(Gdiplus::Graphics& graphics, HDC, int, int, int, int) override {
        Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(m_color), GetGValue(m_color), GetBValue(m_color)),
                        static_cast<Gdiplus::REAL>(m_width));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawLine(&pen, static_cast<INT>(startPt.x), static_cast<INT>(startPt.y),
                          static_cast<INT>(endPt.x), static_cast<INT>(endPt.y));
    }
    std::unique_ptr<AnnotationShape> Clone() const override { return std::make_unique<LineShape>(*this); }

private:
    COLORREF m_color{};
    int m_width{};
};

// ============================================================================
// MosaicShape (High performance pixelation)
// ============================================================================
class MosaicShape : public BoundedAnnotation {
public:
    int   blockSize = 10; // Pixel block size

    MosaicShape(POINT start, POINT end, int block = 10)
        : BoundedAnnotation(start, end), blockSize(block) {}

    void Draw(Gdiplus::Graphics& g, HDC hdcBase, int baseOffsetX, int baseOffsetY, int baseW, int baseH) override {
        int x = (std::min)(startPt.x, endPt.x);
        int y = (std::min)(startPt.y, endPt.y);
        int w = std::abs(endPt.x - startPt.x);
        int h = std::abs(endPt.y - startPt.y);
        if (w <= 0 || h <= 0) return;

        int bs = (blockSize < 4) ? 4 : blockSize;

        if (!hdcBase || baseW <= 0 || baseH <= 0) {
            // Fallback when base HDC is not available: draw alternating frosted mosaic tiles
            for (int curY = y; curY < y + h; curY += bs) {
                int curH = (std::min)(bs, (y + h) - curY);
                for (int curX = x; curX < x + w; curX += bs) {
                    int curW = (std::min)(bs, (x + w) - curX);
                    int tile = ((curX / bs) + (curY / bs)) % 3;
                    BYTE val = (tile == 0) ? 170 : ((tile == 1) ? 200 : 225);
                    Gdiplus::SolidBrush brush(Gdiplus::Color(255, val, val, val));
                    g.FillRectangle(&brush, curX, curY, curW, curH);
                }
            }
            return;
        }

        // Pixelate block by block using sampled color from hdcBase
        for (int curY = y; curY < y + h; curY += bs) {
            int curH = (std::min)(bs, (y + h) - curY);
            for (int curX = x; curX < x + w; curX += bs) {
                int curW = (std::min)(bs, (x + w) - curX);
                int sampleX = baseOffsetX + curX + curW / 2;
                int sampleY = baseOffsetY + curY + curH / 2;
                sampleX = std::clamp(sampleX, 0, baseW - 1);
                sampleY = std::clamp(sampleY, 0, baseH - 1);

                COLORREF c = ::GetPixel(hdcBase, sampleX, sampleY);
                Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
                g.FillRectangle(&brush, curX, curY, curW, curH);
            }
        }
    }

    std::unique_ptr<AnnotationShape> Clone() const override {
        return std::make_unique<MosaicShape>(*this);
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

    RECT Bounds() const override {
        if (!m_extent.cx || !m_extent.cy) {
            SIZE extent{fontSize, fontSize + 4};
            if (HDC dc = ::GetDC(nullptr)) {
                {
                    Gdiplus::Graphics graphics(dc);
                    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
                    Gdiplus::Font font(L"Segoe UI", static_cast<Gdiplus::REAL>(fontSize), Gdiplus::FontStyleBold,
                                       Gdiplus::UnitPixel);
                    Gdiplus::RectF ink;
                    if (graphics.MeasureString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0), &ink) == Gdiplus::Ok)
                        extent = {static_cast<LONG>(std::ceil(ink.Width)) + 1, static_cast<LONG>(std::ceil(ink.Height)) + 1};
                }
                ::ReleaseDC(nullptr, dc);
            }
            m_extent = extent;
        }
        return {pt.x, pt.y, pt.x + m_extent.cx, pt.y + m_extent.cy};
    }
    void SetBoundsFrom(const AnnotationShape&, RECT bounds) override { pt = {bounds.left, bounds.top}; }

    void EditFrom(const AnnotationShape& before, int handle, POINT delta) override {
        const auto& original = static_cast<const TextShape&>(before);
        pt = original.pt;
        if (handle < 0) {
            pt.x += delta.x;
            pt.y += delta.y;
            return;
        }
        const auto old = original.Bounds();
        const double horizontal = double(handle & 1 ? delta.x : -delta.x) / (old.right - old.left);
        const double vertical = double(handle & 2 ? delta.y : -delta.y) / (old.bottom - old.top);
        const double adjustment = std::abs(horizontal) > std::abs(vertical) ? horizontal : vertical;
        const int next = int(std::round(std::clamp(original.fontSize * (1.0 + adjustment), 8.0, 256.0)));
        if (fontSize != next) m_extent = {};
        fontSize = next;
        const auto scaled = Bounds();
        if (!(handle & 1)) pt.x = old.right - (scaled.right - scaled.left);
        if (!(handle & 2)) pt.y = old.bottom - (scaled.bottom - scaled.top);
    }
    void SetText(std::wstring value) { text = std::move(value); m_extent = {}; }

    std::unique_ptr<AnnotationShape> Clone() const override {
        return std::make_unique<TextShape>(*this);
    }

private:
    mutable SIZE m_extent{};
};

} // namespace nskry
