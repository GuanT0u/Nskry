#pragma once
#include <mfidl.h>
#include <mfreadwrite.h>
#include <string>
#include <vector>
#include <windows.h>
#include <winrt/base.h>

namespace nskry::record {
// MTA recording worker only. This object balances its own MFStartup/Shutdown.
class MfEncoder {
  public:
    MfEncoder();
    ~MfEncoder();
    MfEncoder(const MfEncoder &) = delete;
    MfEncoder &operator=(const MfEncoder &) = delete;
    void Open(const std::wstring &temporary, UINT width, UINT height, UINT fps, bool preferHardware = true,
              volatile LONG *created = nullptr);
    // Internal stream sink (e.g. quota/error regression). AddRefs caller's stream,
    // does not own/delete a path, no destructive automatic retry on caller data.
    void OpenStream(IMFByteStream *stream, UINT width, UINT height, UINT fps);
    void Write(const uint8_t *topDownBgra, int64_t time, int64_t duration);
    void WriteNv12(const uint8_t *packedNv12, int64_t time, int64_t duration);
    void Finalize();
    const std::wstring &Backend() const {
        return m_backend;
    }
    uint64_t Samples() const {
        return m_samples;
    }
    bool CreatedFile() const {
        return m_ownsFile;
    }
    bool FinalizationAttempted() const {
        return m_finalizationAttempted;
    }
    bool Finalized() const {
        return m_finalized;
    }
    int64_t EncodedDuration() const {
        return m_lastEnd;
    }

  private:
    void Configure(bool hardware);
    void SetFormat(UINT width, UINT height, UINT fps);
    void DescribeBackend();
    void ClosePipeline() noexcept;
    void WritePixels(const uint8_t *pixels, int64_t time, int64_t duration, bool nv12);
    winrt::com_ptr<IMFSinkWriter> m_writer;
    winrt::com_ptr<IMFByteStream> m_file;
    std::wstring m_path, m_backend;
    UINT m_width{}, m_height{}, m_fps{};
    DWORD m_stream{};
    uint64_t m_samples{};
    bool m_ownsFile = false;
    bool m_finalizationAttempted = false, m_finalized = false;
    int64_t m_lastEnd{};
};
} // namespace nskry::record
