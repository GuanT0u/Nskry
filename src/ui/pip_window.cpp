#include "pch.h"
#include "ui/pip_window.h"
#include <d3dcompiler.h>
#include <windowsx.h>

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

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PipWindow::PipWindow(
    std::shared_ptr<D3DDevice> device,
    UINT contentWidth, UINT contentHeight,
    std::function<void()> onCloseRequest,
    AnnotationEngine initialEngine,
    ImageActions imageActions)
    : m_device(std::move(device))
    , m_contentW(contentWidth)
    , m_contentH(contentHeight)
    , m_onClose(std::move(onCloseRequest))
    , m_annotationEngine(std::move(initialEngine))
    , m_imageActions(std::move(imageActions))
{
    // --- Register main window class (once) ---------------------------------
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

    // --- Register toolbar child window class (once) ------------------------
    std::call_once(s_toolbarClassOnce, [&] {
        WNDCLASSEXW twc{};
        twc.cbSize        = sizeof(twc);
        twc.style         = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
        twc.lpfnWndProc   = ToolbarWndProc;
        twc.hInstance      = ::GetModuleHandleW(nullptr);
        twc.lpszClassName  = kToolbarClassName;
        twc.hbrBackground  = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
        twc.hCursor        = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        ::RegisterClassExW(&twc);
    });

    m_font = ::CreateFontW(
        -12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

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
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;

    RECT rc = { 0, 0, winW, winH };
    ::AdjustWindowRectEx(&rc, style, FALSE, exStyle);

    // --- Create the main window --------------------------------------------
    m_hwnd = ::CreateWindowExW(
        exStyle, kClassName, L"Nskry PiP",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr,
        ::GetModuleHandleW(nullptr),
        this);

    // --- Create DXGI swap chain --------------------------------------------
    CreateSwapChain(contentWidth, contentHeight);

    // --- Initialize D3D overlay pipeline -----------------------------------
    InitD3DOverlayPipeline();

    // --- Render initial annotations if present -----------------------------
    if (!m_annotationEngine.IsEmpty()) {
        UpdateOverlayFromEngine();
    }

    // --- Create toolbar child window (initially hidden) --------------------
    CreateToolbarWindow();
}

PipWindow::~PipWindow() {
    CommitTextEdit();
    if (m_font) {
        ::DeleteObject(m_font);
        m_font = nullptr;
    }
    if (m_hwndToolbar) {
        ::DestroyWindow(m_hwndToolbar);
        m_hwndToolbar = nullptr;
    }
    {
        std::lock_guard lk(m_renderMutex);
        m_swapChain = nullptr;
        m_overlaySRV = nullptr;
        m_overlayTex = nullptr;
        m_vs = nullptr;
        m_ps = nullptr;
        m_blendState = nullptr;
        m_sampler = nullptr;
        m_lastFrameTex = nullptr;
        m_stagingTex = nullptr;
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
// D3D11 Overlay Pipeline & Texture Update
// ---------------------------------------------------------------------------

void PipWindow::InitD3DOverlayPipeline() {
    auto dev = m_device->Device();

    winrt::com_ptr<ID3DBlob> vsBlob, psBlob, errBlob;
    if (FAILED(::D3DCompile(kPipShaderSource, sizeof(kPipShaderSource) - 1, nullptr, nullptr, nullptr,
                            "vs_main", "vs_4_0", 0, 0, vsBlob.put(), errBlob.put()))) return;
    if (FAILED(::D3DCompile(kPipShaderSource, sizeof(kPipShaderSource) - 1, nullptr, nullptr, nullptr,
                            "ps_main", "ps_4_0", 0, 0, psBlob.put(), errBlob.put()))) return;

    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.put());
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.put());

    // Alpha blend state (premultiplied / standard over blend)
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

    // Sampler state
    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev->CreateSamplerState(&sd, m_sampler.put());
}

PipWindow::FrameHdcGuard PipWindow::GetLastFrameHdc() {
    FrameHdcGuard guard;
    if (m_contentW == 0 || m_contentH == 0) return guard;

    auto dev = m_device->Device();
    auto ctx = m_device->Context();

    std::lock_guard lk(m_renderMutex);
    if (!m_lastFrameTex) return guard;

    if (m_stagingTex) {
        D3D11_TEXTURE2D_DESC sd{};
        m_stagingTex->GetDesc(&sd);
        if (sd.Width != m_contentW || sd.Height != m_contentH) {
            m_stagingTex = nullptr;
        }
    }
    if (!m_stagingTex) {
        D3D11_TEXTURE2D_DESC desc{};
        m_lastFrameTex->GetDesc(&desc);
        desc.Usage          = D3D11_USAGE_STAGING;
        desc.BindFlags      = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags      = 0;
        if (FAILED(dev->CreateTexture2D(&desc, nullptr, m_stagingTex.put()))) {
            return guard;
        }
    }

    ctx->CopyResource(m_stagingTex.get(), m_lastFrameTex.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(m_stagingTex.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = static_cast<LONG>(m_contentW);
        bmi.bmiHeader.biHeight      = -static_cast<LONG>(m_contentH); // Top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC hdcScreen = ::GetDC(nullptr);
        guard.hdc  = ::CreateCompatibleDC(hdcScreen);
        guard.hbmp = ::CreateDIBSection(guard.hdc, &bmi, DIB_RGB_COLORS, &guard.bits, nullptr, 0);
        ::ReleaseDC(nullptr, hdcScreen);

        if (guard.hbmp && guard.bits) {
            guard.oldBmp = ::SelectObject(guard.hdc, guard.hbmp);
            const BYTE* src = static_cast<const BYTE*>(mapped.pData);
            BYTE* dst = static_cast<BYTE*>(guard.bits);
            size_t rowBytes = m_contentW * 4;
            for (UINT r = 0; r < m_contentH; ++r) {
                memcpy(dst + r * rowBytes, src + r * mapped.RowPitch, rowBytes);
            }
        }
        ctx->Unmap(m_stagingTex.get(), 0);
    }

    return guard;
}

void PipWindow::UpdateOverlayFromEngine() {
    if (m_contentW == 0 || m_contentH == 0) return;

    std::vector<uint32_t> pixels;
    if (!m_annotationEngine.IsEmpty()) {
        FrameHdcGuard fg = GetLastFrameHdc();
        pixels = m_annotationEngine.RenderOverlayRgba(fg.hdc, 0, 0, m_contentW, m_contentH);
    }

    std::lock_guard lk(m_renderMutex);
    if (pixels.empty()) {
        m_overlaySRV = nullptr;
        m_overlayTex = nullptr;
    } else {
        auto dev = m_device->Device();
        if (!m_overlayTex) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width            = m_contentW;
            desc.Height           = m_contentH;
            desc.MipLevels        = 1;
            desc.ArraySize        = 1;
            desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage            = D3D11_USAGE_DEFAULT;
            desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA subData{};
            subData.pSysMem     = pixels.data();
            subData.SysMemPitch = m_contentW * sizeof(uint32_t);

            if (SUCCEEDED(dev->CreateTexture2D(&desc, &subData, m_overlayTex.put()))) {
                dev->CreateShaderResourceView(m_overlayTex.get(), nullptr, m_overlaySRV.put());
            }
        } else {
            m_device->Context()->UpdateSubresource(
                m_overlayTex.get(), 0, nullptr,
                pixels.data(), m_contentW * sizeof(uint32_t), 0);
        }
    }

    // Re-present on last known frame if available
    if (m_lastFrameTex) {
        BlitAndPresent(m_lastFrameTex.get(), m_contentW, m_contentH);
    }
}

void PipWindow::BlitAndPresent(ID3D11Texture2D* frameTex, UINT w, UINT h) {
    if (!m_swapChain || !frameTex) return;

    // Get back buffer
    winrt::com_ptr<ID3D11Texture2D> backBuf;
    if (FAILED(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuf.put_void()))) return;

    D3D11_TEXTURE2D_DESC bbDesc{};
    backBuf->GetDesc(&bbDesc);

    if (bbDesc.Width == w && bbDesc.Height == h) {
        m_device->Context()->CopyResource(backBuf.get(), frameTex);
    } else {
        D3D11_BOX box{ 0, 0, 0, (std::min)(w, bbDesc.Width), (std::min)(h, bbDesc.Height), 1 };
        m_device->Context()->CopySubresourceRegion(backBuf.get(), 0, 0, 0, 0, frameTex, 0, &box);
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

    m_swapChain->Present(1, 0);
}

// ---------------------------------------------------------------------------
// Frame rendering (called from CaptureSession's thread-pool thread)
// ---------------------------------------------------------------------------

void PipWindow::RenderFrame(ID3D11Texture2D* croppedTexture, UINT w, UINT h) {
    std::lock_guard lk(m_renderMutex);
    if (!m_swapChain) return;

    // Cache latest frame
    if (!m_lastFrameTex) {
        D3D11_TEXTURE2D_DESC d{};
        croppedTexture->GetDesc(&d);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.Usage     = D3D11_USAGE_DEFAULT;
        m_device->Device()->CreateTexture2D(&d, nullptr, m_lastFrameTex.put());
    }
    if (m_lastFrameTex) {
        m_device->Context()->CopyResource(m_lastFrameTex.get(), croppedTexture);
    }

    BlitAndPresent(croppedTexture, w, h);
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
// WndProc dispatch
// ---------------------------------------------------------------------------

LRESULT CALLBACK PipWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto self = reinterpret_cast<PipWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMessage(hwnd, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PipWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: {
        LRESULT hit = ::DefWindowProcW(hwnd, msg, wp, lp);
        if (hit == HTCLIENT) {
            if (m_isEditing) return HTCLIENT;
            return HTCAPTION; // Draggable client area
        }
        return hit;
    }

    case WM_SIZING: {
        if (m_contentH == 0) break;
        // Free resize for edge drags
        if (wp == WMSZ_LEFT || wp == WMSZ_RIGHT || wp == WMSZ_TOP || wp == WMSZ_BOTTOM) {
            return TRUE;
        }

        // Corner drag — lock aspect ratio
        RECT* r = reinterpret_cast<RECT*>(lp);
        const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_EXSTYLE));

        RECT adj = {0, 0, 0, 0};
        ::AdjustWindowRectEx(&adj, style, FALSE, exStyle);
        int bw = adj.right - adj.left;
        int bh = adj.bottom - adj.top;

        float aspect = static_cast<float>(m_contentW) / static_cast<float>(m_contentH);

        int proposedCW = (r->right - r->left) - bw;
        int proposedCH = (r->bottom - r->top) - bh;
        if (proposedCW < 1) proposedCW = 1;
        if (proposedCH < 1) proposedCH = 1;

        int cwFromH = static_cast<int>(proposedCH * aspect);
        int chFromW = static_cast<int>(proposedCW / aspect);

        int finalCW, finalCH;
        if (cwFromH > proposedCW) {
            finalCW = proposedCW;
            finalCH = chFromW;
        } else {
            finalCW = cwFromH;
            finalCH = proposedCH;
        }

        bool anchorRight  = (wp == WMSZ_TOPLEFT  || wp == WMSZ_BOTTOMLEFT);
        bool anchorBottom = (wp == WMSZ_TOPLEFT  || wp == WMSZ_TOPRIGHT);

        if (anchorRight) r->left   = r->right  - finalCW - bw;
        else             r->right  = r->left   + finalCW + bw;

        if (anchorBottom) r->top   = r->bottom - finalCH - bh;
        else              r->bottom = r->top   + finalCH + bh;

        return TRUE;
    }

    case WM_NCRBUTTONUP:
        if (wp == HTCAPTION) {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ShowContextMenu(pt.x, pt.y);
            return 0;
        }
        break;

    case WM_NCLBUTTONDBLCLK:
        if (wp == HTCAPTION) {
            EnterEditMode();
            return 0;
        }
        break;

    case WM_CONTEXTMENU: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt.x == -1 && pt.y == -1) {
            RECT rc{}; ::GetWindowRect(hwnd, &rc);
            pt = { rc.left + 20, rc.top + 20 };
        }
        ShowContextMenu(pt.x, pt.y);
        return 0;
    }

    case WM_RBUTTONUP: {
        if (m_isEditing) {
            if (m_annotationEngine.GetTool() != ToolType::None) {
                m_annotationEngine.SetTool(ToolType::None);
                RECT rc{}; ::GetClientRect(hwnd, &rc);
                BuildToolbar(rc.right, rc.bottom);
            } else {
                FinishEdit(false);
            }
            return 0;
        }
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ::ClientToScreen(hwnd, &pt);
        ShowContextMenu(pt.x, pt.y);
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        if (!m_isEditing) {
            EnterEditMode();
            return 0;
        }
        break;

    case WM_LBUTTONDOWN:
        if (m_isEditing) {
            OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
        if (m_isEditing) {
            OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (m_isEditing) {
            OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (m_isEditing) {
            POINT pt;
            ::GetCursorPos(&pt);
            ::ScreenToClient(hwnd, &pt);
            RECT rc{}; ::GetClientRect(hwnd, &rc);
            if (::PtInRect(&rc, pt)) {
                if (m_annotationEngine.GetTool() == ToolType::Text) {
                    ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32513))); // IDC_IBEAM
                } else if (m_annotationEngine.GetTool() != ToolType::None) {
                    ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32515))); // IDC_CROSS
                } else {
                    ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))); // IDC_ARROW
                }
                return TRUE;
            }
        }
        break;

    case WM_WINDOWPOSCHANGED: {
        auto pos = reinterpret_cast<WINDOWPOS*>(lp);
        if (m_isEditing && m_hwndToolbar) {
            if (!(pos->flags & SWP_NOMOVE) || !(pos->flags & SWP_NOSIZE)) {
                if (m_hTextEdit && !(pos->flags & SWP_NOMOVE)) {
                    CommitTextEdit();
                }
                RECT rc{}; ::GetClientRect(hwnd, &rc);
                BuildToolbar(rc.right, rc.bottom);
            }
        }
        break;
    }

    case WM_SHOWWINDOW: {
        if (m_hwndToolbar) {
            if (!wp) {
                ::ShowWindow(m_hwndToolbar, SW_HIDE);
            } else if (m_isEditing) {
                ::ShowWindow(m_hwndToolbar, SW_SHOWNOACTIVATE);
            }
        }
        break;
    }

    case WM_KEYDOWN:
        if (m_isEditing) {
            if (wp == VK_ESCAPE) {
                if (m_annotationEngine.GetTool() != ToolType::None) {
                    CommitTextEdit();
                    m_annotationEngine.SetTool(ToolType::None);
                    RECT rc{}; ::GetClientRect(hwnd, &rc);
                    BuildToolbar(rc.right, rc.bottom);
                } else {
                    FinishEdit(false);
                }
                return 0;
            }
            if (::GetKeyState(VK_CONTROL) & 0x8000) {
                if (wp == 'Z') {
                    CommitTextEdit();
                    m_annotationEngine.Undo();
                    UpdateOverlayFromEngine();
                    return 0;
                }
                if (wp == 'Y') {
                    CommitTextEdit();
                    m_annotationEngine.Redo();
                    UpdateOverlayFromEngine();
                    return 0;
                }
            }
        }
        break;

    case WM_COMMAND:
        if (HIWORD(wp) == EN_KILLFOCUS && reinterpret_cast<HWND>(lp) == m_hTextEdit) {
            CommitTextEdit();
            return 0;
        }
        break;

    case WM_SIZE: {
        RECT rc{}; ::GetClientRect(hwnd, &rc);
        if (m_isEditing) {
            BuildToolbar(rc.right, rc.bottom);
        }
        return 0;
    }

    case WM_CLOSE:
        FinishEdit(false);
        if (m_onClose) m_onClose();
        return 0;

    case WM_DESTROY:
        m_hwnd = nullptr;
        return 0;
    }

    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Context Menu
