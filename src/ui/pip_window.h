#pragma once

#include "capture/d3d_device.h"
#include <functional>
#include <mutex>

namespace nskry {

/// Always-on-top Win32 window that renders captured frames via a DXGI SwapChain.
///
/// The SwapChain is created at the captured content's native resolution;
/// DXGI_SCALING_STRETCH lets DWM scale the output to the actual window size,
/// so the PiP can be freely resized without recreating the swap chain.
///
/// The whole client area is draggable (WM_NCHITTEST → HTCAPTION).
class PipWindow {
public:
    /// @param device         Shared D3D11 device (same one used by CaptureSession).
    /// @param contentWidth   Native width  of the captured crop region.
    /// @param contentHeight  Native height of the captured crop region.
    /// @param onCloseRequest Called (on the main thread, via PostMessage) when
    ///                       the user clicks the X button. The caller is
    ///                       responsible for tearing down capture + PipWindow.
    PipWindow(std::shared_ptr<D3DDevice> device,
              UINT contentWidth, UINT contentHeight,
              std::function<void()> onCloseRequest,
              const std::vector<uint32_t>& overlayPixels = {});
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
    void CreateSwapChain(UINT w, UINT h);
    void InitOverlayResources(const std::vector<uint32_t>& overlayPixels);

    std::shared_ptr<D3DDevice>      m_device;
    HWND                            m_hwnd{};
    winrt::com_ptr<IDXGISwapChain1> m_swapChain;
    std::mutex                      m_renderMutex;
    UINT                            m_contentW{};
    UINT                            m_contentH{};
    std::function<void()>           m_onClose;

    // Optional GPU annotation overlay
    winrt::com_ptr<ID3D11Texture2D>          m_overlayTex;
    winrt::com_ptr<ID3D11ShaderResourceView> m_overlaySRV;
    winrt::com_ptr<ID3D11VertexShader>       m_vs;
    winrt::com_ptr<ID3D11PixelShader>        m_ps;
    winrt::com_ptr<ID3D11BlendState>         m_blendState;
    winrt::com_ptr<ID3D11SamplerState>       m_sampler;

    static constexpr wchar_t kClassName[] = L"NskryPipWindow";
    static inline std::once_flag s_classOnce;
};

} // namespace nskry
