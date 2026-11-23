#include "../../common/native_ui.h"
#include "nskry_actions.h"
#include "nskry_commands.h"
#include "nskry_plugin.h"
#include "record_delivery.h"
#include "record_export.h"
#include "record_preview.h"
#include "record_session.h"
#include "record_ui_policy.h"
#include <cmath>
#include <commdlg.h>
#include <filesystem>
#include <memory>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <vector>
#include <string>
#include <windowsx.h>
#include <winrt/base.h>

namespace {
using namespace nskry::record;
NskryCaptureServiceV1 service{};
NskryCaptureServiceV2 serviceV2{};
std::unique_ptr<RecordSession> session;
HWND window{}, statusLabel{}, pauseButton{}, stopButton{}, discardButton{};
HWND cleanupWindow{};
bool cleanupPending = false;
HWND fpsChoice{}, qualityChoice{}, followChoice{}, defaultsButton{}, trimButton{}, trimStart{}, trimEnd{}, trimApply{},
    trimCancel{};
HWND cacheButton{};
HWND areaWindow{};
HWND rangeWindow{};
std::unique_ptr<PreviewTask> previewTask;
PreviewFrame previewFrame;
int64_t rangeStart{}, rangeEnd{}, wantedPreview{};
bool rangeDragging = false, rangeTail = false, syncingRange = false;
bool previewFailed = false;
bool previewRetry = false;
bool pendingClose = false, pendingDiscard = false;
int pendingExport = -1, layoutMode = -1;
RECT layoutBounds{};
RECT areaBounds{}, setupRegion{};
HWND setupSource{}, audioChoice{}, audioLabel{};
struct AudioChoice {
    std::wstring label;
    AudioSource source;
};
std::vector<AudioChoice> audioSources;

void AddAudioApplication(DWORD pid, std::wstring label = {}) {
    if (!pid || pid == GetCurrentProcessId() || audioSources.size() >= 256 ||
        std::any_of(audioSources.begin(), audioSources.end(),
                    [pid](const AudioChoice &choice) { return choice.source.pid == pid; }))
        return;
    winrt::handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!process || !GetProcessTimes(process.get(), &created, &exited, &kernel, &user))
        return;
    if (label.empty()) {
        wchar_t executable[1024]{};
        DWORD length = ARRAYSIZE(executable);
        if (QueryFullProcessImageNameW(process.get(), 0, executable, &length))
            label = std::filesystem::path(executable).filename().wstring();
        else
            label = nskry::plugin_ui::Text(L"Audio session", L"音频会话");
    }
    ULARGE_INTEGER time{};
    time.LowPart = created.dwLowDateTime;
    time.HighPart = created.dwHighDateTime;
    audioSources.push_back({nskry::plugin_ui::Text(L"App: ", L"应用 · ") + label + L" (PID " + std::to_wstring(pid) + L")",
                            {{}, pid, time.QuadPart}});
}

void AddEndpointSessions(IMMDevice &device) {
    winrt::com_ptr<IAudioSessionManager2> manager;
    winrt::com_ptr<IAudioSessionEnumerator> sessions;
    int count{};
    if (FAILED(device.Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.put_void())) ||
        FAILED(manager->GetSessionEnumerator(sessions.put())) || FAILED(sessions->GetCount(&count)))
        return;
    for (int i = 0; i < count && audioSources.size() < 256; ++i) {
        winrt::com_ptr<IAudioSessionControl> control;
        AudioSessionState state{};
        if (FAILED(sessions->GetSession(i, control.put())) || FAILED(control->GetState(&state)) ||
            state != AudioSessionStateActive)
            continue;
        const auto session = control.try_as<IAudioSessionControl2>();
        DWORD pid{};
        if (session && session->IsSystemSoundsSession() == S_FALSE && session->GetProcessId(&pid) == S_OK)
            AddAudioApplication(pid);
    }
}

void PopulateAudioChoices() {
    audioSources = {{nskry::plugin_ui::Text(L"No audio", L"不录音频"), {}}};
    EnumWindows([](HWND candidate, LPARAM) -> BOOL {
        if (!IsWindowVisible(candidate) || GetWindow(candidate, GW_OWNER) || candidate == window)
            return TRUE;
        wchar_t title[180]{};
        if (!GetWindowTextW(candidate, title, ARRAYSIZE(title)))
            return TRUE;
        DWORD pid{};
        GetWindowThreadProcessId(candidate, &pid);
        try {
            AddAudioApplication(pid, title);
        } catch (...) {
            return FALSE;
        }
        return audioSources.size() < 256;
    }, 0);
    try {
        auto devices = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
        winrt::com_ptr<IMMDeviceCollection> endpoints;
        winrt::check_hresult(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, endpoints.put()));
        UINT count{};
        winrt::check_hresult(endpoints->GetCount(&count));
        for (UINT i = 0; i < count; ++i) {
            winrt::com_ptr<IMMDevice> device;
            if (FAILED(endpoints->Item(i, device.put())))
                continue;
            wchar_t *id{};
            if (FAILED(device->GetId(&id)))
                continue;
            const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(id, CoTaskMemFree);
            winrt::com_ptr<IPropertyStore> properties;
            std::wstring name = nskry::plugin_ui::Text(L"Device ", L"设备 ") + std::to_wstring(i + 1);
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.put()))) {
                PROPVARIANT value{};
                struct Clear {
                    PROPVARIANT &value;
                    ~Clear() { PropVariantClear(&value); }
                } clear{value};
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                    value.vt == VT_LPWSTR && value.pwszVal)
                    name = value.pwszVal;
            }
            audioSources.push_back({nskry::plugin_ui::Text(L"Output: ", L"输出 · ") + name, {id}});
            AddEndpointSessions(*device);
        }
    } catch (const winrt::hresult_error &) {
        // Window-based selection remains usable if endpoint enumeration fails.
    }
    std::stable_partition(audioSources.begin() + 1, audioSources.end(),
                          [](const AudioChoice &choice) { return !choice.source.endpoint.empty(); });
    SendMessageW(audioChoice, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < audioSources.size(); ++i) {
        const auto item = SendMessageW(audioChoice, CB_ADDSTRING, 0, LPARAM(audioSources[i].label.c_str()));
        if (item >= 0)
            SendMessageW(audioChoice, CB_SETITEMDATA, item, i);
    }
    SendMessageW(audioChoice, CB_SETCURSEL, 0, 0);
}
bool setup = false, gifMode = false, copied = false, delivered = false, completed = false, trimMode = false;
bool requestingGif = false;
bool preferencesOnly = false;
std::wstring resultPath, exportPath;
Snapshot resultSnapshot{};
std::unique_ptr<ExportTask> exportTask;
bool exportingTrim = false;
int64_t exportStart{}, exportEnd{};
HINSTANCE module{};
bool closing = false;
bool selectingOutput = false;
bool closePromptActive = false;
bool failurePromptActive = false;
NskryPluginInfo info{sizeof(info),
                     NSKRY_PLUGIN_API_VERSION,
                     L"Screen recording",
                     L"nskry-record",
                     L"0.5.0",
                     L"Nskry Team",
                     L"On-demand MP4/GIF region recording with optional window follow and MP4 application or output audio.",
                     nullptr,
                     NSKRY_CAP_TOOLBAR_ACTION,
                     {}};
