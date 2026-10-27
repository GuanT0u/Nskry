#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace nskry::record {
namespace pixels {
#if defined(_M_X64) || defined(__SSE2__)
inline __m128i SumPairs(__m128i value) {
    value = _mm_add_epi32(value, _mm_shuffle_epi32(value, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_shuffle_epi32(value, _MM_SHUFFLE(2, 0, 2, 0));
}
inline void StoreFour(uint8_t *to, __m128i value) {
    const auto zero = _mm_setzero_si128();
    const auto packed = _mm_packus_epi16(_mm_packs_epi32(value, zero), zero);
    const auto bytes = _mm_cvtsi128_si32(packed);
    std::memcpy(to, &bytes, sizeof(bytes)); // unaligned, no aliasing assumptions
}
inline __m128i Luma(__m128i row) {
    const auto zero = _mm_setzero_si128();
    const auto coefficients = _mm_setr_epi16(16, 157, 47, 0, 16, 157, 47, 0);
    const auto lo = SumPairs(_mm_madd_epi16(_mm_unpacklo_epi8(row, zero), coefficients));
    const auto hi = SumPairs(_mm_madd_epi16(_mm_unpackhi_epi8(row, zero), coefficients));
    auto values = _mm_unpacklo_epi64(lo, hi);
    values = _mm_srai_epi32(_mm_add_epi32(values, _mm_set1_epi32(128)), 8);
    return _mm_add_epi32(values, _mm_set1_epi32(16));
}
// Four pixels across each of two rows; averaging is floor(sum/4), not
// rounded pavg, preserving the scalar formula and its chroma exactly.
inline void Block(const uint8_t *a, const uint8_t *b, uint8_t *ya, uint8_t *yb, uint8_t *uv) {
    const auto row0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(a));
    const auto row1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(b));
    StoreFour(ya, Luma(row0));
    StoreFour(yb, Luma(row1));
    const auto zero = _mm_setzero_si128();
    auto lo = _mm_add_epi16(_mm_unpacklo_epi8(row0, zero), _mm_unpacklo_epi8(row1, zero));
    auto hi = _mm_add_epi16(_mm_unpackhi_epi8(row0, zero), _mm_unpackhi_epi8(row1, zero));
    lo = _mm_srli_epi16(_mm_add_epi16(lo, _mm_srli_si128(lo, 8)), 2);
    hi = _mm_srli_epi16(_mm_add_epi16(hi, _mm_srli_si128(hi, 8)), 2);
    const auto average = _mm_unpacklo_epi64(lo, hi);
    const auto u = SumPairs(_mm_madd_epi16(average, _mm_setr_epi16(112, -86, -26, 0, 112, -86, -26, 0)));
    const auto v = SumPairs(_mm_madd_epi16(average, _mm_setr_epi16(-10, -102, 112, 0, -10, -102, 112, 0)));
    auto chroma = _mm_unpacklo_epi32(u, v);
    chroma = _mm_srai_epi32(_mm_add_epi32(chroma, _mm_set1_epi32(128)), 8);
    StoreFour(uv, _mm_add_epi32(chroma, _mm_set1_epi32(128)));
}
#endif
} // namespace pixels
// Tightly packed BT.709 limited-range NV12. Caller validates even dimensions
// and owns both buffers. SSE2 is baseline on x64, not AVX/modern-CPU-only.
inline void BgraToNv12Sse2(const uint8_t *bgra, uint8_t *output, unsigned width, unsigned height) {
    const auto clamp = [](int n) { return uint8_t(std::clamp(n, 0, 255)); };
    auto *uv = output + uint64_t(width) * height;
    for (unsigned y = 0; y < height; y += 2) {
        unsigned x = 0;
#if defined(_M_X64) || defined(__SSE2__)
        for (; x + 4 <= width; x += 4)
            pixels::Block(bgra + (uint64_t(y) * width + x) * 4, bgra + (uint64_t(y + 1) * width + x) * 4,
                          output + uint64_t(y) * width + x, output + uint64_t(y + 1) * width + x,
                          uv + uint64_t(y / 2) * width + x);
#endif
        for (; x < width; x += 2) {
            int r = 0, g = 0, b = 0;
            for (unsigned yy = 0; yy < 2; ++yy)
                for (unsigned xx = 0; xx < 2; ++xx) {
                    const auto *p = bgra + (uint64_t(y + yy) * width + x + xx) * 4;
                    output[uint64_t(y + yy) * width + x + xx] =
                        clamp(((47 * p[2] + 157 * p[1] + 16 * p[0] + 128) >> 8) + 16);
                    b += p[0];
                    g += p[1];
                    r += p[2];
                }
            r /= 4;
            g /= 4;
            b /= 4;
            uv[uint64_t(y / 2) * width + x] = clamp(((-26 * r - 86 * g + 112 * b + 128) >> 8) + 128);
            uv[uint64_t(y / 2) * width + x + 1] = clamp(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
        }
    }
}
bool HasAvx2PixelConversion();
void BgraToNv12(const uint8_t *bgra, uint8_t *output, unsigned width, unsigned height);
} // namespace nskry::record
