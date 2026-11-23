#include <oleacc.h>
#include <windows.h>
#include <commctrl.h>
#include "ui/settings/settings_window.h"
#include "ui/long_image_crop_window.h"
#include "ui/image_action.h"
#include "ui/selection_window.h"
#include "ui/pin_window.h"
#include "ui/window_visual_style.h"
#include "ui/toolbar_layout.h"
#include "core/monitor_visibility_controller.h"
#include "ui/interaction_visuals.h"
#include "core/json.h"
#include <filesystem>
#include <chrono>
#include <iostream>
#include <fstream>
#include <condition_variable>

namespace nskry {
struct InputPageTestAccess {
    static bool Verify() {
        HWND parent=CreateWindowExW(0,L"STATIC",L"input UI test",WS_POPUP,0,0,660,400,nullptr,nullptr,nullptr,nullptr);
        bool suspended=false;int transitions=0;bool ok=true;
        {
            auto definitions=CoreShortcuts();
            AddBuiltinCapturePlaceholders(definitions);
            definitions.push_back({L"plugin.org.test.launch",L"Third party test",L"",L"org.test",L"A third-party plugin",input::Kind::Global,false,true});
            InputPage page(parent,definitions,false,[&](bool state){suspended=state;++transitions;},nullptr);
            SetWindowPos(page.Hwnd(),nullptr,0,0,630,280,SWP_NOZORDER);page.Load({});
            auto& row=page.m_rows[0];
            ok &= row.display && !(GetWindowLongPtrW(row.edit,GWL_STYLE)&WS_VISIBLE);
            page.BeginRecord(0);SendMessageW(page.Hwnd(),WM_KEYDOWN,VK_CONTROL,0);SendMessageW(page.Hwnd(),WM_SYSKEYDOWN,VK_MENU,0);
            SendMessageW(page.Hwnd(),WM_KEYDOWN,'A',0);SendMessageW(page.Hwnd(),WM_KEYUP,'A',0);
            ok &= suspended && page.m_recording==0;
            SendMessageW(page.Hwnd(),WM_KEYUP,VK_CONTROL,0);SendMessageW(page.Hwnd(),WM_SYSKEYUP,VK_MENU,0);
            UserSettings settings;page.Store(settings);
            ok &= !suspended && transitions==2 && settings.captureShortcut==L"Ctrl+Alt+A";
            SendMessageW(page.Hwnd(),WM_COMMAND,MAKEWPARAM(10001,BN_CLICKED),LPARAM(row.type));
            SetWindowTextW(row.edit,L"Ctrl+Alt+B");page.Store(settings);ok &= settings.captureShortcut==L"Ctrl+Alt+B";
            SendMessageW(page.Hwnd(),WM_COMMAND,MAKEWPARAM(10002,BN_CLICKED),LPARAM(row.remove));
            page.Store(settings);ok &= settings.captureShortcut.empty();
            page.BeginRecord(0);SendMessageW(page.Hwnd(),WM_KEYDOWN,'X',0);SendMessageW(page.Hwnd(),WM_KILLFOCUS,0,0);
            page.Store(settings);ok &= !suspended && settings.captureShortcut.empty();
            page.Jump(L"plugin.org.test.launch");
            size_t controls=0;for(auto& r:page.m_rows)if(r.display)++controls;
            ok &= controls<10 && page.m_flashRow>=0;
            for(int i=0;i<4;++i)SendMessageW(page.Hwnd(),WM_TIMER,1,0);
            ok &= page.m_flashRow==-1;
            page.Store(settings);ok &= settings.captureShortcut.empty(); // Scroll must retain earlier edits.
        }
        {
            std::vector<ShortcutDefinition> many;
            for (size_t i=0;i<=5001;++i)
                many.push_back({L"core.test."+std::to_wstring(i),L"Catalog row",L"Ctrl+C"});
            InputPage page(parent,std::move(many),false,nullptr,nullptr);
            SetWindowPos(page.Hwnd(),nullptr,0,0,630,280,SWP_NOZORDER);
            page.Jump(L"core.test.5001");
            auto& row=page.m_rows[5001];
            SendMessageW(page.Hwnd(),WM_COMMAND,MAKEWPARAM(10001,BN_CLICKED),LPARAM(row.type));
            SetWindowTextW(row.edit,L"Shift+F24");
            UserSettings settings;page.Store(settings);
            ok &= settings.shortcuts[L"core.test.5001"]==L"Shift+F24" &&
                settings.shortcuts[L"core.test.0"]==L"Ctrl+C";
            ShowWindow(row.display,SW_HIDE);page.Load(settings);
            ok &= (GetWindowLongPtrW(row.display,GWL_STYLE)&WS_VISIBLE)!=0;
        }
        DestroyWindow(parent);return ok;
    }
};
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
            ShowWindow(pip, SW_SHOWMINNOACTIVE);
            controller.Evaluate();
            ok &= !hidden && paused && !released;
            SendMessageW(controller.m_hwnd, WM_TIMER, 1, 0);
            ok &= controller.m_destroyHook ? released : !released;
            ShowWindow(pip, SW_SHOWNOACTIVATE);
            controller.Evaluate();
            ok &= !IsIconic(pip) && !hidden && !paused && !released;
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
    static bool VerifyOverflow() {
        std::vector<PluginToolbarAction> plugins;
        for (int i = 0; i < 8; ++i)
            plugins.push_back({L"org.test", std::wstring(180, L'W'), std::to_wstring(i)});
        bool copied = false;
        SelectionWindow window([&](SelectionAction action, SelectionResult result) {
            copied = action == SelectionAction::Copy;
            if (result.bitmap) DeleteObject(result.bitmap);
        }, std::move(plugins));
        window.m_windows.clear();
        window.OnLButtonDown(30, 40);
        window.OnLButtonUp(130, 140);
        const auto &visible = window.m_toolbarItems;
        const auto &overflow = window.m_overflowItems;
        if (visible.empty() || overflow.empty() || visible.back().type != SelectionWindow::ToolbarItemType::More)
            return false;
        IAccessible* accessible = nullptr;
        if (FAILED(::AccessibleObjectFromWindow(window.m_hwnd, OBJID_CLIENT, IID_IAccessible,
                                                reinterpret_cast<void**>(&accessible))) || !accessible)
            return false;
        LONG childCount{};
        bool hasCopy = false;
        if (SUCCEEDED(accessible->get_accChildCount(&childCount))) {
            for (LONG child = 1; child <= childCount; ++child) {
                VARIANT id{};
                id.vt = VT_I4;
                id.lVal = child;
                BSTR name = nullptr;
                if (SUCCEEDED(accessible->get_accName(id, &name)) && name &&
                    (!wcscmp(name, L"Copy image") || !wcscmp(name, L"复制图片"))) {
                    VARIANT role{};
                    hasCopy = SUCCEEDED(accessible->get_accRole(id, &role)) &&
                        role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_PUSHBUTTON;
                }
                ::SysFreeString(name);
            }
        }
        accessible->Release();
        if (!hasCopy) return false;
        std::wstring order;
        for (const auto &item : overflow)
            if (item.type == SelectionWindow::ToolbarItemType::PluginAction)
                order += item.pluginActionId;
        if (order.empty() || order.back() != L'7' || !std::is_sorted(order.begin(), order.end()))
            return false;
        for (size_t i = 0; i < visible.size(); ++i) {
            if (!window.HandleToolbarKey(VK_TAB))
                return false;
            if (visible[window.m_toolbarFocus].action == SelectionAction::Copy)
                return window.HandleToolbarKey(VK_RETURN) && copied;
        }
        return false;
    }

