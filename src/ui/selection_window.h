#pragma once

#include "interop/window_enumerator.h"
#include "capture/capture_session.h"
#include "ui/annotation/annotation_engine.h"
#include <functional>
#include <vector>
#include <mutex>

namespace nskry {

// ============================================================================
// Selection actions available after the user finishes drawing a region
// ============================================================================

enum class SelectionAction {
    Copy,       // Copy screenshot to clipboard
    Save,       // Save screenshot to file
    Pin,        // Pin static screenshot on top of desktop
    PiP,        // Start WGC live capture PiP
    Cancel      // User cancelled (ESC / right-click)
};

/// Result data produced by SelectionWindow after region selection.
struct SelectionResult {
    HWND                  targetHwnd{};    // The window under the selection start point
    CropRegion            crop{};          // Crop region relative to targetHwnd (for PiP)
    HBITMAP               bitmap{};        // Static screenshot of selected area. Caller takes ownership.
    int                   bitmapWidth{};
    int                   bitmapHeight{};
    std::vector<uint32_t> overlayPixels;   // Transparent ARGB annotation overlay for PiP
};

// ============================================================================
// SelectionWindow — full-screen overlay with smart snapping + annotation toolbar
// ============================================================================

class SelectionWindow {
public:
    using CompletionCallback = std::function<void(SelectionAction action, SelectionResult result)>;

    explicit SelectionWindow(CompletionCallback onComplete);
    ~SelectionWindow();

    SelectionWindow(const SelectionWindow&)            = delete;
    SelectionWindow& operator=(const SelectionWindow&) = delete;

    void Show();
    void Close();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void TakeSnapshot();
    void CleanupGdi();

    void OnMouseMove(int x, int y);
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnPaint(HWND hwnd);

    // Toolbar item representation
    enum class ToolbarItemType {
        Action,        // Copy, Save, Pin, PiP
        ToolToggle,    // Rect, Ellipse, Arrow, Pen, Mosaic, Text
        Undo,
        Redo,
        WidthChoice,   // Stroke width
        ColorChoice    // Palette color
    };

    struct ToolbarItem {
        RECT            rect{};
        ToolbarItemType type = ToolbarItemType::Action;
        SelectionAction action = SelectionAction::Cancel;
        ToolType        tool   = ToolType::None;
        COLORREF        color  = 0;
        int             widthVal = 0;
        const wchar_t*  label = nullptr;
        bool            hovered  = false;
        bool            selected = false;
        bool            isSeparator = false;
    };

    void BuildToolbar(RECT selectionRect);
    void DrawToolbar(HDC hdc);
    void DrawInfoBox(HDC hdc, RECT selectionRect);
    HBITMAP CaptureSelectedRegion(RECT localRect);
    void FinishWithAction(SelectionAction action);
    void CommitTextEdit();

    // ---- Members ----
    HWND               m_hwnd{};
    CompletionCallback m_onComplete;

    std::vector<WindowInfo> m_windows;

    // GDI resources
    HDC     m_hdcSnapshot{};
    HBITMAP m_bmpSnapshot{};
    HDC     m_hdcBlack{};
    HBITMAP m_bmpBlack{};
    HFONT   m_font{};
    HFONT   m_fontIcon{};

    // Virtual screen metrics (spans all monitors)
    int m_vX = 0, m_vY = 0, m_vW = 0, m_vH = 0;

    // State machine
    enum class State { Hovering, Dragging, Selected, Adjusting, Annotating };
    State m_state = State::Hovering;

    enum class HitZone { None, Inside, Top, Bottom, Left, Right, TopLeft, TopRight, BottomLeft, BottomRight, Toolbar };
    HitZone HitTest(int x, int y);
    void UpdateCursor(HitZone zone);

    // Hover / drag state
    HWND  m_targetHwnd{};
    RECT  m_targetBounds{};
    RECT  m_hoveredRect{};     // local coords
    POINT m_dragStart{};
    RECT  m_dragRect{};        // local coords

    // Adjusting state (modifying selected rect)
    HitZone m_activeZone = HitZone::None;
    POINT   m_adjustStartPt{};
    RECT    m_adjustStartRect{};

    // Selected state
    RECT                     m_finalRect{};       // confirmed selection, local coords
    std::vector<ToolbarItem> m_toolbarItems;
    HBITMAP                  m_capturedBitmap{};

    // Annotation Engine & In-place Text Editing
    AnnotationEngine m_annotationEngine;
    HWND             m_hTextEdit{};
    POINT            m_textEditPos{};

    static constexpr wchar_t kClassName[] = L"NskrySelectionWindow";
    static inline std::once_flag s_classOnce;
};

} // namespace nskry
