#pragma once
#include <windows.h>
#include <functional>
#include "core/monitor_visibility_policy.h"

namespace nskry {
// UI-thread controller. Never injects code or polls while events are available.
class MonitorVisibilityController {
public:
    using Update = std::function<void(bool sourceHidden, bool paused, bool releaseCapture)>;
    MonitorVisibilityController(HWND source, HWND pip, std::function<bool()> protectedInteraction,
                                Update update, std::function<void()> sourceClosed,
                                std::function<HWND()> foregroundQuery = {});
    ~MonitorVisibilityController();
    MonitorVisibilityController(const MonitorVisibilityController&) = delete;
    MonitorVisibilityController& operator=(const MonitorVisibilityController&) = delete;
    void SetGlobal(bool enabled);
    void SetOverride(MonitorHideOverride choice);
    MonitorHideOverride Override() const { return m_override; }
    void Refresh();
    bool EventsAvailable() const { return m_foregroundHook && m_destroyHook; }
private:
    friend struct MonitorControllerTestAccess;
    static void CALLBACK EventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Evaluate();
    void RegisterEvents();
    HWND m_hwnd{}, m_source{}, m_pip{};
    HWINEVENTHOOK m_foregroundHook{}, m_destroyHook{};
    bool m_global{}, m_sourceGone{}, m_hidden{}, m_paused{}, m_released{}, m_pending{}, m_shuttingDown{};
    bool m_closeReported{};
    MonitorHideOverride m_override = MonitorHideOverride::FollowGlobal;
    std::function<bool()> m_protected;
    Update m_update;
    std::function<void()> m_closed;
    std::function<HWND()> m_foregroundQuery;
};
}
