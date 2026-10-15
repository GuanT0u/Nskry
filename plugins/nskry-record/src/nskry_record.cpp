#include "nskry_commands.h"
#include "nskry_plugin.h"
#include "record_session.h"
#include <commdlg.h>
#include <memory>
#include <string>
#include <windowsx.h>

namespace {
using namespace nskry::record;
NskryCaptureServiceV1 service{};
std::unique_ptr<RecordSession> session;
HWND window{}, statusLabel{}, pauseButton{}, stopButton{}, discardButton{};
HINSTANCE module{};
bool closing = false;
bool selectingOutput = false;
bool closePromptActive = false;
NskryPluginInfo info{sizeof(info),
                     NSKRY_PLUGIN_API_VERSION,
                     L"Screen recording",
                     L"nskry-record",
                     L"0.1.1",
                     L"Nskry Team",
                     L"On-demand fixed screen-region MP4/H.264 recording. No audio.",
                     nullptr,
                     NSKRY_CAP_TOOLBAR_ACTION,
                     {}};
constexpr UINT Poll = 1;
constexpr wchar_t ClassName[] = L"NskryRecordControlV1";

void Update() {
    if (!session)
        return;
    const auto s = session->Status();
    const bool finished = s.state == State::Done || s.state == State::Failed;
    const wchar_t *state = s.state == State::Starting     ? L"Starting"
                           : s.state == State::Recording  ? L"Recording"
                           : s.state == State::Paused     ? L"Paused"
                           : s.state == State::Stopping   ? L"Finalizing"
                           : s.state == State::Discarding ? L"Discarding"
                           : s.state == State::Publishing ? L"Publishing (cannot discard)"
                           : s.state == State::Recovering ? L"Recovering captured portion"
                                                          : L"Finished";
    const auto seconds = s.duration / 10000000;
    const auto text = std::wstring(state) + L"  " + std::to_wstring(seconds / 60) + L":" +
                      (seconds % 60 < 10 ? L"0" : L"") + std::to_wstring(seconds % 60) + L"\n" +
                      (finished ? s.message : s.backend);
    SetWindowTextW(statusLabel, text.c_str());
    SetWindowTextW(pauseButton, s.state == State::Paused ? L"Resume" : L"Pause");
    EnableWindow(pauseButton, s.state == State::Recording || s.state == State::Paused);
    EnableWindow(stopButton, s.state == State::Starting || s.state == State::Recording || s.state == State::Paused);
    SetWindowTextW(discardButton, finished ? L"Close" : L"Cancel / discard");
    EnableWindow(discardButton, s.state != State::Publishing);
    if (finished) {
        // MessageBox pumps timers/hotkeys. Keep the session alive until its
        // caller returns; otherwise a concurrent Stop can invalidate WM_CLOSE.
        if (closePromptActive)
            return;
        KillTimer(window, Poll);
        session.reset(); // Done published after resource release; join finished thread, no idle thread.
        if (s.state == State::Failed) {
            closing = false;
            MessageBoxW(window, s.message.c_str(), L"Recording failed", MB_OK | MB_ICONWARNING);
        }
        if (closing)
            DestroyWindow(window);
    }
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        Update();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 1:
            if (session)
                session->PauseResume();
            break;
        case 2:
            if (session)
                session->Stop(false);
            break;
        case 3:
            if (session) {
                closing = session->Stop(true);
            } else
                DestroyWindow(hwnd);
            break;
        }
        Update();
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND && session)
            session->Stop(false);
        return TRUE;
    case WM_DISPLAYCHANGE:
        if (session)
            session->Stop(false);
        return 0;
    case WM_CLOSE:
        if (closePromptActive)
            return 0;
        if (session) {
            if (session->Status().state == State::Publishing) {
                closing = true;
                return 0;
            }
            closePromptActive = true;
            const int choice =
                MessageBoxW(hwnd, L"Save this recording before closing?\nYes: stop and save. No: discard.",
                            L"Screen recording", MB_YESNOCANCEL | MB_ICONQUESTION);
            closePromptActive = false;
            if (choice == IDCANCEL) {
                Update();
                return 0;
            }
            // A global Stop may already have committed while the prompt was open.
            // Do not promise a discard or dereference a timer-deleted session.
            closing = session->Stop(choice == IDNO) || choice == IDYES;
            Update();
        } else
            DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, Poll);
        window = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
