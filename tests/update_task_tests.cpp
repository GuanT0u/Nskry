#include "core/update_task.h"
#include "update/downloader.h"
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <winsock2.h>
#include <ws2tcpip.h>
using namespace nskry;
namespace {
unsigned heartbeats{};
void CALLBACK Heartbeat(HWND, UINT, UINT_PTR, DWORD) { ++heartbeats; }
bool Check(bool ok, const char *message) {
    if (!ok)
        std::cerr << message << '\n';
    return ok;
}
bool PumpUntil(const std::function<bool()> &done, DWORD timeout = 4000) {
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
// Own loopback TCP peer deliberately stalls TLS. No external service, certificate
// bypass, user network credentials or input injection is used.
struct StalledPeer {
    SOCKET listener = INVALID_SOCKET;
    std::jthread server;
    HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    unsigned port{};
    StalledPeer() {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
            listen(listener, 1))
            return;
        int length = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length);
        port = ntohs(address.sin_port);
        u_long nonblocking = 1;
        ioctlsocket(listener, FIONBIO, &nonblocking);
        server = std::jthread([this](std::stop_token stop) {
            std::stop_callback callback(stop, [this] { SetEvent(quit); });
            SOCKET client = INVALID_SOCKET;
            while (!stop.stop_requested()) {
                WSAPOLLFD fd{listener, POLLRDNORM, 0};
                if (WSAPoll(&fd, 1, 50) > 0) {
                    client = accept(listener, nullptr, nullptr);
                    if (client != INVALID_SOCKET)
                        break;
                }
            }
            if (client != INVALID_SOCKET) {
                WaitForSingleObject(quit, INFINITE);
                closesocket(client);
            }
        });
    }
    ~StalledPeer() {
        server.request_stop();
        SetEvent(quit);
        if (server.joinable())
            server.join();
        if (listener != INVALID_SOCKET)
            closesocket(listener);
        CloseHandle(quit);
    }
    std::wstring Url() const {
        return L"https://127.0.0.1:" + std::to_wstring(port) + L"/stalled";
    }
};
} // namespace
int wmain() {
    bool ok = true;
    UpdateTask task;
    const auto ui = GetCurrentThreadId();
    bool delivered = false;
    ok &= Check(ResponseChunkFits(8, 2, 10) && !ResponseChunkFits(8, 3, 10) && !ResponseChunkFits(SIZE_MAX, 1, 10),
                "response size/overflow policy");
    auto payload = std::make_shared<int>(7);
    std::weak_ptr<int> released = payload;
    ok &= Check(
        task.Start(
            [payload](std::stop_token) {
                UpdateTaskResult result;
                result.message = L"done";
                return result;
            },
            [&](UpdateTaskResult result) { delivered = GetCurrentThreadId() == ui && result.message == L"done"; }),
        "start completion");
    payload.reset();
    ok &= Check(PumpUntil([&] { return !task.Busy(); }) && delivered && released.expired(),
                "main delivery and captured resource release");
    bool cancelledCallback = false;
    std::mutex mutex;
    std::condition_variable_any cv;
    ok &= Check(task.Start(
                    [&](std::stop_token stop) {
                        std::unique_lock lock(mutex);
                        cv.wait(lock, stop, [] { return false; });
                        return UpdateTaskResult{};
                    },
                    [&](UpdateTaskResult) { cancelledCallback = true; }),
                "start cancellation");
    ok &= Check(!task.Start([](std::stop_token) { return UpdateTaskResult{}; }, {}), "no duplicate task/queue");
    const auto cancelStart = GetTickCount64();
    task.Cancel();
    ok &= Check(GetTickCount64() - cancelStart < 100 && PumpUntil([&] { return !task.Busy(); }) && !cancelledCallback,
                "nonblocking cancel drops stale UI callback");
    // Cancellation may need time to release network/temp resources. UI must
    // continue dispatching, and the bounded task slot must not be reused early.
    std::atomic<bool> started{}, cleaning{};
    bool staleDelivered = false;
    task.Start([&](std::stop_token stop) {
        started = true;
        std::unique_lock lock(mutex);
        cv.wait(lock, stop, [] { return false; });
        cleaning = true;
        lock.unlock();
        Sleep(350); // Test-only delayed resource teardown.
        return UpdateTaskResult{};
    }, [&](UpdateTaskResult) { staleDelivered = true; });
    ok &= Check(PumpUntil([&] { return started.load(); }), "delayed cleanup starts");
    const auto slowCancelStart = GetTickCount64();
    task.Cancel();
    const auto slowCancelMs = GetTickCount64() - slowCancelStart;
    const auto heartbeat = SetTimer(nullptr, 0, 10, Heartbeat);
    ok &= Check(slowCancelMs < 100 && task.Busy() &&
        !task.Start([](std::stop_token) { return UpdateTaskResult{}; }, {}), "cancel keeps bounded busy slot");
    ok &= Check(PumpUntil([&] { return !task.Busy(); }) && cleaning && !staleDelivered,
                "delayed teardown releases task without stale UI delivery");
    KillTimer(nullptr, heartbeat);
    ok &= Check(heartbeat && heartbeats >= 5 &&
        !FindWindowExW(HWND_MESSAGE, nullptr, L"NskryUpdateCompletion", nullptr),
        "UI remains responsive and completion window is released");
    std::cout << "Delayed update cancel ms=" << slowCancelMs << ", UI heartbeats=" << heartbeats << '\n';
    bool failed = false;
    task.Start([](std::stop_token) -> UpdateTaskResult { throw std::runtime_error("test failure"); },
               [&](UpdateTaskResult result) { failed = !result.message.empty(); });
    ok &= Check(PumpUntil([&] { return !task.Busy(); }) && failed, "worker exception delivered and released");
    task.Start(
        [&](std::stop_token stop) {
            std::unique_lock lock(mutex);
            cv.wait(lock, stop, [] { return false; });
            return UpdateTaskResult{};
        },
        {});
    task.Shutdown();
    ok &= Check(!task.Busy(), "exit joins cancelled worker");
    std::stop_source stopped;
    stopped.request_stop();
    std::wstring text, error;
    ok &= Check(!Downloader::DownloadTextHttps(L"https://invalid.example", text, &error, stopped.get_token()) &&
                    text.empty(),
                "pre-cancel before networking");
    ok &= Check(!Downloader::DownloadTextHttps(L"http://127.0.0.1/", text, &error), "HTTPS policy");
    const auto directory =
        std::filesystem::current_path() /
        (L"transfer-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(directory))
        return 1;
    const auto archive = directory / L"cancel.nskryplugin";
    ok &= Check(
        !Downloader::DownloadFileHttps(L"https://invalid.example", archive.wstring(), &error, stopped.get_token()) &&
            !std::filesystem::exists(archive),
        "cancelled file transfer removes owned partial file");
    {
        std::ofstream empty(archive, std::ios::binary);
    }
    ok &= Check(!Downloader::DownloadFileHttps(L"http://invalid.example", archive.wstring(), &error) &&
                    std::filesystem::exists(archive),
                "existing destination is never overwritten or deleted");
    ok &= Check(Downloader::VerifySha256File(
                    archive.wstring(), L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", &error) &&
                    !Downloader::VerifySha256File(archive.wstring(), std::wstring(64, L'0'), &error),
                "stream hash validation and mismatch");
    std::filesystem::remove(archive);
    std::filesystem::remove(directory);
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return 1;
    {
        StalledPeer peer;
        bool result = false;
        task.Start(
            [url = peer.Url()](std::stop_token stop) {
                UpdateTaskResult r;
                std::wstring text;
                Downloader::DownloadTextHttps(url, text, &r.message, stop, 150);
                return r;
            },
            [&](UpdateTaskResult r) { result = r.message.find(L"timed out") != std::wstring::npos; });
        ok &= Check(peer.port && PumpUntil([&] { return !task.Busy(); }, 3000) && result,
                    "real async WinHTTP deadline/cleanup on stalled TLS");
        task.Shutdown();
    }
    {
        StalledPeer peer;
        task.Start(
            [url = peer.Url()](std::stop_token stop) {
                UpdateTaskResult r;
                Downloader::DownloadTextHttps(url, r.message, nullptr, stop);
                return r;
            },
            [&](UpdateTaskResult) { cancelledCallback = true; });
        PumpUntil([] { return false; }, 150);
        const auto start = GetTickCount64();
        task.Cancel();
        ok &= Check(peer.port && PumpUntil([&] { return !task.Busy(); }, 3000) && !cancelledCallback &&
                        GetTickCount64() - start < 3000,
                    "real request cancellation returns without waiting network timeout");
    }
    WSACleanup();
    task.Shutdown();
    return ok ? 0 : 1;
}
