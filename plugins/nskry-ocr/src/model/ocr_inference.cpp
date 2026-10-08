// PP-OCRv4 DB detection / CTC recognition. Geometry follows the upstream
// PaddleOCR/RapidOCR pipeline (Apache-2.0); see THIRD_PARTY_NOTICES.md.
#include "ocr_inference.h"
#include "ctc_decode.h"
#include <windows.h>
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <clipper.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace nskry::ocr {
namespace {
using Clock = std::chrono::steady_clock;
using Quad = std::array<cv::Point2f, 4>;
double Ms(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}
std::wstring Wide(const std::string& value) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count && !value.empty()) throw std::runtime_error("Invalid model character metadata");
    std::wstring result(count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
Quad Order(std::vector<cv::Point2f> points) {
    std::sort(points.begin(), points.end(), [](auto a, auto b) { return a.x < b.x; });
    if (points[0].y > points[1].y) std::swap(points[0], points[1]);
    if (points[2].y > points[3].y) std::swap(points[2], points[3]);
    return {points[0], points[2], points[3], points[1]};
}
template<class Point> std::pair<Quad, float> MiniBox(const std::vector<Point>& contour) {
    const auto rect = cv::minAreaRect(contour);
    std::vector<cv::Point2f> points(4); rect.points(points.data());
    return {Order(std::move(points)), std::min(rect.size.width, rect.size.height)};
}
Quad Expand(const Quad& quad) {
    const std::vector<cv::Point2f> points(quad.begin(), quad.end());
    const double perimeter = cv::arcLength(points, true);
    if (perimeter <= 0) throw std::runtime_error("Invalid detection contour");
    const double distance = std::abs(cv::contourArea(points)) * 1.6 / perimeter;
    ClipperLib::Path path;
    for (const auto& p : quad) path.emplace_back(static_cast<ClipperLib::cInt>(p.x), static_cast<ClipperLib::cInt>(p.y));
    ClipperLib::ClipperOffset offset;
    offset.AddPath(path, ClipperLib::jtRound, ClipperLib::etClosedPolygon);
    ClipperLib::Paths expanded; offset.Execute(expanded, distance);
    if (expanded.size() != 1 || expanded[0].size() < 4) return quad;
    std::vector<cv::Point2f> result;
    for (const auto& p : expanded[0]) result.emplace_back(static_cast<float>(p.X), static_cast<float>(p.Y));
    return MiniBox(result).first;
}
float BoxScore(const cv::Mat& probability, const Quad& quad) {
    float minX = quad[0].x, maxX = minX, minY = quad[0].y, maxY = minY;
    for (auto p : quad) { minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); }
    const int left = std::clamp(static_cast<int>(std::floor(minX)), 0, probability.cols - 1);
    const int top = std::clamp(static_cast<int>(std::floor(minY)), 0, probability.rows - 1);
    const int right = std::clamp(static_cast<int>(std::ceil(maxX)), left, probability.cols - 1);
    const int bottom = std::clamp(static_cast<int>(std::ceil(maxY)), top, probability.rows - 1);
    cv::Mat mask = cv::Mat::zeros(bottom - top + 1, right - left + 1, CV_8U);
    std::vector<cv::Point> points;
    for (auto p : quad) points.emplace_back(static_cast<int>(p.x - left), static_cast<int>(p.y - top));
    cv::fillPoly(mask, std::vector<std::vector<cv::Point>>{points}, cv::Scalar(1));
    return static_cast<float>(cv::mean(probability(cv::Rect(left, top, mask.cols, mask.rows)), mask)[0]);
}
std::vector<float> Tensor(const cv::Mat& image, int tensorWidth = 0) {
    const int width = tensorWidth ? tensorWidth : image.cols;
    const size_t plane = static_cast<size_t>(width) * image.rows;
    std::vector<float> values(plane * 3, 0.f); // Normalized padding is zero.
    for (int y = 0; y < image.rows; ++y) {
        const auto* row = image.ptr<cv::Vec3b>(y);
        for (int x = 0; x < image.cols; ++x) for (int channel = 0; channel < 3; ++channel)
            values[channel * plane + y * width + x] = row[x][channel] / 127.5f - 1.f;
    }
    return values;
}
struct Session {
    Ort::Session value;
    std::string input, output;
    Session(Ort::Env& env, const std::filesystem::path& model, const Ort::SessionOptions& options)
        : value(env, model.c_str(), options) {
        Ort::AllocatorWithDefaultOptions allocator;
        input = value.GetInputNameAllocated(0, allocator).get();
        output = value.GetOutputNameAllocated(0, allocator).get();
    }
    Ort::Value Run(std::span<float> data, const std::array<int64_t, 4>& shape) {
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, data.data(), data.size(), shape.data(), shape.size());
        const char* inputs[]{input.c_str()}, *outputs[]{output.c_str()};
        auto result = value.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        return std::move(result.front());
    }
};
std::vector<Quad> Detect(Session& detector, const cv::Mat& image) {
    // A bounded raster budget is separate from the minimum text-detection size.
    double ratio = std::max(1.0, 320.0 / std::min(image.cols, image.rows));
    ratio = std::min(ratio, 2048.0 / std::max(image.cols, image.rows));
    const int width = std::clamp(static_cast<int>(std::nearbyint(static_cast<int>(image.cols * ratio) / 32.0)) * 32, 32, 2048);
    const int height = std::clamp(static_cast<int>(std::nearbyint(static_cast<int>(image.rows * ratio) / 32.0)) * 32, 32, 2048);
    cv::Mat resized; cv::resize(image, resized, cv::Size(width, height));
    auto data = Tensor(resized);
    auto result = detector.Run(data, {1, 3, height, width});
    const auto info = result.GetTensorTypeAndShapeInfo();
    const auto shape = info.GetShape();
    if (shape.size() != 4 || shape[0] != 1 || shape[1] != 1 || shape[2] <= 0 || shape[3] <= 0 ||
        shape[2] > 2048 || shape[3] > 2048) throw std::runtime_error("Unexpected detection tensor");
    cv::Mat probability(static_cast<int>(shape[2]), static_cast<int>(shape[3]), CV_32F, result.GetTensorMutableData<float>());
    cv::Mat mask; cv::compare(probability, .3, mask, cv::CMP_GT);
    cv::dilate(mask, mask, cv::Mat::ones(2, 2, CV_8U));
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    std::vector<Quad> boxes;
    for (size_t i = 0; i < std::min<size_t>(1000, contours.size()); ++i) {
        if (contours[i].size() < 3) continue;
        const auto [quad, shortSide] = MiniBox(contours[i]);
        if (shortSide < 3 || BoxScore(probability, quad) < .5f) continue;
        const Quad expanded = Expand(quad);
        if (cv::norm(expanded[0] - expanded[1]) < 5 || cv::norm(expanded[0] - expanded[3]) < 5) continue;
        Quad mapped;
        for (size_t j = 0; j < 4; ++j) {
            mapped[j].x = std::clamp(static_cast<float>(std::nearbyint(expanded[j].x / probability.cols * image.cols)), 0.f, static_cast<float>(image.cols - 1));
            mapped[j].y = std::clamp(static_cast<float>(std::nearbyint(expanded[j].y / probability.rows * image.rows)), 0.f, static_cast<float>(image.rows - 1));
        }
        if (cv::norm(mapped[0] - mapped[1]) <= 3 || cv::norm(mapped[0] - mapped[3]) <= 3) continue;
        boxes.push_back(mapped);
    }
    std::stable_sort(boxes.begin(), boxes.end(), [](const auto& a, const auto& b) {
        return a[0].y < b[0].y || (a[0].y == b[0].y && a[0].x < b[0].x);
    });
    // Match upstream reading order tolerance without combining separate columns.
    for (size_t i = 1; i < boxes.size(); ++i) for (size_t j = i; j > 0; --j) {
        if (std::abs(boxes[j][0].y - boxes[j - 1][0].y) < 10 && boxes[j][0].x < boxes[j - 1][0].x)
            std::swap(boxes[j], boxes[j - 1]);
        else break;
    }
    return boxes;
}
wire::Rect Bounds(const Quad& quad, double scaleX, double scaleY, int padding, int width, int height) {
    double left = width, top = height, right = 0, bottom = 0;
    for (auto p : quad) {
        const double x = p.x * scaleX, y = (p.y - padding) * scaleY;
        left = std::min(left, x); top = std::min(top, y); right = std::max(right, x); bottom = std::max(bottom, y);
    }
    const int x0 = std::clamp(static_cast<int>(std::floor(left)), 0, width - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(top)), 0, height - 1);
    return {x0, y0, std::clamp(static_cast<int>(std::ceil(right)), x0 + 1, width),
        std::clamp(static_cast<int>(std::ceil(bottom)), y0 + 1, height)};
}
Quad GlyphQuad(const Quad& quad, float left, float right, bool vertical) {
    if (vertical) return {quad[0] + (quad[3] - quad[0]) * left, quad[1] + (quad[2] - quad[1]) * left,
                         quad[1] + (quad[2] - quad[1]) * right, quad[0] + (quad[3] - quad[0]) * right};
    return {quad[0] + (quad[1] - quad[0]) * left, quad[0] + (quad[1] - quad[0]) * right,
            quad[3] + (quad[2] - quad[3]) * right, quad[3] + (quad[2] - quad[3]) * left};
}
} // namespace