bool ShowControl(RECT region) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Procedure;
    wc.hInstance = module;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = HBRUSH(COLOR_WINDOW + 1);
    wc.lpszClassName = ClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    auto monitor = MonitorFromRect(&region, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    window =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, ClassName, L"Nskry — Screen recording (fixed display region)",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, mi.rcWork.right - 560, mi.rcWork.top + 12, 540, 160,
                        nullptr, nullptr, module, nullptr);
    if (!window)
        return false;
    // Refuse recording if the OS cannot exclude this control window. No black UI box in output.
    if (!SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) {
        DestroyWindow(window);
        return false;
    }
    statusLabel = CreateWindowExW(0, L"STATIC", L"Starting...", WS_CHILD | WS_VISIBLE, 12, 10, 510, 60, window, nullptr,
                                  module, nullptr);
    pauseButton = CreateWindowExW(0, L"BUTTON", L"Pause", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 12, 78, 100, 28, window,
                                  HMENU(1), module, nullptr);
    stopButton = CreateWindowExW(0, L"BUTTON", L"Stop / save", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 124, 78, 115, 28,
                                 window, HMENU(2), module, nullptr);
    discardButton = CreateWindowExW(0, L"BUTTON", L"Cancel / discard", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 252, 78, 140,
                                    28, window, HMENU(3), module, nullptr);
    if (!statusLabel || !pauseButton || !stopButton || !discardButton) {
        DestroyWindow(window);
        return false;
    }
    closing = false;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    if (!SetTimer(window, Poll, 250, nullptr)) {
        DestroyWindow(window);
        return false;
    }
    return true;
}
void Error(const wchar_t *text) {
    MessageBoxW(window, text, L"Screen recording", MB_OK | MB_ICONWARNING);
}
} // namespace
extern "C" {
NSKRY_API const NskryPluginInfo *NSKRY_CALL nskry_plugin_info() {
    return &info;
}
NSKRY_API int32_t NSKRY_CALL nskry_plugin_connect_capture_v1(const NskryCaptureServiceV1 *value) {
    if (!value || value->structSize < sizeof(*value) || value->version != 1 || !value->open || !value->read ||
        !value->close)
        return 0;
    service = *value;
    return 1;
}
NSKRY_API int32_t NSKRY_CALL nskry_plugin_init(const NskryHostContext *ctx) {
    return ctx && ctx->structSize >= sizeof(*ctx) && ctx->apiVersion == 2 && service.version == 1;
}
NSKRY_API void NSKRY_CALL nskry_plugin_execute(const NskryHostContext *ctx) {
    try {
        if (session || selectingOutput) {
            if (window)
                SetForegroundWindow(window);
            return;
        }
        if (!ctx || service.version != 1) {
            Error(L"This host does not provide region capture v1.");
            return;
        }
        selectingOutput = true;
        struct SelectingGuard {
            ~SelectingGuard() {
                selectingOutput = false;
            }
        } selectingGuard;
        MONITORINFO mi{sizeof(mi)};
        const auto monitor = MonitorFromRect(&ctx->capturedRegion, MONITOR_DEFAULTTONULL);
        const auto r = ctx->capturedRegion;
        if (!monitor || !GetMonitorInfoW(monitor, &mi) || r.left < mi.rcMonitor.left || r.top < mi.rcMonitor.top ||
            r.right > mi.rcMonitor.right || r.bottom > mi.rcMonitor.bottom || r.right - r.left < 2 ||
            r.bottom - r.top < 2) {
            Error(L"Select a region inside one monitor. Cross-monitor recording is not supported.");
            return;
        }
        wchar_t path[32768] = L"Recording.mp4";
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = ctx->mainHwnd;
        dialog.lpstrFilter = L"MP4 video\0*.mp4\0\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = 32768;
        dialog.lpstrDefExt = L"mp4";
        dialog.lpstrTitle = L"Record fixed screen area (no audio; does not follow windows)";
        dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog))
            return;
        const auto length = wcslen(path);
        if (length < 4 || _wcsicmp(path + length - 4, L".mp4")) {
            Error(L"Choose a file name ending in .mp4.");
            return;
        }
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            Error(L"Choose a new file name. Recording never overwrites an existing file.");
            return;
        }
        if (window)
            DestroyWindow(window);
        if (!ShowControl(r)) {
            Error(L"Cannot create a capture-excluded recording control window.");
            return;
        }
        session = std::make_unique<RecordSession>(service, r, path);
        session->Start();
        Update();
    } catch (...) {
        Error(L"Could not start recording.");
        if (window && !session)
            DestroyWindow(window);
    }
}
NSKRY_API int32_t NSKRY_CALL nskry_plugin_command_v1(const NskryCommandV1 *request, NskryCommandResultV1 *result) {
    if (!request || request->structSize < sizeof(*request) || request->version != 1 || !request->id || !result ||
        result->structSize < sizeof(*result) || result->version != 1)
        return NSKRY_COMMAND_UNSUPPORTED;
    if (wcscmp(request->id, L"stop"))
        return NSKRY_COMMAND_UNSUPPORTED;
    if (!session || !session->Stop(false)) {
        if (result->error && result->capacity)
            wcsncpy_s(result->error, result->capacity, L"No recording is ready to stop.", _TRUNCATE);
        return NSKRY_COMMAND_FAILED;
    }
    Update();
    return NSKRY_COMMAND_OK;
}
NSKRY_API void NSKRY_CALL nskry_plugin_shutdown() {
    if (window)
        KillTimer(window, Poll);
    if (session) {
        session->Shutdown();
        session.reset();
    }
    if (window)
        DestroyWindow(window);
    UnregisterClassW(ClassName, module);
    service = {};
}
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

#ifdef NSKRY_RECORD_CONTROL_TEST
// Private static-link regression seam. Not compiled/exported by the shipped DLL.
HWND RecordControlTestStart(NskryCaptureServiceV1 capture, RECT region, const std::wstring &path) {
    module = GetModuleHandleW(nullptr);
    service = capture;
    if (!ShowControl(region))
        return nullptr;
    session = std::make_unique<RecordSession>(service, region, path);
    session->Start();
    return window;
}
bool RecordControlTestAlive() {
    return bool(session);
}
Snapshot RecordControlTestStatus() {
    return session ? session->Status() : Snapshot{};
}
void RecordControlTestHoldPrompt(bool value) {
    closePromptActive = value;
}
void RecordControlTestUpdate() {
    Update();
}
#endif
