#include "pch.h"
#include "capture/capture_session.h"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace nskry {

// ---------------------------------------------------------------------------
// Construction — create WGC objects from an HWND (no Picker UI)
// ---------------------------------------------------------------------------

CaptureSession::CaptureSession(
    std::shared_ptr<D3DDevice> device,
    HWND targetHwnd,
    CropRegion crop)
    : m_device(std::move(device))
    , m_targetHwnd(targetHwnd)
    , m_crop(crop)
{
    // --- 1. Create GraphicsCaptureItem via COM interop (no picker) ----------
    auto interop = winrt::get_activation_factory<
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();

    winrt::check_hresult(interop->CreateForWindow(
        m_targetHwnd,
        winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
        winrt::put_abi(m_item)));

    if (!m_item)
        throw std::runtime_error("CreateForWindow returned null GraphicsCaptureItem");

    // --- 2. Free-threaded frame pool (event-driven, no dispatcher needed) ---
    auto sz = m_item.Size();
    m_poolWidth = sz.Width; m_poolHeight = sz.Height;
    m_pool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool
        ::CreateFreeThreaded(
            m_device->WinRTDevice(),
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,      // double-buffer
            sz);

    // --- 3. Capture session ------------------------------------------------
    m_session = m_pool.CreateCaptureSession(m_item);

    // Hide the yellow capture border (Windows 11 22H2+, silently ignored otherwise)
    try { m_session.IsBorderRequired(false); } catch (...) {}
    // Do not show cursor in the capture
    try { m_session.IsCursorCaptureEnabled(false); } catch (...) {}

    // --- 4. Subscribe to events --------------------------------------------
    m_eventGate = std::make_shared<CallbackGate<CaptureSession>>(this);
    try {
        m_frameToken = m_pool.FrameArrived([gate = m_eventGate](auto const& sender, auto const& args) {
            gate->Run([&](CaptureSession& owner) { owner.OnFrameArrived(sender, args); });
        });
        m_closedToken = m_item.Closed([gate = m_eventGate](auto const& sender, auto const& args) {
            gate->Run([&](CaptureSession& owner) { owner.OnItemClosed(sender, args); });
        });
    } catch (...) { Stop(); throw; }
}

