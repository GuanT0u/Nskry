#pragma once
#include <windows.h>

namespace nskry {
// Prepare a replacement before asking the current instance to shut down.
// Start it only after the caller has released startup resources and its guard.
class AppRestart {
public:
    AppRestart() = default;
    ~AppRestart() {
        if (Pending()) {
            ::TerminateProcess(m_child.hProcess, ERROR_CANCELLED);
            CloseHandles();
        }
    }
    AppRestart(const AppRestart&) = delete;
    AppRestart& operator=(const AppRestart&) = delete;

    bool Pending() const { return m_child.hProcess != nullptr; }

    DWORD Prepare() {
        if (Pending()) return ERROR_SUCCESS;
        wchar_t executable[32768]{};
        const DWORD length = ::GetModuleFileNameW(nullptr, executable, 32768);
        if (!length) return ::GetLastError();
        if (length == 32768) return ERROR_INSUFFICIENT_BUFFER;

        STARTUPINFOW startup{sizeof(startup)};
        if (!::CreateProcessW(executable, nullptr, nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                              nullptr, nullptr, &startup, &m_child))
            return ::GetLastError();
        return ERROR_SUCCESS;
    }

    DWORD Start() {
        if (!Pending()) return ERROR_INVALID_HANDLE;
        if (::ResumeThread(m_child.hThread) == DWORD(-1)) return ::GetLastError();
        CloseHandles();
        return ERROR_SUCCESS;
    }

private:
    void CloseHandles() {
        ::CloseHandle(m_child.hThread);
        ::CloseHandle(m_child.hProcess);
        m_child = {};
    }

    PROCESS_INFORMATION m_child{};
};
} // namespace nskry
