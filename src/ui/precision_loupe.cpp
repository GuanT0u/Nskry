#include "pch.h"
#include "ui/precision_loupe.h"

#include <algorithm>

namespace nskry {

void DrawPrecisionLoupe(HDC target, HDC source, RECT sourceBounds, POINT sourcePoint,
                        POINT cursorPoint, RECT targetBounds,
                        std::wstring_view coordinateText) {
    if (!target || !source || sourceBounds.right <= sourceBounds.left || sourceBounds.bottom <= sourceBounds.top) return;

    constexpr int kPixels = 13;
    constexpr int kPixelSize = 10;
    constexpr int kImageSize = kPixels * kPixelSize;
    constexpr int kPadding = 6;
    constexpr int kTextHeight = 36;
    constexpr int kPanelWidth = kImageSize + kPadding * 2;
    constexpr int kPanelHeight = kImageSize + kPadding * 2 + kTextHeight;

    sourcePoint.x = (std::clamp)(sourcePoint.x, sourceBounds.left, sourceBounds.right - 1);
    sourcePoint.y = (std::clamp)(sourcePoint.y, sourceBounds.top, sourceBounds.bottom - 1);
    int left = cursorPoint.x + 20;
    int top = cursorPoint.y + 20;
    if (left + kPanelWidth > targetBounds.right) left = cursorPoint.x - kPanelWidth - 20;
    if (top + kPanelHeight > targetBounds.bottom) top = cursorPoint.y - kPanelHeight - 20;
    const int targetLeft = static_cast<int>(targetBounds.left);
    const int targetTop = static_cast<int>(targetBounds.top);
    const int targetRight = static_cast<int>(targetBounds.right);
    const int targetBottom = static_cast<int>(targetBounds.bottom);
    left = (std::clamp)(left, targetLeft + 4, (std::max)(targetLeft + 4, targetRight - kPanelWidth - 4));
    top = (std::clamp)(top, targetTop + 4, (std::max)(targetTop + 4, targetBottom - kPanelHeight - 4));

    RECT panel{ left, top, left + kPanelWidth, top + kPanelHeight };
    HBRUSH panelBrush = ::CreateSolidBrush(RGB(26, 27, 33));
    if (panelBrush) { ::FillRect(target, &panel, panelBrush); ::DeleteObject(panelBrush); }
    HPEN border = ::CreatePen(PS_SOLID, 1, RGB(72, 76, 88));
    HGDIOBJ oldPen = border ? ::SelectObject(target, border) : nullptr;
    HGDIOBJ oldBrush = ::SelectObject(target, ::GetStockObject(HOLLOW_BRUSH));
    ::Rectangle(target, panel.left, panel.top, panel.right, panel.bottom);
    if (oldBrush) ::SelectObject(target, oldBrush);
    if (oldPen && oldPen != HGDI_ERROR) ::SelectObject(target, oldPen);
    if (border) ::DeleteObject(border);

    const int half = kPixels / 2;
    const int sourceLeft = (std::clamp)(sourcePoint.x - half, sourceBounds.left, (std::max)(sourceBounds.left, sourceBounds.right - kPixels));
    const int sourceTop = (std::clamp)(sourcePoint.y - half, sourceBounds.top, (std::max)(sourceBounds.top, sourceBounds.bottom - kPixels));
    const int imageLeft = left + kPadding;
    const int imageTop = top + kPadding;
    ::SetStretchBltMode(target, COLORONCOLOR);
    ::StretchBlt(target, imageLeft, imageTop, kImageSize, kImageSize, source,
                 sourceLeft, sourceTop, kPixels, kPixels, SRCCOPY);

    HPEN grid = ::CreatePen(PS_SOLID, 1, RGB(80, 84, 96));
    oldPen = grid ? ::SelectObject(target, grid) : nullptr;
    for (int index = 0; index <= kPixels; ++index) {
        const int offset = index * kPixelSize;
        ::MoveToEx(target, imageLeft + offset, imageTop, nullptr);
        ::LineTo(target, imageLeft + offset, imageTop + kImageSize);
        ::MoveToEx(target, imageLeft, imageTop + offset, nullptr);
        ::LineTo(target, imageLeft + kImageSize, imageTop + offset);
    }
    if (oldPen && oldPen != HGDI_ERROR) ::SelectObject(target, oldPen);
    if (grid) ::DeleteObject(grid);

    HPEN crosshair = ::CreatePen(PS_SOLID, 2, RGB(0, 174, 255));
    oldPen = crosshair ? ::SelectObject(target, crosshair) : nullptr;
    const int center = kImageSize / 2;
    ::MoveToEx(target, imageLeft + center, imageTop, nullptr);
    ::LineTo(target, imageLeft + center, imageTop + kImageSize);
    ::MoveToEx(target, imageLeft, imageTop + center, nullptr);
    ::LineTo(target, imageLeft + kImageSize, imageTop + center);
    if (oldPen && oldPen != HGDI_ERROR) ::SelectObject(target, oldPen);
    if (crosshair) ::DeleteObject(crosshair);

    const COLORREF color = ::GetPixel(source, sourcePoint.x, sourcePoint.y);
    wchar_t colorText[32]{};
    if (color != CLR_INVALID) {
        swprintf_s(colorText, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    } else {
        wcscpy_s(colorText, L"#------");
    }
    RECT text{ left + kPadding, imageTop + kImageSize + 3, panel.right - kPadding, panel.bottom - 2 };
    ::SetBkMode(target, TRANSPARENT);
    ::SetTextColor(target, RGB(238, 238, 242));
    std::wstring firstLine(coordinateText);
    ::DrawTextW(target, firstLine.c_str(), static_cast<int>(firstLine.size()), &text, DT_LEFT | DT_TOP | DT_SINGLELINE);
    text.top += 17;
    ::SetTextColor(target, RGB(160, 205, 255));
    ::DrawTextW(target, colorText, -1, &text, DT_LEFT | DT_TOP | DT_SINGLELINE);
}

} // namespace nskry
