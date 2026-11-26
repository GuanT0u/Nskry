#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <new>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>


#include "nskry_image_recognition.h"
#include "nskry_ocr_layout.h"
#include "model/ocr_process.h"
#include "ocr_layout.h"
#include <functional>
namespace {
struct Recognition {
    std::wstring text, error;
    std::vector<nskry::ocr::WordBox> words;
    std::vector<RECT> lineHighlights;
    int status = NSKRY_OCR_OK;
};
constexpr int kMaxPreviewDimension = 3200;
constexpr size_t kMaxOcrPasses = 3;

bool StartsWithIgnoreCase(const std::wstring& value, const wchar_t* prefix) {
    const size_t length = wcslen(prefix);
    return value.size() >= length && ::CompareStringOrdinal(value.c_str(), static_cast<int>(length),
        prefix, static_cast<int>(length), TRUE) == CSTR_EQUAL;
}

winrt::Windows::Media::Ocr::OcrEngine CreatePreferredOcrEngine() {
    const auto languages = winrt::Windows::Media::Ocr::OcrEngine::AvailableRecognizerLanguages();
    // The available-language collection has no promised ordering. Prefer the
    // Simplified Chinese recognizer for the Chinese UI used by this app, while
    // keeping Traditional Chinese and profile-language fallbacks.
    for (const wchar_t* preferred : { L"zh-Hans", L"zh-CN", L"zh-SG" }) {
        for (const auto& language : languages) {
            const std::wstring tag = language.LanguageTag().c_str();
            if (!StartsWithIgnoreCase(tag, preferred)) continue;
            if (const auto engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromLanguage(language)) return engine;
        }
    }
    for (const auto& language : languages) {
        if (!StartsWithIgnoreCase(language.LanguageTag().c_str(), L"zh")) continue;
        if (const auto engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromLanguage(language)) return engine;
    }
    return winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
}

using nskry::ocr::WordBox;
using nskry::ocr::RecognizedLine;

enum class OcrPassKind { Fast, Enhanced, Binary, InvertEnhanced };

struct OcrPassResult {
    OcrPassKind kind{};
    double scale{1.0};
    std::vector<RecognizedLine> lines;
    double preprocessMs{};
    double engineMs{};
};

double ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string bytes(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                          bytes.data(), length, nullptr, nullptr);
    return bytes;
}

void DumpOcrTrace(uint64_t generation, const std::vector<OcrPassResult>& passes,
                  const std::vector<RecognizedLine>& finalLines, double totalMs,
                  const std::vector<nskry::ocr::AlignedLineGroup>& groups,
                  const std::vector<nskry::ocr::ArbitrationDecision>& decisions) {
    wchar_t path[32768]{};
    const DWORD length = ::GetEnvironmentVariableW(L"NSKRY_OCR_TRACE_FILE", path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return; // Off by default, including Release.
    std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::app);
    if (!output) return;
    output << "========== OCR REQUEST " << generation << " ==========\n";
    const char* names[]{ "Fast", "Enhanced", "Binary", "InvertEnhanced" };
    for (size_t i = 0; i < passes.size(); ++i) {
        const auto& pass = passes[i];
        output << "========== OCR PASS " << static_cast<char>('A' + i) << " ==========\n"
               << "Kind: " << names[static_cast<int>(pass.kind)] << " Scale: " << pass.scale
               << " PreprocessMs: " << pass.preprocessMs << " EngineMs: " << pass.engineMs
               << " Lines: " << pass.lines.size() << '\n';
        for (const auto& line : pass.lines) {
            output << "Line [" << line.rect.left << ',' << line.rect.top << ',' << line.rect.right << ','
                   << line.rect.bottom << "] " << Utf8(line.text) << '\n';
            for (const auto& word : line.words)
                output << "  Word [" << word.rect.left << ',' << word.rect.top << ',' << word.rect.right
                       << ',' << word.rect.bottom << "] " << Utf8(word.text) << '\n';
        }
    }
    output << "========== OCR ARBITRATION ==========\n";
    for (size_t i = 0; i < groups.size(); ++i) {
        output << "Group #" << i << '\n';
        for (size_t pass = 0; pass < groups[i].candidates.size(); ++pass)
            if (const auto* candidate = groups[i].candidates[pass])
                output << "  " << static_cast<char>('A' + pass) << ": " << Utf8(candidate->text) << '\n';
        if (i < decisions.size()) output << "  Decision: " << static_cast<char>('A' + decisions[i].primaryPass)
            << " Agreement: " << decisions[i].agreementCount
            << " RecoveredWords: " << decisions[i].recoveredWords
            << " ConflictReplacement: " << decisions[i].conflictReplacement
            << " Reason: " << Utf8(decisions[i].reason) << '\n';
    }
    for (const auto& line : finalLines) output << "Final: " << Utf8(line.text) << '\n';
    output << "PassCount: " << passes.size() << " TotalMs: " << totalMs << "\n";
}

