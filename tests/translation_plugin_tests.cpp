#include "nskry_actions.h"
#include "nskry_plugin.h"
#include "nskry_text_translation.h"
#include <chrono>
#include <functional>
#include <gdiplus.h>
#include <iostream>
#include <thread>
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
unsigned uploadDialogs{};
bool allowPublicUpload{};
NskryTranslateTextV1 reentrantProvider{};
int32_t reentrantStatus{};
void CALLBACK RejectUpload(HWND, UINT, UINT_PTR, DWORD) {
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM) -> BOOL {
            wchar_t title[100]{};
            GetWindowTextW(hwnd, title, 100);
            if (!wcscmp(title, L"Confirm online text upload")) {
                ++uploadDialogs;
                if (reentrantProvider) {
                    const wchar_t value[] = L"Public synthetic text";
                    NskryTextTranslationV1 request{sizeof(request), 1, nullptr, value,
                                                   uint32_t(wcslen(value))};
                    reentrantStatus = reentrantProvider(&request, nullptr);
                }
                SendMessageW(hwnd, WM_COMMAND, allowPublicUpload ? IDYES : IDNO, 0);
            }
            return TRUE;
        },
        0);
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 2 && argc != 3 && argc != 4)
        return 2;
    const bool liveFree = argc == 4 && !wcscmp(argv[3], L"--live-free");
    struct Registry {
        HKEY key{};
        std::wstring path;
        bool active{};
        ~Registry() {
            if (active)
                RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            if (key)
                RegCloseKey(key);
            if (active)
                RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        }
    } registry;
    registry.path = L"Software\\NskryTests\\TranslationUi-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(GetTickCount64());
    DWORD created{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, registry.path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                        &registry.key, &created) ||
        created != REG_CREATED_NEW_KEY || RegOverridePredefKey(HKEY_CURRENT_USER, registry.key))
        return 8;
    registry.active = true;
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
    auto checkpoint = [&](const char *name) {
        if (!ok)
            std::cerr << "Failed checkpoint: " << name << '\n';
    };
    HWND window = OwnWindow();
    ok = window && ok;
    if (window) {
        ok = SendDlgItemMessageW(window, 20, CB_GETCURSEL, 0, 0) == 0 && ok;
        // Explicit online choice still requires per-request upload authorization.
        // This test rejects the owned dialog; no external text is uploaded.
        SendDlgItemMessageW(window, 20, CB_SETCURSEL, 3, 0);
        const wchar_t source[] = L"Public synthetic hello";
        NskryTextTranslationV1 online{sizeof(online), 1, nullptr, source, uint32_t(wcslen(source))};
        reentrantProvider = translateText;
        const auto timer = SetTimer(nullptr, 0, 20, RejectUpload);
        if (!timer)
            return 7;
        ok = translateText(&online, nullptr) == NSKRY_TRANSLATION_CANCELED && ok;
        KillTimer(nullptr, timer);
        reentrantProvider = nullptr;
        ok = uploadDialogs == 1 && reentrantStatus == NSKRY_TRANSLATION_BUSY &&
             IsWindowEnabled(GetDlgItem(window, 1)) && !GetWindowTextLengthW(GetDlgItem(window, 11)) && ok;
        SendDlgItemMessageW(window, 20, CB_SETCURSEL, 0, 0);
        checkpoint("upload confirmation canceled without task");
        // Missing engine/model must fail without an online fallback or stuck task.
        SetWindowTextW(GetDlgItem(window, 12), L"Z:\\nskry-test-missing-model");
        // Empty/too-long input must not start a task or silently truncate.
        SetWindowTextW(GetDlgItem(window, 10), L"");
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
        checkpoint("missing engine/model");
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
            std::wstring paragraph;
            for (int i = 0; i < 35; ++i)
                paragraph += L"Save the screenshot to the clipboard. Keep the application open. ";
            SetWindowTextW(GetDlgItem(window, 10), paragraph.c_str());
            SendMessageW(window, WM_COMMAND, 1, 0);
            ok = Pump([&] { return IsWindowEnabled(GetDlgItem(window, 1)); }, 25000) &&
                 GetWindowTextLengthW(GetDlgItem(window, 11)) > 0 && ok;
            wchar_t status[512]{};
            GetWindowTextW(GetDlgItem(window, 14), status, 512);
            ok = wcsstr(status, L"bounded parts") && ok;
            std::wcout << L"Long paragraph status: " << status << L'\n';
            checkpoint("real long paragraph");
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
        if (execute)
            execute(&context);
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
        checkpoint("screenshot OCR");
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
        if (liveFree) {
            allowPublicUpload = true;
            SetWindowTextW(GetDlgItem(window, 10), L"Hello world.");
            SendDlgItemMessageW(window, 20, CB_SETCURSEL, 3, 0);
            const auto timer = SetTimer(nullptr, 0, 20, RejectUpload);
            const auto start = GetTickCount64();
            SendMessageW(window, WM_COMMAND, 1, 0);
            KillTimer(nullptr, timer);
            ok = Pump([&] { return IsWindowEnabled(GetDlgItem(window, 1)); }, 20000) &&
                 GetWindowTextLengthW(GetDlgItem(window, 11)) > 0 && ok;
            std::cout << "Public free API full UI elapsed_ms=" << GetTickCount64() - start
                      << " output_units=" << GetWindowTextLengthW(GetDlgItem(window, 11)) << '\n';
            allowPublicUpload = false;
            SendDlgItemMessageW(window, 20, CB_SETCURSEL, 0, 0);
        }
        if (argc == 4 && !liveFree) {
            Gdiplus::GdiplusStartupInput input;
            ULONG_PTR token{};
            HBITMAP fixture{};
            if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
                return 5;
            {
                Gdiplus::Bitmap image(argv[3]);
                if (image.GetLastStatus() == Gdiplus::Ok)
                    image.GetHBITMAP(Gdiplus::Color::White, &fixture);
            }
            Gdiplus::GdiplusShutdown(token);
            if (!fixture)
                return 6;
            context.capturedBitmap = fixture;
            for (int run = 0; run < 3; ++run) {
                const auto began = GetTickCount64();
                execute(&context);
                const bool finished = Pump([&] { return IsWindowEnabled(GetDlgItem(window, 1)); }, 20000);
                const auto sourceUnits = GetWindowTextLengthW(GetDlgItem(window, 10));
                const auto resultUnits = GetWindowTextLengthW(GetDlgItem(window, 11));
                ok = finished && sourceUnits > 0 && resultUnits > 0 && ok;
                // Counts only; arbitrary local test screenshots must not be logged as plaintext.
                std::cout << "Fixture direct translation run=" << run + 1 << ", source units=" << sourceUnits
                          << ", result units=" << resultUnits << ", elapsed ms=" << GetTickCount64() - began
                          << '\n';
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
        checkpoint("cancel and close");
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
    checkpoint("shutdown running task");
    ok = init(nullptr) && settings(nullptr) && ok;
    reopened = OwnWindow();
    if (reopened) {
        ok = SendDlgItemMessageW(reopened, 20, CB_GETCURSEL, 0, 0) == 0 && ok;
        // A modeless settings window must not survive its owning result window.
        SendDlgItemMessageW(reopened, 20, CB_SETCURSEL, 1, 0);
        SendMessageW(reopened, WM_COMMAND, 21, 0);
        HWND settingsWindow{};
        EnumThreadWindows(
            GetCurrentThreadId(),
            [](HWND hwnd, LPARAM result) -> BOOL {
                wchar_t name[100]{};
                GetClassNameW(hwnd, name, 100);
                if (!wcscmp(name, L"NskryTranslationOnlineSettingsV1"))
                    *reinterpret_cast<HWND *>(result) = hwnd;
                return TRUE;
            },
            LPARAM(&settingsWindow));
        ok = settingsWindow && (GetWindowLongPtrW(GetDlgItem(settingsWindow, 12), GWL_STYLE) & ES_PASSWORD) &&
             ok;
        checkpoint("online configuration window");
        SendDlgItemMessageW(reopened, 20, CB_SETCURSEL, 0, 0);
        SetWindowTextW(GetDlgItem(reopened, 12), L"Z:\\nskry-test-missing-model");
        SetWindowTextW(GetDlgItem(reopened, 10), L"Hello");
        SendMessageW(reopened, WM_COMMAND, 1, 0);
        DestroyWindow(reopened); // Abrupt owner destruction bypasses WM_CLOSE.
        ok = !IsWindow(settingsWindow) && ok;
    } else {
        ok = false;
    }
    shutdown();
    // Remember only an explicit engine selection, inside the isolated registry.
    ok = settings(nullptr) && ok;
    reopened = OwnWindow();
    SendDlgItemMessageW(reopened, 20, CB_SETCURSEL, 3, 0);
    SendMessageW(reopened, WM_COMMAND, MAKEWPARAM(20, CBN_SELCHANGE), LPARAM(GetDlgItem(reopened, 20)));
    SendMessageW(reopened, WM_CLOSE, 0, 0);
    ok = settings(nullptr) && ok;
    reopened = OwnWindow();
    ok = reopened && SendDlgItemMessageW(reopened, 20, CB_GETCURSEL, 0, 0) == 3 && ok;
    shutdown();
    FreeLibrary(dll);
    winrt::uninit_apartment();
    std::cout << "Translation UI, actual offline worker, cancel/close/unload=" << ok << '\n';
    return ok ? 0 : 1;
}
