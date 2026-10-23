#include "capture/gpu_region_readback.h"
#include "capture/d3d_device.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>
#include <psapi.h>

namespace {
using Clock = std::chrono::steady_clock;
double Milliseconds(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}
uint64_t CpuTime() {
    FILETIME c{}, e{}, k{}, u{};
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
           (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
}
bool DeviceCost() {
    const auto memory = [](const char *label) {
        PROCESS_MEMORY_COUNTERS_EX value{};
        value.cb = sizeof(value);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&value),
                                  sizeof(value)))
            winrt::throw_last_error();
        std::cout << label << " private_bytes=" << value.PrivateUsage << " working_bytes=" << value.WorkingSetSize
                  << '\n';
    };
    memory("before_device");
    const auto cpu = CpuTime();
    const auto start = Clock::now();
    {
        nskry::D3DDevice device;
        std::cout << "device_init_ms=" << Milliseconds(start) << " device_init_cpu_ms="
                  << (CpuTime() - cpu) / 10000.0 << '\n';
        memory("device_live");
        if (!device.Device() || !device.Context() || !device.WinRTDevice())
            return false;
    }
    memory("device_released");
    return true;
}
struct Device {
    winrt::com_ptr<ID3D11Device> value;
    winrt::com_ptr<ID3D11DeviceContext> context;
    explicit Device(D3D_DRIVER_TYPE type) {
        winrt::check_hresult(D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                               D3D11_SDK_VERSION, value.put(), nullptr, context.put()));
    }
    winrt::com_ptr<ID3D11Texture2D> Texture(UINT w, UINT h, const uint32_t* pixels) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        D3D11_SUBRESOURCE_DATA initial{pixels, w * 4, 0};
        winrt::com_ptr<ID3D11Texture2D> result;
        winrt::check_hresult(value->CreateTexture2D(&desc, &initial, result.put()));
        return result;
    }
};
bool Verify(Device& device) {
    constexpr UINT width = 24, height = 18;
    std::vector<uint32_t> pixels(width * height);
    const D3D11_BOX crop{3, 2, 0, 21, 16, 1};
    bool ok = true;
    for (auto rotation : {DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE90,
                          DXGI_MODE_ROTATION_ROTATE180, DXGI_MODE_ROTATION_ROTATE270}) {
        const bool rotated = rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
        const UINT logicalWidth = rotated ? 14 : 18, logicalHeight = rotated ? 18 : 14;
        for (const UINT divisor : {2u, 3u}) {
            const UINT w = logicalWidth / divisor, h = logicalHeight / divisor;
            nskry::GpuRegionReadback reader(device.value.get(), device.context.get(), 18, 14, w, h, rotation);
            for (UINT scene = 0; scene < 2; ++scene) {
                for (UINT i = 0; i < pixels.size(); ++i)
                    pixels[i] = ((i * 37 + scene) & 255) | (((i * 19 + 29) & 255) << 8) |
                                (((i * 23 + scene * 41) & 255) << 16) | (((i * 11 + 7) & 255) << 24);
                auto source = device.Texture(width, height, pixels.data());
                std::vector<uint32_t> output(w * h + 4, 0xdeadbeef);
                reader.Read(source.get(), crop, reinterpret_cast<uint8_t*>(output.data()), UINT(output.size() * 4));
                ok &= reader.ReadbackBytes() == w * h * 4;
                for (UINT y = 0; y < h; ++y)
                    for (UINT x = 0; x < w; ++x) {
                        const UINT sx = x * logicalWidth / w, sy = y * logicalHeight / h;
                        UINT px = sx, py = sy;
                        if (rotation == DXGI_MODE_ROTATION_ROTATE90) { px = sy; py = 13 - sx; }
                        if (rotation == DXGI_MODE_ROTATION_ROTATE180) { px = 17 - sx; py = 13 - sy; }
                        if (rotation == DXGI_MODE_ROTATION_ROTATE270) { px = 17 - sy; py = sx; }
                        ok &= output[y * w + x] == pixels[(py + crop.top) * width + px + crop.left];
                    }
                for (UINT i = w * h; i < output.size(); ++i)
                    ok &= output[i] == 0xdeadbeef;
                try {
                    reader.Read(source.get(), crop, reinterpret_cast<uint8_t*>(output.data()), w * h * 4 - 1);
                    ok = false;
                } catch (const winrt::hresult_error& error) {
                    ok &= error.code() == E_INVALIDARG;
                }
            }
        }
    }
    return ok;
}
bool Benchmark(Device& device) {
    auto dxgi = device.value.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
    DXGI_ADAPTER_DESC adapterInfo{};
    winrt::check_hresult(adapter->GetDesc(&adapterInfo));
    std::cout << "adapter=" << winrt::to_string(std::wstring_view(adapterInfo.Description)) << '\n';
    constexpr UINT width = 3840, height = 2160, w = 1920, h = 1080, iterations = 30;
    std::vector<uint32_t> input(width * height), cpu(w * h), gpu(w * h);
    for (UINT i = 0; i < input.size(); ++i)
        input[i] = (i * 751 & 0xffffff) | 0xff000000;
    auto source = device.Texture(width, height, input.data());
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(device.value->CreateTexture2D(&desc, nullptr, staging.put()));
    const auto initialization = Clock::now();
    nskry::GpuRegionReadback reader(device.value.get(), device.context.get(), width, height, w, h,
                                   DXGI_MODE_ROTATION_IDENTITY);
    std::cout << "gpu_init_ms=" << Milliseconds(initialization) << '\n';
    auto oldRead = [&] {
        device.context->CopyResource(staging.get(), source.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(device.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        for (UINT row = 0; row < h; ++row) {
            const auto* src = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) +
                                                               uint64_t(row) * height / h * mapped.RowPitch);
            for (UINT col = 0; col < w; ++col)
                cpu[row * w + col] = src[uint64_t(col) * width / w];
        }
        device.context->Unmap(staging.get(), 0);
    };
    auto newRead = [&] {
        reader.Read(source.get(), {0, 0, 0, width, height, 1}, reinterpret_cast<uint8_t*>(gpu.data()), w * h * 4);
    };
    for (int i = 0; i < 3; ++i) { oldRead(); newRead(); }
    auto measure = [&](auto read, const char* label) {
        const auto cpuBefore = CpuTime();
        const auto start = Clock::now();
        for (UINT i = 0; i < iterations; ++i) read();
        std::cout << label << " wall_ms=" << Milliseconds(start) << " process_cpu_ms="
                  << (CpuTime() - cpuBefore) / 10000.0 << '\n';
    };
    measure(oldRead, "cpu_resize");
    measure(newRead, "gpu_resize");
    std::cout << "iterations=" << iterations << " old_readback_bytes=" << width * height * 4
              << " new_readback_bytes=" << reader.ReadbackBytes() << " byte_equal=" << (cpu == gpu) << '\n';
    return cpu == gpu;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--device-cost") {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            const bool ok = DeviceCost();
            winrt::uninit_apartment();
            return ok ? 0 : 1;
        }
        if (argc == 2 && std::string(argv[1]) == "--benchmark") {
            Device hardware(D3D_DRIVER_TYPE_HARDWARE);
            return Benchmark(hardware) ? 0 : 1;
        }
        Device warp(D3D_DRIVER_TYPE_WARP);
        const bool ok = Verify(warp);
        std::cout << "GPU crop/scale/rotation, BGRA bytes, source reuse and output bounds=" << ok << '\n';
        return ok ? 0 : 1;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "D3D/readback failure=0x" << std::hex << error.code().value << ' '
                  << winrt::to_string(error.message()) << '\n';
        return 1;
    }
}
