#include "pch.h"
#include "interop/window_enumerator.h"
#include "capture/d3d_device.h"
#include "capture/capture_session.h"
#include "ui/pip_window.h"
#include "ui/pin_window.h"
#include "ui/long_image_crop_window.h"
#include "ui/selection_window.h"
#include "ui/settings/settings_window.h"
#include "core/command_registry.h"
#include "core/hotkey_manager.h"
#include "core/plugin_manager.h"
#include "core/plugin_package_manager.h"
#include "core/plugin_update_manager.h"
#include "core/plugin_registry.h"
#include "core/settings_manager.h"

// GDI+ for PNG saving (needs min/max workaround with NOMINMAX)
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include <commdlg.h>     // GetSaveFileNameW
#include <shlobj.h>      // SHGetKnownFolderPath
#include <shellapi.h>    // Shell_NotifyIconW

// ============================================================================
// Application globals
// ============================================================================

static HWND                                      g_mainHwnd = nullptr;
static std::shared_ptr<nskry::D3DDevice>         g_device;
static std::unique_ptr<nskry::CaptureSession>    g_capture;
static std::unique_ptr<nskry::PipWindow>         g_pip;
static std::unique_ptr<nskry::SelectionWindow>   g_selectionWindow;
static std::unique_ptr<nskry::SettingsWindow>    g_settingsWindow;
static std::unique_ptr<nskry::LongImageCropWindow> g_longImageEditor;
static std::vector<std::unique_ptr<nskry::PinWindow>> g_pins;
static nskry::SettingsWindowServices*             g_settingsServices = nullptr;
static bool                                      g_captureWorkflowActive = false;

static ULONG_PTR g_gdiplusToken = 0;

// ============================================================================
// Constants
// ============================================================================

static constexpr UINT WM_CLEANUP     = WM_APP + 1;
static constexpr UINT WM_USER_TRAY   = WM_USER + 1;
static constexpr UINT WM_SETTINGS_CLOSED = WM_APP + 42;
static constexpr UINT WM_LONG_IMAGE_EDITOR_CLOSED = WM_APP + 43;
static constexpr UINT WM_PIN_CLOSED = WM_APP + 44;

// ============================================================================
// Forward declarations
// ============================================================================

static LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
static void ShowSelectionOverlay();
static void ShowSettings();
static void OnSelectionComplete(nskry::SelectionAction action, nskry::SelectionResult result);
static void StartPiP(HWND targetHwnd, nskry::CropRegion crop, nskry::AnnotationEngine engine = {});
static void CleanupPip();
static int32_t CopyBitmapToClipboard(HBITMAP hbmp);
static void OpenBitmapEditor(HBITMAP hbmp, int width, int height);
static void ShowPluginNotification(const wchar_t* message, int durationMs);
static void NotifyPinClosed(nskry::PinWindow* pin);
static void ExecuteImagePlugin(const std::wstring& pluginId, HBITMAP bitmap, int width, int height,
                               RECT sourceRegion, HWND sourceHwnd);
static nskry::ImageActions GetEnabledImageActions();
static void SaveBitmapToFile(HBITMAP hbmp, int w, int h);
static int  GetPngEncoderClsid(CLSID* pClsid);
static bool RunPluginPackageCli(int& exitCode);
static bool SetRunAtStartup(bool enabled);

struct StartupHintState {
    std::wstring text;
    HWND checkbox{};
    bool disableFutureHints{};
};

static LRESULT CALLBACK StartupHintWndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* state = reinterpret_cast<StartupHintState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        HFONT font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        HWND text = ::CreateWindowExW(0, L"STATIC", state ? state->text.c_str() : L"", WS_CHILD | WS_VISIBLE,
            20, 18, 390, 92, hwnd, nullptr, nullptr, nullptr);
        if (text) ::SendMessageW(text, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        state->checkbox = ::CreateWindowExW(0, L"BUTTON", L"Do not show this message again", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            20, 118, 260, 24, hwnd, reinterpret_cast<HMENU>(1), nullptr, nullptr);
        HWND ok = ::CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            330, 152, 78, 28, hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        if (state->checkbox) ::SendMessageW(state->checkbox, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (ok) ::SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            if (state && state->checkbox) state->disableFutureHints = ::SendMessageW(state->checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            ::DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE: ::DestroyWindow(hwnd); return 0;
    case WM_NCDESTROY: ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); break;
    }
    return ::DefWindowProcW(hwnd, message, wp, lp);
}

