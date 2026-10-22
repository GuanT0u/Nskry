#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <winrt/base.h>

namespace nskry {
// Private capture-thread pipeline. Crop/nearest-scale/rotate before readback;
// all resources are bounded and retained only for one capture session.
class GpuRegionReadback {
  public:
    GpuRegionReadback(ID3D11Device* device, ID3D11DeviceContext* context, UINT inputWidth, UINT inputHeight,
                      UINT outputWidth, UINT outputHeight, DXGI_MODE_ROTATION rotation);
    void Read(ID3D11Texture2D* source, const D3D11_BOX& crop, uint8_t* pixels, uint32_t capacity);
    UINT ReadbackBytes() const { return m_bytes; }

  private:
    DWORD m_thread = GetCurrentThreadId();
    UINT m_inputWidth{}, m_inputHeight{}, m_width{}, m_height{}, m_bytes{};
    winrt::com_ptr<ID3D11DeviceContext> m_context;
    winrt::com_ptr<ID3D11ComputeShader> m_shader;
    winrt::com_ptr<ID3D11Buffer> m_constants, m_output, m_readback;
    winrt::com_ptr<ID3D11Texture2D> m_input;
    winrt::com_ptr<ID3D11ShaderResourceView> m_view;
    winrt::com_ptr<ID3D11UnorderedAccessView> m_outputView;
};
} // namespace nskry
