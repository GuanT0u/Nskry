#include "capture/region_stream.h"
#include "pch.h"
#include "record_session.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <psapi.h>

using namespace nskry::record;
namespace {
COLORREF color = RGB(30, 170, 50);
int ticks{};
LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(hwnd, &ps);
        RECT r{};
        GetClientRect(hwnd, &r);
        auto b = CreateSolidBrush(GetWindowLongPtrW(hwnd, GWLP_USERDATA) ? RGB(220, 30, 220) : color);
        FillRect(dc, &r, b);
        DeleteObject(b);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_TIMER) {
        ++ticks;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
bool Pump(const std::function<bool()> &done, DWORD timeout = 12000) {
    auto end = GetTickCount64() + timeout;
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
bool Finished(RecordSession &s) {
    auto state = s.Status().state;
    return state == State::Done || state == State::Failed;
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
void Memory(const char *name) {
    PROCESS_MEMORY_COUNTERS_EX m{};
    m.cb = sizeof(m);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m), sizeof(m));
    DWORD handles{};
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    std::cout << name << " private=" << m.PrivateUsage << " working=" << m.WorkingSetSize
              << " peak=" << m.PeakWorkingSetSize << " handles=" << handles << '\n';
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    MEMORYSTATUSEX memory{sizeof(memory)};
    GlobalMemoryStatusEx(&memory);
    wchar_t cpu[256]{};
    DWORD bytes = sizeof(cpu);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                 RRF_RT_REG_SZ, nullptr, cpu, &bytes);
    std::wcout << L"CPU=" << cpu << L" logical=" << system.dwNumberOfProcessors << L" RAM=" << memory.ullTotalPhys
               << L"\n";
    winrt::com_ptr<IDXGIFactory1> factory;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void());
    if (factory)
        for (UINT i = 0; i < 8; ++i) {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            if (FAILED(factory->EnumAdapters1(i, adapter.put())))
                break;
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            std::wcout << L"GPU=" << desc.Description << L"\n";
        }
    WNDCLASSW wc{};
    wc.lpszClassName = L"NskryRecordOwnedCaptureTest";
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT region{mi.rcWork.left + 40, mi.rcWork.top + 40, mi.rcWork.left + 360, mi.rcWork.top + 220};
    HWND source = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                                  L"Test-owned capture pixels", WS_POPUP, region.left, region.top, 320, 180, nullptr,
                                  nullptr, wc.hInstance, nullptr);
    ShowWindow(source, SW_SHOWNOACTIVATE);
    UpdateWindow(source);
    SetTimer(source, 1, 40, nullptr);
    // Only test-owned pixels are persisted, never a user's application or whole monitor.
    HWND excluded =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"EXCLUDED", WS_POPUP,
                        region.left + 20, region.top + 20, 80, 40, nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(excluded, GWLP_USERDATA, 1);
    bool ok = source && excluded && SetWindowDisplayAffinity(excluded, WDA_EXCLUDEFROMCAPTURE);
    ShowWindow(excluded, SW_SHOWNOACTIVATE);
    UpdateWindow(excluded);
    const auto path =
        std::filesystem::path(argv[1]) / (L"record-wgc-" + std::to_wstring(GetCurrentProcessId()) + L".mp4");
    try {
        auto service = nskry::RegionCaptureService();
        void *invalid{};
        NskryCaptureDescriptionV1 desc{sizeof(desc), 1};
        wchar_t error[512]{};
        ok = ok &&
             FAILED(service.open({region.left, region.top, mi.rcMonitor.right + 10, region.bottom}, 20, &invalid, &desc,
                                 error, 512)) &&
             !invalid;
        Memory("idle");
        const auto start = GetTickCount64();
        const auto cpuStart = Cpu();
        SessionOptions fixed;
        fixed.capture = nskry::RegionCaptureServiceV2();
        fixed.fps = 30;
        RecordSession recording(service, region, path.wstring(), true, fixed);
        ok = recording.Start() && ok;
        ok = Pump([&] { return recording.Status().state == State::Recording || Finished(recording); }) && ok;
        if (Finished(recording))
            std::wcout << recording.Status().message << L"\n";
        const auto beforePause = GetTickCount64();
        Pump([&] { return GetTickCount64() - beforePause > 700; });
        Memory("recording");
        ok = recording.PauseResume() && ok;
        const auto pauseStart = GetTickCount64();
        const auto pausedCpu = Cpu();
        Pump([&] { return GetTickCount64() - pauseStart > 650; });
        Memory("paused");
        const auto pauseCpuUsed = Cpu() - pausedCpu;
        color = RGB(190, 60, 30);
        InvalidateRect(source, nullptr, FALSE);
        UpdateWindow(source);
        ok = recording.PauseResume() && ok;
        const auto resumed = GetTickCount64();
        Pump([&] { return GetTickCount64() - resumed > 700; });
        ok = recording.Stop(false) && ok;
        ok = Pump([&] { return Finished(recording); }) && ok;
        const auto snapshot = recording.Status();
        recording.Shutdown();
        Memory("stopped_joined");
        std::wcout << snapshot.message << L"\nBackend=" << snapshot.backend << L"\n";
        std::cout << "wall_ms=" << GetTickCount64() - start << " CPU_ms=" << (Cpu() - cpuStart) / 10000
                  << " paused_CPU_ms=" << pauseCpuUsed / 10000 << " active100ns=" << snapshot.duration
                  << " samples=" << snapshot.frames << " skipped=" << snapshot.skipped << " UI_ticks=" << ticks << '\n';
        ok = snapshot.state == State::Done && ticks > 20 && ok;
        KillTimer(source, 1);
        const auto quiet = GetTickCount64(), quietCpu = Cpu();
        Pump([&] { return GetTickCount64() - quiet > 500; });
        std::cout << "post_stop_quiet_CPU_ms=" << (Cpu() - quietCpu) / 10000 << '\n';
        if (std::filesystem::exists(path)) {
            winrt::check_hresult(MFStartup(MF_VERSION));
            {
                winrt::com_ptr<IMFAttributes> attributes;
                MFCreateAttributes(attributes.put(), 1);
                attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
                winrt::com_ptr<IMFSourceReader> reader;
                winrt::check_hresult(MFCreateSourceReaderFromURL(path.c_str(), attributes.get(), reader.put()));
                PROPVARIANT duration{};
                winrt::check_hresult(
                    reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration));
                std::cout << "decoded_duration100ns=" << duration.uhVal.QuadPart << '\n';
                ok = std::abs(int64_t(duration.uhVal.QuadPart) - snapshot.duration) < 100000 && ok;
                PropVariantClear(&duration);
                winrt::com_ptr<IMFMediaType> type;
                MFCreateMediaType(type.put());
                type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                winrt::check_hresult(
                    reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.get()));
                unsigned decoded = 0, green = 0, red = 0, excludedPixels = 0;
                for (int i = 0; i < 200; ++i) {
                    DWORD flags{};
                    winrt::com_ptr<IMFSample> sample;
                    winrt::check_hresult(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags,
                                                            nullptr, sample.put()));
                    if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                        break;
                    if (!sample)
                        continue;
                    winrt::com_ptr<IMFMediaBuffer> buffer;
                    sample->ConvertToContiguousBuffer(buffer.put());
                    BYTE *p{};
                    DWORD length{};
                    buffer->Lock(&p, nullptr, &length);
                    if (length >= 320 * 180 * 4) {
                        const auto *center = p + (90 * 320 + 160) * 4;
                        if (center[1] > center[2] + 40)
                            ++green;
                        if (center[2] > center[1] + 40)
                            ++red;
                        const auto *control = p + (40 * 320 + 40) * 4;
                        if (control[0] > 100 && control[2] > 100 && control[1] < 80)
                            ++excludedPixels;
                    }
                    buffer->Unlock();
                    ++decoded;
                }
                std::cout << "decoded=" << decoded << " green=" << green << " red=" << red
                          << " leaked_control=" << excludedPixels << '\n';
                ok = decoded > 10 && green > 3 && red > 3 && !excludedPixels && ok;
            }
            MFShutdown();
        } else
            ok = false;
    } catch (const winrt::hresult_error &e) {
        std::wcerr << e.message().c_str() << L" 0x" << std::hex << e.code().value << L"\n";
        ok = false;
    }
    try {
        SessionOptions options;
        options.fps = 60;
        options.capture = nskry::RegionCaptureServiceV2();
        options.followWindow = source;
        const auto followedPath = std::filesystem::path(argv[1]) /
            (L"record-follow-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".mp4");
        const RECT selected{region.left + 40, region.top + 20, region.left + 200, region.top + 110};
        RecordSession followed({}, selected, followedPath.wstring(), true, options);
        ok = followed.Start() && Pump([&] { return followed.Status().state == State::Recording || Finished(followed); }) && ok;
        SetWindowPos(source, nullptr, region.left + 50, region.top + 30, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ok = Pump([&] { const auto s = followed.Status(); return s.region.left == selected.left + 50 || Finished(followed); }) && ok;
        auto report = followed.Status();
        ok = report.region.left == selected.left + 50 && report.region.top == selected.top + 30 &&
             report.width == 160 && report.height == 90 && report.fps == 60 && ok;
        ok = followed.PauseResume() && ok;
        const auto paused = GetTickCount64();
        Pump([&] { return GetTickCount64() - paused > 250; });
        SetWindowPos(source, nullptr, region.left + 80, region.top + 50, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ok = followed.PauseResume() && Pump([&] { return followed.Status().region.left == selected.left + 80 || Finished(followed); }) && ok;
        ok = followed.Status().region.left == selected.left + 80 && followed.Stop(false) &&
             Pump([&] { return Finished(followed); }) && followed.Status().state == State::Done && ok;
        std::wcout << L"Follow result: " << followed.Status().message << L"\n";
        followed.Shutdown();
        SetWindowPos(source, nullptr, region.left, region.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        const auto resizedPath = followedPath.parent_path() / (followedPath.stem().wstring() + L"-resize.mp4");
        RecordSession resizing({}, selected, resizedPath.wstring(), true, options);
        ok = resizing.Start() && Pump([&] { return resizing.Status().state == State::Recording || Finished(resizing); }) && ok;
        SetWindowPos(source, nullptr, 0, 0, 330, 190, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        ok = Pump([&] { return Finished(resizing); }) && resizing.Status().state == State::Failed && ok;
        resizing.Shutdown();
        SetWindowPos(source, nullptr, region.left, region.top, 320, 180, SWP_NOZORDER | SWP_NOACTIVATE);
        const auto closedPath = followedPath.parent_path() / (followedPath.stem().wstring() + L"-close.mp4");
        RecordSession closingSource({}, selected, closedPath.wstring(), true, options);
        ok = closingSource.Start() && Pump([&] { return closingSource.Status().state == State::Recording || Finished(closingSource); }) && ok;
        DestroyWindow(source); // Only this test's window; never a user process/window.
        source = nullptr;
        ok = Pump([&] { return Finished(closingSource); }) && closingSource.Status().state == State::Failed && ok;
        closingSource.Shutdown();
        std::cout << "Window follow movement, pause/reopen movement, resize and source-close termination " << (ok ? "passed" : "FAILED") << '\n';
    } catch (const winrt::hresult_error &e) { std::wcerr << L"Follow: " << e.message().c_str() << '\n'; ok = false; }
    if (source) KillTimer(source, 1);
    DestroyWindow(excluded);
    DestroyWindow(source);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    winrt::uninit_apartment();
    return ok ? 0 : 1;
}
