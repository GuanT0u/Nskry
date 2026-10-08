#include "ocr_inference.h"
#include "win_handle.h"
#include <psapi.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace {
bool WriteAll(HANDLE handle, const std::vector<uint8_t>& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &written, nullptr) || !written) return false;
        offset += written;
    }
    return true;
}
std::filesystem::path Directory() {
    std::array<wchar_t, 32768> path{};
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) throw std::runtime_error("Cannot locate OCR models");
    return std::filesystem::path(path.data()).parent_path();
}
std::wstring ErrorText(const char* text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (size <= 1 || size > 10000) return L"Offline OCR failed.";
    std::wstring value(size, L'\0'); MultiByteToWideChar(CP_UTF8, 0, text, -1, value.data(), size);
    value.pop_back(); return value;
}
void Usage(nskry::ocr::wire::Reply& reply) {
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        reply.peakWorkingSetBytes = memory.PeakWorkingSetSize;
        reply.peakCommitBytes = memory.PeakPagefileUsage;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto time = [](FILETIME value) { return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
        reply.cpuMs = static_cast<double>(time(kernel) + time(user)) / 10000;
    }
}
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // Hidden child process. No named shared objects, files containing captures,
    // browser, network, UI or persistent engine sessions.
    nskry::ocr::wire::Reply reply;
    try {
        if (argc != 3) return 2;
        wchar_t* end{};
        const auto rawHandle = wcstoull(argv[1], &end, 10);
        if (!rawHandle || !end || *end) return 2;
        const auto threads = wcstoul(argv[2], &end, 10);
        if (!end || *end || threads < 1 || threads > 4) return 2;
        nskry::ocr::Handle mapping(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(rawHandle)));
        nskry::ocr::wire::Input input;
        {
            nskry::ocr::MappingView header{MapViewOfFile(mapping.Get(), FILE_MAP_READ, 0, 0, sizeof(input))};
            if (!header.value) throw std::runtime_error("Cannot read OCR input");
            std::memcpy(&input, header.value, sizeof(input));
        }
        if (!nskry::ocr::wire::ValidInput(input)) throw std::runtime_error("Invalid OCR input dimensions");
        const size_t bytes = sizeof(input) + static_cast<size_t>(input.pixelBytes);
        nskry::ocr::MappingView image{MapViewOfFile(mapping.Get(), FILE_MAP_READ, 0, 0, bytes)};
        if (!image.value) throw std::runtime_error("Truncated OCR input");
        const auto* pixels = static_cast<const uint8_t*>(image.value) + sizeof(input);
        reply = nskry::ocr::RecognizeModel({pixels, input.pixelBytes}, input.width, input.height, Directory() / L"models", threads);
    } catch (const std::exception& error) { reply.error = ErrorText(error.what()); }
    catch (...) { reply.error = L"Offline OCR failed."; }
    Usage(reply);
    try {
        return WriteAll(GetStdHandle(STD_OUTPUT_HANDLE), nskry::ocr::wire::Encode(reply)) ? 0 : 3;
    } catch (...) { return 4; }
}
