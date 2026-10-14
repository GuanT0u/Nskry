#include "update/downloader.h"
#include "pch.h"
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <chrono>
#include <fstream>
#include <winhttp.h>
namespace nskry {
namespace {
void Error(std::wstring *out, const wchar_t *text) {
    if (out)
        *out = text;
}
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct InternetHandle {
    HINTERNET value{};
    ~InternetHandle() {
        if (value)
            WinHttpCloseHandle(value);
    }
};
struct Response {
    Handle ready{CreateEventW(nullptr, FALSE, FALSE, nullptr)}, closed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<DWORD> status{}, read{};
};
void CALLBACK Status(HINTERNET, DWORD_PTR context, DWORD status, void *, DWORD length) {
    auto *state = reinterpret_cast<Response *>(context);
    if (!state)
        return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        SetEvent(state->closed.value);
        return;
    }
    if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE || status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
        status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE || status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        state->read = length;
        state->status = status;
        SetEvent(state->ready.value);
    }
}
struct Request {
    HINTERNET value{};
    Response *state{};
    bool callbacks{};
    ~Request() {
        if (value) {
            WinHttpCloseHandle(value);
            if (callbacks)
                WaitForSingleObject(state->closed.value, INFINITE);
        }
    }
};
bool Fetch(const std::wstring &url, size_t limit, const std::function<bool(const BYTE *, DWORD)> &sink,
           std::wstring *error, std::stop_token stop, uint32_t timeoutMs) {
    if (error)
        error->clear();
    if (stop.stop_requested()) {
        Error(error, L"Update cancelled.");
        return false;
    }
    URL_COMPONENTSW c{};
    c.dwStructSize = sizeof(c);
    c.dwSchemeLength = c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = DWORD(-1);
    c.dwUserNameLength = c.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &c) || c.nScheme != INTERNET_SCHEME_HTTPS || c.dwUserNameLength ||
        c.dwPasswordLength) {
        Error(error, L"Update URLs must use HTTPS.");
        return false;
    }
    std::wstring host(c.lpszHostName, c.dwHostNameLength), object(c.lpszUrlPath, c.dwUrlPathLength);
    if (c.dwExtraInfoLength)
        object.append(c.lpszExtraInfo, c.dwExtraInfoLength);
    InternetHandle session{WinHttpOpen(L"Nskry/0.5", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
    if (!session.value || !WinHttpSetTimeouts(session.value, 8000, 8000, 15000, 15000)) {
        Error(error, L"Unable to initialize bounded HTTPS transport.");
        return false;
    }
    InternetHandle connection{WinHttpConnect(session.value, host.c_str(), c.nPort, 0)};
    Response state;
    Handle cancelled{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::array<BYTE, 64 * 1024> buffer{}; // Outlives the final request callback.
    Request request{connection.value
                        ? WinHttpOpenRequest(connection.value, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                        : nullptr,
                    &state};
    if (!request.value || !state.ready.value || !state.closed.value || !cancelled.value) {
        Error(error, L"Unable to create HTTPS request.");
        return false;
    }
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&state);
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        WinHttpSetStatusCallback(request.value, Status,
                                 WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
                                 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        Error(error, L"Unable to configure HTTPS request safety.");
        return false;
    }
    request.callbacks = true;
    std::stop_callback cancellation(stop, [&] { SetEvent(cancelled.value); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto wait = [&](BOOL issued, DWORD expected) {
        if (!issued && GetLastError() != ERROR_IO_PENDING) {
            Error(error, L"HTTPS operation could not be started.");
            return false;
        }
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            Error(error, L"HTTPS request timed out.");
            return false;
        }
        HANDLE events[] = {cancelled.value, state.ready.value};
        const DWORD outcome = WaitForMultipleObjects(2, events, FALSE, DWORD((std::max)(int64_t(0), int64_t(left))));
        if (outcome == WAIT_OBJECT_0 || stop.stop_requested()) {
            Error(error, L"Update cancelled.");
            return false;
        }
        if (outcome == WAIT_TIMEOUT) {
            Error(error, L"HTTPS request timed out.");
            return false;
        }
        if (outcome != WAIT_OBJECT_0 + 1 || state.status != expected) {
            Error(error, L"HTTPS request failed (network, TLS or timeout).");
            return false;
        }
        return true;
    };
    if (!wait(WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, context),
              WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE) ||
        !wait(WinHttpReceiveResponse(request.value, nullptr), WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE))
        return false;
    DWORD code{}, size = sizeof(code);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX) ||
        code != 200) {
        Error(error, L"HTTPS server returned a non-success status.");
        return false;
    }
    size_t total = 0;
    for (;;) {
        if (stop.stop_requested()) {
            Error(error, L"Update cancelled.");
            return false;
        }
        if (!wait(WinHttpReadData(request.value, buffer.data(), DWORD(buffer.size()), nullptr),
                  WINHTTP_CALLBACK_STATUS_READ_COMPLETE))
            return false;
        const DWORD read = state.read;
        if (!read)
            return true;
        if (!ResponseChunkFits(total, read, limit)) {
            Error(error, L"HTTPS response exceeds the allowed size.");
            return false;
        }
        if (!sink(buffer.data(), read)) {
            Error(error, L"Unable to store HTTPS response.");
            return false;
        }
        total += read;
    }
}
std::wstring Hex(const BYTE *bytes, DWORD size) {
    const wchar_t *digits = L"0123456789abcdef";
    std::wstring out;
    for (DWORD i = 0; i < size; ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}
} // namespace
bool ResponseChunkFits(size_t total, size_t received, size_t limit) noexcept {
    return total <= limit && received <= limit - total;
}
bool Downloader::DownloadTextHttps(const std::wstring &url, std::wstring &text, std::wstring *error,
                                   std::stop_token stop, uint32_t timeoutMs) {
    text.clear();
    std::vector<BYTE> bytes;
    if (!Fetch(
            url, 2 * 1024 * 1024,
            [&](const BYTE *data, DWORD n) {
                bytes.insert(bytes.end(), data, data + n);
                return true;
            },
            error, stop, timeoutMs))
        return false;
    if (bytes.empty())
        return true;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char *>(bytes.data()),
                                          int(bytes.size()), nullptr, 0);
    if (!count) {
        Error(error, L"Update response is not valid UTF-8.");
        return false;
    }
    text.resize(count);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char *>(bytes.data()),
                               int(bytes.size()), text.data(), count) > 0;
}
bool Downloader::DownloadFileHttps(const std::wstring &url, const std::wstring &path, std::wstring *error,
                                   std::stop_token stop, uint32_t timeoutMs) {
    bool ok = false;
    {
        Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) {
            Error(error, L"Unable to create unique download file.");
            return false;
        }
        try {
            ok = Fetch(
                url, 128 * 1024 * 1024,
                [&](const BYTE *data, DWORD n) {
                    DWORD written{};
                    return WriteFile(file.value, data, n, &written, nullptr) && written == n;
                },
                error, stop, timeoutMs);
        } catch (...) {
            ok = false;
            Error(error, L"HTTPS transfer failed unexpectedly.");
        }
    }
    if (!ok)
        DeleteFileW(path.c_str());
    return ok;
}
bool Downloader::VerifySha256File(const std::wstring &path, const std::wstring &expected, std::wstring *error,
                                  std::stop_token stop) {
    if (expected.size() != 64 || !std::all_of(expected.begin(), expected.end(), iswxdigit)) {
        Error(error, L"Invalid SHA-256.");
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Error(error, L"Downloaded package is unavailable.");
        return false;
    }
    struct HashGuard {
        BCRYPT_ALG_HANDLE alg{};
        BCRYPT_HASH_HANDLE hash{};
        ~HashGuard() {
            if (hash)
                BCryptDestroyHash(hash);
            if (alg)
                BCryptCloseAlgorithmProvider(alg, 0);
        }
    } guard;
    auto &alg = guard.alg;
    auto &hash = guard.hash;
    DWORD objectSize{}, size{}, hashSize{};
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                                &size, 0) >= 0 &&
              BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &size,
                                0) >= 0;
    std::vector<BYTE> object(objectSize), digest(hashSize), buffer(64 * 1024);
    if (ok)
        ok = BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0;
    while (ok && file && !stop.stop_requested()) {
        file.read(reinterpret_cast<char *>(buffer.data()), std::streamsize(buffer.size()));
        if (file.gcount() > 0)
            ok = BCryptHashData(hash, buffer.data(), ULONG(file.gcount()), 0) >= 0;
    }
    ok = ok && !file.bad() && !stop.stop_requested();
    if (ok)
        ok = BCryptFinishHash(hash, digest.data(), hashSize, 0) >= 0;
    if (!ok || _wcsicmp(Hex(digest.data(), hashSize).c_str(), expected.c_str())) {
        Error(error, stop.stop_requested() ? L"Update cancelled." : L"SHA-256 verification failed.");
        return false;
    }
    return true;
}
} // namespace nskry
