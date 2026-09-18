#pragma once

#include <functional>
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

    void Show(HWND owner);

private:
    enum class DragHandle { None, Top, Bottom };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);
    void Paint(HWND);
    void LayoutControls();
    RECT ImageRect() const;
    int SourceYFromClientY(int y) const;
    HBITMAP CreateCroppedBitmap() const;
    void ApplyCrop();

    HWND m_hwnd{};
    HWND m_applyButton{};
    HWND m_cancelButton{};
    HBITMAP m_bitmap{};
    int m_width{};
    int m_height{};
    int m_cropTop{};
    int m_cropBottom{};
    DragHandle m_drag = DragHandle::None;
    ApplyCallback m_apply;
    CloseCallback m_closed;

    static constexpr wchar_t kClassName[] = L"NskryLongImageCropWindow";
};

} // namespace nskry
