#pragma once

#include <windows.h>
#include <mutex>

namespace nskry {

/// Always-on-top Win32 window that displays a static screenshot bitmap.
/// Used for the "Pin Screenshot" feature — the user can drag and resize it.
/// Rendering uses GDI StretchBlt (no D3D needed for a static image).
class PinWindow {
public:
    /// Takes ownership of the HBITMAP.
    PinWindow(HBITMAP bitmap, int width, int height);
    ~PinWindow();

    PinWindow(const PinWindow&)            = delete;
    PinWindow& operator=(const PinWindow&) = delete;

    void Show();
    [[nodiscard]] HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint(HWND hwnd);

    HWND    m_hwnd{};
    HBITMAP m_bitmap{};
    int     m_width{};
    int     m_height{};

    static constexpr wchar_t kClassName[] = L"NskryPinWindow";
    static inline std::once_flag s_classOnce;
};

} // namespace nskry
