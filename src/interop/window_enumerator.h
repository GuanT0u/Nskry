#pragma once

#include "interop/window_info.h"
#include <vector>

namespace nskry {

/// One-shot enumerator that snapshots every visible application window
/// via EnumWindows + DwmGetWindowAttribute at the moment of invocation.
/// The result is an immutable vector suitable for pure-memory hit-testing.
class WindowEnumerator {
public:
    /// Enumerate all visible, non-cloaked, non-tool application windows.
    /// Returned list is ordered by z-order (foreground first).
    static std::vector<WindowInfo> GetAllVisibleWindows();

private:
    static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lParam);
    static bool IsAppWindow(HWND hwnd);
    static RECT GetAccurateBounds(HWND hwnd);
    static std::wstring GetTitle(HWND hwnd);
};

} // namespace nskry
