#pragma once
#include <cstdint>
#include <string>

namespace nskry::record {
struct AudioSource {
    std::wstring endpoint;
    uint32_t pid{};
    uint64_t processCreated{};
    bool Enabled() const { return pid || !endpoint.empty(); }
};
constexpr uint32_t AudioRate = 44100;
constexpr uint32_t AudioBlock = 4;
constexpr int64_t AudioTime(int64_t frames) {
    return frames / AudioRate * 10000000 + frames % AudioRate * 10000000 / AudioRate;
}
constexpr int64_t AudioFrames(int64_t time) {
    return time / 10000000 * AudioRate + time % 10000000 * AudioRate / 10000000;
}
}
