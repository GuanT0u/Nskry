#pragma once

#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>

namespace nskry {
bool ResponseChunkFits(size_t total, size_t received, size_t limit) noexcept;

/// Short-lived HTTPS-only transfer helper. No handle survives a request.
class Downloader {
  public:
    static bool DownloadTextHttps(const std::wstring &url, std::wstring &text, std::wstring *error = nullptr,
                                  std::stop_token stop = {}, uint32_t timeoutMs = 120000);
    static bool DownloadFileHttps(const std::wstring &url, const std::wstring &destination,
                                  std::wstring *error = nullptr, std::stop_token stop = {},
                                  uint32_t timeoutMs = 120000);
    static bool VerifySha256File(const std::wstring &path, const std::wstring &expectedHex,
                                 std::wstring *error = nullptr, std::stop_token stop = {});
};

} // namespace nskry
