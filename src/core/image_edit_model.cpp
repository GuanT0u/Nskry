#include "core/image_edit_model.h"
#include <algorithm>

namespace nskry {
ImageEditModel::ImageEditModel(int width, int height) {
    if (width > 0 && height > 0) {
        m_state.x.push_back({0, width}); m_state.y.push_back({0, height});
        m_state.crop = {0, 0, width, height};
    }
}
int ImageEditModel::Length(const std::vector<Span>& spans) {
    int sum = 0; for (const auto& s : spans) sum += s.last - s.first; return sum;
}
int ImageEditModel::Width() const { return Length(m_state.x); }
int ImageEditModel::Height() const { return Length(m_state.y); }
bool ImageEditModel::SetCrop(ImageRect r) {
    if (r.left < 0 || r.top < 0 || r.right > Width() || r.bottom > Height() ||
        r.right <= r.left || r.bottom <= r.top) return false;
    m_state.crop = r; return true;
}
std::vector<ImageEditModel::Span> ImageEditModel::Retain(const std::vector<Span>& spans, int first, int last) {
    std::vector<Span> out; int offset = 0;
    for (const auto& s : spans) {
        const int length = s.last - s.first;
        const int a = (std::max)(first, offset), b = (std::min)(last, offset + length);
        if (b > a) {
            Span kept{s.first + a - offset, s.first + b - offset};
            if (!out.empty() && out.back().last == kept.first) out.back().last = kept.last;
            else out.push_back(kept);
        }
        offset += length;
    }
    return out;
}
bool ImageEditModel::Cut(bool columns, int first, int second) {
    if (first > second) std::swap(first, second);
    const int length = columns ? Width() : Height();
    const auto crop = m_state.crop;
    const int cropStart = columns ? crop.left : crop.top, cropEnd = columns ? crop.right : crop.bottom;
    // Only accept cuts inside the current export area, leaving it nonempty.
    if (first < cropStart || second > cropEnd || second <= first || second - first >= cropEnd - cropStart ||
        m_undo.size() >= MaxHistory) return false;
    State next = m_state;
    auto& axis = columns ? next.x : next.y;
    auto prefix = Retain(axis, 0, first), suffix = Retain(axis, second, length);
    prefix.insert(prefix.end(), suffix.begin(), suffix.end()); axis = std::move(prefix);
    if (next.x.size() * next.y.size() > MaxPieces) return false;
    if (columns) next.crop.right -= second - first;
    else next.crop.bottom -= second - first;
    m_undo.push_back(m_state); m_state = std::move(next); m_redo.clear(); return true;
}
bool ImageEditModel::Undo() {
    if (m_undo.empty()) return false;
    m_redo.push_back(std::move(m_state)); m_state = std::move(m_undo.back()); m_undo.pop_back(); return true;
}
bool ImageEditModel::Redo() {
    if (m_redo.empty()) return false;
    m_undo.push_back(std::move(m_state)); m_state = std::move(m_redo.back()); m_redo.pop_back(); return true;
}
std::vector<ImagePiece> ImageEditModel::Pieces(ImageRect r) const {
    std::vector<ImagePiece> out;
    if (r.left < 0 || r.top < 0 || r.right > Width() || r.bottom > Height() || r.right <= r.left || r.bottom <= r.top) return out;
    int y = 0;
    for (const auto& sy : m_state.y) {
        const int yEnd = y + sy.last - sy.first;
        int x = 0;
        for (const auto& sx : m_state.x) {
            const int xEnd = x + sx.last - sx.first;
            ImageRect clipped{(std::max)(x, r.left), (std::max)(y, r.top),
                              (std::min)(xEnd, r.right), (std::min)(yEnd, r.bottom)};
            if (clipped.right > clipped.left && clipped.bottom > clipped.top)
                out.push_back({{sx.first + clipped.left - x, sy.first + clipped.top - y,
                                sx.first + clipped.right - x, sy.first + clipped.bottom - y}, clipped});
            x = xEnd;
        }
        y = yEnd;
    }
    return out;
}
}
