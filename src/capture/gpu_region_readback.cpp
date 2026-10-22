#include "gpu_region_readback.h"
#include <cstring>
#include <d3dcompiler.h>
#include <string_view>

namespace nskry {
namespace {
constexpr char Shader[] = R"(
Texture2D<float4> source : register(t0);
RWByteAddressBuffer output : register(u0);
cbuffer Geometry : register(b0) {
    uint outputWidth, outputHeight, logicalWidth, logicalHeight;
    uint rotation, inputWidth, inputHeight, padding;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outputWidth || id.y >= outputHeight) return;
    uint x = id.x * logicalWidth / outputWidth;
    uint y = id.y * logicalHeight / outputHeight;
    uint2 pixelPosition = uint2(x, y);
    if (rotation == 2) pixelPosition = uint2(y, inputHeight - 1 - x);
    if (rotation == 3) pixelPosition = uint2(inputWidth - 1 - x, inputHeight - 1 - y);
    if (rotation == 4) pixelPosition = uint2(inputWidth - 1 - y, x);
    uint4 color = uint4(round(saturate(source.Load(int3(pixelPosition, 0))) * 255.0));
    output.Store((id.y * outputWidth + id.x) * 4,
                 color.b | (color.g << 8) | (color.r << 16) | (color.a << 24));
}
)";
}

GpuRegionReadback::GpuRegionReadback(ID3D11Device* device, ID3D11DeviceContext* context, UINT inputWidth,
                                   UINT inputHeight, UINT outputWidth, UINT outputHeight,
                                   DXGI_MODE_ROTATION rotation)
    : m_inputWidth(inputWidth), m_inputHeight(inputHeight), m_width(outputWidth), m_height(outputHeight) {
    if (!device || !context || !inputWidth || !inputHeight || inputWidth > 3840 || inputHeight > 3840 ||
        uint64_t(inputWidth) * inputHeight > 3840ULL * 2160 || !outputWidth || !outputHeight ||
        outputWidth > 1920 || outputHeight > 1920 || uint64_t(outputWidth) * outputHeight > 1920ULL * 1080 ||
        rotation > DXGI_MODE_ROTATION_ROTATE270)
        winrt::throw_hresult(E_INVALIDARG);
    m_context.copy_from(context);
    m_bytes = outputWidth * outputHeight * 4;
    winrt::com_ptr<ID3DBlob> code, error;
    const auto compiled = D3DCompile(Shader, sizeof(Shader) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0",
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), error.put());
    if (FAILED(compiled))
        throw winrt::hresult_error(compiled, error ? winrt::to_hstring(std::string_view(
            static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize())) : L"GPU scaling shader failed.");
    winrt::check_hresult(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                     m_shader.put()));
    const bool rotated = rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
    const UINT geometry[]{outputWidth, outputHeight, rotated ? inputHeight : inputWidth,
                           rotated ? inputWidth : inputHeight, UINT(rotation), inputWidth, inputHeight, 0};
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(geometry);
    constants.Usage = D3D11_USAGE_IMMUTABLE;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{geometry, 0, 0};
    winrt::check_hresult(device->CreateBuffer(&constants, &initial, m_constants.put()));
    D3D11_TEXTURE2D_DESC input{};
    input.Width = inputWidth;
    input.Height = inputHeight;
    input.MipLevels = input.ArraySize = input.SampleDesc.Count = 1;
    input.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    input.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    winrt::check_hresult(device->CreateTexture2D(&input, nullptr, m_input.put()));
    winrt::check_hresult(device->CreateShaderResourceView(m_input.get(), nullptr, m_view.put()));
    D3D11_BUFFER_DESC output{};
    output.ByteWidth = m_bytes;
    output.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    output.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    winrt::check_hresult(device->CreateBuffer(&output, nullptr, m_output.put()));
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = m_bytes / 4;
    view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    winrt::check_hresult(device->CreateUnorderedAccessView(m_output.get(), &view, m_outputView.put()));
    output.BindFlags = output.MiscFlags = 0;
    output.Usage = D3D11_USAGE_STAGING;
    output.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::check_hresult(device->CreateBuffer(&output, nullptr, m_readback.put()));
}

void GpuRegionReadback::Read(ID3D11Texture2D* source, const D3D11_BOX& crop, uint8_t* pixels, uint32_t capacity) {
    if (!source || !pixels || capacity < m_bytes || GetCurrentThreadId() != m_thread || crop.front || crop.back != 1 ||
        crop.right <= crop.left || crop.bottom <= crop.top || crop.right - crop.left != m_inputWidth ||
        crop.bottom - crop.top != m_inputHeight)
        winrt::throw_hresult(E_INVALIDARG);
    D3D11_TEXTURE2D_DESC actual{};
    source->GetDesc(&actual);
    if (crop.right > actual.Width || crop.bottom > actual.Height || actual.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        actual.SampleDesc.Count != 1)
        winrt::throw_hresult(E_BOUNDS);
    m_context->CopySubresourceRegion(m_input.get(), 0, 0, 0, 0, source, 0, &crop);
    auto* view = m_view.get();
    auto* output = m_outputView.get();
    auto* constants = m_constants.get();
    m_context->CSSetShader(m_shader.get(), nullptr, 0);
    m_context->CSSetShaderResources(0, 1, &view);
    m_context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    m_context->CSSetConstantBuffers(0, 1, &constants);
    m_context->Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);
    view = nullptr;
    output = nullptr;
    constants = nullptr;
    m_context->CSSetShaderResources(0, 1, &view);
    m_context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    m_context->CSSetConstantBuffers(0, 1, &constants);
    m_context->CSSetShader(nullptr, nullptr, 0);
    m_context->CopyResource(m_readback.get(), m_output.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(m_context->Map(m_readback.get(), 0, D3D11_MAP_READ, 0, &mapped));
    memcpy(pixels, mapped.pData, m_bytes);
    m_context->Unmap(m_readback.get(), 0);
}
} // namespace nskry
