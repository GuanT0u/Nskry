#include "capture/region_stream.h"
#include "capture/d3d_device.h"
#include "pch.h"
#include <cstring>
#include <shellscalingapi.h>

namespace nskry {
namespace {
struct RegionStream {
    DWORD thread = GetCurrentThreadId();
    HMONITOR monitor{};
    MONITORINFOEXW initial{};
    RECT region{};
    uint32_t width{}, height{};
    UINT dpiX{}, dpiY{};
    std::shared_ptr<std::atomic<bool>> closed = std::make_shared<std::atomic<bool>>(false);
    winrt::event_token closedToken{};
    std::unique_ptr<D3DDevice> device;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession capture{nullptr};
    winrt::com_ptr<ID3D11Texture2D> staging;
    ~RegionStream() {
        try {
            if (item)
                item.Closed(closedToken);
        } catch (...) {
        }
        try {
            if (capture)
                capture.Close();
        } catch (...) {
        }
        try {
            if (pool)
                pool.Close();
        } catch (...) {
        }
    }
};
void Error(wchar_t *text, uint32_t capacity, const wchar_t *message) {
    if (text && capacity)
        wcsncpy_s(text, capacity, message, _TRUNCATE);
}
HRESULT __cdecl Open(RECT region, uint32_t fps, void **handle, NskryCaptureDescriptionV1 *output, wchar_t *error,
                     uint32_t capacity) {
    if (handle)
        *handle = nullptr;
    if (!handle || !output || output->structSize < sizeof(*output) || output->version != 1 || fps < 1 || fps > 30)
        return E_INVALIDARG;
    try {
        auto stream = std::make_unique<RegionStream>();
        stream->monitor = MonitorFromRect(&region, MONITOR_DEFAULTTONULL);
        stream->initial.cbSize = sizeof(MONITORINFOEXW);
        if (!stream->monitor || !GetMonitorInfoW(stream->monitor, &stream->initial))
            throw std::runtime_error("Display unavailable");
        const auto bounds = stream->initial.rcMonitor;
        const int64_t w = int64_t(region.right) - region.left, h = int64_t(region.bottom) - region.top;
        if (w < 2 || h < 2 || w > 3840 || h > 3840 || w * h > 3840LL * 2160 || region.left < bounds.left ||
            region.top < bounds.top || region.right > bounds.right || region.bottom > bounds.bottom ||
            int64_t(bounds.right - bounds.left) * (bounds.bottom - bounds.top) > 16777216) {
            Error(error, capacity,
                  L"Select one monitor only (2..3840 pixels per side, at most 3840x2160 pixels). Cross-monitor "
                  L"recording is not supported.");
            return E_INVALIDARG;
        }
        stream->region = region;
        winrt::check_hresult(GetDpiForMonitor(stream->monitor, MDT_EFFECTIVE_DPI, &stream->dpiX, &stream->dpiY));
        const double scale = (std::min)(1.0, (std::min)(1920.0 / (std::max)(w, h), 1080.0 / (std::min)(w, h)));
        stream->width = (std::max)(2u, uint32_t(w * scale) & ~1u);
        stream->height = (std::max)(2u, uint32_t(h * scale) & ~1u);
        if (w * scale < 2 || h * scale < 2) {
            Error(error, capacity, L"This region is too thin after downscaling. Select a larger height/width.");
            return E_INVALIDARG;
        }
        stream->device = std::make_unique<D3DDevice>();
        auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                     IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForMonitor(
            stream->monitor, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
            winrt::put_abi(stream->item)));
        stream->closedToken =
            stream->item.Closed([closed = stream->closed](auto const &, auto const &) { closed->store(true); });
        stream->pool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            stream->device->WinRTDevice(),
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, stream->item.Size());
        stream->capture = stream->pool.CreateCaptureSession(stream->item);
        // Retain Windows capture indicator; never bypass OS consent/border policy.
        stream->capture.IsCursorCaptureEnabled(true);
        // Newer Windows can throttle production too. Older Windows still get bounded consumption.
        try {
            stream->capture.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{10000000 / fps});
        } catch (...) {
        }
        stream->capture.StartCapture();
        *output = {sizeof(*output), 1, stream->width, stream->height, stream->width * 4};
        *handle = stream.release();
        return S_OK;
    } catch (const winrt::hresult_error &e) {
        Error(error, capacity, e.message().c_str());
        return e.code();
    } catch (...) {
        Error(error, capacity, L"Could not start monitor capture.");
        return E_FAIL;
    }
}
HRESULT __cdecl Read(void *handle, uint8_t *output, uint32_t capacity) {
    auto *s = static_cast<RegionStream *>(handle);
    if (!s || GetCurrentThreadId() != s->thread || !output || capacity < s->width * s->height * 4)
        return E_INVALIDARG;
    try {
        MONITORINFOEXW now{};
        now.cbSize = sizeof(now);
        UINT dpiX{}, dpiY{};
        if (!GetMonitorInfoW(s->monitor, &now) || !EqualRect(&now.rcMonitor, &s->initial.rcMonitor) ||
            wcscmp(now.szDevice, s->initial.szDevice) || s->closed->load() ||
            FAILED(GetDpiForMonitor(s->monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX != s->dpiX || dpiY != s->dpiY)
            return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
        winrt::check_hresult(s->device->Device()->GetDeviceRemovedReason());
        auto frame = s->pool.TryGetNextFrame();
        if (!frame)
            return S_FALSE;
        struct CloseFrame {
            decltype(frame) &frame;
            ~CloseFrame() {
                try {
                    frame.Close();
                } catch (...) {
                }
            }
        } closer{frame};
        // At most two pool frames. Discard the older, never accumulate a recording queue.
        if (auto newer = s->pool.TryGetNextFrame()) {
            frame.Close();
            frame = std::move(newer);
        }
        const auto size = frame.ContentSize();
        if (size.Width != now.rcMonitor.right - now.rcMonitor.left ||
            size.Height != now.rcMonitor.bottom - now.rcMonitor.top)
            return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> source;
        winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), source.put_void()));
        const UINT w = s->region.right - s->region.left, h = s->region.bottom - s->region.top;
        if (!s->staging) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            winrt::check_hresult(s->device->Device()->CreateTexture2D(&desc, nullptr, s->staging.put()));
        }
        const UINT x = s->region.left - now.rcMonitor.left, y = s->region.top - now.rcMonitor.top;
        D3D11_TEXTURE2D_DESC sourceDesc{};
        source->GetDesc(&sourceDesc);
        if (x + w > sourceDesc.Width || y + h > sourceDesc.Height)
            return E_BOUNDS;
        D3D11_BOX box{x, y, 0, x + w, y + h, 1};
        auto *context = s->device->Context();
        context->CopySubresourceRegion(s->staging.get(), 0, 0, 0, 0, source.get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(s->staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        for (UINT row = 0; row < s->height; ++row) {
            const auto *src =
                static_cast<const uint8_t *>(mapped.pData) + (uint64_t(row) * h / s->height) * mapped.RowPitch;
            auto *dst = output + row * s->width * 4;
            if (w == s->width)
                std::memcpy(dst, src, s->width * 4);
            else
                for (UINT col = 0; col < s->width; ++col)
                    std::memcpy(dst + col * 4, src + (uint64_t(col) * w / s->width) * 4, 4);
        }
        context->Unmap(s->staging.get(), 0);
        return S_OK;
    } catch (const winrt::hresult_error &e) {
        return e.code();
    } catch (...) {
        return E_FAIL;
    }
}
void __cdecl Close(void *handle) {
    delete static_cast<RegionStream *>(handle);
}
} // namespace
const NskryCaptureServiceV1 &RegionCaptureService() {
    static const NskryCaptureServiceV1 service{sizeof(service), 1, Open, Read, Close};
    return service;
}
} // namespace nskry
