#pragma once
#include <algorithm>
#include <windows.h>
#include <winrt/Windows.Foundation.h>

namespace nskry::translate {
// Worker thread only. Repeated IAsyncOperation::wait_for installs another
// Completed delegate and fails when an OCR request outlasts the first timeout.
// Status queries do not register callbacks that could outlive this DLL.
template <class Operation> void WaitForOcr(const Operation &operation, HANDLE cancel, DWORD timeoutMs) {
    const auto started = GetTickCount64();
    for (;;) {
        if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) {
            operation.Cancel();
            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }
        if (operation.Status() != winrt::Windows::Foundation::AsyncStatus::Started)
            return;
        const auto elapsed = GetTickCount64() - started;
        if (elapsed >= timeoutMs) {
            operation.Cancel();
            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        }
        const auto wait = WaitForSingleObject(cancel, (std::min)(DWORD(25), DWORD(timeoutMs - elapsed)));
        if (wait == WAIT_FAILED) {
            const auto error = GetLastError();
            operation.Cancel();
            winrt::throw_hresult(HRESULT_FROM_WIN32(error));
        }
    }
}
} // namespace nskry::translate
