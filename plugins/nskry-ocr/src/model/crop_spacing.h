#pragma once
#include "ctc_decode.h"
#include <algorithm>
#include <opencv2/imgproc.hpp>

namespace nskry::ocr {
inline void RecoverCropSpacing(CtcText &text, const cv::Mat &bgr) {
    if (std::count_if(text.text.begin(), text.text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }) <
        4)
        return;
    cv::Mat gray, mask;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    double low{}, high{};
    cv::minMaxLoc(gray, &low, &high);
    if (high - low < 32)
        return; // No reliable foreground evidence.
    cv::threshold(gray, mask, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    if (cv::countNonZero(mask) > mask.total() / 2)
        cv::bitwise_not(mask, mask);
    // Whole connected glyph components avoid turning a narrow '1', or a CTC
    // midpoint cutting through a digit, into invented whitespace.
    cv::Mat labels, stats, centers;
    const int count = cv::connectedComponentsWithStats(mask, labels, stats, centers, 8);
    if (count > 4096)
        return; // Complex/noisy crop: do not spend an unbounded alignment budget.
    std::vector<CtcInkBounds> ink(text.glyphs.size());
    for (size_t i = 0; i < text.glyphs.size(); ++i) {
        const auto &g = text.glyphs[i];
        if (g.length != 1 || text.text[g.offset] < L'0' || text.text[g.offset] > L'9')
            continue;
        const float expected = (g.emissionLeft + g.emissionRight) * .5f * bgr.cols;
        const float cellWidth = (g.right - g.left) * bgr.cols;
        float distance = cellWidth * .6f;
        for (int label = 1; label < count; ++label) {
            const int left = stats.at<int>(label, cv::CC_STAT_LEFT),
                      width = stats.at<int>(label, cv::CC_STAT_WIDTH);
            if (stats.at<int>(label, cv::CC_STAT_HEIGHT) < bgr.rows * .2f ||
                stats.at<int>(label, cv::CC_STAT_AREA) < std::max(3, bgr.rows / 4) ||
                width > cellWidth * 1.6f)
                continue;
            const float delta = std::abs(left + width * .5f - expected);
            if (delta < distance) {
                distance = delta;
                ink[i] = {float(left) / bgr.cols, float(left + width) / bgr.cols};
            }
        }
    }
    RecoverNumericSpacing(text, ink);
}
} // namespace nskry::ocr
