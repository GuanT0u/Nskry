#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional permission: Nskry Independent Open-Source Plugin Exception 1.0.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <windows.h>

// Optional, separately versioned extension. API 2 context/reserved stay intact.
struct NskryInputBindingV1 {
    const wchar_t *id;
    const wchar_t *shortcut;
};
struct NskryInputConfigV1 {
    uint32_t structSize, version, count;
    const NskryInputBindingV1 *bindings; // Borrowed only during configure; copy strings.
};
using NskryConfigureInputV1 = void(__cdecl *)(const NskryInputConfigV1 *);

namespace nskry::input {
enum class Kind {
    Global,
    Key,
    Hold,
    Wheel
};
struct Chord {
    UINT modifiers{}, key{};
    bool wheel{};
};
inline UINT NormalizeKey(UINT key) {
    if (key == VK_LCONTROL || key == VK_RCONTROL)
        return VK_CONTROL;
    if (key == VK_LSHIFT || key == VK_RSHIFT)
        return VK_SHIFT;
    if (key == VK_LMENU || key == VK_RMENU)
        return VK_MENU;
    if (key == VK_RWIN)
        return VK_LWIN;
    return key;
}
inline UINT Modifier(UINT key) {
    switch (NormalizeKey(key)) {
    case VK_CONTROL:
        return MOD_CONTROL;
    case VK_SHIFT:
        return MOD_SHIFT;
    case VK_MENU:
        return MOD_ALT;
    case VK_LWIN:
        return MOD_WIN;
    default:
        return 0;
    }
}
inline std::wstring KeyName(UINT key) {
    key = NormalizeKey(key);
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
        return std::wstring(1, wchar_t(key));
    if (key >= VK_F1 && key <= VK_F24)
        return L"F" + std::to_wstring(key - VK_F1 + 1);
    const std::pair<UINT, const wchar_t *> names[] = {{VK_CONTROL, L"Ctrl"},
                                                      {VK_MENU, L"Alt"},
                                                      {VK_SHIFT, L"Shift"},
                                                      {VK_LWIN, L"Win"},
                                                      {VK_ESCAPE, L"Esc"},
                                                      {VK_SNAPSHOT, L"PrintScreen"},
                                                      {VK_PAUSE, L"Pause"},
                                                      {VK_CAPITAL, L"CapsLock"},
                                                      {VK_NUMLOCK, L"NumLock"},
                                                      {VK_SCROLL, L"ScrollLock"},
                                                      {VK_APPS, L"Menu"},
                                                      {VK_VOLUME_MUTE, L"VolumeMute"},
                                                      {VK_VOLUME_DOWN, L"VolumeDown"},
                                                      {VK_VOLUME_UP, L"VolumeUp"},
                                                      {VK_MEDIA_NEXT_TRACK, L"MediaNext"},
                                                      {VK_MEDIA_PREV_TRACK, L"MediaPrevious"},
                                                      {VK_MEDIA_STOP, L"MediaStop"},
                                                      {VK_MEDIA_PLAY_PAUSE, L"MediaPlayPause"},
                                                      {VK_RETURN, L"Enter"},
                                                      {VK_SPACE, L"Space"},
                                                      {VK_TAB, L"Tab"},
                                                      {VK_INSERT, L"Insert"},
                                                      {VK_DELETE, L"Delete"},
                                                      {VK_BACK, L"Backspace"},
                                                      {VK_HOME, L"Home"},
                                                      {VK_END, L"End"},
                                                      {VK_PRIOR, L"PageUp"},
                                                      {VK_NEXT, L"PageDown"},
                                                      {VK_LEFT, L"Left"},
                                                      {VK_RIGHT, L"Right"},
                                                      {VK_UP, L"Up"},
                                                      {VK_DOWN, L"Down"},
                                                      {VK_OEM_PLUS, L"Plus"},
                                                      {VK_OEM_MINUS, L"Minus"},
                                                      {VK_OEM_COMMA, L"Comma"},
                                                      {VK_OEM_PERIOD, L"Period"},
                                                      {VK_OEM_1, L"Semicolon"},
                                                      {VK_OEM_2, L"Slash"},
                                                      {VK_OEM_3, L"Backtick"},
                                                      {VK_OEM_4, L"LeftBracket"},
                                                      {VK_OEM_5, L"Backslash"},
                                                      {VK_OEM_6, L"RightBracket"},
                                                      {VK_OEM_7, L"Quote"},
                                                      {VK_NUMPAD0, L"Num0"},
                                                      {VK_NUMPAD1, L"Num1"},
                                                      {VK_NUMPAD2, L"Num2"},
                                                      {VK_NUMPAD3, L"Num3"},
                                                      {VK_NUMPAD4, L"Num4"},
                                                      {VK_NUMPAD5, L"Num5"},
                                                      {VK_NUMPAD6, L"Num6"},
                                                      {VK_NUMPAD7, L"Num7"},
                                                      {VK_NUMPAD8, L"Num8"},
                                                      {VK_NUMPAD9, L"Num9"},
                                                      {VK_ADD, L"NumPlus"},
                                                      {VK_SUBTRACT, L"NumMinus"},
                                                      {VK_MULTIPLY, L"NumMultiply"},
                                                      {VK_DIVIDE, L"NumDivide"},
                                                      {VK_DECIMAL, L"NumDecimal"}};
    for (auto [code, name] : names)
        if (code == key)
            return name;
    return {};
}
inline std::wstring Upper(std::wstring s) {
    for (auto &c : s)
        c = wchar_t(towupper(c));
    return s;
}
inline bool Parse(std::wstring text, Kind kind, Chord &chord) {
    static const auto names = [] {
        std::unordered_map<std::wstring, UINT> out;
        for (UINT v = 1; v < 256; ++v) {
            auto name = KeyName(v);
            if (!name.empty())
                out.emplace(Upper(name), NormalizeKey(v));
        }
        return out;
    }();
    chord = {};
    if (text.empty() || text.size() > 128 || text.back() == '+')
        return false;
    std::wstringstream stream(text);
    std::wstring token;
    while (std::getline(stream, token, L'+')) {
        const auto a = token.find_first_not_of(L" \t"), b = token.find_last_not_of(L" \t");
        if (a == std::wstring::npos)
            return false;
        token = Upper(token.substr(a, b - a + 1));
        if (token == L"CONTROL")
            token = L"CTRL";
        if (token == L"WINDOWS")
            token = L"WIN";
        if (token == L"ESCAPE")
            token = L"ESC";
        UINT key = 0;
        if (token == L"WHEEL") {
            if (chord.wheel)
                return false;
            chord.wheel = true;
            continue;
        }
        if (auto it = names.find(token); it != names.end())
            key = it->second;
        if (!key)
            return false;
        const auto modifier = Modifier(key);
        if (modifier) {
            if (chord.modifiers & modifier)
                return false;
            chord.modifiers |= modifier;
        } else {
            if (chord.key)
                return false;
            chord.key = key;
        }
    }
    if (kind == Kind::Wheel)
        return chord.wheel && !chord.key;
    if (chord.wheel)
        return false;
    return kind == Kind::Hold ? (chord.key || chord.modifiers) : chord.key != 0;
}
inline UINT CurrentModifiers() {
    UINT m = 0;
    if (GetKeyState(VK_CONTROL) < 0)
        m |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0)
        m |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0)
        m |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0)
        m |= MOD_WIN;
    return m;
}
inline bool Matches(const Chord &c, UINT key, Kind kind = Kind::Key) {
    if (c.modifiers != CurrentModifiers())
        return false;
    return kind == Kind::Wheel ||
           (kind == Kind::Hold ? (!c.key || GetKeyState(int(c.key)) < 0) : c.key == NormalizeKey(key));
}
inline bool Matches(const std::wstring &text, UINT key, Kind kind = Kind::Key) {
    Chord c;
    return Parse(text, kind, c) && Matches(c, key, kind);
}
inline std::wstring Format(const std::vector<UINT> &keys, bool wheel = false) {
    std::wstring result;
    auto add = [&](UINT k) {
        auto name = KeyName(k);
        if (!name.empty()) {
            if (!result.empty())
                result += L"+";
            result += name;
        }
    };
    for (UINT k : {UINT(VK_CONTROL), UINT(VK_MENU), UINT(VK_SHIFT), UINT(VK_LWIN)})
        if (std::find(keys.begin(), keys.end(), k) != keys.end())
            add(k);
    for (UINT k : keys)
        if (!Modifier(k))
            add(k);
    if (wheel) {
        if (!result.empty())
            result += L"+";
        result += L"Wheel";
    }
    return result;
}
class Recorder {
  public:
    void Reset() {
        m_down.clear();
        m_seen.clear();
    }
    void Down(UINT k) {
        if (std::find(m_down.begin(), m_down.end(), k) == m_down.end())
            m_down.push_back(k);
        k = NormalizeKey(k);
        if (std::find(m_seen.begin(), m_seen.end(), k) == m_seen.end())
            m_seen.push_back(k);
    }
    bool Up(UINT k) {
        std::erase(m_down, k);
        return !m_seen.empty() && m_down.empty();
    }
    std::wstring Text(bool wheel = false) const {
        return Format(m_seen, wheel);
    }

