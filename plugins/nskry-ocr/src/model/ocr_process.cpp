#include "ocr_process.h"
#include "win_handle.h"
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace nskry::ocr {
namespace {
using Clock = std::chrono::steady_clock;
struct Child {
    Handle job, process;
    bool Stop() {
        if (process && WaitForSingleObject(process.Get(), 0) != WAIT_OBJECT_0) {
            if (job) TerminateJobObject(job.Get(), ERROR_CANCELLED);
            // Assignment can fail while the child is still suspended and not
            // yet in the job. Always terminate that process as well.
            if (WaitForSingleObject(process.Get(), 0) != WAIT_OBJECT_0) TerminateProcess(process.Get(), ERROR_CANCELLED);
            return WaitForSingleObject(process.Get(), 5000) == WAIT_OBJECT_0;
        }
        return static_cast<bool>(process);
    }
    ~Child() { Stop(); }
};
struct Attributes {
    std::vector<uint8_t> data;
    LPPROC_THREAD_ATTRIBUTE_LIST list{};
    ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
    bool Init() {
        SIZE_T size{}; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        data.resize(size);
        list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(data.data());
        if (InitializeProcThreadAttributeList(list, 1, 0, &size)) return true;
        list = nullptr; return false;
    }
};
ModelResult Failed(const wchar_t* message) {
    ModelResult result; result.reply.error = message; return result;
}
}
ModelResult RunModelProcess(HBITMAP bitmap, int width, int height,
    const std::filesystem::path& executable, const std::function<bool()>& canceled, DWORD timeoutMs) {
    const auto started = Clock::now();
    const auto isCanceled = [&] { return canceled && canceled(); };
    if (isCanceled()) { ModelResult value; value.status = ModelStatus::Canceled; return value; }
    std::error_code error;
    if (!std::filesystem::is_regular_file(executable, error)) {
        auto value = Failed(L"The offline OCR engine is missing. Reinstall the complete OCR plugin package, or select Fast OCR.");
        value.status = ModelStatus::Unavailable; return value;
    }
    wire::Input input;
    input.width = width; input.height = height;
    const uint64_t pixels = static_cast<uint64_t>(input.width) * input.height;
    if (pixels <= wire::MaxPixels) input.pixelBytes = static_cast<uint32_t>(pixels * 4);
    if (!wire::ValidInput(input)) return Failed(L"This image is too large for offline OCR. Select a smaller area (up to 16 megapixels).");
    BITMAP bitmapInfo{};
    if (!bitmap || GetObjectW(bitmap, sizeof(bitmapInfo), &bitmapInfo) != sizeof(bitmapInfo) ||
        bitmapInfo.bmWidth != width || std::abs(bitmapInfo.bmHeight) != height)
        return Failed(L"The OCR image dimensions do not match its bitmap.");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(input) + input.pixelBytes, nullptr));
    if (!mapping) return Failed(L"Could not allocate the OCR image buffer.");
    {
        MappingView view{MapViewOfFile(mapping.Get(), FILE_MAP_WRITE, 0, 0, sizeof(input) + input.pixelBytes)};
        if (!view.value) return Failed(L"Could not prepare the OCR image buffer.");
        std::memcpy(view.value, &input, sizeof(input));
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        const int rows = dc ? GetDIBits(dc, bitmap, 0, height, static_cast<uint8_t*>(view.value) + sizeof(input), &info, DIB_RGB_COLORS) : 0;
        if (dc) ReleaseDC(nullptr, dc);
        if (rows != height) return Failed(L"Could not read the image for OCR.");
    }
    HANDLE rawRead{}, rawWrite{};
    if (!CreatePipe(&rawRead, &rawWrite, &security, 65536)) return Failed(L"Could not create the OCR result channel.");
    Handle read(rawRead), write(rawWrite);
    if (!SetHandleInformation(read.Get(), HANDLE_FLAG_INHERIT, 0)) return Failed(L"Could not protect the OCR result channel.");
    Handle nullFile(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!nullFile) return Failed(L"Could not initialize the OCR process.");
    Attributes attributes;
    HANDLE inherited[]{mapping.Get(), write.Get(), nullFile.Get()};
    if (!attributes.Init() || !UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr)) return Failed(L"Could not limit OCR handle inheritance.");
    Child child;
    child.job.Reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(1024) * 1024 * 1024;
    if (!child.job || !SetInformationJobObject(child.job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return Failed(L"Could not bound OCR process resources.");
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const int threads = pixels > 500000 && system.dwNumberOfProcessors >= 8 ? 4 :
        (system.dwNumberOfProcessors >= 2 ? 2 : 1);
    std::wstring command = L"\"" + executable.wstring() + L"\" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(mapping.Get())) + L" " + std::to_wstring(threads);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = write.Get(); startup.StartupInfo.hStdError = nullFile.Get(); startup.StartupInfo.hStdInput = nullFile.Get();
    startup.lpAttributeList = attributes.list;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
            executable.parent_path().c_str(), &startup.StartupInfo, &process))
        return Failed(L"Could not start offline OCR. Reinstall the complete plugin package, or select Fast OCR.");
    child.process.Reset(process.hProcess); Handle thread(process.hThread);
    if (!AssignProcessToJobObject(child.job.Get(), child.process.Get()) || ResumeThread(thread.Get()) == static_cast<DWORD>(-1))
        return Failed(L"Could not start the bounded OCR process.");
    write.Reset(); // EOF now reflects the child's lifetime, not a parent writer.
    ModelResult result; result.processId = process.dwProcessId; result.threads = threads;
    const auto finish = [&](ModelResult value) {
        value.workerExited = child.Stop();
        value.elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        return value;
    };
    std::vector<uint8_t> bytes;
    bool exited = false;
    for (;;) {
        if (isCanceled()) { result.status = ModelStatus::Canceled; return finish(std::move(result)); }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        if (elapsed >= timeoutMs) { result.reply.error = L"Offline OCR timed out. Try a smaller area or select Fast OCR."; return finish(std::move(result)); }
        DWORD available{};
        if (PeekNamedPipe(read.Get(), nullptr, 0, nullptr, &available, nullptr) && available) {
            if (available > wire::MaxReplyBytes - bytes.size()) { result.reply.error = L"The OCR process returned too much data."; return finish(std::move(result)); }
            const size_t offset = bytes.size();
            bytes.resize(offset + available);
            DWORD received{};
            if (!ReadFile(read.Get(), bytes.data() + offset, available, &received, nullptr)) { result.reply.error = L"Could not read the OCR result."; return finish(std::move(result)); }
            bytes.resize(offset + received);
            continue;
        }
        if (exited) break;
        const auto wait = WaitForSingleObject(child.process.Get(), 10);
        if (wait == WAIT_FAILED) { result.reply.error = L"Could not wait for the OCR process."; return finish(std::move(result)); }
        exited = wait == WAIT_OBJECT_0;
    }
    DWORD code{};
    result.workerExited = exited;
    result.elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    if (!GetExitCodeProcess(child.process.Get(), &code) || code != 0 || !wire::Decode(bytes, width, height, result.reply)) {
        result.reply.error = L"The offline OCR process stopped unexpectedly. Reinstall its runtime files, or select Fast OCR.";
        return finish(std::move(result));
    }
    result.status = result.reply.error.empty() ? ModelStatus::Success : ModelStatus::Failed;
    return finish(std::move(result));
}
} // namespace nskry::ocr