static bool ShowStartupHint(const std::wstring& text) {
    static std::once_flag registered;
    std::call_once(registered, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc); wc.lpfnWndProc = StartupHintWndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr); wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
        wc.lpszClassName = L"NskryStartupHint";
        ::RegisterClassExW(&wc);
    });
    StartupHintState state{ text };
    HWND dialog = ::CreateWindowExW(WS_EX_DLGMODALFRAME, L"NskryStartupHint", L"Nskry is ready",
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 430, 225,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), &state);
    if (!dialog) return false;
    ::SetForegroundWindow(dialog);
    MSG message{};
    while (::IsWindow(dialog) && ::GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(dialog, &message)) { ::TranslateMessage(&message); ::DispatchMessageW(&message); }
    }
    return state.disableFutureHints;
}

// ============================================================================
// WinMain
// ============================================================================

int WINAPI wWinMain(
    [[maybe_unused]] HINSTANCE hInstance,
    [[maybe_unused]] HINSTANCE hPrevInstance,
    [[maybe_unused]] PWSTR     pCmdLine,
    [[maybe_unused]] int       nCmdShow)
{
    int packageCliExitCode = 0;
    if (RunPluginPackageCli(packageCliExitCode)) return packageCliExitCode;

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // GDI+ init (for PNG saving)
    Gdiplus::GdiplusStartupInput gdipInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdipInput, nullptr);

    // D3D11 device
    try {
        g_device = std::make_shared<nskry::D3DDevice>();
    } catch (const winrt::hresult_error& e) {
        ::MessageBoxW(nullptr,
            (std::wstring(L"Failed to create D3D11 device:\n") + e.message().c_str()).c_str(),
            L"Nskry", MB_ICONERROR);
        return 1;
    }

    // Hidden message-only main window
    {
        WNDCLASSEXW wc{};
        wc.cbSize       = sizeof(wc);
        wc.lpfnWndProc  = MainWndProc;
        wc.hInstance     = hInstance;
        wc.lpszClassName = L"NskryMain";
        ::RegisterClassExW(&wc);
    }
    g_mainHwnd = ::CreateWindowExW(
        0, L"NskryMain", L"Nskry", 0,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr);

    // Stage A core services: settings drive stable command IDs, which in turn
    // drive global hotkey registrations. UI can later edit settings without
    // reaching into main.cpp globals.
    auto& settings = nskry::SettingsManager::Instance();
    settings.Load();

    auto& commands = nskry::CommandRegistry::Instance();
    commands.Register(L"core.capture", []() { ShowSelectionOverlay(); });
    commands.Register(L"core.exit", []() { ::PostQuitMessage(0); });
    commands.Register(L"core.settings", []() { ShowSettings(); });

    auto& hotkeys = nskry::HotkeyManager::Instance();
    hotkeys.Initialize(g_mainHwnd);
    if (!hotkeys.Register(L"core.capture", settings.GetHotkey(L"core.capture"))) {
        ::MessageBoxW(nullptr,
            (L"Failed to register capture shortcut: " + settings.GetHotkey(L"core.capture") +
             L".\nAnother program may be using it.").c_str(),
            L"Nskry", MB_ICONERROR);
        return 1;
    }
    if (!hotkeys.Register(L"core.exit", settings.GetHotkey(L"core.exit"))) {
        ::MessageBoxW(nullptr,
            (L"Failed to register exit shortcut: " + settings.GetHotkey(L"core.exit") +
             L".\nYou can still quit from the tray menu.").c_str(),
            L"Nskry", MB_ICONWARNING);
    }

    // Plugin Manager initialization
    NskryHostContext hostCtx{};
    hostCtx.structSize = sizeof(hostCtx);
    hostCtx.apiVersion = NSKRY_PLUGIN_API_VERSION;
    hostCtx.mainHwnd = g_mainHwnd;
    hostCtx.d3dDevice = g_device ? g_device->Device() : nullptr;
    hostCtx.d3dContext = g_device ? g_device->Context() : nullptr;
    hostCtx.showNotification = ShowPluginNotification;
    hostCtx.copyBitmapToClipboard = CopyBitmapToClipboard;
    hostCtx.openBitmapEditor = OpenBitmapEditor;
    nskry::PluginRegistry pluginRegistry;
    nskry::PluginPackageManager packageManager(pluginRegistry);
    nskry::PluginUpdateManager pluginUpdateManager(pluginRegistry, packageManager);
    std::wstring packageError;
    if (!pluginRegistry.Initialize() || !packageManager.Initialize() ||
        !packageManager.ApplyPendingOperations(&packageError) || !pluginRegistry.Load()) {
        std::wstring registryError = L"Plugin registry could not be initialized. Plugins will be unavailable this session.";
        if (!packageError.empty()) registryError += L"\n\n" + packageError;
        ::MessageBoxW(nullptr, registryError.c_str(), L"Nskry", MB_ICONWARNING);
    } else {
        nskry::PluginManager::Instance().Initialize(pluginRegistry, hostCtx);
    }

    nskry::SettingsWindowServices settingsServices;
    settingsServices.getSettings = [&settings]() { return settings.GetUserSettings(); };
    settingsServices.applySettings = [&settings, &hotkeys](const nskry::UserSettings& next, std::wstring& error) {
        const nskry::UserSettings previous = settings.GetUserSettings();
        if (next.captureShortcut.empty() || next.exitShortcut.empty()) { error = L"Shortcuts cannot be empty."; return false; }
        if (next.captureShortcut != previous.captureShortcut && !hotkeys.Rebind(L"core.capture", next.captureShortcut)) {
            error = L"The capture shortcut is invalid or already in use."; return false;
        }
        if (next.exitShortcut != previous.exitShortcut && !hotkeys.Rebind(L"core.exit", next.exitShortcut)) {
            if (next.captureShortcut != previous.captureShortcut) hotkeys.Rebind(L"core.capture", previous.captureShortcut);
            error = L"The exit shortcut is invalid or already in use."; return false;
        }
        if (next.runAtStartup != previous.runAtStartup && !SetRunAtStartup(next.runAtStartup)) {
            if (next.captureShortcut != previous.captureShortcut) hotkeys.Rebind(L"core.capture", previous.captureShortcut);
            if (next.exitShortcut != previous.exitShortcut) hotkeys.Rebind(L"core.exit", previous.exitShortcut);
            error = L"Unable to update the Windows startup setting."; return false;
        }
        settings.SetUserSettings(next);
        if (!settings.Save()) { error = L"Unable to save settings.json."; return false; }
        return true;
    };
    settingsServices.getPlugins = [&pluginRegistry]() {
        std::vector<nskry::SettingsPluginItem> items;
        for (const nskry::PluginRecord* record : pluginRegistry.GetAll()) {
            nskry::SettingsPluginItem item;
            item.id = record->manifest.id; item.name = record->manifest.name; item.version = record->manifest.version;
            item.author = record->manifest.author; item.enabled = record->enabled;
            if (!record->manifest.IsCompatibleWithHost()) item.status = L"Incompatible with this Nskry version";
            else item.status = record->enabled ? (record->loaded ? L"Enabled · Loaded" : L"Enabled · Not loaded") : L"Disabled";
            items.push_back(std::move(item));
        }
        return items;
    };
    settingsServices.setPluginEnabled = [&pluginRegistry](const std::wstring& id, bool enabled) { return pluginRegistry.SetEnabled(id, enabled); };
    settingsServices.uninstallPlugin = [&packageManager](const std::wstring& id, std::wstring& error) {
        const auto result = packageManager.Uninstall(id); error = result.message; return result.success;
    };
    settingsServices.inspectPluginPackage = [&packageManager](const std::wstring& path, nskry::PluginManifest& manifest, std::wstring& error) {
        return packageManager.InspectPackage(path, manifest, &error);
    };
    settingsServices.installThirdPartyPlugin = [&packageManager](const std::wstring& path) {
        return packageManager.InstallPackage(path, nskry::PluginSource::ThirdParty);
    };
    settingsServices.checkPluginUpdates = [&pluginUpdateManager, &settings](std::wstring& error) {
        return pluginUpdateManager.CheckForUpdates(settings.GetOfficialPluginCatalogUrl(), &error);
    };
    settingsServices.downloadPluginUpdate = [&pluginUpdateManager](const nskry::PluginUpdate& update) {
        return pluginUpdateManager.DownloadAndStage(update);
    };
    g_settingsServices = &settingsServices;

    // System Tray Icon
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_mainHwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_USER_TRAY;
    nid.hIcon            = ::LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); /* IDI_APPLICATION */
    const std::wstring captureShortcut = settings.GetHotkey(L"core.capture");
    wcscpy_s(nid.szTip, (L"Nskry (" + captureShortcut + L")").c_str());
    ::Shell_NotifyIconW(NIM_ADD, &nid);

    // Notify user
    auto startupSettings = settings.GetUserSettings();
    if (startupSettings.showStartupHint) {
        const std::wstring startupMessage = std::wstring(L"Nskry is running in the background.\n\n") +
            L"  " + settings.GetHotkey(L"core.capture") + L" \u2014 capture a window\n" +
            L"  " + settings.GetHotkey(L"core.exit") + L" \u2014 quit";
        if (ShowStartupHint(startupMessage)) {
            startupSettings.showStartupHint = false;
            settings.SetUserSettings(startupSettings);
            settings.Save();
        }
    }

    // Message loop
    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    // Cleanup
    nskry::PluginManager::Instance().Shutdown();
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
    CleanupPip();
    g_pins.clear();
    g_longImageEditor.reset();
    g_selectionWindow.reset();
    g_settingsWindow.reset();
    g_settingsServices = nullptr;
    nskry::HotkeyManager::Instance().Shutdown();
    nskry::CommandRegistry::Instance().Clear();
    g_device.reset();
    Gdiplus::GdiplusShutdown(g_gdiplusToken);

    return static_cast<int>(msg.wParam);
}

