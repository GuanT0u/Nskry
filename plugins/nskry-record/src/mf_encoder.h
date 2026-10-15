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
    void Open(const std::wstring &temporary, UINT width, UINT height, UINT fps, bool preferHardware = true);
    void Write(const uint8_t *topDownBgra, int64_t time, int64_t duration);
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

  private:
    void Configure(bool hardware);
    void DescribeBackend();
    winrt::com_ptr<IMFSinkWriter> m_writer;
    winrt::com_ptr<IMFByteStream> m_file;
    std::wstring m_path, m_backend;
    UINT m_width{}, m_height{}, m_fps{};
    DWORD m_stream{};
    uint64_t m_samples{};
    bool m_ownsFile = false;
};
} // namespace nskry::record