bool IsHan(wchar_t ch) {
    return ch >= 0x3400 && ch <= 0x9fff;
}

void AddRecognizedWord(RecognizedLine& line, WordBox box) {
    if (box.text.empty() || box.rect.right <= box.rect.left || box.rect.bottom <= box.rect.top) return;
    if (line.words.empty()) line.rect = box.rect;
    else {
        line.rect.left = (std::min)(line.rect.left, box.rect.left);
        line.rect.top = (std::min)(line.rect.top, box.rect.top);
        line.rect.right = (std::max)(line.rect.right, box.rect.right);
        line.rect.bottom = (std::max)(line.rect.bottom, box.rect.bottom);
    }
    line.words.push_back(std::move(box));
}

void AddDisplayWord(std::vector<WordBox>& displayWords, WordBox word) {
    // Selection/hit testing still needs approximate character boxes, although
    // default highlighting now uses cached visual-line rectangles. Keep the
    // unmodified Windows word box for spatial arbitration above.
    const bool splitHan = word.text.size() > 1 &&
        std::all_of(word.text.begin(), word.text.end(), [](wchar_t ch) { return IsHan(ch); }) &&
        word.rect.right - word.rect.left >= static_cast<LONG>(word.text.size());
    if (!splitHan) { displayWords.push_back(std::move(word)); return; }
    const LONG left = word.rect.left, right = word.rect.right;
    for (size_t index = 0; index < word.text.size(); ++index) {
        WordBox piece = word;
        piece.text.assign(1, word.text[index]);
        piece.joinPrevious = index > 0 || word.joinPrevious;
        piece.rect.left = left + static_cast<LONG>((right - left) * index / word.text.size());
        piece.rect.right = left + static_cast<LONG>((right - left) * (index + 1) / word.text.size());
        displayWords.push_back(std::move(piece));
    }
}

std::vector<RecognizedLine> ReadLines(const winrt::Windows::Media::Ocr::OcrResult& recognized,
                                      int imageWidth, int imageHeight, int displayWidth, int displayHeight) {
    std::vector<RecognizedLine> lines;
    const double scaleX = static_cast<double>(displayWidth) / imageWidth;
    const double scaleY = static_cast<double>(displayHeight) / imageHeight;
    for (const auto& line : recognized.Lines()) {
        RecognizedLine output;
        output.text = line.Text().c_str();
        for (const auto& word : line.Words()) {
            const auto bounds = word.BoundingRect();
            WordBox box;
            box.rect.left = (std::clamp)(static_cast<LONG>(std::floor(bounds.X * scaleX)), 0L, static_cast<LONG>(displayWidth));
            box.rect.top = (std::clamp)(static_cast<LONG>(std::floor(bounds.Y * scaleY)), 0L, static_cast<LONG>(displayHeight));
            box.rect.right = (std::clamp)(static_cast<LONG>(std::ceil((bounds.X + bounds.Width) * scaleX)), box.rect.left, static_cast<LONG>(displayWidth));
            box.rect.bottom = (std::clamp)(static_cast<LONG>(std::ceil((bounds.Y + bounds.Height) * scaleY)), box.rect.top, static_cast<LONG>(displayHeight));
            box.text = word.Text().c_str();
            AddRecognizedWord(output, std::move(box));
        }
        if (!output.words.empty()) lines.push_back(std::move(output));
    }
    return lines;
}

bool HasUncoveredHanGap(const std::vector<RecognizedLine>& lines) {
    for (const auto& line : lines) {
        std::vector<const WordBox*> han;
        for (const auto& word : line.words)
            if (word.text.size() == 1 && IsHan(word.text.front())) han.push_back(&word);
        if (han.size() < 4) continue;
        std::sort(han.begin(), han.end(), [](const WordBox* a, const WordBox* b) {
            return a->rect.left < b->rect.left;
        });
        std::vector<int> widths;
        for (const WordBox* word : han) widths.push_back(word->rect.right - word->rect.left);
        auto middle = widths.begin() + widths.size() / 2;
        std::nth_element(widths.begin(), middle, widths.end());
        const int typicalWidth = (std::max)(1, *middle);
        for (size_t i = 1; i < han.size(); ++i) {
            const int gap = han[i]->rect.left - han[i - 1]->rect.right;
            if (gap > typicalWidth * 3 / 4 && gap < typicalWidth * 3) return true;
        }
    }
    return false;
}