// ---------------------------------------------------------------------------

void PipWindow::ShowContextMenu(int screenX, int screenY) {
    HMENU hMenu = ::CreatePopupMenu();
    ::InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, 301, L"\x270E \x6807\x6CE8\x7F16\x8F91 (Edit)");     // ✎ 标注编辑
    ::InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    int position = 2;
    for (size_t index = 0; index < m_imageActions.size() && index < 100; ++index) {
        const std::wstring item = L"T  " + m_imageActions[index].label;
        ::InsertMenuW(hMenu, position++, MF_BYPOSITION | MF_STRING, 320 + static_cast<UINT>(index), item.c_str());
    }
    if (!m_imageActions.empty()) ::InsertMenuW(hMenu, position++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    ::InsertMenuW(hMenu, position, MF_BYPOSITION | MF_STRING, 302, L"\x2715 \x5173\x95ED (Close)");            // ✕ 关闭

    ::SetForegroundWindow(m_hwnd);
    int cmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, screenX, screenY, 0, m_hwnd, nullptr);
    ::DestroyMenu(hMenu);

    switch (cmd) {
    case 301: EnterEditMode(); break;
    case 302: if (m_onClose) m_onClose(); break;
    default:
        if (cmd >= 320 && cmd < 320 + static_cast<int>(m_imageActions.size())) {
            FrameHdcGuard frame = GetLastFrameHdc();
            if (!frame.hbmp) break;
            RECT region{};
            ::GetWindowRect(m_hwnd, &region);
            const ImageAction& action = m_imageActions[cmd - 320];
            if (action.invoke) action.invoke(frame.hbmp, static_cast<int>(m_contentW), static_cast<int>(m_contentH), region, m_hwnd);
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// In-place Annotation Editing
// ---------------------------------------------------------------------------

void PipWindow::EnterEditMode() {
    m_isEditing = true;
    m_backupEngine = m_annotationEngine.Clone();
    m_annotationEngine.SetTool(ToolType::Pen);

    RECT rc{}; ::GetClientRect(m_hwnd, &rc);
    BuildToolbar(rc.right, rc.bottom);
    ::ShowWindow(m_hwndToolbar, SW_SHOWNOACTIVATE);
    ::InvalidateRect(m_hwndToolbar, nullptr, FALSE);
    ::UpdateWindow(m_hwndToolbar);
}

void PipWindow::FinishEdit(bool apply) {
    CommitTextEdit();

    if (!apply) {
        m_annotationEngine.RestoreFrom(m_backupEngine);
    }

    m_annotationEngine.SetTool(ToolType::None);
    m_isEditing = false;
    if (m_hwndToolbar) {
        ::ShowWindow(m_hwndToolbar, SW_HIDE);
    }
    UpdateOverlayFromEngine();
}

static WNDPROC s_origPipEditProc = nullptr;
LRESULT CALLBACK PipWindow::TextEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto pip = reinterpret_cast<PipWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS;
    }
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            if (pip) pip->CommitTextEdit();
            return 0;
        }
        if (wp == VK_ESCAPE) {
            HWND h = hwnd;
            ::DestroyWindow(h);
            return 0;
        }
    } else if (msg == WM_KILLFOCUS) {
        if (pip) pip->CommitTextEdit();
        return 0;
    }
    return ::CallWindowProcW(s_origPipEditProc, hwnd, msg, wp, lp);
}