  private:
    std::vector<UINT> m_down, m_seen;
};
inline std::unordered_map<std::wstring, std::wstring> pluginBindings;
inline void Configure(const NskryInputConfigV1 *config) {
    if (!config || config->structSize < sizeof(*config) || config->version != 1 || config->count > 64 ||
        (config->count && !config->bindings))
        return;
    pluginBindings.clear();
    for (uint32_t i = 0; i < config->count; ++i)
        if (config->bindings[i].id && config->bindings[i].shortcut)
            pluginBindings.emplace(config->bindings[i].id, config->bindings[i].shortcut);
}
inline bool PluginMatches(const wchar_t *id, const wchar_t *fallback, UINT key, Kind kind = Kind::Key) {
    if (!id || !fallback)
        return false;
    struct CompiledBinding {
        std::wstring text;
        Chord chord;
        Kind kind = Kind::Key;
        bool valid{};
    };
    // UI-thread helper: do not parse/allocate a token stream on every wheel event.
    // Compare values as well as Configure calls so explicit empty values stay disabled.
    static std::unordered_map<std::wstring, CompiledBinding> compiled;
    auto it = pluginBindings.find(id);
    const std::wstring_view text = it == pluginBindings.end() ? std::wstring_view(fallback) : it->second;
    auto [slot, created] = compiled.try_emplace(id);
    auto &binding = slot->second;
    if (created || binding.text != text || binding.kind != kind) {
        binding.text = text;
        binding.kind = kind;
        binding.valid = Parse(binding.text, kind, binding.chord);
    }
    return binding.valid && Matches(binding.chord, key, kind);
}
} // namespace nskry::input
