#pragma once

#include <functional>
#include <string>
#include <unordered_map>

namespace nskry {

/// Maps stable command identifiers to actions.  Input sources such as tray
/// menus and global hotkeys depend on command IDs instead of main.cpp globals.
class CommandRegistry {
public:
    static CommandRegistry& Instance();

    void Register(const std::wstring& commandId, std::function<void()> action);
    bool Execute(const std::wstring& commandId) const;
    void Clear();

private:
    std::unordered_map<std::wstring, std::function<void()>> m_commands;
};

} // namespace nskry