constexpr UINT Poll = 1;
constexpr UINT PreviewPoll = 2;
constexpr wchar_t ClassName[] = L"NskryRecordControlV1";
void Update();
void Error(const wchar_t *text);
void LayoutControl();
void BeginExport(bool trim);
void CloseCleanupWindow() {
    if (cleanupWindow) {
        KillTimer(cleanupWindow, 1);
        DestroyWindow(cleanupWindow);
        cleanupWindow = nullptr;
    }
    cleanupPending = false;
}
void DrainClosedControl() {
    if (session) {
        const auto state = session->Status().state;
        if (state != State::Done && state != State::Failed)
            return;
        session.reset(); // Terminal state is published after capture/encoder release.
    }
    if (previewTask) {
        if (!previewTask->Done())
            return;
        previewTask.reset();
    }
    if (exportTask) {
        if (!exportTask->Status().done)
            return;
        // A completed save belongs to the user. Keep the original recording
        // and any completed trim too; abrupt owner destruction is not discard.
        exportTask.reset();
    }
    previewFrame = {};
    resultPath.clear();
    exportPath.clear();
    completed = copied = delivered = trimMode = setup = closing = false;
    closePromptActive = false;
    CloseCleanupWindow();
}
LRESULT CALLBACK CleanupProcedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_TIMER && wp == 1) {
        try {
            DrainClosedControl();
        } catch (...) {
            // Keep ownership for shutdown if allocation/status retrieval fails.
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
bool PrepareCleanupWindow() {
    WNDCLASSW wc{};
    wc.hInstance = module;
    wc.lpfnWndProc = CleanupProcedure;
    wc.lpszClassName = L"NskryRecordCleanupV1";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    cleanupWindow = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, nullptr);
    return cleanupWindow != nullptr;
}
void QueuePreview(int64_t time) {
    if (previewFailed)
        return;
    wantedPreview = (std::max)(int64_t(0), time);
    if (previewTask && previewTask->Done())
        previewTask.reset();
    if (!previewTask)
        previewTask = std::make_unique<PreviewTask>(resultPath, resultSnapshot.width, resultSnapshot.height, gifMode);
    previewRetry = !previewTask->Request(wantedPreview);
    SetTimer(window, PreviewPoll, 40, nullptr);
}
void CancelPreview() {
    previewRetry = false;
    if (previewTask) {
        previewTask->Cancel();
        SetTimer(window, PreviewPoll, 40, nullptr);
    }
    previewFrame = {};
}
void PreviewTick() {
    if (!previewTask) {
        KillTimer(window, PreviewPoll);
        return;
    }
    PreviewFrame frame;
    bool previewDone{};
    const bool hasPreview = previewTask->Take(frame, previewDone);
    if (hasPreview && trimMode) {
        previewFrame = std::move(frame);
        previewFailed = !previewFrame.error.empty();
        InvalidateRect(window, nullptr, FALSE);
    }
    if (!previewDone)
        return;
    previewTask.reset();
    KillTimer(window, PreviewPoll);
    if (pendingClose || pendingDiscard) {
        const bool discard = pendingDiscard;
        pendingClose = pendingDiscard = false;
        if (discard) {
            SendMessageW(window, WM_COMMAND, 3, 0);
        } else
            SendMessageW(window, WM_CLOSE, 0, 0);
    } else if (pendingExport >= 0) {
        const bool trim = pendingExport != 0;
        pendingExport = -1;
        BeginExport(trim);
    } else if ((rangeDragging || previewRetry) && trimMode && !previewFailed) {
        QueuePreview(wantedPreview);
        if (!rangeDragging && previewTask)
            previewTask->Finish();
    }
}
void SyncRangeEdits() {
    syncingRange = true;
    SetWindowTextW(trimStart, std::to_wstring((rangeStart / 10000) / 1000.0).c_str());
    SetWindowTextW(trimEnd, std::to_wstring((rangeEnd / 10000) / 1000.0).c_str());
    syncingRange = false;
    InvalidateRect(rangeWindow, nullptr, FALSE);
}
LRESULT CALLBACK RangeProcedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int left = 12, right = (std::max)(13L, client.right - 12);
    const auto duration = (std::max)(int64_t(1), resultSnapshot.encodedDuration);
    const int a = left + int((long double)rangeStart * (right - left) / duration);
    const int b = left + int((long double)rangeEnd * (right - left) / duration);
    try {
        if (exportTask && (msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE || msg == WM_SETCURSOR)) {
            if (msg == WM_SETCURSOR) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            return 0;
        }
        switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        case WM_LBUTTONDOWN:
            previewFailed = false;
            rangeTail = std::abs(GET_X_LPARAM(lp) - b) < std::abs(GET_X_LPARAM(lp) - a);
            rangeDragging = true;
            SetCapture(hwnd);
            [[fallthrough]];
        case WM_MOUSEMOVE:
            if (rangeDragging && (wp & MK_LBUTTON || msg == WM_LBUTTONDOWN)) {
                const auto value = TrimTimeAt(GET_X_LPARAM(lp), left, right, duration);
                const auto gap = (std::min)(int64_t(10000), duration);
                if (rangeTail)
                    rangeEnd = std::clamp(value, rangeStart + gap, duration);
                else
                    rangeStart = std::clamp(value, int64_t(0), rangeEnd - gap);
                SyncRangeEdits();
                QueuePreview(rangeTail ? rangeEnd - 1 : rangeStart);
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            if (rangeDragging) {
                rangeDragging = false;
                if (previewTask)
                    previewTask->Finish();
                if (GetCapture() == hwnd)
                    ReleaseCapture();
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(hwnd, &ps);
            auto memory = CreateCompatibleDC(dc);
            auto bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            auto old = SelectObject(memory, bitmap);
            FillRect(memory, &client, HBRUSH(COLOR_WINDOW + 1));
            RECT track{left, 10, right, 24};
            auto gray = CreateSolidBrush(RGB(160, 160, 160));
            FillRect(memory, &track, gray);
            DeleteObject(gray);
            RECT selected{a, 10, b, 24};
            auto blue = CreateSolidBrush(RGB(40, 140, 220));
            FillRect(memory, &selected, blue);
            DeleteObject(blue);
            for (int x : {a, b}) {
                RECT handle{x - 5, 3, x + 5, 31};
                FillRect(memory, &handle, HBRUSH(COLOR_BTNTEXT + 1));
            }
            BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, old);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(hwnd, &ps);
            return 0;
        }
        }
    } catch (...) {
        rangeDragging = false;
        if (GetCapture() == hwnd)
            ReleaseCapture();
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
void ClearResult() {
    CancelPreview();
    if (!resultPath.empty() && !copied)
        DeleteFileW(resultPath.c_str());
    resultPath.clear();
    completed = copied = delivered = trimMode = false;
}
void Area(RECT region, bool visible) {
    if (!visible) {
        if (areaWindow)
            ShowWindow(areaWindow, SW_HIDE);
        return;
    }
    if (!areaWindow) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
            if (m == WM_NCHITTEST)
                return HTTRANSPARENT;
            if (m == WM_PAINT) {
                PAINTSTRUCT ps{};
                HDC dc = BeginPaint(h, &ps);
                RECT r{};
                GetClientRect(h, &r);
                HBRUSH brush = CreateSolidBrush(RGB(240, 70, 45));
                FillRect(dc, &r, brush);
                DeleteObject(brush);
                EndPaint(h, &ps);
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
        };
        wc.hInstance = module;
        wc.lpszClassName = L"NskryRecordAreaV1";
        RegisterClassW(&wc);
        areaWindow = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED, wc.lpszClassName,
            nskry::plugin_ui::Text(L"Recording region", L"录制区域"), WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, module, nullptr);
        if (!areaWindow || !SetWindowDisplayAffinity(areaWindow, WDA_EXCLUDEFROMCAPTURE)) {
            if (areaWindow)
                DestroyWindow(areaWindow);
            areaWindow = nullptr;
            return;
        }
        SetLayeredWindowAttributes(areaWindow, 0, 255, LWA_ALPHA);
        areaBounds = {};
    }
    if (!EqualRect(&areaBounds, &region)) {
        areaBounds = region;
        const int width = region.right - region.left, height = region.bottom - region.top;
        HRGN outer = CreateRectRgn(0, 0, width, height);
        HRGN inner = CreateRectRgn(2, 2, width - 2, height - 2);
        CombineRgn(outer, outer, inner, RGN_DIFF);
        DeleteObject(inner);
        if (!SetWindowRgn(areaWindow, outer, TRUE))
            DeleteObject(outer);
        SetWindowPos(areaWindow, HWND_TOPMOST, region.left, region.top, width, height, SWP_NOACTIVATE);
    }
    ShowWindow(areaWindow, SW_SHOWNOACTIVATE);
}
void SetTrimVisible(bool visible) {
    for (HWND h : {trimStart, trimEnd, trimApply, trimCancel})
        ShowWindow(h, visible ? SW_SHOW : SW_HIDE);
    ShowWindow(rangeWindow, visible ? SW_SHOW : SW_HIDE);
    if (!visible)
        CancelPreview();
    layoutMode = -1;
    LayoutControl();
}
void LayoutControl() {
    if (!window)
        return;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromRect(&setupRegion, MONITOR_DEFAULTTONEAREST), &mi))
        return;
    const UINT windowDpi = ::GetDpiForWindow(window);
    const UINT dpi = windowDpi ? windowDpi : 96;
    const auto dip = [dpi](int value) { return ::MulDiv(value, dpi, 96); };
    const bool showTrim = trimMode && !exportTask;
    const int mode = setup ? (preferencesOnly ? 4 : 0) : completed ? (showTrim ? 3 : 2) : 1;
    const auto bounds =
        RecordingBarBounds(setupRegion, mi.rcWork, dip(mode == 0 || mode == 4 ? 900 : 800), dip(mode == 3 ? 386 : mode == 0 && !gifMode ? 88 : mode == 1 || mode == 2 ? 116 : 52));
    if (mode == layoutMode && EqualRect(&bounds, &layoutBounds))
        return;
    layoutMode = mode;
    layoutBounds = bounds;
    SetWindowPos(window, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOACTIVATE);
    RECT client{};
    GetClientRect(window, &client);
    client.right = ::MulDiv(client.right, 96, dpi);
    const auto place = [&](HWND h, bool visible, int x, int y, int w, int height = 32) {
        if (!h) return;
        ::SetWindowPos(h, nullptr, dip(x), dip(y), dip(w), dip(height), SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(h, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    };
    const bool pre = mode == 0 || mode == 4;
    const bool chooseAudio = mode == 0 && !gifMode;
    place(audioLabel, chooseAudio, 12, 45, 65);
    place(audioChoice, chooseAudio, 80, 42, (std::max)(100L, client.right - 92), 240);
    const auto setupWidths = SetupWidths(client.right);
    const bool compact = pre && setupWidths.compact;
    const int statusWidth = compact ? 0 : (std::max)(45L, client.right - (pre ? 570 : 420));
    place(statusLabel, !compact, 8, 8, pre ? statusWidth : client.right - 16, pre ? 34 : 60);
    int x = compact ? setupWidths.left : setupWidths.left + statusWidth;
    if (pre) {
        place(fpsChoice, true, x, 8, setupWidths.fps, 180);
        if (compact)
            SendMessageW(fpsChoice, CB_SETDROPPEDWIDTH, 90, 0);
        x += setupWidths.fps + setupWidths.gap;
        place(qualityChoice, true, x, 8, setupWidths.quality, 180);
        if (compact)
            SendMessageW(qualityChoice, CB_SETDROPPEDWIDTH, 100, 0);
        x += setupWidths.quality + setupWidths.gap;
        if (compact)
            SetWindowTextW(followChoice, nskry::plugin_ui::Text(L"Follow", L"跟随"));
        else
            SetWindowTextW(followChoice, nskry::plugin_ui::Text(L"Follow selected window", L"跟随所选窗口"));
        place(followChoice, mode == 0, x, 8, setupWidths.follow);
        place(cacheButton, mode == 4, x, 8, setupWidths.follow);
        x += setupWidths.follow + setupWidths.gap;
        place(defaultsButton, !compact, x, 8, setupWidths.defaults);
        if (!compact)
            x += setupWidths.defaults + setupWidths.gap;
        place(pauseButton, mode == 0, x, 8, setupWidths.start);
        if (compact)
            SetWindowTextW(pauseButton, nskry::plugin_ui::Text(L"Start", L"开始"));
        x += setupWidths.start + setupWidths.gap;
        if (compact)
            SetWindowTextW(discardButton, nskry::plugin_ui::Text(L"Cancel", L"取消"));
        place(discardButton, true, x, 8, setupWidths.cancel);
        place(stopButton, false, 0, 0, 1);
        place(trimButton, false, 0, 0, 1);
    } else {
        for (HWND h : {fpsChoice, qualityChoice, followChoice, defaultsButton, cacheButton})
            ShowWindow(h, SW_HIDE);
        const int count = completed ? 4 : 3;
        const int width = (std::max)(1, static_cast<int>(client.right - 16 - 6 * (count - 1)) / count);
        int actionX = 8;
        for (HWND button : {pauseButton, stopButton, discardButton, trimButton}) {
            const bool visible = button != trimButton || completed;
            place(button, visible, actionX, 70, width, 32);
            actionX += width + 6;
        }
    }
    place(rangeWindow, showTrim, 12, 282, client.right - 24, 36);
    place(trimStart, showTrim, 12, 328, 100);
    place(trimEnd, showTrim, 124, 328, 100);
    place(trimApply, showTrim, 252, 328, 104);
    place(trimCancel, showTrim, 368, 328, 104);
    InvalidateRect(window, nullptr, FALSE);
}
void ResultControls() {
    SetWindowTextW(pauseButton, nskry::plugin_ui::Text(L"Save...", L"保存…"));
    SetWindowTextW(stopButton, nskry::plugin_ui::Text(L"Copy file", L"复制文件"));
    SetWindowTextW(discardButton, delivered || copied ? nskry::plugin_ui::Text(L"Close", L"关闭") : nskry::plugin_ui::Text(L"Discard", L"丢弃"));
    for (HWND h : {pauseButton, stopButton, discardButton, trimButton})
        EnableWindow(h, !exportTask);
    ShowWindow(trimButton, SW_SHOW);
    SetWindowTextW(statusLabel, L"Recording finished. Save it, copy the file, or trim its beginning/end.\nCopy retains "
                                L"a local file for clipboard/history; paste support depends on the destination app.");
    LayoutControl();
}
bool ReadSeconds(HWND edit, int64_t &ticks) {
    wchar_t text[64]{};
    GetWindowTextW(edit, text, 64);
    wchar_t *end{};
    const double value = wcstod(text, &end);
    if (end == text || *end || !std::isfinite(value) || value < 0 || value > 86400 * 365)
        return false;
    ticks = int64_t(std::llround(value * 10000000));
    return true;
}
void BeginExport(bool trim) {
    if (exportTask || !completed)
        return;
    if (previewTask && !previewTask->Done()) {
        pendingExport = trim ? 1 : 0;
        rangeDragging = false;
        CancelPreview();
        return;
    }
    exportStart = 0;
    exportEnd = resultSnapshot.encodedDuration;
    if (trim) {
        if (!ReadSeconds(trimStart, exportStart) || !ReadSeconds(trimEnd, exportEnd) || exportStart < 0 ||
            exportEnd <= exportStart || exportEnd > resultSnapshot.encodedDuration) {
            Error(nskry::plugin_ui::Text(L"Enter start/end seconds inside the recording (start < end).", L"请输入录制范围内的起止秒数，开始时间须早于结束时间。"));
            return;
        }
        exportPath = NewRecordingPath(gifMode);
    } else {
        std::wstring path(32768, L'\0');
        wcscpy_s(path.data(), path.size(), gifMode ? L"Recording.gif" : L"Recording.mp4");
        OPENFILENAMEW d{sizeof(d)};
        d.hwndOwner = window;
        d.lpstrFilter = gifMode ? L"GIF animation\0*.gif\0\0" : L"MP4 video\0*.mp4\0\0";
        d.lpstrFile = path.data();
        d.nMaxFile = DWORD(path.size());
        d.lpstrDefExt = gifMode ? L"gif" : L"mp4";
        d.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&d))
            return;
        path.resize(wcslen(path.c_str()));
        if (_wcsicmp(std::filesystem::path(path).extension().c_str(), gifMode ? L".gif" : L".mp4") ||
            GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            Error(nskry::plugin_ui::Text(L"Choose a new file with the correct extension. Existing files are never overwritten.", L"请选择扩展名正确的新文件名。已有文件会保留。"));
            return;
        }
        exportPath = path;
    }
    auto task = std::make_unique<ExportTask>();
    task->Start(resultPath, exportPath, resultSnapshot, gifMode, exportStart, exportEnd, trim);
    exportTask = std::move(task);
    exportingTrim = trim;
    ResultControls();
    SetWindowTextW(statusLabel, trim ? nskry::plugin_ui::Text(L"Trimming / re-encoding... Cancel export leaves the original unchanged.", L"正在裁剪和重新编码… 取消导出会保留原始录制。")
                                     : nskry::plugin_ui::Text(L"Saving... Cancel export leaves the original unchanged.", L"正在保存… 取消导出会保留原始录制。"));
    EnableWindow(discardButton, TRUE);
    SetWindowTextW(discardButton, nskry::plugin_ui::Text(L"Cancel export", L"取消导出"));
    SetTrimVisible(false);
    SetTimer(window, Poll, 250, nullptr);
}