double LineQuality(const RecognizedLine& line) {
    if (line.text.empty()) return 0.0;
    int useful = 0, suspicious = 0, visible = 0;
    for (wchar_t ch : line.text) {
        if (iswspace(ch)) continue;
        ++visible;
        if ((ch >= 0x3400 && ch <= 0x9fff) || iswalnum(ch)) ++useful;
        if (ch == 0xfffd || ch == 0x25a1 || ch == L'?' || ch == L'_') ++suspicious;
    }
    if (!visible) return 0.0;
    const double validRatio = static_cast<double>(useful) / visible;
    const double coverage = (std::min)(1.0, static_cast<double>(useful) /
        (std::max)(1.0, static_cast<double>(line.rect.right - line.rect.left) / 16.0));
    return 65.0 * validRatio + 25.0 * coverage +
        (std::min)(10.0, static_cast<double>(useful) / 2.0) - 12.0 * suspicious;
}

int MedianLineHeight(const std::vector<RecognizedLine>& lines) {
    if (lines.empty()) return 0;
    std::vector<int> heights;
    heights.reserve(lines.size());
    for (const auto& line : lines) heights.push_back(line.rect.bottom - line.rect.top);
    auto middle = heights.begin() + heights.size() / 2;
    std::nth_element(heights.begin(), middle, heights.end());
    return *middle;
}

bool NeedsEnhancedPass(const std::vector<RecognizedLine>& lines) {
    if (lines.empty()) return true;
    if (HasUncoveredHanGap(lines)) return true;
    double quality = 0.0;
    for (const auto& line : lines) quality += LineQuality(line);
    quality /= lines.size();
    // A small but otherwise clean single label should stay on the fast path.
    // Dense small-type UI remains eligible for another treatment.
    if (MedianLineHeight(lines) < 19 && (lines.size() >= 8 || quality < 82.0)) return true;
    return quality < 68.0;
}

double AverageQuality(const std::vector<RecognizedLine>& lines) {
    if (lines.empty()) return 0.0;
    double total = 0.0;
    for (const auto& line : lines) total += LineQuality(line);
    return total / lines.size();
}

void ApplyTextLayout(Recognition &result, const std::vector<RecognizedLine> &lines) {
    const auto layout = nskry::ocr::BuildTextLayout(lines);
    result.text = layout.text;
    for (size_t index = 0; index < layout.lines.size(); ++index) {
        result.lineHighlights.push_back(layout.lines[index].rect);
        for (auto word : layout.lines[index].words) {
            word.line = index;
            AddDisplayWord(result.words, std::move(word));
        }
    }
}

bool GetBitmapSize(HBITMAP bitmap, int& width, int& height) {
    BITMAP info{};
    if (!bitmap || ::GetObjectW(bitmap, sizeof(info), &info) != sizeof(info) ||
        info.bmWidth <= 0 || info.bmHeight == 0) return false;
    width = info.bmWidth;
    height = std::abs(info.bmHeight);
    return height > 0;
}

HBITMAP CopyBitmap(HBITMAP bitmap) {
    return bitmap ? static_cast<HBITMAP>(::CopyImage(bitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION)) : nullptr;
}

