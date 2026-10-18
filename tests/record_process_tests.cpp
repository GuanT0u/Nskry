#include "isolated_encoder.h"
#include "record_worker_protocol.h"
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>
#include <winrt/base.h>
using namespace nskry::record;
namespace {
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
// A test-only parent that deliberately exits without C++ cleanup. The outer
// test owns a DIFFERENT job which stays open while it checks encoder death.
int OrphanParent(const wchar_t *root, const wchar_t *pipeText) {
    HANDLE raw{};
    if (!wire::ParseHandle(pipeText, raw))
        return 61;
    Handle pipe{raw};
    try {
        IsolatedEncoder encoder;
        encoder.Open((std::filesystem::path(root) / L"abrupt-parent.partial.mp4").wstring(), 320, 180, 20, false);
        const auto pid = encoder.ProcessId();
        DWORD written{};
        if (!WriteFile(pipe.value, &pid, sizeof(pid), &written, nullptr) || written != sizeof(pid))
            return 62;
        Sleep(INFINITE);
    } catch (...) {
        return 63;
    }
    return 64;
}
bool ParentExit(const std::filesystem::path &root) {
    struct Attributes {
        std::vector<uint8_t> bytes;
        LPPROC_THREAD_ATTRIBUTE_LIST value{};
        ~Attributes() {
            if (value)
                DeleteProcThreadAttributeList(value);
        }
    } attributes;
    SIZE_T size{};
    InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
    attributes.bytes.resize(size);
    auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.bytes.data());
    if (!InitializeProcThreadAttributeList(list, 2, 0, &size))
        return false;
    attributes.value = list;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE rawRead{}, rawWrite{};
    if (!CreatePipe(&rawRead, &rawWrite, &security, 0))
        return false;
    Handle read{rawRead}, write{rawWrite}, job{CreateJobObjectW(nullptr, nullptr)};
    if (!job.value || !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0))
        return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write.value, sizeof(HANDLE), nullptr,
                                   nullptr) ||
        !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job.value, sizeof(HANDLE), nullptr,
                                   nullptr))
        return false;
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, 32768))
        return false;
    auto command = L"\"" + std::wstring(executable) + L"\" --orphan-parent \"" + root.wstring() + L"\" " +
                   std::to_wstring(reinterpret_cast<uintptr_t>(write.value));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo,
                        &process))
        return false;
    Handle parent{process.hProcess}, thread{process.hThread};
    const auto deadline = GetTickCount64() + 5000;
    DWORD available{};
    while (GetTickCount64() < deadline) {
        if (PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available >= sizeof(DWORD))
            break;
        if (WaitForSingleObject(parent.value, 10) == WAIT_OBJECT_0)
            return false;
    }
    if (available < sizeof(DWORD))
        return false;
    DWORD pid{}, received{};
    if (!ReadFile(read.value, &pid, sizeof(pid), &received, nullptr) || received != sizeof(pid))
        return false;
    Handle encoder{OpenProcess(SYNCHRONIZE, FALSE, pid)};
    if (!encoder.value)
        return false;
    // Only the explicitly created test harness is ended. Its child must die
    // from the private recording Job, not from closing the outer test Job.
    const bool ended = TerminateProcess(parent.value, 0) && WaitForSingleObject(parent.value, 5000) == WAIT_OBJECT_0 &&
                       WaitForSingleObject(encoder.value, 5000) == WAIT_OBJECT_0;
    std::cout << "abrupt_parent_exit encoder_exited=" << ended << '\n';
    return ended;
}
bool Fault(const std::filesystem::path &exe, const std::filesystem::path &path, HANDLE canceled, HRESULT expected,
           bool finalizing = false) {
    const auto start = GetTickCount64();
    HANDLE process{};
    bool failed = false, owned = false;
    {
        IsolatedEncoder encoder(canceled, exe, 2000);
        encoder.Open(path.wstring(), 320, 180, 20, false);
        process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, encoder.ProcessId());
        if (!process)
            return false;
        std::thread cancel;
        if (canceled)
            cancel = std::thread([&] {
                Sleep(100);
                SetEvent(canceled);
            });
        try {
            std::vector<uint8_t> pixels(320 * 180 * 4, 90);
            encoder.Write(pixels.data(), 0, 500000);
            if (finalizing)
                encoder.Finalize();
        } catch (const winrt::hresult_error &e) {
            failed = e.code() == expected;
        }
        if (cancel.joinable())
            cancel.join();
        encoder.Close();
        owned = encoder.CreatedFile();
    }
    const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    std::cout << "fault expected=0x" << std::hex << unsigned(expected) << std::dec
              << " elapsed_ms=" << GetTickCount64() - start << " exited=" << exited << '\n';
    return failed && exited && owned && std::filesystem::exists(path) && GetTickCount64() - start < 5000;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc == 4 && !wcscmp(argv[1], L"--orphan-parent"))
        return OrphanParent(argv[2], argv[3]);
    if (argc != 5)
        return 2;
    const auto root =
        std::filesystem::absolute(argv[1]) /
        (L"record-process-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(root);
    bool ok = true;
    try {
        HANDLE process{};
        {
            IsolatedEncoder encoder;
            encoder.Open((root / L"正常 output.mp4").wstring(), 320, 180, 20, false);
            process = OpenProcess(SYNCHRONIZE, FALSE, encoder.ProcessId());
            std::vector<uint8_t> pixels(320 * 180 * 4, 100);
            for (int i = 0; i < 10; ++i)
                encoder.Write(pixels.data(), i * 500000LL, 500000);
            try {
                encoder.Write(pixels.data(), 0, 1);
                ok = false;
            } catch (const winrt::hresult_error &) {
            }
            encoder.Finalize();
            ok = encoder.CreatedFile() && encoder.Finalized() && encoder.Samples() == 10 &&
                 encoder.EncodedDuration() == 5000000 && ok;
            encoder.Close();
        }
        ok = process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0 && ok;
        if (process)
            CloseHandle(process);
        {
            IsolatedEncoder relative;
            const auto path = root / L"relative file.mp4";
            relative.Open(std::filesystem::relative(path).wstring(), 320, 180, 20, false);
            std::vector<uint8_t> pixels(320 * 180 * 4, 80);
            relative.Write(pixels.data(), 0, 500000);
            relative.Finalize();
            ok = relative.Close() && std::filesystem::exists(path) && ok;
        }
        ok = ParentExit(root) && ok;
        ok = Fault(argv[2], root / L"crash.partial.mp4", nullptr, HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE)) && ok;
        ok = Fault(argv[3], root / L"timeout.partial.mp4", nullptr, HRESULT_FROM_WIN32(ERROR_TIMEOUT)) && ok;
        ok = Fault(argv[4], root / L"finalize-timeout.partial.mp4", nullptr, HRESULT_FROM_WIN32(ERROR_TIMEOUT), true) &&
             ok;
        HANDLE canceled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!canceled)
            return 3;
        ok = Fault(argv[3], root / L"cancel.partial.mp4", canceled, HRESULT_FROM_WIN32(ERROR_CANCELLED)) && ok;
        CloseHandle(canceled);
        try {
            IsolatedEncoder missing(nullptr, root / L"missing.exe");
            missing.Open((root / L"must-not-exist.mp4").wstring(), 320, 180, 20);
            ok = false;
        } catch (const winrt::hresult_error &e) {
            ok = e.code() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
                 !std::filesystem::exists(root / L"must-not-exist.mp4") && ok;
        }
    } catch (const winrt::hresult_error &e) {
        std::wcerr << e.message().c_str() << L" 0x" << std::hex << unsigned(e.code().value) << L'\n';
        ok = false;
    }
    std::cout << (ok ? "Encoder child normal/crash/timeout/cancel/missing cleanup passed.\n"
                     : "Encoder process failure.\n");
    return ok ? 0 : 1;
}
