#include "pch.h"
#include "ui/pip_window.h"
#include <d3dcompiler.h>

namespace nskry {

static const char kPipShaderSource[] = R"(
struct VS_OUT {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VS_OUT vs_main(uint id : SV_VertexID) {
    VS_OUT output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
Texture2D g_tex : register(t0);
SamplerState g_samp : register(s0);
float4 ps_main(VS_OUT input) : SV_Target {
    return g_tex.Sample(g_samp, input.uv);
}
)";

// Custom message posted to MainWindow when the user closes the PiP.
// This avoids re-entrance issues (the PiP's WndProc returns before cleanup).
static constexpr UINT WM_PIP_CLOSE_REQUEST = WM_APP + 100;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PipWindow::PipWindow(
    std::shared_ptr<D3DDevice> device,
    UINT contentWidth, UINT contentHeight,
    std::function<void()> onCloseRequest,
    const std::vector<uint32_t>& overlayPixels)
    : m_device(std::move(device))
    , m_contentW(contentWidth)
    , m_contentH(contentHeight)
    , m_onClose(std::move(onCloseRequest))
{
    // --- Register window class (once) --------------------------------------
    std::call_once(s_classOnce, [&] {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance      = ::GetModuleHandleW(nullptr);
        wc.lpszClassName  = kClassName;
        wc.hbrBackground  = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
        wc.hCursor        = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        ::RegisterClassExW(&wc);
    });

    // --- Calculate initial window size (≈ 400px wide, aspect-preserving) ----
    constexpr int kDefaultWidth = 400;
    const float aspect = static_cast<float>(contentWidth) / static_cast<float>(contentHeight);
    int winW = kDefaultWidth;
    int winH = static_cast<int>(winW / aspect);
    if (winH < 100) {
        winH = 100;
        winW = static_cast<int>(winH * aspect);
    }

    const DWORD style   = WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_TOPMOST;

    RECT rc = { 0, 0, winW, winH };
    ::AdjustWindowRectEx(&rc, style, FALSE, exStyle);

    // --- Create the window -------------------------------------------------
    m_hwnd = ::CreateWindowExW(
        exStyle, kClassName, L"Nskry PiP",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr,
        ::GetModuleHandleW(nullptr),
        this);    // pass 'this' via CREATESTRUCT

    // --- Create DXGI swap chain --------------------------------------------
    CreateSwapChain(contentWidth, contentHeight);

    // --- Initialize optional annotation overlay ----------------------------
    InitOverlayResources(overlayPixels);
}

PipWindow::~PipWindow() {
    // Release swap chain under lock so an in-flight RenderFrame sees nullptr
    {
        std::lock_guard lk(m_renderMutex);
        m_swapChain = nullptr;
        m_overlaySRV = nullptr;
        m_overlayTex = nullptr;
        m_vs = nullptr;
        m_ps = nullptr;
        m_blendState = nullptr;
        m_sampler = nullptr;
    }
    if (m_hwnd) {
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Show
// ---------------------------------------------------------------------------

void PipWindow::Show() {
    ::ShowWindow(m_hwnd, SW_SHOWNA);
    ::UpdateWindow(m_hwnd);
}

void PipWindow::InitOverlayResources(const std::vector<uint32_t>& overlayPixels) {
    if (overlayPixels.empty() || m_contentW == 0 || m_contentH == 0) return;

    auto dev = m_device->Device();

    // 1. Create overlay texture
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = m_contentW;
    desc.Height           = m_contentH;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subData{};
    subData.pSysMem     = overlayPixels.data();
    subData.SysMemPitch = m_contentW * sizeof(uint32_t);

    if (FAILED(dev->CreateTexture2D(&desc, &subData, m_overlayTex.put()))) return;
    if (FAILED(dev->CreateShaderResourceView(m_overlayTex.get(), nullptr, m_overlaySRV.put()))) return;

    // 2. Compile shaders
    winrt::com_ptr<ID3DBlob> vsBlob, psBlob, errBlob;
    if (FAILED(::D3DCompile(kPipShaderSource, sizeof(kPipShaderSource) - 1, nullptr, nullptr, nullptr,
                            "vs_main", "vs_4_0", 0, 0, vsBlob.put(), errBlob.put()))) return;
    if (FAILED(::D3DCompile(kPipShaderSource, sizeof(kPipShaderSource) - 1, nullptr, nullptr, nullptr,
                            "ps_main", "ps_4_0", 0, 0, psBlob.put(), errBlob.put()))) return;

    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.put());
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.put());

    // 3. Create blend state (standard premultiplied/alpha blend)
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, m_blendState.put());

