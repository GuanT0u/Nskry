#include "core/single_instance.h"
#include <array>
#include <iostream>
#include <string>

namespace {
using Guard = nskry::SingleInstanceGuard;
struct Child {
    HANDLE process{};
    ~Child() {
        if (process) {
            WaitForSingleObject(process, 7000);
            CloseHandle(process);
        }
    }
    bool Start(const std::wstring& arguments, const wchar_t* application = nullptr) {
        wchar_t executable[32768]{};
        if (!application) {
            const auto length = GetModuleFileNameW(nullptr, executable, 32768);
            if (!length || length == 32768)
                return false;
            application = executable;
        }
        std::wstring command = L"\"" + std::wstring(application) + L"\" " + arguments;
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(application, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            nullptr, &startup, &child))
            return false;
        process = child.hProcess;
        CloseHandle(child.hThread);
        return true;
    }
    DWORD ExitCode() const {
        DWORD value = STILL_ACTIVE;
        if (process)
            GetExitCodeProcess(process, &value);
        return value;
    }
};
bool Check(bool result, const char* label) {
    std::cout << (result ? "PASS " : "FAIL ") << label << '\n';
    return result;
}
bool Probe(const std::wstring& id, DWORD expected, bool abrupt = false) {
    Child child;
    return child.Start(std::wstring(abrupt ? L"--exit " : L"--probe ") + id) &&
           WaitForSingleObject(child.process, 5000) == WAIT_OBJECT_0 && child.ExitCode() == expected;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3) {
        Guard guard(argv[2]);
        if (guard.Status() == Guard::Result::AlreadyRunning)
            return 10;
        if (guard.Status() != Guard::Result::Primary)
            return 20;
        if (std::wstring(argv[1]) == L"--exit")
            ExitProcess(0); // OS cleanup must work without a C++ destructor.
        if (std::wstring(argv[1]) == L"--race" && argc == 5) {
            HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
            HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, argv[4]);
            const bool signaled = ready && release && SetEvent(ready);
            const DWORD result = signaled ? WaitForSingleObject(release, 5000) : WAIT_FAILED;
            if (ready)
                CloseHandle(ready);
            if (release)
                CloseHandle(release);
            return result == WAIT_OBJECT_0 ? 0 : 21;
        }
        return 0;
    }
    if (argc != 2)
        return 2;
    const std::wstring id = L"Nskry.SingleInstanceTest." + std::to_wstring(GetCurrentProcessId()) + L"." +
                            std::to_wstring(GetTickCount64());
    bool ok = true;
    {
        Guard primary(id.c_str());
        Guard duplicate(id.c_str());
        ok &= Check(primary.Status() == Guard::Result::Primary &&
                        duplicate.Status() == Guard::Result::AlreadyRunning && Probe(id, 10),
                    "same-process and second-process launches are rejected");
    }
    ok &= Check(Probe(id, 0), "normal exit permits immediate restart");
    ok &= Check(Probe(id, 0, true) && Probe(id, 0), "abrupt process exit leaves no stale lock");
    {
        const std::wstring otherId = id + L".independent";
        Guard other(otherId.c_str());
        ok &= Check(other.Status() == Guard::Result::Primary && Probe(id, 0),
                    "independent namespaces do not interfere");
    }
    const std::wstring readyName = L"Local\\" + id + L".ready";
    const std::wstring releaseName = L"Local\\" + id + L".release";
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
    {
        std::array<Child, 4> children;
        bool started = ready && release;
        if (started)
            for (auto& child : children)
                started = child.Start(L"--race " + id + L" " + readyName + L" " + releaseName) && started;
        bool oneLive = false;
        if (started && WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0) {
            const auto deadline = GetTickCount64() + 3000;
            do {
                unsigned duplicates = 0, live = 0;
                for (const auto& child : children) {
                    duplicates += child.ExitCode() == 10;
                    live += child.ExitCode() == STILL_ACTIVE;
                }
                oneLive = duplicates == 3 && live == 1;
                if (!oneLive)
                    Sleep(10);
            } while (!oneLive && GetTickCount64() < deadline);
        }
        if (release)
            SetEvent(release);
        unsigned winners = 0;
        for (auto& child : children)
            if (child.process && WaitForSingleObject(child.process, 5000) == WAIT_OBJECT_0)
                winners += child.ExitCode() == 0;
        ok &= Check(oneLive && winners == 1, "racing launches elect exactly one primary");
    }
    if (ready)
        CloseHandle(ready);
    if (release)
        CloseHandle(release);
    {
        // The real entrypoint must reject a duplicate before it can initialize
        // graphics, global shortcuts, plugin state or the startup dialog.
        Guard production;
        Child duplicate;
        ok &= Check(production.Status() != Guard::Result::Failed && duplicate.Start(L"", argv[1]) &&
                        WaitForSingleObject(duplicate.process, 3000) == WAIT_OBJECT_0 && duplicate.ExitCode() == 0,
                    "real Nskry entrypoint exits while an instance guard exists");
    }
    return ok ? 0 : 1;
}
