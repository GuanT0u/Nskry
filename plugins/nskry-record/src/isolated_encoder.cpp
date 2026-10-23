#include "isolated_encoder.h"
#include "pixel_conversion.h"
#include "record_policy.h"
#include "record_worker_protocol.h"
#include <mferror.h>
#include <vector>
#include <winrt/base.h>

namespace nskry::record {
namespace {
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
};
struct View {
    wire::Header *value{};
    ~View() {
        if (value)
            UnmapViewOfFile(value);
    }
};
struct Attributes {
    std::vector<uint8_t> bytes;
    LPPROC_THREAD_ATTRIBUTE_LIST value{};
    Attributes() {
        SIZE_T size{};
        InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        bytes.resize(size);
        auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        if (!InitializeProcThreadAttributeList(list, 2, 0, &size))
            winrt::throw_last_error();
        value = list;
    }
    ~Attributes() {
        if (value)
            DeleteProcThreadAttributeList(value);
    }
};
// Resolve relative to the DLL that contains this code (test executable when
// statically linked), never PATH/current-directory search or a mutable env var.
std::filesystem::path WorkerPath() {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&WorkerPath), &module))
        winrt::throw_last_error();
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
    if (!size || size >= path.size())
        winrt::throw_hresult(E_FAIL);
    path.resize(size);
    return std::filesystem::path(path).parent_path() / L"nskry-record-worker.exe";
}
} // namespace
struct IsolatedEncoder::Channel {
    Handle mapping, request, response, job, process;
    View view;
    DWORD pid{};
    uint32_t pixelBytes{}, width{}, height{}, fps{};
    uint64_t sequence{};
    HANDLE canceled{};
    DWORD timeout{};
    bool broken = false, closed = false;

