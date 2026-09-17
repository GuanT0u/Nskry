#pragma once

#include <string>
#include <unordered_map>

namespace nskry {

/// Owns Win32 global-hotkey registrations and maps their transient numeric IDs
/// back to stable command IDs.
class HotkeyManager {
public:
    static HotkeyManager& Instance();

    void Initialize(HWND messageWindow);
    void Shutdown();

    bool Register(const std::wstring& commandId, const std::wstring& shortcut);
    bool Rebind(const std::wstring& commandId, const std::wstring& shortcut);
    bool Unregister(const std::wstring& commandId);
    std::wstring CommandForHotkeyId(int hotkeyId) const;

    static bool ParseShortcut(const std::wstring& shortcut, UINT& modifiers, UINT& virtualKey);

private:
    struct Registration {
        int hotkeyId = 0;
        std::wstring commandId;
    };

    HWND m_messageWindow = nullptr;
    int m_nextHotkeyId = 1;
    std::unordered_map<int, Registration> m_registrations;
};

} // namespace nskry