// Package CLI is intentionally small: installers and future Settings UI use
// the same PluginPackageManager rather than maintaining another install path.
static bool RunPluginPackageCli(int& exitCode) {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return false;

    std::wstring packagePath;
    nskry::PluginSource source = nskry::PluginSource::Local;
    bool silent = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--install-plugin" && i + 1 < argc) packagePath = argv[++i];
        else if (arg == L"--source" && i + 1 < argc) {
            const std::wstring value = argv[++i];
            if (value == L"official") source = nskry::PluginSource::Official;
            else if (value == L"third_party") source = nskry::PluginSource::ThirdParty;
        } else if (arg == L"--silent") {
            silent = true;
        }
    }
    ::LocalFree(argv);
    if (packagePath.empty()) return false;

    nskry::PluginRegistry registry;
    nskry::PluginPackageManager packageManager(registry);
    nskry::PluginPackageResult result;
    if (!registry.Initialize() || !packageManager.Initialize() || !registry.Load()) {
        result.message = L"Plugin package service could not be initialized.";
    } else {
        result = packageManager.InstallPackage(packagePath, source);
    }
    exitCode = result.success ? 0 : 1;
    if (!silent) {
        ::MessageBoxW(nullptr, result.message.c_str(), L"Nskry Plugin Installer",
            result.success ? MB_ICONINFORMATION : MB_ICONERROR);
    }
    return true;
}

