#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <wincodec.h>
#include <winrt/base.h>
namespace nskry::record {
// Private worker-only streaming encoder. One indexed frame and one previous
// BGRA frame for palette reuse; no animation cache or external model.
class GifEncoder {
  public:
    void Open(const std::wstring &path, UINT width, UINT height, volatile LONG *created);
    void Write(const uint8_t *bgra, int64_t time, int64_t duration);
    void Finalize();

  private:
    void Quantize(const uint8_t *bgra);
    winrt::com_ptr<IWICImagingFactory> m_factory;
    winrt::com_ptr<IStream> m_stream;
    winrt::com_ptr<IWICBitmapEncoder> m_encoder;
    winrt::com_ptr<IWICPalette> m_palette;
    std::vector<uint8_t> m_indices;
    std::vector<uint8_t> m_previous;
    UINT m_width{}, m_height{}, m_frames{};
    uint64_t m_signature{};
    int64_t m_end{}, m_delayEnd{};
    bool m_finalized = false, m_havePrevious = false;
};
} // namespace nskry::record
