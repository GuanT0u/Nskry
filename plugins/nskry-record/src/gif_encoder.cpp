#include "gif_encoder.h"
#include "record_policy.h"
#include <array>
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
    std::array<WICColor, 256> colors{};
    for (UINT i = 0; i < 256; ++i)
        colors[i] = 0xff000000 | (((i >> 5) * 255 / 7) << 16) | ((((i >> 2) & 7) * 255 / 7) << 8) | ((i & 3) * 255 / 3);
    winrt::check_hresult(m_palette->InitializeCustom(colors.data(), UINT(colors.size())));
    winrt::check_hresult(m_encoder->SetPalette(m_palette.get()));
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
    for (size_t i = 0; i < m_indices.size(); ++i) {
        const auto *p = bgra + i * 4;
        m_indices[i] = (p[2] & 0xe0) | ((p[1] >> 3) & 0x1c) | (p[0] >> 6);
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
    v.bVal = 1; // Keep full canvas, no transparent disposal artefacts.
    winrt::check_hresult(metadata->SetMetadataByName(L"/grctlext/Disposal", &v));
    winrt::check_hresult(frame->WritePixels(m_height, m_width, UINT(m_indices.size()), m_indices.data()));
    winrt::check_hresult(frame->Commit());
    m_delayEnd = end;
    m_end = time + duration;
    ++m_frames;
}
void GifEncoder::Finalize() {
    if (!m_encoder || !m_frames || m_finalized)
        winrt::throw_hresult(E_UNEXPECTED);
    winrt::check_hresult(m_encoder->Commit());
    winrt::check_hresult(m_stream->Commit(STGC_DEFAULT));
    m_finalized = true;
}
} // namespace nskry::record
