// This translation unit alone is built with AVX2. The caller checks CPU and
// OS YMM-state support before entering it; baseline binaries remain SSE2-safe.
#include <immintrin.h>
#include <cstdint>
namespace nskry::record {
namespace {
__m256i SumPairs(__m256i value) {
    value = _mm256_add_epi32(value, _mm256_shuffle_epi32(value, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm256_shuffle_epi32(value, _MM_SHUFFLE(2, 0, 2, 0));
}
void StoreEight(uint8_t *to, __m256i value) {
    const auto zero = _mm256_setzero_si256();
    const auto packed = _mm256_packus_epi16(_mm256_packs_epi32(value, zero), zero);
    const auto joined =
        _mm_unpacklo_epi32(_mm256_castsi256_si128(packed), _mm256_extracti128_si256(packed, 1));
    _mm_storel_epi64(reinterpret_cast<__m128i *>(to), joined);
}
__m256i Luma(__m256i row) {
    const auto zero = _mm256_setzero_si256();
    const auto coefficients =
        _mm256_setr_epi16(16, 157, 47, 0, 16, 157, 47, 0, 16, 157, 47, 0, 16, 157, 47, 0);
    const auto lo = SumPairs(_mm256_madd_epi16(_mm256_unpacklo_epi8(row, zero), coefficients));
    const auto hi = SumPairs(_mm256_madd_epi16(_mm256_unpackhi_epi8(row, zero), coefficients));
    auto values = _mm256_unpacklo_epi64(lo, hi);
    values = _mm256_srai_epi32(_mm256_add_epi32(values, _mm256_set1_epi32(128)), 8);
    return _mm256_add_epi32(values, _mm256_set1_epi32(16));
}
} // namespace
void BgraToNv12Avx2(const uint8_t *bgra, uint8_t *output, unsigned width, unsigned height) {
    auto *uv = output + uint64_t(width) * height;
    for (unsigned y = 0; y < height; y += 2) {
        for (unsigned x = 0; x < width; x += 8) {
            const auto row0 =
                _mm256_loadu_si256(reinterpret_cast<const __m256i *>(bgra + (uint64_t(y) * width + x) * 4));
            const auto row1 = _mm256_loadu_si256(
                reinterpret_cast<const __m256i *>(bgra + (uint64_t(y + 1) * width + x) * 4));
            StoreEight(output + uint64_t(y) * width + x, Luma(row0));
            StoreEight(output + uint64_t(y + 1) * width + x, Luma(row1));
            const auto zero = _mm256_setzero_si256();
            auto lo = _mm256_add_epi16(_mm256_unpacklo_epi8(row0, zero), _mm256_unpacklo_epi8(row1, zero));
            auto hi = _mm256_add_epi16(_mm256_unpackhi_epi8(row0, zero), _mm256_unpackhi_epi8(row1, zero));
            lo = _mm256_srli_epi16(_mm256_add_epi16(lo, _mm256_srli_si256(lo, 8)), 2);
            hi = _mm256_srli_epi16(_mm256_add_epi16(hi, _mm256_srli_si256(hi, 8)), 2);
            const auto average = _mm256_unpacklo_epi64(lo, hi);
            const auto u =
                SumPairs(_mm256_madd_epi16(average, _mm256_setr_epi16(112, -86, -26, 0, 112, -86, -26, 0, 112,
                                                                      -86, -26, 0, 112, -86, -26, 0)));
            const auto v =
                SumPairs(_mm256_madd_epi16(average, _mm256_setr_epi16(-10, -102, 112, 0, -10, -102, 112, 0,
                                                                      -10, -102, 112, 0, -10, -102, 112, 0)));
            auto chroma = _mm256_unpacklo_epi32(u, v);
            chroma = _mm256_srai_epi32(_mm256_add_epi32(chroma, _mm256_set1_epi32(128)), 8);
            StoreEight(uv + uint64_t(y / 2) * width + x, _mm256_add_epi32(chroma, _mm256_set1_epi32(128)));
        }
    }
}
} // namespace nskry::record