    static bool Verify() {
        bool direct=false;
        {
            SelectionWindow window([&](SelectionAction action,SelectionResult result){
                direct=action==SelectionAction::Plugin && result.pluginId==L"org.test" && result.bitmapWidth==60 && result.bitmapHeight==60;
                if(result.bitmap)DeleteObject(result.bitmap);
            },{},SelectionAction::Plugin,L"org.test");
            window.m_windows.clear();window.OnLButtonDown(30,40);window.OnLButtonUp(90,100);
        }
        if(!direct)return false;
        bool extra = false;
        {
            SelectionWindow actionWindow([&](SelectionAction action, SelectionResult result) {
                extra = action == SelectionAction::Plugin && result.pluginId == L"org.test" &&
                        result.pluginActionId == L"gif";
                if (result.bitmap) DeleteObject(result.bitmap);
            }, {{L"org.test", L"GIF", L"gif"}});
            actionWindow.m_windows.clear();
            actionWindow.OnLButtonDown(30, 40); actionWindow.OnLButtonUp(130, 140);
            for (const auto &item : actionWindow.m_toolbarItems) {
                if (item.pluginActionId == L"gif") {
                    const int x = item.rect.left + 3, y = item.rect.top + 3;
                    actionWindow.OnLButtonDown(x, y);
                    break;
                }
            }
            if (!extra) {
                for (const auto &item : actionWindow.m_overflowItems) {
                    if (item.pluginActionId == L"gif") {
                        actionWindow.ActivateToolbarItem(item);
                        break;
                    }
                }
            }
        }
        if (!extra) return false;
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
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    struct ComGuard { ~ComGuard() { ::CoUninitialize(); } } com;

    Gdiplus::GdiplusStartupInput gdiplusInput;ULONG_PTR gdiplusToken{};
    if(Gdiplus::GdiplusStartup(&gdiplusToken,&gdiplusInput,nullptr)!=Gdiplus::Ok)return 1;
    struct GdiplusGuard{ULONG_PTR token;~GdiplusGuard(){Gdiplus::GdiplusShutdown(token);}} gdiplusGuard{gdiplusToken};
    using namespace nskry;
    bool ok = true, closed = false;
    {
        AnnotationEngine engine;
        engine.SetTool(ToolType::Rect);
        engine.OnMouseDown({4, 4});
        engine.OnMouseUp({20, 20});
        const auto pixels = engine.RenderOverlayRgba(nullptr, 0, 0, 32, 32);
        ok &= Expect(std::any_of(pixels.begin(), pixels.end(), [](uint32_t pixel) { return pixel != 0; }),
                     "annotation records the mouse-release endpoint without a move message");
    }
    // Redirect persistence to an owned, unique test folder; never touch user settings.
    const auto testDirectory = std::filesystem::current_path() / (L"feature-config-" + std::to_wstring(GetCurrentProcessId()));
    if (!std::filesystem::create_directory(testDirectory)) return 1;
    std::wstring oldLocal(32768, L'\0'); DWORD oldLength = GetEnvironmentVariableW(L"LOCALAPPDATA", oldLocal.data(), static_cast<DWORD>(oldLocal.size()));
    oldLocal.resize(oldLength);
    if(!SetEnvironmentVariableW(L"LOCALAPPDATA", testDirectory.c_str()))return 1;
    // Establish isolation before any UI helper can initialize the settings singleton.
    std::mutex waitMutex;std::condition_variable_any waitSignal;UpdateTask waitingUpdate;
    ok &= Expect(waitingUpdate.Start([&](std::stop_token stop){
        std::unique_lock lock(waitMutex);waitSignal.wait(lock,stop,[]{return false;});return UpdateTaskResult{};
    },{}),"background wait starts without blocking UI");
    ok &= Expect(InputPageTestAccess::Verify(),"display/record/type/delete, blur cancellation, plugin sections, jump pulse and virtual rows");
    for (UINT dpi : {96, 120, 144, 192}) {
        const RECT display{-920, -560, -120, -40}, selection{-800, -180, -180, -70};
        const std::vector<int> widths(7, ::MulDiv(44, dpi, 96));
        const auto rectangles = LayoutSelectionToolbar(widths, selection, display, 5, dpi);
        ok &= Expect(rectangles.size() == widths.size() && std::all_of(rectangles.begin(), rectangles.end(),
            [&](RECT r) {
                return r.left >= display.left && r.right <= display.right && r.top >= display.top &&
                    r.bottom <= display.bottom && (r.bottom - r.top) == ::MulDiv(32, dpi, 96);
            }), "toolbar stays in negative-coordinate work area at 100, 125, 150 and 200 percent DPI");
    }
    ok &= Expect(SelectionWindowTestAccess::VerifyOverflow(), "selection overflow preserves copy, plugin order and keyboard activation");
    ok &= Expect(SelectionWindowTestAccess::Verify(), "desktop drag, reverse drag, release endpoint, empty click and window snap");
    ok &= Expect(MonitorControllerTestAccess::Verify(), "event controller focus/menu/override/release/resume/source destruction");
    waitingUpdate.Cancel();waitingUpdate.Shutdown();
    if(std::filesystem::path(SettingsManager::Instance().SettingsPath()).parent_path()!=testDirectory/L"Nskry"/L"config")return 1;
    auto& manager = SettingsManager::Instance(); manager.Load();
    {
        std::ofstream output(std::filesystem::path(manager.SettingsPath()),std::ios::binary|std::ios::trunc);
        output << R"({"hotkeys":{},"general":{"future_setting":42},"capture":{"default_save_directory":"future path"},
            "updates":{"check_automatically":false,"include_prerelease":true},"future":{"items":[true,null,7]}})";
    }
    ok &= Expect(manager.Load(),"future configuration fields do not block known settings");
    auto stored = manager.GetUserSettings(); stored.hideMonitorOnSourceFocus = true;
    stored.language = L"zh-CN";
    stored.visuals.shadowEnabled = true; stored.visuals.cornersEnabled = true;
    stored.visuals.cornerMode = CornerMode::Percent; stored.visuals.cornerPercent = 11;
    stored.interaction.dashed=true;stored.interaction.borderWidth=6;stored.interaction.borderOpacity=42;
    stored.interaction.cutStyle=1;stored.interaction.cutEdges=true;stored.interaction.cursors[2].style=CursorStyle::Cross;
    stored.shortcuts[L"core.monitor"]=L"Ctrl+Alt+M";stored.shortcuts[L"plugin.nskry-ocr.launch"]=L"Ctrl+Alt+O";
    manager.SetUserSettings(stored);
    ok &= Expect(manager.Save() && manager.Load() && manager.GetUserSettings().hideMonitorOnSourceFocus &&
        manager.GetUserSettings().language == L"zh-CN" && manager.GetUserSettings().visuals == stored.visuals && manager.GetUserSettings().interaction==stored.interaction &&
        manager.GetHotkey(L"core.monitor")==L"Ctrl+Alt+M" && manager.GetHotkey(L"plugin.nskry-ocr.launch")==L"Ctrl+Alt+O",
        "atomic settings round trip including all input/custom visual values");
    {
        json::Value saved;unsigned future=0;std::wstring path;bool autoCheck=true;
        ok &= Expect(json::ParseUtf8File(manager.SettingsPath(),saved) && saved.Find(L"general") &&
            saved.Find(L"general")->GetUnsigned(L"future_setting",future) && future==42 && saved.Find(L"capture") &&
            saved.Find(L"capture")->GetString(L"default_save_directory",path) && path==L"future path" &&
            saved.Find(L"updates") && saved.Find(L"updates")->GetBool(L"check_automatically",autoCheck) && !autoCheck &&
            saved.Find(L"future"),"saving preserves reserved and unknown configuration sections");
    }
    {
        BITMAPINFO image{};image.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);image.bmiHeader.biWidth=32;image.bmiHeader.biHeight=-32;
        image.bmiHeader.biPlanes=1;image.bmiHeader.biBitCount=32;void* data{};
        HBITMAP bitmap=CreateDIBSection(nullptr,&image,DIB_RGB_COLORS,&data,nullptr,0);HDC dc=CreateCompatibleDC(nullptr);
        auto previous=SelectObject(dc,bitmap);std::fill_n(static_cast<unsigned long*>(data),32*32,0xffffffffUL);
        auto settings=stored;settings.interaction.cutStyle=0;settings.interaction.cutEdges=false;
        settings.interaction.cutColor=RGB(0,0,0);settings.interaction.cutOpacity=50;manager.SetUserSettings(settings);
        CutPreview(dc,{8,8,24,24},false);auto sample=GetPixel(dc,16,16);
        ok &= Expect(GetRValue(sample)>=126 && GetRValue(sample)<=129 && GetGValue(sample)==GetRValue(sample) && GetPixel(dc,2,2)==RGB(255,255,255),"cut mask alpha blends only the chosen strip");
        SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);manager.SetUserSettings(stored);
    }
    SetEnvironmentVariableW(L"LOCALAPPDATA", oldLocal.empty() ? nullptr : oldLocal.c_str());
    std::filesystem::remove(testDirectory / L"Nskry" / L"config" / L"settings.json");
    std::filesystem::remove(testDirectory / L"Nskry" / L"config");
    std::filesystem::remove(testDirectory / L"Nskry"); std::filesystem::remove(testDirectory);
    int networkCalls = 0, pluginSettingsCalls = 0;
    SettingsWindowServices services;
    services.getSettings = [&] { return manager.GetUserSettings(); };
    services.applySettings = [&](const UserSettings &next, std::wstring &) {
        manager.SetUserSettings(next);
        return true;
    };
    services.getPlugins = [] {
        SettingsPluginItem item;
        item.id = L"org.example.image"; item.name = L"Test image plugin";
        item.description = L"Plugin description from manifest only"; item.author = L"Test author";
        item.version = L"1.0.0"; item.status = L"Enabled - Not loaded"; item.loadPolicy = L"on_demand";
        item.hasSettings = true;
        return std::vector<SettingsPluginItem>{item};
    };
    services.showPluginSettings = [&](const std::wstring &id, HWND owner) {
        ++pluginSettingsCalls;
        return id == L"org.example.image" && owner;
    };
    UpdateTask::Completion pendingUpdate;int updateCancels=0;
    services.checkPluginUpdates = [&](UpdateTask::Completion completion) { ++networkCalls;pendingUpdate=std::move(completion);return true; };
    services.cancelPluginUpdate=[&]{++updateCancels;};
    SettingsWindow settings(std::move(services), [&] { closed = true; });
    settings.Show(nullptr);
    HWND window = OwnWindow(L"NskrySettings");
    ok &= Expect(window != nullptr, "settings created");
    HWND tabs = GetDlgItem(window, 100);
    HWND language = GetDlgItem(window, 105);
    SendMessageW(language, CB_SETCURSEL, 2, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(105, CBN_SELCHANGE), LPARAM(language));
    ok &= Expect(IsWindowEnabled(GetDlgItem(window, 130)), "changing language marks settings dirty");
    SendMessageW(window, WM_COMMAND, 130, LPARAM(GetDlgItem(window, 130)));
    ok &= Expect(manager.GetUserSettings().language == L"en" && Text(window) == L"Nskry Settings" &&
                 !IsWindowEnabled(GetDlgItem(window, 130)), "applying language refreshes active controls and clears the draft");
    tabs = GetDlgItem(window, 100);
    TabCtrl_SetCurSel(tabs, 4);
    NMHDR effects{tabs, 100, TCN_SELCHANGE}; SendMessageW(window, WM_NOTIFY, 100, reinterpret_cast<LPARAM>(&effects));
    for (int id : {140,141,142,143,144,149,150,151,152,153,154})
        ok &= Expect(InClient(window, GetDlgItem(window, id)) && IsWindowVisible(GetDlgItem(window, id)), "effects controls fit default client");
    SetWindowPos(window, nullptr, 0, 0, 420, 380, SWP_NOMOVE | SWP_NOZORDER);
    SCROLLINFO horiz{sizeof(horiz), SIF_RANGE | SIF_PAGE};
    SCROLLINFO vert{sizeof(vert), SIF_RANGE | SIF_PAGE};
    GetScrollInfo(window, SB_HORZ, &horiz);
    GetScrollInfo(window, SB_VERT, &vert);
    SendMessageW(window, WM_HSCROLL, SB_RIGHT, 0);
    SendMessageW(window, WM_VSCROLL, SB_BOTTOM, 0);
    ok &= Expect(horiz.nPage < static_cast<UINT>(horiz.nMax) && vert.nPage < static_cast<UINT>(vert.nMax) &&
                 InClient(window, GetDlgItem(window, 130)), "narrow settings scroll in both axes and keep Apply in view");
    SetWindowPos(window, nullptr, 0, 0, 720, 590, SWP_NOMOVE | SWP_NOZORDER);
    SendMessageW(window, WM_HSCROLL, SB_LEFT, 0);
    SendMessageW(window, WM_VSCROLL, SB_TOP, 0);
    TabCtrl_SetCurSel(tabs, 2);
    NMHDR selected{tabs, 100, TCN_SELCHANGE}; SendMessageW(window, WM_NOTIFY, 100, reinterpret_cast<LPARAM>(&selected));
    ok &= Expect(Text(GetDlgItem(window, 125)).find(L"Plugin description from manifest only") != std::wstring::npos, "selected plugin description shown");
    ok &= Expect(Text(GetDlgItem(window, 125)).find(UiText(L"Local / unverified", L"本地安装／未经验证")) != std::wstring::npos, "source does not imply certified publisher");
    for (int id : {120, 121, 122, 123, 124, 125, 126, 127, 128})
        ok &= Expect(InClient(window, GetDlgItem(window, id)) && IsWindowVisible(GetDlgItem(window, id)), "plugin controls fit default client");
    SetWindowPos(window, nullptr, 0, 0, 1000, 800, SWP_NOMOVE | SWP_NOZORDER);
    for (int id : {120, 125, 126, 127, 128}) ok &= Expect(InClient(window, GetDlgItem(window, id)), "resized plugin controls fit");
    ok &= Expect(pluginSettingsCalls == 0 && IsWindowEnabled(GetDlgItem(window, 128)), "plugin settings metadata discovery is inert");
    SendMessageW(window, WM_COMMAND, 128, LPARAM(GetDlgItem(window, 128)));
    ok &= Expect(pluginSettingsCalls == 1, "plugin settings opens only on explicit click");
    ok &= Expect(networkCalls == 0, "opening plugins page does not request updates");
    SendMessageW(window,WM_COMMAND,124,LPARAM(GetDlgItem(window,124)));
    auto stale=pendingUpdate;
    ok &= Expect(Text(GetDlgItem(window,124))==UiText(L"Cancel update", L"取消更新") && !IsWindowEnabled(GetDlgItem(window,123)),"active task UI and commit exclusion");
    SendMessageW(window,WM_COMMAND,124,LPARAM(GetDlgItem(window,124)));
    UpdateTaskResult late;late.message=L"stale response";stale(late);
    ok &= Expect(updateCancels==1 && Text(GetDlgItem(window,125)).find(L"stale response")==std::wstring::npos,"cancel ignores stale completion");
    SendMessageW(window,WM_COMMAND,124,LPARAM(GetDlgItem(window,124)));
    SendMessageW(window, WM_CLOSE, 0, 0);
    pendingUpdate(late);
    ok &= Expect(updateCancels==2,"close cancels active work and ignores late callback");
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
    HBITMAP pinBitmap = CreateBitmap(120, 80, 1, 32, nullptr);
    PinWindow pin(pinBitmap, 120, 80, {}, {}, {60, 60, 230, 170});
    ok &= Expect(pin.ShowInEditMode(), "small fixed image can enter annotation mode");
    RECT expanded{};
    GetWindowRect(pin.Hwnd(), &expanded);
    ok &= Expect(expanded.right - expanded.left > 170 && expanded.bottom - expanded.top > 110,
                 "annotation controls fit by temporarily expanding a small fixed image");
    IAccessible* annotation = nullptr;
    if (SUCCEEDED(::AccessibleObjectFromWindow(pin.Hwnd(), OBJID_CLIENT, IID_IAccessible,
        reinterpret_cast<void**>(&annotation))) && annotation) {
        LONG count{};
        ok &= Expect(SUCCEEDED(annotation->get_accChildCount(&count)) && count > 8,
                     "fixed image annotation commands are visible to assistive tools");
        VARIANT child{};
        child.vt = VT_I4;
        child.lVal = 1;
        LONG x{}, y{}, width{}, height{}, movedX{}, movedY{};
        const bool located = annotation->accLocation(&x, &y, &width, &height, child) == S_OK;
        const bool moved = ::SetWindowPos(pin.Hwnd(), nullptr, expanded.left + 36, expanded.top + 24,
                                          0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
        VARIANT hit{};
        ok &= Expect(located && moved &&
                     annotation->accLocation(&movedX, &movedY, &width, &height, child) == S_OK &&
                     movedX == x + 36 && movedY == y + 24 &&
                     annotation->accHitTest(movedX + width / 2, movedY + height / 2, &hit) == S_OK &&
                     hit.vt == VT_I4 && hit.lVal == child.lVal,
                     "fixed annotation accessibility follows a move without rebuilding the toolbar");
        ::VariantClear(&hit);
        ::SetWindowPos(pin.Hwnd(), nullptr, expanded.left, expanded.top, 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        annotation->Release();
    } else {
        ok &= Expect(false, "fixed image annotation toolbar publishes accessibility");
    }
    SendMessageW(pin.Hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
    SendMessageW(pin.Hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
    RECT restored{};
    GetWindowRect(pin.Hwnd(), &restored);
    ok &= Expect(restored.right - restored.left == 170 && restored.bottom - restored.top == 110,
                 "leaving annotation restores the original fixed image bounds");

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