static bool SetRunAtStartup(bool enabled) {
    HKEY key{};
    const wchar_t* path = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    LONG result = ERROR_SUCCESS;
    if (enabled) {
        wchar_t executable[MAX_PATH]{};
        if (::GetModuleFileNameW(nullptr, executable, MAX_PATH) == 0) result = ERROR_FILE_NOT_FOUND;
        else {
            const std::wstring command = L"\"" + std::wstring(executable) + L"\"";
            result = ::RegSetValueExW(key, L"Nskry", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        result = ::RegDeleteValueW(key, L"Nskry");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    ::RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

// ============================================================================
// Main window proc
// ============================================================================

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        nskry::CommandRegistry::Instance().Execute(
            nskry::HotkeyManager::Instance().CommandForHotkeyId(static_cast<int>(wp)));
        return 0;
    case WM_CLEANUP:
        CleanupPip();
        return 0;
    case WM_SETTINGS_CLOSED:
        g_settingsWindow.reset();
        return 0;
    case WM_LONG_IMAGE_EDITOR_CLOSED:
        g_longImageEditor.reset();
        return 0;
    case WM_PIN_CLOSED: {
        const auto* closed = reinterpret_cast<nskry::PinWindow*>(lp);
        std::erase_if(g_pins, [closed](const auto& pin) { return pin.get() == closed; });
        return 0;
    }
    case WM_USER_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            nskry::CommandRegistry::Instance().Execute(L"core.capture");
        }
        else if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt;
            ::GetCursorPos(&pt);
            HMENU hMenu = ::CreatePopupMenu();
            const std::wstring captureLabel = L"Capture (" + nskry::SettingsManager::Instance().GetHotkey(L"core.capture") + L")";
            const std::wstring exitLabel = L"Quit (" + nskry::SettingsManager::Instance().GetHotkey(L"core.exit") + L")";
            ::InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, 1001, captureLabel.c_str());
            ::InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_STRING, 1003, L"Settings...");
            ::InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
            ::InsertMenuW(hMenu, 3, MF_BYPOSITION | MF_STRING, 1002, exitLabel.c_str());
            ::SetForegroundWindow(hwnd);
            int cmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
            ::DestroyMenu(hMenu);
            if (cmd == 1001) nskry::CommandRegistry::Instance().Execute(L"core.capture");
            if (cmd == 1003) nskry::CommandRegistry::Instance().Execute(L"core.settings");
            if (cmd == 1002) nskry::CommandRegistry::Instance().Execute(L"core.exit");
        }
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowSettings() {
    if (!g_settingsServices || g_settingsWindow) return;
    g_settingsWindow = std::make_unique<nskry::SettingsWindow>(*g_settingsServices, []() {
        ::PostMessageW(g_mainHwnd, WM_SETTINGS_CLOSED, 0, 0);
    });
    g_settingsWindow->Show(g_mainHwnd);
}

