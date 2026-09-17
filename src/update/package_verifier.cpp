#include "pch.h"
#include "update/package_verifier.h"

#include <fstream>

namespace nskry {
namespace {

constexpr uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr uint32_t kCentralDirectoryEntry = 0x02014b50;

uint16_t ReadU16(const std::vector<unsigned char>& bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t ReadU32(const std::vector<unsigned char>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

bool Utf8ToWide(const char* data, size_t length, std::wstring& result) {
    if (length == 0) { result.clear(); return true; }
    const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(length), nullptr, 0);
    if (required <= 0) return false;
    result.resize(required);
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(length), result.data(), required) > 0;
}

bool IsSafeArchivePath(const std::wstring& path) {
    if (path.empty() || path.front() == L'/' || path.front() == L'\\' || path.find(L'\\') != std::wstring::npos ||
        path.find(L':') != std::wstring::npos) return false;

    size_t start = 0;
    while (start < path.size()) {
        const size_t end = path.find(L'/', start);
        const std::wstring component = path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (component.empty() || component == L"." || component == L"..") return false;
        start = end == std::wstring::npos ? path.size() : end + 1;
    }
    return true;
}

void SetError(std::wstring* error, const wchar_t* message) {
    if (error) *error = message;
}

} // namespace

bool PackageVerifier::VerifyNskryPluginArchive(const std::wstring& archivePath,
                                                VerifiedPackage& package,
                                                std::wstring* error) {
    package = {};
    const size_t extensionPos = archivePath.find_last_of(L'.');
    if (extensionPos == std::wstring::npos || _wcsicmp(archivePath.c_str() + extensionPos, L".nskryplugin") != 0) {
        SetError(error, L"Plugin package must use the .nskryplugin extension.");
        return false;
    }

    std::ifstream input(archivePath, std::ios::binary);
    if (!input) { SetError(error, L"Unable to open plugin package."); return false; }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() < 22) { SetError(error, L"Package is not a valid ZIP archive."); return false; }

    const size_t searchStart = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    size_t eocd = bytes.size();
    for (size_t pos = bytes.size() - 22;; --pos) {
        if (ReadU32(bytes, pos) == kEndOfCentralDirectory) { eocd = pos; break; }
        if (pos == searchStart) break;
    }
    if (eocd == bytes.size()) { SetError(error, L"ZIP end-of-directory record was not found."); return false; }

    const uint16_t diskNumber = ReadU16(bytes, eocd + 4);
    const uint16_t directoryDisk = ReadU16(bytes, eocd + 6);
    const uint16_t entryCount = ReadU16(bytes, eocd + 10);
    const uint32_t directorySize = ReadU32(bytes, eocd + 12);
    const uint32_t directoryOffset = ReadU32(bytes, eocd + 16);
    if (diskNumber != 0 || directoryDisk != 0 || entryCount == 0 || directoryOffset > bytes.size() ||
        directorySize > bytes.size() - directoryOffset) {
        SetError(error, L"Multi-volume, empty, or malformed ZIP packages are not supported.");
        return false;
    }

    size_t cursor = directoryOffset;
    const size_t directoryEnd = directoryOffset + directorySize;
    for (uint16_t index = 0; index < entryCount; ++index) {
        if (cursor + 46 > directoryEnd || ReadU32(bytes, cursor) != kCentralDirectoryEntry) {
            SetError(error, L"ZIP central directory is malformed.");
            return false;
        }
        const uint16_t flags = ReadU16(bytes, cursor + 8);
        const uint16_t method = ReadU16(bytes, cursor + 10);
        const uint16_t fileNameLength = ReadU16(bytes, cursor + 28);
        const uint16_t extraLength = ReadU16(bytes, cursor + 30);
        const uint16_t commentLength = ReadU16(bytes, cursor + 32);
        const size_t next = cursor + 46ull + fileNameLength + extraLength + commentLength;
        if (next > directoryEnd || (flags & 0x0001) != 0 || (method != 0 && method != 8)) {
            SetError(error, L"Package contains an encrypted or unsupported ZIP entry.");
            return false;
        }

        std::wstring path;
        if (!Utf8ToWide(reinterpret_cast<const char*>(bytes.data() + cursor + 46), fileNameLength, path)) {
            SetError(error, L"Package contains a non-UTF-8 file name.");
            return false;
        }
        if (!path.empty() && path.back() == L'/') path.pop_back();
        if (!path.empty()) {
            if (!IsSafeArchivePath(path)) { SetError(error, L"Package contains an unsafe archive path."); return false; }
            package.entries.push_back(path);
            if (path == L"manifest.json") package.hasManifest = true;
        }
        cursor = next;
    }

    if (!package.hasManifest) { SetError(error, L"Package must contain manifest.json at its root."); return false; }
    return true;
}

} // namespace nskry