void PipWindow::CommitTextEdit() {
    if (!m_hTextEdit) return;

    HWND hEdit = m_hTextEdit;
    m_hTextEdit = nullptr;

    int len = ::GetWindowTextLengthW(hEdit);
    if (len > 0) {
        std::vector<wchar_t> buf(len + 1);
        ::GetWindowTextW(hEdit, buf.data(), len + 1);
        m_annotationEngine.AddTextShape(m_textEditPos, buf.data());
        UpdateOverlayFromEngine();
    }

    ::DestroyWindow(hEdit);
}

void PipWindow::OnLButtonDown(int x, int y) {
    if (m_annotationEngine.GetTool() == ToolType::None) return;

    CommitTextEdit();

    RECT rc{}; ::GetClientRect(m_hwnd, &rc);
    int cw = rc.right; int ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return;

    int bmpX = x * static_cast<int>(m_contentW) / cw;
    int bmpY = y * static_cast<int>(m_contentH) / ch;

    if (m_annotationEngine.GetTool() == ToolType::Text) {
        m_textEditPos = { bmpX, bmpY };
        POINT ptScreen = { x, y };
        ::ClientToScreen(m_hwnd, &ptScreen);

        HMONITOR hMon = ::MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (::GetMonitorInfoW(hMon, &mi)) {
            if (ptScreen.x + 140 > mi.rcWork.right - 4) ptScreen.x = mi.rcWork.right - 144;
            if (ptScreen.y + 26 > mi.rcWork.bottom - 4) ptScreen.y = mi.rcWork.bottom - 30;
        }

        m_hTextEdit = ::CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"EDIT", L"",
            WS_POPUP | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            ptScreen.x, ptScreen.y, 140, 26,
            m_hwnd,
            nullptr,
            ::GetModuleHandleW(nullptr), nullptr);
        ::SetWindowLongPtrW(m_hTextEdit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        s_origPipEditProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(m_hTextEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(TextEditSubclassProc)));
        ::SendMessageW(m_hTextEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        ::SetActiveWindow(m_hTextEdit);
        ::SetFocus(m_hTextEdit);
    } else {
        m_isDrawing = true;
        m_annotationEngine.OnMouseDown({ bmpX, bmpY });
        ::SetCapture(m_hwnd);
        UpdateOverlayFromEngine();
    }
}

