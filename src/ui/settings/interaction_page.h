#pragma once
#include "core/input_settings.h"
namespace nskry {
class InteractionPage {
  public:
    explicit InteractionPage(HWND parent);
    ~InteractionPage();
    HWND Hwnd() const {
        return m_hwnd;
    }
    void Load(InteractionStyle);
    InteractionStyle Value();

  private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void Layout();
    void Scroll(int);
    struct Item {
        HWND hwnd;
        int x, y, w, h;
    };
    std::vector<Item> m_items;
    HWND m_hwnd{};
    int m_scroll{};
    InteractionStyle m_style;
    std::array<HWND, 5> m_cursor{}, m_files{};
};
} // namespace nskry
