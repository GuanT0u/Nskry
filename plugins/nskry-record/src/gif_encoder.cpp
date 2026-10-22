#include "gif_encoder.h"
#include "gif_palette.h"
#include "record_policy.h"
#include <array>
#include <cstring>
#include <shlwapi.h>
namespace nskry::record {
void GifEncoder::Open(const std::wstring &path, UINT width, UINT height, volatile LONG *created) {
    if (m_encoder || width < 2 || height < 2 || width > 960 || height > 960 || uint64_t(width) * height > 960 * 540)
        winrt::throw_hresult(E_INVALIDARG);
    m_factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
    winrt::check_hresult(SHCreateStreamOnFileEx(path.c_str(), STGM_WRITE | STGM_SHARE_EXCLUSIVE | STGM_FAILIFTHERE,
                                                FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, m_stream.put()));
    InterlockedExchange(created, 1);
    winrt::check_hresult(m_factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, m_encoder.put()));
    winrt::check_hresult(m_encoder->Initialize(m_stream.get(), WICBitmapEncoderNoCache));
    winrt::check_hresult(m_factory->CreatePalette(m_palette.put()));
    winrt::com_ptr<IWICMetadataQueryWriter> metadata;
    winrt::check_hresult(m_encoder->GetMetadataQueryWriter(metadata.put()));
    // Netscape looping extension, copied synchronously by WIC.
    BYTE application[] = {'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'};
    BYTE data[] = {3, 1, 0, 0, 0};
    PROPVARIANT v{};
    v.vt = VT_UI1 | VT_VECTOR;
    v.caub = {sizeof(application), application};
    winrt::check_hresult(metadata->SetMetadataByName(L"/appext/application", &v));
    v.caub = {sizeof(data), data};
    winrt::check_hresult(metadata->SetMetadataByName(L"/appext/data", &v));
    m_width = width;
    m_height = height;
    m_indices.resize(uint64_t(width) * height);
}
void GifEncoder::Write(const uint8_t *bgra, int64_t time, int64_t duration) {
    if (!m_encoder || m_finalized || !bgra || !ValidSampleTime(time, duration, m_end) ||
        time + duration > 120LL * 10000000 || m_frames >= 1200)
        winrt::throw_hresult(E_INVALIDARG);
    STATSTG stat{};
    winrt::check_hresult(m_stream->Stat(&stat, STATFLAG_NONAME));
    if (stat.cbSize.QuadPart >= 128ULL * 1024 * 1024)
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
    uint64_t signature = 14695981039346656037ULL;
    for (size_t i = 0; i < 32; ++i) {
        uint32_t pixel{};
        memcpy(&pixel, bgra + (i * (m_indices.size() - 1) / 31) * 4, sizeof(pixel));
        signature = (signature ^ pixel) * 1099511628211ULL;
    }
    const bool similar = m_frames && signature == m_signature;
    m_signature = signature;
    // The signature only avoids cache copies in obvious animation. Exact
    // comparison is always required to reuse a palette; hash collisions cannot
    // cause incorrect pixels. Cache and indices must belong to the same frame.
    if (!similar || !m_havePrevious || memcmp(m_previous.data(), bgra, m_previous.size())) {
        Quantize(bgra);
        m_havePrevious = similar;
        if (m_havePrevious) {
            m_previous.resize(m_indices.size() * 4);
            memcpy(m_previous.data(), bgra, m_previous.size());
        }
    }
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::check_hresult(m_encoder->CreateNewFrame(frame.put(), nullptr));
    winrt::check_hresult(frame->Initialize(nullptr));
    winrt::check_hresult(frame->SetSize(m_width, m_height));
    WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
    winrt::check_hresult(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat8bppIndexed)
        winrt::throw_hresult(E_UNEXPECTED);
    winrt::check_hresult(frame->SetPalette(m_palette.get()));
    winrt::com_ptr<IWICMetadataQueryWriter> metadata;
    winrt::check_hresult(frame->GetMetadataQueryWriter(metadata.put()));
    const auto end = (time + duration + 50000) / 100000;
    PROPVARIANT v{};
    v.vt = VT_UI2;
    v.uiVal = USHORT(std::clamp(end - m_delayEnd, int64_t(1), int64_t(65535)));
    winrt::check_hresult(metadata->SetMetadataByName(L"/grctlext/Delay", &v));
    v.vt = VT_UI1;
    v.bVal = 1;
    winrt::check_hresult(metadata->SetMetadataByName(L"/grctlext/Disposal", &v));
    winrt::check_hresult(frame->WritePixels(m_height, m_width, UINT(m_indices.size()), m_indices.data()));
    winrt::check_hresult(frame->Commit());
    m_delayEnd = end;
    m_end = time + duration;
    ++m_frames;
}
void GifEncoder::Quantize(const uint8_t *bgra) {
    std::array<uint32_t, 256> exact{};
    uint32_t count{};
    if (ExactGifPalette(bgra, m_indices.size(), m_indices.data(), exact, count)) {
        winrt::check_hresult(m_palette->InitializeCustom(exact.data(), count));
    } else {
        winrt::com_ptr<IWICBitmap> bitmap;
        winrt::check_hresult(m_factory->CreateBitmapFromMemory(m_width, m_height, GUID_WICPixelFormat32bppBGRA,
                                                               m_width * 4, m_width * m_height * 4,
                                                               const_cast<BYTE *>(bgra), bitmap.put()));
        // Bound palette analysis to 256x256; mapping still uses original pixels.
        // Local palettes preserve dark neutral tones and scene-specific colors,
        // unlike the old RGB332 truncation (only four blue values).
        winrt::com_ptr<IWICBitmapScaler> sample;
        winrt::check_hresult(m_factory->CreateBitmapScaler(sample.put()));
        const double scale = (std::min)(1.0, 256.0 / (std::max)(m_width, m_height));
        winrt::check_hresult(sample->Initialize(bitmap.get(), (std::max)(1u, UINT(m_width * scale)),
                                                (std::max)(1u, UINT(m_height * scale)),
                                                WICBitmapInterpolationModeFant));
        winrt::check_hresult(m_palette->InitializeFromBitmap(sample.get(), 256, FALSE));
        winrt::com_ptr<IWICFormatConverter> indexed;
        winrt::check_hresult(m_factory->CreateFormatConverter(indexed.put()));
        winrt::check_hresult(indexed->Initialize(bitmap.get(), GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeNone,
                                                 m_palette.get(), 0, WICBitmapPaletteTypeCustom));
        winrt::check_hresult(indexed->CopyPixels(nullptr, m_width, UINT(m_indices.size()), m_indices.data()));
        // WIC's indexed converter can collapse exact neutral grays to another
        // palette entry. Map neutral grays explicitly using the actual local palette.
        std::array<WICColor, 256> colors{};
        UINT actual{};
        winrt::check_hresult(m_palette->GetColors(256, colors.data(), &actual));
        std::array<uint8_t, 256> grays{};
        for (UINT value = 0; value < 256; ++value) {
            unsigned best = UINT_MAX;
            for (UINT j = 0; j < actual; ++j) {
                const int b = int(colors[j] & 255) - int(value), g = int((colors[j] >> 8) & 255) - int(value),
                          r = int((colors[j] >> 16) & 255) - int(value);
                const unsigned error = b * b + g * g + r * r;
                if (error < best) {
                    best = error;
                    grays[value] = uint8_t(j);
                }
            }
        }
        for (size_t i = 0; i < m_indices.size(); ++i) {
            const auto *p = bgra + i * 4;
            if (p[0] == p[1] && p[1] == p[2])
                m_indices[i] = grays[p[0]];
        }
    }
}
void GifEncoder::Finalize() {
    if (!m_encoder || !m_frames || m_finalized)
        winrt::throw_hresult(E_UNEXPECTED);
    winrt::check_hresult(m_encoder->Commit());
    winrt::check_hresult(m_stream->Commit(STGC_DEFAULT));
    m_finalized = true;
}
} // namespace nskry::record
