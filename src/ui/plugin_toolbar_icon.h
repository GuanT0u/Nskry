#pragma once
#include "core/plugin_manifest.h"
#include <algorithm>
#include <iterator>

namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

namespace nskry {
inline void DrawPluginToolbarIcon(HDC dc, const RECT& bounds, const PluginToolbarIcon& icon,
                                  UINT dpi, COLORREF color) {
    using namespace Gdiplus;
    const int size = ::MulDiv(16, dpi, 96);
    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.TranslateTransform(static_cast<REAL>(bounds.left + (bounds.right - bounds.left - size) / 2),
                                static_cast<REAL>(bounds.top + (bounds.bottom - bounds.top - size) / 2));
    graphics.ScaleTransform(size / 16.0f, size / 16.0f);
    Pen pen(Color(255, GetRValue(color), GetGValue(color), GetBValue(color)), 1.5f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    PointF points[kMaxPluginIconStrokePoints]{};
    for (const auto& stroke : icon) {
        if (stroke.size() < 2 || stroke.size() > std::size(points)) continue;
        for (size_t i = 0; i < stroke.size(); ++i)
            points[i] = PointF(stroke[i].x, stroke[i].y);
        graphics.DrawLines(&pen, points, static_cast<INT>(stroke.size()));
    }
}
} // namespace nskry
