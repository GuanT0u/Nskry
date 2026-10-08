// Diagnostic only: independent OCR calls for a user-specified crop. Not the
// production pipeline, and not subject to its three-pass request budget.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 6) {
        std::cerr << "Usage: NskryOcrNativeProbe image.png [x y width height]\n";
        return 2;
    }
    ULONG_PTR token{};
    Gdiplus::GdiplusStartupInput startup;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) return 3;
    int status = 0;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        Gdiplus::Bitmap input(argv[1]);
        if (input.GetLastStatus() != Gdiplus::Ok) throw std::runtime_error("Cannot load input");
        const auto number = [](const wchar_t* value) {
            size_t consumed = 0;
            const int result = std::stoi(value, &consumed);
            if (value[consumed]) throw std::runtime_error("Invalid crop coordinate");
            return result;
        };
        const int x = argc == 6 ? number(argv[2]) : 0;
        const int y = argc == 6 ? number(argv[3]) : 0;
        const int w = argc == 6 ? number(argv[4]) : static_cast<int>(input.GetWidth());
        const int h = argc == 6 ? number(argv[5]) : static_cast<int>(input.GetHeight());
        if (x < 0 || y < 0 || w <= 0 || h <= 0 || w > input.GetWidth() || h > input.GetHeight() ||
            x > input.GetWidth() - w || y > input.GetHeight() - h)
            throw std::runtime_error("Crop outside input");
        auto engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromLanguage(
            winrt::Windows::Globalization::Language(L"zh-Hans"));
        if (!engine) throw std::runtime_error("Chinese Windows OCR not installed");
        Gdiplus::Color background;
        input.GetPixel(x, y, &background);
        for (int padding : { 0, 12 }) for (int scale : { 1, 2, 3, 4 }) for (int mode = 0; mode < 3; ++mode) {
            const int64_t wideWidth = (static_cast<int64_t>(w) + padding * 2) * scale;
            const int64_t wideHeight = (static_cast<int64_t>(h) + padding * 2) * scale;
            if (wideWidth > engine.MaxImageDimension() || wideHeight > engine.MaxImageDimension()) continue;
            const int width = static_cast<int>(wideWidth), height = static_cast<int>(wideHeight);
            const auto start = std::chrono::steady_clock::now();
            Gdiplus::Bitmap raster(width, height, PixelFormat32bppARGB);
            {
                Gdiplus::Graphics graphics(&raster);
                graphics.Clear(background);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                graphics.DrawImage(&input, Gdiplus::Rect(padding * scale, padding * scale, w * scale, h * scale),
                    x, y, w, h, Gdiplus::UnitPixel);
            }
            const auto count = static_cast<uint32_t>(width * height * 4);
            winrt::Windows::Storage::Streams::Buffer buffer(count);
            buffer.Length(count);
            uint8_t* data{};
            winrt::check_hresult(buffer.as<winrt::impl::IBufferByteAccess>()->Buffer(&data));
            Gdiplus::BitmapData locked{};
            Gdiplus::Rect bounds(0, 0, width, height);
            if (raster.LockBits(&bounds, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &locked) != Gdiplus::Ok)
                throw std::runtime_error("Cannot read raster");
            for (int row = 0; row < height; ++row) {
                const auto* source = static_cast<uint8_t*>(locked.Scan0) + row * locked.Stride;
                for (int col = 0; col < width; ++col) {
                    auto* p = data + (row * width + col) * 4;
                    std::copy_n(source + col * 4, 4, p);
                    if (mode) {
                        const int gray = (29 * p[0] + 150 * p[1] + 77 * p[2]) >> 8;
                        p[0] = p[1] = p[2] = static_cast<uint8_t>(mode == 2 ? 255 - gray : gray);
                    }
                    p[3] = 255;
                }
            }
            raster.UnlockBits(&locked);
            auto bitmap = winrt::Windows::Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(buffer,
                winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8, width, height,
                winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Ignore);
            const auto result = engine.RecognizeAsync(bitmap).get();
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::cout << "crop=" << x << ',' << y << ',' << w << ',' << h << " padding=" << padding
                << " scale=" << scale << " mode=" << mode << " total_ms=" << ms << '\n';
            for (const auto& line : result.Lines()) std::cout << winrt::to_string(line.Text()) << '\n';
        }
    } catch (const winrt::hresult_error& e) {
        std::cerr << winrt::to_string(e.message()) << '\n'; status = 4;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; status = 4; }
    Gdiplus::GdiplusShutdown(token);
    return status;
}
