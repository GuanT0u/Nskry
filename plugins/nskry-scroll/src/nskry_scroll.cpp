#include "../../common/native_ui.h"
#include <shellscalingapi.h>
#include "nskry_plugin.h"
#include "scroll_composite.h"
#include "scroll_local_match.h"
#include "scroll_orientation.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr UINT_PTR kCaptureTimer = 1;
// Sampling at roughly 11 fps lets a normal wheel/trackpad gesture be
// committed while it is still in flight, rather than only after it rests.
constexpr UINT kCaptureIntervalMs = 90;
constexpr UINT kAutoScrollIntervalMs = 450;
constexpr int kAutoWheelDelta = WHEEL_DELTA / 2;
constexpr int kAutoButton = 101, kCopyButton = 102, kEditButton = 103, kCancelButton = 104;
constexpr UINT kFocusSource = WM_APP + 1;
constexpr int kStartButton = 105, kAutoLeftButton = 106;
using namespace nskry::scroll;
constexpr size_t kMaxStitchedBytes = 192ull * 1024 * 1024;
constexpr size_t kMaxWorkingBytes = 512ull * 1024 * 1024;

NskryPluginInfo kInfo{ sizeof(NskryPluginInfo), NSKRY_PLUGIN_API_VERSION,
    L"Long Screenshot", L"nskry-scroll", L"0.6.0", L"Nskry Team",
    L"Vertical and horizontal scrolling capture with live preview", nullptr, NSKRY_CAP_TOOLBAR_ACTION, {} };

HINSTANCE ModuleHandle() noexcept {
    HMODULE module = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&kInfo), &module);
    return module;
}

bool ReadBitmap(HBITMAP bitmap, Frame& frame) {
    BITMAP info{};
    if (!bitmap || !::GetObjectW(bitmap, sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight <= 0) return false;
    size_t count = 0;
    if (!PixelCount(info.bmWidth, info.bmHeight, count)) return false;
    Frame result; result.width = info.bmWidth; result.height = info.bmHeight; result.pixels.resize(count);
    BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = result.width;
    bmi.bmiHeader.biHeight = -result.height; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    HDC dc = ::GetDC(nullptr); if (!dc) return false;
    const int rows = ::GetDIBits(dc, bitmap, 0, result.height, result.pixels.data(), &bmi, DIB_RGB_COLORS); ::ReleaseDC(nullptr, dc);
    if (rows != result.height) return false;
    frame = std::move(result); return true;
}

bool CaptureScreenRegion(const RECT& region, Frame& frame) {
    const int width = region.right - region.left, height = region.bottom - region.top;
    size_t count = 0; if (width < 32 || height < 32 || !PixelCount(width, height, count)) return false;
    HDC screen = ::GetDC(nullptr); if (!screen) return false;
    HDC memory = ::CreateCompatibleDC(screen); if (!memory) { ::ReleaseDC(nullptr, screen); return false; }
    HBITMAP bitmap = ::CreateCompatibleBitmap(screen, width, height);
    if (!bitmap) { ::DeleteDC(memory); ::ReleaseDC(nullptr, screen); return false; }
    HGDIOBJ old = ::SelectObject(memory, bitmap);
    if (!old || old == HGDI_ERROR) { ::DeleteObject(bitmap); ::DeleteDC(memory); ::ReleaseDC(nullptr, screen); return false; }
    const bool copied = ::BitBlt(memory, 0, 0, width, height, screen, region.left, region.top, SRCCOPY | CAPTUREBLT) != FALSE;
    ::SelectObject(memory, old); ::DeleteDC(memory); ::ReleaseDC(nullptr, screen);
    bool read = false;
    try { read = copied && ReadBitmap(bitmap, frame); }
    catch (...) { ::DeleteObject(bitmap); throw; }
    ::DeleteObject(bitmap);
    return read;
}

HBITMAP MakeBitmap(const Frame& frame, ScrollAxis axis) {
    if (!ValidFrame(frame, kMaxStitchedBytes)) return nullptr;
    BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = frame.width;
    bmi.bmiHeader.biHeight = -frame.height; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    if (axis == ScrollAxis::Horizontal) {
        bmi.bmiHeader.biWidth = frame.height;
        bmi.bmiHeader.biHeight = -frame.width;
    }
    void* bits = nullptr; HBITMAP bitmap = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) { if (bitmap) ::DeleteObject(bitmap); return nullptr; }
    if (axis == ScrollAxis::Horizontal)
        TransposePixels(frame, static_cast<unsigned int*>(bits));
    else
        std::memcpy(bits, frame.pixels.data(), frame.pixels.size() * sizeof(unsigned int));
    return bitmap;
}

class ScrollCaptureUi {
public:
    ScrollCaptureUi(const NskryHostContext& context, Frame first, bool chinese, NskryUiTheme theme)
        : m_context(context), m_stitched(std::move(first)), m_chinese(chinese), m_theme(theme) {}
    ~ScrollCaptureUi() { Cleanup(); }

