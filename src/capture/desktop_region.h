#pragma once
#include "capture/gpu_region_readback.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <winrt/base.h>

namespace nskry {
inline POINT DesktopSurfacePoint(int x, int y, int width, int height, DXGI_MODE_ROTATION rotation) {
    switch (rotation) {
    case DXGI_MODE_ROTATION_ROTATE90:
        return {y, height - 1 - x};
    case DXGI_MODE_ROTATION_ROTATE180:
        return {width - 1 - x, height - 1 - y};
    case DXGI_MODE_ROTATION_ROTATE270:
        return {width - 1 - y, x};
    default:
        return {x, y};
    }
}
// Private region-stream backend. One adapter-specific device and acquired frame;
// no WGC monitor session/indicator, background thread, or shared host context.
class DesktopRegion {
  public:
    DesktopRegion(HMONITOR monitor, RECT region, UINT width, UINT height)
        : m_region(region), m_width(width), m_height(height) {
        winrt::com_ptr<IDXGIFactory1> factory;
        winrt::check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()));
        for (UINT a = 0;; ++a) {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            const auto hr = factory->EnumAdapters1(a, adapter.put());
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            winrt::check_hresult(hr);
            for (UINT o = 0;; ++o) {
                winrt::com_ptr<IDXGIOutput> output;
                const auto result = adapter->EnumOutputs(o, output.put());
                if (result == DXGI_ERROR_NOT_FOUND)
                    break;
                winrt::check_hresult(result);
                DXGI_OUTPUT_DESC desc{};
                winrt::check_hresult(output->GetDesc(&desc));
                if (desc.Monitor != monitor)
                    continue;
                m_desc = desc;
                const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
                D3D_FEATURE_LEVEL actual{};
                winrt::check_hresult(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                                       m_device.put(), &actual, m_context.put()));
                auto extended = output.as<IDXGIOutput1>();
                winrt::check_hresult(extended->DuplicateOutput(m_device.get(), m_duplication.put()));
                return;
            }
        }
        throw winrt::hresult_error(DXGI_ERROR_NOT_FOUND, L"The selected display has no desktop duplication output.");
    }
    HRESULT Read(uint8_t *pixels) {
        winrt::check_hresult(m_device->GetDeviceRemovedReason());
        DXGI_OUTDUPL_FRAME_INFO info{};
        winrt::com_ptr<IDXGIResource> resource;
        const auto acquired = m_duplication->AcquireNextFrame(0, &info, resource.put());
        if (acquired == DXGI_ERROR_WAIT_TIMEOUT)
            return S_FALSE;
        winrt::check_hresult(acquired);
        struct Release {
            IDXGIOutputDuplication *value;
            ~Release() {
                value->ReleaseFrame();
            }
        } release{m_duplication.get()};
        if (info.LastMouseUpdateTime.QuadPart)
            m_pointer = info.PointerPosition;
        if (info.PointerShapeBufferSize) {
            if (info.PointerShapeBufferSize > 1024 * 1024)
                winrt::throw_hresult(E_BOUNDS);
            m_shape.resize(info.PointerShapeBufferSize);
            UINT required{};
            winrt::check_hresult(
                m_duplication->GetFramePointerShape(UINT(m_shape.size()), m_shape.data(), &required, &m_shapeInfo));
            if (!m_shapeInfo.Width || m_shapeInfo.Width > 512 || !m_shapeInfo.Height || m_shapeInfo.Height > 1024 ||
                uint64_t(m_shapeInfo.Pitch) * m_shapeInfo.Height > m_shape.size())
                winrt::throw_hresult(E_BOUNDS);
            if ((m_shapeInfo.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME &&
                 m_shapeInfo.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR &&
                 m_shapeInfo.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) ||
                (m_shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME && (m_shapeInfo.Height & 1)))
                winrt::throw_hresult(E_BOUNDS);
        }
        auto source = resource.as<ID3D11Texture2D>();
        D3D11_TEXTURE2D_DESC texture{};
        source->GetDesc(&texture);
        const bool rotated =
            m_desc.Rotation == DXGI_MODE_ROTATION_ROTATE90 || m_desc.Rotation == DXGI_MODE_ROTATION_ROTATE270;
        if (texture.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            texture.Width != UINT(rotated ? m_desc.DesktopCoordinates.bottom - m_desc.DesktopCoordinates.top
                                          : m_desc.DesktopCoordinates.right - m_desc.DesktopCoordinates.left) ||
            texture.Height != UINT(rotated ? m_desc.DesktopCoordinates.right - m_desc.DesktopCoordinates.left
                                           : m_desc.DesktopCoordinates.bottom - m_desc.DesktopCoordinates.top))
            winrt::throw_hresult(DXGI_ERROR_ACCESS_LOST);
        const int x = m_region.left - m_desc.DesktopCoordinates.left, y = m_region.top - m_desc.DesktopCoordinates.top;
        const int w = m_region.right - m_region.left, h = m_region.bottom - m_region.top;
        const auto first = DesktopSurfacePoint(x, y, texture.Width, texture.Height, m_desc.Rotation);
        const auto last = DesktopSurfacePoint(x + w - 1, y + h - 1, texture.Width, texture.Height, m_desc.Rotation);
        D3D11_BOX box{UINT((std::min)(first.x, last.x)),     UINT((std::min)(first.y, last.y)),     0,
                      UINT((std::max)(first.x, last.x) + 1), UINT((std::max)(first.y, last.y) + 1), 1};
        if (box.right > texture.Width || box.bottom > texture.Height)
            winrt::throw_hresult(E_BOUNDS);
        if ((m_width != UINT(w) || m_height != UINT(h)) && !m_gpuUnavailable) {
            try {
                if (!m_gpu)
                    m_gpu = std::make_unique<GpuRegionReadback>(m_device.get(), m_context.get(), box.right - box.left,
                        box.bottom - box.top, m_width, m_height, m_desc.Rotation);
                m_gpu->Read(source.get(), box, pixels, m_width * m_height * 4);
                DrawPointer(pixels, x, y, w, h);
                return S_OK;
            } catch (const winrt::hresult_error&) {
                m_gpu.reset();
                winrt::check_hresult(m_device->GetDeviceRemovedReason());
                m_gpuUnavailable = true;
            }
        }
        if (!m_staging) {
            texture.Width = box.right - box.left;
            texture.Height = box.bottom - box.top;
            texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
            texture.BindFlags = texture.MiscFlags = texture.SampleDesc.Quality = 0;
            texture.Usage = D3D11_USAGE_STAGING;
            texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            winrt::check_hresult(m_device->CreateTexture2D(&texture, nullptr, m_staging.put()));
        }
        m_context->CopySubresourceRegion(m_staging.get(), 0, 0, 0, 0, source.get(), 0, &box);
        D3D11_TEXTURE2D_DESC full{};
        source->GetDesc(&full);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(m_context->Map(m_staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        for (UINT row = 0; row < m_height; ++row) {
            if (m_desc.Rotation == DXGI_MODE_ROTATION_IDENTITY || m_desc.Rotation == DXGI_MODE_ROTATION_UNSPECIFIED) {
                const auto *src =
                    static_cast<const uint8_t *>(mapped.pData) + (uint64_t(row) * h / m_height) * mapped.RowPitch;
                auto *dst = pixels + uint64_t(row) * m_width * 4;
                if (m_width == UINT(w))
                    memcpy(dst, src, m_width * 4);
                else
                    for (UINT col = 0; col < m_width; ++col)
                        memcpy(dst + col * 4, src + (uint64_t(col) * w / m_width) * 4, 4);
                continue;
            }
            for (UINT col = 0; col < m_width; ++col) {
                const auto p =
                    DesktopSurfacePoint(x + int(uint64_t(col) * w / m_width), y + int(uint64_t(row) * h / m_height),
                                        full.Width, full.Height, m_desc.Rotation);
                memcpy(pixels + (uint64_t(row) * m_width + col) * 4,
                       static_cast<const uint8_t *>(mapped.pData) + (p.y - box.top) * mapped.RowPitch +
                           (p.x - box.left) * 4,
                       4);
            }
        }
        m_context->Unmap(m_staging.get(), 0);
        DrawPointer(pixels, x, y, w, h);
        return S_OK;
    }

  private:
    void DrawPointer(uint8_t *pixels, int x, int y, int width, int height) {
        if (!m_pointer.Visible || m_shape.empty())
            return;
        const auto &s = m_shapeInfo;
        const UINT cursorHeight = s.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? s.Height / 2 : s.Height;
        if ((s.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME && s.Pitch < (s.Width + 7) / 8) ||
            (s.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME && s.Pitch < s.Width * 4))
            winrt::throw_hresult(E_BOUNDS);
        // Position is the top-left, not the hotspot. Composite only affected
        // output pixels, after downsampling; no extra full-size CPU frame.
        const int left = (std::max)(0, int(m_pointer.Position.x - x) * int(m_width) / width - 1);
        const int top = (std::max)(0, int(m_pointer.Position.y - y) * int(m_height) / height - 1);
        const int right =
            (std::min)(int(m_width), int(m_pointer.Position.x + int(s.Width) - x) * int(m_width) / width + 2);
        const int bottom =
            (std::min)(int(m_height), int(m_pointer.Position.y + int(cursorHeight) - y) * int(m_height) / height + 2);
        for (int row = top; row < bottom; ++row)
            for (int col = left; col < right; ++col) {
                const int px = x + int(uint64_t(col) * width / m_width) - m_pointer.Position.x;
                const int py = y + int(uint64_t(row) * height / m_height) - m_pointer.Position.y;
                if (px < 0 || py < 0 || px >= int(s.Width) || py >= int(cursorHeight))
                    continue;
                auto *dst = pixels + (uint64_t(row) * m_width + col) * 4;
                if (s.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                    const BYTE bit = BYTE(0x80 >> (px & 7));
                    const BYTE andMask = m_shape[py * s.Pitch + px / 8] & bit ? 255 : 0;
                    const BYTE xorMask = m_shape[(py + cursorHeight) * s.Pitch + px / 8] & bit ? 255 : 0;
                    for (int i = 0; i < 3; ++i)
                        dst[i] = (dst[i] & andMask) ^ xorMask;
                } else {
                    const auto *src = m_shape.data() + py * s.Pitch + px * 4;
                    for (int i = 0; i < 3; ++i)
                        dst[i] = s.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR
                                     ? (src[3] ? dst[i] ^ src[i] : src[i])
                                     : BYTE((UINT(src[i]) * src[3] + UINT(dst[i]) * (255 - src[3]) + 127) / 255);
                }
                dst[3] = 255;
            }
    }
    RECT m_region{};
    UINT m_width{}, m_height{};
    DXGI_OUTPUT_DESC m_desc{};
    DXGI_OUTDUPL_POINTER_POSITION m_pointer{};
    DXGI_OUTDUPL_POINTER_SHAPE_INFO m_shapeInfo{};
    std::vector<uint8_t> m_shape;
    winrt::com_ptr<ID3D11Device> m_device;
    winrt::com_ptr<ID3D11DeviceContext> m_context;
    winrt::com_ptr<IDXGIOutputDuplication> m_duplication;
    winrt::com_ptr<ID3D11Texture2D> m_staging;
    std::unique_ptr<GpuRegionReadback> m_gpu;
    bool m_gpuUnavailable = false;
};
} // namespace nskry
