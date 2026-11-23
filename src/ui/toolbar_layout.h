#pragma once
#include <algorithm>
#include <vector>
#include <windows.h>
namespace nskry {
// Physical/client coordinates are supplied by the caller. Wrap on small screens
// and use the inside of the selection if neither outside edge has enough room.
inline std::vector<RECT> LayoutSelectionToolbar(const std::vector<int> &widths, RECT selection, RECT viewport,
                                                size_t mainCount, UINT dpi = 96) {
    const auto dip = [dpi](int n) { return MulDiv(n, dpi, 96); };
    const int inset = dip(4), gap = dip(4), row = dip(32);
    std::vector<RECT> result;
    const int available = std::max(1L, viewport.right - viewport.left - 2 * inset);
    int x = 0, y = 0, used = 0;
    for (size_t i = 0; i < widths.size(); ++i) {
        const int width = std::min(available, widths[i]);
        if (i == mainCount || (x && x + width > available)) {
            x = 0;
            y += row + gap;
        }
        result.push_back({x, y, x + width, y + row});
        x += width + gap;
        used = std::max(used, x - gap);
    }
    const int height = result.empty() ? 0 : y + row;
    const int left = std::clamp((selection.left + selection.right - used) / 2, viewport.left + inset,
                                std::max(viewport.left + inset, viewport.right - used - inset));
    int top = selection.bottom + dip(8);
    if (top + height > viewport.bottom - inset)
        top = selection.top - height - dip(8);
    if (top < viewport.top + inset)
        top = std::max(viewport.top + inset, selection.top + dip(8));
    top = std::clamp<LONG>(top, viewport.top + inset, std::max(viewport.top + inset, viewport.bottom - height - inset));
    for (auto &rect : result)
        OffsetRect(&rect, left, top);
    return result;
}
} // namespace nskry
