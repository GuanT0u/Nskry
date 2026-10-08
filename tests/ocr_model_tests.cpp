#include "ocr_process.h"
#include "core/json.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace nskry::ocr;
namespace {
std::string Json(const std::wstring& text) {
    std::string result;
    if (!nskry::json::ToUtf8(nskry::json::EscapeString(text), result)) throw std::runtime_error("Invalid UTF16 output");
    return '"' + result + '"';
}
std::wstring Content(const std::wstring& text) {
    std::wstring result;
    for (auto ch : text) if ((ch >= 0x3400 && ch <= 0x9fff) || iswalnum(ch)) result += ch;
    return result;
}
std::wstring ReadUtf8(const wchar_t* path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) throw std::runtime_error("Missing expected text");
    const std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid expected text");
    std::wstring value(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), value.data(), count);
    return value;
}
bool Regions(const wire::Reply& reply, const wchar_t* path) {
    nskry::json::Value value;
    if (!nskry::json::ParseUtf8File(path, value) || !value.IsArray()) return false;
    for (const auto& item : *value.AsArray()) {
        std::wstring expected;
        if (!item.GetString(L"text", expected)) return false;
        const auto* array = item.Find(L"rect");
        if (!array || !array->IsArray() || array->AsArray()->size() != 4) return false;
        std::array<double, 4> rect{};
        for (size_t i = 0; i < 4; ++i) {
            const auto* number = (*array->AsArray())[i].AsNumber(); if (!number) return false;
            rect[i] = *number;
        }
        std::wstring observed;
        for (const auto& line : reply.lines) {
            const auto& box = line.rect;
            const double area = static_cast<double>(box.right - box.left) * (box.bottom - box.top);
            const double overlap = std::max(0.0, std::min(rect[2], static_cast<double>(box.right)) - std::max(rect[0], static_cast<double>(box.left))) *
                std::max(0.0, std::min(rect[3], static_cast<double>(box.bottom)) - std::max(rect[1], static_cast<double>(box.top)));
            if (overlap >= area * .5) observed += line.text;
        }
        if (Content(observed).find(Content(expected)) == std::wstring::npos) {
            std::cerr << "Missing required spatial text: " << Json(expected) << " observed=" << Json(observed) << '\n'; return false;
        }
    }
    return true;
}
bool Exited(DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return true;
    const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process); return exited;
}
void WriteRun(std::ostream& out, int run, const ModelResult& result) {
    out << "{\"run\":" << run << ",\"engine\":\"PP-OCRv4 native / ORT 1.29.0\",\"model_pass_count\":1,\"threads\":" << result.threads
        << ",\"load_ms\":" << result.reply.loadMs << ",\"inference_ms\":" << result.reply.inferenceMs
        << ",\"end_to_end_ms\":" << result.elapsedMs << ",\"cpu_s\":" << result.reply.cpuMs / 1000
        << ",\"process_id\":" << result.processId << ",\"worker_exited\":true,\"memory_MiB\":{\"PeakWorkingSetSize\":"
        << result.reply.peakWorkingSetBytes / 1048576.0 << ",\"PeakPagefileUsage\":" << result.reply.peakCommitBytes / 1048576.0 << "},\"lines\":[";
    bool first = true;
    for (const auto& line : result.reply.lines) {
        if (!first) out << ','; first = false;
        const auto& box = line.rect;
        out << "{\"text\":" << Json(line.text) << ",\"score\":" << line.confidence
            << ",\"box\":[[" << box.left << ',' << box.top << "],[" << box.right << ',' << box.top << "],[" << box.right << ',' << box.bottom
            << "],[" << box.left << ',' << box.bottom << "]],\"glyphs\":[";
        bool firstGlyph = true;
        for (const auto& glyph : line.glyphs) {
            if (!firstGlyph) out << ','; firstGlyph = false;
            out << "{\"text\":" << Json(line.text.substr(glyph.offset, glyph.length)) << ",\"rect\":["
                << glyph.rect.left << ',' << glyph.rect.top << ',' << glyph.rect.right << ',' << glyph.rect.bottom << "]}";
        }
        out << "]}";
    }
    out << "]}";
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 7) { std::cerr << "Usage: test worker.exe image.png --regions|--content expected runs output.json\n"; return 2; }
    const bool regions = wcscmp(argv[3], L"--regions") == 0;
    if (!regions && wcscmp(argv[3], L"--content") != 0) return 2;
    const int runs = _wtoi(argv[5]); if (runs < 1 || runs > 100) return 2;
    ULONG_PTR token{}; Gdiplus::GdiplusStartupInput startup;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) return 3;
    int status = 0;
    try {
        Gdiplus::Bitmap image(argv[2]); HBITMAP bitmap{};
        if (image.GetLastStatus() != Gdiplus::Ok || image.GetHBITMAP(Gdiplus::Color(0, 0, 0), &bitmap) != Gdiplus::Ok)
            throw std::runtime_error("Missing image fixture");
        struct Guard { HBITMAP value; ~Guard() { DeleteObject(value); } } guard{bitmap};
        std::ofstream out(std::filesystem::path(argv[6]), std::ios::binary);
        if (!out) throw std::runtime_error("Cannot write benchmark report");
        out << "{\"image\":" << Json(argv[2]) << ",\"cold_process_runs\":[";
        std::vector<double> elapsed;
        for (int run = 1; run <= runs; ++run) {
            const auto result = RunModelProcess(bitmap, image.GetWidth(), image.GetHeight(), argv[1], [] { return false; });
            if (result.status != ModelStatus::Success) { std::cerr << Json(result.reply.error) << '\n'; throw std::runtime_error("Model OCR failed"); }
            if (!result.workerExited || !result.processId || !Exited(result.processId)) throw std::runtime_error("Worker remained alive");
            std::wstring text;
            for (const auto& line : result.reply.lines) {
                text += line.text;
                std::wstring selected;
                for (const auto& glyph : line.glyphs) selected += line.text.substr(glyph.offset, glyph.length);
                if (selected != line.text) throw std::runtime_error("Glyph text does not cover recognized text");
            }
            if (regions ? !Regions(result.reply, argv[4]) : Content(text) != Content(ReadUtf8(argv[4])))
                throw std::runtime_error("Fixture accuracy regression");
            elapsed.push_back(result.elapsedMs);
            if (run > 1) out << ','; WriteRun(out, run, result);
            std::cout << "Run " << run << ": total=" << result.elapsedMs << " ms load=" << result.reply.loadMs
                << " ms inference=" << result.reply.inferenceMs << " ms peak_WS=" << result.reply.peakWorkingSetBytes / 1048576.0
                << " MiB lines=" << result.reply.lines.size() << " worker_exited=1\n" << std::flush;
        }
        double total = 0; for (double time : elapsed) total += time;
        std::sort(elapsed.begin(), elapsed.end());
        out << "],\"mean_ms\":" << total / runs << ",\"p95_ms\":" << elapsed[static_cast<size_t>(std::ceil(runs * .95)) - 1] << '}';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; status = 1; }
    Gdiplus::GdiplusShutdown(token); return status;
}
