#pragma once
#include <vector>
#include <cstddef>

namespace nskry {
struct ImageRect { int left{}, top{}, right{}, bottom{}; };
struct ImagePiece { ImageRect source, output; };

// Non-destructive, whole-row/whole-column editing. Owns metadata, never pixels.
class ImageEditModel {
public:
    ImageEditModel(int width = 0, int height = 0);
    int Width() const;
    int Height() const;
    ImageRect Crop() const { return m_state.crop; }
    bool SetCrop(ImageRect crop);
    bool Cut(bool columns, int first, int second);
    bool Undo();
    bool Redo();
    std::vector<ImagePiece> Pieces(ImageRect region) const;
    static constexpr size_t MaxPieces = 128, MaxHistory = 256;
private:
    struct Span { int first, last; };
    struct State { std::vector<Span> x, y; ImageRect crop; };
    static int Length(const std::vector<Span>& spans);
    static std::vector<Span> Retain(const std::vector<Span>& spans, int first, int last);
    State m_state;
    std::vector<State> m_undo, m_redo;
};
}
