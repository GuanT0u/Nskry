#pragma once
#include <windows.h>

namespace nskry {
// One UI instance per Windows user and interactive session. The kernel object
// is retained by a handle, not an owned mutex, so process exit needs no recovery.
class SingleInstanceGuard {
  public:
    enum class Result { Primary, AlreadyRunning, Failed };

    explicit SingleInstanceGuard(const wchar_t* applicationId = L"Nskry.Application") noexcept;
    ~SingleInstanceGuard();
    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    Result Status() const { return m_result; }
    DWORD Error() const { return m_error; }

  private:
    HANDLE m_handle{};
    Result m_result = Result::Failed;
    DWORD m_error = ERROR_SUCCESS;
};
} // namespace nskry
