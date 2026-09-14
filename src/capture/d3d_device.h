#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace nskry {

/// Owns a D3D11 device + immediate context and exposes a WinRT IDirect3DDevice
/// wrapper required by the WGC frame pool.  Provides helper factories for textures.
class D3DDevice {
public:
    D3DDevice();
    ~D3DDevice() = default;

    D3DDevice(const D3DDevice&)            = delete;
    D3DDevice& operator=(const D3DDevice&) = delete;

    [[nodiscard]] ID3D11Device*        Device()      const { return m_device.get();  }
    [[nodiscard]] ID3D11DeviceContext*  Context()     const { return m_context.get(); }
    [[nodiscard]] auto                 WinRTDevice() const { return m_winrtDevice;   }

    /// Create a BGRA texture with the given dimensions and flags.
    [[nodiscard]] winrt::com_ptr<ID3D11Texture2D> CreateTexture(
        UINT width, UINT height,
        D3D11_USAGE usage    = D3D11_USAGE_DEFAULT,
        UINT bindFlags       = 0,
        UINT miscFlags       = 0,
        DXGI_FORMAT format   = DXGI_FORMAT_B8G8R8A8_UNORM) const;

private:
    void InitD3D11();
    void WrapAsWinRT();

    winrt::com_ptr<ID3D11Device>        m_device;
    winrt::com_ptr<ID3D11DeviceContext>  m_context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };
};

} // namespace nskry