HBITMAP ResizeForOcr(HBITMAP source, int width, int height, int& outputWidth, int& outputHeight,
                     double scaleLimit = 4.0) {
    outputWidth = width;
    outputHeight = height;
    if (!source || width <= 0 || height <= 0) return nullptr;

    uint32_t engineMaximum = 0;
    try { engineMaximum = winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension(); }
    catch (...) { engineMaximum = kMaxPreviewDimension; }
    const int maximum = (std::max)(1, (std::min)(kMaxPreviewDimension, static_cast<int>(engineMaximum)));
    const int largestDimension = (std::max)(width, height);
    double scale = 1.0;
    if (largestDimension > maximum) scale = static_cast<double>(maximum) / largestDimension;
    // Small UI text benefits considerably from a larger raster. Keep the
    // temporary recognition image bounded, and never expose it to the result
    // window (which continues to own and display the original-size bitmap).
    else scale = (std::min)(scaleLimit, static_cast<double>(maximum) / largestDimension);
    if (std::abs(scale - 1.0) < 0.01) return CopyBitmap(source);

    outputWidth = (std::max)(1, static_cast<int>(std::lround(width * scale)));
    outputHeight = (std::max)(1, static_cast<int>(std::lround(height * scale)));

    HDC screen = ::GetDC(nullptr);
    HDC src = screen ? ::CreateCompatibleDC(screen) : nullptr;
    HDC dst = screen ? ::CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = outputWidth;
    bmi.bmiHeader.biHeight = -outputHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP scaled = dst ? ::CreateDIBSection(dst, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    HGDIOBJ oldSrc = src && source ? ::SelectObject(src, source) : nullptr;
    HGDIOBJ oldDst = dst && scaled ? ::SelectObject(dst, scaled) : nullptr;
    const bool copied = oldSrc && oldSrc != HGDI_ERROR && oldDst && oldDst != HGDI_ERROR &&
        (::SetStretchBltMode(dst, HALFTONE), ::StretchBlt(dst, 0, 0, outputWidth, outputHeight,
                                                          src, 0, 0, width, height, SRCCOPY) != FALSE);
    if (oldSrc && oldSrc != HGDI_ERROR) ::SelectObject(src, oldSrc);
    if (oldDst && oldDst != HGDI_ERROR) ::SelectObject(dst, oldDst);
    if (src) ::DeleteDC(src);
    if (dst) ::DeleteDC(dst);
    if (screen) ::ReleaseDC(nullptr, screen);
    if (!copied) { if (scaled) ::DeleteObject(scaled); return nullptr; }
    return scaled;
}

enum class OcrTreatment { Enhanced, Binary, InvertEnhanced };

HBITMAP TreatForOcr(HBITMAP source, int width, int height, OcrTreatment treatment,
                    bool* darkBackgroundOut = nullptr) {
    if (!source || width <= 0 || height <= 0) return nullptr;
    HDC screen = ::GetDC(nullptr);
    if (!screen) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP treated = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!treated || !pixels || ::GetDIBits(screen, source, 0, height, pixels, &info, DIB_RGB_COLORS) != height) {
        if (treated) ::DeleteObject(treated);
        ::ReleaseDC(nullptr, screen);
        return nullptr;
    }
    ::ReleaseDC(nullptr, screen);

    const size_t count = static_cast<size_t>(width) * height;
    auto* rgba = static_cast<uint8_t*>(pixels);
    std::vector<uint8_t> gray(count);
    uint64_t borderSum = 0;
    size_t borderCount = 0;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const size_t index = static_cast<size_t>(y) * width + x;
        const uint8_t* p = rgba + index * 4;
        gray[index] = static_cast<uint8_t>((29 * p[0] + 150 * p[1] + 77 * p[2]) >> 8);
        if (x == 0 || x == width - 1 || y == 0 || y == height - 1) {
            borderSum += gray[index];
            ++borderCount;
        }
    }
    const bool darkBackground = borderCount && borderSum / borderCount < 112;
    if (darkBackgroundOut) *darkBackgroundOut = darkBackground;
    for (uint8_t& value : gray) {
        const int oriented = treatment == OcrTreatment::InvertEnhanced ? 255 - value : value;
        value = static_cast<uint8_t>((std::clamp)(static_cast<int>((oriented - 128) * 1.25 + 128), 0, 255));
    }

    int threshold = 128;
    if (treatment == OcrTreatment::Binary) {
        std::array<uint32_t, 256> histogram{};
        for (uint8_t value : gray) ++histogram[value];
        uint64_t totalSum = 0;
        for (int i = 0; i < 256; ++i) totalSum += static_cast<uint64_t>(i) * histogram[i];
        uint64_t lowSum = 0, lowCount = 0;
        double bestVariance = -1.0;
        for (int i = 0; i < 256; ++i) {
            lowCount += histogram[i];
            lowSum += static_cast<uint64_t>(i) * histogram[i];
            if (!lowCount || lowCount == count) continue;
            const double difference = static_cast<double>(lowSum) / lowCount -
                static_cast<double>(totalSum - lowSum) / (count - lowCount);
            const double variance = static_cast<double>(lowCount) * (count - lowCount) * difference * difference;
            if (variance > bestVariance) { bestVariance = variance; threshold = i; }
        }
    }
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const size_t index = static_cast<size_t>(y) * width + x;
        int value = gray[index];
        if (treatment == OcrTreatment::Binary) value = value > threshold ? 255 : 0;
        else if (x > 0 && x + 1 < width && y > 0 && y + 1 < height) {
            value = (std::clamp)(static_cast<int>(1.4 * value - 0.1 *
                (gray[index - 1] + gray[index + 1] + gray[index - width] + gray[index + width])), 0, 255);
        }
        uint8_t* p = rgba + index * 4;
        p[0] = p[1] = p[2] = static_cast<uint8_t>(value);
        p[3] = 255;
    }
    return treated;
}

