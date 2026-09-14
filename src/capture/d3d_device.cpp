#include "pch.h"
#include "capture/d3d_device.h"

namespace nskry {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

D3DDevice::D3DDevice() {
    InitD3D11();
    WrapAsWinRT();
}

// ---------------------------------------------------------------------------
// D3D11 device creation (hardware-accelerated, BGRA support for WGC)
// ---------------------------------------------------------------------------

void D3DDevice::InitD3D11() {
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // required by WGC
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL actual{};
    winrt::check_hresult(::D3D11CreateDevice(
        nullptr,                        // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        m_device.put(),
        &actual,
        m_context.put()));
}

// ---------------------------------------------------------------------------
// Wrap native D3D11 device as WinRT IDirect3DDevice (for WGC FramePool)
// ---------------------------------------------------------------------------

void D3DDevice::WrapAsWinRT() {
    auto dxgi = m_device.as<IDXGIDevice>();

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(
        dxgi.get(), inspectable.put()));

    m_winrtDevice = inspectable.as<
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}

// ---------------------------------------------------------------------------
// Texture factory
// ---------------------------------------------------------------------------

winrt::com_ptr<ID3D11Texture2D> D3DDevice::CreateTexture(
    UINT width, UINT height,
    D3D11_USAGE usage, UINT bindFlags, UINT miscFlags,
    DXGI_FORMAT format) const
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = width;
    desc.Height           = height;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = format;
    desc.SampleDesc.Count = 1;
    desc.Usage            = usage;
    desc.BindFlags        = bindFlags;
    desc.MiscFlags        = miscFlags;

    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::check_hresult(m_device->CreateTexture2D(&desc, nullptr, tex.put()));
    return tex;
}

} // namespace nskry
