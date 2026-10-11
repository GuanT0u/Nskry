#include <windows.h>
#include <commctrl.h>
#include "ui/settings/settings_window.h"
#include "ui/long_image_crop_window.h"
#include "ui/image_action.h"
#include "ui/selection_window.h"
#include "ui/window_visual_style.h"
#include "core/monitor_visibility_controller.h"
#include <filesystem>
#include <chrono>
#include <iostream>

namespace nskry {
struct MonitorControllerTestAccess {
    static bool Verify() {
        HWND source = CreateWindowExW(0,L"STATIC",L"source",WS_POPUP,0,0,100,100,nullptr,nullptr,nullptr,nullptr);
        HWND pip = CreateWindowExW(0,L"STATIC",L"pip",WS_POPUP,-20000,-20000,100,100,nullptr,nullptr,nullptr,nullptr);
        bool protectedInteraction = false, hidden = false, paused = false, released = false;
        int closeReports = 0;
        HWND foreground = source;
        bool ok = true;
        {
            MonitorVisibilityController controller(source, pip, [&] { return protectedInteraction; },
                [&](bool h, bool p, bool r) { hidden = h; paused = p; released = r; }, [&] { ++closeReports; },
                [&] { return foreground; });
            controller.SetGlobal(true); controller.Evaluate();
            ok &= hidden && paused;
            SendMessageW(controller.m_hwnd, WM_TIMER, 1, 0);
            ok &= controller.EventsAvailable() ? released : !released;
            foreground = pip; controller.Evaluate(); ok &= !hidden && !paused && !released;
            foreground = source; protectedInteraction = true; controller.Evaluate(); ok &= !hidden;
            protectedInteraction = false; controller.Evaluate(); ok &= hidden;
            controller.SetOverride(MonitorHideOverride::Disabled); controller.Evaluate();
            ok &= !hidden && !paused && !controller.EventsAvailable();
            controller.SetOverride(MonitorHideOverride::Enabled); controller.Evaluate(); ok &= hidden;
            DestroyWindow(source); controller.Evaluate(); controller.Evaluate(); ok &= closeReports == 1;
        }
        DestroyWindow(pip); return ok;
    }
};
struct ImageEditorTestAccess {
    static bool Verify(LongImageCropWindow& window) {
        if (!window.m_model.Cut(false, 10, 20) || !window.m_model.Cut(true, 30, 40)) return false;
        window.SyncModel();
        HBITMAP result = window.CreateCroppedBitmap();
        if (!result) return false;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 110; info.bmiHeader.biHeight = -70;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        std::vector<unsigned long> output(110 * 70);
        HDC dc = CreateCompatibleDC(nullptr);
        bool ok = GetDIBits(dc, result, 0, 70, output.data(), &info, DIB_RGB_COLORS) == 70;
        DeleteDC(dc); DeleteObject(result);
        for (int y = 0; y < 70; ++y) for (int x = 0; x < 110; ++x) {
            const unsigned expected = (y < 10 ? y : y + 10) * 120 + (x < 30 ? x : x + 10);
            ok &= (output[y * 110 + x] & 0xffffffUL) == expected;
        }
        if (!window.m_model.Undo() || !window.m_model.Undo()) return false;
        window.SyncModel(); window.SetCutMode(1); window.m_hasCut = true; window.SetCutMode(0);
        return ok && window.m_width == 120 && window.m_height == 80 && !window.m_hasCut;
    }
};
struct SelectionWindowTestAccess {
    static bool Verify() {
        SelectionWindow window([](SelectionAction, SelectionResult) {});
        // Deterministic empty-space target list, regardless of real desktop apps.
        // The overlay stays hidden; no input is sent to other processes.
        window.m_windows.clear();
        window.OnLButtonDown(30, 40);
        if (window.m_state != SelectionWindow::State::Dragging) return false;
        window.OnMouseMove(50, 60);
        window.OnLButtonUp(130, 140);
        if (window.m_state != SelectionWindow::State::Selected || window.m_targetHwnd ||
            window.m_finalRect.left != 30 || window.m_finalRect.top != 40 ||
            window.m_finalRect.right != 130 || window.m_finalRect.bottom != 140) return false;
        window.m_state = SelectionWindow::State::Hovering;
        window.OnLButtonDown(130, 140);
        window.OnLButtonUp(30, 40);
        if (window.m_finalRect.left != 30 || window.m_finalRect.top != 40 ||
            window.m_finalRect.right != 130 || window.m_finalRect.bottom != 140) return false;
        window.m_state = SelectionWindow::State::Hovering;
        window.OnLButtonDown(30, 40);
        window.OnLButtonUp(30, 40);
        if (window.m_state != SelectionWindow::State::Hovering) return false;
        WindowInfo snap{}; snap.hwnd = window.m_hwnd;
        snap.bounds = {window.m_vX + 20, window.m_vY + 20, window.m_vX + 200, window.m_vY + 200};
        window.m_windows.push_back(snap);
        // Mouse-down without preceding movement still discovers the snap target.
        window.OnLButtonDown(30, 40);
        window.OnLButtonUp(30, 40);
        if (window.m_state != SelectionWindow::State::Selected || window.m_finalRect.right != 200) return false;
        window.m_state = SelectionWindow::State::Hovering;
        window.m_windows.clear();
        window.OnLButtonDown(30, 40);
        window.OnLButtonUp(100, 110);
        return !window.m_targetHwnd && IsRectEmpty(&window.m_targetBounds) &&
               window.m_finalRect.right == 100 && window.m_finalRect.bottom == 110;
    }
};
}