    // 4. Create sampler state
    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev->CreateSamplerState(&sd, m_sampler.put());
}

// ---------------------------------------------------------------------------
// Frame rendering (called from CaptureSession's thread-pool thread)
// ---------------------------------------------------------------------------

void PipWindow::RenderFrame(ID3D11Texture2D* croppedTexture, UINT w, UINT h) {
    std::lock_guard lk(m_renderMutex);
    if (!m_swapChain) return;

    // Get back buffer
    winrt::com_ptr<ID3D11Texture2D> backBuf;
    winrt::check_hresult(m_swapChain->GetBuffer(
        0, __uuidof(ID3D11Texture2D), backBuf.put_void()));

    // If sizes match: CopyResource (fastest). Otherwise: CopySubresourceRegion.
    D3D11_TEXTURE2D_DESC bbDesc{};
    backBuf->GetDesc(&bbDesc);

    if (bbDesc.Width == w && bbDesc.Height == h) {
        m_device->Context()->CopyResource(backBuf.get(), croppedTexture);
    } else {
        D3D11_BOX box{ 0, 0, 0, (std::min)(w, bbDesc.Width),
                                  (std::min)(h, bbDesc.Height), 1 };
        m_device->Context()->CopySubresourceRegion(
            backBuf.get(), 0, 0, 0, 0,
            croppedTexture, 0, &box);
    }

    // Blend annotation overlay on top if present
    if (m_overlaySRV && m_vs && m_ps) {
        winrt::com_ptr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(m_device->Device()->CreateRenderTargetView(backBuf.get(), nullptr, rtv.put()))) {
            auto ctx = m_device->Context();
            ID3D11RenderTargetView* rtvs[] = { rtv.get() };
            ctx->OMSetRenderTargets(1, rtvs, nullptr);
            ctx->OMSetBlendState(m_blendState.get(), nullptr, 0xffffffff);

            D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(bbDesc.Width), static_cast<float>(bbDesc.Height), 0.0f, 1.0f };
            ctx->RSSetViewports(1, &vp);

            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(m_vs.get(), nullptr, 0);
            ctx->PSSetShader(m_ps.get(), nullptr, 0);

            ID3D11ShaderResourceView* srvs[] = { m_overlaySRV.get() };
            ctx->PSSetShaderResources(0, 1, srvs);
            ID3D11SamplerState* samps[] = { m_sampler.get() };
            ctx->PSSetSamplers(0, 1, samps);

            ctx->Draw(3, 0);

            ID3D11ShaderResourceView* nullSRV[] = { nullptr };
            ctx->PSSetShaderResources(0, 1, nullSRV);
            ID3D11RenderTargetView* nullRTV[] = { nullptr };
            ctx->OMSetRenderTargets(1, nullRTV, nullptr);
        }
    }

    // VSync present — DWM stretches the back buffer to the window rect
    m_swapChain->Present(1, 0);
}

// ---------------------------------------------------------------------------
// Swap chain (DXGI 1.2+ FLIP_DISCARD with STRETCH scaling)
// ---------------------------------------------------------------------------

