#pragma once

#include <string>

namespace nskry {

/// Short-lived HTTPS-only transfer helper. No handle survives a request.
class Downloader {
public:
    static bool DownloadTextHttps(const std::wstring& url, std::wstring& text, std::wstring* error = nullptr);
    static bool DownloadFileHttps(const std::wstring& url, const std::wstring& destination, std::wstring* error = nullptr);
    static bool VerifySha256File(const std::wstring& path, const std::wstring& expectedHex, std::wstring* error = nullptr);
};

} // namespace nskry
