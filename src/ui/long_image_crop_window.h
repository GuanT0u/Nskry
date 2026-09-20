#pragma once

#include <functional>
#include <mutex>
#include <windows.h>

namespace nskry {

// A small pre-annotation editor for long captures.  It keeps the original
// bitmap intact until the user confirms the vertical crop.
class LongImageCropWindow {
public:
    using ApplyCallback = std::function<void(HBITMAP, int, int)>;
    using CloseCallback = std::function<void()>;

    // Takes ownership of bitmap.  apply receives ownership of its bitmap.
    LongImageCropWindow(HBITMAP bitmap, int width, int height,
                        ApplyCallback apply, CloseCallback closed);
    ~LongImageCropWindow();

    LongImageCropWindow(const LongImageCropWindow&) = delete;
    LongImageCropWindow& operator=(const LongImageCropWindow&) = delete;

    [[nodiscard]] bool Show(HWND owner);

private:
    enum class DragHandle { None, Top, Bottom };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);
    void Paint(HWND);
    void LayoutControls();
    RECT ImageRect() const;
    void CenterViewOn(int sourceY);
    void ScrollView(int wheelDelta);
    int SourceYFromClientY(int y) const;
    HBITMAP CreateCroppedBitmap() const;
    void ApplyCrop();

    HWND m_hwnd{};
    HWND m_applyButton{};
    HWND m_cancelButton{};
    HWND m_zoomInButton{};
    HWND m_zoomOutButton{};
    HBITMAP m_bitmap{};
    int m_width{};
    int m_height{};
    int m_cropTop{};
    int m_cropBottom{};
    DragHandle m_drag = DragHandle::None;
    POINT m_dragLastPoint{};
    double m_dragRemainderY{};
    double m_zoom = 1.0;
    int m_zoomFocusY{};
    double m_viewTopY{};       // Source-space y at the top of the zoomed viewport.
    ApplyCallback m_apply;
    CloseCallback m_closed;
    bool m_destroying = false;

    static constexpr wchar_t kClassName[] = L"NskryLongImageCropWindow";
    static inline std::once_flag s_classOnce;
};

} // namespace nskry
