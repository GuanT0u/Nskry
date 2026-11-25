#pragma once
#include "../../sdk/nskry_input.h"
namespace nskry {
struct ShortcutDefinition {
    std::wstring id, label, defaultShortcut, pluginId, pluginName;
    input::Kind kind = input::Kind::Key;
    bool official = false, generalJump = false;
    bool pluginCommand = false; // Optional commands v1; not a capture/launch route.
};
inline std::vector<ShortcutDefinition> CoreShortcuts() {
    using K = input::Kind;
    return {{L"core.capture", L"Capture", L"Ctrl+Alt+A", {}, {}, K::Global},
            {L"core.exit", L"Quit", L"Ctrl+Alt+Q", {}, {}, K::Global},
            {L"core.monitor", L"Monitor a selected region", L"", {}, {}, K::Global, false, true},
            {L"core.cancel", L"Cancel selection / annotation", L"Esc"},
            {L"core.undo", L"Undo annotation", L"Ctrl+Z"},
            {L"core.redo", L"Redo annotation", L"Ctrl+Y"},
            {L"core.copy", L"Copy selection", L"Ctrl+C"},
            {L"core.save", L"Save selection", L"Ctrl+S"},
            {L"core.text.accept", L"Confirm annotation text", L"Enter"},
            {L"core.text.cancel", L"Cancel annotation text", L"Esc"},
            {L"core.editor.cancel", L"Cancel editor / tool mode", L"Esc"},
            {L"core.editor.confirm", L"Confirm cut / OCR selection", L"Enter"},
            {L"core.precision", L"Hold for precision adjustment", L"Alt", {}, {}, K::Hold},
            {L"core.editor.zoom", L"Editor zoom", L"Wheel", {}, {}, K::Wheel},
            {L"core.editor.pan_x", L"Editor horizontal pan", L"Shift+Wheel", {}, {}, K::Wheel},
            {L"core.editor.pan_y", L"Editor vertical pan", L"Alt+Wheel", {}, {}, K::Wheel}};
}
// Product launch placeholders only. Installed DLL-local bindings must come from its manifest.
inline void AddBuiltinCapturePlaceholders(std::vector<ShortcutDefinition> &rows) {
    struct CapturePlugin {
        const wchar_t *id, *name, *launchLabel;
    };
    static constexpr CapturePlugin plugins[] = {
        {L"nskry-ocr", L"Text recognition", L"Recognize a selected region"},
        {L"nskry-scroll", L"Long Screenshot", L"Scroll capture a selected region"},
    };
    for (const auto &[id, name, label] : plugins) {
        const bool present = std::any_of(rows.begin(), rows.end(), [&](const auto &row) { return row.pluginId == id; });
        if (!present)
            rows.push_back({std::wstring(L"plugin.") + id + L".launch", label, L"", id, name,
                            input::Kind::Global, true, true});
    }
}
inline bool ValidateShortcuts(const std::vector<ShortcutDefinition> &definitions,
                              const std::unordered_map<std::wstring, std::wstring> &values, std::wstring &error) {
    error.clear();
    struct Used {
        input::Chord chord;
        input::Kind kind;
        std::wstring scope, label;
    };
    std::vector<Used> used;
    for (const auto &row : definitions) {
        auto it = values.find(row.id);
        const auto text = it == values.end() ? row.defaultShortcut : it->second;
        if (text.empty())
            continue;
        input::Chord c;
        if (!input::Parse(text, row.kind, c)) {
            error = L"Invalid shortcut for " + row.label +
                    L". Use modifiers and one main key; wheel rows use e.g. Shift+Wheel.";
            return false;
        }
        std::wstring scope = row.pluginId.empty() ? L"core.annotation" : row.pluginId;
        if (row.id.starts_with(L"core.editor."))
            scope = L"core.editor";
        if (row.id.starts_with(L"core.text."))
            scope = L"core.text";
        for (const auto &old : used) {
            const bool global = row.kind == input::Kind::Global || old.kind == input::Kind::Global;
            const bool keyKinds = (row.kind == input::Kind::Key || row.kind == input::Kind::Global) &&
                                  (old.kind == input::Kind::Key || old.kind == input::Kind::Global);
            if (c.modifiers == old.chord.modifiers && c.key == old.chord.key && c.wheel == old.chord.wheel &&
                ((global && keyKinds) || (scope == old.scope && row.kind == old.kind))) {
                error = L"Conflicting shortcuts: " + old.label + L" / " + row.label;
                return false;
            }
        }
        used.push_back({c, row.kind, scope, row.label});
    }
    return true;
}
enum class CursorStyle {
    Arrow,
    Cross,
    Hand,
    SizeAll,
    SizeWE,
    SizeNS,
    Custom
};
struct CursorChoice {
    CursorStyle style = CursorStyle::Arrow;
    std::wstring file;
    bool operator==(const CursorChoice &) const = default;
};
enum class DefaultShape { Rectangle, Ellipse, Line };

struct InteractionStyle {
    DefaultShape defaultShape = DefaultShape::Rectangle;
    bool dashed = false;
    int borderWidth = 2, borderOpacity = 100;
    COLORREF borderColor = RGB(20, 150, 255);
    int cutStyle = 0, cutOpacity = 30;
    COLORREF cutColor = RGB(190, 30, 30);
    bool cutEdges = false;
    std::array<CursorChoice, 5> cursors{{{CursorStyle::Cross, {}},
                                         {CursorStyle::SizeAll, {}},
                                         {CursorStyle::SizeNS, {}},
                                         {CursorStyle::SizeWE, {}},
                                         {CursorStyle::SizeAll, {}}}};
    bool operator==(const InteractionStyle &) const = default;
};
inline InteractionStyle NormalizeInteractionStyle(InteractionStyle s) {
    if (s.defaultShape != DefaultShape::Ellipse && s.defaultShape != DefaultShape::Line)
        s.defaultShape = DefaultShape::Rectangle;
    s.borderWidth = std::clamp(s.borderWidth, 1, 6);
    s.borderOpacity = std::clamp(s.borderOpacity, 0, 100);
    s.cutStyle = std::clamp(s.cutStyle, 0, 3);
    s.cutOpacity = std::clamp(s.cutOpacity, 0, 100);
    for (auto &c : s.cursors)
        if (int(c.style) < 0 || int(c.style) > 6)
            c.style = CursorStyle::Arrow;
    return s;
}
} // namespace nskry
