#include "nskry_plugin.h"

#include <algorithm>
#include <chrono>
#include <gdiplus.h>
#include <iostream>
#include <thread>

namespace {

using PluginInit = int32_t (NSKRY_CALL*)(const NskryHostContext*);
using PluginExecute = void (NSKRY_CALL*)(const NskryHostContext*);
using PluginShutdown = void (NSKRY_CALL*)();
int s_copiedWidth{}, s_copiedHeight{};
int s_editedWidth{}, s_editedHeight{};

void Notify(const wchar_t*, int) {}
int32_t CopyToClipboard(HBITMAP bitmap) {
    BITMAP info{};
    if (::GetObjectW(bitmap, sizeof(info), &info) != sizeof(info)) return 0;
    s_copiedWidth = info.bmWidth; s_copiedHeight = std::abs(info.bmHeight);
    return 1; // Test callback does not touch the user's clipboard.
}
void OpenEditor(HBITMAP bitmap, int width, int height) {
    s_editedWidth = width; s_editedHeight = height;
    if (bitmap) ::DeleteObject(bitmap);
}

HWND FindOwnWindow(const wchar_t* wantedClass) {
    struct Search { const wchar_t* wanted; HWND result{}; } search{wantedClass};
    ::EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        DWORD pid{}; ::GetWindowThreadProcessId(window, &pid);
        if (pid != ::GetCurrentProcessId()) return TRUE;
        wchar_t name[64]{}; ::GetClassNameW(window, name, ARRAYSIZE(name));
        auto& search = *reinterpret_cast<Search*>(parameter);
        if (wcscmp(name, search.wanted) != 0) return TRUE;
        search.result = window; return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}
HWND FindOwnResult() { return FindOwnWindow(L"NskryOcrResult"); }
HWND WaitForResult(std::wstring& text) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message); ::DispatchMessageW(&message);
        }
        if (HWND result = FindOwnResult()) {
            HWND edit = ::FindWindowExW(result, nullptr, L"EDIT", nullptr);
            const int length = edit ? ::GetWindowTextLengthW(edit) : 0;
            text.resize(length + 1);
            if (edit) ::GetWindowTextW(edit, text.data(), length + 1);
            text.resize(length); return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return nullptr;
}
bool InClient(HWND parent, HWND child) {
    RECT bounds{}, client{}; ::GetClientRect(parent, &client);
    if (!child || !::IsWindowVisible(child) || !::GetWindowRect(child, &bounds)) return false;
    ::MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&bounds), 2);
    return bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right && bounds.bottom <= client.bottom;
}
bool Workflow(HWND& result, std::wstring& text, bool missingEngine) {
    // Public UI workflow: original bitmap callbacks, small-window controls,
    // engine switching, rotation. Only windows in this test process are used.
    ::SetWindowPos(result, nullptr, 0, 0, 640, 360, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    for (int id : {1004, 1005, 1006, 1007, 1003, 1008, 1009, 1010})
        if (!InClient(result, ::GetDlgItem(result, id))) return false;
    ::SendMessageW(result, WM_COMMAND, 1003, 0);
    ::SendMessageW(result, WM_COMMAND, 1007, 0);
    ::SendMessageW(result, WM_COMMAND, 1003, 0);
    if (s_copiedWidth != 640 || s_copiedHeight != 240 || s_editedWidth != 640 || s_editedHeight != 240) return false;
    if (missingEngine && text.find(L"missing") == std::wstring::npos) return false;
    ::SendMessageW(result, WM_COMMAND, 1010, 0); // Accurate -> Fast.
    result = WaitForResult(text);
    if (!result || text.find(L"Nskry") == std::wstring::npos) return false;
    if (missingEngine) return true;
    ::SendMessageW(result, WM_COMMAND, 1010, 0); // Fast -> Accurate.
    result = WaitForResult(text);
    if (!result || text.find(L"Nskry") == std::wstring::npos) return false;
    ::SendMessageW(result, WM_COMMAND, 1009, 0); // Rotate and re-recognize.
    result = WaitForResult(text);
    if (!result || text.find(L"Nskry") == std::wstring::npos) return false;
    ::SendMessageW(result, WM_COMMAND, 1003, 0);
    return s_copiedWidth == 240 && s_copiedHeight == 640;
}

HBITMAP MakeTestBitmap() {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 640;
    info.bmiHeader.biHeight = -240;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels{};
    HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC dc = ::CreateCompatibleDC(nullptr);
    HGDIOBJ previous = dc && bitmap ? ::SelectObject(dc, bitmap) : nullptr;
    if (!dc || !previous || previous == HGDI_ERROR) {
        if (dc) ::DeleteDC(dc);
        if (bitmap) ::DeleteObject(bitmap);
        return nullptr;
    }
    RECT bounds{ 0, 0, 640, 240 };
    ::FillRect(dc, &bounds, static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, RGB(0, 0, 0));
    ::DrawTextW(dc, L"Nskry OCR smoke test 123", -1, &bounds,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, previous);
    ::DeleteDC(dc);
    return bitmap;
}

HBITMAP LoadFixtureBitmap(const wchar_t* path) {
    Gdiplus::Bitmap image(path);
    if (image.GetLastStatus() != Gdiplus::Ok) return nullptr;
    HBITMAP bitmap{};
    return image.GetHBITMAP(Gdiplus::Color(0, 0, 0), &bitmap) == Gdiplus::Ok ? bitmap : nullptr;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool fixtureMode = argc == 5 && wcscmp(argv[2], L"--fixture") == 0;
    const bool workflow = (argc == 3 || argc == 4) && wcscmp(argv[2], L"--workflow") == 0;
    const bool missingEngine = workflow && argc == 4 && wcscmp(argv[3], L"--missing-engine") == 0;
    if (argc != 2 && !fixtureMode && !workflow) return 2;
    if (workflow) ::SetEnvironmentVariableW(L"NSKRY_OCR_ENGINE", L"accurate");
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken{};
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) return 2;
    HMODULE module = ::LoadLibraryW(argv[1]);
    if (!module) return 3;
    const auto init = reinterpret_cast<PluginInit>(::GetProcAddress(module, "nskry_plugin_init"));
    const auto execute = reinterpret_cast<PluginExecute>(::GetProcAddress(module, "nskry_plugin_execute"));
    const auto shutdown = reinterpret_cast<PluginShutdown>(::GetProcAddress(module, "nskry_plugin_shutdown"));
    HBITMAP bitmap = fixtureMode ? LoadFixtureBitmap(argv[3]) : MakeTestBitmap();
    if (!init || !execute || !shutdown || !bitmap) return 4;

    NskryHostContext context{};
    context.structSize = sizeof(context);
    context.apiVersion = NSKRY_PLUGIN_API_VERSION;
    context.capturedBitmap = bitmap;
    context.capturedRegion = { 50, 50, 690, 290 };
    context.showNotification = Notify;
    context.copyBitmapToClipboard = CopyToClipboard;
    context.openBitmapEditor = OpenEditor;
    if (!init(&context)) return 5;
    if (workflow && !missingEngine) {
        execute(&context);
        // Dispatch the posted progress UI, then cancel before model loading.
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message); ::DispatchMessageW(&message);
        }
        HWND progress = FindOwnWindow(L"NskryOcrProgress");
        if (!progress) return 8;
        ::SendMessageW(progress, WM_COMMAND, 1100, 0);
        if (FindOwnWindow(L"NskryOcrProgress") || FindOwnResult()) return 9;
    }
    bool resultShown = false;
    bool recognizedText = false;
    std::wstring observedText;
    const int runs = fixtureMode ? (std::max)(1, _wtoi(argv[4])) : 1;
    for (int run = 0; run < runs; ++run) {
        execute(&context);
        if (!fixtureMode) {
            // A newer request supersedes an in-flight job without blocking UI.
            execute(&context);
        }
        resultShown = false;
        if (HWND result = WaitForResult(observedText)) {
            resultShown = !workflow || Workflow(result, observedText, missingEngine);
            recognizedText = observedText.find(L"Nskry") != std::wstring::npos;
            if (result && !workflow) ::SendMessageW(result, WM_CLOSE, 0, 0);
        }
        if (fixtureMode) {
            const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, observedText.data(),
                static_cast<int>(observedText.size()), nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<size_t>((std::max)(0, bytes)), '\0');
            if (bytes > 0) ::WideCharToMultiByte(CP_UTF8, 0, observedText.data(),
                static_cast<int>(observedText.size()), utf8.data(), bytes, nullptr, nullptr);
            std::cout << "Run " << run + 1 << " (characters=" << observedText.size() << "): " << utf8 << '\n';
        }
        if (!resultShown) break;
    }

    shutdown();
    if (workflow && (FindOwnResult() || FindOwnWindow(L"NskryOcrProgress"))) return 10;
    ::DeleteObject(bitmap);
    ::FreeLibrary(module);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    if (!resultShown) {
        std::wcerr << L"OCR result window did not appear within the timeout\n";
        return 6;
    }
    if (!fixtureMode && !recognizedText) {
        std::wcerr << L"OCR result did not contain the test label: " << observedText << L"\n";
        return 7;
    }
    return 0;
}