void Update() {
    if (exportTask) {
        const auto status = exportTask->Status();
        if (status.done) {
            exportTask.reset();
            KillTimer(window, Poll);
            if (status.success) {
                if (exportingTrim) {
                    if (!copied)
                        DeleteFileW(resultPath.c_str());
                    resultPath = exportPath;
                    resultSnapshot.encodedDuration = exportEnd - exportStart;
                    copied = delivered = false;
                } else
                    delivered = true;
            }
            trimMode = false;
            ResultControls();
            SetWindowTextW(statusLabel, status.message.c_str());
        }
        return;
    }
    if (!session)
        return;
    const auto s = session->Status();
    const bool finished = s.state == State::Done || s.state == State::Failed;
    if (!finished) {
        setupRegion = s.region;
        LayoutControl();
    }
    Area(s.region, !finished && (s.state == State::Recording || s.state == State::Starting));
    const wchar_t *state = s.state == State::Starting     ? nskry::plugin_ui::Text(L"Starting", L"正在准备")
                           : s.state == State::Recording  ? nskry::plugin_ui::Text(L"● Recording", L"● 录制中")
                           : s.state == State::Paused     ? nskry::plugin_ui::Text(L"Ⅱ Paused", L"Ⅱ 已暂停")
                           : s.state == State::Stopping   ? nskry::plugin_ui::Text(L"Finalizing", L"正在完成录制")
                           : s.state == State::Discarding ? nskry::plugin_ui::Text(L"Discarding", L"正在丢弃")
                           : s.state == State::Publishing ? nskry::plugin_ui::Text(L"Publishing (cannot discard)", L"正在写入文件")
                           : s.state == State::Recovering ? nskry::plugin_ui::Text(L"Recovering captured portion", L"正在恢复已有录制")
                                                          : nskry::plugin_ui::Text(L"Finished", L"已完成");
    const auto seconds = s.duration / 10000000;
    const auto rate = s.duration > 0 ? s.frames * 10000000 / uint64_t(s.duration) : 0;
    const auto text =
        (closePromptActive ? std::wstring(nskry::plugin_ui::Text(L"Close recording? Save, discard, or go back.\n", L"要关闭录制吗？请选择保存、丢弃或返回。\n")) : std::wstring()) + state +
        L"  " + std::to_wstring(seconds / 60) + L":" + (seconds % 60 < 10 ? L"0" : L"") +
        std::to_wstring(seconds % 60) +
        (s.fps ? L"  (" + std::to_wstring(rate) + L" / " + std::to_wstring(s.fps) + L" fps)" : L"") + L"\n" +
        (finished ? s.message : s.backend);
    const int length = GetWindowTextLengthW(statusLabel);
    std::wstring previous(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(statusLabel, previous.data(), length + 1);
    previous.resize(length);
    if (previous != text)
        SetWindowTextW(statusLabel, text.c_str());
    if (closePromptActive) {
        // Confirmation stays INSIDE the capture-excluded control window. No
        // new top-level MessageBox which could leak into a full-screen recording.
        SetWindowTextW(pauseButton, finished ? nskry::plugin_ui::Text(L"Close", L"关闭") : nskry::plugin_ui::Text(L"Save / close", L"保存并关闭"));
        SetWindowTextW(stopButton, nskry::plugin_ui::Text(L"Discard / close", L"丢弃并关闭"));
        SetWindowTextW(discardButton, nskry::plugin_ui::Text(L"Back", L"返回"));
        EnableWindow(pauseButton, s.state != State::Recovering && s.state != State::Discarding);
        EnableWindow(stopButton, !finished && s.state != State::Publishing && s.state != State::Discarding);
        EnableWindow(discardButton, TRUE);
        return; // Keep the session alive while its inline confirmation is shown.
    }
    SetWindowTextW(pauseButton, s.state == State::Paused ? nskry::plugin_ui::Text(L"Resume", L"继续") : nskry::plugin_ui::Text(L"Pause", L"暂停"));
    SetWindowTextW(stopButton, nskry::plugin_ui::Text(L"Stop / save", L"结束录制"));
    EnableWindow(pauseButton, s.state == State::Recording || s.state == State::Paused);
    EnableWindow(stopButton, s.state == State::Starting || s.state == State::Recording || s.state == State::Paused);
    SetWindowTextW(discardButton, finished ? nskry::plugin_ui::Text(L"Close", L"关闭") : nskry::plugin_ui::Text(L"Cancel / discard", L"取消或丢弃"));
    EnableWindow(discardButton, s.state != State::Publishing);
    if (finished) {
        KillTimer(window, Poll);
        session.reset(); // Done published after resource release; join finished thread, no idle thread.
        if (!resultPath.empty() && s.state == State::Done && s.frames &&
            GetFileAttributesW(resultPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            resultSnapshot = s;
            completed = true;
            closing = false;
            ResultControls();
            return;
        }
        if (!resultPath.empty())
            ClearResult();
        if (s.state == State::Failed) {
            closing = false;
            failurePromptActive = true;
            MessageBoxW(window, s.message.c_str(), nskry::plugin_ui::Text(L"Recording failed", L"录制失败"), MB_OK | MB_ICONWARNING);
            failurePromptActive = false;
        }
        if (closing)
            DestroyWindow(window);
    }
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DPICHANGED:
        layoutMode = -1;
        if (window) ::PostMessageW(hwnd, WM_APP + 42, 0, 0);
        return 0;
    case WM_APP + 42:
        LayoutControl();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r{};
        GetClientRect(hwnd, &r);
        auto memory = CreateCompatibleDC(dc);
        auto bitmap = CreateCompatibleBitmap(dc, r.right, r.bottom);
        auto old = SelectObject(memory, bitmap);
        const auto colors = nskry::plugin_ui::Colors(nskry::plugin_ui::preferences.theme);
        FillRect(memory, &r, nskry::plugin_ui::Brush(memory, colors.window));
        if (trimMode) {
            const UINT dpi = ::GetDpiForWindow(hwnd);
            const auto dip = [dpi](int value) { return ::MulDiv(value, dpi ? dpi : 96, 96); };
            RECT view{dip(12), dip(112), r.right - dip(12), dip(272)};
            FillRect(memory, &view, nskry::plugin_ui::Brush(memory, colors.panel));
            if (!previewFrame.pixels.empty()) {
                const double scale = (std::min)(double(view.right - view.left) / previewFrame.width,
                                                double(view.bottom - view.top) / previewFrame.height);
                const int w = int(previewFrame.width * scale), h = int(previewFrame.height * scale);
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = previewFrame.width;
                info.bmiHeader.biHeight = -LONG(previewFrame.height);
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                SetStretchBltMode(memory, HALFTONE);
                StretchDIBits(memory, view.left + (view.right - view.left - w) / 2,
                              view.top + (view.bottom - view.top - h) / 2, w, h, 0, 0, previewFrame.width,
                              previewFrame.height, previewFrame.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
            } else {
                SetBkMode(memory, TRANSPARENT);
                SetTextColor(memory, colors.text);
                const auto *hint = previewFrame.error.empty() ? nskry::plugin_ui::Text(L"Drag the start/end handle to preview a frame", L"拖动起止手柄可预览该帧")
                                                              : nskry::plugin_ui::Text(L"Preview unavailable; you can still trim by time", L"无法预览画面，仍可按时间裁剪");
                DrawTextW(memory, hint, -1, &view, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        BitBlt(dc, 0, 0, r.right, r.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == PreviewPoll) {
            try {
                PreviewTick();
            } catch (...) {
                CancelPreview();
            }
            return 0;
        }
        Update();
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return nskry::plugin_ui::ControlColors(wp, nskry::plugin_ui::preferences.theme, msg == WM_CTLCOLOREDIT);
    case WM_COMMAND:
        try {
            if (setup) {
                if (preferencesOnly && LOWORD(wp) == 15) {
                    if (MessageBoxW(hwnd,
                            L"Copied recordings remain here so clipboard paste and history can use them.\n"
                            L"Deleting a file may break later paste. Save anything you need before deleting it.\n\n"
                            L"Open the cache folder to manage files yourself?",
                            nskry::plugin_ui::Text(L"Recording cache", L"录制缓存"), MB_OKCANCEL | MB_ICONINFORMATION) == IDOK &&
                        !OpenRecordingCache(hwnd))
                        Error(nskry::plugin_ui::Text(L"Could not open the recording cache folder.", L"无法打开录制缓存文件夹。"));
                    return 0;
                }
                if (LOWORD(wp) == 3) {
                    setup = false;
                    DestroyWindow(hwnd);
                    return 0;
                }
                const Options o{SendMessageW(fpsChoice, CB_GETCURSEL, 0, 0) == 1 ? 60u : 30u,
                                Quality(SendMessageW(qualityChoice, CB_GETCURSEL, 0, 0))};
                if (LOWORD(wp) == 11) {
                    if (!SaveDefaults(o))
                        Error(nskry::plugin_ui::Text(L"Could not save recording defaults.", L"无法保存默认录制设置。"));
                    else
                        SetWindowTextW(statusLabel, nskry::plugin_ui::Text(L"Default quality and frame rate saved.", L"已保存默认画质和帧率。"));
                    return 0;
                }
                if (LOWORD(wp) != 1)
                    return 0;
                if (preferencesOnly)
                    return 0;
                if (serviceV2.version != 2) {
                    Error(nskry::plugin_ui::Text(L"This version requires capture service v2.", L"此版本需要支持采集服务 v2 的 Nskry。"));
                    return 0;
                }
                SessionOptions options;
                options.fps = gifMode ? 10 : o.fps;
                options.quality = o.quality;
                options.gif = gifMode;
                if (!gifMode) {
                    const auto item = SendMessageW(audioChoice, CB_GETCURSEL, 0, 0);
                    const auto index = SendMessageW(audioChoice, CB_GETITEMDATA, item, 0);
                    if (index < 0 || size_t(index) >= audioSources.size()) {
                        Error(nskry::plugin_ui::Text(L"Select an audio source or No audio.", L"请选择音频来源，或选择「不录音频」。"));
                        return 0;
                    }
                    options.audio = audioSources[size_t(index)].source;
                }
                options.deliveryCache = true;
                options.capture = serviceV2;
                options.followWindow =
                    SendMessageW(followChoice, BM_GETCHECK, 0, 0) == BST_CHECKED ? setupSource : nullptr;
                resultPath = NewRecordingPath(gifMode);
                auto recording = std::make_unique<RecordSession>(service, setupRegion, resultPath, true, options);
                if (!recording->Start()) {
                    ClearResult();
                    Error(nskry::plugin_ui::Text(L"Could not start recording.", L"无法开始录制。"));
                    return 0;
                }
                session = std::move(recording);
                setup = false;
                for (HWND h : {fpsChoice, qualityChoice, followChoice, defaultsButton})
                    ShowWindow(h, SW_HIDE);
                EnableWindow(stopButton, TRUE);
                SetTimer(hwnd, Poll, 250, nullptr);
                Update();
                return 0;
            }
            if (completed) {
                if (trimMode && !syncingRange && HIWORD(wp) == EN_CHANGE && (LOWORD(wp) == 12 || LOWORD(wp) == 13)) {
                    int64_t a{}, b{};
                    if (ReadSeconds(trimStart, a) && ReadSeconds(trimEnd, b) && a >= 0 && b > a &&
                        b <= resultSnapshot.encodedDuration) {
                        rangeStart = a;
                        rangeEnd = b;
                        InvalidateRect(rangeWindow, nullptr, FALSE);
                    }
                    return 0;
                }
                if (exportTask) {
                    if (LOWORD(wp) == 3)
                        exportTask->Cancel();
                    return 0;
                }
                switch (LOWORD(wp)) {
                case 1:
                    BeginExport(false);
                    break;
                case 2:
                    if (!CopyRecordingFile(hwnd, resultPath))
                        Error(nskry::plugin_ui::Text(L"The file could not be copied. Try again.", L"无法复制文件，请重试。"));
                    else {
                        copied = true;
                        ResultControls();
                        SetWindowTextW(
                            statusLabel,
                            L"File copied. Paste in an app accepting files; local clipboard cache retained.");
                    }
                    break;
                case 3:
                    if (previewTask && !previewTask->Done()) {
                        pendingDiscard = true;
                        CancelPreview();
                        return 0;
                    }
                    ClearResult();
                    DestroyWindow(hwnd);
                    break;
                case 4:
                    trimMode = true;
                    rangeStart = 0;
                    rangeEnd = resultSnapshot.encodedDuration;
                    SyncRangeEdits();
                    SetTrimVisible(true);
                    SetWindowTextW(statusLabel, L"Trim range in seconds: Start (left), End (right). Apply creates a "
                                                L"new recording; original is unchanged on failure.");
                    break;
                case 5:
                    BeginExport(true);
                    break;
                case 6:
                    trimMode = false;
                    SetTrimVisible(false);
                    ResultControls();
                    break;
                }
                return 0;
            }
            if (closePromptActive) {
                const auto state = session ? session->Status().state : State::Done;
                if (LOWORD(wp) == 1) {
                    closing = !session || state == State::Done || state == State::Failed || state == State::Stopping ||
                              state == State::Publishing || session->Stop(false);
                } else if (LOWORD(wp) == 2) {
                    closing = session && session->Stop(true);
                } else if (LOWORD(wp) == 3) {
                    closing = false;
                } else
                    return 0;
                closePromptActive = false;
                Update();
                return 0;
            }
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
        } catch (const winrt::hresult_error &e) {
            Error(e.message().c_str());
            return 0;
        } catch (...) {
            Error(nskry::plugin_ui::Text(L"Recording operation failed. The original file is unchanged.", L"录制操作失败，原始文件已保留。"));
            return 0;
        }
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND && session)
            session->Stop(false);
        return TRUE;
    case WM_DISPLAYCHANGE:
        if (session)
            session->Stop(false);
        return 0;
    case WM_CLOSE:
        if (previewTask && !previewTask->Done()) {
            pendingClose = true;
            CancelPreview();
            return 0;
        }
        if (exportTask) {
            exportTask->Cancel();
            return 0;
        }
        if (setup) {
            setup = false;
            DestroyWindow(hwnd);
            return 0;
        }
        if (completed) {
            if (!copied && !delivered &&
                MessageBoxW(hwnd, nskry::plugin_ui::Text(L"Discard this unsaved recording?", L"要丢弃这段尚未保存的录制吗？"), nskry::plugin_ui::Text(L"Recording", L"录制中"), MB_YESNO | MB_ICONQUESTION) !=
                    IDYES)
                return 0;
            ClearResult();
        }
        if (closePromptActive)
            return 0;
        if (session) {
            const auto state = session->Status().state;
            if (state == State::Publishing || state == State::Discarding) {
                closing = true;
                return 0;
            }
            closePromptActive = true;
            Update();
        } else
            DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        audioSources.clear();
        audioChoice = audioLabel = nullptr;
        if (previewTask && previewTask->Done())
            previewTask.reset();
        previewFrame = {};
        KillTimer(hwnd, PreviewPoll);
        pendingClose = pendingDiscard = false;
        pendingExport = -1;
        rangeDragging = false;
        Area({}, false);
        if (areaWindow)
            DestroyWindow(areaWindow);
        areaWindow = nullptr;
        KillTimer(hwnd, Poll);
        window = nullptr;
        setup = false;
        if (session || previewTask || exportTask) {
            cleanupPending = true;
            if (session)
                session->Stop(false); // Finalize captured footage; never keep recording without UI.
            if (previewTask)
                previewTask->Cancel();
            if (exportTask)
                exportTask->Cancel();
            if (!SetTimer(cleanupWindow, 1, 50, nullptr)) {
                // Resource exhaustion: keep safe DLL ownership and stop work,
                // even if synchronous teardown is the only remaining option.
                session.reset();
                previewTask.reset();
                exportTask.reset();
                DrainClosedControl();
            }
        } else {
            // A destroyed result window must not leave a permanent busy flag.
            resultPath.clear(); // Keep unsaved footage; no implicit discard.
            completed = copied = delivered = trimMode = false;
            CloseCleanupWindow();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
bool ShowControl(RECT region) {
    if (cleanupPending)
        return false;
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
    if (!monitor || !GetMonitorInfoW(monitor, &mi))
        return false;
    setupRegion = region;
    layoutMode = -1;
    const auto initial = RecordingBarBounds(region, mi.rcWork, 800, 52);
    window =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, ClassName, nskry::plugin_ui::Text(L"Nskry — Recording / settings", L"Nskry — 录屏"),
                        WS_POPUP | WS_BORDER | WS_CLIPCHILDREN, initial.left, initial.top, initial.right - initial.left,
                        initial.bottom - initial.top, nullptr, nullptr, module, nullptr);
    if (!window)
        return false;
    if (!PrepareCleanupWindow()) {
        DestroyWindow(window);
        return false;
    }
    // Refuse recording if the OS cannot exclude this control window. No black UI box in output.
    if (!SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) {
        DestroyWindow(window);
        return false;
    }
    statusLabel = CreateWindowExW(0, L"STATIC", nskry::plugin_ui::Text(L"Starting...", L"正在准备…"), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 12, 10, 510, 60,
                                  window, nullptr, module, nullptr);
    pauseButton = CreateWindowExW(0, L"BUTTON", nskry::plugin_ui::Text(L"Pause", L"暂停"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_MULTILINE, 12, 78, 100, 28, window,
                                  HMENU(1), module, nullptr);
    stopButton = CreateWindowExW(0, L"BUTTON", nskry::plugin_ui::Text(L"Stop / save", L"结束录制"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_MULTILINE, 124, 78, 115, 28,
                                 window, HMENU(2), module, nullptr);
    discardButton = CreateWindowExW(0, L"BUTTON", nskry::plugin_ui::Text(L"Cancel / discard", L"取消或丢弃"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_MULTILINE, 252, 78, 140,
                                    28, window, HMENU(3), module, nullptr);
    const auto child = [&](const wchar_t *type, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
        return CreateWindowExW(0, type, text, WS_CHILD | style, x, y, w, h, window, HMENU(INT_PTR(id)), module,
                               nullptr);
    };
    audioLabel = child(L"STATIC", nskry::plugin_ui::Text(L"Audio", L"音频"), 0, 12, 45, 65, 28, 0);
    audioChoice = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 80, 42, 400, 240, 16);
    fpsChoice = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 12, 122, 100, 160, 7);
    qualityChoice = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 124, 122, 120, 160, 8);
    followChoice = child(L"BUTTON", nskry::plugin_ui::Text(L"Follow window", L"跟随窗口"), BS_AUTOCHECKBOX | WS_TABSTOP, 12, 168, 225, 28, 9);
    defaultsButton = child(L"BUTTON", nskry::plugin_ui::Text(L"Save defaults", L"保存为默认"), WS_TABSTOP, 252, 122, 140, 28, 11);
    cacheButton = child(L"BUTTON", nskry::plugin_ui::Text(L"Cache folder...", L"缓存文件夹…"), WS_TABSTOP, 12, 168, 225, 28, 15);
    for (const auto *text : {L"30 fps", L"60 fps"})
        SendMessageW(fpsChoice, CB_ADDSTRING, 0, LPARAM(text));
    for (const auto *text : {nskry::plugin_ui::Text(L"中 / Medium", L"中等"), nskry::plugin_ui::Text(L"高 / High", L"高画质"), nskry::plugin_ui::Text(L"高清 / HD", L"高清")})
        SendMessageW(qualityChoice, CB_ADDSTRING, 0, LPARAM(text));
    trimButton = child(L"BUTTON", nskry::plugin_ui::Text(L"Trim...", L"裁剪…"), WS_TABSTOP, 404, 78, 100, 28, 4);
    trimStart = child(L"EDIT", L"0", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 12, 130, 100, 28, 12);
    trimEnd = child(L"EDIT", L"0", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 124, 130, 120, 28, 13);
    trimApply = child(L"BUTTON", nskry::plugin_ui::Text(L"Apply trim", L"应用裁剪"), WS_TABSTOP, 252, 130, 100, 28, 5);
    trimCancel = child(L"BUTTON", nskry::plugin_ui::Text(L"Cancel trim", L"取消裁剪"), WS_TABSTOP, 364, 130, 100, 28, 6);
    WNDCLASSW rangeClass{};
    rangeClass.lpfnWndProc = RangeProcedure;
    rangeClass.hInstance = module;
    rangeClass.lpszClassName = L"NskryRecordTrimRangeV1";
    RegisterClassW(&rangeClass);
    rangeWindow = child(rangeClass.lpszClassName, L"", 0, 12, 282, 760, 36, 14);
    if (!statusLabel || !pauseButton || !stopButton || !discardButton || !fpsChoice || !qualityChoice ||
        !followChoice || !defaultsButton || !cacheButton || !trimButton || !trimStart || !trimEnd || !trimApply ||
        !trimCancel || !rangeWindow || !audioLabel || !audioChoice) {
        DestroyWindow(window);
        return false;
    }
    closing = false;
    pendingClose = pendingDiscard = false;
    pendingExport = -1;
    LayoutControl();
    closePromptActive = false;
    nskry::plugin_ui::AttachTabNavigation(window);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    if (!SetTimer(window, Poll, 250, nullptr)) {
        DestroyWindow(window);
        return false;
    }
    return true;
}
void Error(const wchar_t *text) {
    MessageBoxW(window, text, nskry::plugin_ui::Text(L"Screen recording", L"录屏"), MB_OK | MB_ICONWARNING);
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
NSKRY_API int32_t NSKRY_CALL nskry_plugin_connect_capture_v2(const NskryCaptureServiceV2 *value) {
    if (!value || value->structSize < sizeof(*value) || value->version != 2 || !value->open || !value->read ||
        !value->bounds || !value->close)
        return 0;
    serviceV2 = *value;
    return 1;
}
NSKRY_API int32_t NSKRY_CALL nskry_plugin_init(const NskryHostContext *ctx) {
    return ctx && ctx->structSize >= sizeof(*ctx) && ctx->apiVersion == 2 &&
           (service.version == 1 || serviceV2.version == 2);
}
NSKRY_API void NSKRY_CALL nskry_plugin_execute(const NskryHostContext *ctx) {
    try {
        if (cleanupPending || session || setup || completed || exportTask || selectingOutput || failurePromptActive) {
            if (window)
                SetForegroundWindow(window);
            return;
        }
        gifMode = requestingGif;
        if (!ctx || ctx->structSize < sizeof(*ctx) || ctx->apiVersion != 2 || serviceV2.version != 2) {
            Error(nskry::plugin_ui::Text(L"This host does not provide region capture v2. Update the host and recording package together.", L"当前版本缺少区域采集 v2。请一起更新 Nskry 和录屏插件。"));
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
            Error(nskry::plugin_ui::Text(L"Select a region inside one monitor. Cross-monitor recording is not supported.", L"请在同一台显示器上框选录制区域。"));
            return;
        }
        if (window)
            DestroyWindow(window);
        if (!ShowControl(r)) {
            Error(nskry::plugin_ui::Text(L"Cannot create a capture-excluded recording control window.", L"无法建立可排除在录制画面外的控制窗口。"));
            return;
        }
        KillTimer(window, Poll);
        setup = true;
        preferencesOnly = false;
        setupRegion = r;
        setupSource = ctx->sourceHwnd ? GetAncestor(ctx->sourceHwnd, GA_ROOT) : nullptr;
        if (!gifMode)
            PopulateAudioChoices();
        const auto defaults = LoadDefaults();
        SendMessageW(fpsChoice, CB_SETCURSEL, defaults.fps == 60 ? 1 : 0, 0);
        SendMessageW(qualityChoice, CB_SETCURSEL, WPARAM(defaults.quality), 0);
        SendMessageW(followChoice, BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(followChoice, setupSource != nullptr);
        for (HWND h : {fpsChoice, qualityChoice, followChoice, defaultsButton})
            ShowWindow(h, SW_SHOW);
        EnableWindow(fpsChoice, !gifMode);
        EnableWindow(qualityChoice, !gifMode);
        EnableWindow(defaultsButton, !gifMode);
        SetWindowTextW(pauseButton, nskry::plugin_ui::Text(L"Start", L"开始"));
        SetWindowTextW(stopButton, nskry::plugin_ui::Text(L"Stop", L"结束"));
        EnableWindow(stopButton, FALSE);
        SetWindowTextW(discardButton, nskry::plugin_ui::Text(L"Cancel", L"取消"));
        SetWindowTextW(statusLabel, (std::wstring(gifMode ? L"GIF " : nskry::plugin_ui::Text(L"Video ", L"视频 ")) + std::to_wstring(r.right - r.left) +
                                     L" x " + std::to_wstring(r.bottom - r.top))
                                        .c_str());
        LayoutControl();
        Area(r, true);
    } catch (...) {
        Error(nskry::plugin_ui::Text(L"Could not start recording.", L"无法开始录制。"));
        if (window && !session)
            DestroyWindow(window);
    }
}
NSKRY_API int32_t NSKRY_CALL nskry_plugin_capture_action_v1(const NskryCaptureActionV1 *action) {
    if (!action || action->structSize < sizeof(*action) || action->version != 1 || !action->id ||
        wcscmp(action->id, L"gif") || !action->context)
        return 0;
    if (cleanupPending || session || setup || completed || exportTask) {
        if (window)
            SetForegroundWindow(window);
        LayoutControl();
        return 1;
    }
    requestingGif = true;
    nskry_plugin_execute(action->context);
    requestingGif = false;
    return 1;
}
NSKRY_API int32_t NSKRY_CALL nskry_plugin_show_settings_v1(HWND owner) {
    try {
        if (cleanupPending || session || setup || completed || exportTask) {
            if (window)
                SetForegroundWindow(window);
            return 1;
        }
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &mi))
            return 0;
        if (window)
            DestroyWindow(window);
        if (!ShowControl(mi.rcWork))
            return 0;
        KillTimer(window, Poll);
        setup = preferencesOnly = true;
        gifMode = false;
        const auto defaults = LoadDefaults();
        SendMessageW(fpsChoice, CB_SETCURSEL, defaults.fps == 60 ? 1 : 0, 0);
        SendMessageW(qualityChoice, CB_SETCURSEL, WPARAM(defaults.quality), 0);
        for (HWND h : {fpsChoice, qualityChoice, defaultsButton})
            ShowWindow(h, SW_SHOW);
        ShowWindow(pauseButton, SW_HIDE);
        ShowWindow(stopButton, SW_HIDE);
        SetWindowTextW(discardButton, nskry::plugin_ui::Text(L"Close", L"关闭"));
        SetWindowTextW(statusLabel, L"Default video frame rate (left) and bitrate quality (right).\nThese defaults do "
                                    L"not change GIF's fixed settings. Save defaults to apply.");
        LayoutControl();
        if (window)
            SetForegroundWindow(window);
        return 1;
    } catch (...) {
        return 0;
    }
}
NSKRY_API int32_t NSKRY_CALL nskry_plugin_command_v1(const NskryCommandV1 *request, NskryCommandResultV1 *result) {
    if (!request || request->structSize < sizeof(*request) || request->version != 1 || !request->id || !result ||
        result->structSize < sizeof(*result) || result->version != 1)
        return NSKRY_COMMAND_UNSUPPORTED;
    const bool stop = !wcscmp(request->id, L"stop");
    const bool pause = !wcscmp(request->id, L"pause_resume");
    if (!stop && !pause)
        return NSKRY_COMMAND_UNSUPPORTED;
    if (!session || !(stop ? session->Stop(false) : session->PauseResume())) {
        if (result->error && result->capacity)
            wcsncpy_s(result->error, result->capacity,
                      stop ? L"No recording is ready to stop." : L"No recording is ready to pause or resume.",
                      _TRUNCATE);
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
    previewTask.reset();
    previewFrame = {};
    if (exportTask) {
        exportTask->Shutdown();
        if (exportingTrim && exportTask->Status().success)
            DeleteFileW(exportPath.c_str());
        exportTask.reset();
    }
    ClearResult();
    setup = false;
    if (window)
        DestroyWindow(window);
    CloseCleanupWindow();
    UnregisterClassW(ClassName, module);
    UnregisterClassW(L"NskryRecordCleanupV1", module);
    UnregisterClassW(L"NskryRecordTrimRangeV1", module);
    if (areaWindow)
        DestroyWindow(areaWindow);
    areaWindow = nullptr;
    UnregisterClassW(L"NskryRecordAreaV1", module);
    service = {};
    serviceV2 = {};
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
void RecordControlTestEnableResult(const std::wstring &path) {
    resultPath = path;
    completed = copied = delivered = trimMode = gifMode = false;
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
bool RecordControlTestPreviewReady() {
    return !previewFrame.pixels.empty();
}
bool RecordControlTestPreviewAlive() {
    return bool(previewTask);
}
bool RecordControlTestCleanupPending() {
    return cleanupPending;
}
bool RecordControlTestExportAlive() {
    return bool(exportTask);
}
void RecordControlTestExportCopy(const std::wstring &input, const std::wstring &output) {
    exportTask = std::make_unique<ExportTask>();
    exportTask->Start(input, output, {}, false, 0, 0, false);
}
#endif

extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_set_ui_preferences_v1(const NskryUiPreferencesV1* ui) {
    nskry::plugin_ui::SetPreferences(ui);
}
