#pragma once
#include <algorithm>
#include <windows.h>

namespace nskry {
inline bool LayoutFloatingActions(HWND parent, const HWND (&buttons)[4], bool hasImageActions) {
    const UINT dpi = ::GetDpiForWindow(parent);
    const auto dip = [dpi](int n) { return ::MulDiv(n, dpi ? dpi : 96, 96); };
    const int size = dip(32), gap = dip(4), margin = dip(8);
    RECT client{};
    ::GetClientRect(parent, &client);
    const int count = hasImageActions ? 4 : 3;
    const int width = count * size + (count - 1) * gap;
    if (client.right < width + 2 * margin || client.bottom < size + 2 * margin)
        return false;
    int x = client.right - margin - width;
    for (size_t index = 0; index < 4; ++index) {
        if (index == 1 && !hasImageActions)
            continue;
        if (buttons[index])
            ::SetWindowPos(buttons[index], HWND_TOP, x, margin, size, size, SWP_NOACTIVATE);
        x += size + gap;
    }
    return true;
}

inline bool IsFloatingContextKey(WPARAM key) {
    return key == VK_APPS || (key == VK_F10 && ::GetKeyState(VK_SHIFT) < 0);
}
} // namespace nskry
