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
#include <string>
#include <windowsx.h>
#include <winrt/base.h>

namespace {
using namespace nskry::record;
NskryCaptureServiceV1 service{};
NskryCaptureServiceV2 serviceV2{};
std::unique_ptr<RecordSession> session;
HWND window{}, statusLabel{}, pauseButton{}, stopButton{}, discardButton{};
HWND fpsChoice{}, qualityChoice{}, followChoice{}, defaultsButton{}, trimButton{}, trimStart{}, trimEnd{}, trimApply{},
    trimCancel{};
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
HWND setupSource{};
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
                     L"0.4.0",
                     L"Nskry Team",
                     L"On-demand MP4/GIF region recording with optional window follow. No audio.",
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
    if (previewTask->Take(frame) && trimMode) {
        previewFrame = std::move(frame);
        previewFailed = !previewFrame.error.empty();
        InvalidateRect(window, nullptr, FALSE);
    }
    if (!previewTask->Done())
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
            L"Recording region", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, module, nullptr);
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
    const int mode = setup ? (preferencesOnly ? 4 : 0) : completed ? (trimMode ? 3 : 2) : 1;
    const auto bounds =
        RecordingBarBounds(setupRegion, mi.rcWork, mode == 0 || mode == 4 ? 900 : 800, mode == 3 ? 370 : 52);
    if (mode == layoutMode && EqualRect(&bounds, &layoutBounds))
        return;
    layoutMode = mode;
    layoutBounds = bounds;
    SetWindowPos(window, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOACTIVATE);
    RECT client{};
    GetClientRect(window, &client);
    const auto place = [](HWND h, bool visible, int x, int y, int w, int height = 30) {
        if (!h)
            return;
        SetWindowPos(h, nullptr, x, y, w, height, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(h, visible ? SW_SHOW : SW_HIDE);
    };
    const bool pre = mode == 0 || mode == 4;
    const int statusWidth = (std::max)(45L, client.right - (pre ? 570 : 420));
    place(statusLabel, true, 8, 10, statusWidth, 28);
    int x = 14 + statusWidth;
    if (pre) {
        place(fpsChoice, true, x, 8, 64, 180);
        x += 70;
        place(qualityChoice, true, x, 8, 84, 180);
        x += 90;
        place(followChoice, mode == 0, x, 8, 122);
        x += 128;
        place(defaultsButton, true, x, 8, 94);
        x += 100;
        place(pauseButton, mode == 0, x, 8, 64);
        x += 70;
        place(discardButton, true, x, 8, 72);
        place(stopButton, false, 0, 0, 1);
        place(trimButton, false, 0, 0, 1);
    } else {
        for (HWND h : {fpsChoice, qualityChoice, followChoice, defaultsButton})
            ShowWindow(h, SW_HIDE);
        place(pauseButton, true, x, 8, 100);
        x += 106;
        place(stopButton, true, x, 8, 100);
        x += 106;
        place(discardButton, true, x, 8, 110);
        x += 116;
        place(trimButton, completed, x, 8, 72);
    }
    place(rangeWindow, mode == 3, 12, 282, client.right - 24, 36);
    place(trimStart, mode == 3, 12, 328, 100);
    place(trimEnd, mode == 3, 124, 328, 100);
    place(trimApply, mode == 3, 252, 328, 104);
    place(trimCancel, mode == 3, 368, 328, 104);
    InvalidateRect(window, nullptr, FALSE);
}
void ResultControls() {
    SetWindowTextW(pauseButton, L"Save...");
    SetWindowTextW(stopButton, L"Copy file");
    SetWindowTextW(discardButton, delivered || copied ? L"Close" : L"Discard");
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
            Error(L"Enter start/end seconds inside the recording (start < end).");
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
            Error(L"Choose a new file with the correct extension. Existing files are never overwritten.");
            return;
        }
        exportPath = path;
    }
    auto task = std::make_unique<ExportTask>();
    task->Start(resultPath, exportPath, resultSnapshot, gifMode, exportStart, exportEnd, trim);
    exportTask = std::move(task);
    exportingTrim = trim;
    ResultControls();
    SetWindowTextW(statusLabel, trim ? L"Trimming / re-encoding... Cancel export leaves the original unchanged."
                                     : L"Saving... Cancel export leaves the original unchanged.");
    EnableWindow(discardButton, TRUE);
    SetWindowTextW(discardButton, L"Cancel export");
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
    const wchar_t *state = s.state == State::Starting     ? L"Starting"
                           : s.state == State::Recording  ? L"Recording"
                           : s.state == State::Paused     ? L"Paused"
                           : s.state == State::Stopping   ? L"Finalizing"
                           : s.state == State::Discarding ? L"Discarding"
                           : s.state == State::Publishing ? L"Publishing (cannot discard)"
                           : s.state == State::Recovering ? L"Recovering captured portion"
                                                          : L"Finished";
    const auto seconds = s.duration / 10000000;
    const auto rate = s.duration > 0 ? s.frames * 10000000 / uint64_t(s.duration) : 0;
    const auto text =
        (closePromptActive ? std::wstring(L"Close recording? Save, discard, or go back.\n") : std::wstring()) + state +
        L"  " + std::to_wstring(seconds / 60) + L":" + (seconds % 60 < 10 ? L"0" : L"") +
        std::to_wstring(seconds % 60) +
        (s.fps ? L"  (" + std::to_wstring(rate) + L" / " + std::to_wstring(s.fps) + L" fps)" : L"") + L"\n" +
        (finished ? s.message : s.backend);
    SetWindowTextW(statusLabel, text.c_str());
    if (closePromptActive) {
        // Confirmation stays INSIDE the capture-excluded control window. No
        // new top-level MessageBox which could leak into a full-screen recording.
        SetWindowTextW(pauseButton, finished ? L"Close" : L"Save / close");
        SetWindowTextW(stopButton, L"Discard / close");
        SetWindowTextW(discardButton, L"Back");
        EnableWindow(pauseButton, s.state != State::Recovering && s.state != State::Discarding);
        EnableWindow(stopButton, !finished && s.state != State::Publishing && s.state != State::Discarding);
        EnableWindow(discardButton, TRUE);
        return; // Keep the session alive while its inline confirmation is shown.
    }
    SetWindowTextW(pauseButton, s.state == State::Paused ? L"Resume" : L"Pause");
    SetWindowTextW(stopButton, L"Stop / save");
    EnableWindow(pauseButton, s.state == State::Recording || s.state == State::Paused);
    EnableWindow(stopButton, s.state == State::Starting || s.state == State::Recording || s.state == State::Paused);
    SetWindowTextW(discardButton, finished ? L"Close" : L"Cancel / discard");
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
            MessageBoxW(window, s.message.c_str(), L"Recording failed", MB_OK | MB_ICONWARNING);
            failurePromptActive = false;
        }
        if (closing)
            DestroyWindow(window);
    }
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
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
        FillRect(memory, &r, HBRUSH(COLOR_WINDOW + 1));
        if (trimMode) {
            RECT view{12, 48, r.right - 12, 272};
            auto dark = CreateSolidBrush(RGB(32, 33, 36));
            FillRect(memory, &view, dark);
            DeleteObject(dark);
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
                SetTextColor(memory, RGB(230, 230, 230));
                const auto *hint = previewFrame.error.empty() ? L"Drag the start/end handle to preview a frame"
                                                              : L"Preview unavailable; you can still trim by time";
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
    case WM_COMMAND:
        try {
            if (setup) {
                if (LOWORD(wp) == 3) {
                    setup = false;
                    DestroyWindow(hwnd);
                    return 0;
                }
                const Options o{SendMessageW(fpsChoice, CB_GETCURSEL, 0, 0) == 1 ? 60u : 30u,
                                Quality(SendMessageW(qualityChoice, CB_GETCURSEL, 0, 0))};
                if (LOWORD(wp) == 11) {
                    if (!SaveDefaults(o))
                        Error(L"Could not save recording defaults.");
                    else
                        SetWindowTextW(statusLabel, L"Default quality and frame rate saved.");
                    return 0;
                }
                if (LOWORD(wp) != 1)
                    return 0;
                if (preferencesOnly)
                    return 0;
                if (serviceV2.version != 2) {
                    Error(L"This version requires capture service v2.");
                    return 0;
                }
                SessionOptions options;
                options.fps = gifMode ? 10 : o.fps;
                options.quality = o.quality;
                options.gif = gifMode;
                options.deliveryCache = true;
                options.capture = serviceV2;
                options.followWindow =
                    SendMessageW(followChoice, BM_GETCHECK, 0, 0) == BST_CHECKED ? setupSource : nullptr;
                resultPath = NewRecordingPath(gifMode);
                auto recording = std::make_unique<RecordSession>(service, setupRegion, resultPath, true, options);
                if (!recording->Start()) {
                    ClearResult();
                    Error(L"Could not start recording.");
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
                        Error(L"The file could not be copied. Try again.");
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
            Error(L"Recording operation failed. The original file is unchanged.");
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
                MessageBoxW(hwnd, L"Discard this unsaved recording?", L"Recording", MB_YESNO | MB_ICONQUESTION) !=
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
        if (previewTask && previewTask->Done())
            previewTask.reset();
        previewFrame = {};
        KillTimer(hwnd, PreviewPoll);
        Area({}, false);
        if (areaWindow)
            DestroyWindow(areaWindow);
        areaWindow = nullptr;
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
    if (!monitor || !GetMonitorInfoW(monitor, &mi))
        return false;
    setupRegion = region;
    layoutMode = -1;
    const auto initial = RecordingBarBounds(region, mi.rcWork, 800, 52);
    window =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, ClassName, L"Nskry — Recording / settings",
                        WS_POPUP | WS_BORDER | WS_CLIPCHILDREN, initial.left, initial.top, initial.right - initial.left,
                        initial.bottom - initial.top, nullptr, nullptr, module, nullptr);
    if (!window)
        return false;
    // Refuse recording if the OS cannot exclude this control window. No black UI box in output.
    if (!SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) {
        DestroyWindow(window);
        return false;
    }
    statusLabel = CreateWindowExW(0, L"STATIC", L"Starting...", WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, 12, 10, 510, 60,
                                  window, nullptr, module, nullptr);
    pauseButton = CreateWindowExW(0, L"BUTTON", L"Pause", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 12, 78, 100, 28, window,
                                  HMENU(1), module, nullptr);
    stopButton = CreateWindowExW(0, L"BUTTON", L"Stop / save", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 124, 78, 115, 28,
                                 window, HMENU(2), module, nullptr);
    discardButton = CreateWindowExW(0, L"BUTTON", L"Cancel / discard", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 252, 78, 140,
                                    28, window, HMENU(3), module, nullptr);
    const auto child = [&](const wchar_t *type, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
        return CreateWindowExW(0, type, text, WS_CHILD | style, x, y, w, h, window, HMENU(INT_PTR(id)), module,
                               nullptr);
    };
    fpsChoice = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 12, 122, 100, 160, 7);
    qualityChoice = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 124, 122, 120, 160, 8);
    followChoice = child(L"BUTTON", L"Follow window", BS_AUTOCHECKBOX, 12, 168, 225, 28, 9);
    defaultsButton = child(L"BUTTON", L"Save defaults", WS_TABSTOP, 252, 122, 140, 28, 11);
    for (const auto *text : {L"30 fps", L"60 fps"})
        SendMessageW(fpsChoice, CB_ADDSTRING, 0, LPARAM(text));
    for (const auto *text : {L"中 / Medium", L"高 / High", L"高清 / HD"})
        SendMessageW(qualityChoice, CB_ADDSTRING, 0, LPARAM(text));
    trimButton = child(L"BUTTON", L"Trim...", WS_TABSTOP, 404, 78, 100, 28, 4);
    trimStart = child(L"EDIT", L"0", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 12, 130, 100, 28, 12);
    trimEnd = child(L"EDIT", L"0", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 124, 130, 120, 28, 13);
    trimApply = child(L"BUTTON", L"Apply trim", WS_TABSTOP, 252, 130, 100, 28, 5);
    trimCancel = child(L"BUTTON", L"Cancel trim", WS_TABSTOP, 364, 130, 100, 28, 6);
    WNDCLASSW rangeClass{};
    rangeClass.lpfnWndProc = RangeProcedure;
    rangeClass.hInstance = module;
    rangeClass.lpszClassName = L"NskryRecordTrimRangeV1";
    RegisterClassW(&rangeClass);
    rangeWindow = child(rangeClass.lpszClassName, L"", 0, 12, 282, 760, 36, 14);
    if (!statusLabel || !pauseButton || !stopButton || !discardButton || !fpsChoice || !qualityChoice ||
        !followChoice || !defaultsButton || !trimButton || !trimStart || !trimEnd || !trimApply || !trimCancel ||
        !rangeWindow) {
        DestroyWindow(window);
        return false;
    }
    closing = false;
    pendingClose = pendingDiscard = false;
    pendingExport = -1;
    LayoutControl();
    closePromptActive = false;
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
        if (session || setup || completed || exportTask || selectingOutput || failurePromptActive) {
            if (window)
                SetForegroundWindow(window);
            return;
        }
        gifMode = requestingGif;
        if (!ctx || ctx->structSize < sizeof(*ctx) || ctx->apiVersion != 2 || serviceV2.version != 2) {
            Error(L"This host does not provide region capture v2. Update the host and recording package together.");
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
        if (window)
            DestroyWindow(window);
        if (!ShowControl(r)) {
            Error(L"Cannot create a capture-excluded recording control window.");
            return;
        }
        KillTimer(window, Poll);
        setup = true;
        preferencesOnly = false;
        setupRegion = r;
        setupSource = ctx->sourceHwnd ? GetAncestor(ctx->sourceHwnd, GA_ROOT) : nullptr;
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
        SetWindowTextW(statusLabel,
                       gifMode ? L"GIF: fixed 10 fps / up to 960 x 540 pixels / up to 2 minutes.\nChoose window follow "
                                 L"(optional), then Start."
                               : L"Video settings: FPS (left), quality (right). Choose optional window follow.\nSave "
                                 L"defaults remembers these options; no encoder starts before Start.");
        SetWindowTextW(pauseButton, L"Start");
        SetWindowTextW(stopButton, L"Stop");
        EnableWindow(stopButton, FALSE);
        SetWindowTextW(discardButton, L"Cancel");
        SetWindowTextW(statusLabel, (std::wstring(gifMode ? L"GIF " : L"Video ") + std::to_wstring(r.right - r.left) +
                                     L" x " + std::to_wstring(r.bottom - r.top))
                                        .c_str());
        LayoutControl();
        Area(r, true);
    } catch (...) {
        Error(L"Could not start recording.");
        if (window && !session)
            DestroyWindow(window);
    }
}
NSKRY_API int32_t NSKRY_CALL nskry_plugin_capture_action_v1(const NskryCaptureActionV1 *action) {
    if (!action || action->structSize < sizeof(*action) || action->version != 1 || !action->id ||
        wcscmp(action->id, L"gif") || !action->context)
        return 0;
    if (session || setup || completed || exportTask) {
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
        if (session || setup || completed || exportTask) {
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
        SetWindowTextW(discardButton, L"Close");
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
    UnregisterClassW(ClassName, module);
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
#endif
