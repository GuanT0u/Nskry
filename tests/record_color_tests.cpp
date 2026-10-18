#include "mf_encoder.h"
#include <filesystem>
#include <iostream>
#include <mfapi.h>
#include <mfidl.h>
using namespace nskry::record;
bool CheckColor(const std::filesystem::path &path, bool hardware, unsigned width, unsigned height) {
    {
        MfEncoder encoder;
        encoder.Open(path.wstring(), width, height, 20, hardware);
        std::vector<uint8_t> pixels(width * height * 4);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = 20;
            pixels[i + 1] = 30;
            pixels[i + 2] = 180;
            pixels[i + 3] = 255;
        }
        for (int i = 0; i < 2; ++i)
            encoder.Write(pixels.data(), i * 500000LL, 500000);
        encoder.Finalize();
    }
    winrt::com_ptr<IMFAttributes> attributes;
    MFCreateAttributes(attributes.put(), 1);
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    winrt::com_ptr<IMFSourceReader> reader;
    winrt::check_hresult(MFCreateSourceReaderFromURL(path.c_str(), attributes.get(), reader.put()));
    winrt::com_ptr<IMFMediaType> native;
    winrt::check_hresult(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, native.put()));
    UINT32 matrix = UINT32_MAX;
    native->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
    winrt::com_ptr<IMFMediaType> decoded;
    MFCreateMediaType(decoded.put());
    decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    winrt::check_hresult(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, decoded.get()));
    int count = 0;
    bool ok = matrix == MFVideoTransferMatrix_BT709;
    for (int safety = 0; safety < 20; ++safety) {
        DWORD flags{};
        winrt::com_ptr<IMFSample> sample;
        winrt::check_hresult(
            reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr, sample.put()));
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (!sample)
            continue;
        winrt::com_ptr<IMFMediaBuffer> buffer;
        winrt::check_hresult(sample->ConvertToContiguousBuffer(buffer.put()));
        BYTE *bytes{};
        DWORD length{};
        winrt::check_hresult(buffer->Lock(&bytes, nullptr, &length));
        if (length < width * height * 4) {
            ok = false;
        } else {
            const auto *pixel = bytes + (height / 2 * width + width / 2) * 4;
            std::cout << width << 'x' << height << " hardware=" << hardware << " matrix=" << matrix
                      << " BGR=" << int(pixel[0]) << ',' << int(pixel[1]) << ',' << int(pixel[2]) << '\n';
            ok = std::abs(int(pixel[0]) - 20) <= 8 && std::abs(int(pixel[1]) - 30) <= 8 &&
                 std::abs(int(pixel[2]) - 180) <= 8 && ok;
        }
        buffer->Unlock();
        ++count;
    }
    return ok && count == 2;
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::check_hresult(MFStartup(MF_VERSION));
    int result = 1;
    try {
        const auto root =
            std::filesystem::absolute(argv[1]) /
            (L"record-color-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directory(root);
        bool ok = true;
        for (const auto hardware : {true, false}) {
            const auto prefix = hardware ? L"hardware-" : L"software-";
            ok = CheckColor(root / (std::wstring(prefix) + L"hd.mp4"), hardware, 1920, 1080) && ok;
            ok = CheckColor(root / (std::wstring(prefix) + L"sd.mp4"), hardware, 320, 180) && ok;
        }
        result = ok ? 0 : 1;
    } catch (const winrt::hresult_error &e) {
        std::wcerr << e.message().c_str() << '\n';
    }
    MFShutdown();
    winrt::uninit_apartment();
    return result;
}
