#include "mf_encoder.h"
#include "test_artifacts.h"
#include <filesystem>
#include <iostream>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const nskry::test::Artifacts artifacts(argv[1], L"record-encoder");
    const auto path = artifacts.Path() / L"record.mp4";
    int result = 0;
    try {
        {
            nskry::record::MfEncoder encoder;
            encoder.Open(path.wstring(), 320, 180, 20);
            std::vector<uint8_t> pixels(320 * 180 * 4);
            for (int frame = 0; frame < 40; ++frame) {
                for (size_t p = 0; p < pixels.size(); p += 4) {
                    pixels[p] = 20;
                    pixels[p + 1] = uint8_t(frame * 5);
                    pixels[p + 2] = 180;
                    pixels[p + 3] = 255;
                }
                encoder.Write(pixels.data(), frame * 500000LL, 500000);
            }
            encoder.Finalize();
            std::wcout << L"Encoder: " << encoder.Backend() << L"\n";
        }
        winrt::check_hresult(MFStartup(MF_VERSION));
        {
            winrt::com_ptr<IMFSourceReader> reader;
            winrt::com_ptr<IMFAttributes> attributes;
            MFCreateAttributes(attributes.put(), 1);
            attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            winrt::check_hresult(MFCreateSourceReaderFromURL(path.c_str(), attributes.get(), reader.put()));
            PROPVARIANT duration{};
            winrt::check_hresult(
                reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration));
            std::cout << "MP4 duration100ns=" << duration.uhVal.QuadPart << '\n';
            if (duration.vt != VT_UI8 || duration.uhVal.QuadPart < 19900000 || duration.uhVal.QuadPart > 20100000)
                result = 3;
            PropVariantClear(&duration);
            winrt::com_ptr<IMFMediaType> type;
            winrt::check_hresult(MFCreateMediaType(type.put()));
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            winrt::check_hresult(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.get()));
            DWORD count = 0;
            LONGLONG previous = -1;
            for (int safety = 0; safety < 100; ++safety) {
                DWORD flags{};
                LONGLONG timestamp{};
                winrt::com_ptr<IMFSample> sample;
                winrt::check_hresult(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags,
                                                        &timestamp, sample.put()));
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                    break;
                if (sample) {
                    if (timestamp <= previous)
                        result = 4;
                    previous = timestamp;
                    ++count;
                }
            }
            std::cout << "Decoded RGB frames=" << count << " finalTimestamp=" << previous << '\n';
            if (count != 40)
                result = 5;
        }
        MFShutdown();
        // Refuse an existing file, leaving its valid MP4 untouched.
        const auto size = std::filesystem::file_size(path);
        try {
            nskry::record::MfEncoder encoder;
            encoder.Open(path.wstring(), 320, 180, 20);
            result = 6;
        } catch (const winrt::hresult_error &) {
        }
        if (std::filesystem::file_size(path) != size)
            result = 7;
        {
            nskry::record::MfEncoder software;
            auto softwarePath = path;
            softwarePath += L".software.mp4";
            software.Open(softwarePath.wstring(), 320, 180, 20, false);
            std::vector<uint8_t> pixels(320 * 180 * 4, 128);
            for (int i = 0; i < 10; ++i)
                software.Write(pixels.data(), i * 500000LL, 500000);
            software.Finalize();
            std::wcout << L"Software path: " << software.Backend() << L"\n";
        }
    } catch (const winrt::hresult_error &e) {
        std::wcerr << L"MF error 0x" << std::hex << e.code().value << L" " << e.message().c_str() << L'\n';
        result = 1;
    }
    winrt::uninit_apartment();
    if (!artifacts.Finish(result == 0))
        result = 1;
    return result;
}
