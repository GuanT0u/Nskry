#pragma once

#include <functional>
#include <windows.h>
#include <mutex>
#include <vector>
#include "ui/annotation/annotation_engine.h"
#include "ui/image_action.h"

namespace nskry {

/// Always-on-top Win32 window that displays a static screenshot bitmap.
/// Supports dragging, aspect-ratio locked resizing, right-click context menu,
/// and in-place annotation editing.
class PinWindow {
public:
    using CloseCallback = std::function<void(PinWindow*)>;

    /// Takes ownership of the HBITMAP.
    PinWindow(HBITMAP bitmap, int width, int height, CloseCallback closed = {},
              ImageActions imageActions = {});
    ~PinWindow();

    PinWindow(const PinWindow&)            = delete;
    PinWindow& operator=(const PinWindow&) = delete;

    [[nodiscard]] bool Show();
    // Opens the existing annotation toolbar immediately (used after a long
    // capture has been cropped in its pre-edit view).
    [[nodiscard]] bool ShowInEditMode();
    [[nodiscard]] HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void OnPaint(HWND hwnd);
    void OnLButtonDown(int x, int y);
    void OnMouseMove(int x, int y);
    void OnLButtonUp(int x, int y);
    void ShowContextMenu(int screenX, int screenY);

    void CopyToClipboard();
    void SaveToFile();
    void EnterEditMode();
    void FinishEdit(bool apply);
    void CommitTextEdit();
    void CreateQuickActions(HWND parent);
    void LayoutQuickActions();
    void ShowQuickActions(bool show);
    void RunTextRecognition();

    // Pin Toolbar
    struct PinToolItem {
        RECT            rect{};
        ToolType        tool   = ToolType::None;
        int             action = 0; // 1: Done, 2: Cancel, 3: Undo, 4: Redo, 5: Width, 6: Color
        int             widthVal = 0;
        COLORREF        color  = 0;
        const wchar_t*  label  = nullptr;
        bool            hovered  = false;
        bool            selected = false;
        bool            isSeparator = false;
        bool            isColorChoice = false;
    };

    void BuildPinToolbar(int clientW, int clientH);
    void DrawPinToolbar(HDC hdc, int clientW, int clientH);
    RECT ImageRect() const;
    bool ClientPointToBitmap(int x, int y, POINT& bitmapPoint, bool clampToImage = false) const;

    HWND    m_hwnd{};
    HBITMAP m_bitmap{};
    int     m_width{};
    int     m_height{};

    // Edit mode state
    bool             m_isEditing = false;
    AnnotationEngine m_annotationEngine;
    std::vector<PinToolItem> m_toolbarItems;
    RECT             m_toolbarBounds{};
    bool             m_isDrawing = false;
    HFONT            m_font{};

    // In-place text input
    HWND             m_hTextEdit{};
    HWND             m_quickButtons[4]{};
    bool             m_quickActionsVisible{};
    POINT            m_textEditPos{};
    CloseCallback    m_closed;
    ImageActions     m_imageActions;
    bool             m_destroying = false;

    static constexpr wchar_t kClassName[] = L"NskryPinWindow";
    static inline std::once_flag s_classOnce;
};

} // namespace nskry
