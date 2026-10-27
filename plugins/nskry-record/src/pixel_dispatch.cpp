#include "pixel_conversion.h"
#if NSKRY_RECORD_AVX2
#include <intrin.h>
#endif
namespace nskry::record {
#if NSKRY_RECORD_AVX2
void BgraToNv12Avx2(const uint8_t *, uint8_t *, unsigned, unsigned);
#endif
bool HasAvx2PixelConversion() {
    static const bool available = [] {
#if NSKRY_RECORD_AVX2
        int regs[4]{};
        __cpuid(regs, 0);
        if (regs[0] < 7)
            return false;
        __cpuid(regs, 1);
        if ((regs[2] & ((1 << 27) | (1 << 28))) != ((1 << 27) | (1 << 28)) || (_xgetbv(0) & 6) != 6)
            return false;
        __cpuidex(regs, 7, 0);
        return (regs[1] & (1 << 5)) != 0;
#else
        return false;
#endif
    }();
    return available;
}
void BgraToNv12(const uint8_t *bgra, uint8_t *output, unsigned width, unsigned height) {
#if NSKRY_RECORD_AVX2
    if ((width % 8) == 0 && HasAvx2PixelConversion()) {
        BgraToNv12Avx2(bgra, output, width, height);
        return;
    }
#endif
    BgraToNv12Sse2(bgra, output, width, height);
}
} // namespace nskry::record
