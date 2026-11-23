#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace nskry {
class ToolbarAccessibility {
public:
    struct Item {
        std::wstring name;
        RECT screenBounds{};
        bool selected{};
    };

    void Update(HWND window, std::vector<Item> items, const wchar_t* name, const wchar_t* action);
    void Focus(HWND window, LONG child);
    void Detach();
    LRESULT GetObject(HWND window, WPARAM wp, LPARAM lp) const;

    static constexpr UINT InvokeMessage = WM_APP + 0x193;
    static constexpr UINT FocusMessage = WM_APP + 0x194;

    struct State {
        std::mutex mutex;
        HWND window{};
        std::vector<Item> items;
        std::wstring name, action;
        LONG focused{};
        UINT generation{};
    };
    bool Current(UINT generation) const;

private:
    std::shared_ptr<State> m_state = std::make_shared<State>();
};
} // namespace nskry
