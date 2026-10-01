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

void Notify(const wchar_t*, int) {}
int32_t CopyToClipboard(HBITMAP) { return 0; }
void OpenEditor(HBITMAP bitmap, int, int) { if (bitmap) ::DeleteObject(bitmap); }

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
    if (argc != 2 && !fixtureMode) return 2;
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            MSG message{};
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            if (HWND result = ::FindWindowW(L"NskryOcrResult", nullptr)) {
                if (HWND edit = ::FindWindowExW(result, nullptr, L"EDIT", nullptr)) {
                    const int length = ::GetWindowTextLengthW(edit);
                    observedText.resize(length + 1);
                    ::GetWindowTextW(edit, observedText.data(), length + 1);
                    observedText.resize(length);
                    recognizedText = observedText.find(L"Nskry") != std::wstring::npos;
                }
                ::SendMessageW(result, WM_CLOSE, 0, 0);
                resultShown = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
