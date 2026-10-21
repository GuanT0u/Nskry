#include "record_delivery.h"
#include <cstring>
#include <filesystem>
#include <shellapi.h>
#include <shlobj.h>
#include <winrt/base.h>
namespace nskry::record {
namespace {
constexpr wchar_t Key[] = L"Software\\Nskry\\Plugins\\nskry-record";
}
Options LoadDefaults() {
    Options options;
    DWORD preset{}, presetSize = sizeof(preset);
    if (RegGetValueW(HKEY_CURRENT_USER, Key, L"VideoPreset", RRF_RT_REG_DWORD, nullptr, &preset, &presetSize) ==
            ERROR_SUCCESS &&
        DecodeDefaults(preset, options))
        return options;
    DWORD fps{}, quality{}, size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, Key, L"Fps", RRF_RT_REG_DWORD, nullptr, &fps, &size) == ERROR_SUCCESS &&
        (fps == 30 || fps == 60))
        options.fps = fps;
    size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, Key, L"Quality", RRF_RT_REG_DWORD, nullptr, &quality, &size) == ERROR_SUCCESS &&
        quality <= uint32_t(Quality::Hd))
        options.quality = Quality(quality);
    return options;
}
bool SaveDefaults(Options o) {
    if (!ValidOptions(o))
        return false;
    DWORD existing{}, size = sizeof(existing);
    const auto read = RegGetValueW(HKEY_CURRENT_USER, Key, L"VideoPreset", RRF_RT_REG_DWORD, nullptr, &existing, &size);
    Options previous;
    if (read != ERROR_FILE_NOT_FOUND && read != ERROR_PATH_NOT_FOUND &&
        (read != ERROR_SUCCESS || !DecodeDefaults(existing, previous)))
        return false; // Do not overwrite corrupt or future-version private configuration.
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, Key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const DWORD preset = EncodeDefaults(o);
    const bool ok = RegSetValueExW(key, L"VideoPreset", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&preset),
                                   sizeof(preset)) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}
std::wstring NewRecordingPath(bool gif) {
    PWSTR local{};
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local));
    std::filesystem::path root;
    try {
        root = std::filesystem::path(local) / L"Nskry" / L"cache" / L"recordings";
    } catch (...) {
        CoTaskMemFree(local);
        throw;
    }
    CoTaskMemFree(local);
    GUID guid{};
    winrt::check_hresult(CoCreateGuid(&guid));
    wchar_t text[40]{};
    if (!StringFromGUID2(guid, text, 40))
        winrt::throw_hresult(E_FAIL);
    return (root / (std::wstring(text) + (gif ? L".gif" : L".mp4"))).wstring();
}
void PrepareRecordingCache(const std::wstring &output, HANDLE canceled) {
    const auto root = std::filesystem::path(output).parent_path();
    std::filesystem::create_directories(root);
    // Copied files must survive app exit/clipboard history. Do not evict them
    // behind the user's back. Bound retained disk use instead of an idle janitor.
    uint64_t bytes = 0;
    size_t files = 0;
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (canceled && WaitForSingleObject(canceled, 0) == WAIT_OBJECT_0)
            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        if (++files > 2048)
            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES));
        if (entry.is_regular_file()) {
            bytes += entry.file_size();
            if (bytes > 1024ULL * 1024 * 1024)
                throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_DISK_FULL),
                                           L"Recording clipboard cache exceeds 1 GiB. Save any needed files, then "
                                           L"clear Nskry/cache/recordings.");
        }
    }
}
bool CopyRecordingFile(HWND owner, const std::wstring &path) {
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    const auto bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!data)
        return false;
    auto *drop = static_cast<DROPFILES *>(GlobalLock(data));
    if (!drop) {
        GlobalFree(data);
        return false;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    memcpy(reinterpret_cast<BYTE *>(drop) + sizeof(DROPFILES), path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(data);
    if (!OpenClipboard(owner)) {
        GlobalFree(data);
        return false;
    }
    const bool ok = EmptyClipboard() && SetClipboardData(CF_HDROP, data);
    if (ok) {
        const auto format = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (effect) {
            auto *value = static_cast<DWORD *>(GlobalLock(effect));
            if (value && format) {
                *value = DROPEFFECT_COPY;
                GlobalUnlock(effect);
                if (!SetClipboardData(format, effect)) GlobalFree(effect);
            } else {
                if (value) GlobalUnlock(effect);
                GlobalFree(effect);
            }
        }
    }
    CloseClipboard();
    if (!ok)
        GlobalFree(data);
    return ok;
}
} // namespace nskry::record
