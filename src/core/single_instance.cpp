#include "single_instance.h"
#include <sddl.h>
#include <string>

namespace nskry {
SingleInstanceGuard::SingleInstanceGuard(const wchar_t* applicationId) noexcept {
    if (!applicationId || !*applicationId) {
        m_error = ERROR_INVALID_PARAMETER;
        return;
    }
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        m_error = GetLastError();
        return;
    }
    alignas(TOKEN_USER) BYTE tokenUser[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
    DWORD size{};
    const BOOL tokenRead = GetTokenInformation(token, TokenUser, tokenUser, sizeof(tokenUser), &size);
    const DWORD tokenError = tokenRead ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    if (!tokenRead) {
        m_error = tokenError;
        return;
    }
    LPWSTR sid{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(tokenUser)->User.Sid, &sid)) {
        m_error = GetLastError();
        return;
    }
    try {
        const std::wstring name = std::wstring(L"Local\\") + applicationId + L"." + sid;
        // Opening an existing instance only needs SYNCHRONIZE, including when
        // the first instance has a different integrity level for the same user.
        m_handle = CreateMutexExW(nullptr, name.c_str(), 0, SYNCHRONIZE);
        const DWORD result = GetLastError();
        if (!m_handle) {
            m_error = result;
        } else if (result == ERROR_ALREADY_EXISTS) {
            CloseHandle(m_handle);
            m_handle = nullptr;
            m_result = Result::AlreadyRunning;
        } else {
            m_result = Result::Primary;
        }
    } catch (...) {
        m_error = ERROR_OUTOFMEMORY;
    }
    LocalFree(sid);
}

SingleInstanceGuard::~SingleInstanceGuard() {
    if (m_handle)
        CloseHandle(m_handle);
}
} // namespace nskry
