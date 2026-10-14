#include "core/update_task.h"
#include <mutex>
namespace nskry {
UpdateTask::~UpdateTask() {
    Shutdown();
}
bool UpdateTask::Start(Work work, Completion completion) {
    if (Busy() || !work)
        return false;
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSW cls{};
        cls.lpfnWndProc = Proc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"NskryUpdateCompletion";
        RegisterClassW(&cls);
    });
    if (!m_window)
        m_window = CreateWindowExW(0, L"NskryUpdateCompletion", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleW(nullptr), this);
    if (!m_window)
        return false;
    m_stop = std::stop_source{};
    m_completion = std::move(completion);
    const auto generation = ++m_generation;
    try {
        m_thread = std::thread([this, work = std::move(work), token = m_stop.get_token(), generation]() mutable {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            try {
                m_result = work(token);
            } catch (...) {
                m_result = {};
                m_result.message = L"Update task failed unexpectedly.";
            }
            m_result.cancelled = token.stop_requested() || m_result.cancelled;
            if (m_result.cancelled) {
                m_result = {};
                m_result.cancelled = true;
            }
            work = {};
            if (!PostMessageW(m_window, WM_APP + 1, generation, 0))
                SendNotifyMessageW(m_window, WM_APP + 1, generation, 0);
        });
    } catch (...) {
        m_completion = {};
        m_stop = std::stop_source{std::nostopstate};
        DestroyWindow(m_window);
        m_window = nullptr;
        return false;
    }
    return true;
}
void UpdateTask::Cancel() {
    m_completion = {};
    m_stop.request_stop();
}
void UpdateTask::Shutdown() {
    Cancel();
    if (m_thread.joinable())
        m_thread.join();
    m_stop = std::stop_source{std::nostopstate};
    m_result = {};
    if (m_window) {
        DestroyWindow(m_window);
        m_window = nullptr;
    }
}
void UpdateTask::Complete() {
    if (!m_thread.joinable())
        return;
    m_thread.join();
    m_stop = std::stop_source{std::nostopstate};
    auto callback = std::move(m_completion);
    auto result = std::move(m_result);
    m_result = {};
    if (m_window) {
        DestroyWindow(m_window);
        m_window = nullptr;
    }
    if (!callback && result.download) {
        // Cancellation may arrive after publication. Dispose large staged trees
        // on the same short-lived task facility, never through a dead page.
        Start(
            [discard = std::move(result)](std::stop_token) mutable {
                discard = {};
                return UpdateTaskResult{};
            },
            {});
        return;
    }
    if (callback && !result.cancelled)
        callback(std::move(result));
}
LRESULT CALLBACK UpdateTask::Proc(HWND w, UINT m, WPARAM p, LPARAM l) {
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(w, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
    auto self = reinterpret_cast<UpdateTask *>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (self && m == WM_APP + 1) {
        if (p == self->m_generation)
            self->Complete();
        return 0;
    }
    return DefWindowProcW(w, m, p, l);
}
} // namespace nskry
