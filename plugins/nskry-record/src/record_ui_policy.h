#pragma once
#include <algorithm>
#include <cstdint>
#include <windows.h>
namespace nskry::record {
struct RecordingSetupWidths {
    bool compact{};
    int left{}, status{}, fps{}, quality{}, follow{}, defaults{}, start{}, cancel{}, gap{};
    int Required() const {
        return left + status + fps + quality + follow + defaults + start + cancel + gap * (compact ? 4 : 5);
    }
};
inline RecordingSetupWidths SetupWidths(int available) {
    const bool compact = available < 640;
    if (!compact)
        return {false, 14, (std::max)(45, available - 570), 64, 84, 122, 94, 64, 72, 6};
    constexpr int fps = 45, quality = 55, follow = 65, start = 50, cancel = 52;
    constexpr int fields = fps + quality + follow + start + cancel;
    const double scale = std::clamp((available - 8.0 - 12.0) / fields, 0.6, 1.0);
    return {true, 8, 0, int(fps * scale), int(quality * scale), int(follow * scale), 0, int(start * scale),
            int(cancel * scale), 3};
}
inline RECT RecordingBarBounds(RECT region, RECT work, int width, int height) {
    width = (std::min)(width, int(work.right - work.left) - 8);
    height = (std::min)(height, int(work.bottom - work.top) - 8);
    const int left = std::clamp<int>((region.left + region.right - width) / 2, work.left + 4, work.right - width - 4);
    int top = region.bottom + 8;
    if (top + height > work.bottom - 4)
        top = region.top - height - 8;
    if (top < work.top + 4)
        top = region.bottom - height - 8;
    top = std::clamp<int>(top, work.top + 4, work.bottom - height - 4);
    return {left, top, left + width, top + height};
}
inline int64_t TrimTimeAt(int x, int left, int right, int64_t duration) {
    if (right <= left || duration <= 0)
        return 0;
    return int64_t((long double)std::clamp(x - left, 0, right - left) * duration / (right - left));
}
} // namespace nskry::record
