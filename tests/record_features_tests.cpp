#include "isolated_encoder.h"
#include "record_export.h"
#include "ui/toolbar_layout.h"
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
    return count == expected && time == duration;
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
                for (UINT i = 0; i < fps; ++i) {
                    const auto first = int64_t(i) * 10000000 / fps, last = int64_t(i + 1) * 10000000 / fps;
                    encoder.Write(pixels.data(), first, last - first);
                }
                encoder.Finalize();
                ok = Check(encoder.Close(), "encoder exits") && ok;
            }
            ok = Check(gif ? Gif(path, 10, 10000000) : Video(path, 10000000, fps), "new format decodes") && ok;
            Snapshot snapshot;
            snapshot.width = 160;
            snapshot.height = 90;
            snapshot.fps = fps;
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
