#pragma once

#include <windows.h>
#include <mutex>
#include <vector>
#include "ui/annotation/annotation_engine.h"

namespace nskry {

/// Always-on-top Win32 window that displays a static screenshot bitmap.
/// Supports dragging, aspect-ratio locked resizing, right-click context menu,
/// and in-place annotation editing.
class PinWindow {
public:
    /// Takes ownership of the HBITMAP.
    PinWindow(HBITMAP bitmap, int width, int height);
    ~PinWindow();

    PinWindow(const PinWindow&)            = delete;
    PinWindow& operator=(const PinWindow&) = delete;

    void Show();
    // Opens the existing annotation toolbar immediately (used after a long
    // capture has been cropped in its pre-edit view).
    void ShowInEditMode();
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
    HFONT            m_fontIcon{};

    // In-place text input
    HWND             m_hTextEdit{};
    POINT            m_textEditPos{};

    static constexpr wchar_t kClassName[] = L"NskryPinWindow";
    static inline std::once_flag s_classOnce;
};

} // namespace nskry
