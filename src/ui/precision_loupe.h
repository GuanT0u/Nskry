#pragma once

#include <windows.h>
#include <string_view>

namespace nskry {

// Draws a pixel-accurate inspection loupe next to an active crop/resize
// pointer. sourceBounds and sourcePoint use the coordinate system of source.
void DrawPrecisionLoupe(HDC target, HDC source, RECT sourceBounds, POINT sourcePoint,
                        POINT cursorPoint, RECT targetBounds,
                        std::wstring_view coordinateText);

} // namespace nskry
