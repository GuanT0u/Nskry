#include "gif_encoder.h"
#include "mf_encoder.h"
#include "record_transcode.h"
#include "record_worker_protocol.h"
#include <memory>

using namespace nskry::record;
int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    HANDLE mapping{}, request{}, response{};
    if (argc != 4 || !wire::ParseHandle(argv[1], mapping) || !wire::ParseHandle(argv[2], request) ||
        !wire::ParseHandle(argv[3], response))
        return 2;
    struct Handles {
        HANDLE a, b, c;
        ~Handles() {
            CloseHandle(a);
            CloseHandle(b);
            CloseHandle(c);
        }
    } handles{mapping, request, response};
    struct View {
        void *value{};
        ~View() {
            if (value)
                UnmapViewOfFile(value);
        }
    } header{MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(wire::Header))};
    if (!header.value || !wire::ValidHeader(*static_cast<wire::Header *>(header.value)))
        return 3;
    // Copy only immutable configuration, not the 64 KiB transport header onto
    // the native thread stack. The path's bounded storage is heap-owned.
    struct Configuration {
        uint32_t width, height, fps, pixelBytes, hardware, bitrate, gif, preview, sourceWidth, sourceHeight;
        std::wstring path;
    };
    const auto *input = static_cast<const wire::Header *>(header.value);
    const Configuration initial{input->width,       input->height,       input->fps, input->pixelBytes,
                                input->hardware,    input->bitrate,      input->gif, input->preview,
                                input->sourceWidth, input->sourceHeight, input->path};
    View memory{MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(wire::Header) + initial.pixelBytes)};
    if (!memory.value)
        return 4;
    auto &h = *static_cast<wire::Header *>(memory.value);
    bool apartment = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
        std::unique_ptr<MfEncoder> encoder;
        std::unique_ptr<GifEncoder> gif;
        std::unique_ptr<PreviewDecoder> preview;
        uint64_t sequence = 0;
        while (WaitForSingleObject(request, INFINITE) == WAIT_OBJECT_0) {
            MemoryBarrier();
            const auto command = h.command;
            const auto number = h.request;
            HRESULT result = S_OK;
            try {
                if (number != sequence + 1 || !wire::ValidHeader(h) || h.width != initial.width ||
                    h.height != initial.height || h.pixelBytes != initial.pixelBytes || h.fps != initial.fps)
                    winrt::throw_hresult(E_INVALIDARG);
                sequence = number;
                if (command == wire::Command::Open) {
                    if (encoder || gif)
                        winrt::throw_hresult(E_UNEXPECTED);
                    if (initial.preview) {
                        wcscpy_s(h.backend, L"On-demand frame preview");
                    } else if (initial.gif) {
                        gif = std::make_unique<GifEncoder>();
                        gif->Open(initial.path, initial.width, initial.height, &h.created);
                        wcscpy_s(h.backend, L"GIF / adaptive 256 colors / 10 fps");
                    } else {
                        encoder = std::make_unique<MfEncoder>();
                        encoder->Open(initial.path, initial.width, initial.height, initial.fps, initial.hardware != 0,
                                      &h.created, initial.bitrate);
                        wcsncpy_s(h.backend, encoder->Backend().c_str(), _TRUNCATE);
                    }
                } else if (command == wire::Command::Close) {
                    preview.reset();
                    encoder.reset();
                    gif.reset();
                } else {
                    if (initial.preview) {
                        if (command != wire::Command::Preview || h.time < 0 || h.time > INT64_MAX - 1000000)
                            winrt::throw_hresult(E_INVALIDARG);
                        if (!preview)
                            preview = std::make_unique<PreviewDecoder>(initial.path, initial.gif != 0,
                                                                       initial.sourceWidth, initial.sourceHeight);
                        preview->Read(h.time, initial.width, initial.height,
                                      reinterpret_cast<uint8_t *>(&h + 1));
                    } else if (!encoder && !gif)
                        winrt::throw_hresult(E_UNEXPECTED);
                    else if (command == wire::Command::Trim) {
                        if (!h.inputPath[0] || h.inputPath[32767] || h.trimStart < 0 || h.trimEnd <= h.trimStart)
                            winrt::throw_hresult(E_INVALIDARG);
                        const std::wstring inputPath = h.inputPath;
                        TrimRecording(inputPath, initial.gif != 0, initial.width, initial.height, h.trimStart,
                                      h.trimEnd, [&](const uint8_t *pixels, int64_t time, int64_t duration) {
                                          if (gif)
                                              gif->Write(pixels, time, duration);
                                          else
                                              encoder->Write(pixels, time, duration);
                                          ++h.samples;
                                      });
                    } else if (command == wire::Command::Frame) {
#if defined(NSKRY_RECORD_TEST_CRASH_WORKER)
                        ExitProcess(99);
#elif defined(NSKRY_RECORD_TEST_HANG_WORKER)
                        Sleep(INFINITE);
#endif
                        if (gif)
                            gif->Write(reinterpret_cast<const uint8_t *>(&h + 1), h.time, h.duration);
                        else
                            encoder->WriteNv12(reinterpret_cast<const uint8_t *>(&h + 1), h.time, h.duration);
                    } else if (command == wire::Command::Finalize) {
#if defined(NSKRY_RECORD_TEST_FINALIZE_HANG_WORKER)
                        Sleep(INFINITE);
#endif
                        if (gif)
                            gif->Finalize();
                        else
                            encoder->Finalize();
                        InterlockedExchange(&h.finalized, 1);
                    } else
                        winrt::throw_hresult(E_INVALIDARG);
                }
            } catch (const winrt::hresult_error &e) {
                result = e.code();
                wcsncpy_s(h.error, e.message().c_str(), _TRUNCATE);
            } catch (...) {
                result = E_FAIL;
                wcscpy_s(h.error, L"Recording encoder allocation or system error.");
            }
            h.result = result;
            h.response = number;
            MemoryBarrier();
            if (!SetEvent(response))
                break;
            if (command == wire::Command::Close)
                break;
        }
    } catch (...) {
        if (apartment)
            winrt::uninit_apartment();
        return 5;
    }
    if (apartment)
        winrt::uninit_apartment();
    return 0;
}
