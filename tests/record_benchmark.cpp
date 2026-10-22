#include "record_session.h"
#include "isolated_encoder.h"
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <psapi.h>
#include <tuple>
#include <winrt/base.h>

using namespace nskry::record;
namespace {
unsigned width{}, height{};
std::atomic<unsigned> captures{}, frame{};
HRESULT __cdecl Open(RECT, uint32_t, void **value, NskryCaptureDescriptionV1 *desc, wchar_t *, uint32_t) {
    *value = new int(1);
    *desc = {sizeof(*desc), 1, width, height, width * 4};
    ++captures;
    return S_OK;
}
HRESULT __cdecl Read(void *, uint8_t *pixels, uint32_t capacity) {
    if (capacity != width * height * 4)
        return E_INVALIDARG;
    std::memset(pixels, 30, capacity);
    const auto column = (++frame * 20) % (width - 20);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = column; x < column + 20; ++x) {
            auto *p = pixels + (size_t(y) * width + x) * 4;
            p[0] = 30;
            p[1] = 80;
            p[2] = 190;
            p[3] = 255;
        }
    return S_OK;
}
void __cdecl Close(void *value) {
    delete static_cast<int *>(value);
    --captures;
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
bool Finished(const RecordSession &s) {
    auto state = s.Status().state;
    return state == State::Done || state == State::Failed;
}
bool Pump(const std::function<bool()> &done, DWORD timeout = 15000) {
    const auto end = GetTickCount64() + timeout;
    while (!done() && GetTickCount64() < end) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return done();
}
struct Metrics {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    DWORD handles{};
    Metrics() {
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory));
        GetProcessHandleCount(GetCurrentProcess(), &handles);
    }
};
struct EncoderMetrics {
    uint64_t cpu{};
    PROCESS_MEMORY_COUNTERS_EX memory{};
    DWORD handles{};
    explicit EncoderMetrics(const Snapshot &s) : cpu(s.encoderCpu100ns) {
        if (!s.encoderProcessId || (s.state != State::Recording && s.state != State::Paused))
            return;
        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, s.encoderProcessId);
        if (!process)
            return;
        FILETIME c{}, e{}, k{}, u{};
        if (GetProcessTimes(process, &c, &e, &k, &u))
            cpu = (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
                  (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory));
        GetProcessHandleCount(process, &handles);
        CloseHandle(process);
    }
};
int Number(const wchar_t *text, int low, int high) {
    if (!text || !*text)
        return -1;
    int value = 0;
    for (; *text; ++text) {
        if (*text < L'0' || *text > L'9' || value > (high - int(*text - L'0')) / 10)
            return -1;
        value = value * 10 + int(*text - L'0');
    }
    return value >= low && value <= high ? value : -1;
}
int64_t Verify(const std::filesystem::path &path, uint64_t expectedFrames) {
    winrt::check_hresult(MFStartup(MF_VERSION));
    struct Mf {
        ~Mf() {
            MFShutdown();
        }
    } platform;
    winrt::com_ptr<IMFSourceReader> reader;
    winrt::check_hresult(MFCreateSourceReaderFromURL(path.c_str(), nullptr, reader.put()));
    PROPVARIANT v{};
    winrt::check_hresult(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v));
    const auto duration = int64_t(v.uhVal.QuadPart);
    const bool validDuration = v.vt == VT_UI8 && duration > 0;
    PropVariantClear(&v);
    if (!validDuration)
        winrt::throw_hresult(E_FAIL);
    winrt::com_ptr<IMFMediaType> decoded;
    MFCreateMediaType(decoded.put());
    decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    winrt::check_hresult(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, decoded.get()));
    unsigned count = 0;
    bool end = false;
    LONGLONG previous = -1;
    for (unsigned safety = 0; safety < 72000; ++safety) {
        DWORD flags{};
        LONGLONG timestamp{};
        winrt::com_ptr<IMFSample> sample;
        winrt::check_hresult(
            reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestamp, sample.put()));
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            end = true;
            break;
        }
        if (sample) {
            if (timestamp <= previous || (!count && timestamp != 0))
                winrt::throw_hresult(E_FAIL);
            previous = timestamp;
            ++count;
        }
    }
    if (!end || count != expectedFrames)
        winrt::throw_hresult(E_FAIL);
    std::cout << "decoded_frames=" << count << " duration100ns=" << duration << '\n';
    return duration;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc == 4 && !wcscmp(argv[1], L"--gif-compare")) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        int result = 0;
        try {
            const auto root = std::filesystem::path(argv[2]) /
                (L"gif-perf-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            std::filesystem::create_directory(root);
            std::ofstream metrics(root / L"metrics.csv");
            if (!metrics) winrt::throw_hresult(E_FAIL);
            metrics << "changes,current,wall_ms,child_cpu_ms,child_private_bytes\n";
            for (UINT changes : {1u, 6u, 30u}) {
                std::vector<uint8_t> baseline;
                for (bool current : {false, true}) {
                    const auto output = root / (std::to_wstring(changes) + (current ? L"-new.gif" : L"-old.gif"));
                    IsolatedEncoder encoder(nullptr, current ? std::filesystem::path{} : argv[3]);
                    encoder.Open(output.wstring(), 960, 540, 10, false, 0, true);
                    std::vector<uint8_t> pixels(960 * 540 * 4);
                    const auto start = GetTickCount64();
                    for (UINT frame = 0; frame < 30; ++frame) {
                        if (!(frame % (30 / changes)))
                            for (UINT y = 0; y < 540; ++y)
                                for (UINT x = 0; x < 960; ++x) {
                                    auto* p = pixels.data() + (y * 960 + x) * 4;
                                    p[0] = BYTE(x * 13 + y * 17 + frame);
                                    p[1] = BYTE(x * 11 + frame);
                                    p[2] = BYTE(y * 23 + frame);
                                    p[3] = 255;
                                }
                        encoder.Write(pixels.data(), frame * 1000000LL, 1000000);
                    }
                    encoder.Finalize();
                    PROCESS_MEMORY_COUNTERS_EX memory{};
                    memory.cb = sizeof(memory);
                    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                                                  encoder.ProcessId());
                    if (process) {
                        GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
                        CloseHandle(process);
                    }
                    encoder.Close();
                    const auto wall = GetTickCount64() - start;
                    const auto cpu = encoder.CpuTime() / 10000.0;
                    std::cout << "gif changes=" << changes << " current=" << current << " wall_ms="
                              << wall << " child_cpu_ms=" << cpu
                              << " child_private_bytes=" << memory.PrivateUsage << '\n';
                    metrics << changes << ',' << current << ',' << wall << ',' << cpu << ',' << memory.PrivateUsage << '\n';
                    std::ifstream stream(output, std::ios::binary);
                    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
                    stream.close();
                    if (!current)
                        baseline = bytes;
                    else {
                        const bool equal = !bytes.empty() && bytes == baseline;
                        std::cout << "gif bytes_equal=" << equal << " output_bytes=" << bytes.size() << '\n';
                        if (!equal) result = 1;
                        else {
                            std::filesystem::remove(output);
                            std::filesystem::remove(root / (std::to_wstring(changes) + L"-old.gif"));
                        }
                    }
                }
            }
            std::wcout << L"GIF comparison artifacts: " << root.wstring() << '\n';
        } catch (const winrt::hresult_error& error) {
            std::cerr << "GIF comparison failure=0x" << std::hex << error.code().value << '\n';
            result = 1;
        }
        winrt::uninit_apartment();
        return result;
    }
    // Revalidate a retained file without recording or overwriting anything.
    if (argc == 5 && !wcscmp(argv[1], L"--verify")) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        int result = 1;
        try {
            const auto frames = Number(argv[3], 1, 72000);
            size_t consumed{};
            const auto expected = std::stoll(argv[4], &consumed);
            if (frames > 0 && consumed == wcslen(argv[4]) && expected > 0 &&
                std::abs(Verify(argv[2], uint64_t(frames)) - expected) <= 100000)
                result = 0;
        } catch (...) {
        }
        winrt::uninit_apartment();
        return result;
    }
    if (argc != 7) {
        std::cerr << "Usage: benchmark output-directory width height active-seconds hardware|software cycles\n";
        return 2;
    }
    const int w = Number(argv[2], 160, 1920), h = Number(argv[3], 90, 1920), seconds = Number(argv[4], 1, 1800),
              cycles = Number(argv[6], 1, 20);
    const bool hardware = !wcscmp(argv[5], L"hardware");
    if (w < 0 || h < 0 || seconds < 0 || cycles < 0 || w % 2 || h % 2 || int64_t(w) * h > 1920 * 1080 ||
        (!hardware && wcscmp(argv[5], L"software")))
        return 2;
    width = w;
    height = h;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    std::cout << "Synthetic frames + real MF encoding; no WGC measurement; logical_cpus=" << system.dwNumberOfProcessors
              << '\n';
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    int result = 0;
    try {
        const auto root =
            std::filesystem::absolute(argv[1]) /
            (L"record-benchmark-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(root))
            winrt::throw_hresult(E_FAIL);
        std::ofstream csv(root / L"metrics.csv", std::ios::binary);
        csv << "cycle,phase,wall_ms,cpu_ms,private_bytes,working_bytes,handles,frames,slow_intervals,active_100ns,"
               "encoder_cpu_ms,encoder_private_bytes,encoder_working_bytes,encoder_handles,encoder_pid,"
               "max_capture_ms,max_encode_ms\n";
        const auto epoch = GetTickCount64(), cpuEpoch = Cpu();
        auto sample = [&](int cycle, const char *phase, const Snapshot &s) {
            Metrics m;
            EncoderMetrics child(s);
            csv << cycle << ',' << phase << ',' << GetTickCount64() - epoch << ',' << (Cpu() - cpuEpoch) / 10000 << ','
                << m.memory.PrivateUsage << ',' << m.memory.WorkingSetSize << ',' << m.handles << ',' << s.frames << ','
                << s.skipped << ',' << s.duration << ',' << child.cpu / 10000 << ',' << child.memory.PrivateUsage << ','
                << child.memory.WorkingSetSize << ',' << child.handles << ',' << s.encoderProcessId << ','
                << s.maxRead100ns / 10000.0 << ',' << s.maxEncode100ns / 10000.0 << '\n';
            if (!csv)
                winrt::throw_hresult(E_FAIL);
            std::cout << cycle << ',' << phase << " private=" << m.memory.PrivateUsage
                      << " working=" << m.memory.WorkingSetSize << " handles=" << m.handles
                      << " encoder_private=" << child.memory.PrivateUsage << " encoder_cpu_ms=" << child.cpu / 10000
                      << '\n';
        };
        NskryCaptureServiceV1 capture{sizeof(capture), 1, Open, Read, Close};
        std::vector<std::tuple<std::filesystem::path, int64_t, uint64_t>> completed;
        for (int i = 0; i < cycles; ++i) {
            sample(i, "idle", {});
            const auto path = root / (L"cycle-" + std::to_wstring(i) + L".mp4");
            RecordSession session(capture, {0, 0, w, h}, path.wstring(), hardware);
            session.Start();
            if (!Pump([&] { return session.Status().state == State::Recording || Finished(session); }) ||
                Finished(session)) {
                std::wcerr << session.Status().message << L'\n';
                result = 1;
                break;
            }
            std::wcout << L"Backend=" << session.Status().backend << L'\n';
            const auto target = int64_t(seconds) * 10000000;
            bool paused = false;
            uint64_t sampledAt = 0;
            while (!Finished(session) && session.Status().duration < target) {
                if (!paused && session.Status().duration >= target / 2) {
                    session.PauseResume();
                    const auto pausedAt = GetTickCount64();
                    Pump([&] { return !captures || Finished(session); });
                    sample(i, "paused", session.Status());
                    const auto frozen = session.Status().duration;
                    const auto pausedCpu = Cpu();
                    const auto pausedChildCpu = EncoderMetrics(session.Status()).cpu;
                    Pump([&] { return GetTickCount64() - pausedAt >= 1000; });
                    std::cout << "pause_cpu_ms=" << (Cpu() - pausedCpu) / 10000 << '\n';
                    std::cout << "pause_encoder_cpu_ms="
                              << (EncoderMetrics(session.Status()).cpu - pausedChildCpu) / 10000 << '\n';
                    if (captures || session.Status().duration != frozen)
                        result = 1;
                    session.PauseResume();
                    paused = true;
                }
                const auto now = GetTickCount64();
                if (now - sampledAt >= 1000) {
                    sample(i, "recording", session.Status());
                    sampledAt = now;
                }
                const auto until = now + 20;
                Pump([&] { return GetTickCount64() >= until || Finished(session); });
            }
            session.Stop(false);
            if (!Pump([&] { return Finished(session); }))
                result = 1;
            const auto s = session.Status();
            session.Shutdown();
            sample(i, "stopped", s);
            if (s.state != State::Done || captures) {
                std::wcerr << s.message << L'\n';
                result = 1;
                break;
            }
            const auto quiet = GetTickCount64(), quietCpu = Cpu();
            Pump([&] { return GetTickCount64() - quiet >= 1000; });
            std::cout << "post_stop_cpu_ms=" << (Cpu() - quietCpu) / 10000 << " samples=" << s.frames
                      << " active100ns=" << s.duration << '\n';
            sample(i, "quiet", s);
            completed.emplace_back(path, s.duration, s.frames);
        }
        // Decoder caches must not contaminate the next recording cycle baseline.
        for (const auto &[path, duration, frames] : completed)
            if (std::abs(Verify(path, frames) - duration) > 100000)
                result = 1;
        std::wcout << L"Metrics retained: " << (root / L"metrics.csv").c_str() << L'\n';
    } catch (const winrt::hresult_error &e) {
        std::wcerr << e.message().c_str() << L" 0x" << std::hex << e.code().value << L'\n';
        result = 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    winrt::uninit_apartment();
    return result;
}
