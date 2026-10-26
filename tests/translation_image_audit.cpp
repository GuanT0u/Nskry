// Explicit, opt-in audit of already packaged DLLs and reviewed PUBLIC images.
// Not a production feature; never added as an online CTest or installed plugin.
#include "core/json.h"
#include "nskry_actions.h"
#include "nskry_plugin.h"
#include "nskry_text_translation.h"
#include <algorithm>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <gdiplus.h>
#include <iostream>
#include <map>
#include <psapi.h>
#include <thread>
#include <tlhelp32.h>
#include <winrt/base.h>

using nskry::json::Value;
namespace {
std::wstring Text(HWND control) {
    std::wstring text(GetWindowTextLengthW(control) + 1, 0);
    const auto length = GetWindowTextW(control, text.data(), int(text.size()));
    text.resize(length);
    return text;
}
std::wstring Hash(const std::filesystem::path &file) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    struct Cleanup {
        BCRYPT_ALG_HANDLE &algorithm;
        BCRYPT_HASH_HANDLE &hash;
        ~Cleanup() {
            if (hash)
                BCryptDestroyHash(hash);
            if (algorithm)
                BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } cleanup{algorithm, hash};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("SHA-256 unavailable");
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Fixture or DLL unavailable");
    char buffer[65536];
    while (stream) {
        stream.read(buffer, sizeof(buffer));
        if (stream.gcount() &&
            BCryptHashData(hash, reinterpret_cast<BYTE *>(buffer), ULONG(stream.gcount()), 0) < 0)
            throw std::runtime_error("SHA-256 read failed");
    }
    if (!stream.eof())
        throw std::runtime_error("Fixture read failed");
    BYTE digest[32]{};
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0)
        throw std::runtime_error("SHA-256 failed");
    std::wstring result;
    for (const auto byte : digest) {
        result += L"0123456789abcdef"[byte >> 4];
        result += L"0123456789abcdef"[byte & 15];
    }
    return result;
}
HWND Window() {
    HWND result{};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM value) -> BOOL {
            wchar_t name[100]{};
            GetClassNameW(hwnd, name, 100);
            if (!wcscmp(name, L"NskryTranslateV1"))
                *reinterpret_cast<HWND *>(value) = hwnd;
            return TRUE;
        },
        LPARAM(&result));
    return result;
}
bool allowUpload{};
unsigned dialogs{};
void CALLBACK AuthorizePublicFixture(HWND, UINT, UINT_PTR, DWORD) {
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM) -> BOOL {
            if (Text(hwnd) == L"Confirm online text upload") {
                ++dialogs;
                SendMessageW(hwnd, WM_COMMAND, allowUpload ? IDYES : IDNO, 0);
            }
            return TRUE;
        },
        0);
}
struct Registry {
    HKEY key{};
    std::wstring path;
    bool active{};
    Registry() {
        path = L"Software\\NskryTests\\ImageAudit-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64());
        DWORD created{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key,
                            &created) ||
            created != REG_CREATED_NEW_KEY || RegOverridePredefKey(HKEY_CURRENT_USER, key))
            throw std::runtime_error("Could not isolate test configuration");
        active = true;
    }
    ~Registry() {
        if (active)
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (key)
            RegCloseKey(key);
        if (active)
            RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); // Only this newly created test key.
    }
};
uint64_t Cpu(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        return 0;
    return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
           ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
}
struct Sampling {
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread thread;
    size_t hostPrivate{}, hostWorking{}, childPrivate{}, childWorking{}, ocrPrivate{}, ocrWorking{};
    size_t simultaneousPrivate{}, simultaneousWorking{}, simultaneousChildren{};
    std::map<DWORD, HANDLE> children, ocrChildren;
    const uint64_t cpuStart = Cpu(GetCurrentProcess());
    Sampling() {
        if (!stop)
            throw std::runtime_error("Could not create sampler event");
        thread = std::thread([this] {
            unsigned iteration{};
            do {
                PROCESS_MEMORY_COUNTERS_EX memory{};
                memory.cb = sizeof(memory);
                if (GetProcessMemoryInfo(GetCurrentProcess(),
                                         reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                                         sizeof(memory))) {
                    hostPrivate = (std::max)(hostPrivate, memory.PrivateUsage);
                    hostWorking = (std::max)(hostWorking, memory.WorkingSetSize);
                }
                if (iteration++ % 5 == 0) {
                    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                    if (snapshot != INVALID_HANDLE_VALUE) {
                        PROCESSENTRY32W entry{sizeof(entry)};
                        for (BOOL more = Process32FirstW(snapshot, &entry); more;
                             more = Process32NextW(snapshot, &entry)) {
                            const bool translation = !wcscmp(entry.szExeFile, L"nskry-translate-worker.exe");
                            const bool recognition = !wcscmp(entry.szExeFile, L"nskry-ocr-worker.exe");
                            auto &owned = recognition ? ocrChildren : children;
                            if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                                (translation || recognition) && !owned.count(entry.th32ProcessID)) {
                                const auto handle =
                                    OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE,
                                                FALSE, entry.th32ProcessID);
                                if (handle)
                                    owned.emplace(entry.th32ProcessID, handle);
                            }
                        }
                        CloseHandle(snapshot);
                    }
                }
                size_t livePrivate = memory.PrivateUsage, liveWorking = memory.WorkingSetSize, liveChildren{};
                for (const auto *owned : {&children, &ocrChildren}) {
                    for (const auto &[id, handle] : *owned) {
                        if (WaitForSingleObject(handle, 0) != WAIT_TIMEOUT)
                            continue;
                        ++liveChildren;
                        if (GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                                                 sizeof(memory))) {
                            auto &commit = owned == &children ? childPrivate : ocrPrivate;
                            auto &working = owned == &children ? childWorking : ocrWorking;
                            commit = (std::max)(commit, memory.PrivateUsage);
                            working = (std::max)(working, memory.PeakWorkingSetSize);
                            livePrivate += memory.PrivateUsage;
                            liveWorking += memory.WorkingSetSize;
                        }
                    }
                }
                simultaneousPrivate = (std::max)(simultaneousPrivate, livePrivate);
                simultaneousWorking = (std::max)(simultaneousWorking, liveWorking);
                simultaneousChildren = (std::max)(simultaneousChildren, liveChildren);
            } while (WaitForSingleObject(stop, 20) == WAIT_TIMEOUT);
        });
    }
    void Stop() {
        SetEvent(stop);
        if (thread.joinable())
            thread.join();
    }
    Value Metrics() {
        Stop();
        double cpu{}, ocrCpu{};
        bool exited = true;
        for (const auto &[id, handle] : children) {
            cpu += Cpu(handle) / 10000.0;
            exited &= WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
        }
        for (const auto &[id, handle] : ocrChildren) {
            ocrCpu += Cpu(handle) / 10000.0;
            exited &= WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
        }
        return Value(Value::Object{
            {L"host_cpu_ms", Value((Cpu(GetCurrentProcess()) - cpuStart) / 10000.0)},
            {L"host_private_sampled_bytes", Value(double(hostPrivate))},
            {L"host_working_sampled_bytes", Value(double(hostWorking))},
            {L"worker_count_observed", Value(double(children.size()))},
            {L"worker_private_sampled_bytes", Value(double(childPrivate))},
            {L"worker_working_peak_bytes", Value(double(childWorking))},
            {L"worker_cpu_ms", Value(cpu)},
            {L"ocr_worker_count_observed", Value(double(ocrChildren.size()))},
            {L"ocr_worker_private_sampled_bytes", Value(double(ocrPrivate))},
            {L"ocr_worker_working_peak_bytes", Value(double(ocrWorking))},
            {L"ocr_worker_cpu_ms", Value(ocrCpu)},
            {L"simultaneous_host_children_private_sampled_bytes", Value(double(simultaneousPrivate))},
            {L"simultaneous_host_children_working_sampled_bytes", Value(double(simultaneousWorking))},
            {L"simultaneous_children_observed", Value(double(simultaneousChildren))},
            {L"observed_workers_exited", Value(exited)}});
    }
    ~Sampling() {
        Stop();
        for (const auto &[id, handle] : children)
            CloseHandle(handle);
        for (const auto &[id, handle] : ocrChildren)
            CloseHandle(handle);
        CloseHandle(stop);
    }
};
struct Plugin {
    HMODULE dll{};
    decltype(&nskry_plugin_shutdown) shutdown{};
    ~Plugin() {
        if (shutdown)
            shutdown();
        if (dll)
            FreeLibrary(dll);
    }
};
} // namespace

