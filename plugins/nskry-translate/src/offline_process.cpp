#include "offline_process.h"
#include "core/json.h"
#include "translation_policy.h"
#include <fstream>
#include <shlobj.h>
#include <vector>
#include <wincrypt.h>
#include <winrt/base.h>

namespace nskry::translate {
namespace {
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct ProtectedBlob {
    DATA_BLOB value{};
    ~ProtectedBlob() {
        if (value.pbData)
            LocalFree(value.pbData);
    }
};
} // namespace
WorkerReply RunOffline(const std::filesystem::path &pluginDirectory, HANDLE cancel, const std::wstring &text,
                       const std::wstring &root, bool toChinese) {
    PWSTR local{};
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local));
    std::filesystem::path parent;
    try {
        parent = std::filesystem::path(local) / L"Nskry" / L"cache" / L"translation";
    } catch (...) {
        CoTaskMemFree(local);
        throw;
    }
    CoTaskMemFree(local);
    std::filesystem::create_directories(parent);
    GUID id{};
    winrt::check_hresult(CoCreateGuid(&id));
    wchar_t name[40]{};
    StringFromGUID2(id, name, 40);
    const auto directory = parent / name;
    if (!std::filesystem::create_directory(directory))
        winrt::throw_hresult(E_FAIL);
    struct Files {
        std::filesystem::path root;
        ~Files() {
            DeleteFileW((root / L"input.txt").c_str());
            DeleteFileW((root / L"reply.json").c_str());
            RemoveDirectoryW(root.c_str());
        }
    } files{directory};
    std::string utf8;
    if (!nskry::json::ToUtf8(text, utf8))
        winrt::throw_hresult(E_INVALIDARG);
    {
        DATA_BLOB plain{DWORD(utf8.size()), reinterpret_cast<BYTE *>(utf8.data())};
        ProtectedBlob encrypted;
        if (!CryptProtectData(&plain, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                              &encrypted.value))
            winrt::throw_last_error();
        std::ofstream input(directory / L"input.txt", std::ios::binary);
        input.write(reinterpret_cast<const char *>(encrypted.value.pbData), encrypted.value.cbData);
        if (!input)
            winrt::throw_hresult(E_FAIL);
    }
    const auto executable = pluginDirectory / L"engine" / L"nskry-translate-worker.exe";
    if (!std::filesystem::is_regular_file(executable))
        throw TranslationFailure{
            L"Translation engine missing. Install a complete plugin package with its engine folder."};
    const auto quote = [](const std::wstring &value) {
        if (value.find(L'"') != value.npos)
            winrt::throw_hresult(E_INVALIDARG);
        // Windows argv quoting also escapes trailing backslashes.
        std::wstring result = L"\"" + value;
        for (auto i = value.rbegin(); i != value.rend() && *i == L'\\'; ++i)
            result += L'\\';
        return result + L"\"";
    };
    auto command = quote(executable.wstring()) + L" " + quote(root) + (toChinese ? L" en-zh " : L" zh-en ") +
                   quote((directory / L"input.txt").wstring()) + L" " +
                   quote((directory / L"reply.json").wstring());
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    if (!job.value)
        winrt::throw_last_error();
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    // Measured cold-load private peak is ~650MiB despite 77MiB INT8 weights.
    // Keep a hard bound with headroom for a full paragraph; never infer RAM
    // demand from download size. The process exits after this single request.
    limits.ProcessMemoryLimit = 1024ULL * 1024 * 1024;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        winrt::throw_last_error();
    SIZE_T attributeBytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<uint8_t> storage(attributeBytes);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
        winrt::throw_last_error();
    struct Attributes {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~Attributes() {
            DeleteProcThreadAttributeList(value);
        }
    } attributeGuard{attributes};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job.value, sizeof(HANDLE),
                                   nullptr, nullptr))
        winrt::throw_last_error();
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS |
                            EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process))
        winrt::throw_last_error();
    Handle child{process.hProcess}, thread{process.hThread};
    if (ResumeThread(thread.value) == DWORD(-1)) {
        TerminateProcess(child.value, ERROR_CANCELLED); // Only the child just created by this task.
        WaitForSingleObject(child.value, 2000);
        winrt::throw_hresult(E_FAIL);
    }
    HANDLE signals[]{cancel, child.value};
    const auto wait = WaitForMultipleObjects(2, signals, FALSE, TaskTimeoutMs);
    if (wait != WAIT_OBJECT_0 + 1) {
        TerminateJobObject(job.value, ERROR_CANCELLED);
        WaitForSingleObject(child.value, 2000);
        winrt::throw_hresult(HRESULT_FROM_WIN32(wait == WAIT_OBJECT_0 ? ERROR_CANCELLED : ERROR_TIMEOUT));
    }
    const auto reply = directory / L"reply.json";
    DWORD exitCode{};
    if (!GetExitCodeProcess(child.value, &exitCode))
        winrt::throw_last_error();
    // Exit 1 is a structured worker failure, not a reason to discard its reply.
    // Killed/crashed workers can lack a reply and must not publish partial text.
    if (!std::filesystem::is_regular_file(reply))
        throw TranslationFailure{
            L"Translation worker exited without a complete reply. Check available memory and the runtime."};
    if (std::filesystem::file_size(reply) > MaxReplyBytes)
        winrt::throw_hresult(E_BOUNDS);
    std::ifstream replyStream(reply, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(replyStream),
                                     std::istreambuf_iterator<char>()};
    DATA_BLOB encrypted{DWORD(bytes.size()), const_cast<BYTE *>(bytes.data())};
    ProtectedBlob plain;
    if (!CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &plain.value))
        winrt::throw_last_error();
    // JSON worker replies use ASCII escapes; no locale-dependent decoding.
    std::wstring json(plain.value.pbData, plain.value.pbData + plain.value.cbData);
    auto decoded = DecodeWorkerReply(json, exitCode);
    if (!decoded.success)
        throw TranslationFailure{std::move(decoded.error)};
    return decoded;
}
} // namespace nskry::translate
