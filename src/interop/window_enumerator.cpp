#include "pch.h"
#include "interop/window_enumerator.h"

namespace nskry {

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<WindowInfo> WindowEnumerator::GetAllVisibleWindows() {
    std::vector<WindowInfo> result;
    result.reserve(32);
    ::EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&result));
    return result;
}

// ---------------------------------------------------------------------------
// EnumWindows callback (z-order: foreground → background)
// ---------------------------------------------------------------------------

BOOL CALLBACK WindowEnumerator::EnumProc(HWND hwnd, LPARAM lParam) {
    auto& out = *reinterpret_cast<std::vector<WindowInfo>*>(lParam);

    if (!IsAppWindow(hwnd))
        return TRUE;   // skip, continue enumeration

    WindowInfo wi;
    wi.hwnd   = hwnd;
    wi.title  = GetTitle(hwnd);
    wi.bounds = GetAccurateBounds(hwnd);

    if (wi.width() > 0 && wi.height() > 0)
        out.push_back(std::move(wi));

    return TRUE;
}

// ---------------------------------------------------------------------------
// Filtering — mirrors the heuristics that Shell / Alt-Tab use
// ---------------------------------------------------------------------------

bool WindowEnumerator::IsAppWindow(HWND hwnd) {
    // Must be visible
    if (!::IsWindowVisible(hwnd))
        return false;

    // Skip the desktop shell window
    if (hwnd == ::GetShellWindow())
        return false;

    // Must be a top-level (root-owned) window
    if (::GetAncestor(hwnd, GA_ROOT) != hwnd)
        return false;

    // Skip tool windows (floating palettes, tooltips, etc.)
    const LONG exStyle = ::GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW)
        return false;

    // Skip DWM-cloaked windows (hidden UWP, virtual-desktop windows)
    DWORD cloaked = 0;
    ::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked != 0)
        return false;

    // Must have a non-empty title (filters out nameless helper windows)
    if (::GetWindowTextLengthW(hwnd) == 0)
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// Bounds — prefer DWM extended frame (excludes drop shadow)
// ---------------------------------------------------------------------------

RECT WindowEnumerator::GetAccurateBounds(HWND hwnd) {
    RECT rc{};

    // DWMWA_EXTENDED_FRAME_BOUNDS gives the visible pixel rect, which
    // excludes the invisible DWM-rendered drop shadow around each window.
    HRESULT hr = ::DwmGetWindowAttribute(
        hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc));
    if (SUCCEEDED(hr))
        return rc;

    // Fallback: includes shadow area, but better than nothing.
    ::GetWindowRect(hwnd, &rc);
    return rc;
}

// ---------------------------------------------------------------------------
// Title
// ---------------------------------------------------------------------------

std::wstring WindowEnumerator::GetTitle(HWND hwnd) {
    const int len = ::GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};

    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    ::GetWindowTextW(hwnd, buf.data(), len + 1);
    buf.resize(static_cast<size_t>(len));
    return buf;
}

} // namespace nskry
