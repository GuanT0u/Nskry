#pragma once
#include "record_policy.h"
#include <cstdint>
#include <type_traits>
#include <windows.h>

namespace nskry::record::wire {
// PRIVATE x64 child protocol, not a public SDK extension. One request/response
// slot, synchronized by inherited events. No pointers, textures or D3D contexts.
constexpr uint32_t Magic = 0x4E524543, Version = 3;
enum class Command : uint32_t {
    Open = 1,
    Frame,
    Finalize,
    Close,
    Trim,
    Preview
};
struct alignas(8) Header {
    uint32_t magic = Magic, version = Version;
    uint32_t headerBytes = sizeof(Header), pixelBytes{};
    uint32_t width{}, height{}, fps{}, hardware{};
    uint32_t bitrate{};
    uint32_t gif{};
    uint32_t preview{}, sourceWidth{}, sourceHeight{};
    uint64_t request{}, response{};
    int64_t time{}, duration{};
    int64_t trimStart{}, trimEnd{};
    uint64_t samples{};
    wchar_t inputPath[32768]{};
    Command command{};
    HRESULT result = E_PENDING;
    volatile LONG created{}, finalized{};
    wchar_t backend[128]{}, error[512]{}, path[32768]{};
};
static_assert(std::is_standard_layout_v<Header> && std::is_trivially_copyable_v<Header>);
inline bool ValidHeader(const Header &h) {
    return h.magic == Magic && h.version == Version && h.headerBytes == sizeof(Header) &&
           ValidFrameFormat(h.width, h.height, h.fps) && h.gif <= 1 && h.preview <= 1 &&
           (!h.preview || (h.width <= 640 && h.height <= 360 && ValidFrameFormat(h.sourceWidth, h.sourceHeight, h.fps))) &&
           (!h.gif || (h.fps == 10 && h.width <= 960 && h.height <= 960 && uint64_t(h.width) * h.height <= 960 * 540)) &&
           h.pixelBytes == uint64_t(h.width) * h.height * (h.gif || h.preview ? 4 : 3) / (h.gif || h.preview ? 1 : 2) &&
           h.hardware <= 1 && (!h.bitrate || (h.bitrate >= 500000 && h.bitrate <= 24000000)) &&
           h.path[0] && h.path[32767] == 0;
}
// Decimal handles only. Never permit pseudo handles, signs or overflow.
inline bool ParseHandle(const wchar_t *text, HANDLE &value) {
    uintptr_t n = 0;
    if (!text || !*text)
        return false;
    for (; *text; ++text) {
        if (*text < L'0' || *text > L'9' || n > (UINTPTR_MAX - unsigned(*text - L'0')) / 10)
            return false;
        n = n * 10 + unsigned(*text - L'0');
    }
    if (!n || n >= UINTPTR_MAX - 16)
        return false;
    value = reinterpret_cast<HANDLE>(n);
    return true;
}
} // namespace nskry::record::wire
