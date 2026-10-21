#pragma once
#include <algorithm>
#include <cstdint>
namespace nskry::record {
inline bool ValidTrim(int64_t start, int64_t end, int64_t duration) {
    return start >= 0 && end > start && end <= duration;
}
struct TrimSample {
    int64_t time{}, duration{};
};
inline TrimSample ClipSample(int64_t time, int64_t duration, int64_t start, int64_t end) {
    if (time < 0 || duration <= 0 || time > INT64_MAX - duration || start < 0 || end <= start)
        return {};
    const auto first = std::max(time, start), last = std::min(time + duration, end);
    return last > first ? TrimSample{first - start, last - first} : TrimSample{};
}
} // namespace nskry::record
