#include "pch.h"
#include "ui/image_action.h"

namespace nskry {
std::optional<size_t> ChooseImageAction(HWND owner, const ImageActions& actions) {
    if (actions.empty()) return std::nullopt;
    if (actions.size() == 1) return actions.front().invoke ? std::optional<size_t>{0} : std::nullopt;
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return std::nullopt;
    for (size_t i = 0; i < actions.size() && i < 100; ++i) {
        if (actions[i].invoke) ::AppendMenuW(menu, MF_STRING, i + 1, actions[i].label.c_str());
    }
    POINT cursor{}; ::GetCursorPos(&cursor);
    ::SetForegroundWindow(owner);
    const UINT chosen = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, owner, nullptr);
    ::DestroyMenu(menu);
    return chosen && chosen <= actions.size() ? std::optional<size_t>{chosen - 1} : std::nullopt;
}
} // namespace nskry
