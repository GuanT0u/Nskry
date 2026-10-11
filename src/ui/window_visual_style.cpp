#include "ui/window_visual_style.h"
#include <dwmapi.h>
#include <cmath>
#include <mutex>

namespace nskry {
namespace {
std::once_flag shadowClass;
size_t totalShadowBytes = 0; // UI thread only, bounded across all windows.
constexpr size_t MaxShadowBytes = 32ULL * 1024 * 1024;
}
WindowVisualStyle::~WindowVisualStyle() { ClearShadow(); }
void WindowVisualStyle::ClearShadow() {
    for (auto& tile : m_tiles) {
        if (tile.hwnd) DestroyWindow(tile.hwnd);
        if (tile.dc) { SelectObject(tile.dc, tile.original); DeleteDC(tile.dc); }
        if (tile.bitmap) DeleteObject(tile.bitmap);
        tile = {};
    }
    totalShadowBytes -= m_bytes; m_bytes = 0; m_geometry = {}; m_dirty = true;
}
LRESULT CALLBACK WindowVisualStyle::ShadowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, message, wp, lp);
}
bool WindowVisualStyle::SetSettings(WindowVisualSettings settings) {
    settings = NormalizeVisualSettings(settings);
    if (m_failed) { m_failed = false; m_dirty = true; }
    if (!(m_settings == settings)) {
        m_settings = settings; m_dirty = m_cornerDirty = true;
    }
    return Sync();
}
bool WindowVisualStyle::Generate(int width, int height, unsigned dpi, int radius) {
    ClearShadow();
    const int pad = (std::max)(1, MulDiv(m_settings.shadowRange + m_settings.shadowOffset, dpi, 96));
    const int offset = MulDiv(m_settings.shadowOffset, dpi, 96);
    const double sigma = m_settings.shadowBlur * dpi / 96.0;
    const POINT positions[] = {{-pad,-pad},{0,-pad},{width,-pad},{width,0},
                               {width,height},{0,height},{-pad,height},{-pad,0}};
    const SIZE sizes[] = {{pad,pad},{width,pad},{pad,pad},{pad,height},
                          {pad,pad},{width,pad},{pad,pad},{pad,height}};
    size_t bytes = 0;
    for (auto size : sizes) {
        const auto count = static_cast<unsigned long long>(size.cx) * size.cy * 4ULL;
        if (count > MaxShadowBytes || bytes > MaxShadowBytes - count) return false;
        bytes += static_cast<size_t>(count);
    }
    if (bytes > MaxShadowBytes - totalShadowBytes) return false;
    std::call_once(shadowClass, [] {
        WNDCLASSW wc{}; wc.lpfnWndProc = ShadowProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"NskryGeometryShadow"; RegisterClassW(&wc);
    });
    for (size_t index = 0; index < m_tiles.size(); ++index) {
        auto& tile = m_tiles[index]; tile.size = sizes[index]; tile.offset = positions[index];
        tile.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
            L"NskryGeometryShadow", L"", WS_POPUP, 0, 0, tile.size.cx, tile.size.cy, nullptr,
            nullptr, GetModuleHandleW(nullptr), nullptr);
        tile.dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = tile.size.cx; info.bmiHeader.biHeight = -tile.size.cy;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* data{}; tile.bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &data, nullptr, 0);
        if (!tile.hwnd || !tile.dc || !tile.bitmap || !data) { ClearShadow(); return false; }
        tile.original = SelectObject(tile.dc, tile.bitmap);
        if (!tile.original || tile.original == HGDI_ERROR) { ClearShadow(); return false; }
        auto pixels = static_cast<unsigned long*>(data);
        for (int y = 0; y < tile.size.cy; ++y) for (int x = 0; x < tile.size.cx; ++x) {
            const double px = tile.offset.x + x + 0.5, py = tile.offset.y + y + 0.5 - offset;
            const double qx = std::abs(px - width / 2.0) - (width / 2.0 - radius);
            const double qy = std::abs(py - height / 2.0) - (height / 2.0 - radius);
            const double distance = std::hypot((std::max)(qx, 0.0), (std::max)(qy, 0.0)) +
                (std::min)((std::max)(qx, qy), 0.0) - radius;
            const double alpha = sigma > 0.0 ? 0.5 * std::erfc(distance / (sigma * std::sqrt(2.0))) : (distance <= 0 ? 1.0 : 0.0);
            const double fade = (std::clamp)((pad - (std::max)(distance, 0.0)) / (std::max)(1.0, sigma), 0.0, 1.0);
            const auto a = static_cast<unsigned long>(std::lround(255.0 * m_settings.shadowOpacity / 100.0 * alpha * fade));
            pixels[y * tile.size.cx + x] = a << 24; // Black, premultiplied BGRA.
        }
        POINT origin{}; BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(tile.hwnd, nullptr, nullptr, &tile.size, tile.dc, &origin, 0, &blend, ULW_ALPHA)) {
            ClearShadow(); return false;
        }
    }
    m_bytes = bytes; totalShadowBytes += bytes; m_geometry = {width,height}; m_dpi = dpi; m_radius = radius;
    m_dirty = false; ++m_generations; return true;
}
bool WindowVisualStyle::Sync() {
    if (m_syncing || !IsWindow(m_owner)) return true;
    m_syncing = true;
    struct Guard { bool& value; ~Guard() { value = false; } } guard{m_syncing};
    RECT rectangle{}; if (!GetWindowRect(m_owner, &rectangle)) return false;
    const int width = rectangle.right - rectangle.left, height = rectangle.bottom - rectangle.top;
    if (width <= 0 || height <= 0) return false;
    const unsigned dpi = GetDpiForWindow(m_owner);
    const int radius = WindowCornerRadius(m_settings, width, height, dpi);
    if (m_cornerDirty || m_cornerGeometry.cx != width || m_cornerGeometry.cy != height || m_cornerDpi != dpi) {
        bool regionRequired = m_settings.cornersEnabled && m_settings.cornerMode != CornerMode::System;
        DWM_WINDOW_CORNER_PREFERENCE preference = m_settings.cornersEnabled && !regionRequired ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
        if (!regionRequired) SetWindowRgn(m_owner, nullptr, TRUE);
        const HRESULT native = DwmSetWindowAttribute(m_owner, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
        if (FAILED(native) && m_settings.cornersEnabled) regionRequired = true;
        if (regionRequired) {
            HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2);
            if (!region || !SetWindowRgn(m_owner, region, TRUE)) { if (region) DeleteObject(region); return false; }
        }
        m_cornerDirty = false; m_cornerGeometry = {width,height}; m_cornerDpi = dpi;
    }
    const bool enabled = m_settings.shadowEnabled && m_settings.shadowOpacity > 0 && m_settings.shadowRange > 0;
    const bool visible = IsWindowVisible(m_owner) && !IsIconic(m_owner);
    if (!enabled) { if (m_bytes) ClearShadow(); return true; }
    if (!visible || (m_interactive && (m_geometry.cx != width || m_geometry.cy != height))) {
        for (auto& tile : m_tiles) if (tile.hwnd) ShowWindow(tile.hwnd, SW_HIDE);
        return true;
    }
    if (m_failed && !m_dirty && m_geometry.cx == width && m_geometry.cy == height && m_dpi == dpi && m_radius == radius) return false;
    if (m_dirty || m_geometry.cx != width || m_geometry.cy != height || m_dpi != dpi || m_radius != radius)
        if (!Generate(width, height, dpi, radius)) {
            m_failed = true; m_dirty = false; m_geometry = {width,height}; m_dpi = dpi; m_radius = radius;
            OutputDebugStringW(L"Nskry: optional shadow unavailable (resource or API failure).\n"); return false;
        } else m_failed = false;
    HWND after = m_owner;
    HDWP batch = BeginDeferWindowPos(static_cast<int>(m_tiles.size()));
    for (const auto& tile : m_tiles) {
        if (!batch) break;
        batch = DeferWindowPos(batch, tile.hwnd, after, rectangle.left + tile.offset.x, rectangle.top + tile.offset.y,
                               tile.size.cx, tile.size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        after = tile.hwnd;
    }
    if (!batch || !EndDeferWindowPos(batch)) {
        // A failed batch must not leave stale shadow pieces at old coordinates.
        for (auto& tile : m_tiles) if (tile.hwnd) ShowWindow(tile.hwnd, SW_HIDE);
        return false;
    }
    return true;
}
}
