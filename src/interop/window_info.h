#pragma once

#include <windows.h>
#include <string>

namespace nskry {

/// Cached snapshot of a visible window's handle, title, and screen-space bounds.
/// Designed for fast in-memory hit-testing during screenshot frame selection.
struct WindowInfo {
    HWND  hwnd{};
    std::wstring title;
    RECT  bounds{};     // Screen coordinates (DWM extended frame bounds preferred)

    [[nodiscard]] int width()  const noexcept { return bounds.right  - bounds.left; }
    [[nodiscard]] int height() const noexcept { return bounds.bottom - bounds.top;  }

    /// Pure-memory point-in-rect test for MouseMove hit detection.
    [[nodiscard]] bool contains(POINT pt) const noexcept {
        return pt.x >= bounds.left  && pt.x < bounds.right
            && pt.y >= bounds.top   && pt.y < bounds.bottom;
    }
};

} // namespace nskry
