#include "pch.h"
#include "ui/pip_window.h"

namespace nskry {

// Custom message posted to MainWindow when the user closes the PiP.
// This avoids re-entrance issues (the PiP's WndProc returns before cleanup).
static constexpr UINT WM_PIP_CLOSE_REQUEST = WM_APP + 100;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PipWindow::PipWindow(
    std::shared_ptr<D3DDevice> device,
    UINT contentWidth, UINT contentHeight,
    std::function<void()> onCloseRequest)
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
}

PipWindow::~PipWindow() {
    // Release swap chain under lock so an in-flight RenderFrame sees nullptr
    {
        std::lock_guard lk(m_renderMutex);
        m_swapChain = nullptr;
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
