#pragma once
#include <string>
#include <vector>
#include <wincodec.h>
#include <winrt/base.h>
namespace nskry::record {
// Private worker-only streaming encoder. One indexed frame, no animation cache.
// Bounded adaptive local palette; no animation/frame cache or external model.
class GifEncoder {
  public:
    void Open(const std::wstring &path, UINT width, UINT height, volatile LONG *created);
    void Write(const uint8_t *bgra, int64_t time, int64_t duration);
    void Finalize();

  private:
    winrt::com_ptr<IWICImagingFactory> m_factory;
    winrt::com_ptr<IStream> m_stream;
    winrt::com_ptr<IWICBitmapEncoder> m_encoder;
    winrt::com_ptr<IWICPalette> m_palette;
    std::vector<uint8_t> m_indices;
    UINT m_width{}, m_height{}, m_frames{};
    int64_t m_end{}, m_delayEnd{};
    bool m_finalized = false;
};
} // namespace nskry::record