wire::Reply RecognizeModel(std::span<const uint8_t> bgra, int width, int height,
                           const std::filesystem::path& models, int threads) {
    if (width <= 0 || height <= 0 || bgra.size() != static_cast<size_t>(width) * height * 4) throw std::invalid_argument("Invalid OCR image");
    const auto started = Clock::now();
    cv::setNumThreads(1); // ORT owns the bounded inference thread count.
    Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "NskryOCR");
    env.DisableTelemetryEvents();
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(std::clamp(threads, 1, 4));
    options.SetInterOpNumThreads(1);
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    options.AddConfigEntry("session.intra_op.allow_spinning", "0");
    options.AddConfigEntry("session.inter_op.allow_spinning", "0");
    // CPU arena lives only for this request; process exit releases all sessions.
    Session detector(env, models / L"ch_PP-OCRv4_det_infer.onnx", options);
    Session recognizer(env, models / L"ch_PP-OCRv4_rec_infer.onnx", options);
    Ort::AllocatorWithDefaultOptions allocator;
    const auto metadata = recognizer.value.GetModelMetadata().LookupCustomMetadataMapAllocated("character", allocator);
    if (!metadata) throw std::runtime_error("Recognition model has no character metadata");
    std::vector<std::wstring> vocabulary(1); // CTC blank class.
    std::istringstream characters(metadata.get()); std::string character;
    while (std::getline(characters, character)) {
        if (!character.empty() && character.back() == '\r') character.pop_back();
        vocabulary.push_back(Wide(character));
    }
    vocabulary.emplace_back(L" ");
    wire::Reply reply; reply.loadMs = Ms(started);
    const auto inferenceStart = Clock::now();
    cv::Mat original(height, width, CV_8UC4, const_cast<uint8_t*>(bgra.data()));
    cv::Mat image; cv::cvtColor(original, image, cv::COLOR_BGRA2BGR);
    // Preserve original display coordinates even for a bounded large image.
    const double ratio = std::min(1.0, 2000.0 / std::max(width, height));
    if (ratio < 1) cv::resize(image, image, cv::Size(std::max(1, static_cast<int>(width * ratio)),
                                                  std::max(1, static_cast<int>(height * ratio))));
    const double scaleX = static_cast<double>(width) / image.cols, scaleY = static_cast<double>(height) / image.rows;
    int padding = 0;
    if (image.rows < 30 || static_cast<double>(image.cols) / image.rows > 8)
        padding = std::abs(std::max(image.cols / 8, 30) * 2 - image.rows) / 2;
    if (padding) cv::copyMakeBorder(image, image, padding, padding, 0, 0, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    const auto boxes = Detect(detector, image);
    struct Crop { cv::Mat pixels; bool vertical{}; };
    std::vector<Crop> crops; crops.reserve(boxes.size());
    for (const auto& quad : boxes) {
        const int cropWidth = std::max(1, static_cast<int>(std::max(cv::norm(quad[0] - quad[1]), cv::norm(quad[2] - quad[3]))));
        const int cropHeight = std::max(1, static_cast<int>(std::max(cv::norm(quad[0] - quad[3]), cv::norm(quad[1] - quad[2]))));
        const Quad destination{cv::Point2f(0, 0), cv::Point2f(static_cast<float>(cropWidth), 0),
                              cv::Point2f(static_cast<float>(cropWidth), static_cast<float>(cropHeight)),
                              cv::Point2f(0, static_cast<float>(cropHeight))};
        cv::Mat crop;
        cv::warpPerspective(image, crop, cv::getPerspectiveTransform(quad.data(), destination.data()),
            cv::Size(cropWidth, cropHeight), cv::INTER_CUBIC, cv::BORDER_REPLICATE);
        const bool vertical = cropHeight >= cropWidth * 1.5;
        if (vertical) cv::rotate(crop, crop, cv::ROTATE_90_COUNTERCLOCKWISE);
        crops.push_back({std::move(crop), vertical});
    }
    // Batches sorted by aspect ratio prevent short labels paying for long lines.
    std::vector<size_t> order(crops.size()); std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return static_cast<double>(crops[a].pixels.cols) / crops[a].pixels.rows <
               static_cast<double>(crops[b].pixels.cols) / crops[b].pixels.rows;
    });
    std::vector<wire::Line> recognized(crops.size());
    for (size_t first = 0; first < order.size(); first += 6) {
        const size_t count = std::min<size_t>(6, order.size() - first);
        double maxAspect = 320.0 / 48;
        for (size_t i = first; i < first + count; ++i) maxAspect = std::max(maxAspect,
            static_cast<double>(crops[order[i]].pixels.cols) / crops[order[i]].pixels.rows);
        const int tensorWidth = std::clamp(static_cast<int>(48 * maxAspect), 320, 8192);
        const size_t itemSize = static_cast<size_t>(3) * 48 * tensorWidth;
        std::vector<float> batch(itemSize * count);
        std::vector<int> contentWidths;
        for (size_t i = 0; i < count; ++i) {
            const auto& crop = crops[order[first + i]].pixels;
            const int resizedWidth = std::clamp(static_cast<int>(std::ceil(48.0 * crop.cols / crop.rows)), 1, tensorWidth);
            contentWidths.push_back(resizedWidth);
            cv::Mat resized; cv::resize(crop, resized, cv::Size(resizedWidth, 48));
            const auto data = Tensor(resized, tensorWidth);
            std::copy(data.begin(), data.end(), batch.begin() + i * itemSize);
        }
        auto predictions = recognizer.Run(batch, {static_cast<int64_t>(count), 3, 48, tensorWidth});
        const auto info = predictions.GetTensorTypeAndShapeInfo(); const auto shape = info.GetShape();
        if (shape.size() != 3 || shape[0] != count || shape[1] <= 0 || shape[1] > 4096 || shape[2] != vocabulary.size())
            throw std::runtime_error("Unexpected recognition tensor");
        const size_t steps = static_cast<size_t>(shape[1]), classes = static_cast<size_t>(shape[2]);
        const auto* data = predictions.GetTensorData<float>();
        for (size_t i = 0; i < count; ++i) {
            auto text = DecodeCtc({data + i * steps * classes, steps * classes}, steps, classes, vocabulary,
                                  static_cast<float>(tensorWidth) / contentWidths[i]);
            if (text.text.empty() || text.confidence < .5f) continue;
            const size_t index = order[first + i]; const auto& quad = boxes[index];
            auto& line = recognized[index];
            line.rect = Bounds(quad, scaleX, scaleY, padding, width, height);
            line.confidence = text.confidence; line.text = std::move(text.text);
            for (const auto& glyph : text.glyphs)
                line.glyphs.push_back({Bounds(GlyphQuad(quad, glyph.left, glyph.right, crops[index].vertical),
                    scaleX, scaleY, padding, width, height), static_cast<uint32_t>(glyph.offset), static_cast<uint32_t>(glyph.length)});
        }
    }
    for (auto& line : recognized) if (!line.text.empty()) reply.lines.push_back(std::move(line));
    reply.inferenceMs = Ms(inferenceStart);
    return reply;
}
} // namespace nskry::ocr
