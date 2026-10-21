#pragma once
#include <algorithm>
#include <vector>
#include <windows.h>
namespace nskry {
// Physical/client coordinates are supplied by the caller. Wrap on small screens
// and use the inside of the selection if neither outside edge has enough room.
inline std::vector<RECT> LayoutSelectionToolbar(const std::vector<int> &widths, RECT selection, RECT viewport,
                                                size_t mainCount) {
    std::vector<RECT> result;
    const int available = std::max(1L, viewport.right - viewport.left - 8);
    int x = 0, y = 0, used = 0;
    for (size_t i = 0; i < widths.size(); ++i) {
        const int width = std::min(available, widths[i]);
        if (i == mainCount || (x && x + width > available)) {
            x = 0;
            y += 36;
        }
        result.push_back({x, y, x + width, y + 32});
        x += width + 3;
        used = std::max(used, x - 3);
    }
    const int height = result.empty() ? 0 : y + 32;
    const int left = std::clamp((selection.left + selection.right - used) / 2, viewport.left + 4,
                                std::max(viewport.left + 4, viewport.right - used - 4));
    int top = selection.bottom + 8;
    if (top + height > viewport.bottom - 4)
        top = selection.top - height - 8;
    if (top < viewport.top + 4)
        top = std::max(viewport.top + 4, selection.top + 8);
    top = std::clamp<LONG>(top, viewport.top + 4, std::max(viewport.top + 4, viewport.bottom - height - 4));
    for (auto &rect : result)
        OffsetRect(&rect, left, top);
    return result;
}
} // namespace nskry
