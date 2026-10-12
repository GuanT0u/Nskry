#pragma once
#include <windows.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace nskry {

/// Owns Win32 global-hotkey registrations and maps their transient numeric IDs
/// back to stable command IDs.
class HotkeyManager {
  public:
    static HotkeyManager &Instance();

    void Initialize(HWND messageWindow);
    void Shutdown();

    bool Register(const std::wstring &commandId, const std::wstring &shortcut);
    bool Rebind(const std::wstring &commandId, const std::wstring &shortcut);
    bool Unregister(const std::wstring &commandId);
    std::wstring CommandForHotkeyId(int hotkeyId) const;
    bool ApplyBindings(const std::vector<std::pair<std::wstring, std::wstring>> &, std::wstring &error);
    bool Suspend(bool suspend);

    static bool ParseShortcut(const std::wstring &shortcut, UINT &modifiers, UINT &virtualKey);

  private:
    struct Registration {
        int hotkeyId = 0;
        std::wstring commandId;
    };

    HWND m_messageWindow = nullptr;
    int m_nextHotkeyId = 1;
    std::unordered_map<int, Registration> m_registrations;
    std::unordered_map<std::wstring, std::wstring> m_bindings;
    bool m_suspended = false;
};

} // namespace nskry