    ~Channel() {
        Close();
    }
    bool Close() noexcept {
        if (!process.value)
            return true;
        if (closed)
            return WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0;
        closed = true;
        if (canceled && WaitForSingleObject(canceled, 0) == WAIT_OBJECT_0)
            broken = true;
        if (!broken && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) {
            try {
                Call(wire::Command::Close, 1500);
            } catch (...) {
            }
        }
        if (WaitForSingleObject(process.value, broken ? 0 : 1500) != WAIT_OBJECT_0) {
            // Only this object's explicitly created child/job, never a user PID.
            TerminateJobObject(job.value, ERROR_CANCELLED);
            WaitForSingleObject(process.value, 2000);
        }
        return WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0;
    }
    void Call(wire::Command command, DWORD waitMs = 0) {
        if (broken || (closed && command != wire::Command::Close))
            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE));
        auto &h = *view.value;
        h.command = command;
        h.request = ++sequence;
        h.result = E_PENDING;
        h.error[0] = 0;
        MemoryBarrier();
        if (!SetEvent(request.value))
            winrt::throw_last_error();
        // Cancellation wins even if a completed response is also signaled.
        HANDLE signals[]{canceled, response.value, process.value};
        const DWORD count = canceled ? 3 : 2;
        const DWORD result =
            WaitForMultipleObjects(count, canceled ? signals : signals + 1, FALSE, waitMs ? waitMs : timeout);
        const DWORD responseIndex = canceled ? 1 : 0;
        if (result != WAIT_OBJECT_0 + responseIndex) {
            broken = true; // No new request may race an unfinished/failed request.
            if (canceled && result == WAIT_OBJECT_0)
                throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED), L"Recording canceled.");
            if (result == WAIT_TIMEOUT)
                throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Recording encoder timed out.");
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE),
                                       L"Recording encoder exited unexpectedly.");
        }
        MemoryBarrier();
        if (h.response != sequence || !wire::ValidHeader(h) || h.width != width || h.height != height ||
            h.pixelBytes != pixelBytes || h.fps != fps) {
            broken = true;
            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        }
        h.error[511] = 0;
        if (FAILED(h.result))
            throw winrt::hresult_error(h.result, h.error);
    }
};
IsolatedEncoder::IsolatedEncoder(HANDLE canceled, std::filesystem::path executable, DWORD operationTimeout)
    : m_canceled(canceled), m_executable(std::move(executable)), m_timeout(operationTimeout) {
}
IsolatedEncoder::~IsolatedEncoder() = default;
void IsolatedEncoder::Open(const std::wstring &path, UINT width, UINT height, UINT fps, bool hardware, uint32_t bitrate,
                           bool gif, bool preview, UINT sourceWidth, UINT sourceHeight) {
    if (bitrate && (bitrate < 500000 || bitrate > 24000000))
        winrt::throw_hresult(E_INVALIDARG);
    if (m_channel || !ValidFrameFormat(width, height, fps) || path.empty() || path.size() >= 32768 ||
        path.find(L'\0') != std::wstring::npos || !m_timeout ||
        (preview && (width > 640 || height > 360 || !ValidFrameFormat(sourceWidth, sourceHeight, fps))))
        winrt::throw_hresult(E_INVALIDARG);
    if (m_canceled && WaitForSingleObject(m_canceled, 0) == WAIT_OBJECT_0)
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    const auto target = std::filesystem::absolute(path).wstring();
    if (target.size() >= 32768)
        winrt::throw_hresult(E_INVALIDARG);
    const auto executable = m_executable.empty() ? WorkerPath() : std::filesystem::absolute(m_executable);
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
                                   L"Recording encoder is missing. Install the complete recording plugin package.");
    m_channel = std::make_unique<Channel>();
    auto &c = *m_channel;
    c.canceled = m_canceled;
    c.timeout = m_timeout;
    c.pixelBytes = gif || preview ? width * height * 4 : width * height * 3 / 2;
    c.width = width;
    c.height = height;
    c.fps = fps;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    c.mapping.value = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
                                         DWORD(sizeof(wire::Header)) + c.pixelBytes, nullptr);
    c.request.value = CreateEventW(&security, FALSE, FALSE, nullptr);
    c.response.value = CreateEventW(&security, FALSE, FALSE, nullptr);
    c.job.value = CreateJobObjectW(nullptr, nullptr);
    if (!c.mapping.value || !c.request.value || !c.response.value || !c.job.value)
        winrt::throw_last_error();
    c.view.value = static_cast<wire::Header *>(
        MapViewOfFile(c.mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(wire::Header) + c.pixelBytes));
    if (!c.view.value)
        winrt::throw_last_error();
    auto &h = *new (c.view.value) wire::Header;
    h.width = width;
    h.height = height;
    h.fps = fps;
    h.bitrate = bitrate;
    h.gif = gif;
    h.preview = preview;
    h.sourceWidth = sourceWidth;
    h.sourceHeight = sourceHeight;
    h.hardware = hardware;
    h.pixelBytes = c.pixelBytes;
    wcscpy_s(h.path, target.c_str());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = 512ULL * 1024 * 1024;
    if (!SetInformationJobObject(c.job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        winrt::throw_last_error();
    Attributes attributes;
    HANDLE inherited[]{c.mapping.value, c.request.value, c.response.value};
    if (!UpdateProcThreadAttribute(attributes.value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                   nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attributes.value, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &c.job.value, sizeof(HANDLE),
                                   nullptr, nullptr))
        winrt::throw_last_error();
    auto command = L"\"" + executable.wstring() + L"\" " +
                   std::to_wstring(reinterpret_cast<uintptr_t>(c.mapping.value)) + L" " +
                   std::to_wstring(reinterpret_cast<uintptr_t>(c.request.value)) + L" " +
                   std::to_wstring(reinterpret_cast<uintptr_t>(c.response.value));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.value;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT |
                            BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process))
        winrt::throw_last_error();
    c.process.value = process.hProcess;
    c.pid = process.dwProcessId;
    Handle thread{process.hThread};
    // Job assignment is atomic with creation, so a parent crash cannot orphan a suspended child.
    if (ResumeThread(thread.value) == DWORD(-1)) {
        c.broken = true;
        winrt::throw_last_error();
    }
    c.Call(wire::Command::Open);
    h.backend[127] = 0;
    m_backend = h.backend;
}
void IsolatedEncoder::Write(const uint8_t *bgra, int64_t time, int64_t duration) {
    if (!m_channel || !bgra || m_channel->view.value->preview || m_finalizationAttempted ||
        !ValidSampleTime(time, duration, m_lastEnd))
        winrt::throw_hresult(E_INVALIDARG);
    auto &c = *m_channel;
    if (c.broken || c.closed)
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE));
    auto &h = *c.view.value;
    // The sole slot belongs to this worker until Call signals it to the child.
    // Converted NV12 needs 3/8 of the BGRA transfer size, no unbounded pipe queue.
    m_payloadReady = false;
    if (h.gif)
        memcpy(&h + 1, bgra, c.pixelBytes);
    else
        BgraToNv12(bgra, reinterpret_cast<uint8_t *>(&h + 1), c.width, c.height);
    SubmitFrame(time, duration);
}
void IsolatedEncoder::Repeat(int64_t time, int64_t duration) {
    if (!m_payloadReady)
        winrt::throw_hresult(E_INVALIDARG);
    SubmitFrame(time, duration);
}
void IsolatedEncoder::SubmitFrame(int64_t time, int64_t duration) {
    if (!m_channel || m_channel->view.value->preview || m_finalizationAttempted ||
        !ValidSampleTime(time, duration, m_lastEnd))
        winrt::throw_hresult(E_INVALIDARG);
    auto &c = *m_channel;
    if (c.broken || c.closed)
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE));
    auto &h = *c.view.value;
    m_payloadReady = false;
    h.time = time;
    h.duration = duration;
    c.Call(wire::Command::Frame);
    m_payloadReady = true;
    ++m_samples;
    m_lastEnd = time + duration;
}
void IsolatedEncoder::Preview(int64_t time, uint8_t *pixels, uint32_t capacity) {
    if (!m_channel || !m_channel->view.value->preview || time < 0 || !pixels || capacity < m_channel->pixelBytes)
        winrt::throw_hresult(E_INVALIDARG);
    auto &h = *m_channel->view.value;
    h.time = time;
    m_channel->Call(wire::Command::Preview);
    memcpy(pixels, &h + 1, m_channel->pixelBytes);
}
void IsolatedEncoder::Finalize() {
    if (!m_channel || !m_samples || m_finalizationAttempted)
        winrt::throw_hresult(MF_E_INVALIDREQUEST);
    m_finalizationAttempted = true;
    m_channel->Call(wire::Command::Finalize);
}
void IsolatedEncoder::Trim(const std::wstring &input, int64_t start, int64_t end) {
    if (!m_channel || m_samples || input.empty() || input.size() >= 32768 || start < 0 || end <= start)
        winrt::throw_hresult(E_INVALIDARG);
    auto &c = *m_channel;
    auto &h = *c.view.value;
    wcscpy_s(h.inputPath, input.c_str());
    h.trimStart = start;
    h.trimEnd = end;
    const auto timeout = c.timeout;
    c.timeout = 300000; // Explicit bounded export, cancellation interrupts IPC immediately.
    try {
        c.Call(wire::Command::Trim);
    } catch (...) {
        c.timeout = timeout;
        throw;
    }
    c.timeout = timeout;
    m_samples = h.samples;
    m_lastEnd = end - start;
}
bool IsolatedEncoder::Close() noexcept {
    return !m_channel || m_channel->Close();
}
bool IsolatedEncoder::CreatedFile() const {
    return m_channel && m_channel->view.value && InterlockedCompareExchange(&m_channel->view.value->created, 0, 0) == 1;
}
bool IsolatedEncoder::Finalized() const {
    return m_channel && m_channel->view.value &&
           InterlockedCompareExchange(&m_channel->view.value->finalized, 0, 0) == 1;
}
DWORD IsolatedEncoder::ProcessId() const {
    return m_channel ? m_channel->pid : 0;
}
uint64_t IsolatedEncoder::CpuTime() const {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!m_channel || !m_channel->process.value ||
        !GetProcessTimes(m_channel->process.value, &creation, &exit, &kernel, &user))
        return 0;
    return (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
           (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
}
} // namespace nskry::record
