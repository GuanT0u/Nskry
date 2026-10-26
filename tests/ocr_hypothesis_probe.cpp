// Opt-in experiments, never installed or called by the product. Only the
// compile-time observer captures actual production crops; inference is unchanged.
#include "core/json.h"
#include "ctc_decode.h"
#include "model/crop_spacing.h"
#include "ocr_inference.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <objidl.h> // GDI+ needs IStream declared first.
#include <gdiplus.h>
#include <iostream>
#include <memory>
#include <numeric>
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <sstream>
#include <windows.h>

using nskry::json::Value;
namespace {
using Clock = std::chrono::steady_clock;
struct Captured {
    cv::Mat crop;
    int width{};
    Value trace;
    std::wstring text;
};
std::vector<Captured> captured;
std::wstring W(const std::string &value) {
    const int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
    if (!n && !value.empty())
        throw std::runtime_error("UTF8 decode");
    std::wstring result(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), result.data(), n);
    return result;
}
void Write(const std::filesystem::path &path, const Value &value) {
    std::wstring json;
    std::string utf8;
    if (std::filesystem::exists(path) || !nskry::json::Serialize(value, json) ||
        !nskry::json::ToUtf8(json, utf8))
        throw std::runtime_error("New output path required");
    std::ofstream stream(path, std::ios::binary);
    stream.write(utf8.data(), utf8.size());
    if (!stream)
        throw std::runtime_error("Write failed");
}
cv::Mat ReadImage(const wchar_t *path) {
    Gdiplus::Bitmap image(path);
    if (image.GetLastStatus() != Gdiplus::Ok)
        throw std::runtime_error("Image unavailable");
    Gdiplus::Rect rect(0, 0, image.GetWidth(), image.GetHeight());
    Gdiplus::BitmapData bits{};
    if (image.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bits) != Gdiplus::Ok)
        throw std::runtime_error("Image read failed");
    cv::Mat copy(rect.Height, rect.Width, CV_8UC4);
    for (int y = 0; y < rect.Height; ++y)
        std::memcpy(copy.ptr(y), static_cast<unsigned char *>(bits.Scan0) + y * bits.Stride, rect.Width * 4);
    image.UnlockBits(&bits);
    return copy;
}
void SaveImage(const std::filesystem::path &path, const cv::Mat &bgr) {
    cv::Mat bgra;
    cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
    UINT count{}, bytes{};
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto *encoders = reinterpret_cast<Gdiplus::ImageCodecInfo *>(storage.data());
    Gdiplus::GetImageEncoders(count, bytes, encoders);
    for (UINT i = 0; i < count; ++i)
        if (!wcscmp(encoders[i].MimeType, L"image/png")) {
            Gdiplus::Bitmap image(bgra.cols, bgra.rows, int(bgra.step), PixelFormat32bppARGB, bgra.data);
            if (image.Save(path.c_str(), &encoders[i].Clsid, nullptr) != Gdiplus::Ok)
                throw std::runtime_error("PNG save failed");
            return;
        }
    throw std::runtime_error("PNG encoder unavailable");
}
Value Analyze(std::span<const float> p, size_t steps, size_t classes, const std::vector<std::wstring> &vocab,
              float ratio, const cv::Mat &crop) {
    auto decoded = nskry::ocr::DecodeCtc(p, steps, classes, vocab, ratio);
    const auto raw = decoded.text;
    nskry::ocr::RecoverCropSpacing(decoded, crop);
    const auto space = std::find(vocab.begin(), vocab.end(), L" ") - vocab.begin();
    Value::Array frames, glyphs;
    for (size_t t = 0; t < steps; ++t) {
        const auto row = p.subspan(t * classes, classes);
        std::array<size_t, 3> best{};
        std::vector<size_t> indices(classes);
        std::iota(indices.begin(), indices.end(), 0);
        std::partial_sort(indices.begin(), indices.begin() + 3, indices.end(),
                          [&](auto a, auto b) { return row[a] > row[b]; });
        std::copy_n(indices.begin(), 3, best.begin());
        Value::Array top;
        for (auto id : best)
            top.emplace_back(Value::Object{{L"token", Value(vocab[id])}, {L"p", Value(double(row[id]))}});
        frames.emplace_back(Value::Object{{L"step", Value(double(t))},
                                          {L"blank", Value(double(row[0]))},
                                          {L"space", Value(space < classes ? double(row[space]) : 0.0)},
                                          {L"top3", Value(std::move(top))}});
    }
    for (const auto &g : decoded.glyphs)
        glyphs.emplace_back(Value::Object{{L"text", Value(decoded.text.substr(g.offset, g.length))},
                                          {L"left", Value(double(g.left))},
                                          {L"right", Value(double(g.right))}});
    return Value(Value::Object{{L"text", Value(decoded.text)},
                               {L"raw_text", Value(raw)},
                               {L"local_uncertainty", Value(decoded.uncertain)},
                               {L"confidence", Value(double(decoded.confidence))},
                               {L"frames", Value(std::move(frames))},
                               {L"glyphs", Value(std::move(glyphs))}});
}
std::vector<float> Tensor(const cv::Mat &image, int width) {
    const size_t plane = size_t(width) * image.rows;
    std::vector<float> data(3 * plane, 0.f);
    for (int y = 0; y < image.rows; ++y)
        for (int x = 0; x < image.cols; ++x)
            for (int c = 0; c < 3; ++c)
                data[c * plane + y * width + x] = image.at<cv::Vec3b>(y, x)[c] / 127.5f - 1.f;
    return data;
}
struct Recognizer {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "HypothesisProbe"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    std::string input, output;
    std::vector<std::wstring> vocabulary{L""};
    Recognizer(const std::filesystem::path &models) {
        env.DisableTelemetryEvents();
        options.SetIntraOpNumThreads(2);
        options.SetInterOpNumThreads(1);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.AddConfigEntry("session.intra_op.allow_spinning", "0");
        options.AddConfigEntry("session.inter_op.allow_spinning", "0");
        session =
            std::make_unique<Ort::Session>(env, (models / L"ch_PP-OCRv4_rec_infer.onnx").c_str(), options);
        Ort::AllocatorWithDefaultOptions a;
        input = session->GetInputNameAllocated(0, a).get();
        output = session->GetOutputNameAllocated(0, a).get();
        auto metadata = session->GetModelMetadata().LookupCustomMetadataMapAllocated("character", a);
        if (!metadata)
            throw std::runtime_error("No model vocabulary");
        std::istringstream stream(metadata.get());
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            vocabulary.push_back(W(line));
        }
        vocabulary.push_back(L" ");
    }
    Value Run(const cv::Mat &crop, int fixedWidth = 0) {
        const int width = fixedWidth ? fixedWidth : (std::max)(320, int(48.0 * crop.cols / crop.rows));
        const int content = std::clamp(int(std::ceil(48.0 * crop.cols / crop.rows)), 1, width);
        cv::Mat resized;
        cv::resize(crop, resized, cv::Size(content, 48));
        auto data = Tensor(resized, width);
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t, 4> shape{1, 3, 48, width};
        auto tensor =
            Ort::Value::CreateTensor<float>(memory, data.data(), data.size(), shape.data(), shape.size());
        const char *in[]{input.c_str()}, *out[]{output.c_str()};
        const auto start = Clock::now();
        auto result = session->Run(Ort::RunOptions{nullptr}, in, &tensor, 1, out, 1);
        const auto inferenceMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        const auto dims = result[0].GetTensorTypeAndShapeInfo().GetShape();
        if (dims.size() != 3 || dims[0] != 1 || dims[2] != vocabulary.size())
            throw std::runtime_error("Wrong tensor");
        auto trace = Analyze({result[0].GetTensorData<float>(), size_t(dims[1] * dims[2])}, dims[1], dims[2],
                             vocabulary, float(width) / content, crop);
        auto object = *trace.AsObject();
        object[L"inference_ms"] = Value(inferenceMs);
        object[L"inference_and_trace_ms"] =
            Value(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        object[L"content_width"] = Value(double(content));
        object[L"tensor_width"] = Value(double(width));
        return Value(std::move(object));
    }
};
cv::Mat Digits(const wchar_t *font, int size, int gap, bool dark, const std::vector<std::wstring> &groups) {
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), 640, -100, 1, 32, BI_RGB};
    void *bits{};
    auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto old = SelectObject(dc, bitmap);
    HFONT f = CreateFontW(size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, font);
    auto oldFont = SelectObject(dc, f);
    wchar_t actualFont[128]{};
    if (!GetTextFaceW(dc, 128, actualFont) || wcscmp(actualFont, font)) {
        SelectObject(dc, oldFont);
        DeleteObject(f);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        throw std::runtime_error("Required synthetic test font is unavailable");
    }
    const auto bg = dark ? RGB(25, 65, 40) : RGB(255, 255, 255);
    const auto fg = dark ? RGB(200, 185, 125) : RGB(0, 0, 0);
    HBRUSH brush = CreateSolidBrush(bg);
    RECT r{0, 0, 640, 100};
    FillRect(dc, &r, brush);
    DeleteObject(brush);
    SetBkColor(dc, bg);
    SetTextColor(dc, fg);
    int x = 12;
    for (const auto &group : groups) {
        TextOutW(dc, x, 12, group.c_str(), int(group.size()));
        SIZE extent{};
        GetTextExtentPoint32W(dc, group.c_str(), int(group.size()), &extent);
        x += extent.cx + gap;
    }
    GdiFlush();
    cv::Mat bgr;
    cv::cvtColor(cv::Mat(100, 640, CV_8UC4, bits), bgr, cv::COLOR_BGRA2BGR);
    SelectObject(dc, oldFont);
    DeleteObject(f);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    // Remove unused right/bottom canvas only; keep identical padding across gaps.
    return bgr(cv::Rect(0, 0, x - gap + 12, size + 28)).clone();
}
} // namespace
namespace nskry::ocr {
void ObserveHypothesisCrop(const cv::Mat &crop, int width, std::span<const float> p, size_t steps,
                           size_t classes, const std::vector<std::wstring> &vocabulary) {
    const auto contentWidth = std::clamp(int(std::ceil(48.0 * crop.cols / crop.rows)), 1, width);
    auto decoded = DecodeCtc(p, steps, classes, vocabulary, float(width) / contentWidth);
    captured.push_back({crop.clone(), width,
                        Analyze(p, steps, classes, vocabulary, float(width) / contentWidth, crop),
                        decoded.text});
}
} // namespace nskry::ocr
int wmain(int argc, wchar_t **argv) {
    if (argc != 4) {
        std::cerr << "models image NEW-output-directory\n";
        return 2;
    }
    ULONG_PTR token{};
    Gdiplus::GdiplusStartupInput gd;
    if (Gdiplus::GdiplusStartup(&token, &gd, nullptr) != Gdiplus::Ok)
        return 3;
    int status{};
    try {
        const std::filesystem::path output(argv[3]);
        if (!std::filesystem::create_directory(output))
            throw std::runtime_error("NEW directory required");
        cv::setNumThreads(1);
        auto image = ReadImage(argv[2]);
        const auto raw =
            nskry::ocr::RecognizeModel({image.data, image.total() * 4}, image.cols, image.rows, argv[1], 2);
        Value::Array full, variants, synthetic, roiControls, counterexamples;
        for (size_t i = 0; i < captured.size(); ++i) {
            SaveImage(output / (L"crop-" + std::to_wstring(i) + L".png"), captured[i].crop);
            full.push_back(captured[i].trace);
        }
        // The recognizer session is created only after the full-pipeline model
        // sessions have been destroyed; this is a bounded test process.
        Recognizer recognizer(argv[1]);
        // Follow-up geometry control using original pixels, never replacement
        // text. Fixed margins are declared before running these controls.
        cv::Mat originalBgr;
        cv::cvtColor(image, originalBgr, cv::COLOR_BGRA2BGR);
        for (size_t i = 0; i < raw.lines.size(); ++i)
            for (int margin : {0, 8}) {
                const auto &r = raw.lines[i].rect;
                const int left = (std::max)(0, r.left - margin), top = (std::max)(0, r.top - margin);
                const int right = (std::min)(image.cols, r.right + margin),
                          bottom = (std::min)(image.rows, r.bottom + margin);
                const auto crop = originalBgr(cv::Rect(left, top, right - left, bottom - top)).clone();
                SaveImage(output / (L"original-roi-" + std::to_wstring(i) + L"-" + std::to_wstring(margin) +
                                    L".png"),
                          crop);
                for (int repeat = 1; repeat <= 3; ++repeat)
                    roiControls.emplace_back(Value::Object{{L"line", Value(double(i))},
                                                           {L"margin", Value(double(margin))},
                                                           {L"run", Value(double(repeat))},
                                                           {L"baseline", Value(raw.lines[i].text)},
                                                           {L"trace", recognizer.Run(crop)}});
            }
        for (size_t i = 0; i < captured.size(); ++i) {
            const auto &original = captured[i];
            for (int mode = 0; mode < 6; ++mode) {
                cv::Mat crop;
                if (mode == 0)
                    crop = original.crop.clone();
                if (mode == 1)
                    cv::resize(original.crop, crop, {}, 2, 2, cv::INTER_CUBIC);
                if (mode == 2) {
                    cv::Mat gray;
                    cv::cvtColor(original.crop, gray, cv::COLOR_BGR2GRAY);
                    cv::cvtColor(gray, crop, cv::COLOR_GRAY2BGR);
                }
                if (mode == 3)
                    original.crop.convertTo(crop, -1, 1.5, -40);
                if (mode == 4)
                    cv::bitwise_not(original.crop, crop);
                if (mode == 5) {
                    cv::Mat gray, equalized;
                    cv::cvtColor(original.crop, gray, cv::COLOR_BGR2GRAY);
                    cv::createCLAHE(2.0, cv::Size(8, 8))->apply(gray, equalized);
                    cv::cvtColor(equalized, crop, cv::COLOR_GRAY2BGR);
                }
                SaveImage(output /
                              (L"variant-" + std::to_wstring(i) + L"-" + std::to_wstring(mode) + L".png"),
                          crop);
                for (int repeat = 1; repeat <= 3; ++repeat) {
                    auto trace = recognizer.Run(crop, original.width);
                    variants.emplace_back(Value::Object{{L"crop", Value(double(i))},
                                                        {L"mode", Value(double(mode))},
                                                        {L"run", Value(double(repeat))},
                                                        {L"trace", std::move(trace)}});
                }
            }
        }
        unsigned index{};
        for (const auto *font : {L"Segoe UI", L"Consolas"})
            for (int size : {20, 36})
                for (bool dark : {false, true})
                    for (int gap : {0, 4, 12, 24})
                        for (const auto &groups : std::vector<std::vector<std::wstring>>{
                                 {L"25", L"29", L"35"}, {L"12", L"34", L"56"}}) {
                            auto crop = Digits(font, size, gap, dark, groups);
                            std::wstring expected;
                            for (const auto &g : groups) {
                                if (!expected.empty() && gap)
                                    expected += L' ';
                                expected += g;
                            }
                            SaveImage(output / (L"digits-" + std::to_wstring(index) + L".png"), crop);
                            auto trace = recognizer.Run(crop);
                            synthetic.emplace_back(Value::Object{{L"id", Value(double(index++))},
                                                                 {L"font", Value(std::wstring(font))},
                                                                 {L"size", Value(double(size))},
                                                                 {L"gap_px", Value(double(gap))},
                                                                 {L"dark", Value(dark)},
                                                                 {L"expected", Value(expected)},
                                                                 {L"trace", std::move(trace)}});
                        }
        // Expanded font/size controls, subsequently used during alignment-rule
        // refinement (not an independent blind holdout). Retain original cases.
        for (const auto *font : {L"Segoe UI", L"Consolas", L"Arial", L"Tahoma"})
            for (int size : {16, 20, 28, 36})
                for (bool dark : {false, true})
                    for (const auto *value : {L"1234567890", L"1000000123", L"1234.5678", L"2026-10-26",
                                              L"build123456", L"12:34:56", L"192.168.1.1", L"001122334455"}) {
                        const auto crop = Digits(font, size, 0, dark, {value});
                        counterexamples.emplace_back(Value::Object{{L"font", Value(std::wstring(font))},
                                                                   {L"size", Value(double(size))},
                                                                   {L"dark", Value(dark)},
                                                                   {L"expected", Value(std::wstring(value))},
                                                                   {L"trace", recognizer.Run(crop)}});
                    }
        Write(output / L"results.json",
              Value(Value::Object{{L"full_pipeline", Value(std::move(full))},
                                  {L"variants", Value(std::move(variants))},
                                  {L"synthetic", Value(std::move(synthetic))},
                                  {L"counterexamples", Value(std::move(counterexamples))},
                                  {L"roi_controls", Value(std::move(roiControls))}}));
        std::cout << "Diagnostic complete; NOT a production quality pass.\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    Gdiplus::GdiplusShutdown(token);
    return status;
}