winrt::Windows::Graphics::Imaging::SoftwareBitmap ToSoftwareBitmap(HBITMAP bitmap) {
    int width{}, height{};
    if (!GetBitmapSize(bitmap, width, height)) throw winrt::hresult_invalid_argument();

    const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (byteCount == 0 || byteCount > UINT32_MAX) throw winrt::hresult_error(E_OUTOFMEMORY);

    // Feed Windows OCR a raw BGRA buffer.  The previous WIC -> PNG -> COM stream
    // round-trip crossed two independent stream wrappers and corrupted the process
    // heap on some Windows builds while the asynchronous decoder was consuming it.
    // A SoftwareBitmap owns this copied buffer and has no stream lifetime to outlive.
    winrt::Windows::Storage::Streams::Buffer pixels(static_cast<uint32_t>(byteCount));
    pixels.Length(static_cast<uint32_t>(byteCount));
    uint8_t* destination{};
    winrt::check_hresult(pixels.as<winrt::impl::IBufferByteAccess>()->Buffer(&destination));
    if (!destination) throw winrt::hresult_error(E_OUTOFMEMORY);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top-down, matching OCR coordinates
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = ::GetDC(nullptr);
    if (!screen) throw winrt::hresult_error(HRESULT_FROM_WIN32(::GetLastError()));
    const int rows = ::GetDIBits(screen, bitmap, 0, static_cast<UINT>(height), destination,
                                  &info, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, screen);
    if (rows != height) throw winrt::hresult_error(HRESULT_FROM_WIN32(::GetLastError()));

    auto converted = winrt::Windows::Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(
        pixels, winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
        width, height, winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Ignore);
    return converted;
}

