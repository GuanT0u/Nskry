#include "nskry_commands.h"
#include "nskry_plugin.h"
#include "record_session.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <iostream>
#include <winrt/base.h>
using namespace nskry::record;
// Internal seam exists only in this test's statically linked UI source.
HWND RecordControlTestStart(NskryCaptureServiceV1, RECT, const std::wstring &);
bool RecordControlTestAlive();
Snapshot RecordControlTestStatus();
void RecordControlTestHoldPrompt(bool);
void RecordControlTestUpdate();
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
    const auto root = std::filesystem::path(argv[1]) / (L"record-control-" + std::to_wstring(GetCurrentProcessId()) +
                                                        L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(root);
    NskryCaptureServiceV1 capture{sizeof(capture), 1, Open, Read, Close};
    HWND hwnd = RecordControlTestStart(capture, {0, 0, 160, 90}, (root / L"save.mp4").wstring());
    bool ok = hwnd && Pump([] { return RecordControlTestStatus().state == State::Recording; });
    DWORD affinity{};
    ok = GetWindowDisplayAffinity(hwnd, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE && ok;
    SendMessageW(hwnd, WM_COMMAND, 1, 0);
    ok = Pump([] { return !captures; }) && RecordControlTestStatus().state == State::Paused && ok;
    SendMessageW(hwnd, WM_COMMAND, 1, 0);
    ok = Pump([] { return captures == 1; }) && ok;
    const auto until = GetTickCount64() + 200;
    Pump([&] { return GetTickCount64() >= until; });
    // Emulate the nested close prompt's message loop while a global Stop completes.
    RecordControlTestHoldPrompt(true);
    SendMessageW(hwnd, WM_COMMAND, 2, 0);
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
    hwnd = RecordControlTestStart(capture, {0, 0, 160, 90}, (root / L"discard.mp4").wstring());
    ok = hwnd && Pump([] { return RecordControlTestStatus().state == State::Recording; }) && ok;
    SendMessageW(hwnd, WM_COMMAND, 3, 0);
    ok = Pump([&] { return !IsWindow(hwnd); }) && !RecordControlTestAlive() && !captures &&
         !std::filesystem::exists(root / L"discard.mp4") && ok;
    nskry_plugin_shutdown();
    for (const auto &file : std::filesystem::directory_iterator(root))
        ok = !file.path().filename().wstring().starts_with(L".nskry-record-") && ok;
    winrt::uninit_apartment();
    std::cout
        << (ok ? "Record control UI pause/resume/save/discard, capture exclusion and nested-close lifetime passed.\n"
               : "Record control UI regression failed.\n");
    return ok ? 0 : 1;
}