static void CleanupPip() {
    g_capture.reset();
    g_pip.reset();
}

static void OpenBitmapEditor(HBITMAP hbmp, int width, int height) {
    if (!hbmp || width <= 0 || height <= 0) {
        if (hbmp) ::DeleteObject(hbmp);
        return;
    }
    // A capture session owns its editor. Keeping one focused avoids two large
    // live DIBs consuming memory if the toolbar button is clicked repeatedly.
    if (g_longImageEditor) {
        ::DeleteObject(hbmp);
        return;
    }
    g_longImageEditor = std::make_unique<nskry::LongImageCropWindow>(hbmp, width, height,
        [](HBITMAP cropped, int croppedWidth, int croppedHeight) {
            auto pin = std::make_unique<nskry::PinWindow>(cropped, croppedWidth, croppedHeight,
                                                          NotifyPinClosed, GetEnabledImageActions());
            if (pin->ShowInEditMode()) g_pins.push_back(std::move(pin));
            else ShowPluginNotification(L"The annotation window could not be created.", 3500);
        },
        []() { ::PostMessageW(g_mainHwnd, WM_LONG_IMAGE_EDITOR_CLOSED, 0, 0); },
        GetEnabledImageActions());
    if (!g_longImageEditor->Show(g_mainHwnd)) {
        g_longImageEditor.reset();
        ShowPluginNotification(L"The long screenshot editor could not be created.", 3500);
    }
}