std::unique_ptr<Recognition> RunWindowsOcr(HBITMAP displayBitmap, int displayWidth, int displayHeight, uint64_t generation,
        const std::function<bool()>& canceled) {
    struct OcrBitmapGuard { HBITMAP value{}; ~OcrBitmapGuard() { if (value) DeleteObject(value); } } guard;
    auto result = std::make_unique<Recognition>();
    const auto requestStart = std::chrono::steady_clock::now();
    std::vector<OcrPassResult> passes;
    const auto recognize = [&](const auto& engine, HBITMAP input) {
        // Keep the WinRT bitmap alive across status polling, just as the old
        // synchronous full expression retained it until .get() completed.
        const auto softwareBitmap = ToSoftwareBitmap(input);
        auto operation = engine.RecognizeAsync(softwareBitmap);
        // Status polling lives only during this request and registers no DLL
        // callbacks that could outlive cancellation/unload.
        while (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
            if (canceled() || ElapsedMs(requestStart) > 28000) {
                operation.Cancel();
                winrt::throw_hresult(HRESULT_FROM_WIN32(canceled() ? ERROR_CANCELLED : ERROR_TIMEOUT));
            }
            ::Sleep(15);
        }
        return operation.GetResults();
    };
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        // The final MTA may end between superseded requests. Release WinRT's
        // DLL-local cached factories before tearing it down, not after it has
        // invalidated apartment-bound factory state used by the next request.
        struct Apartment { ~Apartment() { winrt::clear_factory_cache(); winrt::uninit_apartment(); } } apartment;
        const auto engine = CreatePreferredOcrEngine();
        if (!engine) {
            result->status = NSKRY_OCR_LANGUAGE;
            result->error = L"Windows OCR has no language pack for your profile. Install an OCR language in Windows Settings, then try again.";
            return result;
        }
        if (canceled()) return nullptr;
        int ocrWidth{}, ocrHeight{};
        const auto fastStart = std::chrono::steady_clock::now();
        // Benchmark override only. Production keeps the existing auto-scale
        // until fixture data demonstrates a better default.
        const bool benchmarkFast1x = ::GetEnvironmentVariableW(L"NSKRY_OCR_BENCH_FAST_1X", nullptr, 0) > 0;
        guard.value = ResizeForOcr(displayBitmap, displayWidth, displayHeight, ocrWidth, ocrHeight,
                                   benchmarkFast1x ? 1.0 : 4.0);
        if (!guard.value) {
            result->status = NSKRY_OCR_FAILED;
            result->error = L"Could not prepare the image for text recognition.";
            return result;
        }
        std::vector<RecognizedLine> lines;
        const double fastPreprocessMs = ElapsedMs(fastStart);
        const auto fastEngineStart = std::chrono::steady_clock::now();
        {
            const auto recognized = recognize(engine, guard.value);
            lines = ReadLines(recognized, ocrWidth, ocrHeight, displayWidth, displayHeight);
        }
        passes.push_back({ OcrPassKind::Fast, static_cast<double>(ocrWidth) / displayWidth,
                           lines, fastPreprocessMs, ElapsedMs(fastEngineStart) });
        // The first OCR input and WinRT result are no longer needed. In
        // particular, do not keep the large scaled bitmap during later passes.
        ::DeleteObject(guard.value);
        guard.value = nullptr;
        if (canceled()) return nullptr;

        if (NeedsEnhancedPass(lines)) {
            const int textHeight = MedianLineHeight(lines);
            const double scale = textHeight == 0 || textHeight < 12 ? 4.0 :
                textHeight < 18 ? 3.0 : textHeight < 28 ? 2.0 : 1.0;
            bool darkBackground = false;
            auto runTreatment = [&](OcrTreatment treatment) {
                if (canceled()) return;
                struct BitmapGuard { HBITMAP value{}; ~BitmapGuard() { if (value) ::DeleteObject(value); } } scaled, prepared;
                const auto preprocessStart = std::chrono::steady_clock::now();
                int width{}, height{};
                scaled.value = ResizeForOcr(displayBitmap, displayWidth, displayHeight, width, height, scale);
                if (!scaled.value) return;
                prepared.value = TreatForOcr(scaled.value, width, height, treatment,
                    treatment == OcrTreatment::Enhanced ? &darkBackground : nullptr);
                ::DeleteObject(scaled.value);
                scaled.value = nullptr;
                if (!prepared.value) return;
                const double preprocessMs = ElapsedMs(preprocessStart);
                const auto engineStart = std::chrono::steady_clock::now();
                const auto recognized = recognize(engine, prepared.value);
                if (canceled()) return;
                auto candidate = ReadLines(recognized, width, height, displayWidth, displayHeight);
                const OcrPassKind kind = treatment == OcrTreatment::Enhanced ? OcrPassKind::Enhanced :
                    treatment == OcrTreatment::Binary ? OcrPassKind::Binary : OcrPassKind::InvertEnhanced;
                passes.push_back({ kind,
                    static_cast<double>(width) / displayWidth, candidate, preprocessMs, ElapsedMs(engineStart) });
                // Keep the raw candidate until pass scheduling is complete.
            };
            try { runTreatment(OcrTreatment::Enhanced); }
            catch (...) { /* Preserve the successful fast pass. */ }
            if (canceled()) return nullptr;
            std::vector<std::vector<RecognizedLine>> comparison;
            for (const auto& pass : passes) comparison.push_back(pass.lines);
            const auto localGroups = nskry::ocr::AlignLines(comparison);
            const bool needThird = AverageQuality(lines) < 62.0 || HasUncoveredHanGap(lines) ||
                nskry::ocr::HasLocalDisagreement(localGroups) ||
                nskry::ocr::HasCoverageDifference(localGroups) ||
                nskry::ocr::HasSuspiciousGeometry(localGroups);
            if (passes.size() == 2 && passes.size() < kMaxOcrPasses && needThird) {
                const bool conflict = nskry::ocr::HasLocalDisagreement(localGroups);
                const OcrTreatment third = darkBackground && conflict ? OcrTreatment::InvertEnhanced :
                    OcrTreatment::Binary;
                try { runTreatment(third); }
                catch (...) { /* Preserve prior passes. */ }
            }
        }
        if (canceled()) return nullptr;
        std::vector<std::vector<RecognizedLine>> rawLines;
        std::vector<nskry::ocr::AlignedLineGroup> groups;
        std::vector<nskry::ocr::ArbitrationDecision> decisions;
        try {
            for (const auto& pass : passes) rawLines.push_back(pass.lines);
            groups = nskry::ocr::AlignLines(rawLines);
            decisions = nskry::ocr::Arbitrate(groups);
            if (!decisions.empty()) {
                lines.clear();
                for (const auto& decision : decisions) lines.push_back(decision.line);
            }
        } catch (...) {
            // Candidate logic must not make a successful Fast pass unusable.
            lines = passes.front().lines;
            groups.clear();
            decisions.clear();
        }
        std::stable_sort(lines.begin(), lines.end(), [](const RecognizedLine& a, const RecognizedLine& b) {
            return a.rect.top < b.rect.top || (a.rect.top == b.rect.top && a.rect.left < b.rect.left);
        });
        try { DumpOcrTrace(generation, passes, lines, ElapsedMs(requestStart), groups, decisions); }
        catch (...) { /* Optional diagnostics must not turn successful OCR into an error. */ }
        ApplyTextLayout(*result, lines);
    } catch (const winrt::hresult_error& error) {
        if (error.code() == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) result->status = NSKRY_OCR_TIMEOUT;
        if (error.code() == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) result->status = NSKRY_OCR_TIMEOUT;
        result->error = L"Windows OCR could not recognize this image: " + std::wstring(error.message().c_str());
    } catch (const std::exception& error) {
        std::wstring message;
        const int needed = ::MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, nullptr, 0);
        if (needed > 1) {
            message.resize(needed);
            ::MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, message.data(), needed);
            message.pop_back();
        }
        result->error = L"Windows OCR could not recognize this image" + (message.empty() ? L"." : L": " + message);
    } catch (...) {
        result->error = L"Windows OCR could not recognize this image.";
    }
    return result;
}

