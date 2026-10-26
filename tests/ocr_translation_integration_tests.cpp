#include "core/plugin_manager.h"
#include "test_artifacts.h"
#include <functional>
#include <winrt/base.h>

HWND Own(const wchar_t *name) {
    struct Search {
        const wchar_t *name;
        HWND window{};
    } search{name};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND window, LPARAM value) -> BOOL {
            auto &search = *reinterpret_cast<Search *>(value);
            wchar_t name[80]{};
            GetClassNameW(window, name, 80);
            if (!wcscmp(name, search.name))
                search.window = window;
            return TRUE;
        },
        LPARAM(&search));
    return search.window;
}
bool Pump(const std::function<bool()> &done) {
    const auto end = GetTickCount64() + 20000;
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
std::wstring Text(HWND control) {
    std::wstring text(GetWindowTextLengthW(control) + 1, 0);
    GetWindowTextW(control, text.data(), int(text.size()));
    text.resize(text.size() - 1);
    return text;
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 5)
        return 2;
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    nskry::test::Artifacts artifacts(argv[4], L"ocr-translation");
    struct RegistryOverride {
        HKEY key{};
        std::wstring path;
        bool active{};
        ~RegistryOverride() {
            if (active)
                RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            if (key)
                RegCloseKey(key);
            if (active)
                RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        }
    } preferences;
    preferences.path = L"Software\\NskryTests\\Recognition-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64());
    DWORD created{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, preferences.path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                        &preferences.key, &created) ||
        created != REG_CREATED_NEW_KEY || RegOverridePredefKey(HKEY_CURRENT_USER, preferences.key))
        return 8;
    preferences.active = true;
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", artifacts.Path().c_str()))
        return 3;
    SetEnvironmentVariableW(L"NSKRY_OCR_ENGINE", L"fast"); // Only this test process.
    nskry::PluginRegistry registry;
    if (!registry.Initialize())
        return 4;
    nskry::PluginManifest translation, ocr;
    const auto translator = std::filesystem::absolute(argv[1]);
    const auto recognizer = std::filesystem::absolute(argv[2]);
    if (!nskry::PluginManifest::LoadFromFile((translator.parent_path() / L"manifest.json").wstring(),
                                             translation) ||
        !nskry::PluginManifest::LoadFromFile((recognizer.parent_path() / L"manifest.json").wstring(), ocr) ||
        !registry.UpsertInstalled(translation, translator.parent_path().wstring(),
                                  nskry::PluginSource::ThirdParty) ||
        !registry.UpsertInstalled(ocr, recognizer.parent_path().wstring(), nskry::PluginSource::ThirdParty))
        return 5;
    NskryHostContext context{sizeof(context), NSKRY_PLUGIN_API_VERSION};
    auto &runtime = nskry::PluginManager::Instance();
    runtime.Initialize(registry, context);
    struct Stop {
        nskry::PluginManager &runtime;
        ~Stop() {
            runtime.Shutdown();
        }
    } stop{runtime};
    if (!runtime.ShowPluginSettings(translation.id, nullptr))
        return 6;
    const auto translated = Own(L"NskryTranslateV1");
    SetWindowTextW(GetDlgItem(translated, 12), argv[3]); // Do not save/modify user settings.
    registry.SetEnabled(translation.id, false);
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, 480, 100);
    struct Image {
        HBITMAP value;
        ~Image() {
            DeleteObject(value);
        }
    } image{bitmap};
    const auto previous = SelectObject(dc, bitmap);
    RECT area{0, 0, 480, 100};
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetTextColor(dc, RGB(0, 0, 0));
    SetBkColor(dc, RGB(255, 255, 255));
    HFONT font = CreateFontW(32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    const auto previousFont = SelectObject(dc, font);
    TextOutW(dc, 10, 20, L"Hello world", 11);
    SelectObject(dc, previousFont);
    DeleteObject(font);
    SelectObject(dc, previous);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    context.capturedBitmap = bitmap;
    bool ok = runtime.ExecutePlugin(ocr.id, context);
    ok = Pump([] { return Own(L"NskryOcrResult") != nullptr; }) && ok;
    const auto result = Own(L"NskryOcrResult");
    ok = result && !IsWindowEnabled(GetDlgItem(result, 1011)) && ok;
    if (result) {
        const auto original = Text(FindWindowExW(result, nullptr, L"EDIT", nullptr));
        ok = original.find(L"world") != std::wstring::npos &&
             ok; // OCR may confuse I/l; this tests handoff, not spelling.
        ok = registry.SetEnabled(translation.id, true) && ok;
        SendMessageW(result, WM_ACTIVATE, WA_ACTIVE, 0);
        ok = IsWindowEnabled(GetDlgItem(result, 1011)) && ok;
        SendMessageW(result, WM_COMMAND, 1011, 0);
        ok = Text(GetDlgItem(translated, 10)) == original && !IsWindowEnabled(GetDlgItem(translated, 1)) &&
             !Own(L"NskryOcrProgress") && ok;
        DestroyWindow(result); // Translation owns its copy/window, not OCR's lifetime.
        ok = IsWindow(translated) && Pump([&] { return IsWindowEnabled(GetDlgItem(translated, 1)); }) && ok;
        ok = !Text(GetDlgItem(translated, 11)).empty() && ok;
        std::cout << "Forwarded OCR source units=" << original.size()
                  << ", translated result units=" << Text(GetDlgItem(translated, 11)).size()
                  << ", window alive=" << IsWindow(translated) << '\n';
    }
    // Reverse direction: opt-in screenshot → host-owned background OCR → translation.
    runtime.Unload(ocr.id);
    for (const auto* name : {L"NskryOcrResult", L"NskryOcrProgress", L"NskryOcrDispatch"}) {
        WNDCLASSEXW registered{sizeof(registered)};
        ok = !GetClassInfoExW(GetModuleHandleW(nullptr), name, &registered) && ok;
    }
    ok = !runtime.IsLoaded(ocr.id) &&
         SendMessageW(GetDlgItem(translated, 22), BM_GETCHECK, 0, 0) == BST_UNCHECKED && ok;
    SendMessageW(GetDlgItem(translated, 22), BM_CLICK, 0, 0);
    ok = !runtime.IsLoaded(ocr.id) &&
         SendMessageW(GetDlgItem(translated, 22), BM_GETCHECK, 0, 0) == BST_CHECKED && ok;
    auto borrowed = static_cast<HBITMAP>(CopyImage(bitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
    context.capturedBitmap = borrowed;
    ok = runtime.ExecutePlugin(translation.id, context) && ok;
    DeleteObject(borrowed); // Host task must already own its image.
    context.capturedBitmap = bitmap;
    ok = Pump([&] { return IsWindowEnabled(GetDlgItem(translated, 1)); }) && ok;
    ok = Text(GetDlgItem(translated, 10)).find(L"world") != std::wstring::npos &&
         !Text(GetDlgItem(translated, 11)).empty() && !Own(L"NskryOcrResult") && !Own(L"NskryOcrProgress") &&
         ok;
    std::cout << "Reverse Fast source=" << Text(GetDlgItem(translated, 10)).size()
              << ", result=" << Text(GetDlgItem(translated, 11)).size() << '\n';
    // Accurate mode uses the provider's own native assets, not the client's ocr/.
    SetEnvironmentVariableW(L"NSKRY_OCR_ENGINE", L"accurate");
    uint64_t ticket{};
    ok = runtime.BeginImageRecognition(ocr.id.c_str(), bitmap, &ticket) == NSKRY_IMAGE_PENDING && ok;
    std::vector<wchar_t> recognized(NSKRY_IMAGE_MAX_TEXT + 1);
    wchar_t error[256]{};
    NskryImageTextV1 output{sizeof(output), 1, recognized.data(), uint32_t(recognized.size()), 0, error, 256};
    int32_t providerStatus{};
    ok = Pump([&] {
             providerStatus = runtime.PollImageRecognition(ticket, &output);
             return providerStatus != NSKRY_IMAGE_PENDING;
         }) &&
         ok;
    ok = providerStatus == NSKRY_IMAGE_SUCCESS &&
         std::wstring(recognized.data()).find(L"world") != std::wstring::npos && ok;
    runtime.ReleaseImageRecognition(ticket);
    // Cancel, settings close and exit must stop an active recognition task.
    ok = runtime.ExecutePlugin(translation.id, context) && ok;
    SendMessageW(translated, WM_COMMAND, 2, 0);
    ok = Pump([&] { return IsWindowEnabled(GetDlgItem(translated, 1)); }) &&
         Text(GetDlgItem(translated, 11)).empty() && ok;
    registry.SetEnabled(ocr.id, false);
    ok = runtime.ExecutePlugin(translation.id, context) && Text(GetDlgItem(translated, 10)).empty() &&
         Text(GetDlgItem(translated, 14)).find(L"No fallback") != std::wstring::npos && ok;
    SendMessageW(translated, WM_CLOSE, 0, 0);
    ok = runtime.ShowPluginSettings(translation.id, nullptr) && ok;
    const auto reopened = Own(L"NskryTranslateV1");
    ok = SendMessageW(GetDlgItem(reopened, 22), BM_GETCHECK, 0, 0) == BST_CHECKED && ok;
    registry.SetEnabled(ocr.id, true);
    ok = runtime.ExecutePlugin(translation.id, context) && ok;
    SendMessageW(reopened, WM_CLOSE, 0, 0);
    ok = Pump([&] { return !IsWindow(reopened); }) && ok;
    ok = runtime.ShowPluginSettings(translation.id, nullptr) && runtime.ExecutePlugin(translation.id, context) && ok;
    DestroyWindow(Own(L"NskryTranslateV1")); // Owner destruction bypasses WM_CLOSE.
    ok = Pump([] { return !FindWindowExW(HWND_MESSAGE, nullptr, L"NskryTranslateTaskV1", nullptr); }) && ok;
    ok = !Own(L"NskryTranslateV1") && runtime.BeginImageRecognition(ocr.id.c_str(), bitmap, &ticket) == NSKRY_IMAGE_PENDING && ok;
    runtime.CancelImageRecognition(ticket);
    runtime.ReleaseImageRecognition(ticket);
    ok = runtime.ShowPluginSettings(translation.id, nullptr) &&
         runtime.ExecutePlugin(translation.id, context) && ok;
    runtime.Shutdown();
    ok = !Own(L"NskryTranslateV1") && !Own(L"NskryOcrResult") && ok;
    std::cout << "Real OCR → host → translation (no second OCR), optional disable/enable/close=" << ok
              << '\n';
    winrt::uninit_apartment();
    return artifacts.Finish(ok) && ok ? 0 : 1;
}
