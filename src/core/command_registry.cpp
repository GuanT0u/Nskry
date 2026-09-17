#include "pch.h"
#include "core/command_registry.h"

namespace nskry {

CommandRegistry& CommandRegistry::Instance() {
    static CommandRegistry s_instance;
    return s_instance;
}

void CommandRegistry::Register(const std::wstring& commandId, std::function<void()> action) {
    if (commandId.empty() || !action) return;
    m_commands[commandId] = std::move(action);
}

bool CommandRegistry::Execute(const std::wstring& commandId) const {
    const auto it = m_commands.find(commandId);
    if (it == m_commands.end()) return false;
    it->second();
    return true;
}

void CommandRegistry::Clear() {
    m_commands.clear();
}

} // namespace nskry