std::filesystem::path ModelExecutable() {
    HMODULE module{};
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModelExecutable), &module)) return {};
    wchar_t path[32768]{};
    const DWORD count = ::GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (!count || count >= ARRAYSIZE(path)) return {};
    return std::filesystem::path(path).parent_path() / L"engine" / L"nskry-ocr-worker.exe";
}

void DumpModelTrace(uint64_t generation, const nskry::ocr::ModelResult& model) {
    wchar_t path[32768]{};
    const DWORD count = ::GetEnvironmentVariableW(L"NSKRY_OCR_TRACE_FILE", path, ARRAYSIZE(path));
    if (!count || count >= ARRAYSIZE(path)) return;
    std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::app);
    if (!output) return;
    output << "========== OCR MODEL REQUEST " << generation << " ==========\n"
        << "Backend: PP-OCRv4 ModelPassCount: 1 Threads: " << model.threads
        << " LoadMs: " << model.reply.loadMs << " InferenceMs: " << model.reply.inferenceMs
        << " TotalMs: " << model.elapsedMs << " CpuMs: " << model.reply.cpuMs
        << " PeakWorkingSetBytes: " << model.reply.peakWorkingSetBytes << " PeakCommitBytes: " << model.reply.peakCommitBytes
        << " ProcessId: " << model.processId << " WorkerExited: " << model.workerExited << '\n';
    for (const auto& line : model.reply.lines) {
        output << "Line [" << line.rect.left << ',' << line.rect.top << ',' << line.rect.right << ',' << line.rect.bottom
            << "] " << Utf8(line.text) << "\n  Confidence: " << line.confidence << '\n';
        for (const auto& glyph : line.glyphs)
            output << "  Glyph [" << glyph.rect.left << ',' << glyph.rect.top << ',' << glyph.rect.right << ',' << glyph.rect.bottom
                << "] " << Utf8(line.text.substr(glyph.offset, glyph.length)) << '\n';
    }
    if (!model.reply.error.empty()) output << "Error: " << Utf8(model.reply.error) << '\n';
}

