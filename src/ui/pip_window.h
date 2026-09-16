#pragma once

#include "capture/d3d_device.h"
#include "ui/annotation/annotation_engine.h"
#include <functional>
#include <mutex>
#include <vector>

namespace nskry {

/// Always-on-top Win32 window that renders captured frames via a DXGI SwapChain.
///
/// The SwapChain is created at the captured content's native resolution;
/// DXGI_SCALING_STRETCH lets DWM scale the output to the actual window size,
/// so the PiP can be freely resized without recreating the swap chain.
///
/// Supports dragging, aspect-ratio locked resizing, right-click context menu,
/// and in-place GPU annotation overlay editing.
class PipWindow {
public:
    PipWindow(std::shared_ptr<D3DDevice> device,
              UINT contentWidth, UINT contentHeight,
              std::function<void()> onCloseRequest,
              AnnotationEngine initialEngine = {});
    ~PipWindow();

    PipWindow(const PipWindow&)            = delete;
    PipWindow& operator=(const PipWindow&) = delete;

    void Show();

    /// Blit the cropped texture into the swap-chain back buffer, blend annotations if present, and Present.
    /// Safe to call from any thread (internally mutex-protected).
    void RenderFrame(ID3D11Texture2D* croppedTexture, UINT w, UINT h);

    [[nodiscard]] HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    static LRESULT CALLBACK ToolbarWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleToolbarMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    static LRESULT CALLBACK TextEditSubclassProc(HWND, UINT, WPARAM, LPARAM);

    void CreateSwapChain(UINT w, UINT h);
    void InitD3DOverlayPipeline();
    void UpdateOverlayFromEngine();
    void BlitAndPresent(ID3D11Texture2D* frameTex, UINT w, UINT h);

    void ShowContextMenu(int screenX, int screenY);
    void EnterEditMode();
    void FinishEdit(bool apply);
    void CommitTextEdit();

    void CreateToolbarWindow();
    void BuildToolbar(int clientW, int clientH);
    void DrawToolbar(HDC hdc, int clientW, int clientH);

    void OnLButtonDown(int x, int y);
    void OnMouseMove(int x, int y);
    void OnLButtonUp(int x, int y);

    std::shared_ptr<D3DDevice>      m_device;
    HWND                            m_hwnd{};
    winrt::com_ptr<IDXGISwapChain1> m_swapChain;
    std::mutex                      m_renderMutex;
    UINT                            m_contentW{};
    UINT                            m_contentH{};
    std::function<void()>           m_onClose;

    // GPU annotation overlay resources
    winrt::com_ptr<ID3D11Texture2D>          m_overlayTex;
    winrt::com_ptr<ID3D11ShaderResourceView> m_overlaySRV;
    winrt::com_ptr<ID3D11VertexShader>       m_vs;
    winrt::com_ptr<ID3D11PixelShader>        m_ps;
    winrt::com_ptr<ID3D11BlendState>         m_blendState;
    winrt::com_ptr<ID3D11SamplerState>       m_sampler;
    winrt::com_ptr<ID3D11Texture2D>          m_lastFrameTex;

    // In-place annotation editing state
    bool             m_isEditing = false;
    AnnotationEngine m_annotationEngine;
    AnnotationEngine m_backupEngine;
    bool             m_isDrawing = false;
    HWND             m_hwndToolbar{};
    HWND             m_hTextEdit{};
    POINT            m_textEditPos{};
    HFONT            m_font{};

    struct PipToolItem {
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
    std::vector<PipToolItem> m_toolbarItems;

    static constexpr wchar_t kClassName[] = L"NskryPipWindow";
    static constexpr wchar_t kToolbarClassName[] = L"NskryPipToolbar";
    static inline std::once_flag s_classOnce;
    static inline std::once_flag s_toolbarClassOnce;
};

} // namespace nskry