void PipWindow::OnMouseMove(int x, int y) {
    if (m_isDrawing) {
        RECT rc{}; ::GetClientRect(m_hwnd, &rc);
        int cw = rc.right; int ch = rc.bottom;
        if (cw > 0 && ch > 0) {
            int bmpX = x * static_cast<int>(m_contentW) / cw;
            int bmpY = y * static_cast<int>(m_contentH) / ch;
            m_annotationEngine.OnMouseMove({ bmpX, bmpY });
            UpdateOverlayFromEngine();
        }
    }
}

void PipWindow::OnLButtonUp(int x, int y) {
    if (m_isDrawing) {
        ::ReleaseCapture();
        m_isDrawing = false;
        RECT rc{}; ::GetClientRect(m_hwnd, &rc);
        int cw = rc.right; int ch = rc.bottom;
        if (cw > 0 && ch > 0) {
            int bmpX = x * static_cast<int>(m_contentW) / cw;
            int bmpY = y * static_cast<int>(m_contentH) / ch;
            m_annotationEngine.OnMouseUp({ bmpX, bmpY });
        }
        UpdateOverlayFromEngine();
    }
}

// ---------------------------------------------------------------------------
// Toolbar Child Window
// ---------------------------------------------------------------------------

void PipWindow::CreateToolbarWindow() {
    m_hwndToolbar = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kToolbarClassName, L"",
        WS_POPUP,
        0, 0, 100, 30,
        m_hwnd, nullptr,
        ::GetModuleHandleW(nullptr),
        this);
}

