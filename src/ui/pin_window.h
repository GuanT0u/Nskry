#pragma once
#include "ui/ui_preferences.h"
#include "ui/annotation_toolbar_input.h"


#include <functional>
#include <windows.h>
#include <mutex>
#include <vector>
#include "ui/annotation/annotation_engine.h"
#include "ui/image_action.h"
#include "ui/window_visual_style.h"
#include <memory>

namespace nskry {

/// Always-on-top Win32 window that displays a static screenshot bitmap.
/// Supports dragging, aspect-ratio locked resizing, right-click context menu,
/// and in-place annotation editing.
class PinWindow {
public:
    using CloseCallback = std::function<void(PinWindow*)>;

    /// Takes ownership of the HBITMAP.
    PinWindow(HBITMAP bitmap, int width, int height, CloseCallback closed = {},
              ImageActions imageActions = {}, RECT initialScreenRect = {}, ImageEditorCallback editor = {});
    ~PinWindow();

    PinWindow(const PinWindow&)            = delete;
    PinWindow& operator=(const PinWindow&) = delete;

    [[nodiscard]] bool Show();
    // Opens the existing annotation toolbar immediately (used after a long
    // capture has been cropped in its pre-edit view).
    [[nodiscard]] bool ShowInEditMode(bool requirePinDecision = false);
    [[nodiscard]] HWND Hwnd() const { return m_hwnd; }
    bool SetVisualSettings(WindowVisualSettings settings);

private:
    DesktopPalette m_palette = GetDesktopPalette();
    AnnotationToolbarInput m_toolbarInput;
    RECT m_editOriginalBounds{}, m_editExpandedBounds{};
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void OnPaint(HWND hwnd);
    void OnLButtonDown(int x, int y);
    void OnMouseMove(int x, int y);
    void OnLButtonUp(int x, int y);
    void ShowContextMenu(int screenX, int screenY);

    bool CopyToClipboard(HBITMAP bitmap = nullptr);
    bool SaveToFile(HBITMAP bitmap = nullptr);
    HBITMAP BakeEditedBitmap();
    void ExportEditedBitmap(bool save);
    void OpenStructureEditor();
    void EnterEditMode();
    void FinishEdit(bool apply);
    void CommitTextEdit();
    void CreateQuickActions(HWND parent);
    void LayoutQuickActions();
    void ShowQuickActions(bool show);
    void RunQuickImageAction();
    bool IsQuickActionPoint(POINT clientPoint) const;

    // Pin Toolbar

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
    std::vector<AnnotationToolbarItem> m_toolbarItems;
    RECT             m_toolbarBounds{};
    bool             m_isDrawing = false;
    HFONT            m_font{};
    HFONT            m_quickFont{};

    // In-place text input
    HWND             m_hTextEdit{};
    HWND             m_quickButtons[4]{};
    bool             m_quickActionsVisible{};
    bool             m_hasInitialScreenRect{};
    bool             m_requiresPinDecision{};
    POINT            m_textEditPos{};
    CloseCallback    m_closed;
    ImageActions     m_imageActions;
    ImageEditorCallback m_editor;
    std::unique_ptr<WindowVisualStyle> m_visualStyle;
    bool             m_destroying = false;

    static constexpr wchar_t kClassName[] = L"NskryPinWindow";
    static inline std::once_flag s_classOnce;
};

} // namespace nskry
