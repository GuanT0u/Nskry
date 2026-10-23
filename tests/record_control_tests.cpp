#include "nskry_actions.h"
#include "nskry_commands.h"
#include "nskry_plugin.h"
#include "record_session.h"
#include "test_artifacts.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <iostream>
#include <winrt/base.h>
using namespace nskry::record;
extern "C" int32_t NSKRY_CALL nskry_plugin_command_v1(const NskryCommandV1 *, NskryCommandResultV1 *);
// Internal seam exists only in this test's statically linked UI source.
HWND RecordControlTestStart(NskryCaptureServiceV1, RECT, const std::wstring &);
bool RecordControlTestAlive();
Snapshot RecordControlTestStatus();
void RecordControlTestHoldPrompt(bool);
void RecordControlTestUpdate();
void RecordControlTestEnableResult(const std::wstring &);
bool RecordControlTestPreviewReady();
bool RecordControlTestPreviewAlive();
extern "C" int32_t NSKRY_CALL nskry_plugin_connect_capture_v2(const NskryCaptureServiceV2 *);
extern "C" int32_t NSKRY_CALL nskry_plugin_capture_action_v1(const NskryCaptureActionV1 *);
extern "C" int32_t NSKRY_CALL nskry_plugin_show_settings_v1(HWND);
namespace {
std::atomic<int> captures{};
HRESULT __cdecl Open(RECT, uint32_t, void **value, NskryCaptureDescriptionV1 *d, wchar_t *, uint32_t) {
    *value = new int(1);
    *d = {sizeof(*d), 1, 160, 90, 640};
    ++captures;
    return S_OK;
}
HRESULT __cdecl Read(void *, uint8_t *pixels, uint32_t count) {
    memset(pixels, 100, count);
    return S_OK;
}
void __cdecl Close(void *value) {
    delete static_cast<int *>(value);
    --captures;
}
HRESULT __cdecl OpenV2(const NskryCaptureRequestV2 *r, void **v, NskryCaptureDescriptionV1 *d, wchar_t *e, uint32_t n) {
    return Open(r->physicalRegion, r->fps, v, d, e, n);
}
HRESULT __cdecl Bounds(void *, RECT *r) {
    *r = {0, 0, 160, 90};
    return S_OK;
}
HWND OwnControl() {
    HWND result{};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM p) -> BOOL {
            wchar_t name[128]{};
            GetClassNameW(h, name, 128);
            if (!wcscmp(name, L"NskryRecordControlV1"))
                *reinterpret_cast<HWND *>(p) = h;
            return TRUE;
        },
        LPARAM(&result));
    return result;
}
bool Pump(const std::function<bool()> &done) {
    const auto end = GetTickCount64() + 8000;
    while (!done() && GetTickCount64() < end) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG m{};
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return done();
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    const nskry::test::Artifacts artifacts(argv[1], L"record-control");
    const auto &root = artifacts.Path();
    NskryCaptureServiceV1 capture{sizeof(capture), 1, Open, Read, Close};
    HWND hwnd = RecordControlTestStart(capture, {0, 0, 160, 90}, (root / L"save.mp4").wstring());
    bool ok = hwnd && Pump([] { return RecordControlTestStatus().state == State::Recording; });
    DWORD affinity{};
    ok = GetWindowDisplayAffinity(hwnd, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE && ok;
    wchar_t commandError[256]{};
    NskryCommandV1 pauseCommand{sizeof(pauseCommand), 1, L"pause_resume"};
    NskryCommandResultV1 pauseResult{sizeof(pauseResult), 1, commandError, 256};
    ok = nskry_plugin_command_v1(&pauseCommand, &pauseResult) == NSKRY_COMMAND_OK && ok;
    ok = Pump([] { return !captures; }) && RecordControlTestStatus().state == State::Paused && ok;
    ok = nskry_plugin_command_v1(&pauseCommand, &pauseResult) == NSKRY_COMMAND_OK && ok;
    ok = Pump([] { return captures == 1; }) && ok;
    const auto until = GetTickCount64() + 200;
    Pump([&] { return GetTickCount64() >= until; });
    // A global Stop may complete while inline close confirmation remains open.
    RecordControlTestHoldPrompt(true);
    NskryCommandV1 stopCommand{sizeof(stopCommand), 1, L"stop"};
    ok = nskry_plugin_command_v1(&stopCommand, &pauseResult) == NSKRY_COMMAND_OK && ok;
    ok = Pump([] { return RecordControlTestStatus().state == State::Done; }) && ok;
    RecordControlTestUpdate();
    ok = RecordControlTestAlive() && !captures && IsWindow(hwnd) && ok;
    SendMessageW(hwnd, WM_CLOSE, 0, 0); // recursive close during confirmation must be ignored
    ok = RecordControlTestAlive() && IsWindow(hwnd) && ok;
    RecordControlTestHoldPrompt(false);
    RecordControlTestUpdate();
    ok = !RecordControlTestAlive() && std::filesystem::exists(root / L"save.mp4") && ok;
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    ok = !IsWindow(hwnd) && ok;
    for (const auto save : {true, false}) {
        const auto path = root / (save ? L"inline-save.mp4" : L"inline-discard.mp4");
        hwnd = RecordControlTestStart(capture, {0, 0, 160, 90}, path.wstring());
        ok = hwnd && Pump([] { return RecordControlTestStatus().frames >= 2; }) && ok;
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
        wchar_t label[64]{};
        GetWindowTextW(GetDlgItem(hwnd, 1), label, 64);
        ok = !wcscmp(label, L"Save / close") && RecordControlTestAlive() && ok;
        SendMessageW(hwnd, WM_COMMAND, 3, 0); // Back preserves the live recording.
        GetWindowTextW(GetDlgItem(hwnd, 2), label, 64);
        ok = !wcscmp(label, L"Stop / save") && ok;
        ok = RecordControlTestStatus().state == State::Recording && ok;
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
        SendMessageW(hwnd, WM_COMMAND, save ? 1 : 2, 0);
        ok = Pump([&] { return !IsWindow(hwnd); }) && !RecordControlTestAlive() && !captures &&
             std::filesystem::exists(path) == save && ok;
    }
    hwnd = RecordControlTestStart(capture, {0, 0, 160, 90}, (root / L"discard.mp4").wstring());
    ok = hwnd && Pump([] { return RecordControlTestStatus().state == State::Recording; }) && ok;
    SendMessageW(hwnd, WM_COMMAND, 3, 0);
    ok = Pump([&] { return !IsWindow(hwnd); }) && !RecordControlTestAlive() && !captures &&
         !std::filesystem::exists(root / L"discard.mp4") && ok;
    for (const auto message : {WM_DISPLAYCHANGE, WM_POWERBROADCAST}) {
        const auto path = root / (message == WM_DISPLAYCHANGE ? L"display-change.mp4" : L"suspend.mp4");
        hwnd = RecordControlTestStart(capture, {0, 0, 160, 90}, path.wstring());
        ok = hwnd && Pump([] { return RecordControlTestStatus().frames >= 2; }) && ok;
        SendMessageW(hwnd, message, message == WM_POWERBROADCAST ? PBT_APMSUSPEND : 0, 0);
        ok = Pump([] { return !RecordControlTestAlive(); }) && !captures && std::filesystem::exists(path) && ok;
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    const auto postPath = root / L"post-result.mp4";
    hwnd = RecordControlTestStart(capture, {0, 0, 160, 90}, postPath.wstring());
    RecordControlTestEnableResult(postPath.wstring());
    ok = hwnd && Pump([] { return RecordControlTestStatus().frames >= 2; }) && ok;
    SendMessageW(hwnd, WM_COMMAND, 2, 0);
    ok = Pump([] { return !RecordControlTestAlive(); }) && std::filesystem::exists(postPath) && ok;
    wchar_t resultLabel[64]{};
    GetWindowTextW(GetDlgItem(hwnd, 1), resultLabel, 64);
    ok = !wcscmp(resultLabel, L"Save...") && IsWindowVisible(GetDlgItem(hwnd, 4)) && ok;
    SendMessageW(hwnd, WM_COMMAND, 4, 0);
    ok = IsWindowVisible(GetDlgItem(hwnd, 12)) && IsWindowVisible(GetDlgItem(hwnd, 13)) && ok;
    HWND range = GetDlgItem(hwnd, 14);
    RECT track{};
    GetClientRect(range, &track);
    SendMessageW(range, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(16, 16));
    SendMessageW(range, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(track.right / 4, 16));
    SendMessageW(range, WM_LBUTTONUP, 0, MAKELPARAM(track.right / 4, 16));
    // Start/release another gesture before the previous decoder has drained.
    SendMessageW(range, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(track.right / 4, 16));
    SendMessageW(range, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(track.right / 3, 16));
    SendMessageW(range, WM_LBUTTONUP, 0, MAKELPARAM(track.right / 3, 16));
    const bool previewOk = Pump([] { return RecordControlTestPreviewReady() && !RecordControlTestPreviewAlive(); });
    std::cout << "Timeline preview and idle cleanup=" << previewOk << '\n';
    ok = previewOk && ok;
    SendMessageW(hwnd, WM_COMMAND, 6, 0);
    ok = !IsWindowVisible(GetDlgItem(hwnd, 12)) && !IsWindowVisible(GetDlgItem(hwnd, 13)) && ok;
    ok = Pump([] { return !RecordControlTestPreviewAlive(); }) && !RecordControlTestPreviewReady() && ok;
    SendMessageW(hwnd, WM_COMMAND, 3, 0); // Explicit discard, no native modal dialog.
    ok = !IsWindow(hwnd) && !std::filesystem::exists(postPath) && ok;
    NskryCaptureServiceV2 capture2{sizeof(capture2), 2, OpenV2, Read, Bounds, Close};
    ok = nskry_plugin_connect_capture_v2(&capture2) != 0 && ok;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    NskryHostContext context{sizeof(context), 2};
    context.capturedRegion = {mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.left + 160, mi.rcMonitor.top + 90};
    nskry_plugin_execute(&context);
    hwnd = OwnControl();
    ok = hwnd && SendMessageW(GetDlgItem(hwnd, 7), CB_GETCOUNT, 0, 0) == 2 &&
         SendMessageW(GetDlgItem(hwnd, 8), CB_GETCOUNT, 0, 0) == 3 && !captures && ok;
    RECT bar{};
    GetWindowRect(hwnd, &bar);
    bool outline = false;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM value) -> BOOL {
            wchar_t name[128]{};
            GetClassNameW(h, name, 128);
            if (!wcscmp(name, L"NskryRecordAreaV1") && IsWindowVisible(h))
                *reinterpret_cast<bool *>(value) = true;
            return TRUE;
        },
        LPARAM(&outline));
    const bool setupOk = outline && (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) != WS_CAPTION &&
                         bar.top >= context.capturedRegion.bottom;
    std::cout << "Preflight outline and horizontal bar=" << setupOk << '\n';
    ok = setupOk && ok;
    SendMessageW(hwnd, WM_COMMAND, 3, 0);
    NskryCaptureActionV1 gif{sizeof(gif), 1, L"gif", &context};
    ok = nskry_plugin_capture_action_v1(&gif) != 0 && ok;
    hwnd = OwnControl();
    ok = hwnd && !IsWindowEnabled(GetDlgItem(hwnd, 7)) && !IsWindowEnabled(GetDlgItem(hwnd, 8)) && !captures && ok;
    SendMessageW(hwnd, WM_COMMAND, 3, 0);
    ok = nskry_plugin_show_settings_v1(nullptr) != 0 && ok;
    hwnd = OwnControl();
    ok = hwnd && IsWindowVisible(GetDlgItem(hwnd, 15)) && !IsWindowVisible(GetDlgItem(hwnd, 9)) && !captures && ok;
    // Do not invoke Explorer or touch the user's recording cache in this regression.
    SendMessageW(hwnd, WM_COMMAND, 3, 0);
    nskry_plugin_shutdown();
    for (const auto &file : std::filesystem::directory_iterator(root))
        ok = !file.path().filename().wstring().starts_with(L".nskry-record-") && ok;
    winrt::uninit_apartment();
    ok = artifacts.Finish(ok) && ok;
    std::cout
        << (ok ? "Record control UI pause/resume/save/discard, capture exclusion and inline-close lifetime passed.\n"
               : "Record control UI regression failed.\n");
    return ok ? 0 : 1;
}
