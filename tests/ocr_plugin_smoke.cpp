#include "nskry_plugin.h"

#include <chrono>
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

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    HMODULE module = ::LoadLibraryW(argv[1]);
    if (!module) return 3;
    const auto init = reinterpret_cast<PluginInit>(::GetProcAddress(module, "nskry_plugin_init"));
    const auto execute = reinterpret_cast<PluginExecute>(::GetProcAddress(module, "nskry_plugin_execute"));
    const auto shutdown = reinterpret_cast<PluginShutdown>(::GetProcAddress(module, "nskry_plugin_shutdown"));
    HBITMAP bitmap = MakeTestBitmap();
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
    execute(&context);

    bool resultShown = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        if (HWND result = ::FindWindowW(L"NskryOcrResult", nullptr)) {
            ::SendMessageW(result, WM_CLOSE, 0, 0);
            resultShown = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    shutdown();
    ::DeleteObject(bitmap);
    ::FreeLibrary(module);
    if (!resultShown) {
        std::wcerr << L"OCR result window did not appear within the timeout\n";
        return 6;
    }
    return 0;
}
