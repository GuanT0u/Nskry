#include "pch.h"
#include "core/hotkey_manager.h"

#include <cwctype>
#include <sstream>

namespace nskry {
namespace {

std::wstring Trim(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring Upper(std::wstring value) {
    for (auto& ch : value) ch = static_cast<wchar_t>(std::towupper(ch));
    return value;
}

bool ParseVirtualKey(const std::wstring& token, UINT& virtualKey) {
    if (token.size() == 1) {
        const wchar_t key = token[0];
        if ((key >= L'A' && key <= L'Z') || (key >= L'0' && key <= L'9')) {
            virtualKey = static_cast<UINT>(key);
            return true;
        }
    }

    if (token.size() >= 2 && token[0] == L'F') {
        try {
            const int number = std::stoi(token.substr(1));
            if (number >= 1 && number <= 24) {
                virtualKey = VK_F1 + number - 1;
                return true;
            }
        } catch (const std::exception&) {
        }
    }

    if (token == L"ESC" || token == L"ESCAPE") { virtualKey = VK_ESCAPE; return true; }
    if (token == L"SPACE") { virtualKey = VK_SPACE; return true; }
    if (token == L"ENTER") { virtualKey = VK_RETURN; return true; }
    if (token == L"TAB") { virtualKey = VK_TAB; return true; }
    return false;
}

} // namespace

HotkeyManager& HotkeyManager::Instance() {
    static HotkeyManager s_instance;
    return s_instance;
}

void HotkeyManager::Initialize(HWND messageWindow) {
    Shutdown();
    m_messageWindow = messageWindow;
}

void HotkeyManager::Shutdown() {
    if (m_messageWindow) {
        for (const auto& [id, registration] : m_registrations) {
            ::UnregisterHotKey(m_messageWindow, id);
        }
    }
    m_registrations.clear();
    m_nextHotkeyId = 1;
    m_messageWindow = nullptr;
}

bool HotkeyManager::Register(const std::wstring& commandId, const std::wstring& shortcut) {
    if (!m_messageWindow || commandId.empty()) return false;
    for (const auto& [id, registration] : m_registrations) {
        if (registration.commandId == commandId) return false;
    }

    UINT modifiers = 0;
    UINT virtualKey = 0;
    if (!ParseShortcut(shortcut, modifiers, virtualKey)) return false;

    const int hotkeyId = m_nextHotkeyId++;
    if (!::RegisterHotKey(m_messageWindow, hotkeyId, modifiers, virtualKey)) return false;

    m_registrations.emplace(hotkeyId, Registration{ hotkeyId, commandId });
    return true;
}

bool HotkeyManager::Rebind(const std::wstring& commandId, const std::wstring& shortcut) {
    if (!m_messageWindow || commandId.empty()) return false;

    UINT modifiers = 0;
    UINT virtualKey = 0;
    if (!ParseShortcut(shortcut, modifiers, virtualKey)) return false;

    const int newHotkeyId = m_nextHotkeyId++;
    if (!::RegisterHotKey(m_messageWindow, newHotkeyId, modifiers, virtualKey)) return false;

    Unregister(commandId);
    m_registrations.emplace(newHotkeyId, Registration{ newHotkeyId, commandId });
    return true;
}

bool HotkeyManager::Unregister(const std::wstring& commandId) {
    for (auto it = m_registrations.begin(); it != m_registrations.end(); ++it) {
        if (it->second.commandId != commandId) continue;
        ::UnregisterHotKey(m_messageWindow, it->first);
        m_registrations.erase(it);
        return true;
    }
    return false;
}

std::wstring HotkeyManager::CommandForHotkeyId(int hotkeyId) const {
    const auto it = m_registrations.find(hotkeyId);
    return it == m_registrations.end() ? std::wstring{} : it->second.commandId;
}

bool HotkeyManager::ParseShortcut(const std::wstring& shortcut, UINT& modifiers, UINT& virtualKey) {
    modifiers = 0;
    virtualKey = 0;

    std::wstringstream stream(shortcut);
    std::wstring part;
    bool hasKey = false;
    while (std::getline(stream, part, L'+')) {
        const std::wstring token = Upper(Trim(part));
        if (token.empty()) return false;
        if (token == L"CTRL" || token == L"CONTROL") {
            modifiers |= MOD_CONTROL;
        } else if (token == L"ALT") {
            modifiers |= MOD_ALT;
        } else if (token == L"SHIFT") {
            modifiers |= MOD_SHIFT;
        } else if (token == L"WIN" || token == L"WINDOWS") {
            modifiers |= MOD_WIN;
        } else if (!hasKey && ParseVirtualKey(token, virtualKey)) {
            hasKey = true;
        } else {
            return false;
        }
    }
    return hasKey;
}

} // namespace nskry