CaptureSession::~CaptureSession() {
    Stop();
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

void CaptureSession::Start() {
    if (m_stopped.load()) return;
    if (m_capturing.exchange(true))
        return;  // already running
    m_session.StartCapture();
}

void CaptureSession::Stop() {
    if (m_stopped.exchange(true)) return;
    m_capturing.store(false);

    // Stop accepting owner access and drain existing leases before teardown.
    // Delegate revocation alone is not a barrier for previously queued calls.
    if (m_eventGate) m_eventGate->Close();
    try { if (m_pool) m_pool.FrameArrived(m_frameToken); } catch (...) {}
    try { if (m_item) m_item.Closed(m_closedToken); } catch (...) {}

    {
        std::lock_guard lock(m_callbackMutex);
        m_callback = {};
        m_closedCallback = {};
    }

    try { if (m_session) m_session.Close(); } catch (...) {}
    try { if (m_pool) m_pool.Close(); } catch (...) {}
    m_session = nullptr; m_pool = nullptr;
    m_item        = nullptr;
    m_cropTexture = nullptr;
}

// ---------------------------------------------------------------------------
// Setters
// ---------------------------------------------------------------------------

void CaptureSession::SetFrameCallback(FrameCallback cb) {
    std::lock_guard lk(m_callbackMutex);
    m_callback = std::move(cb);
}

void CaptureSession::SetCropRegion(CropRegion crop) {
    std::lock_guard lock(m_processingMutex);
    m_crop = crop;
}
void CaptureSession::SetClosedCallback(std::function<void()> cb) {
    std::lock_guard lock(m_callbackMutex); m_closedCallback = std::move(cb);
}

// ---------------------------------------------------------------------------
// Frame handling (runs on thread-pool thread)
// ---------------------------------------------------------------------------

void CaptureSession::OnFrameArrived(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
    winrt::Windows::Foundation::IInspectable const&)
{
    if (!m_capturing.load()) return;

    try {
        std::lock_guard processingLock(m_processingMutex);
        if (!m_capturing.load()) return;
        auto frame = sender.TryGetNextFrame();
        if (!frame) return;
        ++m_acquired;
        struct FrameCloser {
            decltype(frame)& value; bool active = true;
            ~FrameCloser() { if (active) try { value.Close(); } catch (...) {} }
        } closer{frame};
        if (m_processingEnabled.load()) {
            const auto size = frame.ContentSize();
            if (size.Width > 0 && size.Height > 0 && (size.Width != m_poolWidth || size.Height != m_poolHeight)) {
                frame.Close(); closer.active = false;
                sender.Recreate(m_device->WinRTDevice(),
                    winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                m_poolWidth = size.Width; m_poolHeight = size.Height;
            } else { ProcessFrame(frame); ++m_processed; }
        }
    } catch (...) {
        // Capture failures must not escape a WinRT thread-pool callback.
        // Keep the session alive; a later frame may still be valid, and Stop()
        // remains the single owner of teardown/revocation.
    }
}

void CaptureSession::OnItemClosed(
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem const&,
    winrt::Windows::Foundation::IInspectable const&)
{
    // Never synchronously tear down/drain from a capture event callback.
    m_capturing.store(false);
    {
        std::lock_guard lock(m_callbackMutex);
        try { if (!m_stopped.load() && m_closedCallback) m_closedCallback(); } catch (...) {}
    }
}

// ---------------------------------------------------------------------------
// GPU-side crop + callback delivery
// ---------------------------------------------------------------------------

void CaptureSession::ProcessFrame(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& frame)
{
    // --- Get the underlying D3D11 texture from the WinRT surface -----------
    auto surface = frame.Surface();

    winrt::com_ptr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
    winrt::check_hresult(surface.as<::IUnknown>()->QueryInterface(
        __uuidof(::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess),
        access.put_void()));

    winrt::com_ptr<ID3D11Texture2D> srcTex;
    winrt::check_hresult(access->GetInterface(
        __uuidof(ID3D11Texture2D), srcTex.put_void()));

    // --- Clamp crop to actual content size ---------------------------------
    auto cs = frame.ContentSize();
    D3D11_TEXTURE2D_DESC sourceDesc{}; srcTex->GetDesc(&sourceDesc);
    cs.Width = (std::min)(cs.Width, static_cast<int>(sourceDesc.Width));
    cs.Height = (std::min)(cs.Height, static_cast<int>(sourceDesc.Height));

    const int srcX  = (std::max)(0, (std::min)(m_crop.x, cs.Width));
    const int srcY  = (std::max)(0, (std::min)(m_crop.y, cs.Height));
    const int cropW = (std::min)(m_crop.width, cs.Width - srcX);
    const int cropH = (std::min)(m_crop.height, cs.Height - srcY);
    if (cropW <= 0 || cropH <= 0) return;

    // --- Ensure intermediate crop texture ----------------------------------
    EnsureCropTexture(static_cast<UINT>(cropW), static_cast<UINT>(cropH));

    // --- GPU copy: source sub-rect → crop texture (zero CPU) ---------------
    D3D11_BOX box{};
    box.left   = static_cast<UINT>(srcX);
    box.top    = static_cast<UINT>(srcY);
    box.right  = static_cast<UINT>(srcX + cropW);
    box.bottom = static_cast<UINT>(srcY + cropH);
    box.front  = 0;
    box.back   = 1;

    m_device->Context()->CopySubresourceRegion(
        m_cropTexture.get(), 0,     // dest subresource
        0, 0, 0,                     // dest x, y, z
        srcTex.get(), 0,             // src subresource
        &box);

    // --- Deliver to consumer -----------------------------------------------
    std::lock_guard lk(m_callbackMutex);
    if (m_callback)
        m_callback(m_cropTexture.get(),
                   static_cast<UINT>(cropW),
                   static_cast<UINT>(cropH));
}

void CaptureSession::EnsureCropTexture(UINT w, UINT h) {
    if (m_cropTexture) {
        D3D11_TEXTURE2D_DESC d{};
        m_cropTexture->GetDesc(&d);
        if (d.Width == w && d.Height == h)
            return;   // reuse
    }
    m_cropTexture = m_device->CreateTexture(w, h);
}

} // namespace nskry