int wmain(int argc, wchar_t **argv) {
    // DLL, images root, reviewed manifest, external models root or -, engine,
    // repeat count, new output JSON, optional explicit public-network consent.
    if (argc < 8 || argc > 10)
        return 2;
    const bool online = !wcscmp(argv[5], L"mymemory");
    bool consent{}, joinGoldEnglish{};
    for (int i = 8; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--allow-public-network"))
            consent = true;
        else if (!wcscmp(argv[i], L"--gold-soft-wrap-control"))
            joinGoldEnglish = true;
        else
            return 2;
    }
    const auto repetitions = wcstoul(argv[6], nullptr, 10);
    if ((!online && wcscmp(argv[5], L"offline")) || repetitions < 1 || repetitions > 3 ||
        (online && !consent))
        return 2;
    try {
        if (!std::filesystem::path(argv[1]).is_absolute() || std::filesystem::exists(argv[7]))
            throw std::runtime_error("Use an absolute packaged DLL and a NEW result file");
        Value review;
        unsigned schema{};
        if (!nskry::json::ParseUtf8File(argv[3], review) || !review.GetUnsigned(L"schema", schema) ||
            schema != 1)
            throw std::runtime_error("Invalid reviewed fixture manifest");
        const auto *fixtures = review.Find(L"fixtures");
        if (!fixtures || !fixtures->AsArray() || fixtures->AsArray()->size() != 2)
            throw std::runtime_error("Exactly two independently reviewed public images required");
        // Verify every image BEFORE loading the DLL or authorizing any upload.
        for (const auto &fixture : *fixtures->AsArray()) {
            std::wstring file, expectedHash;
            if (!fixture.GetString(L"file", file) || !fixture.GetString(L"sha256", expectedHash) ||
                std::filesystem::path(file).filename() != file ||
                Hash(std::filesystem::path(argv[2]) / file) != expectedHash)
                throw std::runtime_error("Public fixture hash mismatch; no upload is authorized");
        }
        Registry registry;
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        struct Apartment {
            ~Apartment() {
                winrt::uninit_apartment();
            }
        } apartment;
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token{};
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
            return 3;
        struct Imaging {
            ULONG_PTR token;
            ~Imaging() {
                Gdiplus::GdiplusShutdown(token);
            }
        } imaging{token};
        Plugin plugin;
        plugin.dll = LoadLibraryExW(argv[1], nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!plugin.dll)
            throw std::runtime_error("Packaged DLL failed to load");
        const auto init =
            reinterpret_cast<decltype(&nskry_plugin_init)>(GetProcAddress(plugin.dll, "nskry_plugin_init"));
        plugin.shutdown =
            reinterpret_cast<decltype(plugin.shutdown)>(GetProcAddress(plugin.dll, "nskry_plugin_shutdown"));
        const auto execute = reinterpret_cast<decltype(&nskry_plugin_execute)>(
            GetProcAddress(plugin.dll, "nskry_plugin_execute"));
        const auto show = reinterpret_cast<NskryShowSettingsV1>(
            GetProcAddress(plugin.dll, "nskry_plugin_show_settings_v1"));
        const auto translate = reinterpret_cast<NskryTranslateTextV1>(
            GetProcAddress(plugin.dll, "nskry_plugin_translate_text_v1"));
        if (!init || !plugin.shutdown || !execute || !show || !translate || !init(nullptr) || !show(nullptr))
            throw std::runtime_error("Packaged plugin could not initialize its UI");
        const HWND window = Window();
        if (!window)
            throw std::runtime_error("No result window");
        SetWindowTextW(GetDlgItem(window, 12), argv[4]);
        SendDlgItemMessageW(window, 20, CB_SETCURSEL, online ? 3 : 0, 0);
        Value::Array results;
        bool allWorkflows = true, quotaReached = false;
        for (const auto &fixture : *fixtures->AsArray()) {
            std::wstring id, file, expected, target;
            fixture.GetString(L"id", id);
            fixture.GetString(L"file", file);
            fixture.GetString(L"expected_source", expected);
            fixture.GetString(L"target_language", target);
            for (unsigned index = 0; index <= repetitions; ++index) {
                if (quotaReached)
                    break;
                const bool control = index == repetitions;
                SetWindowTextW(GetDlgItem(window, 10), L"");
                dialogs = 0;
                allowUpload = online;
                const auto timer = SetTimer(nullptr, 0, 20, AuthorizePublicFixture);
                if (!timer)
                    throw std::runtime_error("Could not install test-only consent handler");
                const auto start = GetTickCount64();
                uint64_t sourceObserved{};
                int32_t accepted = NSKRY_TRANSLATION_ACCEPTED;
                Sampling sampling;
                if (control) {
                    auto controlText = expected;
                    if (joinGoldEnglish && id == L"english_phototest")
                        std::replace(controlText.begin(), controlText.end(), L'\n', L' ');
                    const NskryTextTranslationV1 request{sizeof(request), 1, nullptr, controlText.c_str(),
                                                         uint32_t(controlText.size())};
                    accepted = translate(&request, nullptr);
                    sourceObserved = GetTickCount64();
                } else {
                    HBITMAP bitmap{};
                    {
                        Gdiplus::Bitmap image((std::filesystem::path(argv[2]) / file).c_str());
                        unsigned width{}, height{};
                        fixture.GetUnsigned(L"width", width);
                        fixture.GetUnsigned(L"height", height);
                        if (image.GetLastStatus() != Gdiplus::Ok || image.GetWidth() != width ||
                            image.GetHeight() != height ||
                            image.GetHBITMAP(Gdiplus::Color::White, &bitmap) != Gdiplus::Ok)
                            throw std::runtime_error("Fixture image decode/dimension failure");
                    }
                    NskryHostContext context{sizeof(context), NSKRY_PLUGIN_API_VERSION};
                    context.capturedBitmap = bitmap;
                    execute(&context);
                    DeleteObject(bitmap); // Exercise borrowed-image ownership.
                }
                const auto deadline = start + 130000;
                size_t pumpCycles{};
                while (!IsWindowEnabled(GetDlgItem(window, 1)) && GetTickCount64() < deadline) {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    if (!sourceObserved && GetWindowTextLengthW(GetDlgItem(window, 10)))
                        sourceObserved = GetTickCount64();
                    ++pumpCycles;
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
                }
                KillTimer(nullptr, timer);
                allowUpload = false;
                const auto elapsed = GetTickCount64() - start;
                const auto source = Text(GetDlgItem(window, 10)), output = Text(GetDlgItem(window, 11)),
                           status = Text(GetDlgItem(window, 14));
                const bool done = IsWindowEnabled(GetDlgItem(window, 1));
                const bool success = done && !output.empty() && status.find(L": done.") != status.npos;
                allWorkflows &= success;
                quotaReached = status.find(L"quota") != status.npos && !success;
                results.emplace_back(Value::Object{
                    {L"fixture", Value(id)},
                    {L"path", Value(std::wstring(control ? (joinGoldEnglish && id == L"english_phototest"
                                                                ? L"manual_gold_soft_wrap_control"
                                                                : L"manual_gold_text_control")
                                                         : L"image_end_to_end"))},
                    {L"run", Value(double(control ? 1 : index + 1))},
                    {L"completed", Value(done)},
                    {L"workflow_success", Value(success)},
                    {L"accepted", Value(double(accepted))},
                    {L"upload_dialogs", Value(double(dialogs))},
                    {L"elapsed_ms", Value(double(elapsed))},
                    {L"source_observed_ms", Value(double(sourceObserved ? sourceObserved - start : 0))},
                    {L"ui_pump_cycles", Value(double(pumpCycles))},
                    {L"source", Value(source)},
                    {L"translation", Value(output)},
                    {L"status", Value(status)},
                    {L"direction_matches_reference",
                     Value(status.find(target == L"zh" ? L"English → 中文" : L"中文 → English") !=
                           status.npos)},
                    {L"resources", sampling.Metrics()}});
                std::cout << "Fixture=" << (id == L"english_phototest" ? "English" : "Chinese")
                          << " path=" << (control ? "gold-control" : "image")
                          << " run=" << (control ? 1 : index + 1) << " workflow=" << success
                          << " elapsed_ms=" << elapsed << '\n';
                if (!done) {
                    plugin.shutdown();
                    throw std::runtime_error("UI task exceeded the audit deadline");
                }
            }
        }
        Value::Object report{{L"schema", Value(1.0)},
                             {L"dll", Value(std::wstring(argv[1]))},
                             {L"dll_sha256", Value(Hash(argv[1]))},
                             {L"review_sha256", Value(Hash(argv[3]))},
                             {L"engine", Value(std::wstring(argv[5]))},
                             {L"quality_is_not_workflow_success", Value(true)},
                             {L"all_workflows_succeeded", Value(allWorkflows)},
                             {L"runs", Value(std::move(results))}};
        std::wstring json;
        std::string utf8;
        if (!nskry::json::Serialize(Value(std::move(report)), json) || !nskry::json::ToUtf8(json, utf8))
            throw std::runtime_error("Could not serialize audit");
        std::ofstream stream(std::filesystem::path(argv[7]), std::ios::binary);
        stream.write(utf8.data(), std::streamsize(utf8.size()));
        stream.close();
        if (!stream)
            throw std::runtime_error("Could not write audit result");
        return allWorkflows ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}
