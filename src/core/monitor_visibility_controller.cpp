#include "core/monitor_visibility_controller.h"
#include <unordered_map>
#include <mutex>

namespace nskry {
namespace {
constexpr UINT RefreshMessage = WM_APP + 1;
constexpr UINT_PTR ReleaseTimer = 1, FallbackTimer = 2;
std::unordered_map<HWINEVENTHOOK, MonitorVisibilityController*> hooks; // All use UI thread.
std::once_flag windowClass;
}
MonitorVisibilityController::MonitorVisibilityController(HWND source, HWND pip, std::function<bool()> protectedInteraction,
                                                         Update update, std::function<void()> sourceClosed, std::function<HWND()> foregroundQuery)
    : m_source(source), m_pip(pip), m_protected(std::move(protectedInteraction)),
      m_update(std::move(update)), m_closed(std::move(sourceClosed)), m_foregroundQuery(std::move(foregroundQuery)) {
    std::call_once(windowClass, [] {
        WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"NskryMonitorVisibility"; RegisterClassW(&wc);
    });
    m_hwnd = CreateWindowExW(0, L"NskryMonitorVisibility", L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                            nullptr, GetModuleHandleW(nullptr), this);
}
MonitorVisibilityController::~MonitorVisibilityController() {
    m_shuttingDown = true;
    for (auto hook : {m_foregroundHook, m_destroyHook}) if (hook) { hooks.erase(hook); UnhookWinEvent(hook); }
    if (m_hwnd) DestroyWindow(m_hwnd);
}
void MonitorVisibilityController::SetGlobal(bool enabled) { m_global = enabled; RegisterEvents(); Refresh(); }
void MonitorVisibilityController::SetOverride(MonitorHideOverride choice) { m_override = choice; RegisterEvents(); Refresh(); }
void MonitorVisibilityController::RegisterEvents() {
    const bool enabled = MonitorHideEnabled(m_global, m_override);
    if (!enabled) {
        for (auto& hook : {&m_foregroundHook, &m_destroyHook}) if (*hook) {
            hooks.erase(*hook); UnhookWinEvent(*hook); *hook = nullptr;
        }
        KillTimer(m_hwnd, FallbackTimer); return;
    }
    if (!m_foregroundHook) {
        m_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                          nullptr, EventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (m_foregroundHook) hooks[m_foregroundHook] = this;
    }
    if (!m_destroyHook) {
        DWORD process{}; GetWindowThreadProcessId(m_source, &process);
        m_destroyHook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr, EventProc,
                                       process, 0, WINEVENT_OUTOFCONTEXT);
        if (m_destroyHook) hooks[m_destroyHook] = this;
    }
    if (!m_foregroundHook) SetTimer(m_hwnd, FallbackTimer, 250, nullptr);
    else KillTimer(m_hwnd, FallbackTimer);
}
void CALLBACK MonitorVisibilityController::EventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                                     LONG object, LONG child, DWORD, DWORD) {
    const auto found = hooks.find(hook); if (found == hooks.end()) return;
    auto self = found->second;
    if (self->m_shuttingDown) return;
    if (event == EVENT_OBJECT_DESTROY) {
        if (hwnd != self->m_source || object != OBJID_WINDOW || child != CHILDID_SELF) return;
        self->m_sourceGone = true;
    }
    self->Refresh();
}
void MonitorVisibilityController::Refresh() {
    if (m_hwnd && !m_pending && !m_shuttingDown) {
        m_pending = PostMessageW(m_hwnd, RefreshMessage, 0, 0) != FALSE;
    }
}
void MonitorVisibilityController::Evaluate() {
    if (m_sourceGone || !IsWindow(m_source)) {
        KillTimer(m_hwnd, ReleaseTimer);
        KillTimer(m_hwnd, FallbackTimer);
        if (!m_sourceGone) m_sourceGone = true;
        if (!m_closeReported) { m_closeReported = true; if (m_closed) m_closed(); }
        return;
    }
    const HWND foreground = m_foregroundQuery ? m_foregroundQuery() : GetForegroundWindow();
    const HWND sourceRoot = GetAncestor(m_source, GA_ROOT);
    const bool focused = foreground && (GetAncestor(foreground, GA_ROOT) == sourceRoot ||
                                        GetAncestor(foreground, GA_ROOTOWNER) == sourceRoot);
    const auto state = DecideMonitorVisibility(MonitorHideEnabled(m_global, m_override), focused,
                                               IsIconic(m_pip) != FALSE, m_protected && m_protected());
    if (state.sourceHidden != m_hidden || state.processingPaused != m_paused) {
        m_hidden = state.sourceHidden; m_paused = state.processingPaused;
        if (!m_paused) m_released = false;
        if (m_update) m_update(m_hidden, m_paused, false);
        KillTimer(m_hwnd, ReleaseTimer);
        // Hard suspension is only safe if target destruction remains tracked.
        if (m_hidden && !m_released && m_destroyHook) SetTimer(m_hwnd, ReleaseTimer, 1000, nullptr);
    }
}
LRESULT CALLBACK MonitorVisibilityController::WndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto self = reinterpret_cast<MonitorVisibilityController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && message == RefreshMessage) { self->m_pending = false; self->Evaluate(); return 0; }
    if (self && message == WM_TIMER) {
        if (wp == FallbackTimer) self->Refresh();
        if (wp == ReleaseTimer) {
            KillTimer(hwnd, ReleaseTimer); self->Evaluate();
            if (self->m_hidden && self->m_paused && self->m_destroyHook && !self->m_released && !self->m_sourceGone) {
                self->m_released = true; if (self->m_update) self->m_update(true, true, true);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}
