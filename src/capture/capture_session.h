#pragma once

#include "capture/d3d_device.h"
#include <winrt/Windows.Graphics.Capture.h>
#include <functional>
#include <mutex>
#include <atomic>

namespace nskry {

/// Axis-aligned rectangle describing the sub-region to crop from a captured frame.
/// Coordinates are relative to the top-left of the captured window content.
struct CropRegion {
    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;
};

/// WGC-based capture session that captures a target HWND, crops the specified
/// sub-region on the GPU, and delivers the cropped texture via a callback.
///
/// Threading model:
///   - FrameArrived fires on a thread-pool thread (CreateFreeThreaded).
///   - The callback is invoked under m_callbackMutex.
///   - The cropped texture is valid only for the duration of the callback.
class CaptureSession {
public:
    /// Callback receives the cropped ID3D11Texture2D and its pixel dimensions.
    using FrameCallback = std::function<void(
        ID3D11Texture2D* croppedTexture, UINT width, UINT height)>;

    CaptureSession(std::shared_ptr<D3DDevice> device,
                   HWND targetHwnd,
                   CropRegion crop);
    ~CaptureSession();

    CaptureSession(const CaptureSession&)            = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    void Start();
    void Stop();

    void SetFrameCallback(FrameCallback cb);
    void SetCropRegion(CropRegion crop);

    [[nodiscard]] bool IsCapturing() const { return m_capturing.load(); }

private:
    // WGC event handlers
    void OnFrameArrived(
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const& args);
    void OnItemClosed(
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& sender,
        winrt::Windows::Foundation::IInspectable const& args);

    void ProcessFrame(
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const& frame);
    void EnsureCropTexture(UINT w, UINT h);

    // Owned resources
    std::shared_ptr<D3DDevice> m_device;
    HWND                       m_targetHwnd{};
    CropRegion                 m_crop;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem           m_item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool    m_pool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession        m_session{ nullptr };

    winrt::com_ptr<ID3D11Texture2D> m_cropTexture;   // intermediate GPU crop dest

    FrameCallback      m_callback;
    std::mutex         m_callbackMutex;
    std::atomic<bool>  m_capturing{ false };

    winrt::event_token m_frameToken{};
    winrt::event_token m_closedToken{};
};

} // namespace nskry
