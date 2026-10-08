#include "ocr_process.h"
#include <chrono>
#include <iostream>

using namespace nskry::ocr;
bool Check(bool value, const char* name) { if (!value) std::cerr << "FAIL: " << name << '\n'; return value; }
bool Exited(DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return true;
    const bool value = WaitForSingleObject(process, 0) == WAIT_OBJECT_0; CloseHandle(process); return value;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 64; info.bmiHeader.biHeight = -32;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* bits{}; HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return 3;
    bool okay = true; DWORD before{}, after{};
    auto result = RunModelProcess(bitmap, 64, 32, L"not-a-real-ocr-worker.exe", [] { return false; });
    okay &= Check(result.status == ModelStatus::Unavailable && !result.processId, "missing executable is explicit");
    result = RunModelProcess(bitmap, 0, 32, argv[1], [] { return false; });
    okay &= Check(result.status == ModelStatus::Failed && !result.processId, "invalid input launches no process");
    result = RunModelProcess(bitmap, 63, 32, argv[1], [] { return false; });
    okay &= Check(result.status == ModelStatus::Failed && !result.processId, "bitmap size mismatch launches no process");
    SetEnvironmentVariableW(L"NSKRY_OCR_FAKE_MODE", L"garbage");
    // First GetDC/process creation can initialize persistent Windows handles.
    // Warm that system path before checking for growth across repeated requests.
    result = RunModelProcess(bitmap, 64, 32, argv[1], [] { return false; });
    okay &= Check(result.status == ModelStatus::Failed && result.workerExited && Exited(result.processId), "malformed reply rejected");
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (int i = 0; i < 3; ++i) {
        result = RunModelProcess(bitmap, 64, 32, argv[1], [] { return false; });
        okay &= Check(result.status == ModelStatus::Failed && result.workerExited && Exited(result.processId), "repeated malformed replies reaped");
    }
    SetEnvironmentVariableW(L"NSKRY_OCR_FAKE_MODE", L"hang");
    auto start = std::chrono::steady_clock::now();
    result = RunModelProcess(bitmap, 64, 32, argv[1], [] { return false; }, 120);
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    okay &= Check(result.status == ModelStatus::Failed && result.processId && Exited(result.processId) && ms < 2000,
                  "timeout kills and reaps worker");
    start = std::chrono::steady_clock::now();
    result = RunModelProcess(bitmap, 64, 32, argv[1], [&] {
        return std::chrono::steady_clock::now() - start > std::chrono::milliseconds(120);
    });
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    okay &= Check(result.status == ModelStatus::Canceled && result.processId && Exited(result.processId) && ms < 2000,
                  "superseded request kills worker");
    SetEnvironmentVariableW(L"NSKRY_OCR_FAKE_MODE", nullptr);
    GetProcessHandleCount(GetCurrentProcess(), &after);
    if (after > before) std::cerr << "Handles before=" << before << " after=" << after << '\n';
    okay &= Check(after <= before, "failure paths release handles");
    DeleteObject(bitmap); return okay ? 0 : 1;
}
