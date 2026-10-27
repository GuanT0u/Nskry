#include "pixel_conversion.h"
#include "record_worker_protocol.h"
#include <array>
#include <chrono>
#include <iostream>
#include <random>
#include <vector>
using namespace nskry::record;
namespace {
void Reference(const uint8_t *bgra, uint8_t *out, unsigned width, unsigned height) {
    auto clamp = [](int v) { return uint8_t(std::clamp(v, 0, 255)); };
    for (unsigned i = 0; i < width * height; ++i) {
        const auto *p = bgra + i * 4;
        out[i] = clamp(((47 * p[2] + 157 * p[1] + 16 * p[0] + 128) >> 8) + 16);
    }
    auto *uv = out + width * height;
    for (unsigned y = 0; y < height; y += 2)
        for (unsigned x = 0; x < width; x += 2) {
            int b = 0, g = 0, r = 0;
            for (unsigned yy = 0; yy < 2; ++yy)
                for (unsigned xx = 0; xx < 2; ++xx) {
                    const auto *p = bgra + ((y + yy) * width + x + xx) * 4;
                    b += p[0];
                    g += p[1];
                    r += p[2];
                }
            b /= 4;
            g /= 4;
            r /= 4;
            uv[y / 2 * width + x] = clamp(((-26 * r - 86 * g + 112 * b + 128) >> 8) + 128);
            uv[y / 2 * width + x + 1] = clamp(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
        }
}
int Benchmark() {
    constexpr unsigned width = 1920, height = 1080, iterations = 100;
    std::vector<uint8_t> input(width * height * 4), reference(width * height * 3 / 2), actual(reference.size());
    std::mt19937 rng(456);
    for (auto &v : input)
        v = uint8_t(rng());
    auto measure = [&](auto convert, auto &output) {
        for (int i = 0; i < 5; ++i)
            convert(input.data(), output.data(), width, height);
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < iterations; ++i)
            convert(input.data(), output.data(), width, height);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    std::cout << "1080p conversion only, iterations=" << iterations << " avx2=" << HasAvx2PixelConversion() << '\n';
    for (int round=0; round<5; ++round) {
        double oldMs{}, newMs{};
        if (round % 2) {
            newMs=measure(BgraToNv12, actual); oldMs=measure(BgraToNv12Sse2, reference);
        } else {
            oldMs=measure(BgraToNv12Sse2, reference); newMs=measure(BgraToNv12, actual);
        }
        std::cout << "round=" << round << " sse2_ms=" << oldMs << " selected_ms=" << newMs
                  << " byte_equal=" << (reference == actual) << '\n';
        if (reference != actual) return 8;
    }
    return reference == actual ? 0 : 8;
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--benchmark"))
        return Benchmark();
    if (argc != 1)
        return 9;
    wire::Header h;
    h.width = 320;
    h.height = 180;
    h.fps = 20;
    h.pixelBytes = 320 * 180 * 3 / 2;
    wcscpy_s(h.path, L"X:\\test.partial.mp4");
    if (!wire::ValidHeader(h))
        return 1;
    for (int fault = 0; fault < 9; ++fault) {
        auto bad = h;
        switch (fault) {
        case 0:
            ++bad.magic;
            break;
        case 1:
            ++bad.version;
            break;
        case 2:
            --bad.headerBytes;
            break;
        case 3:
            ++bad.pixelBytes;
            break;
        case 4:
            ++bad.width;
            break;
        case 5:
            bad.height = UINT32_MAX;
            break;
        case 6:
            bad.fps = 0;
            break;
        case 7:
            bad.path[32767] = L'x';
            break;
        case 8:
            bad.hardware = 2;
            break;
        }
        if (wire::ValidHeader(bad))
            return 2;
    }
    auto preview = h;
    preview.preview = 1;
    preview.sourceWidth = 1920;
    preview.sourceHeight = 1080;
    preview.pixelBytes = preview.width * preview.height * 4;
    if (!wire::ValidHeader(preview))
        return 10;
    for (int fault = 0; fault < 4; ++fault) {
        auto bad = preview;
        if (fault == 0)
            bad.preview = 2;
        if (fault == 1)
            bad.width = 642;
        if (fault == 2)
            bad.height = 362;
        if (fault == 3)
            bad.sourceWidth = 0;
        if (fault == 1 || fault == 2)
            bad.pixelBytes = bad.width * bad.height * 4;
        if (wire::ValidHeader(bad))
            return 11;
    }
    HANDLE handle{};
    if (!wire::ParseHandle(L"123", handle) || reinterpret_cast<uintptr_t>(handle) != 123)
        return 3;
    for (const auto text : {L"", L"0", L"-1", L"+2", L" 12", L"12x", L"18446744073709551616", L"18446744073709551615"})
        if (wire::ParseHandle(text, handle))
            return 4;
    if (ValidFrameFormat(2, 1000000, 20) || ValidFrameFormat(1920, 1920, 20) || ValidFrameFormat(1440, 1440, 20) ||
        !ValidFrameFormat(1080, 1920, 20))
        return 5;
    std::mt19937 rng(123);
    for (unsigned side = 2; side <= 32; side += 2) {
        const unsigned width = side, height = side + 2, count = width * height;
        std::vector<uint8_t> bgra(count * 4), output(count * 3 / 2 + 16, 0xAB), reference(count * 3 / 2);
        for (auto &v : bgra)
            v = uint8_t(rng());
        BgraToNv12(bgra.data(), output.data(), width, height);
        // Independent BT.709 integer reference; SIMD must not change rounding.
        auto clamp = [](int v) { return uint8_t(std::clamp(v, 0, 255)); };
        for (unsigned i = 0; i < count; ++i) {
            const auto *p = bgra.data() + i * 4;
            reference[i] = clamp(((47 * p[2] + 157 * p[1] + 16 * p[0] + 128) >> 8) + 16);
        }
        for (unsigned y = 0; y < height; y += 2)
            for (unsigned x = 0; x < width; x += 2) {
                std::array<int, 3> sum{};
                for (unsigned yy = 0; yy < 2; ++yy)
                    for (unsigned xx = 0; xx < 2; ++xx)
                        for (unsigned c = 0; c < 3; ++c)
                            sum[c] += bgra[((y + yy) * width + x + xx) * 4 + c];
                const int b = sum[0] / 4, g = sum[1] / 4, r = sum[2] / 4;
                reference[count + y / 2 * width + x] = clamp(((-26 * r - 86 * g + 112 * b + 128) >> 8) + 128);
                reference[count + y / 2 * width + x + 1] = clamp(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
            }
        if (!std::equal(reference.begin(), reference.end(), output.begin()))
            return 6;
        for (size_t i = reference.size(); i < output.size(); ++i)
            if (output[i] != 0xAB)
                return 7;
    }
    for (unsigned offset : {1u, 3u, 7u, 15u}) {
        constexpr unsigned width=24, height=14, count=width*height;
        std::vector<uint8_t> bgra(count*4+offset), output(count*3/2+offset+16,0xAB), reference(count*3/2);
        for (auto& v : bgra) v=uint8_t(rng());
        BgraToNv12(bgra.data()+offset, output.data()+offset, width, height);
        Reference(bgra.data()+offset, reference.data(), width, height);
        if (!std::equal(reference.begin(),reference.end(),output.begin()+offset)) return 12;
        for (unsigned i=0; i<offset; ++i) if(output[i]!=0xAB) return 13;
        for (size_t i=offset+reference.size(); i<output.size(); ++i) if(output[i]!=0xAB) return 14;
    }
    std::cout << "Private wire bounds, unaligned buffers and exact NV12 conversion passed, avx2="
              << HasAvx2PixelConversion() << '\n';
}
