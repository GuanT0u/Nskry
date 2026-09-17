#include "pch.h"
#include "update/downloader.h"

#include <bcrypt.h>
#include <fstream>
#include <winhttp.h>

namespace nskry {
namespace {
void Error(std::wstring* error, const wchar_t* text) { if (error) *error = text; }
bool IsHttps(const std::wstring& url) { return url.rfind(L"https://", 0) == 0; }
bool Fetch(const std::wstring& url, std::vector<BYTE>& bytes, std::wstring* error) {
    if (!IsHttps(url)) { Error(error, L"Update URLs must use HTTPS."); return false; }
    URL_COMPONENTSW components{}; components.dwStructSize = sizeof(components); components.dwSchemeLength = components.dwHostNameLength = components.dwUrlPathLength = components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &components) || components.nScheme != INTERNET_SCHEME_HTTPS) { Error(error, L"Invalid HTTPS update URL."); return false; }
    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring object(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength) object.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    HINTERNET session = ::WinHttpOpen(L"Nskry/0.4", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { Error(error, L"Unable to create HTTPS client."); return false; }
    HINTERNET connection = ::WinHttpConnect(session, host.c_str(), components.nPort, 0);
    HINTERNET request = connection ? ::WinHttpOpenRequest(connection, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = request && ::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && ::WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, statusSize = sizeof(status); if (ok) ok = ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) && status == 200;
    while (ok) { DWORD available = 0; if (!::WinHttpQueryDataAvailable(request, &available)) { ok = false; break; } if (!available) break; const size_t start = bytes.size(); bytes.resize(start + available); DWORD read = 0; if (!::WinHttpReadData(request, bytes.data() + start, available, &read)) { ok = false; break; } bytes.resize(start + read); }
    if (request) ::WinHttpCloseHandle(request); if (connection) ::WinHttpCloseHandle(connection); ::WinHttpCloseHandle(session);
    if (!ok) Error(error, L"HTTPS download failed or returned a non-success status.");
    return ok;
}
std::wstring Hex(const BYTE* data, DWORD size) { const wchar_t* digits = L"0123456789abcdef"; std::wstring out; out.reserve(size * 2); for (DWORD i = 0; i < size; ++i) { out += digits[data[i] >> 4]; out += digits[data[i] & 15]; } return out; }
}
bool Downloader::DownloadTextHttps(const std::wstring& url, std::wstring& text, std::wstring* error) { std::vector<BYTE> bytes; if (!Fetch(url, bytes, error)) return false; if (bytes.empty()) { text.clear(); return true; } const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), nullptr, 0); if (!count) { Error(error, L"Update response is not valid UTF-8."); return false; } text.resize(count); return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), text.data(), count) > 0; }
bool Downloader::DownloadFileHttps(const std::wstring& url, const std::wstring& destination, std::wstring* error) { std::vector<BYTE> bytes; if (!Fetch(url, bytes, error)) return false; std::ofstream output(destination, std::ios::binary | std::ios::trunc); if (!output) { Error(error, L"Unable to create update download file."); return false; } output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); return static_cast<bool>(output); }
bool Downloader::VerifySha256File(const std::wstring& path, const std::wstring& expectedHex, std::wstring* error) { if (expectedHex.size() != 64 || !std::all_of(expectedHex.begin(), expectedHex.end(), iswxdigit)) { Error(error, L"Update manifest SHA-256 is invalid."); return false; } std::ifstream input(path, std::ios::binary); if (!input) { Error(error, L"Downloaded update package is unavailable."); return false; } BCRYPT_ALG_HANDLE alg{}; BCRYPT_HASH_HANDLE hash{}; DWORD objectSize{}, resultSize{}, hashSize{}; bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 && BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0) >= 0 && BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &resultSize, 0) >= 0; std::vector<BYTE> object(objectSize), digest(hashSize), buffer(64 * 1024); if (ok) ok = BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0; while (ok && input) { input.read(reinterpret_cast<char*>(buffer.data()), buffer.size()); const auto got = input.gcount(); if (got > 0) ok = BCryptHashData(hash, buffer.data(), static_cast<ULONG>(got), 0) >= 0; } if (ok) ok = BCryptFinishHash(hash, digest.data(), hashSize, 0) >= 0; if (hash) BCryptDestroyHash(hash); if (alg) BCryptCloseAlgorithmProvider(alg, 0); if (!ok || _wcsicmp(Hex(digest.data(), hashSize).c_str(), expectedHex.c_str()) != 0) { Error(error, L"Downloaded package SHA-256 verification failed."); return false; } return true; }
} // namespace nskry