std::unique_ptr<Recognition> RunOcr(HBITMAP bitmap, int width, int height, uint64_t generation,
        bool fast, const std::function<bool()>& canceled) {
    if (fast) return RunWindowsOcr(bitmap, width, height, generation, canceled);
    auto result = std::make_unique<Recognition>();
    try {
        const auto executable = ModelExecutable();
        nskry::ocr::ModelResult model;
        const auto directory = executable.parent_path();
        bool complete = !executable.empty();
        std::error_code error;
        for (const auto& file : {executable, directory / L"onnxruntime.dll",
                directory / L"models/ch_PP-OCRv4_det_infer.onnx", directory / L"models/ch_PP-OCRv4_rec_infer.onnx"})
            complete = std::filesystem::is_regular_file(file, error) && complete;
        if (complete) model = nskry::ocr::RunModelProcess(bitmap, width, height, executable,
            canceled);
        else {
            model.status = nskry::ocr::ModelStatus::Unavailable;
            model.reply.error = L"The offline OCR engine or its models are missing. Reinstall the complete OCR provider package, or select Fast OCR.";
        }
        if (model.status == nskry::ocr::ModelStatus::Canceled || canceled()) return nullptr;
        try { DumpModelTrace(generation, model); } catch (...) {}
        if (model.status != nskry::ocr::ModelStatus::Success) {
            if (model.status == nskry::ocr::ModelStatus::Timeout) result->status = NSKRY_OCR_TIMEOUT;
            if (model.status == nskry::ocr::ModelStatus::Unavailable)
                result->status = NSKRY_OCR_UNAVAILABLE;
            result->error = model.reply.error.empty() ? L"Offline recognition failed." : model.reply.error;
            return result;
        }
        if (!model.workerExited) { result->error = L"The OCR worker did not finish cleanly. No partial result was used."; return result; }
        std::vector<RecognizedLine> displayLines;
        for (size_t index = 0; index < model.reply.lines.size(); ++index) {
            const auto& line = model.reply.lines[index];
            RecognizedLine display{{line.rect.left, line.rect.top, line.rect.right, line.rect.bottom}, line.text, {}};
            if (line.glyphs.empty()) {
                display.words.push_back({display.rect, line.text, index});
            } else for (size_t glyphIndex = 0; glyphIndex < line.glyphs.size(); ++glyphIndex) {
                const auto& glyph = line.glyphs[glyphIndex];
                display.words.push_back({{glyph.rect.left, glyph.rect.top, glyph.rect.right, glyph.rect.bottom},
                    line.text.substr(glyph.offset, glyph.length), index, glyphIndex > 0});
            }
            displayLines.push_back(std::move(display));
        }
        ApplyTextLayout(*result, displayLines);
    } catch (...) { result->error = L"Could not complete offline OCR. Try a smaller area or select Fast OCR."; }
    return result;
}

} // namespace
extern "C" NSKRY_API const NskryPluginInfo* NSKRY_CALL nskry_plugin_info() {
    static const NskryPluginInfo info{sizeof(info), 2, L"OCR Provider", L"nskry-ocr-provider", L"1.0.0",
                                      L"Nskry Team", L"Local image recognition", nullptr, 0, {}};
    return &info;
}
extern "C" NSKRY_API int32_t NSKRY_CALL nskry_plugin_init(const NskryHostContext*) { return 1; }
extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_shutdown() {}
extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_execute(const NskryHostContext*) {}
extern "C" NSKRY_API int32_t NSKRY_CALL nskry_plugin_ocr_layout_v1(const NskryOcrRequestV1 *r) {
    if (!r || r->version != 1 || r->structSize < sizeof(*r) || !r->receive || !r->canceled ||
        !r->error || !r->errorCapacity || r->errorCapacity > INT_MAX || r->fast > 1) return NSKRY_OCR_INVALID;
    r->error[0] = 0;
    int w{}, h{};
    if (!GetBitmapSize(r->bitmap, w, h)) return NSKRY_OCR_INVALID;
    static std::atomic_flag running{};
    if (running.test_and_set()) return NSKRY_OCR_BUSY;
    struct Guard { std::atomic_flag& flag; ~Guard() { flag.clear(); } } guard{running};
    try {
        const auto canceled = [&] { return r->canceled(r->context) != 0; };
        if (canceled()) return NSKRY_OCR_CANCELED;
        auto result = RunOcr(r->bitmap, w, h, r->requestId, r->fast, canceled);
        if (!result || canceled()) return NSKRY_OCR_CANCELED;
        if (!result->error.empty()) {
            lstrcpynW(r->error, result->error.c_str(), int(r->errorCapacity));
            return result->status == NSKRY_OCR_OK ? NSKRY_OCR_FAILED : result->status;
        }
        if (result->text.empty()) return NSKRY_OCR_EMPTY;
        std::vector<NskryOcrWordV1> words;
        size_t cursor = 0;
        for (const auto& word : result->words) {
            const auto pos = result->text.find(word.text, cursor);
            if (pos == std::wstring::npos) return NSKRY_OCR_FAILED;
            words.push_back({word.rect, uint32_t(pos), uint32_t(word.text.size()), uint32_t(word.line), word.joinPrevious});
            cursor = pos + word.text.size();
        }
        NskryOcrLayoutV1 out{result->text.c_str(), uint32_t(result->text.size()), result->lineHighlights.data(),
                             uint32_t(result->lineHighlights.size()), words.data(), uint32_t(words.size())};
        return r->receive(r->context, &out) ? NSKRY_OCR_OK : NSKRY_OCR_FAILED;
    } catch (...) { return NSKRY_OCR_FAILED; }
}
extern "C" NSKRY_API int32_t NSKRY_CALL nskry_plugin_recognize_image_v1(
    const NskryImageInputV1 *input, NskryImageTextV1 *output) {
    if (!NskryValidImageText(output)) return NSKRY_IMAGE_INVALID;
    output->error[0] = output->text[0] = 0;
    output->textLength = 0;
    int width{}, height{};
    if (!input || input->structSize < sizeof(*input) || input->version != 1 || !input->cancel ||
        !GetBitmapSize(input->bitmap, width, height) || input->width != width || input->height != height ||
        width > 4096 || height > 4096 || uint64_t(width) * height > 4194304) return NSKRY_IMAGE_INVALID;
    struct State { HANDLE cancel; NskryImageTextV1 *out; } state{input->cancel, output};
    NskryOcrRequestV1 request{sizeof(request), 1, input->bitmap, !NSKRY_OCR_MODEL_ENGINE, &state,
        [](void *ctx) -> int32_t { return WaitForSingleObject(static_cast<State*>(ctx)->cancel, 0) == WAIT_OBJECT_0; },
        [](void *ctx, const NskryOcrLayoutV1 *layout) -> int32_t {
            auto *out = static_cast<State*>(ctx)->out;
            if (layout->length > NSKRY_IMAGE_MAX_TEXT) return 0;
            out->textLength = layout->length;
            lstrcpynW(out->text, layout->text, out->capacity);
            return 1;
        }, output->error, output->errorCapacity, 0};
    const auto status = nskry_plugin_ocr_layout_v1(&request);
    return status == NSKRY_OCR_OK ? NSKRY_IMAGE_SUCCESS :
           status == NSKRY_OCR_CANCELED ? NSKRY_IMAGE_CANCELED : NSKRY_IMAGE_FAILED;
}