// ============================================================================
// Selection overlay
// ============================================================================

static void ShowSelectionOverlay() {
    if (g_captureWorkflowActive) {
        ShowPluginNotification(L"A capture or plugin workflow is already active.", 2500);
        return;
    }
    g_captureWorkflowActive = true;
    g_selectionWindow.reset();

    g_selectionWindow = std::make_unique<nskry::SelectionWindow>(
        [](nskry::SelectionAction action, nskry::SelectionResult result) {
            g_selectionWindow.reset();
            OnSelectionComplete(action, std::move(result));
            g_captureWorkflowActive = false;
        }, nskry::PluginManager::Instance().GetEnabledToolbarActions());
    g_selectionWindow->Show();
}

// ============================================================================
// Handle the user's chosen action
// ============================================================================

static void OnSelectionComplete(nskry::SelectionAction action, nskry::SelectionResult result) {
    switch (action) {
    case nskry::SelectionAction::Copy:
        if (result.bitmap) {
            if (!CopyBitmapToClipboard(result.bitmap))
                ShowPluginNotification(L"The screenshot could not be copied to the clipboard.", 3500);
            ::DeleteObject(result.bitmap);
        }
        break;

    case nskry::SelectionAction::Save:
        if (result.bitmap) {
            SaveBitmapToFile(result.bitmap, result.bitmapWidth, result.bitmapHeight);
            ::DeleteObject(result.bitmap);
        }
        break;

    case nskry::SelectionAction::Pin:
        if (result.bitmap) {
            auto pin = std::make_unique<nskry::PinWindow>(
                result.bitmap, result.bitmapWidth, result.bitmapHeight, NotifyPinClosed,
                GetEnabledImageActions());
            if (pin->Show()) g_pins.push_back(std::move(pin));
            else ShowPluginNotification(L"The pin window could not be created.", 3500);
            // PinWindow takes ownership of bitmap
        }
        break;

    case nskry::SelectionAction::PiP:
        if (result.bitmap) ::DeleteObject(result.bitmap);
        if (result.targetHwnd)
            StartPiP(result.targetHwnd, result.crop, std::move(result.annotationEngine));
        break;

    case nskry::SelectionAction::Plugin:
        if (result.bitmap && !result.pluginId.empty()) {
            NskryHostContext context{};
            context.structSize = sizeof(context);
            context.apiVersion = NSKRY_PLUGIN_API_VERSION;
            context.mainHwnd = g_mainHwnd;
            context.d3dDevice = g_device ? g_device->Device() : nullptr;
            context.d3dContext = g_device ? g_device->Context() : nullptr;
            context.capturedBitmap = result.bitmap;
            context.capturedRegion = result.screenRegion;
            context.sourceHwnd = result.targetHwnd;
            context.showNotification = ShowPluginNotification;
            context.copyBitmapToClipboard = CopyBitmapToClipboard;
            context.openBitmapEditor = OpenBitmapEditor;
            if (!nskry::PluginManager::Instance().ExecutePlugin(result.pluginId, context))
                ShowPluginNotification(L"The plugin could not be loaded or did not complete successfully.", 3500);
            ::DeleteObject(result.bitmap);
        }
        break;

    case nskry::SelectionAction::Cancel:
        // bitmap already cleaned up by SelectionWindow
        break;
    }
}

