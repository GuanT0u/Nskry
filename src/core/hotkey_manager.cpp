#include "core/hotkey_manager.h"
#include "pch.h"
#include "sdk/nskry_input.h"
#include <unordered_set>

namespace nskry {

HotkeyManager &HotkeyManager::Instance() {
    static HotkeyManager s_instance;
    return s_instance;
}

void HotkeyManager::Initialize(HWND messageWindow) {
    Shutdown();
    m_messageWindow = messageWindow;
}

void HotkeyManager::Shutdown() {
    if (m_messageWindow) {
        for (const auto &[id, registration] : m_registrations) {
            ::UnregisterHotKey(m_messageWindow, id);
        }
    }
    m_registrations.clear();
    m_bindings.clear();
    m_suspended = false;
    m_nextHotkeyId = 1;
    m_messageWindow = nullptr;
}

bool HotkeyManager::Register(const std::wstring &commandId, const std::wstring &shortcut) {
    if (!m_messageWindow || commandId.empty())
        return false;
    for (const auto &[id, registration] : m_registrations) {
        if (registration.commandId == commandId)
            return false;
    }

    UINT modifiers = 0;
    UINT virtualKey = 0;
    if (!ParseShortcut(shortcut, modifiers, virtualKey))
        return false;

    const int hotkeyId = m_nextHotkeyId++;
    if (!m_suspended && !::RegisterHotKey(m_messageWindow, hotkeyId, modifiers | MOD_NOREPEAT, virtualKey))
        return false;

    m_registrations.emplace(hotkeyId, Registration{hotkeyId, commandId});
    m_bindings[commandId] = shortcut;
    return true;
}

bool HotkeyManager::Rebind(const std::wstring &commandId, const std::wstring &shortcut) {
    if (!m_messageWindow || commandId.empty())
        return false;
    std::vector<std::pair<std::wstring, std::wstring>> next;
    for (const auto &binding : m_bindings)
        if (binding.first != commandId)
            next.push_back(binding);
    next.emplace_back(commandId, shortcut);
    std::wstring error;
    return ApplyBindings(next, error);
}

bool HotkeyManager::Unregister(const std::wstring &commandId) {
    for (auto it = m_registrations.begin(); it != m_registrations.end(); ++it) {
        if (it->second.commandId != commandId)
            continue;
        ::UnregisterHotKey(m_messageWindow, it->first);
        m_registrations.erase(it);
        m_bindings.erase(commandId);
        return true;
    }
    return false;
}

std::wstring HotkeyManager::CommandForHotkeyId(int hotkeyId) const {
    if (m_suspended)
        return {};
    const auto it = m_registrations.find(hotkeyId);
    return it == m_registrations.end() ? std::wstring{} : it->second.commandId;
}

bool HotkeyManager::ParseShortcut(const std::wstring &shortcut, UINT &modifiers, UINT &virtualKey) {
    input::Chord chord;
    const bool valid = input::Parse(shortcut, input::Kind::Global, chord);
    modifiers = chord.modifiers;
    virtualKey = chord.key;
    return valid;
}

bool HotkeyManager::ApplyBindings(const std::vector<std::pair<std::wstring, std::wstring>> &bindings,
                                  std::wstring &error) {
    error.clear();
    if (!m_messageWindow) {
        error = L"Hotkey service is not initialized.";
        return false;
    }
    std::unordered_map<UINT, std::wstring> used;
    std::unordered_set<std::wstring> commands;
    for (const auto &[id, text] : bindings) {
        if (id.empty() || !commands.insert(id).second) {
            error = L"Empty or duplicate shortcut command ID.";
            return false;
        }
        if (text.empty())
            continue;
        UINT mods{}, key{};
        if (!ParseShortcut(text, mods, key)) {
            error = L"Invalid global shortcut: " + text;
            return false;
        }
        const UINT code = (mods << 16) | key;
        if (used.contains(code)) {
            error = L"Duplicate global shortcut: " + text;
            return false;
        }
        used[code] = id;
    }
    const auto old = m_bindings;
    for (const auto &[number, registration] : m_registrations)
        ::UnregisterHotKey(m_messageWindow, number);
    m_registrations.clear();
    m_bindings.clear();
    for (const auto &[id, text] : bindings)
        if (!text.empty() && !Register(id, text)) {
            error = L"Shortcut is unavailable: " + text;
            for (const auto &[number, registration] : m_registrations)
                ::UnregisterHotKey(m_messageWindow, number);
            m_registrations.clear();
            m_bindings.clear();
            for (const auto &[oldId, oldText] : old)
                if (!Register(oldId, oldText))
                    error += L"; failed to restore " + oldText;
            return false;
        }
    return true;
}
bool HotkeyManager::Suspend(bool suspend) {
    if (suspend == m_suspended)
        return true;
    m_suspended = suspend;
    bool ok = true;
    for (const auto &[number, registration] : m_registrations) {
        if (suspend)
            ::UnregisterHotKey(m_messageWindow, number);
        else {
            UINT mods{}, key{};
            ParseShortcut(m_bindings[registration.commandId], mods, key);
            ok = ::RegisterHotKey(m_messageWindow, number, mods | MOD_NOREPEAT, key) != FALSE && ok;
        }
    }
    return ok;
}

} // namespace nskry