namespace {
bool Expect(bool value, const char* what) { if (!value) std::cerr << "FAILED: " << what << '\n'; return value; }
HWND OwnWindow(const wchar_t* name) {
    struct Find { const wchar_t* name; HWND window{}; } find{name};
    EnumWindows([](HWND w, LPARAM data) -> BOOL {
        DWORD pid{}; GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        wchar_t name[80]{}; GetClassNameW(w, name, 80);
        auto& find = *reinterpret_cast<Find*>(data);
        if (wcscmp(name, find.name)) return TRUE;
        find.window = w; return FALSE;
    }, reinterpret_cast<LPARAM>(&find));
    return find.window;
}
std::wstring Text(HWND w) {
    std::wstring text(GetWindowTextLengthW(w) + 1, L'\0');
    GetWindowTextW(w, text.data(), static_cast<int>(text.size())); text.pop_back(); return text;
}
bool InClient(HWND parent, HWND child) {
    RECT rect{}, client{}; GetClientRect(parent, &client);
    if (!child || !GetWindowRect(child, &rect)) return false;
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom;
}
}
int wmain() {
    using namespace nskry;
    bool ok = true, closed = false;
    ok &= Expect(SelectionWindowTestAccess::Verify(), "desktop drag, reverse drag, release endpoint, empty click and window snap");
    ok &= Expect(MonitorControllerTestAccess::Verify(), "event controller focus/menu/override/release/resume/source destruction");
    // Redirect persistence to an owned, unique test folder; never touch user settings.
    const auto testDirectory = std::filesystem::current_path() / (L"feature-config-" + std::to_wstring(GetCurrentProcessId()));
    if (!std::filesystem::create_directory(testDirectory)) return 1;
    std::wstring oldLocal(32768, L'\0'); DWORD oldLength = GetEnvironmentVariableW(L"LOCALAPPDATA", oldLocal.data(), static_cast<DWORD>(oldLocal.size()));
    oldLocal.resize(oldLength);
    SetEnvironmentVariableW(L"LOCALAPPDATA", testDirectory.c_str());
    auto& manager = SettingsManager::Instance(); manager.Load();
    auto stored = manager.GetUserSettings(); stored.hideMonitorOnSourceFocus = true;
    stored.visuals.shadowEnabled = true; stored.visuals.cornersEnabled = true;
    stored.visuals.cornerMode = CornerMode::Percent; stored.visuals.cornerPercent = 11;
    manager.SetUserSettings(stored);
    ok &= Expect(manager.Save() && manager.Load() && manager.GetUserSettings().hideMonitorOnSourceFocus &&
        manager.GetUserSettings().visuals == stored.visuals, "settings round trip including custom visual values");
    SetEnvironmentVariableW(L"LOCALAPPDATA", oldLocal.empty() ? nullptr : oldLocal.c_str());
    std::filesystem::remove(testDirectory / L"Nskry" / L"config" / L"settings.json");
    std::filesystem::remove(testDirectory / L"Nskry" / L"config");
    std::filesystem::remove(testDirectory / L"Nskry"); std::filesystem::remove(testDirectory);
    int networkCalls = 0;
    SettingsWindowServices services;
    services.getSettings = [] { return UserSettings{}; };
    services.getPlugins = [] {
        SettingsPluginItem item;
        item.id = L"org.example.image"; item.name = L"Test image plugin";
        item.description = L"Plugin description from manifest only"; item.author = L"Test author";
        item.version = L"1.0.0"; item.status = L"Enabled - Not loaded"; item.loadPolicy = L"on_demand";
        return std::vector<SettingsPluginItem>{item};
    };
    services.checkPluginUpdates = [&](std::wstring&) { ++networkCalls; return std::vector<PluginUpdate>{}; };
    SettingsWindow settings(std::move(services), [&] { closed = true; });
    settings.Show(nullptr);
    HWND window = OwnWindow(L"NskrySettings");
    ok &= Expect(window != nullptr, "settings created");
    HWND tabs = GetDlgItem(window, 100);
    TabCtrl_SetCurSel(tabs, 4);
    NMHDR effects{tabs, 100, TCN_SELCHANGE}; SendMessageW(window, WM_NOTIFY, 100, reinterpret_cast<LPARAM>(&effects));
    for (int id : {140,141,142,143,144,149,150,151,152,153,154})
        ok &= Expect(InClient(window, GetDlgItem(window, id)) && IsWindowVisible(GetDlgItem(window, id)), "effects controls fit default client");
    TabCtrl_SetCurSel(tabs, 2);
    NMHDR selected{tabs, 100, TCN_SELCHANGE}; SendMessageW(window, WM_NOTIFY, 100, reinterpret_cast<LPARAM>(&selected));
    ok &= Expect(Text(GetDlgItem(window, 125)).find(L"Plugin description from manifest only") != std::wstring::npos, "selected plugin description shown");
    ok &= Expect(Text(GetDlgItem(window, 125)).find(L"Local / unverified") != std::wstring::npos, "source does not imply certified publisher");
    for (int id : {120, 121, 122, 123, 124, 125, 126, 127})
        ok &= Expect(InClient(window, GetDlgItem(window, id)) && IsWindowVisible(GetDlgItem(window, id)), "plugin controls fit default client");
    SetWindowPos(window, nullptr, 0, 0, 1000, 800, SWP_NOMOVE | SWP_NOZORDER);
    for (int id : {120, 125, 126, 127}) ok &= Expect(InClient(window, GetDlgItem(window, id)), "resized plugin controls fit");
    ok &= Expect(networkCalls == 0, "opening plugins page does not request updates");
    SendMessageW(window, WM_CLOSE, 0, 0);
    ok &= Expect(closed && !OwnWindow(L"NskrySettings"), "settings destroyed on close");

    // Core editing is available with no plugin runtime or registry at all.
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 120; info.bmiHeader.biHeight = -80;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels{}; HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    for (unsigned i = 0; i < 120 * 80; ++i) static_cast<unsigned long*>(pixels)[i] = i;
    bool copied = false, editorClosed = false;
    LongImageCropWindow editor(bitmap, 120, 80, [](HBITMAP b, int, int) { DeleteObject(b); },
        [&] { editorClosed = true; }, {}, [&](HWND, HBITMAP b, int width, int height) {
            copied = b && width == 120 && height == 80; return copied;
        });
    ok &= Expect(editor.Show(nullptr), "core editor works without OCR or scroll installed");
    ok &= Expect(ImageEditorTestAccess::Verify(editor), "GDI strip export matches every source pixel and restores original on undo");
    HWND crop = OwnWindow(L"NskryLongImageCropWindow");
    ok &= Expect(crop && !GetDlgItem(crop, 5), "no plugin action button without plugins");
    SendMessageW(crop, WM_COMMAND, 6, 0);
    ok &= Expect(copied, "core crop export uses original dimensions");
    SendMessageW(crop, WM_CLOSE, 0, 0);
    ok &= Expect(editorClosed, "core crop closes without plugin");
    ImageActions actions;
    ok &= Expect(!ChooseImageAction(nullptr, actions), "no empty action selection");
    actions.push_back({L"org.thirdparty.action", L"Different translated label", [](HBITMAP, int, int, RECT, HWND) {}});
    ok &= Expect(ChooseImageAction(nullptr, actions) == 0, "single non-OCR third-party action works without special ID");
    const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    std::vector<double> shadowTimings;
    for (int i = 0; i < 30; ++i) {
        HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"test-only", WS_POPUP,
                                      -20000, -20000, 240, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(owner, SW_SHOWNOACTIVATE);
        {
            WindowVisualStyle style(owner); WindowVisualSettings visual;
            ok &= Expect(style.SetSettings(visual) && style.ShadowBytes() == 0, "disabled effects allocate no shadow");
            visual.shadowEnabled = visual.cornersEnabled = true; visual.cornerMode = CornerMode::Fixed;
            const auto began = std::chrono::steady_clock::now();
            ok &= Expect(style.SetSettings(visual) && style.ShadowBytes() > 0 && style.ShadowBytes() < 240 * 160 * 4,
                "thin shadow surfaces smaller than full content surface");
            shadowTimings.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - began).count());
            const unsigned generations = style.GenerationCount();
            SetWindowPos(owner, nullptr, -19990, -19990, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); style.Sync();
            ok &= Expect(style.GenerationCount() == generations, "pure movement does not generate shadow pixels");
            ShowWindow(owner, SW_HIDE); style.Sync(); ShowWindow(owner, SW_SHOWNOACTIVATE); style.Sync();
            ok &= Expect(style.GenerationCount() == generations, "hide and restore reuse shadow cache");
            style.BeginSizeMove(); SetWindowPos(owner, nullptr, 0, 0, 260, 180, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            style.Sync(); ok &= Expect(style.GenerationCount() == generations, "interactive resize defers generation");
            style.EndSizeMove(); ok &= Expect(style.GenerationCount() == generations + 1, "resize finish regenerates once");
            visual.shadowEnabled = visual.cornersEnabled = false;
            ok &= Expect(style.SetSettings(visual) && style.ShadowBytes() == 0, "disable releases shadow surfaces");
        }
        DestroyWindow(owner);
    }
    ok &= Expect(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdiBefore + 1 &&
                 GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= userBefore + 1, "no GDI/USER growth after 30 visual lifecycles");
    std::sort(shadowTimings.begin(), shadowTimings.end());
    std::cout << "shadow generation (240x160, 30 samples) p50=" << shadowTimings[15] << "ms p95=" << shadowTimings[28] << "ms\n";
    return ok ? 0 : 1;
}
