#pragma once
#include <windows.h>
#include <array>
#include "core/window_visual_settings.h"

namespace nskry {
// Geometry-only optional decoration. Never reads screenshot/capture pixels.
class WindowVisualStyle {
public:
    explicit WindowVisualStyle(HWND owner) : m_owner(owner) {}
    ~WindowVisualStyle();
    WindowVisualStyle(const WindowVisualStyle&) = delete;
    WindowVisualStyle& operator=(const WindowVisualStyle&) = delete;
    bool SetSettings(WindowVisualSettings settings);
    bool Sync();
    void BeginSizeMove() { m_interactive = true; }
    void EndSizeMove() { m_interactive = false; Sync(); }
    size_t ShadowBytes() const { return m_bytes; }
    unsigned GenerationCount() const { return m_generations; }
private:
    struct Tile { HWND hwnd{}; HDC dc{}; HBITMAP bitmap{}; HGDIOBJ original{}; SIZE size{}; POINT offset{}; };
    void ClearShadow();
    bool Generate(int width, int height, unsigned dpi, int radius);
    static LRESULT CALLBACK ShadowProc(HWND, UINT, WPARAM, LPARAM);
    HWND m_owner{};
    WindowVisualSettings m_settings;
    std::array<Tile, 8> m_tiles{};
    SIZE m_geometry{}, m_cornerGeometry{};
    unsigned m_dpi{}, m_cornerDpi{}, m_generations{};
    int m_radius{-1};
    size_t m_bytes{};
    bool m_interactive{}, m_syncing{}, m_failed{}, m_dirty = true, m_cornerDirty = true;
};
}
