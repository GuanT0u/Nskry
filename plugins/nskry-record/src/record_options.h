#pragma once
#include <algorithm>
#include <cstdint>

namespace nskry::record {
enum class Quality : uint32_t {
    Medium,
    High,
    Hd
};
struct Options {
    uint32_t fps = 30;
    Quality quality = Quality::High;
};
inline bool ValidOptions(const Options &o) {
    return (o.fps == 30 || o.fps == 60) && uint32_t(o.quality) <= uint32_t(Quality::Hd);
}
inline uint32_t EncodeDefaults(const Options &o) {
    return ValidOptions(o) ? (1u << 16) | (o.fps << 8) | uint32_t(o.quality) : 0;
}
inline bool DecodeDefaults(uint32_t value, Options &o) {
    const Options decoded{(value >> 8) & 255, Quality(value & 255)};
    if ((value >> 16) != 1 || !ValidOptions(decoded))
        return false;
    o = decoded;
    return true;
}
// Resolution-aware presets: cap the encoder target, never queue more frames to
// meet a bitrate. The actual hardware/software backend remains observable.
inline uint32_t Bitrate(uint32_t w, uint32_t h, uint32_t fps, Quality quality) {
    const uint32_t divisor = quality == Quality::Hd ? 4 : quality == Quality::High ? 7 : 12;
    return uint32_t(std::clamp(uint64_t(w) * h * fps / divisor, uint64_t(500000), uint64_t(24000000)));
}
} // namespace nskry::record
