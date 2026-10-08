#include "ocr_wire.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace nskry::ocr::wire {
static_assert(sizeof(wchar_t) == 2, "Wire text is UTF-16 on Windows");
static_assert(sizeof(Input) == 20 && sizeof(Glyph) == 24 && sizeof(ReplyHeader) == 64);
bool ValidInput(const Input& input) {
    const uint64_t pixels = static_cast<uint64_t>(input.width) * input.height;
    return input.magic == Magic && input.version == Version && input.width && input.height &&
        input.width <= 8192 && input.height <= 8192 && pixels <= MaxPixels && input.pixelBytes == pixels * 4;
}
namespace {
template<class T> void Append(std::vector<uint8_t>& bytes, const T& value) {
    const auto* begin = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), begin, begin + sizeof(T));
}
void AppendText(std::vector<uint8_t>& bytes, const std::wstring& text) {
    const auto* begin = reinterpret_cast<const uint8_t*>(text.data());
    bytes.insert(bytes.end(), begin, begin + text.size() * 2);
}
struct Reader {
    std::span<const uint8_t> bytes;
    template<class T> bool Read(T& result) {
        if (bytes.size() < sizeof(T)) return false;
        std::memcpy(&result, bytes.data(), sizeof(T));
        bytes = bytes.subspan(sizeof(T));
        return true;
    }
    bool Text(uint32_t units, std::wstring& result) {
        if (units > MaxTextUnits || bytes.size() < static_cast<size_t>(units) * 2) return false;
        result.resize(units);
        if (units) std::memcpy(result.data(), bytes.data(), static_cast<size_t>(units) * 2);
        bytes = bytes.subspan(static_cast<size_t>(units) * 2);
        // Reject NUL and unpaired surrogates; preserve actual Unicode text.
        for (size_t i = 0; i < result.size(); ++i) {
            const auto ch = static_cast<uint16_t>(result[i]);
            if (!ch) return false;
            if (ch >= 0xd800 && ch <= 0xdbff) {
                if (++i == result.size() || result[i] < 0xdc00 || result[i] > 0xdfff) return false;
            } else if (ch >= 0xdc00 && ch <= 0xdfff) return false;
        }
        return true;
    }
};
bool ValidRect(const Rect& rect, int width, int height) {
    return rect.left >= 0 && rect.top >= 0 && rect.right > rect.left && rect.bottom > rect.top &&
        rect.right <= width && rect.bottom <= height;
}
}
std::vector<uint8_t> Encode(const Reply& reply) {
    if (reply.lines.size() > MaxLines || reply.error.size() > MaxTextUnits) throw std::length_error("OCR reply limit");
    ReplyHeader header;
    header.status = reply.error.empty() ? 0 : 1;
    header.lineCount = header.status ? 0 : static_cast<uint32_t>(reply.lines.size());
    header.errorUnits = static_cast<uint32_t>(reply.error.size());
    header.loadMs = reply.loadMs; header.inferenceMs = reply.inferenceMs;
    header.cpuMs = reply.cpuMs; header.peakWorkingSetBytes = reply.peakWorkingSetBytes; header.peakCommitBytes = reply.peakCommitBytes;
    std::vector<uint8_t> body;
    if (header.status) AppendText(body, reply.error);
    else for (const auto& line : reply.lines) {
        if (line.text.size() > MaxTextUnits || line.glyphs.size() > line.text.size()) throw std::length_error("OCR line limit");
        Append(body, LineHeader{line.rect, line.confidence, static_cast<uint32_t>(line.text.size()),
            static_cast<uint32_t>(line.glyphs.size())});
        AppendText(body, line.text);
        for (const auto& glyph : line.glyphs) Append(body, glyph);
        if (body.size() > MaxReplyBytes - sizeof(ReplyHeader)) throw std::length_error("OCR reply limit");
    }
    header.payloadBytes = static_cast<uint32_t>(body.size());
    std::vector<uint8_t> result;
    result.reserve(sizeof(header) + body.size()); Append(result, header);
    result.insert(result.end(), body.begin(), body.end());
    return result;
}
bool Decode(std::span<const uint8_t> bytes, int width, int height, Reply& reply) {
    reply = {};
    if (bytes.size() > MaxReplyBytes || width <= 0 || height <= 0) return false;
    Reader reader{bytes}; ReplyHeader header;
    if (!reader.Read(header) || header.magic != Magic || header.version != Version || header.status > 1 ||
        header.payloadBytes != reader.bytes.size() || header.lineCount > MaxLines ||
        !std::isfinite(header.loadMs) || !std::isfinite(header.inferenceMs) || !std::isfinite(header.cpuMs) ||
        header.loadMs < 0 || header.inferenceMs < 0 || header.cpuMs < 0) return false;
    Reply value;
    value.loadMs = header.loadMs; value.inferenceMs = header.inferenceMs;
    value.cpuMs = header.cpuMs; value.peakWorkingSetBytes = header.peakWorkingSetBytes; value.peakCommitBytes = header.peakCommitBytes;
    if (header.status) {
        if (header.lineCount || !header.errorUnits || !reader.Text(header.errorUnits, value.error)) return false;
    } else {
        if (header.errorUnits) return false;
        uint32_t totalUnits = 0;
        for (uint32_t index = 0; index < header.lineCount; ++index) {
            LineHeader lineHeader;
            if (!reader.Read(lineHeader) || !ValidRect(lineHeader.rect, width, height) ||
                !std::isfinite(lineHeader.confidence) || lineHeader.confidence < 0 || lineHeader.confidence > 1 ||
                !lineHeader.textUnits || lineHeader.textUnits > MaxTextUnits - totalUnits ||
                lineHeader.glyphCount > lineHeader.textUnits) return false;
            totalUnits += lineHeader.textUnits;
            Line line{lineHeader.rect, lineHeader.confidence};
            if (!reader.Text(lineHeader.textUnits, line.text)) return false;
            uint32_t previousEnd = 0;
            for (uint32_t glyphIndex = 0; glyphIndex < lineHeader.glyphCount; ++glyphIndex) {
                Glyph glyph;
                if (!reader.Read(glyph) || !ValidRect(glyph.rect, width, height) || !glyph.length ||
                    glyph.offset < previousEnd || glyph.offset > line.text.size() ||
                    glyph.length > line.text.size() - glyph.offset) return false;
                if ((line.text[glyph.offset] >= 0xdc00 && line.text[glyph.offset] <= 0xdfff) ||
                    (line.text[glyph.offset + glyph.length - 1] >= 0xd800 && line.text[glyph.offset + glyph.length - 1] <= 0xdbff)) return false;
                previousEnd = glyph.offset + glyph.length;
                line.glyphs.push_back(glyph);
            }
            value.lines.push_back(std::move(line));
        }
    }
    if (!reader.bytes.empty()) return false;
    reply = std::move(value);
    return true;
}
} // namespace nskry::ocr::wire