static void ExecuteImagePlugin(const std::wstring& pluginId, HBITMAP bitmap, int width, int height,
                               RECT sourceRegion, HWND sourceHwnd) {
    if (pluginId.empty() || !bitmap || width <= 0 || height <= 0) return;
    NskryHostContext context{};
    context.structSize = sizeof(context);
    context.apiVersion = NSKRY_PLUGIN_API_VERSION;
    context.mainHwnd = g_mainHwnd;
    context.d3dDevice = g_device ? g_device->Device() : nullptr;
    context.d3dContext = g_device ? g_device->Context() : nullptr;
    context.capturedBitmap = bitmap;
    context.capturedRegion = sourceRegion;
    context.sourceHwnd = sourceHwnd;
    context.showNotification = ShowPluginNotification;
    context.copyBitmapToClipboard = CopyBitmapToClipboard;
    context.openBitmapEditor = OpenBitmapEditor;
    if (!nskry::PluginManager::Instance().ExecutePlugin(pluginId, context))
        ShowPluginNotification(L"The image action could not be loaded or did not complete successfully.", 3500);
}

static nskry::ImageActions GetEnabledImageActions() {
    nskry::ImageActions actions;
    for (const nskry::PluginImageAction& plugin : nskry::PluginManager::Instance().GetEnabledImageActions()) {
        nskry::ImageAction action;
        action.id = plugin.id;
        action.label = plugin.label;
        action.invoke = [id = plugin.id](HBITMAP bitmap, int width, int height, RECT region, HWND sourceHwnd) {
            ExecuteImagePlugin(id, bitmap, width, height, region, sourceHwnd);
        };
        actions.push_back(std::move(action));
    }
    return actions;
}

// ============================================================================
// Start PiP live capture
// ============================================================================

static void StartPiP(HWND targetHwnd, nskry::CropRegion crop, nskry::AnnotationEngine engine) {
    CleanupPip();

    try {
        g_pip = std::make_unique<nskry::PipWindow>(
            g_device,
            static_cast<UINT>(crop.width),
            static_cast<UINT>(crop.height),
            []() { ::PostMessageW(g_mainHwnd, WM_CLEANUP, 0, 0); },
            std::move(engine),
            GetEnabledImageActions());

        g_capture = std::make_unique<nskry::CaptureSession>(
            g_device, targetHwnd, crop);

        g_capture->SetFrameCallback(
            [](ID3D11Texture2D* tex, UINT w, UINT h) {
                if (g_pip) g_pip->RenderFrame(tex, w, h);
            });

        g_capture->Start();
        g_pip->Show();

    } catch (const winrt::hresult_error& e) {
        CleanupPip();
        std::wstring msg = L"Capture failed:\n";
        msg += e.message().c_str();
        ::MessageBoxW(nullptr, msg.c_str(), L"Nskry", MB_ICONERROR);
    } catch (const std::exception& e) {
        CleanupPip();
        ::MessageBoxA(nullptr, e.what(), "Nskry", MB_ICONERROR);
    }
}

// ============================================================================
// Clipboard
// ============================================================================

