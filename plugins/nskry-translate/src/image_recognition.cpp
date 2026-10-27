#include "image_recognition.h"
#include "ocr_layout.h"
#include "translation_policy.h"
#include <winrt/base.h>
#if NSKRY_TRANSLATE_MODEL_OCR
#include "model/ocr_process.h"
#else
#include "ocr_wait.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#endif

namespace nskry::translate {
std::wstring RecognizeImage(const std::vector<unsigned char> &pixels, int width, int height,
                            const std::filesystem::path &directory, HANDLE cancel) {
    const auto canceled = [&] { return WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0; };
    if (canceled())
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || uint64_t(width) * height > 4194304 ||
        pixels.size() != uint64_t(width) * height * 4)
        winrt::throw_hresult(E_BOUNDS);
    std::vector<nskry::ocr::RecognizedLine> lines;
#if NSKRY_TRANSLATE_MODEL_OCR
    const auto engine = directory / L"ocr" / L"nskry-ocr-worker.exe";
    for (const auto &asset :
         {engine, directory / L"ocr/onnxruntime.dll", directory / L"ocr/models/ch_PP-OCRv4_det_infer.onnx",
          directory / L"ocr/models/ch_PP-OCRv4_rec_infer.onnx"}) {
        if (!std::filesystem::is_regular_file(asset))
            throw std::wstring(
                L"Local image recognition runtime missing. Use the complete Translation "
                L"package (including ocr/), or paste text. No OCR plugin installation is required.");
    }
    auto result = nskry::ocr::RunModelPixels(pixels, width, height, engine, canceled, 20000);
    if (result.status == nskry::ocr::ModelStatus::Canceled || canceled())
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    if (result.status != nskry::ocr::ModelStatus::Success || !result.workerExited)
        throw std::wstring(L"Local image recognition failed or timed out. Select a smaller area or paste "
                           L"source text. The owned OCR process was stopped; no online fallback was used.");
    for (const auto &line : result.reply.lines) {
        nskry::ocr::RecognizedLine item{
            {line.rect.left, line.rect.top, line.rect.right, line.rect.bottom}, line.text, {}};
        for (const auto &glyph : line.glyphs)
            item.words.push_back({{glyph.rect.left, glyph.rect.top, glyph.rect.right, glyph.rect.bottom},
                                  line.text.substr(glyph.offset, glyph.length),
                                  lines.size(),
                                  true});
        lines.push_back(std::move(item));
    }
#else
    // Explicit model-free build, not a runtime fallback after accurate OCR fails.
    using namespace winrt::Windows;
    auto engine = Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine || width > int(Media::Ocr::OcrEngine::MaxImageDimension()) ||
        height > int(Media::Ocr::OcrEngine::MaxImageDimension()))
        winrt::throw_hresult(E_BOUNDS);
    Storage::Streams::DataWriter writer;
    writer.WriteBytes(pixels);
    auto bitmap = Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(
        writer.DetachBuffer(), Graphics::Imaging::BitmapPixelFormat::Bgra8, width, height,
        Graphics::Imaging::BitmapAlphaMode::Ignore);
    auto operation = engine.RecognizeAsync(bitmap);
    WaitForOcr(operation, cancel, TaskTimeoutMs);
    for (const auto &line : operation.GetResults().Lines()) {
        nskry::ocr::RecognizedLine item;
        item.text = line.Text();
        for (const auto &word : line.Words()) {
            const auto r = word.BoundingRect();
            RECT rect{LONG(r.X), LONG(r.Y), LONG(r.X + r.Width), LONG(r.Y + r.Height)};
            if (item.words.empty())
                item.rect = rect;
            else
                UnionRect(&item.rect, &item.rect, &rect);
            item.words.push_back({rect, std::wstring(word.Text()), lines.size(), false});
        }
        lines.push_back(std::move(item));
    }
#endif
    if (canceled())
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    auto text = nskry::ocr::BuildTextLayout(lines).text;
    if (ValidateText(text) != InputError::None)
        throw std::wstring(L"No usable text was recognized, or the result exceeds 16000 UTF-16 units. "
                           L"Select a smaller, clearer area or paste source text.");
    return text;
}
} // namespace nskry::translate