void PipWindow::CreateSwapChain(UINT w, UINT h) {
    auto dxgiDev = m_device->Device();

    winrt::com_ptr<IDXGIDevice2> dxgi;
    winrt::check_hresult(dxgiDev->QueryInterface(
        __uuidof(IDXGIDevice2), dxgi.put_void()));

    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(dxgi->GetAdapter(adapter.put()));

    winrt::com_ptr<IDXGIFactory2> factory;
    winrt::check_hresult(adapter->GetParent(
        __uuidof(IDXGIFactory2), factory.put_void()));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width              = w;
    desc.Height             = h;
    desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count   = 1;
    desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount        = 2;
    desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling            = DXGI_SCALING_STRETCH;   // DWM handles resize
    desc.AlphaMode          = DXGI_ALPHA_MODE_IGNORE;

    winrt::check_hresult(factory->CreateSwapChainForHwnd(
        m_device->Device(), m_hwnd,
        &desc, nullptr, nullptr,
        m_swapChain.put()));

    // Disable Alt+Enter fullscreen toggle
    factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
}

// ---------------------------------------------------------------------------
// WndProc — minimal: draggable client area + close handling
// ---------------------------------------------------------------------------

LRESULT CALLBACK PipWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Store 'this' pointer on creation
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }

    auto* self = reinterpret_cast<PipWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCHITTEST: {
        // Keep native resize handles, but make the client area draggable
        LRESULT hit = ::DefWindowProcW(hwnd, msg, wp, lp);
        if (hit == HTCLIENT) return HTCAPTION;
        return hit;
    }

    case WM_SIZING: {
        if (!self || self->m_contentH == 0) break;
        // Free resize for edge drags
        if (wp == WMSZ_LEFT || wp == WMSZ_RIGHT || wp == WMSZ_TOP || wp == WMSZ_BOTTOM) {
            return TRUE;
        }

        // Corner drag — lock aspect ratio, follow whichever axis the user moved more
        RECT* r = reinterpret_cast<RECT*>(lp);
        const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_EXSTYLE));

        RECT adj = {0, 0, 0, 0};
        ::AdjustWindowRectEx(&adj, style, FALSE, exStyle);
        int bw = adj.right - adj.left;
        int bh = adj.bottom - adj.top;

        float aspect = static_cast<float>(self->m_contentW) / static_cast<float>(self->m_contentH);

        int proposedCW = (r->right - r->left) - bw;
        int proposedCH = (r->bottom - r->top) - bh;
        if (proposedCW < 1) proposedCW = 1;
        if (proposedCH < 1) proposedCH = 1;

        // Two candidates: fit-to-width vs fit-to-height
        int cwFromH = static_cast<int>(proposedCH * aspect);
        int chFromW = static_cast<int>(proposedCW / aspect);

        // Pick the one that produces a larger window (follows the cursor outward)
        int finalCW, finalCH;
        if (cwFromH > proposedCW) {
            // Height-driven produces wider → use width-driven
            finalCW = proposedCW;
            finalCH = chFromW;
        } else {
            // Width-driven produces taller → use height-driven
            finalCW = cwFromH;
            finalCH = proposedCH;
        }

        // Apply back to RECT, anchoring the correct edges
        bool anchorRight  = (wp == WMSZ_TOPLEFT  || wp == WMSZ_BOTTOMLEFT);
        bool anchorBottom = (wp == WMSZ_TOPLEFT  || wp == WMSZ_TOPRIGHT);

        if (anchorRight) r->left   = r->right  - finalCW - bw;
        else             r->right  = r->left   + finalCW + bw;

        if (anchorBottom) r->top   = r->bottom - finalCH - bh;
        else              r->bottom = r->top   + finalCH + bh;

        return TRUE;
    }

    case WM_CLOSE:
        // Defer cleanup to the main-thread message loop
        if (self && self->m_onClose)
            self->m_onClose();
        return 0;   // do NOT call DestroyWindow here

    case WM_DESTROY:
        if (self) self->m_hwnd = nullptr;
        return 0;
    }

    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace nskry
