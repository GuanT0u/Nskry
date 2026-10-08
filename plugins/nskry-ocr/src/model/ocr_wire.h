#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nskry::ocr::wire {
constexpr uint32_t Magic = 0x3152434f; // OCR1, little-endian Windows x64 protocol.
constexpr uint32_t Version = 1;
constexpr uint32_t MaxPixels = 16 * 1024 * 1024;
constexpr uint32_t MaxReplyBytes = 16 * 1024 * 1024;
constexpr uint32_t MaxLines = 2000;
constexpr uint32_t MaxTextUnits = 100000;
#pragma pack(push, 1)
struct Input {
    uint32_t magic{Magic}, version{Version}, width{}, height{}, pixelBytes{};
};
struct Rect { int32_t left{}, top{}, right{}, bottom{}; };
struct Glyph { Rect rect; uint32_t offset{}, length{}; };
struct LineHeader { Rect rect; float confidence{}; uint32_t textUnits{}, glyphCount{}; };
struct ReplyHeader {
    uint32_t magic{Magic}, version{Version}, status{}, lineCount{}, payloadBytes{}, errorUnits{};
    double loadMs{}, inferenceMs{};
    double cpuMs{};
    uint64_t peakWorkingSetBytes{}, peakCommitBytes{};
};
#pragma pack(pop)
struct Line {
    Rect rect;
    float confidence{};
    std::wstring text;
    std::vector<Glyph> glyphs;
};
struct Reply {
    std::vector<Line> lines;
    std::wstring error;
    double loadMs{}, inferenceMs{};
    double cpuMs{};
    uint64_t peakWorkingSetBytes{}, peakCommitBytes{};
};
bool ValidInput(const Input& input);
std::vector<uint8_t> Encode(const Reply& reply);
bool Decode(std::span<const uint8_t> bytes, int width, int height, Reply& reply);
} // namespace nskry::ocr::wire
