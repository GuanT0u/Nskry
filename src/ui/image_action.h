#pragma once

#include <functional>
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

} // namespace nskry