LRESULT CALLBACK PipWindow::ToolbarWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto self = reinterpret_cast<PipWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->HandleToolbarMessage(hwnd, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PipWindow::HandleToolbarMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = ::BeginPaint(hwnd, &ps);

        RECT rc{}; ::GetClientRect(hwnd, &rc);
        int w = rc.right;
        int h = rc.bottom;

        HDC hdcMem = ::CreateCompatibleDC(hdc);
        HBITMAP hbmp = ::CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBmp = ::SelectObject(hdcMem, hbmp);

        DrawToolbar(hdcMem, w, h);

        ::BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);

        ::SelectObject(hdcMem, oldBmp);
        ::DeleteObject(hbmp);
        ::DeleteDC(hdcMem);

        ::EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_LBUTTONDBLCLK:
        return HandleToolbarMessage(hwnd, WM_LBUTTONDOWN, wp, lp);

    case WM_SETCURSOR: {
        POINT pt;
        ::GetCursorPos(&pt);
        ::ScreenToClient(hwnd, &pt);
        for (const auto& item : m_toolbarItems) {
            if (!item.isSeparator && ::PtInRect(&item.rect, pt)) {
                ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32649))); // IDC_HAND
                return TRUE;
            }
        }
        ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))); // IDC_ARROW
        return TRUE;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        POINT pt{ x, y };

        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        ::TrackMouseEvent(&tme);

        bool needRepaint = false;
        for (auto& item : m_toolbarItems) {
            if (item.isSeparator) continue;
            bool inside = ::PtInRect(&item.rect, pt);
            if (inside != item.hovered) {
                item.hovered = inside;
                needRepaint = true;
            }
        }
        if (needRepaint) ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSELEAVE: {
        bool needRepaint = false;
        for (auto& item : m_toolbarItems) {
            if (item.hovered) {
                item.hovered = false;
                needRepaint = true;
            }
        }
        if (needRepaint) ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        for (const auto& item : m_toolbarItems) {
            if (item.isSeparator) continue;
            if (::PtInRect(&item.rect, pt)) {
                CommitTextEdit();

                if (item.action == 1) { FinishEdit(true);  return 0; } // Done
                if (item.action == 2) { FinishEdit(false); return 0; } // Cancel
                if (item.action == 3) { m_annotationEngine.Undo(); UpdateOverlayFromEngine(); return 0; }
                if (item.action == 4) { m_annotationEngine.Redo(); UpdateOverlayFromEngine(); return 0; }

                if (item.action == 5) { // Width
                    m_annotationEngine.SetStrokeWidth(item.widthVal);
                    m_annotationEngine.SetFontSize(item.widthVal == 2 ? 14 : (item.widthVal == 4 ? 18 : 26));
                    RECT rc{}; ::GetClientRect(m_hwnd, &rc);
                    BuildToolbar(rc.right, rc.bottom);
                    return 0;
                }

                if (item.action == 6) { // Color
                    m_annotationEngine.SetColor(item.color);
                    RECT rc{}; ::GetClientRect(m_hwnd, &rc);
                    BuildToolbar(rc.right, rc.bottom);
                    return 0;
                }

                if (item.tool != ToolType::None) {
                    if (m_annotationEngine.GetTool() == item.tool) {
                        m_annotationEngine.SetTool(ToolType::None);
                    } else {
                        m_annotationEngine.SetTool(item.tool);
                    }
                    RECT rc{}; ::GetClientRect(m_hwnd, &rc);
                    BuildToolbar(rc.right, rc.bottom);
                    return 0;
                }
            }
        }
        return 0;
    }
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void PipWindow::BuildToolbar(int clientW, int clientH) {
    m_toolbarItems.clear();

    const bool hasSub = (m_annotationEngine.GetTool() != ToolType::None);

    struct Def {
        ToolType tool;
        int      action;
        const wchar_t* label;
        int      width;
        bool     isSep;
    };

    Def defs[] = {
        { ToolType::Rect,    0, L"矩形",   36, false },
        { ToolType::Ellipse, 0, L"圆形",   36, false },
        { ToolType::Arrow,   0, L"箭头",   36, false },
        { ToolType::Pen,     0, L"画笔",   36, false },
        { ToolType::Mosaic,  0, L"马赛克", 46, false },
        { ToolType::Text,    0, L"文本",   36, false },
        { ToolType::None,    0, nullptr,    4, true  },
        { ToolType::None,    3, L"撤销",   36, false },
        { ToolType::None,    4, L"重做",   36, false },
        { ToolType::None,    0, nullptr,    4, true  },
        { ToolType::None,    1, L"✓ 完成", 46, false },
        { ToolType::None,    2, L"✕ 取消", 46, false },
    };

    constexpr int gap = 2;
    constexpr int barH = 26;
    constexpr int subItemH = 24;

    int totalMainW = 0;
    for (const auto& d : defs) totalMainW += d.width + gap;
    totalMainW -= gap;

    int tbW = totalMainW + 12;
    int tbH = hasSub ? (barH + subItemH + 12) : (barH + 10);

    POINT ptScreen = { 0, 0 };
    if (m_hwnd) {
        ::ClientToScreen(m_hwnd, &ptScreen);
    }

    int tbX = ptScreen.x + (clientW - tbW) / 2;
    int tbY = ptScreen.y + clientH - tbH - 10;
    if (tbY < ptScreen.y + 4) tbY = ptScreen.y + 4;

    HMONITOR hMon = ::MonitorFromWindow(m_hwnd ? m_hwnd : m_hwndToolbar, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (::GetMonitorInfoW(hMon, &mi)) {
        if (tbX + tbW > mi.rcWork.right - 4) tbX = mi.rcWork.right - 4 - tbW;
        if (tbX < mi.rcWork.left + 4)        tbX = mi.rcWork.left + 4;
        if (tbY + tbH > mi.rcWork.bottom - 4) tbY = mi.rcWork.bottom - 4 - tbH;
        if (tbY < mi.rcWork.top + 4)          tbY = mi.rcWork.top + 4;
    }

    if (m_hwndToolbar) {
        ::SetWindowPos(
            m_hwndToolbar,
            HWND_TOPMOST,
            tbX, tbY, tbW, tbH,
            SWP_NOACTIVATE | (m_isEditing ? SWP_SHOWWINDOW : SWP_HIDEWINDOW)
        );
    }

    // Primary row inside toolbar local coords
    int startX = (tbW - totalMainW) / 2;
    if (startX < 4) startX = 4;
    int startY = 4;

    int curX = startX;
    for (const auto& d : defs) {
        PipToolItem item{};
        item.rect        = { curX, startY, curX + d.width, startY + barH };
        item.tool        = d.tool;
        item.action      = d.action;
        item.label       = d.label;
        item.isSeparator = d.isSep;
        item.selected    = (d.tool != ToolType::None && m_annotationEngine.GetTool() == d.tool);
        m_toolbarItems.push_back(item);
        curX += d.width + gap;
    }

    // Secondary row (if active tool)
    if (hasSub) {
        int subY = startY + barH + 4;
        const int subBarW = 298;
        int subStartX = (tbW - subBarW) / 2;
        if (subStartX < 4) subStartX = 4;

        int sx = subStartX;
        int widths[] = { 2, 4, 8 };
        const wchar_t* wLabels[] = { L"● 2", L"● 4", L"● 8" };
        for (int i = 0; i < 3; i++) {
            PipToolItem item{};
            item.rect     = { sx, subY, sx + 32, subY + subItemH };
            item.action   = 5; // Width
            item.widthVal = widths[i];
            item.label    = wLabels[i];
            item.selected = (m_annotationEngine.GetStrokeWidth() == widths[i]);
            m_toolbarItems.push_back(item);
            sx += 32 + gap;
        }

        PipToolItem sep{};
        sep.rect        = { sx, subY, sx + 4, subY + subItemH };
        sep.isSeparator = true;
        m_toolbarItems.push_back(sep);
        sx += 4 + gap;

        COLORREF colors[] = {
            RGB(255, 59, 48),   // Red
            RGB(255, 149, 0),  // Orange
            RGB(255, 204, 0),  // Yellow
            RGB(52, 199, 89),  // Green
            RGB(0, 122, 255),  // Blue
            RGB(175, 82, 222), // Purple
            RGB(240, 240, 245),// White
            RGB(30, 30, 35)    // Dark
        };

        for (int i = 0; i < 8; i++) {
            PipToolItem item{};
            item.rect          = { sx, subY, sx + 22, subY + subItemH };
            item.action        = 6; // Color
            item.color         = colors[i];
            item.isColorChoice = true;
            item.selected      = (m_annotationEngine.GetColor() == colors[i]);
            m_toolbarItems.push_back(item);
            sx += 22 + gap;
        }
    }

    if (m_hwndToolbar) {
        ::InvalidateRect(m_hwndToolbar, nullptr, FALSE);
        ::UpdateWindow(m_hwndToolbar);
    }
}

void PipWindow::DrawToolbar(HDC hdc, int clientW, int clientH) {
    // 1. Draw Toolbar background container
    RECT bgRect = { 0, 0, clientW, clientH };
    HBRUSH bgBrush = ::CreateSolidBrush(RGB(32, 32, 38));
    ::FillRect(hdc, &bgRect, bgBrush);
    ::DeleteObject(bgBrush);

    HBRUSH borderBrush = ::CreateSolidBrush(RGB(65, 65, 75));
    ::FrameRect(hdc, &bgRect, borderBrush);
    ::DeleteObject(borderBrush);

    ::SetBkMode(hdc, TRANSPARENT);

    // 2. Draw Items
    for (const auto& item : m_toolbarItems) {
        if (item.isSeparator) {
            int midX = (item.rect.left + item.rect.right) / 2;
            HPEN sepPen = ::CreatePen(PS_SOLID, 1, RGB(70, 70, 80));
            HGDIOBJ oldPen = ::SelectObject(hdc, sepPen);
            ::MoveToEx(hdc, midX, item.rect.top + 3, nullptr);
            ::LineTo(hdc, midX, item.rect.bottom - 3);
            ::SelectObject(hdc, oldPen);
            ::DeleteObject(sepPen);
            continue;
        }

        if (item.isColorChoice) {
            COLORREF cbg = item.hovered ? RGB(60, 60, 70) : RGB(40, 40, 48);
            HBRUSH cbgBrush = ::CreateSolidBrush(cbg);
            ::FillRect(hdc, &item.rect, cbgBrush);
            ::DeleteObject(cbgBrush);

            int cx = (item.rect.left + item.rect.right) / 2;
            int cy = (item.rect.top + item.rect.bottom) / 2;
            int r = 6;

            HBRUSH colBrush = ::CreateSolidBrush(item.color);
            HPEN borderPen = ::CreatePen(PS_SOLID, 1, item.selected ? RGB(255, 255, 255) : RGB(80, 80, 90));
            HGDIOBJ oldBrush = ::SelectObject(hdc, colBrush);
            HGDIOBJ oldPen   = ::SelectObject(hdc, borderPen);

            ::Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);

            if (item.selected) {
                HPEN ringPen = ::CreatePen(PS_SOLID, 2, RGB(0, 150, 255));
                ::SelectObject(hdc, ringPen);
                ::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
                ::Ellipse(hdc, cx - r - 2, cy - r - 2, cx + r + 3, cy + r + 3);
                ::DeleteObject(ringPen);
            }

            ::SelectObject(hdc, oldBrush);
            ::SelectObject(hdc, oldPen);
            ::DeleteObject(colBrush);
            ::DeleteObject(borderPen);
            continue;
        }

        COLORREF btnBg;
        if (item.selected) {
            btnBg = RGB(25, 118, 210);
        } else if (item.action == 1 && item.hovered) { // Done
            btnBg = RGB(46, 125, 50);
        } else if (item.action == 2 && item.hovered) { // Cancel
            btnBg = RGB(198, 40, 40);
        } else if (item.hovered) {
            btnBg = RGB(65, 65, 75);
        } else {
            btnBg = RGB(45, 45, 54);
        }

        HBRUSH itemBgBrush = ::CreateSolidBrush(btnBg);
        ::FillRect(hdc, &item.rect, itemBgBrush);
        ::DeleteObject(itemBgBrush);

        HBRUSH itemBorderBrush = ::CreateSolidBrush(item.selected ? RGB(70, 160, 245) : (item.hovered ? RGB(90, 90, 100) : RGB(60, 60, 70)));
        ::FrameRect(hdc, &item.rect, itemBorderBrush);
        ::DeleteObject(itemBorderBrush);

        if (item.label) {
            HFONT oldFont = static_cast<HFONT>(::SelectObject(hdc, m_font));
            ::SetTextColor(hdc, RGB(240, 240, 245));
            RECT textRect = item.rect;
            ::DrawTextW(hdc, item.label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            ::SelectObject(hdc, oldFont);
        }
    }
}

} // namespace nskry