    bool Run() {
        if (!RegisterClasses() || !CreateOverlay() || !CreatePanel())
            return false;
        MSG message{};
        while (!m_finished) {
            const BOOL status = ::GetMessageW(&message, nullptr, 0, 0);
            if (status <= 0) {
                if (status == 0) ::PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            if ((message.hwnd == m_panel || ::IsChild(m_panel, message.hwnd)) &&
                ::IsDialogMessageW(m_panel, &message))
                continue;
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        return true;
    }

private:
    static LRESULT CALLBACK PanelProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<ScrollCaptureUi*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<ScrollCaptureUi*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->m_panel = hwnd; }
        if (!self) return ::DefWindowProcW(hwnd, message, wp, lp);
        try { return self->HandlePanel(message, wp, lp); }
        catch (const std::bad_alloc&) { self->Abort(self->Text(L"Not enough memory to continue capture", L"内存不足，无法继续长截图。")); return 0; }
        catch (...) { self->Abort(self->Text(L"Long screenshot stopped after an unexpected error", L"长截图遇到意外错误。")); return 0; }
    }
    static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<ScrollCaptureUi*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<ScrollCaptureUi*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->m_overlay = hwnd; }
        if (!self || message != WM_PAINT) return ::DefWindowProcW(hwnd, message, wp, lp);
        try { return self->PaintOverlay(); }
        catch (...) { self->Abort(self->Text(L"Long screenshot preview could not be drawn", L"无法绘制长截图预览。")); return 0; }
    }

    static bool RegisterClasses() noexcept {
        static const bool registered = []() noexcept {
        const HINSTANCE module = ModuleHandle(); if (!module) return false;
        WNDCLASSEXW panel{}; panel.cbSize = sizeof(panel); panel.lpfnWndProc = PanelProc; panel.hInstance = module;
        panel.hCursor = ::LoadCursorW(nullptr, IDC_ARROW); panel.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); panel.lpszClassName = L"NskryScrollPanel";
        if (!::RegisterClassExW(&panel) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        WNDCLASSEXW overlay = panel; overlay.lpfnWndProc = OverlayProc; overlay.hbrBackground = nullptr; overlay.lpszClassName = L"NskryScrollOverlay";
        if (!::RegisterClassExW(&overlay) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        return true;
        }();
        return registered;
    }

    bool CreateOverlay() noexcept {
        m_virtual = { ::GetSystemMetrics(SM_XVIRTUALSCREEN), ::GetSystemMetrics(SM_YVIRTUALSCREEN),
            ::GetSystemMetrics(SM_XVIRTUALSCREEN) + ::GetSystemMetrics(SM_CXVIRTUALSCREEN), ::GetSystemMetrics(SM_YVIRTUALSCREEN) + ::GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        if (m_virtual.right <= m_virtual.left || m_virtual.bottom <= m_virtual.top) return false;
        m_overlay = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            L"NskryScrollOverlay", L"", WS_POPUP, m_virtual.left, m_virtual.top, m_virtual.right - m_virtual.left, m_virtual.bottom - m_virtual.top,
            nullptr, nullptr, ModuleHandle(), this);
        if (!m_overlay || !::SetLayeredWindowAttributes(m_overlay, RGB(255, 0, 255), 170, LWA_COLORKEY | LWA_ALPHA)) return false;
        ::ShowWindow(m_overlay, SW_SHOWNOACTIVATE); return true;
    }

    const wchar_t* Text(const wchar_t* en, const wchar_t* zh) const noexcept {
        return m_chinese ? zh : en;
    }

    bool CreatePanel() {
        const RECT region = m_context.capturedRegion;
        const HMONITOR display = ::MonitorFromRect(&region, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor{sizeof(monitor)};
        if (!::GetMonitorInfoW(display, &monitor))
            return false;
        const RECT work = monitor.rcWork;
        UINT dpi = 96, dpiY = 96;
        if (FAILED(::GetDpiForMonitor(display, MDT_EFFECTIVE_DPI, &dpi, &dpiY)))
            dpi = 96;
        const auto dip = [dpi](int n) { return ::MulDiv(n, dpi, 96); };
        const int margin = dip(12);
        const int width = (std::min)(dip(420), static_cast<int>(work.right - work.left) - 2 * margin);
        const int height = (std::min)(dip(510), static_cast<int>(work.bottom - work.top) - 2 * margin);
        if (width < dip(238) || height < dip(232))
            return false;
        int x = region.right + margin;
        const int left = region.left - margin - width;
        if (x + width > work.right - margin)
            x = left >= work.left + margin ? left : work.right - margin - width;
        const int y = std::clamp<int>(region.top, work.top + margin, work.bottom - margin - height);
        const std::wstring title = std::wstring(Text(L"Scrolling capture", L"长截图")) + L" " + kInfo.version;
        m_panel = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"NskryScrollPanel", title.c_str(),
            WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, m_context.mainHwnd, nullptr, ModuleHandle(), this);
        if (!m_panel)
            return false;
        m_panelExcludedFromCapture = ::SetWindowDisplayAffinity(m_panel, WDA_EXCLUDEFROMCAPTURE) != FALSE;

        const auto control = [&](const wchar_t* type, const wchar_t* label, DWORD style, int id) {
            return ::CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                m_panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        };
        const auto button = [&](int id, const wchar_t* label, DWORD style = 0) {
            return control(L"BUTTON", label, WS_TABSTOP | BS_MULTILINE | style, id);
        };
        m_directionLabel = control(L"STATIC", Text(L"Direction", L"方向"), SS_CENTERIMAGE, 0);
        m_direction = control(L"COMBOBOX", nullptr, WS_TABSTOP | CBS_DROPDOWNLIST, 0);
        m_start = button(kStartButton, Text(L"Start", L"开始"), BS_DEFPUSHBUTTON);
        m_autoLeft = button(kAutoLeftButton, Text(L"Auto scroll left", L"自动向左"), WS_DISABLED);
        m_autoButton = button(kAutoButton, Text(L"Auto scroll down", L"自动向下"), WS_DISABLED);
        m_copy = button(kCopyButton, Text(L"Copy", L"复制"), WS_DISABLED);
        m_edit = button(kEditButton, Text(L"Edit", L"编辑"), WS_DISABLED);
        m_close = button(kCancelButton, Text(L"Cancel", L"取消"));
        m_status = control(L"STATIC", Text(L"Choose a direction, then start.", L"选择方向后开始。"),
                           SS_LEFT | SS_NOPREFIX, 0);
        if (!m_directionLabel || !m_direction || !m_start || !m_autoLeft || !m_autoButton ||
            !m_copy || !m_edit || !m_close || !m_status || !LayoutPanel(dpi))
            return false;
        for (const auto axis : {ScrollAxis::Vertical, ScrollAxis::Horizontal}) {
            if (!SupportsAxis(m_stitched.width, m_stitched.height, axis))
                continue;
            const auto item = ::SendMessageW(m_direction, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(axis == ScrollAxis::Vertical ? Text(L"Vertical", L"竖向") : Text(L"Horizontal", L"横向")));
            if (item < 0 || ::SendMessageW(m_direction, CB_SETITEMDATA, item, static_cast<LPARAM>(axis)) == CB_ERR)
                return false;
        }
        ::SendMessageW(m_direction, CB_SETCURSEL, 0, 0);
        ::ShowWindow(m_autoLeft, SW_HIDE);
        ::ShowWindow(m_autoButton, SW_HIDE);
        if (::SendMessageW(m_direction, CB_GETCOUNT, 0, 0) == 1) {
            const auto axis = static_cast<ScrollAxis>(::SendMessageW(m_direction, CB_GETITEMDATA, 0, 0));
            ::SetWindowTextW(m_status, axis == ScrollAxis::Vertical
                ? Text(L"For horizontal capture, select a region at least 96 px wide.", L"横向长截图至少需要 96 像素宽的选区。")
                : Text(L"For vertical capture, select a region at least 96 px tall.", L"竖向长截图至少需要 96 像素高的选区。"));
        }
        ::ShowWindow(m_panel, SW_SHOW);
        ::SetFocus(m_direction);
        ::UpdateWindow(m_panel);
        return true;
    }

    bool LayoutPanel(UINT dpi) {
        const auto dip = [dpi](int n) { return ::MulDiv(n, dpi ? dpi : 96, 96); };
        const int margin = dip(12), gap = dip(8);
        HFONT font = nullptr;
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
            font = ::CreateFontIndirectW(&metrics.lfMessageFont);
        if (!font) font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        HDC dc = ::GetDC(m_panel);
        TEXTMETRICW textMetrics{};
        if (dc) {
            const auto old = ::SelectObject(dc, font);
            ::GetTextMetricsW(dc, &textMetrics);
            ::SelectObject(dc, old);
            ::ReleaseDC(m_panel, dc);
        }
        const int line = (std::max)(dip(16), static_cast<int>(textMetrics.tmHeight));
        const int row = (std::max)(dip(32), line + 2 * gap);
        RECT client{};
        ::GetClientRect(m_panel, &client);
        const int content = client.right - 2 * margin;
        const int footer = client.bottom - margin - row;
        const int autoRow = footer - gap - row;
        const int statusY = margin + row + gap;
        const int previewY = statusY + 3 * line + gap;
        const RECT preview{margin, previewY, client.right - margin, autoRow - gap};
        if (preview.bottom <= preview.top || content <= 4 * gap) {
            if (font != ::GetStockObject(DEFAULT_GUI_FONT)) ::DeleteObject(font);
            return false;
        }
        const int labelWidth = content / 4, startWidth = content / 4;
        const int comboX = margin + labelWidth, startX = margin + content - startWidth;
        const int half = (content - gap) / 2, third = (content - 2 * gap) / 3;
        const auto move = [&](HWND child, int x, int y, int w, int h) {
            ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            ::SetWindowPos(child, nullptr, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
        };
        move(m_directionLabel, margin, margin, labelWidth, row);
        move(m_direction, comboX, margin + dip(3), startX - comboX - gap, dip(140));
        move(m_start, startX, margin, startWidth, row);
        move(m_autoLeft, margin, autoRow, half, row);
        move(m_autoButton, margin + half + gap, autoRow, half, row);
        move(m_copy, margin, footer, third, row);
        move(m_edit, margin + third + gap, footer, third, row);
        move(m_close, margin + 2 * (third + gap), footer, third, row);
        move(m_status, margin, statusY, content, 3 * line);
        if (m_font) ::DeleteObject(m_font);
        m_font = font == ::GetStockObject(DEFAULT_GUI_FONT) ? nullptr : font;
        m_previewPixels.resize(size_t(preview.right - preview.left) * (preview.bottom - preview.top));
        m_preview = preview;
        ::InvalidateRect(m_panel, nullptr, TRUE);
        return true;
    }

    LRESULT HandlePanel(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_DPICHANGED: {
            const auto& bounds = *reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(m_panel, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                           bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutPanel(LOWORD(wp));
            return 0;
        }
        case kFocusSource:
            if (::GetForegroundWindow() == m_panel)
                ::SetForegroundWindow(::GetAncestor(m_context.sourceHwnd, GA_ROOT));
            return 0;
        case DM_GETDEFID:
            return m_timerRunning ? 0 : MAKELONG(kStartButton, DC_HASDEFID);
        case WM_TIMER:
            if (m_timerRunning && wp == kCaptureTimer)
                Tick();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
            case kStartButton: BeginCapture(); return 0;
            case kAutoButton: if (m_timerRunning) ToggleAuto(1); return 0;
            case kAutoLeftButton: if (m_timerRunning && Horizontal()) ToggleAuto(-1); return 0;
            case kCopyButton: if (m_timerRunning) CopyAndFinish(); return 0;
            case kEditButton: if (m_timerRunning) Edit(); return 0;
            case kCancelButton:
            case IDCANCEL: Finish(); return 0;
            }
            break;
        case WM_ERASEBKGND: return nskry::plugin_ui::Background(m_panel, wp, m_theme);
        case WM_CTLCOLORSTATIC: return nskry::plugin_ui::ControlColors(wp, m_theme);
        case WM_PAINT: PaintPanel(); return 0;
        case WM_CLOSE: Finish(); return 0;
        }
        return ::DefWindowProcW(m_panel, message, wp, lp);
    }

    LRESULT PaintOverlay() {
        PAINTSTRUCT paint{}; HDC dc = ::BeginPaint(m_overlay, &paint); if (!dc) return 0;
        RECT client{}; ::GetClientRect(m_overlay, &client);
        HBRUSH dim = ::CreateSolidBrush(RGB(0, 0, 0)); if (dim) { ::FillRect(dc, &client, dim); ::DeleteObject(dim); }
        RECT clear = m_context.capturedRegion; ::OffsetRect(&clear, -m_virtual.left, -m_virtual.top);
        HBRUSH key = ::CreateSolidBrush(RGB(255, 0, 255)); if (key) { ::FillRect(dc, &clear, key); ::DeleteObject(key); }
        HBRUSH border = ::CreateSolidBrush(RGB(0, 174, 255));
        RECT top{ clear.left - 2, clear.top - 2, clear.right + 2, clear.top };
        RECT bottom{ clear.left - 2, clear.bottom, clear.right + 2, clear.bottom + 2 };
        RECT left{ clear.left - 2, clear.top, clear.left, clear.bottom };
        RECT right{ clear.right, clear.top, clear.right + 2, clear.bottom };
        if (border) {
            ::FillRect(dc, &top, border); ::FillRect(dc, &bottom, border); ::FillRect(dc, &left, border); ::FillRect(dc, &right, border);
            ::DeleteObject(border);
        }
        ::EndPaint(m_overlay, &paint); return 0;
    }

    void PaintPanel() {
        PAINTSTRUCT paint{}; HDC dc = ::BeginPaint(m_panel, &paint); if (!dc) return;
        const RECT preview = m_preview;
        const auto colors = nskry::plugin_ui::Colors(m_theme);
        ::FillRect(dc, &preview, nskry::plugin_ui::Brush(dc, colors.panel));
        ::FrameRect(dc, &preview, nskry::plugin_ui::Brush(dc, colors.border));
        if (ValidFrame(m_stitched, kMaxStitchedBytes) && preview.right > preview.left && preview.bottom > preview.top) {
            const int aw = preview.right - preview.left, ah = preview.bottom - preview.top;
            int width = OutputWidth(), height = OutputHeight();
            const double scale = (std::min)(static_cast<double>(aw) / width, static_cast<double>(ah) / height);
            const int dw = (std::max)(1, static_cast<int>(width * scale));
            const int dh = (std::max)(1, static_cast<int>(height * scale));
            if (Horizontal() && size_t(dw) * dh > m_previewPixels.size()) {
                ::EndPaint(m_panel, &paint);
                return;
            }
            const unsigned int *pixels = m_stitched.pixels.data();
            if (Horizontal()) {
                // Sample directly into the small preview instead of allocating
                // a full second copy of a potentially very wide result.
                for (int y = 0; y < dh; ++y) {
                    const size_t sourceY = size_t(y) * height / dh;
                    for (int x = 0; x < dw; ++x) {
                        const size_t sourceX = size_t(x) * width / dw;
                        m_previewPixels[size_t(y) * dw + x] = pixels[sourceX * height + sourceY];
                    }
                }
                pixels = m_previewPixels.data();
                width = dw;
                height = dh;
            }
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = width;
            bmi.bmiHeader.biHeight = -height;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            ::SetStretchBltMode(dc, HALFTONE);
            ::StretchDIBits(dc, preview.left + (aw - dw) / 2, preview.top + (ah - dh) / 2, dw, dh, 0, 0, width, height,
                pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        ::EndPaint(m_panel, &paint);
    }

    bool PanelOverlapsCapture() const { RECT panel{}, intersection{}; return !m_panelExcludedFromCapture && m_panel && ::GetWindowRect(m_panel, &panel) && ::IntersectRect(&intersection, &panel, &m_context.capturedRegion) != FALSE; }

    bool Horizontal() const noexcept { return m_axis == ScrollAxis::Horizontal; }
    int OutputWidth() const noexcept { return Horizontal() ? m_stitched.height : m_stitched.width; }
    int OutputHeight() const noexcept { return Horizontal() ? m_stitched.width : m_stitched.height; }

    bool CaptureFrame(Frame &frame, ScrollAxis axis) {
        const bool hide = PanelOverlapsCapture();
        if (hide) { ::ShowWindow(m_panel, SW_HIDE); ::Sleep(15); }
        const bool captured = CaptureScreenRegion(m_context.capturedRegion, frame);
        if (hide) ::ShowWindow(m_panel, SW_SHOWNOACTIVATE);
        if (captured && axis == ScrollAxis::Horizontal)
            frame = OrientFrame(std::move(frame), axis);
        return captured;
    }

    void BeginCapture() {
        if (m_timerRunning)
            return;
        const auto item = ::SendMessageW(m_direction, CB_GETCURSEL, 0, 0);
        if (item == CB_ERR)
            return;
        const auto axis = static_cast<ScrollAxis>(::SendMessageW(m_direction, CB_GETITEMDATA, item, 0));
        // Horizontal sampling can briefly hold both orientations of the new frame.
        if (axis == ScrollAxis::Horizontal &&
            m_stitched.pixels.size() > kMaxWorkingBytes / (5 * sizeof(unsigned int))) {
            ::SetWindowTextW(m_status, Text(L"Select a smaller region for horizontal capture.", L"请缩小横向长截图的选区。"));
            return;
        }
        if (!::SetTimer(m_panel, kCaptureTimer, kCaptureIntervalMs, nullptr)) {
            ::SetWindowTextW(m_status, Text(L"Could not start capture. Try Start again.", L"无法开始采集，请重试。"));
            return;
        }
        m_timerRunning = true;

        // The user may have scrolled while choosing the axis. Establish the
        // live baseline at Start, keeping the original selection as fallback.
        CaptureFrame(m_last, axis);
        m_stitched = OrientFrame(std::move(m_stitched), axis);
        m_axis = axis;
        if (m_last.pixels.empty())
            m_last = m_stitched;
        m_probe = m_last;

        ::EnableWindow(m_direction, FALSE);
        ::ShowWindow(m_start, SW_HIDE);
        ::EnableWindow(m_copy, TRUE);
        ::EnableWindow(m_edit, TRUE);
        ::EnableWindow(m_autoButton, TRUE);
        ::ShowWindow(m_autoButton, SW_SHOWNOACTIVATE);
        ::SetWindowTextW(m_close, Text(L"Close", L"关闭"));
        if (Horizontal()) {
            ::ShowWindow(m_autoLeft, SW_SHOWNOACTIVATE);
            ::EnableWindow(m_autoLeft, TRUE);
        }
        SetAutoDirection(0);
        ::SetWindowTextW(m_status, Horizontal() ? Text(L"Scroll left or right inside the selection.", L"在选区内向左或向右滚动。") :
                                                 Text(L"Scroll up or down inside the selection.", L"在选区内向上或向下滚动。"));
        ::InvalidateRect(m_panel, nullptr, FALSE);
        ::PostMessageW(m_panel, kFocusSource, 0, 0);
    }

    void SetStatus(const wchar_t* detail) {
        std::wstring status;
        if (m_haveAlignment) {
            status = std::to_wstring(OutputWidth()) + L" × " + std::to_wstring(OutputHeight()) + L" px";
            if (detail && *detail) status += L"\n";
        }
        if (detail) status += detail;
        if (status != m_displayStatus) {
            m_displayStatus = std::move(status);
            ::SetWindowTextW(m_status, m_displayStatus.c_str());
        }
    }

    void UpdateCaptureStatus() {
        SetStatus(m_autoDirection ? Text(L"Auto-scrolling", L"自动滚动中") : nullptr);
        ::InvalidateRect(m_panel, nullptr, FALSE);
    }

    // Commits only a bounded, confidently aligned increment.  This is called
    // while the page is still moving as well as at its final resting point.
    bool CommitFrame(Frame&& next, int maximumAdvance) {
        Alignment alignment = FindAlignment(m_last, next, maximumAdvance, m_haveAlignment ? &m_fixed : nullptr);
        if (!alignment.shift && alignment.issue != Alignment::Issue::LayoutChanged &&
            !SameFrame(m_last, m_probe) && !SameFrame(m_probe, next)) {
            const auto bridge = BridgeAlignment(m_last, m_probe, next, maximumAdvance,
                                                m_haveAlignment ? &m_fixed : nullptr);
            const auto trial = PlaceFrame(m_position, bridge.shift, next.height, m_stitched.height);
            if (bridge.shift && (trial.prepend || trial.append))
                alignment = bridge;
        }
        const int advance = alignment.shift;
        m_alignmentHint = m_haveAlignment ? Text(L"Could not align · scroll back to the last captured position", L"这一步没有拼上。请回到上次成功的位置。")
                                         : Text(L"First step did not match. Scroll back and try a smaller step.", L"首步没有拼上。请往回滚，再缩小滚动幅度。");
        if (!advance) {
            if (alignment.issue == Alignment::Issue::LayoutChanged)
                m_alignmentHint = Text(L"Layout changed · reselect the scrolling pane", L"布局已改变，请重新框选滚动区域。");
            if (alignment.issue == Alignment::Issue::MixedMotion)
                m_alignmentHint = Text(L"Fixed background · select a smaller scrolling region", L"检测到固定背景，请缩小选区。");
            return false;
        }
        if (m_haveAlignment && alignment.fixed != m_fixed) {
            m_alignmentHint = Text(L"Scrolling pane changed · start a new capture", L"滚动区域已改变，请重新开始长截图。");
            return false;
        }
        const Placement placement = PlaceFrame(m_position, advance, next.height, m_stitched.height);
        // Keep the user's selection bitmap available for Copy/Edit until
        // the live baseline has actually produced a verified extension.
        const auto &mosaic = m_haveAlignment ? m_stitched : m_last;
        const auto join = FindJoin(mosaic, m_last, next, placement, alignment);
        if (join.row < 0 && join.path.empty()) {
            m_alignmentHint = Text(L"No clean join · avoid fixed overlays or scroll back", L"这一步没有拼上。请避开悬浮元素或往回滚一点。");
            return false;
        }
        const size_t width = next.width - alignment.fixed.left - alignment.fixed.right;
        const size_t newPixels = (size_t(m_stitched.height) + placement.prepend + placement.append) * width;
        const size_t maxPixels = kMaxStitchedBytes / sizeof(unsigned int);
        if (newPixels > maxPixels) {
            SetAutoDirection(0);
            m_alignmentHint = Text(L"Maximum capture size reached", L"已达到尺寸上限，仍可复制或编辑当前结果。"); return false;
        }
        if (newPixels > m_stitched.pixels.capacity()) {
            constexpr size_t growthPixels = (16ull * 1024 * 1024) / sizeof(unsigned int);
            const size_t targetCapacity = (std::min)(maxPixels, (std::max)(newPixels, m_stitched.pixels.capacity() + growthPixels));
            const size_t residentBytes = (m_stitched.pixels.capacity() + targetCapacity + m_last.pixels.capacity() +
                m_probe.pixels.capacity() + next.pixels.capacity()) * sizeof(unsigned int);
            const size_t samplingBytes = (targetCapacity + 4 * next.pixels.size()) * sizeof(unsigned int);
            if (residentBytes > kMaxWorkingBytes || (Horizontal() && samplingBytes > kMaxWorkingBytes)) {
                SetAutoDirection(0);
                m_alignmentHint = Text(L"Capture stopped at the memory safety limit", L"已达到内存上限，仍可复制或编辑当前结果。"); return false;
            }
            m_stitched.pixels.reserve(targetCapacity);
        }
        if (!m_haveAlignment)
            std::copy(m_last.pixels.begin(), m_last.pixels.end(), m_stitched.pixels.begin());
        Extend(m_stitched, next, placement, alignment.fixed, join.row, join.path);
        m_fixed = alignment.fixed;
        m_haveAlignment = true;
        m_position = placement.top + placement.prepend;
        m_last = std::move(next); m_probe = m_last;
        m_noProgressCount = 0; m_autoStepAdvanced = true; UpdateCaptureStatus();
        return true;
    }

    void Tick() {
        if (m_autoDirection && !m_waitingForSettle && ::GetTickCount64() - m_lastAutoScroll >= kAutoScrollIntervalMs) {
            const auto& region = m_context.capturedRegion;
            const FixedBands screen = Horizontal() ? FixedBands{m_fixed.left, m_fixed.right, m_fixed.top, m_fixed.bottom}
                                                   : m_fixed;
            POINT center{ (region.left + screen.left + region.right - screen.right) / 2,
                          (region.top + screen.top + region.bottom - screen.bottom) / 2 };
            HWND target = ::WindowFromPoint(center);
            const HWND sourceRoot = ::GetAncestor(m_context.sourceHwnd, GA_ROOT);
            if (!target || !sourceRoot || ::GetAncestor(target, GA_ROOT) != sourceRoot)
                target = m_context.sourceHwnd;
            // A half-notch is deliberately small enough to avoid overshoot;
            // the shorter interval keeps automatic capture responsive.
            const UINT message = Horizontal() ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL;
            const int delta = (Horizontal() ? m_autoDirection : -m_autoDirection) * kAutoWheelDelta;
            ::PostMessageW(target, message, MAKEWPARAM(0, static_cast<WORD>(delta)), MAKELPARAM(center.x, center.y));
            m_waitingForSettle = true; m_autoStepAdvanced = false; m_stableSamples = 0; return;
        }
        Frame next;
        if (!CaptureFrame(next, m_axis))
            return;
        const int advanceLimit = (std::max)(24, m_last.height * 70 / 100);
        if (!SameFrame(m_probe, next)) {
            m_rejectedAtRest = false;
            // Do not defer all work until scrolling stops.  Small deltas are
            // appended as they arrive. A live step may cover up to 70% of the
            // viewport, which handles ordinary wheel and trackpad gestures;
            // movements beyond the searchable overlap remain safely rejected.
            if (!CommitFrame(std::move(next), advanceLimit)) {
                m_probe = std::move(next);
                SetStatus(m_alignmentHint);
            }
            m_stableSamples = 0;
            return;
        }
        if (m_rejectedAtRest && !m_waitingForSettle) return;
        if (++m_stableSamples < 2) return;
        m_stableSamples = 0;
        if (SameFrame(m_last, next)) {
            m_probe = std::move(next);
            if (m_waitingForSettle) {
                m_waitingForSettle = false; m_lastAutoScroll = ::GetTickCount64();
                if (!m_autoStepAdvanced && ++m_noProgressCount >= 2) {
                    SetAutoDirection(0);
                    SetStatus(Text(L"No further scroll in this direction.", L"该方向没有新增内容。可手动滚动或导出。"));
                }
            }
            return;
        }
        // The resting frame is always checked too, so the last short movement
        // is not lost even if it happened just before the user released input.
        if (!CommitFrame(std::move(next), advanceLimit)) {
            m_probe = std::move(next); m_waitingForSettle = false; m_rejectedAtRest = true;
            SetAutoDirection(0);
            SetStatus(m_alignmentHint);
            return;
        }
        m_waitingForSettle = false; m_lastAutoScroll = ::GetTickCount64(); m_noProgressCount = 0;
    }

    void SetAutoDirection(int direction) noexcept {
        m_autoDirection = direction;
        if (m_autoButton)
            ::SetWindowTextW(m_autoButton, direction > 0 ? Text(L"Pause", L"暂停") : (Horizontal() ? Text(L"Auto scroll right", L"自动向右") : Text(L"Auto scroll down", L"自动向下")));
        if (m_autoLeft)
            ::SetWindowTextW(m_autoLeft, direction < 0 ? Text(L"Pause", L"暂停") : Text(L"Auto scroll left", L"自动向左"));
    }

    void ToggleAuto(int direction) {
        SetAutoDirection(m_autoDirection == direction ? 0 : direction);
        m_waitingForSettle = false;
        m_lastAutoScroll = m_autoDirection ? 0 : ::GetTickCount64();
        m_noProgressCount = 0;
        SetStatus(m_autoDirection ? Text(L"Auto scrolling…", L"自动滚动中…") : Text(L"Paused · manual scrolling enabled", L"已暂停自动滚动，仍可手动滚动。"));
        ::PostMessageW(m_panel, kFocusSource, 0, 0);
    }

    void CopyAndFinish() {
        if (m_finished) return;
        if (!m_context.copyBitmapToClipboard) { SetStatus(Text(L"Clipboard service is unavailable", L"剪贴板暂不可用。")); return; }
        HBITMAP bitmap = MakeBitmap(m_stitched, m_axis);
        if (!bitmap) { SetStatus(Text(L"Could not create the final bitmap", L"无法生成结果图片。")); return; }
        int32_t copied = 0;
        try { copied = m_context.copyBitmapToClipboard(bitmap); }
        catch (...) { ::DeleteObject(bitmap); throw; }
        ::DeleteObject(bitmap);
        if (!copied) { SetStatus(Text(L"Could not copy the image to the clipboard", L"无法复制到剪贴板，请重试。")); return; }
        Finish();
    }

    void Finish() noexcept {
        if (m_finished) return;
        if (m_timerRunning && m_panel) { ::KillTimer(m_panel, kCaptureTimer); m_timerRunning = false; }
        m_finished = true; if (m_panel) ::PostMessageW(m_panel, WM_NULL, 0, 0);
    }

    void Edit() {
        if (m_finished) return;
        if (!m_context.openBitmapEditor) { SetStatus(Text(L"Bitmap editor is unavailable", L"图片编辑器暂不可用。")); return; }
        HBITMAP bitmap = MakeBitmap(m_stitched, m_axis); if (!bitmap) { SetStatus(Text(L"Could not create the final bitmap", L"无法生成结果图片。")); return; }
        try { m_context.openBitmapEditor(bitmap, OutputWidth(), OutputHeight()); }
        catch (...) { ::DeleteObject(bitmap); throw; }
        Finish();
    }

    void Abort(const wchar_t* message) noexcept {
        SetAutoDirection(0);
        if (m_status && message) ::SetWindowTextW(m_status, message);
        Finish();
    }

    void Cleanup() noexcept {
        if (m_timerRunning && m_panel) { ::KillTimer(m_panel, kCaptureTimer); m_timerRunning = false; }
        if (m_panel && ::IsWindow(m_panel)) ::DestroyWindow(m_panel);
        m_panel = nullptr; m_autoButton = nullptr; m_status = nullptr;
        if (m_font) { ::DeleteObject(m_font); m_font = nullptr; }
        if (m_overlay && ::IsWindow(m_overlay)) ::DestroyWindow(m_overlay);
        m_overlay = nullptr;
    }

    NskryHostContext m_context{};
    Frame m_last, m_probe, m_stitched;
    std::vector<unsigned int> m_previewPixels;
    RECT m_virtual{};
    const bool m_chinese;
    const NskryUiTheme m_theme;
    RECT m_preview{};
    HFONT m_font{};
    HWND m_close{}, m_directionLabel{};
    HWND m_panel{}, m_overlay{}, m_direction{}, m_start{}, m_autoLeft{}, m_autoButton{}, m_copy{}, m_edit{}, m_status{};
    ScrollAxis m_axis = ScrollAxis::Vertical;
    int m_autoDirection = 0;
    bool m_finished = false, m_waitingForSettle = false, m_autoStepAdvanced = false, m_timerRunning = false;
    bool m_panelExcludedFromCapture = false;
    FixedBands m_fixed{};
    const wchar_t* m_alignmentHint = L"";
    std::wstring m_displayStatus;
    bool m_haveAlignment = false;
    bool m_rejectedAtRest = false;
    int m_position = 0; // Viewport origin on the stitch axis, rebased after prepending.
    int m_stableSamples = 0, m_noProgressCount = 0; ULONGLONG m_lastAutoScroll{};
};
} // namespace

extern "C" NSKRY_API const NskryPluginInfo* NSKRY_CALL nskry_plugin_info() { return &kInfo; }
extern "C" NSKRY_API int32_t NSKRY_CALL nskry_plugin_init(const NskryHostContext* context) {
    return context && context->structSize >= sizeof(NskryHostContext) &&
        context->apiVersion == NSKRY_PLUGIN_API_VERSION ? 1 : 0;
}
extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_shutdown() {}
extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_set_ui_preferences_v1(const NskryUiPreferencesV1* ui) {
    nskry::plugin_ui::SetPreferences(ui);
}

extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_execute(const NskryHostContext* context) {
    const auto ui = nskry::plugin_ui::preferences;
    const LANGID language = ui.language ? ui.language : ::GetUserDefaultUILanguage();
    const bool chinese = PRIMARYLANGID(language) == LANG_CHINESE;
    const auto text = [chinese](const wchar_t* en, const wchar_t* zh) { return chinese ? zh : en; };
    const auto title = text(L"Scrolling capture", L"长截图");
    try {
        if (!context || context->structSize < sizeof(NskryHostContext) ||
            context->apiVersion != NSKRY_PLUGIN_API_VERSION || !context->sourceHwnd ||
            !::IsWindow(context->sourceHwnd) || !context->capturedBitmap) {
            ::MessageBoxW(nullptr, text(L"Select a visible scrollable region first.", L"请先框选一个可滚动区域。"), title, MB_ICONWARNING); return;
        }
        const int regionWidth = context->capturedRegion.right - context->capturedRegion.left;
        const int regionHeight = context->capturedRegion.bottom - context->capturedRegion.top;
        if (!SupportsAxis(regionWidth, regionHeight, ScrollAxis::Vertical) &&
            !SupportsAxis(regionWidth, regionHeight, ScrollAxis::Horizontal)) {
            ::MessageBoxW(context->mainHwnd, text(L"Select at least 64 × 96 pixels for vertical capture or 96 × 64 for horizontal capture.", L"竖向长截图至少需要 64 × 96 像素，横向至少需要 96 × 64 像素。"),
                title, MB_ICONWARNING); return;
        }
        Frame first;
        if (!ReadBitmap(context->capturedBitmap, first) || first.width != regionWidth || first.height != regionHeight ||
            first.pixels.size() * sizeof(unsigned int) * 3 > kMaxWorkingBytes) {
            ::MessageBoxW(context->mainHwnd, text(L"The selected region could not be prepared safely.", L"无法准备所选区域，请缩小选区后重试。"), title, MB_ICONERROR); return;
        }
        ScrollCaptureUi capture(*context, std::move(first), chinese, ui.theme);
        if (!capture.Run()) ::MessageBoxW(context->mainHwnd, text(L"Long screenshot controls could not be initialized.", L"无法打开长截图面板。"), title, MB_ICONERROR);
    } catch (const std::bad_alloc&) {
        ::MessageBoxW(context ? context->mainHwnd : nullptr, text(L"Not enough memory to start long screenshot capture.", L"内存不足，无法开始长截图。"), title, MB_ICONERROR);
    } catch (...) {
        ::MessageBoxW(context ? context->mainHwnd : nullptr, text(L"Long screenshot stopped after an unexpected error.", L"长截图遇到意外错误。"), title, MB_ICONERROR);
    }
}
