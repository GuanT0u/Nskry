#include "core/json.h"
#include "online_translation.h"
#include "translation_http.h"
#include <atomic>
#include <iostream>
#include <psapi.h>
#include <thread>
#include <wincrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace nskry::translate;
namespace {
bool Check(bool value, const char *name) {
    if (!value)
        std::cerr << name << '\n';
    return value;
}
template <class Work> bool Rejects(Work work) {
    try {
        work();
        return false;
    } catch (const std::wstring &) {
        return true;
    }
}
// An owned loopback peer stalls TLS; tests do not change certificates or bypass
// validation, reach a real provider, or read user keys/proxy credentials.
struct Peer {
    SOCKET listener = INVALID_SOCKET;
    std::jthread thread;
    HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    unsigned port{};
    Peer() {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener == INVALID_SOCKET ||
            bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(listener, 1))
            return;
        int size = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size);
        port = ntohs(address.sin_port);
        u_long nonblocking = 1;
        ioctlsocket(listener, FIONBIO, &nonblocking);
        thread = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                WSAPOLLFD fd{listener, POLLRDNORM, 0};
                if (WSAPoll(&fd, 1, 30) > 0) {
                    const auto client = accept(listener, nullptr, nullptr);
                    if (client != INVALID_SOCKET) {
                        WaitForSingleObject(quit, INFINITE);
                        closesocket(client);
                        break;
                    }
                }
            }
        });
    }
    ~Peer() {
        thread.request_stop();
        SetEvent(quit);
        if (thread.joinable())
            thread.join();
        if (listener != INVALID_SOCKET)
            closesocket(listener);
        CloseHandle(quit);
    }
    OnlineRequest Request() const {
        OnlineRequest request;
        request.url = L"https://127.0.0.1:" + std::to_wstring(port) + L"/stalled";
        return request;
    }
};
bool ConfigTests() {
    // Redirect only this test process's HKCU to a newly created owned key.
    const auto path = L"Software\\NskryTests\\Translation-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(GetTickCount64());
    HKEY isolated{};
    DWORD disposition{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &isolated,
                        &disposition) ||
        disposition != REG_CREATED_NEW_KEY)
        return false;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, isolated)) {
        RegCloseKey(isolated);
        RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        return false;
    }
    constexpr wchar_t key[] = L"Software\\Nskry\\Plugins\\nskry-translate";
    OnlineConfig config, loaded;
    config.endpoint = L"https://configured.example/v1/chat/completions";
    config.model = L"user-chosen-model";
    config.key = L"synthetic-fixture-not-a-real-key";
    std::wstring error;
    bool ok = LoadOnlineConfig(Provider::OpenAI, loaded, error) && loaded.endpoint.empty();
    ok = SaveOnlineConfig(Provider::OpenAI, config, error) &&
         LoadOnlineConfig(Provider::OpenAI, loaded, error) && loaded.key == config.key && ok;
    DWORD size{};
    RegGetValueW(HKEY_CURRENT_USER, key, L"OpenAIConfigV1", RRF_RT_REG_BINARY, nullptr, nullptr, &size);
    std::string raw(size, 0);
    RegGetValueW(HKEY_CURRENT_USER, key, L"OpenAIConfigV1", RRF_RT_REG_BINARY, nullptr, raw.data(), &size);
    ok = raw.find("synthetic-fixture") == raw.npos && ok;
    auto write = [&](const std::wstring &json) {
        DATA_BLOB plain{DWORD(json.size() * sizeof(wchar_t)),
                        reinterpret_cast<BYTE *>(const_cast<wchar_t *>(json.data()))},
            encrypted{};
        if (!CryptProtectData(&plain, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                              &encrypted))
            return false;
        const bool result = RegSetKeyValueW(HKEY_CURRENT_USER, key, L"OpenAIConfigV1", REG_BINARY,
                                            encrypted.pbData, encrypted.cbData) == ERROR_SUCCESS;
        LocalFree(encrypted.pbData);
        return result;
    };
    ok = write(LR"({"schema":1,"future":{"keep":true}})") &&
         SaveOnlineConfig(Provider::OpenAI, config, error) && ok;
    RegGetValueW(HKEY_CURRENT_USER, key, L"OpenAIConfigV1", RRF_RT_REG_BINARY, nullptr, nullptr, &size);
    raw.resize(size);
    RegGetValueW(HKEY_CURRENT_USER, key, L"OpenAIConfigV1", RRF_RT_REG_BINARY, nullptr, raw.data(), &size);
    DATA_BLOB encrypted{size, reinterpret_cast<BYTE *>(raw.data())}, plain{};
    if (CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                           &plain)) {
        const std::wstring json(reinterpret_cast<wchar_t *>(plain.pbData), plain.cbData / sizeof(wchar_t));
        ok = json.find(L"future") != json.npos && ok;
        LocalFree(plain.pbData);
    } else
        ok = false;
    ok = write(LR"({"schema":2})") && !SaveOnlineConfig(Provider::OpenAI, config, error) && ok;
    ok = ForgetOnlineConfig(Provider::OpenAI, error) && ok;
    const char bad[] = "bad encrypted settings";
    RegSetKeyValueW(HKEY_CURRENT_USER, key, L"OpenAIConfigV1", REG_BINARY, bad, sizeof(bad));
    ok = !LoadOnlineConfig(Provider::OpenAI, loaded, error) &&
         !SaveOnlineConfig(Provider::OpenAI, config, error) && ok;
    ok = ForgetOnlineConfig(Provider::OpenAI, error) && LoadOnlineConfig(Provider::OpenAI, loaded, error) &&
         loaded.key.empty() && ok;
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(isolated);
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   path.c_str()); // Exact key created above, never a user configuration key.
    return ok;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    bool ok = true;
    OnlineConfig config;
    config.endpoint = L"https://configured.example/v1/chat/completions";
    config.model = L"chosen-model";
    config.key = L"synthetic-not-a-key";
    ok &= Check(ValidateOnlineConfig(Provider::OpenAI, config).empty(), "valid explicit config");
    auto request = BuildOnlineRequest(Provider::OpenAI, config, L"Hello \"world\".\r\nNext paragraph.", true);
    nskry::json::Value body;
    const std::wstring bodyText(request.body.begin(), request.body.end());
    ok &= Check(nskry::json::Parse(bodyText, body) && body.Find(L"messages") && !request.get &&
                    request.headers.find(config.key) != request.headers.npos &&
                    body.Find(L"max_completion_tokens"),
                "OpenAI request schema and escaping");
    config.legacyTokenLimit = true;
    request = BuildOnlineRequest(Provider::OpenAI, config, L"Hello", true);
    ok &= Check(request.body.find("max_tokens") != request.body.npos &&
                    request.body.find("max_completion_tokens") == request.body.npos,
                "legacy token field is explicit");
    for (const auto *bad : {L"http://example.com/translate", L"https://user:pass@example.com/a",
                            L"https://example.com/a#fragment", L"https://example.com/\r\nX:y",
                            L"https://example.com/a b", L"https://example.com/a\\b"})
        ok &= Check(!ValidateEndpoint(bad).empty(), "unsafe endpoint rejected");
    config.key = L"key\r\nInjected: header";
    ok &= Check(!ValidateOnlineConfig(Provider::OpenAI, config).empty(), "header injection rejected");
    config.key.clear();
    config.endpoint = L"https://configured.example/translate";
    request = BuildOnlineRequest(Provider::LibreTranslate, config, L"你好。", false);
    ok &= Check(request.body.find("api_key") == request.body.npos &&
                    request.headers.find(L"Authorization") == request.headers.npos,
                "optional Libre key");
    ok &=
        Check(ParseOnlineReply(
                  Provider::OpenAI,
                  LR"({"choices":[{"finish_reason":"stop","message":{"content":"你好","refusal":null}}]})") ==
                  L"你好",
              "OpenAI reply");
    for (const auto *bad :
         {LR"({"choices":[{"finish_reason":"length","message":{"content":"partial"}}]})",
          LR"({"choices":[{"finish_reason":"tool_calls","message":{"content":"partial"}}]})",
          LR"({"choices":[]})",
          LR"({"choices":[{"finish_reason":"stop","message":{"content":"","refusal":"no"}}]})", L"invalid"})
        ok &= Check(Rejects([&] { ParseOnlineReply(Provider::OpenAI, bad); }), "incomplete output rejected");
    ok &= Check(ParseOnlineReply(Provider::LibreTranslate, LR"({"translatedText":"你好"})") == L"你好",
                "Libre reply");
    request = BuildOnlineRequest(Provider::MyMemory, config, L"你好 &?", false);
    ok &= Check(request.get && request.url.find(L"%E4%BD%A0") != request.url.npos &&
                    request.url.find(L"%26%3F") != request.url.npos &&
                    request.headers.find(L"Authorization") == request.headers.npos && request.body.empty(),
                "free UTF-8 URL encoding, no credentials");
    std::wstring source;
    for (int i = 0; i < 250; ++i)
        source += L"中文";
    source += L"\r\n\nNext paragraph.";
    const auto parts = SplitFreeOnlineText(source);
    std::wstring joined;
    for (const auto &part : parts) {
        joined += part;
        std::string utf8;
        nskry::json::ToUtf8(part, utf8);
        ok &= Check(utf8.size() <= 480, "free UTF-8 segment cap");
    }
    ok &= Check(joined == source && parts.size() > 1, "free split lossless paragraphs");
    ok &= Check(Rejects([] { SplitFreeOnlineText(std::wstring(2001, L'x')); }), "free task cap");
    ok &= Check(Rejects([] {
                    std::wstring text;
                    for (int i = 0; i < 17; ++i)
                        text += L"x\n";
                    SplitFreeOnlineText(text);
                }),
                "free request count cap");
    ok &= Check(
        ParseOnlineReply(Provider::MyMemory,
                         LR"({"responseStatus":200,"responseData":{"translatedText":"hello"}})") ==
                L"hello" &&
            Rejects([] {
                ParseOnlineReply(
                    Provider::MyMemory,
                    LR"({"responseStatus":200,"quotaFinished":true,"responseData":{"translatedText":"QUOTA ERROR"}})");
            }),
        "free quota not published as translation");
    ok &= Check(ConfigTests(), "isolated DPAPI configuration, unknown fields, corruption, removal");
    HANDLE cancel = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    request.url = L"https://127.0.0.1:1/not-contacted";
    ok &= Check(Rejects([&] { FetchTranslation(request, cancel, 1000); }), "pre-cancel before network");
    ResetEvent(cancel);
    WSADATA sockets{};
    if (WSAStartup(MAKEWORD(2, 2), &sockets))
        return 2;
    {
        Peer peer;
        const auto start = GetTickCount64();
        std::wstring error;
        try {
            FetchTranslation(peer.Request(), cancel, 150);
        } catch (const std::wstring &message) {
            error = message;
        }
        ok &= Check(peer.port && error.find(L"timed out") != error.npos && GetTickCount64() - start < 3000,
                    "stalled TLS deadline and callback cleanup");
    }
    {
        Peer peer;
        std::wstring error;
        std::thread worker([&] {
            try {
                FetchTranslation(peer.Request(), cancel, 60000);
            } catch (const std::wstring &message) {
                error = message;
            }
        });
        Sleep(100); // Only this owned loopback request, not a user/UI wait.
        const auto start = GetTickCount64();
        SetEvent(cancel);
        worker.join();
        ok &= Check(peer.port && error.find(L"canceled") != error.npos && GetTickCount64() - start < 3000,
                    "stalled TLS cancel and final callback fence");
    }
    WSACleanup();
    ResetEvent(cancel);
    if (argc == 2 && !wcscmp(argv[1], L"--live-free")) {
        // Explicit manual opt-in only. Public synthetic sentences, never user
        // screenshots/clipboard/configuration. Not part of ordinary CTest.
        for (const bool toChinese : {true, false}) {
            const auto start = GetTickCount64();
            auto cpu = [] {
                FILETIME created{}, exited{}, kernel{}, user{};
                GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
                return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                       ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
            };
            std::atomic<size_t> peakPrivate{}, peakWorkingSet{};
            const auto cpuStart = cpu();
            std::jthread sampler([&](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    PROCESS_MEMORY_COUNTERS_EX memory{};
                    memory.cb = sizeof(memory);
                    if (GetProcessMemoryInfo(GetCurrentProcess(),
                                             reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                                             sizeof(memory))) {
                        peakPrivate = (std::max)(peakPrivate.load(), memory.PrivateUsage);
                        peakWorkingSet = (std::max)(peakWorkingSet.load(), memory.WorkingSetSize);
                    }
                    Sleep(5);
                }
            });
            try {
                const auto output =
                    RunOnline(Provider::MyMemory, OnlineConfig{},
                              toChinese ? L"Hello world." : L"你好，世界。", toChinese, cancel);
                std::cout << "Public free API direction=" << (toChinese ? "en-zh" : "zh-en")
                          << " elapsed_ms=" << GetTickCount64() - start << " result_units=" << output.size()
                          << " cpu_ms=" << (cpu() - cpuStart) / 10000.0
                          << " sampled_private=" << peakPrivate.load()
                          << " sampled_working_set=" << peakWorkingSet.load() << '\n';
            } catch (const std::wstring &error) {
                std::wcerr << error << '\n';
                ok = false;
            }
            sampler.request_stop();
        }
    }
    CloseHandle(cancel);
    std::cout << "Online provider policies, private configuration and transport lifecycle=" << ok << '\n';
    return ok ? 0 : 1;
}
