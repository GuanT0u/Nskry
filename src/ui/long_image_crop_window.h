#pragma once

#include <functional>
#include <mutex>
#include <windows.h>
#include "ui/image_action.h"

namespace nskry {

// A small pre-annotation editor for long captures.  It keeps the original
// bitmap intact until the user confirms the vertical crop.
class LongImageCropWindow {
public:
    using ApplyCallback = std::function<void(HBITMAP, int, int)>;
    using CloseCallback = std::function<void()>;

    // Takes ownership of bitmap.  apply receives ownership of its bitmap.
    LongImageCropWindow(HBITMAP bitmap, int width, int height,
                        ApplyCallback apply, CloseCallback closed,
                        ImageActions imageActions = {});
    ~LongImageCropWindow();

    LongImageCropWindow(const LongImageCropWindow&) = delete;
    LongImageCropWindow& operator=(const LongImageCropWindow&) = delete;

    [[nodiscard]] bool Show(HWND owner);

private:
    enum class DragHandle { None, Top, Bottom, Left, Right, Pan, OcrArea };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);
    void Paint(HWND);
    void LayoutControls();
    RECT ImageRect() const;
    void CenterViewOn(int sourceY);
    void ScrollView(int wheelDelta);
    void ScrollHorizontal(int wheelDelta);
    void ZoomAt(POINT clientPoint, int wheelDelta);
    void SetOcrSelectionMode(bool enabled);
    int SourceYFromClientY(int y) const;
    POINT SourcePointFromClient(POINT point) const;
    HBITMAP CreateCroppedBitmap() const;
    HBITMAP CreateRegionBitmap(RECT source) const;
    void ApplyCrop();
    void RunImageAction(size_t actionIndex);

    HWND m_hwnd{};
    HWND m_applyButton{};
    HWND m_cancelButton{};
    HWND m_zoomInButton{};
    HWND m_zoomOutButton{};
    HWND m_imageActionButton{};
    HBITMAP m_bitmap{};
    int m_width{};
    int m_height{};
    int m_cropTop{};
    int m_cropBottom{};
    int m_cropLeft{};
    int m_cropRight{};
    RECT m_ocrSelection{};
    POINT m_ocrDragStart{};
    bool m_selectingOcr{};
    DragHandle m_drag = DragHandle::None;
    POINT m_dragLastPoint{};
    double m_dragRemainderY{};
    double m_dragRemainderX{};
    double m_zoom = 1.0;
    int m_zoomFocusY{};
    double m_viewTopY{};       // Source-space y at the top of the zoomed viewport.
    double m_viewLeftX{};
    ApplyCallback m_apply;
    CloseCallback m_closed;
    ImageActions m_imageActions;
    bool m_destroying = false;

    static constexpr wchar_t kClassName[] = L"NskryLongImageCropWindow";
    static inline std::once_flag s_classOnce;
};

} // namespace nskry
