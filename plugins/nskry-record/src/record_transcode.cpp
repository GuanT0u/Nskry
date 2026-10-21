#include "record_transcode.h"
#include "record_trim.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <vector>
#include <wincodec.h>
#include <winrt/base.h>
namespace nskry::record {
void TrimRecording(const std::wstring &input, bool gif, UINT width, UINT height, int64_t start, int64_t end,
                   const std::function<void(const uint8_t *, int64_t, int64_t)> &write) {
    if (!ValidTrim(start, end, INT64_MAX))
        winrt::throw_hresult(E_INVALIDARG);
    if (gif) {
        auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        winrt::check_hresult(factory->CreateDecoderFromFilename(input.c_str(), nullptr, GENERIC_READ,
                                                                WICDecodeMetadataCacheOnDemand, decoder.put()));
        UINT count{};
        winrt::check_hresult(decoder->GetFrameCount(&count));
        if (count > 1200)
            winrt::throw_hresult(E_BOUNDS);
        std::vector<uint8_t> pixels(uint64_t(width) * height * 4);
        int64_t time = 0;
        for (UINT i = 0; i < count && time < end; ++i) {
            winrt::com_ptr<IWICBitmapFrameDecode> frame;
            winrt::check_hresult(decoder->GetFrame(i, frame.put()));
            UINT w{}, h{};
            winrt::check_hresult(frame->GetSize(&w, &h));
            if (w != width || h != height)
                winrt::throw_hresult(E_BOUNDS);
            winrt::com_ptr<IWICMetadataQueryReader> metadata;
            winrt::check_hresult(frame->GetMetadataQueryReader(metadata.put()));
            PROPVARIANT delay{};
            winrt::check_hresult(metadata->GetMetadataByName(L"/grctlext/Delay", &delay));
            const int64_t span = delay.vt == VT_UI2 ? int64_t(delay.uiVal) * 100000 : 0;
            PropVariantClear(&delay);
            if (span <= 0)
                winrt::throw_hresult(E_INVALIDARG);
            const auto clip = ClipSample(time, span, start, end);
            if (clip.duration) {
                winrt::com_ptr<IWICFormatConverter> converter;
                winrt::check_hresult(factory->CreateFormatConverter(converter.put()));
                winrt::check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                                                           WICBitmapDitherTypeNone, nullptr, 0,
                                                           WICBitmapPaletteTypeCustom));
                winrt::check_hresult(converter->CopyPixels(nullptr, width * 4, UINT(pixels.size()), pixels.data()));
                write(pixels.data(), clip.time, clip.duration);
            }
            time += span;
        }
        return;
    }
    winrt::check_hresult(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    struct Shutdown {
        ~Shutdown() {
            MFShutdown();
        }
    } shutdown;
    winrt::com_ptr<IMFAttributes> attributes;
    winrt::check_hresult(MFCreateAttributes(attributes.put(), 2));
    winrt::check_hresult(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE));
    constexpr DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    constexpr DWORD allStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
    winrt::com_ptr<IMFSourceReader> reader;
    winrt::check_hresult(MFCreateSourceReaderFromURL(input.c_str(), attributes.get(), reader.put()));
    winrt::check_hresult(reader->SetStreamSelection(allStreams, FALSE));
    winrt::check_hresult(reader->SetStreamSelection(videoStream, TRUE));
    winrt::com_ptr<IMFMediaType> type;
    winrt::check_hresult(MFCreateMediaType(type.put()));
    winrt::check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    winrt::check_hresult(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
    winrt::check_hresult(MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, width, height));
    winrt::check_hresult(reader->SetCurrentMediaType(videoStream, nullptr, type.get()));
    winrt::com_ptr<IMFMediaType> actual;
    winrt::check_hresult(reader->GetCurrentMediaType(videoStream, actual.put()));
    UINT w{}, h{};
    winrt::check_hresult(MFGetAttributeSize(actual.get(), MF_MT_FRAME_SIZE, &w, &h));
    if (w != width || h != height)
        winrt::throw_hresult(E_BOUNDS);
    PROPVARIANT position{};
    position.vt = VT_I8;
    position.hVal.QuadPart = start;
    winrt::check_hresult(reader->SetCurrentPosition(GUID_NULL, position));
    std::vector<uint8_t> pixels(uint64_t(w) * h * 4);
    UINT storageWidth = w, storageHeight = h;
    while (true) {
        DWORD flags{};
        LONGLONG timestamp{};
        winrt::com_ptr<IMFSample> sample;
        winrt::check_hresult(reader->ReadSample(videoStream, 0, nullptr, &flags, &timestamp, sample.put()));
        if (flags & MF_SOURCE_READERF_ERROR)
            throw winrt::hresult_error(E_FAIL, L"Trim decoder reported a source error.");
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            winrt::com_ptr<IMFMediaType> changed;
            winrt::check_hresult(reader->GetCurrentMediaType(videoStream, changed.put()));
            UINT cw{}, ch{};
            GUID subtype{};
            winrt::check_hresult(MFGetAttributeSize(changed.get(), MF_MT_FRAME_SIZE, &cw, &ch));
            winrt::check_hresult(changed->GetGUID(MF_MT_SUBTYPE, &subtype));
            bool aperture = cw == w && ch == h;
            if (!aperture && cw >= w && ch >= h && cw - w < 16 && ch - h < 16) {
                MFVideoArea area{};
                UINT bytes{};
                auto hr = changed->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8 *>(&area),
                                           sizeof(area), &bytes);
                if (FAILED(hr))
                    hr = changed->GetBlob(MF_MT_GEOMETRIC_APERTURE, reinterpret_cast<UINT8 *>(&area), sizeof(area),
                                          &bytes);
                aperture = SUCCEEDED(hr) && bytes == sizeof(area) && area.Area.cx == LONG(w) &&
                           area.Area.cy == LONG(h) && !area.OffsetX.value && !area.OffsetX.fract &&
                           !area.OffsetY.value && !area.OffsetY.fract;
            }
            if (!aperture || subtype != MFVideoFormat_RGB32)
                throw winrt::hresult_error(E_BOUNDS, L"Trim decoder changed image size or format: " +
                                                         std::to_wstring(cw) + L"x" + std::to_wstring(ch) +
                                                         L" subtype=" + std::to_wstring(subtype.Data1));
            storageWidth = cw;
            storageHeight = ch;
        }
        if (timestamp >= end)
            break;
        if (sample) {
            LONGLONG duration{};
            winrt::check_hresult(sample->GetSampleDuration(&duration));
            const auto clip = ClipSample(timestamp, duration, start, end);
            if (clip.duration) {
                winrt::com_ptr<IMFMediaBuffer> buffer;
                winrt::check_hresult(sample->ConvertToContiguousBuffer(buffer.put()));
                if (auto surface = buffer.try_as<IMF2DBuffer>()) {
                    BYTE *top{};
                    LONG stride{};
                    winrt::check_hresult(surface->Lock2D(&top, &stride));
                    const bool valid = int64_t(stride) >= int64_t(w) * 4 || int64_t(stride) <= -int64_t(w) * 4;
                    if (valid)
                        for (UINT row = 0; row < h; ++row)
                            memcpy(pixels.data() + uint64_t(row) * w * 4, top + int64_t(row) * stride, w * 4);
                    surface->Unlock2D();
                    if (!valid)
                        winrt::throw_hresult(E_BOUNDS);
                } else {
                    BYTE *data{};
                    DWORD size{};
                    winrt::check_hresult(buffer->Lock(&data, nullptr, &size));
                    const bool valid = size == uint64_t(storageWidth) * storageHeight * 4;
                    if (valid)
                        for (UINT row = 0; row < h; ++row)
                            memcpy(pixels.data() + uint64_t(row) * w * 4, data + uint64_t(row) * storageWidth * 4,
                                   w * 4);
                    buffer->Unlock();
                    if (!valid)
                        winrt::throw_hresult(E_BOUNDS);
                }
                write(pixels.data(), clip.time, clip.duration);
            }
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
    }
}
} // namespace nskry::record
