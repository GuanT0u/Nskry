#include "record_session.h"
#include "test_artifacts.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <dxgi.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <psapi.h>
#include <winrt/base.h>

using namespace nskry::record;
namespace {
std::atomic<int> active{}, reads{}, delay{}, failAfter{-1}, violations{};
std::atomic<int> closeDelay{};
std::atomic<bool> inRead{}, noFrames{};
std::atomic<bool> scratchOnly{};
std::atomic<bool> badReopen{};
struct Fake {
    DWORD thread = GetCurrentThreadId();
};
HRESULT __cdecl Open(RECT, uint32_t, void **result, NskryCaptureDescriptionV1 *d, wchar_t *, uint32_t) {
    *result = new Fake;
    ++active;
    *d = {sizeof(*d), 1, 320, 180, 1280};
    if (badReopen)
        d->stride += 4;
    return S_OK;
}
HRESULT __cdecl Read(void *handle, uint8_t *p, uint32_t capacity) {
    inRead = true;
    struct Guard {
        ~Guard() {
            inRead = false;
        }
    } guard;
    if (!handle || static_cast<Fake *>(handle)->thread != GetCurrentThreadId())
        ++violations;
    if (capacity != 320 * 180 * 4)
        ++violations;
    if (delay.load())
        Sleep(delay.load());
    const int n = ++reads;
    if (noFrames)
        return S_FALSE;
    if (scratchOnly) {
        std::memset(p, 255, capacity);
        return S_FALSE;
    }
    if (failAfter >= 0 && n > failAfter)
        return DXGI_ERROR_DEVICE_REMOVED;
    for (unsigned i = 0; i < capacity; i += 4) {
        p[i] = 20;
        p[i + 1] = BYTE(n);
        p[i + 2] = 170;
        p[i + 3] = 255;
    }
    return S_OK;
}
void __cdecl Close(void *handle) {
    if (closeDelay)
        Sleep(closeDelay.load());
    if (static_cast<Fake *>(handle)->thread != GetCurrentThreadId())
        ++violations;
    delete static_cast<Fake *>(handle);
    --active;
}
NskryCaptureServiceV1 service{sizeof(service), 1, Open, Read, Close};
bool Check(bool b, const char *message) {
    if (!b)
        std::cerr << message << '\n';
    return b;
}
bool Pump(const std::function<bool()> &predicate, DWORD timeout = 8000) {
    const auto end = GetTickCount64() + timeout;
    while (!predicate() && GetTickCount64() < end) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG m{};
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return predicate();
}
bool Finished(RecordSession &s) {
    auto st = s.Status().state;
    return st == State::Done || st == State::Failed;
}
int64_t Duration(const std::filesystem::path &path, bool decode = false, bool expectRed = false) {
    winrt::check_hresult(MFStartup(MF_VERSION));
    int64_t value{};
    {
        winrt::com_ptr<IMFSourceReader> reader;
        winrt::com_ptr<IMFAttributes> attributes;
        MFCreateAttributes(attributes.put(), 1);
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        winrt::check_hresult(MFCreateSourceReaderFromURL(path.c_str(), attributes.get(), reader.put()));
        PROPVARIANT v{};
        winrt::check_hresult(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v));
        value = v.uhVal.QuadPart;
        PropVariantClear(&v);
        if (decode) {
            winrt::com_ptr<IMFMediaType> type;
            MFCreateMediaType(type.put());
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            winrt::check_hresult(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.get()));
            unsigned frames = 0;
            for (unsigned safety = 0; safety < 1000; ++safety) {
                DWORD flags{};
                winrt::com_ptr<IMFSample> sample;
                winrt::check_hresult(
                    reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr, sample.put()));
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                    break;
                if (sample) {
                    if (expectRed) {
                        winrt::com_ptr<IMFMediaBuffer> buffer;
                        sample->ConvertToContiguousBuffer(buffer.put());
                        BYTE *p{};
                        DWORD size{};
                        buffer->Lock(&p, nullptr, &size);
                        const bool red =
                            size >= 320 * 180 * 4 && p[(90 * 320 + 160) * 4 + 2] > p[(90 * 320 + 160) * 4 + 1] + 50;
                        buffer->Unlock();
                        if (!red)
                            winrt::throw_hresult(E_FAIL);
                    }
                    ++frames;
                }
            }
            if (!frames)
                winrt::throw_hresult(E_FAIL);
            std::cout << "Recovered RGB frames=" << frames << '\n';
        }
    }
    MFShutdown();
    return value;
}
uint64_t Cpu() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = k.dwLowDateTime;
    a.HighPart = k.dwHighDateTime;
    b.LowPart = u.dwLowDateTime;
    b.HighPart = u.dwHighDateTime;
    return a.QuadPart + b.QuadPart;
}
int ticks{};
LRESULT CALLBACK WindowProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER) {
        ++ticks;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const nskry::test::Artifacts artifacts(argv[1], L"record-session");
    const auto &root = artifacts.Path();
    WNDCLASSW wc{};
    wc.lpszClassName = L"RecordSessionTestOwned";
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    HWND ui =
        CreateWindowExW(0, wc.lpszClassName, L"Test only", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    SetTimer(ui, 1, 10, nullptr);
    bool ok = true;
    try {
        const auto path = root / L"pause.mp4";
        const auto before = Cpu();
        RecordSession s(service, {0, 0, 320, 180}, path.wstring());
        ok &= Check(s.Start() && !s.Start(), "duplicate start refused");
        ok &= Check(Pump([&] { return s.Status().state == State::Recording; }), "recording starts");
        const auto start = GetTickCount64();
        ok &= Check(Pump([&] { return GetTickCount64() - start > 350; }), "UI pumping");
        ok &= Check(s.PauseResume() && Pump([] { return active == 0; }), "pause closes capture");
        const auto paused = s.Status().duration;
        const int readCount = reads;
        const auto pauseStart = GetTickCount64();
        const auto pauseCpu = Cpu();
        Pump([&] { return GetTickCount64() - pauseStart > 650; });
        const auto pauseCpuUsed = Cpu() - pauseCpu;
        ok &= Check(s.Status().duration == paused && reads == readCount && !active,
                    "pause neither advances time nor captures");
        ok &= Check(s.PauseResume() && Pump([] { return active == 1; }), "resume rebuilds capture");
        for (int i = 0; i < 10; ++i) {
            ok &= Check(s.PauseResume() && s.PauseResume(), "rapid toggles");
        }
        const auto resume = GetTickCount64();
        Pump([&] { return GetTickCount64() - resume > 400; });
        ok &=
            Check(s.Stop(false) && !s.Stop(false) && Pump([&] { return Finished(s); }), "stop is async and idempotent");
        const auto report = s.Status();
        std::wcout << report.message << L"\nBackend=" << report.backend << L"\n";
        ok &= Check(report.state == State::Done && !active, "stop releases capture before Done");
        if (std::filesystem::exists(path)) {
            const auto actual = Duration(path);
            std::cout << "duration100ns=" << actual << " expected=" << report.duration << " frames=" << report.frames
                      << " skipped=" << report.skipped << '\n';
            ok &= Check(std::abs(actual - report.duration) < 100000, "pause MP4 duration matches active timeline");
        } else
            ok = false;
        std::cout << "CPU_ms=" << (Cpu() - before) / 10000 << " pausedCPU_ms=" << pauseCpuUsed / 10000
                  << " UI_ticks=" << ticks << '\n';
        ok &= Check(ticks > 20 && report.duration > 6000000 && report.duration < 13000000,
                    "UI remains responsive and pause excluded");
        for (const auto name : {L"cancel", L"shutdown", L"early"}) {
            RecordSession canceled(service, {0, 0, 320, 180}, (root / (std::wstring(name) + L".mp4")).wstring());
            canceled.Start();
            if (wcscmp(name, L"early"))
                ok &=
                    Check(Pump([&] { return canceled.Status().state == State::Recording; }), "cancel scenario starts");
            if (!wcscmp(name, L"shutdown"))
                canceled.Shutdown();
            else {
                canceled.Stop(true);
                ok &= Check(Pump([&] { return Finished(canceled); }), "cancel completes");
            }
            ok &= Check(!active && !std::filesystem::exists(root / (std::wstring(name) + L".mp4")),
                        "cancel never publishes output");
        }
        noFrames = true;
        {
            const int priorReads = reads;
            RecordSession empty(service, {0, 0, 320, 180}, (root / L"empty.mp4").wstring());
            empty.Start();
            ok &= Check(Pump([&] { return reads > priorReads; }), "empty capture waiting for first frame");
            empty.Stop(false);
            ok &= Check(Pump([&] { return Finished(empty); }) && empty.Status().state == State::Done &&
                            !std::filesystem::exists(root / L"empty.mp4"),
                        "early stop cleans empty output without false save");
        }
        noFrames = false;
        {
            delay = 120;
            RecordSession race(service, {0, 0, 320, 180}, (root / L"race.mp4").wstring());
            race.Start();
            ok &= Check(Pump([&] { return race.Status().state == State::Recording && inRead.load(); }),
                        "blocked frame for stop/close race");
            ok &= Check(race.Stop(false) && race.Stop(true), "close/discard overrides pending stop before publish");
            ok &= Check(Pump([&] { return Finished(race); }) && race.Status().state == State::Done && !active &&
                            !std::filesystem::exists(root / L"race.mp4"),
                        "stop/close race does not publish canceled output");
        }
        for (const auto &file : std::filesystem::directory_iterator(root))
            ok &= Check(!file.path().filename().wstring().starts_with(L".nskry-record-"),
                        "successful cancel deletes owned temporary data");
        delay = 120;
        reads = 0;
        RecordSession slow(service, {0, 0, 320, 180}, (root / L"slow.mp4").wstring());
        slow.Start();
        ok &= Check(Pump([&] { return slow.Status().frames >= 4 || Finished(slow); }), "slow backend progresses");
        slow.Stop(false);
        ok &= Check(Pump([&] { return Finished(slow); }), "slow stop");
        ok &= Check(slow.Status().frames < 15 && slow.Status().skipped > 0,
                    "backpressure skips work without queued catch-up");
        if (std::filesystem::exists(root / L"slow.mp4"))
            ok &= Check(std::abs(Duration(root / L"slow.mp4") - slow.Status().duration) < 100000,
                        "slow encoder preserves wall-time duration");
        delay = 0;
        {
            RecordSession still(service, {0, 0, 320, 180}, (root / L"static.mp4").wstring());
            still.Start();
            ok &= Check(Pump([&] { return still.Status().state == State::Recording; }), "static frame starts");
            scratchOnly = true;
            const auto until = GetTickCount64() + 300;
            Pump([&] { return GetTickCount64() >= until; });
            still.Stop(false);
            ok &= Check(Pump([&] { return Finished(still); }) && still.Status().state == State::Done,
                        "S_FALSE repeats valid pending frame, ignores scratch pixels");
            scratchOnly = false;
            if (still.Status().state == State::Done)
                ok &= Check(Duration(root / L"static.mp4", true, true) > 0,
                            "static repeated frames decode with correct pixels");
        }
        {
            RecordSession resized(service, {0, 0, 320, 180}, (root / L"bad-resume.mp4").wstring());
            resized.Start();
            ok &= Check(Pump([&] { return resized.Status().frames >= 2; }), "frames before invalid resume");
            ok &= Check(resized.PauseResume() && Pump([] { return active == 0; }), "pause before descriptor change");
            badReopen = true;
            ok &= Check(resized.PauseResume() && Pump([&] { return Finished(resized); }) &&
                            resized.Status().state == State::Failed && !active,
                        "changed resume stride rejected before reading into fixed buffers");
            badReopen = false;
        }
        reads = 0;
        failAfter = 3;
        RecordSession failed(service, {0, 0, 320, 180}, (root / L"failed.mp4").wstring());
        failed.Start();
        ok &= Check(Pump([&] { return Finished(failed); }) && failed.Status().state == State::Failed && !active,
                    "capture/device failure cleanup");
        const auto recovery = failed.Status();
        ok &= Check(recovery.recoveryFinalized && !recovery.recoveryPath.empty() &&
                        !std::filesystem::exists(root / L"failed.mp4"),
                    "capture failure retains a finalized portion, not a false full success");
        if (recovery.recoveryFinalized) {
            const auto duration = Duration(recovery.recoveryPath, true);
            ok &= Check(duration > 0 && std::abs(duration - recovery.encodedDuration) < 100000,
                        "recovered MP4 duration matches only successfully encoded frames");
            std::cout << "Recovered duration100ns=" << duration << " encoded=" << recovery.encodedDuration << '\n';
        }
        failAfter = -1;
        {
            const auto folder = root / L"中文 output path";
            std::filesystem::create_directory(folder);
            const auto collision = folder / L"record.mp4";
            RecordSession publish(service, {0, 0, 320, 180}, collision.wstring());
            publish.Start();
            ok &= Check(Pump([&] { return publish.Status().frames >= 2; }), "recording before target collision");
            const char marker[] = "untouched";
            HANDLE file =
                CreateFileW(collision.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD written{};
            ok &= Check(file != INVALID_HANDLE_VALUE, "test-owned collision file created");
            if (file != INVALID_HANDLE_VALUE) {
                WriteFile(file, marker, sizeof(marker), &written, nullptr);
                CloseHandle(file);
            }
            publish.Stop(false);
            ok &= Check(Pump([&] { return Finished(publish); }), "publish collision completes");
            const auto result = publish.Status();
            std::ifstream input(collision, std::ios::binary);
            std::string contents((std::istreambuf_iterator<char>(input)), {});
            ok &= Check(contents == std::string(marker, sizeof(marker)) && result.state == State::Failed &&
                            result.recoveryFinalized,
                        "target preserved, finalized partial correctly reported after rename failure");
            if (result.recoveryFinalized)
                ok &= Check(Duration(result.recoveryPath, true) > 0, "collision recovery is playable");
        }
        {
            reads = 0;
            failAfter = 3;
            closeDelay = 200;
            RecordSession canceledRecovery(service, {0, 0, 320, 180}, (root / L"cancel-recovery.mp4").wstring());
            canceledRecovery.Start();
            ok &= Check(Pump([&] { return canceledRecovery.Status().state == State::Recovering; }),
                        "recovery freezes clock before cleanup");
            ok &= Check(!canceledRecovery.PauseResume() && canceledRecovery.Stop(true),
                        "recovery remains cancelable, cannot resume");
            ok &= Check(Pump([&] { return Finished(canceledRecovery); }) &&
                            canceledRecovery.Status().state == State::Done &&
                            canceledRecovery.Status().recoveryPath.empty() &&
                            !std::filesystem::exists(root / L"cancel-recovery.mp4") && !active,
                        "cancel during failure cleanup succeeds without retained data");
            closeDelay = 0;
            failAfter = -1;
        }
        RecordSession badPath(service, {0, 0, 320, 180}, (root / L"missing" / L"output.mp4").wstring());
        badPath.Start();
        ok &= Check(Pump([&] { return Finished(badPath); }) && badPath.Status().state == State::Failed && !active,
                    "output failure cleanup");
        for (const auto &file : std::filesystem::directory_iterator(root)) {
            if (file.path().filename().wstring().starts_with(L".nskry-record-")) {
                // Only the deliberately failed encoder/capture run may retain partial data.
                std::wcout << L"Failure trace: " << file.path().c_str() << L"\n";
            }
        }
        ok &= Check(!violations, "capture functions stay on their owning recording worker");
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory));
        std::cout << "process_peak_workingset=" << memory.PeakWorkingSetSize << " final_private=" << memory.PrivateUsage
                  << '\n';
    } catch (const winrt::hresult_error &e) {
        std::cerr << std::hex << e.code().value << '\n';
        ok = false;
    }
    KillTimer(ui, 1);
    DestroyWindow(ui);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    winrt::uninit_apartment();
    ok = artifacts.Finish(ok) && ok;
    return ok ? 0 : 1;
}
