#include "capture/desktop_region.h"
#include "gif_palette.h"
#include "isolated_encoder.h"
#include "record_export.h"
#include "record_preview.h"
#include "record_ui_policy.h"
#include "ui/toolbar_layout.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <winrt/base.h>
using namespace nskry::record;
namespace {
std::atomic<unsigned> captures{};
HRESULT __cdecl CaptureOpen(const NskryCaptureRequestV2 *, void **handle, NskryCaptureDescriptionV1 *d, wchar_t *,
                            uint32_t) {
    *handle = new int(1);
    *d = {sizeof(*d), 1, 160, 90, 640};
    ++captures;
    return S_OK;
}
HRESULT __cdecl CaptureRead(void *, uint8_t *pixels, uint32_t size) {
    for (size_t i = 0; i + 3 < size; i += 4) {
        pixels[i] = 20;
        pixels[i + 1] = 30;
        pixels[i + 2] = 200;
        pixels[i + 3] = 255;
    }
    return S_OK;
}
HRESULT __cdecl CaptureBounds(void *, RECT *r) {
    *r = {0, 0, 160, 90};
    return S_OK;
}
void __cdecl CaptureClose(void *h) {
    delete static_cast<int *>(h);
    --captures;
}
bool Check(bool value, const char *name) {
    std::cout << (value ? "PASS " : "FAIL ") << name << '\n';
    return value;
}
void Message(const std::wstring &text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), utf8.data(), size, nullptr, nullptr);
    std::cout << utf8 << '\n';
}
bool Gif(const std::filesystem::path &path, unsigned expected, int64_t duration) {
    auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::check_hresult(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnDemand, decoder.put()));
    UINT count{};
    winrt::check_hresult(decoder->GetFrameCount(&count));
    int64_t time = 0;
    for (UINT i = 0; i < count; ++i) {
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        winrt::check_hresult(decoder->GetFrame(i, frame.put()));
        winrt::com_ptr<IWICMetadataQueryReader> metadata;
        winrt::check_hresult(frame->GetMetadataQueryReader(metadata.put()));
        PROPVARIANT delay{};
        winrt::check_hresult(metadata->GetMetadataByName(L"/grctlext/Delay", &delay));
        if (delay.vt != VT_UI2 || !delay.uiVal) {
            PropVariantClear(&delay);
            return false;
        }
        time += int64_t(delay.uiVal) * 100000;
        PropVariantClear(&delay);
        winrt::com_ptr<IWICFormatConverter> converter;
        factory->CreateFormatConverter(converter.put());
        winrt::check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                                   nullptr, 0, WICBitmapPaletteTypeCustom));
        std::vector<uint8_t> pixels(160 * 90 * 4);
        winrt::check_hresult(converter->CopyPixels(nullptr, 640, UINT(pixels.size()), pixels.data()));
        if (pixels[2] < 150 || pixels[1] > 100)
            return false;
    }
    std::cout << "GIF frames=" << count << " duration100ns=" << time << '\n';
    return count == expected && std::abs(time - duration) <= 100000;
}
bool Video(const std::filesystem::path &path, int64_t expected, UINT fps) {
    winrt::com_ptr<IMFSourceReader> reader;
    winrt::check_hresult(MFCreateSourceReaderFromURL(path.c_str(), nullptr, reader.put()));
    PROPVARIANT duration{};
    winrt::check_hresult(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration));
    const auto length = int64_t(duration.uhVal.QuadPart);
    PropVariantClear(&duration);
    winrt::com_ptr<IMFMediaType> type;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, type.put());
    UINT n{}, d{};
    winrt::check_hresult(MFGetAttributeRatio(type.get(), MF_MT_FRAME_RATE, &n, &d));
    winrt::com_ptr<IMFMediaType> decoded;
    MFCreateMediaType(decoded.put());
    decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    winrt::check_hresult(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, decoded.get()));
    unsigned frames = 0;
    int64_t previous = -1;
    while (true) {
        DWORD flags{};
        LONGLONG stamp{};
        winrt::com_ptr<IMFSample> sample;
        winrt::check_hresult(
            reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &stamp, sample.put()));
        if (sample) {
            if (stamp <= previous)
                return false;
            previous = stamp;
            ++frames;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
    }
    std::cout << "MP4 frames=" << frames << " duration100ns=" << length << " fps=" << n << '/' << d << '\n';
    return frames > 5 && std::abs(length - expected) < 100000 && n == fps * d;
}
bool AdaptiveColors(const std::filesystem::path &root, bool complex = false) {
    constexpr std::array<std::array<uint8_t, 3>, 8> colors{{{31, 31, 31},
                                                            {49, 40, 39},
                                                            {23, 22, 35},
                                                            {95, 97, 96},
                                                            {180, 40, 25},
                                                            {30, 210, 220},
                                                            {230, 230, 235},
                                                            {6, 7, 8}}};
    std::vector<uint8_t> pixels(160 * 90 * 4);
    const auto path = root / (complex ? L"adaptive-gradient.gif" : L"adaptive-colors.gif");
    {
        IsolatedEncoder encoder;
        encoder.Open(path.wstring(), 160, 90, 10, false, 0, true);
        for (int frame = 0; frame < 2; ++frame) {
            for (int y = 0; y < 90; ++y)
                for (int x = 0; x < 160; ++x) {
                    const auto &c = colors[(x / 20 + frame) % 8];
                    auto *p = pixels.data() + (y * 160 + x) * 4;
                    p[0] = c[0];
                    p[1] = c[1];
                    p[2] = c[2];
                    p[3] = 255;
                    if (complex && y > 50) {
                        p[0] = uint8_t(x * 255 / 159);
                        p[1] = uint8_t((y - 51) * 255 / 38);
                        p[2] = uint8_t(x + y);
                    }
                }
            encoder.Write(pixels.data(), frame * 1000000LL, 1000000);
        }
        encoder.Finalize();
        encoder.Close();
    }
    auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::check_hresult(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnDemand, decoder.put()));
    int error = 0;
    for (UINT f = 0; f < 2; ++f) {
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        winrt::check_hresult(decoder->GetFrame(f, frame.put()));
        winrt::com_ptr<IWICFormatConverter> converter;
        factory->CreateFormatConverter(converter.put());
        winrt::check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                                   nullptr, 0, WICBitmapPaletteTypeCustom));
        winrt::check_hresult(converter->CopyPixels(nullptr, 640, UINT(pixels.size()), pixels.data()));
        for (int i = 0; i < 8; ++i)
            for (int c = 0; c < 3; ++c)
                if (!complex || (i + f) % 8 == 0)
                    error = (std::max)(error, std::abs(int(pixels[(45 * 160 + i * 20 + 10) * 4 + c]) -
                                                       colors[(i + f) % 8][c]));
    }
    std::cout << "Adaptive GIF max channel error=" << error << '\n';
    return error <= 6;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    const auto root = std::filesystem::path(argv[1]) / (L"record-features-" + std::to_wstring(GetCurrentProcessId()) +
                                                        L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(root);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    MFStartup(MF_VERSION);
    bool ok = true;
    try {
        const auto layout =
            nskry::LayoutSelectionToolbar(std::vector<int>(25, 76), {0, 0, 800, 600}, {0, 0, 800, 600}, 20);
        for (auto r : layout)
            ok = Check(r.left >= 4 && r.top >= 4 && r.right <= 796 && r.bottom <= 596,
                       "wrapped toolbar inside fullscreen selection") &&
                 ok;
        std::vector<uint8_t> pixels(160 * 90 * 4);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = 20;
            pixels[i + 1] = 30;
            pixels[i + 2] = 200;
            pixels[i + 3] = 255;
        }
        for (bool gif : {false, true}) {
            const auto path = root / (gif ? L"source.gif" : L"source.mp4");
            const auto trimmed = root / (gif ? L"trim.gif" : L"trim.mp4");
            const UINT fps = gif ? 10 : 60;
            {
                IsolatedEncoder encoder;
                encoder.Open(path.wstring(), 160, 90, fps, false, gif ? 0 : 2000000, gif);
                auto alternate = pixels;
                for (size_t i = 0; i < alternate.size(); i += 4) {
                    alternate[i] = 200;
                    alternate[i + 2] = 20;
                }
                for (UINT i = 0; i < fps; ++i) {
                    const auto first = int64_t(i) * 10000000 / fps, last = int64_t(i + 1) * 10000000 / fps;
                    encoder.Write(!gif && i >= fps / 2 ? alternate.data() : pixels.data(), first, last - first);
                }
                encoder.Finalize();
                ok = Check(encoder.Close(), "encoder exits") && ok;
            }
            ok = Check(gif ? Gif(path, 10, 10000000) : Video(path, 10000000, fps), "new format decodes") && ok;
            const auto before = std::filesystem::file_size(path);
            {
                PreviewTask preview(path.wstring(), 160, 90, gif);
                for (int i = 0; i < 50; ++i)
                    preview.Request(i * 8000000LL / 49);
                preview.Finish();
                const auto until = GetTickCount64() + 8000;
                PreviewFrame image;
                bool previewDone = false;
                bool hasPreview = false;
                while (!previewDone && GetTickCount64() < until) {
                    hasPreview = preview.Take(image, previewDone) || hasPreview;
                    WaitForSingleObject(GetCurrentProcess(), 10);
                }
                ok = Check(previewDone && hasPreview && image.error.empty() && image.time == 8000000 &&
                               image.pixels.size() == 160 * 90 * 4 &&
                               (gif ? image.pixels[2] > 150 : image.pixels[0] > 150),
                           "latest-only preview chooses correct scene and releases decoder") &&
                     ok;
            }
            ok = Check(std::filesystem::file_size(path) == before, "preview never overwrites its input") && ok;
            {
                PreviewTask preview(path.wstring(), 160, 90, gif);
                preview.Request(0);
                preview.Cancel();
                const auto until = GetTickCount64() + 5000;
                while (!preview.Done() && GetTickCount64() < until)
                    WaitForSingleObject(GetCurrentProcess(), 10);
                ok = Check(preview.Done(), "cancel preview releases child and thread work") && ok;
            }
            Snapshot snapshot;
            snapshot.width = 160;
            snapshot.height = 90;
            snapshot.fps = fps;
            snapshot.bitrate = gif ? 0 : 2000000;
            snapshot.encodedDuration = 10000000;
            {
                ExportTask task;
                task.Start(path.wstring(), trimmed.wstring(), snapshot, gif, 2000000, 8000000, true);
                const auto until = GetTickCount64() + 12000;
                while (!task.Status().done && GetTickCount64() < until)
                    WaitForSingleObject(GetCurrentProcess(), 10);
                auto status = task.Status();
                Message(status.message);
                ok = Check(status.done && status.success, "on-demand trim finishes") && ok;
            }
            ok = Check(gif ? Gif(trimmed, 6, 6000000) : Video(trimmed, 6000000, fps),
                       "trim output decodes and has correct duration") &&
                 ok;
            const auto saved = root / (gif ? L"saved.gif" : L"saved.mp4");
            {
                ExportTask task;
                task.Start(trimmed.wstring(), saved.wstring(), snapshot, gif, 0, 0, false);
                const auto until = GetTickCount64() + 5000;
                while (!task.Status().done && GetTickCount64() < until)
                    WaitForSingleObject(GetCurrentProcess(), 10);
                ok = Check(task.Status().success &&
                               std::filesystem::file_size(saved) == std::filesystem::file_size(trimmed),
                           "background file save") &&
                     ok;
            }
            {
                ExportTask task;
                task.Cancel();
                task.Start(path.wstring(), (root / L"cancelled.mp4").wstring(), snapshot, gif, 0, 0, false);
                const auto until = GetTickCount64() + 5000;
                while (!task.Status().done && GetTickCount64() < until)
                    WaitForSingleObject(GetCurrentProcess(), 10);
                ok = Check(task.Status().done && !task.Status().success &&
                               !std::filesystem::exists(root / L"cancelled.mp4"),
                           "cancel export retains source") &&
                     ok;
            }
        }
        bool rotations = true;
        for (auto rotation : {DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180,
                              DXGI_MODE_ROTATION_ROTATE270}) {
            const bool rotated = rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
            std::vector<bool> seen(24);
            for (int y = 0; y < (rotated ? 6 : 4); ++y)
                for (int x = 0; x < (rotated ? 4 : 6); ++x) {
                    auto p = nskry::DesktopSurfacePoint(x, y, 6, 4, rotation);
                    rotations = rotations && p.x >= 0 && p.x < 6 && p.y >= 0 && p.y < 4 && !seen[p.y * 6 + p.x];
                    seen[p.y * 6 + p.x] = true;
                }
        }
        ok = Check(rotations, "rotation mapping is bounded and bijective") && ok;
        ok = Check(AdaptiveColors(root), "adaptive GIF preserves dark/neutral colors across local palettes") && ok;
        ok = Check(AdaptiveColors(root, true), "adaptive GIF fallback preserves grays in gradient scenes") && ok;
        std::vector<uint8_t> unique(257 * 4), indices(257);
        for (unsigned i = 0; i < 257; ++i) {
            unique[i * 4] = uint8_t(i);
            unique[i * 4 + 1] = uint8_t(i >> 8);
            unique[i * 4 + 3] = 255;
        }
        std::array<uint32_t, 256> palette{};
        uint32_t count{};
        ok = Check(!ExactGifPalette(unique.data(), 257, indices.data(), palette, count),
                   "exact GIF palette defers at color 257") &&
             ok;
        auto bar = RecordingBarBounds({50, 40, 350, 220}, {0, 0, 1920, 1080}, 800, 52);
        ok = Check(bar.top == 228 && bar.left >= 4, "bar prefers selection bottom") && ok;
        bar = RecordingBarBounds({0, 0, 800, 600}, {0, 0, 800, 600}, 900, 52);
        ok = Check(bar.left >= 4 && bar.top >= 4 && bar.right <= 796 && bar.bottom <= 596,
                   "fullscreen bar stays inside work area") &&
             ok;
        ok = Check(TrimTimeAt(-1, 10, 110, 1000) == 0 && TrimTimeAt(60, 10, 110, 1000) == 500 &&
                       TrimTimeAt(999, 10, 110, 1000) == 1000,
                   "trim timeline mapping and bounds") &&
             ok;
        const auto compact = SetupWidths(303);
        const auto veryCompact = SetupWidths(248);
        const auto expanded = SetupWidths(900);
        ok = Check(compact.compact && compact.Required() <= 303 && !expanded.compact && expanded.Required() <= 900,
                   "record setup controls fit compact and normal bars") &&
             ok;
        ok = Check(veryCompact.compact && veryCompact.Required() <= 248,
                   "record setup controls contract for narrow work areas") &&
             ok;
        SessionOptions options;
        options.fps = 10;
        options.gif = true;
        options.capture = {sizeof(NskryCaptureServiceV2), 2, CaptureOpen, CaptureRead, CaptureBounds, CaptureClose};
        const auto sessionPath = root / L"session.gif";
        RecordSession animation({}, {0, 0, 160, 90}, sessionPath.wstring(), true, options);
        ok = Check(animation.Start(), "GIF recording session starts") && ok;
        const auto readyUntil = GetTickCount64() + 5000;
        while (animation.Status().state == State::Starting && GetTickCount64() < readyUntil)
            WaitForSingleObject(GetCurrentProcess(), 10);
        WaitForSingleObject(GetCurrentProcess(), 500);
        ok = Check(animation.PauseResume(), "GIF session pauses") && ok;
        const auto pauseUntil = GetTickCount64() + 2000;
        while (captures && GetTickCount64() < pauseUntil)
            WaitForSingleObject(GetCurrentProcess(), 10);
        ok = Check(!captures, "GIF pause releases capture") && ok;
        ok = Check(animation.PauseResume(), "GIF session resumes") && ok;
        WaitForSingleObject(GetCurrentProcess(), 400);
        ok = Check(animation.Stop(false), "GIF session stops") && ok;
        const auto stoppedUntil = GetTickCount64() + 5000;
        while (animation.Status().state != State::Done && animation.Status().state != State::Failed &&
               GetTickCount64() < stoppedUntil)
            WaitForSingleObject(GetCurrentProcess(), 10);
        const auto report = animation.Status();
        animation.Shutdown();
        Message(report.message);
        ok = Check(report.state == State::Done && !captures && report.frames > 5,
                   "GIF session finalizes and releases capture/worker") &&
             ok;
        ok = Check(Gif(sessionPath, unsigned(report.frames), (report.duration + 50000) / 100000 * 100000),
                   "GIF session animation/pause duration decodes") &&
             ok;
    } catch (const winrt::hresult_error &e) {
        Message(e.message().c_str());
        std::cout << "HRESULT=" << std::hex << e.code().value << '\n';
        ok = false;
    }
    MFShutdown();
    winrt::uninit_apartment();
    std::wcout << L"Artifacts: " << root.wstring() << '\n';
    return ok ? 0 : 1;
}
