#include "pch.h"
#include "capture/capture_session.h"

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
    m_frameToken  = m_pool.FrameArrived({ this, &CaptureSession::OnFrameArrived });
    m_closedToken = m_item.Closed({ this, &CaptureSession::OnItemClosed });
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

    // Unsubscribe before closing (prevents callbacks during teardown)
    if (m_pool)    m_pool.FrameArrived(m_frameToken);
    if (m_item)    m_item.Closed(m_closedToken);

    // A free-threaded frame callback may already have entered before event
    // revocation completed.  Wait for it (including the consumer callback) so
    // CleanupPip cannot release PipWindow while RenderFrame is still running.
    {
        std::unique_lock lock(m_frameDrainMutex);
        m_frameDrainCv.wait(lock, [this] { return m_activeFrameCallbacks.load() == 0; });
    }

    {
        std::lock_guard lock(m_callbackMutex);
        m_callback = {};
    }

    if (m_session) { m_session.Close(); m_session = nullptr; }
    if (m_pool)    { m_pool.Close();    m_pool    = nullptr; }
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
    m_crop = crop;
}

// ---------------------------------------------------------------------------
// Frame handling (runs on thread-pool thread)
// ---------------------------------------------------------------------------

void CaptureSession::OnFrameArrived(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
    winrt::Windows::Foundation::IInspectable const&)
{
    m_activeFrameCallbacks.fetch_add(1);
    struct DrainGuard {
        CaptureSession* owner;
        ~DrainGuard() {
            if (owner->m_activeFrameCallbacks.fetch_sub(1) == 1)
                owner->m_frameDrainCv.notify_all();
        }
    } drain{ this };

    if (!m_capturing.load()) return;

    try {
        auto frame = sender.TryGetNextFrame();
        if (!frame) return;
        ProcessFrame(frame);
        frame.Close();      // return the buffer to the pool
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
    Stop();
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

    const int srcX  = (std::max)(0, (std::min)(m_crop.x, cs.Width));
    const int srcY  = (std::max)(0, (std::min)(m_crop.y, cs.Height));
    const int cropW = (std::max)(1, (std::min)(m_crop.width,  cs.Width  - srcX));
    const int cropH = (std::max)(1, (std::min)(m_crop.height, cs.Height - srcY));

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