static int32_t CopyBitmapToClipboard(HBITMAP hbmp) {
    if (!hbmp) return 0;
    BITMAP bitmapInfo{};
    if (!::GetObjectW(hbmp, sizeof(bitmapInfo), &bitmapInfo) || bitmapInfo.bmWidth <= 0 || bitmapInfo.bmHeight <= 0) return 0;

    const unsigned long long imageBytes = static_cast<unsigned long long>(bitmapInfo.bmWidth) *
        static_cast<unsigned long long>(bitmapInfo.bmHeight) * 4ULL;
    if (imageBytes > MAXDWORD || imageBytes > SIZE_MAX - sizeof(BITMAPINFOHEADER)) return 0;

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = bitmapInfo.bmWidth;
    header.biHeight = bitmapInfo.bmHeight; // bottom-up CF_DIB
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(imageBytes);

    HGLOBAL dib = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(header) + header.biSizeImage);
    if (!dib) return 0;
    void* memory = ::GlobalLock(dib);
    if (!memory) { ::GlobalFree(dib); return 0; }
    std::memcpy(memory, &header, sizeof(header));
    BITMAPINFO request{};
    request.bmiHeader = header;
    HDC dc = ::GetDC(nullptr);
    if (!dc) { ::GlobalUnlock(dib); ::GlobalFree(dib); return 0; }
    const int rows = ::GetDIBits(dc, hbmp, 0, bitmapInfo.bmHeight,
        static_cast<BYTE*>(memory) + sizeof(header), &request, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, dc);
    ::GlobalUnlock(dib);
    if (rows != bitmapInfo.bmHeight) { ::GlobalFree(dib); return 0; }

    HBITMAP bitmapCopy = static_cast<HBITMAP>(::CopyImage(hbmp, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
    bool opened = false;
    for (int attempt = 0; attempt < 6 && !opened; ++attempt) {
        opened = ::OpenClipboard(nullptr) != FALSE;
        if (!opened) ::Sleep(20);
    }
    if (!opened) { ::GlobalFree(dib); if (bitmapCopy) ::DeleteObject(bitmapCopy); return 0; }
    if (!::EmptyClipboard()) {
        ::CloseClipboard(); ::GlobalFree(dib); if (bitmapCopy) ::DeleteObject(bitmapCopy); return 0;
    }
    const bool dibPublished = ::SetClipboardData(CF_DIB, dib) != nullptr;
    if (!dibPublished) ::GlobalFree(dib);
    const bool bitmapPublished = bitmapCopy && ::SetClipboardData(CF_BITMAP, bitmapCopy) != nullptr;
    if (bitmapCopy && !bitmapPublished) ::DeleteObject(bitmapCopy);
    ::CloseClipboard();
    return dibPublished || bitmapPublished ? 1 : 0;
}

static void ShowPluginNotification(const wchar_t* message, int durationMs) {
    if (!message || !*message || !g_mainHwnd) return;
    if (!nskry::SettingsManager::Instance().GetUserSettings().notifications) return;
    NOTIFYICONDATAW notification{};
    notification.cbSize = sizeof(notification);
    notification.hWnd = g_mainHwnd;
    notification.uID = 1;
    notification.uFlags = NIF_INFO;
    notification.dwInfoFlags = NIIF_INFO;
    notification.uTimeout = static_cast<UINT>((std::clamp)(durationMs, 1000, 30000));
    ::wcsncpy_s(notification.szInfo, message, _TRUNCATE);
    ::wcsncpy_s(notification.szInfoTitle, L"Nskry", _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &notification);
}

static void NotifyPinClosed(nskry::PinWindow* pin) {
    if (pin && g_mainHwnd)
        ::PostMessageW(g_mainHwnd, WM_PIN_CLOSED, 0, reinterpret_cast<LPARAM>(pin));
}

// ============================================================================
// Save to file (PNG via GDI+)
// ============================================================================

static int GetPngEncoderClsid(CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    auto pInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
    if (!pInfo) return -1;

    Gdiplus::GetImageEncoders(num, size, pInfo);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(pInfo[i].MimeType, L"image/png") == 0) {
            *pClsid = pInfo[i].Clsid;
            free(pInfo);
            return static_cast<int>(i);
        }
    }
    free(pInfo);
    return -1;
}

static void SaveBitmapToFile(HBITMAP hbmp, int /*w*/, int /*h*/) {
    // Show save dialog
    wchar_t szFile[MAX_PATH] = L"screenshot.png";

    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = g_mainHwnd;
    ofn.lpstrFilter  = L"PNG Files\0*.png\0All Files\0*.*\0";
    ofn.lpstrFile    = szFile;
    ofn.nMaxFile     = MAX_PATH;
    ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt  = L"png";
    ofn.lpstrTitle   = L"Nskry \u2014 Save Screenshot";

    if (!::GetSaveFileNameW(&ofn))
        return;   // User cancelled

    // Save using GDI+
    CLSID clsid;
    if (GetPngEncoderClsid(&clsid) < 0) {
        ::MessageBoxW(nullptr, L"PNG encoder not found.", L"Nskry", MB_ICONERROR);
        return;
    }

    Gdiplus::Bitmap bmp(hbmp, nullptr);
    Gdiplus::Status st = bmp.Save(szFile, &clsid);
    if (st != Gdiplus::Ok) {
        ::MessageBoxW(nullptr, L"Failed to save PNG file.", L"Nskry", MB_ICONERROR);
    }
}
