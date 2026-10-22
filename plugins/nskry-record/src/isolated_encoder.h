#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <windows.h>

namespace nskry::record {
// Recording worker only. MF and hardware drivers live in a one-recording child.
// Public host SDK and capture thread ownership are unchanged.
class IsolatedEncoder {
  public:
    explicit IsolatedEncoder(HANDLE canceled = nullptr, std::filesystem::path executable = {},
                             DWORD operationTimeout = 15000);
    ~IsolatedEncoder();
    IsolatedEncoder(const IsolatedEncoder &) = delete;
    IsolatedEncoder &operator=(const IsolatedEncoder &) = delete;
    void Open(const std::wstring &path, UINT width, UINT height, UINT fps, bool hardware = true, uint32_t bitrate = 0,
              bool gif = false, bool preview = false, UINT sourceWidth = 0, UINT sourceHeight = 0);
    void Preview(int64_t time, uint8_t *pixels, uint32_t capacity);
    void Write(const uint8_t *bgra, int64_t time, int64_t duration);
    void Finalize();
    void Trim(const std::wstring &input, int64_t start, int64_t end);
    bool Close() noexcept; // True only after the child process is signaled/exited.
    bool CreatedFile() const;
    bool Finalized() const;
    bool FinalizationAttempted() const {
        return m_finalizationAttempted;
    }
    uint64_t Samples() const {
        return m_samples;
    }
    int64_t EncodedDuration() const {
        return m_lastEnd;
    }
    const std::wstring &Backend() const {
        return m_backend;
    }
    DWORD ProcessId() const;
    uint64_t CpuTime() const;

  private:
    struct Channel;
    std::unique_ptr<Channel> m_channel;
    HANDLE m_canceled{};
    std::filesystem::path m_executable;
    DWORD m_timeout;
    bool m_finalizationAttempted = false;
    uint64_t m_samples{};
    int64_t m_lastEnd{};
    std::wstring m_backend;
};
} // namespace nskry::record
