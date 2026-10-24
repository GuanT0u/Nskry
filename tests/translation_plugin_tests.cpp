#include "nskry_actions.h"
#include "nskry_plugin.h"
#include "nskry_text_translation.h"
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include <gdiplus.h>
#include <winrt/base.h>
bool Pump(const std::function<bool()> &done, DWORD timeout = 15000) {
    const auto end = GetTickCount64() + timeout;
    while (!done() && GetTickCount64() < end) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    return done();
}
HWND OwnWindow() {
    HWND result{};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM output) -> BOOL {
            wchar_t name[80]{};
            GetClassNameW(hwnd, name, 80);
            if (!wcscmp(name, L"NskryTranslateV1"))
                *reinterpret_cast<HWND *>(output) = hwnd;
            return TRUE;
        },
        LPARAM(&result));
    return result;
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 2 && argc != 3 && argc != 4)
        return 2;
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    HMODULE dll =
        LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!dll)
        return 3;
    const auto settings =
        reinterpret_cast<NskryShowSettingsV1>(GetProcAddress(dll, "nskry_plugin_show_settings_v1"));
    const auto shutdown =
        reinterpret_cast<decltype(&nskry_plugin_shutdown)>(GetProcAddress(dll, "nskry_plugin_shutdown"));
    const auto execute =
        reinterpret_cast<decltype(&nskry_plugin_execute)>(GetProcAddress(dll, "nskry_plugin_execute"));
    const auto init =
        reinterpret_cast<decltype(&nskry_plugin_init)>(GetProcAddress(dll, "nskry_plugin_init"));
    const auto translateText =
        reinterpret_cast<NskryTranslateTextV1>(GetProcAddress(dll, "nskry_plugin_translate_text_v1"));
    if (!settings || !shutdown || !execute || !init || !translateText || !init(nullptr))
        return 4;
    bool ok = settings && shutdown && settings(nullptr);
    HWND window = OwnWindow();
    ok = window && ok;
    if (window) {
        // Missing engine/model must fail without an online fallback or stuck task.
        SetWindowTextW(GetDlgItem(window, 12), L"Z:\\nskry-test-missing-model");
        // Empty/too-long input must not start a task or silently truncate.
        SendMessageW(window, WM_COMMAND, 1, 0);
        ok = IsWindowEnabled(GetDlgItem(window, 1)) && ok;
        const std::wstring excessive(16001, L'a');
        SetWindowTextW(GetDlgItem(window, 10), excessive.c_str());
        SendMessageW(window, WM_COMMAND, 1, 0);
        ok = IsWindowEnabled(GetDlgItem(window, 1)) && ok;
        SetWindowTextW(GetDlgItem(window, 10), L"Hello");
        SendMessageW(window, WM_COMMAND, 1, 0);
        ok = Pump([&] { return IsWindowEnabled(GetDlgItem(window, 1)); }) &&
             !GetWindowTextLengthW(GetDlgItem(window, 11)) && ok;
        if (argc >= 3) {
            SetWindowTextW(GetDlgItem(window, 12), argv[2]);
            SetWindowTextW(GetDlgItem(window, 10), L"Save the screenshot to the clipboard.");
            SendMessageW(window, WM_COMMAND, 1, 0);
            ok = Pump([&] { return IsWindowEnabled(GetDlgItem(window, 1)); }) && ok;
            wchar_t translated[2048]{};
            GetWindowTextW(GetDlgItem(window, 11), translated, 2048);
            ok = translated[0] && ok;
            std::wcout << L"Offline result: " << translated << L'\n';
            GetWindowTextW(GetDlgItem(window, 14), translated, 2048);
            std::wcout << L"Status: " << translated << L'\n';
        }
        // Independent screenshot OCR path: only this test's synthetic bitmap.
        HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 480, 100);
        auto old = SelectObject(dc, bitmap);
        RECT area{0, 0, 480, 100};
        FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        SetTextColor(dc, RGB(0, 0, 0));
        SetBkColor(dc, RGB(255, 255, 255));
        HFONT font =
            CreateFontW(32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        auto oldFont = SelectObject(dc, font);
        TextOutW(dc, 10, 20, L"Hello world", 11);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        SelectObject(dc, old);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        NskryHostContext context{sizeof(context), NSKRY_PLUGIN_API_VERSION};
        context.capturedBitmap = bitmap;
        if (execute) execute(&context);
        SendMessageW(window, WM_COMMAND, 2, 0); // Cancel recognition: no automatic translation may follow.
        ok = Pump([&] { return IsWindowEnabled(GetDlgItem(window, 1)); }) &&
            !GetWindowTextLengthW(GetDlgItem(window, 11)) && ok;
        if (execute)
            execute(&context);
        DeleteObject(bitmap); // execute must have copied borrowed pixels before returning.
        ok = execute && Pump([&] { return IsWindowEnabled(GetDlgItem(window, 1)); }) && ok;
        wchar_t recognized[2048]{};
        GetWindowTextW(GetDlgItem(window, 10), recognized, 2048);
        std::wcout << L"Synthetic screenshot OCR: " << recognized << L'\n';
        ok = wcsstr(recognized, L"world") != nullptr && ok;
        if (argc >= 3) {
            wchar_t automatic[2048]{};
            GetWindowTextW(GetDlgItem(window, 11), automatic, 2048);
            ok = automatic[0] && SendMessageW(GetDlgItem(window, 13), CB_GETCURSEL, 0, 0) == 0 && ok;
            std::wcout << L"Screenshot automatically translated: " << automatic << L'\n';
            // OCR handoff accepts a copied, length-delimited source, detects zh→en,
            // and starts immediately, without clicking the Translate button.
            std::wstring chinese = L"请不要关闭应用程序。";
            NskryTextTranslationV1 request{sizeof(request), 1, nullptr, chinese.data(),
                                           uint32_t(chinese.size())};
            wchar_t error[256]{};
            NskryTranslationResultV1 reply{sizeof(reply), 1, error, 256};
            ok = translateText(&request, &reply) == NSKRY_TRANSLATION_ACCEPTED && ok;
            ok = translateText(&request, &reply) == NSKRY_TRANSLATION_BUSY && ok;
            chinese.assign(chinese.size(), L'x'); // Provider must not retain this borrowed pointer.
            ok = Pump([&] { return IsWindowEnabled(GetDlgItem(window, 1)); }) && ok;
            GetWindowTextW(GetDlgItem(window, 11), automatic, 2048);
            ok = automatic[0] && wcsstr(automatic, L"close") && ok;
            std::wcout << L"Chinese handoff automatically translated: " << automatic << L'\n';
        }
        if (argc == 4) {
            Gdiplus::GdiplusStartupInput input;
            ULONG_PTR token{};
            HBITMAP fixture{};
            if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return 5;
            {
                Gdiplus::Bitmap image(argv[3]);
                if (image.GetLastStatus() == Gdiplus::Ok) image.GetHBITMAP(Gdiplus::Color::White, &fixture);
            }
            Gdiplus::GdiplusShutdown(token);
            if (!fixture) return 6;
            context.capturedBitmap = fixture;
            for (int run = 0; run < 3; ++run) {
                const auto began = GetTickCount64();
                execute(&context);
                const bool finished = Pump([&] { return IsWindowEnabled(GetDlgItem(window, 1)); }, 20000);
                const auto sourceUnits = GetWindowTextLengthW(GetDlgItem(window, 10));
                const auto resultUnits = GetWindowTextLengthW(GetDlgItem(window, 11));
                ok = finished && sourceUnits > 0 && resultUnits > 0 && ok;
                // Counts only; arbitrary local test screenshots must not be logged as plaintext.
                std::cout << "Fixture direct translation run=" << run+1 << ", source units=" << sourceUnits
                          << ", result units=" << resultUnits << ", elapsed ms=" << GetTickCount64()-began << '\n';
            }
            DeleteObject(fixture);
        }
        SetWindowTextW(GetDlgItem(window, 10), L"Hello");
        SendMessageW(window, WM_COMMAND, 1, 0);
        SendMessageW(window, WM_COMMAND, 1, 0); // Repeated start is ignored, not queued.
        SendMessageW(window, WM_COMMAND, 2, 0);
        ok = Pump([&] { return IsWindowEnabled(GetDlgItem(window, 1)); }, 5000) && ok;
        SendMessageW(window, WM_COMMAND, 1, 0);
        SendMessageW(window, WM_CLOSE, 0, 0);
        ok = Pump([&] { return !IsWindow(window); }, 5000) && ok;
    }
    if (shutdown)
        shutdown();
    // Reopen after cleanup, then shutdown with a live task; no DLL thread may survive.
    ok = init(nullptr) && settings(nullptr) && ok;
    HWND reopened = OwnWindow();
    ok = reopened && ok;
    if (reopened) {
        SetWindowTextW(GetDlgItem(reopened, 12), argc >= 3 ? argv[2] : L"Z:\\nskry-test-missing-model");
        SetWindowTextW(GetDlgItem(reopened, 10), L"Hello");
        SendMessageW(reopened, WM_COMMAND, 1, 0);
    }
    const auto stopTime = GetTickCount64();
    shutdown();
    ok = !IsWindow(reopened) && GetTickCount64() - stopTime < 5000 && ok;
    ok = init(nullptr) && settings(nullptr) && ok;
    reopened = OwnWindow();
    if (reopened) {
        SetWindowTextW(GetDlgItem(reopened, 12), L"Z:\\nskry-test-missing-model");
        SetWindowTextW(GetDlgItem(reopened, 10), L"Hello");
        SendMessageW(reopened, WM_COMMAND, 1, 0);
        DestroyWindow(reopened); // Abrupt owner destruction bypasses WM_CLOSE.
    } else {
        ok = false;
    }
    shutdown();
    FreeLibrary(dll);
    winrt::uninit_apartment();
    std::cout << "Translation UI, actual offline worker, cancel/close/unload=" << ok << '\n';
    return ok ? 0 : 1;
}
