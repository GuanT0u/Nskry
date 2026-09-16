#include "pch.h"
#include "interop/window_enumerator.h"
#include "capture/d3d_device.h"
#include "capture/capture_session.h"
#include "ui/pip_window.h"
#include "ui/pin_window.h"
#include "ui/selection_window.h"
#include "core/plugin_manager.h"

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
static std::vector<std::unique_ptr<nskry::PinWindow>> g_pins;

static ULONG_PTR g_gdiplusToken = 0;

// ============================================================================
// Constants
// ============================================================================

static constexpr int  HOTKEY_CAPTURE = 1;
static constexpr int  HOTKEY_QUIT    = 2;
static constexpr UINT WM_CLEANUP     = WM_APP + 1;
static constexpr UINT WM_USER_TRAY   = WM_USER + 1;

// ============================================================================
// Forward declarations
// ============================================================================

static LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
static void ShowSelectionOverlay();
static void OnSelectionComplete(nskry::SelectionAction action, nskry::SelectionResult result);
static void StartPiP(HWND targetHwnd, nskry::CropRegion crop, nskry::AnnotationEngine engine = {});
static void CleanupPip();
static void CopyBitmapToClipboard(HBITMAP hbmp);
static void SaveBitmapToFile(HBITMAP hbmp, int w, int h);
static int  GetPngEncoderClsid(CLSID* pClsid);

// ============================================================================
// WinMain
// ============================================================================

int WINAPI wWinMain(
    [[maybe_unused]] HINSTANCE hInstance,
    [[maybe_unused]] HINSTANCE hPrevInstance,
    [[maybe_unused]] PWSTR     pCmdLine,
    [[maybe_unused]] int       nCmdShow)
{
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

    // Plugin Manager initialization
    NskryHostContext hostCtx{};
    hostCtx.mainHwnd = g_mainHwnd;
    hostCtx.d3dDevice = g_device ? g_device->Device() : nullptr;
    hostCtx.d3dContext = g_device ? g_device->Context() : nullptr;
    hostCtx.showNotification = [](const wchar_t* msg, int durationMs) {
        // Can be hooked to toast or status
    };
    hostCtx.copyBitmapToClipboard = CopyBitmapToClipboard;
    nskry::PluginManager::Instance().Initialize(&hostCtx);

    // Global hotkeys
    if (!::RegisterHotKey(g_mainHwnd, HOTKEY_CAPTURE, MOD_CONTROL | MOD_ALT, 'A')) {
        ::MessageBoxW(nullptr,
            L"Failed to register Ctrl+Alt+A.\n"
            L"Another program may be using this shortcut.",
            L"Nskry", MB_ICONERROR);
        return 1;
    }
    ::RegisterHotKey(g_mainHwnd, HOTKEY_QUIT, MOD_CONTROL | MOD_ALT, 'Q');

    // System Tray Icon
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_mainHwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_USER_TRAY;
    nid.hIcon            = ::LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); /* IDI_APPLICATION */
    wcscpy_s(nid.szTip, L"Nskry (Ctrl+Alt+A)");
    ::Shell_NotifyIconW(NIM_ADD, &nid);

    // Notify user
    ::MessageBoxW(nullptr,
        L"Nskry is running in the background.\n\n"
        L"  Ctrl+Alt+A \u2014 capture a window\n"
        L"  Ctrl+Alt+Q \u2014 quit",
        L"Nskry", MB_ICONINFORMATION);

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
    g_selectionWindow.reset();
    ::UnregisterHotKey(g_mainHwnd, HOTKEY_CAPTURE);
    ::UnregisterHotKey(g_mainHwnd, HOTKEY_QUIT);
    g_device.reset();
    Gdiplus::GdiplusShutdown(g_gdiplusToken);

    return static_cast<int>(msg.wParam);
}

// ============================================================================
// Main window proc
// ============================================================================

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        if (wp == HOTKEY_CAPTURE) ShowSelectionOverlay();
        if (wp == HOTKEY_QUIT)    ::PostQuitMessage(0);
        return 0;
    case WM_CLEANUP:
        CleanupPip();
        return 0;
    case WM_USER_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            ShowSelectionOverlay();
        }
        else if (LOWORD(lp) == WM_RBUTTONUP) {
            POINT pt;
            ::GetCursorPos(&pt);
            HMENU hMenu = ::CreatePopupMenu();
            ::InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, 1001, L"Capture (Ctrl+Alt+A)");
            ::InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
            ::InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_STRING, 1002, L"Quit (Ctrl+Alt+Q)");
            ::SetForegroundWindow(hwnd);
            int cmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
            ::DestroyMenu(hMenu);
            if (cmd == 1001) ShowSelectionOverlay();
            if (cmd == 1002) ::PostQuitMessage(0);
        }
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

static void CleanupPip() {
    g_capture.reset();
    g_pip.reset();
}

// ============================================================================
// Selection overlay
// ============================================================================

static void ShowSelectionOverlay() {
    g_selectionWindow.reset();

    g_selectionWindow = std::make_unique<nskry::SelectionWindow>(
        [](nskry::SelectionAction action, nskry::SelectionResult result) {
            g_selectionWindow.reset();
            OnSelectionComplete(action, std::move(result));
        });
    g_selectionWindow->Show();
}

// ============================================================================
// Handle the user's chosen action
// ============================================================================

static void OnSelectionComplete(nskry::SelectionAction action, nskry::SelectionResult result) {
    switch (action) {
    case nskry::SelectionAction::Copy:
        if (result.bitmap) {
            CopyBitmapToClipboard(result.bitmap);
            // SetClipboardData takes ownership — don't delete
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
                result.bitmap, result.bitmapWidth, result.bitmapHeight);
            pin->Show();
            g_pins.push_back(std::move(pin));
            // PinWindow takes ownership of bitmap
        }
        break;

    case nskry::SelectionAction::PiP:
        if (result.bitmap) ::DeleteObject(result.bitmap);
        if (result.targetHwnd)
            StartPiP(result.targetHwnd, result.crop, std::move(result.annotationEngine));
        break;

    case nskry::SelectionAction::Cancel:
        // bitmap already cleaned up by SelectionWindow
        break;
    }
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
            std::move(engine));

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

static void CopyBitmapToClipboard(HBITMAP hbmp) {
    if (!::OpenClipboard(g_mainHwnd)) return;
    ::EmptyClipboard();
    ::SetClipboardData(CF_BITMAP, hbmp);
    ::CloseClipboard();
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
