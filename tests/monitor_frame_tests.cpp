#include "pch.h"
#include "ui/pip_window.h"
#include <gdiplus.h>
#include <iostream>

namespace nskry {
struct MonitorFrameTestAccess {
    static bool Verify(PipWindow& window) {
        auto frame = window.GetLastFrameHdc();
        if (!frame.bits || !frame.hbmp) return false;
        const auto pixels = static_cast<const unsigned long*>(frame.bits);
        // Source shrank to 32x24; the original 80x60 monitor canvas is retained.
        for (unsigned y = 0; y < 60; ++y) for (unsigned x = 0; x < 80; ++x) {
            const unsigned long expected = x < 32 && y < 24 ? 0xff204060UL : 0;
            if (pixels[y * 80 + x] != expected) return false;
        }
        window.UpdateOverlayFromEngine(); // Redraw with the original canvas dimensions.
        return true;
    }
    static bool Empty(PipWindow& window) { return !window.m_lastFrameTex; }
};
}
int wmain() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token{};
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return 1;
    int result = 0;
    try {
        auto device = std::make_shared<nskry::D3DDevice>();
        std::vector<unsigned long> pixels(32 * 24, 0xff204060UL);
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = 32; desc.Height = 24;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        D3D11_SUBRESOURCE_DATA data{pixels.data(), 32 * 4, 0};
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(device->Device()->CreateTexture2D(&desc, &data, texture.put()));
        // Test-owned offscreen window: never activate or capture another app.
        nskry::PipWindow window(device, 80, 60, [] {}, {}, {}, {-20000,-20000,-19920,-19940});
        window.RenderFrame(texture.get(), 32, 24);
        if (!nskry::MonitorFrameTestAccess::Empty(window)) result = 1;
        window.Show();
        window.RenderFrame(texture.get(), 32, 24);
        if (!nskry::MonitorFrameTestAccess::Verify(window)) result = 1;
        window.SetAutoHidden(true);
        auto replacement = device->CreateTexture(80, 60);
        window.RenderFrame(replacement.get(), 80, 60);
        if (!nskry::MonitorFrameTestAccess::Verify(window)) result = 1;
        window.SetAutoHidden(false);
    } catch (const winrt::hresult_error& error) {
        std::cerr << "D3D/window integration failure: " << std::hex << error.code().value << '\n';
        result = 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; result = 1;
    }
    Gdiplus::GdiplusShutdown(token);
    winrt::uninit_apartment();
    return result;
}
