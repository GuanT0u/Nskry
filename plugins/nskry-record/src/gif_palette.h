#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace nskry::record {
// Lossless fast path for UI frames using <=256 opaque colors. Fixed 512-slot
// table; stop immediately on the 257th color, never allocate a pixel dictionary.
inline bool ExactGifPalette(const uint8_t *bgra, size_t pixels, uint8_t *indices, std::array<uint32_t, 256> &colors,
                            uint32_t &count) {
    std::array<uint32_t, 512> keys;
    keys.fill(UINT32_MAX);
    std::array<uint8_t, 512> values{};
    count = 0;
    for (size_t i = 0; i < pixels; ++i) {
        const auto *p = bgra + i * 4;
        const uint32_t key = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
        auto slot = (key * 2654435761u) >> 23;
        while (keys[slot] != UINT32_MAX && keys[slot] != key)
            slot = (slot + 1) & 511;
        if (keys[slot] == UINT32_MAX) {
            if (count == 256)
                return false;
            keys[slot] = key;
            values[slot] = uint8_t(count);
            colors[count++] = key | 0xff000000;
        }
        indices[i] = values[slot];
    }
    return count > 0;
}
} // namespace nskry::record
