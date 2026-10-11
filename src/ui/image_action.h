#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

namespace nskry {

// A host-owned action that can operate on a currently visible bitmap.  The
// bitmap is borrowed and is valid only while invoke is running; an extension
// that needs it asynchronously must make its own copy before returning.
struct ImageAction {
    std::wstring id;
    std::wstring label;
    std::function<void(HBITMAP, int, int, RECT, HWND)> invoke;
};

using ImageActions = std::vector<ImageAction>;
// Core-only structural editor service. Transfers ownership of the bitmap.
using ImageEditorCallback = std::function<void(HBITMAP, int, int)>;

// Host-owned surface: one action runs directly, several use a bounded menu.
// No publisher id or translated display name is privileged.
std::optional<size_t> ChooseImageAction(HWND owner, const ImageActions& actions);

} // namespace nskry
