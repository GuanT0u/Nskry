#include "nskry_plugin.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr UINT_PTR kCaptureTimer = 1;
constexpr UINT kCaptureIntervalMs = 320;
constexpr UINT kAutoScrollIntervalMs = 950;
constexpr int kAutoButton = 101, kCopyButton = 102, kCancelButton = 103;

struct Frame { int width{}, height{}; std::vector<unsigned int> pixels; };
NskryPluginInfo kInfo{ L"Long Screenshot", L"nskry-scroll", L"0.4.3", L"Nskry Team",
    L"Interactive scrolling capture with live preview", nullptr, NSKRY_CAP_TOOLBAR_ACTION };

bool ReadBitmap(HBITMAP bitmap, Frame& frame) {
    BITMAP info{};
    if (!bitmap || !::GetObjectW(bitmap, sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight <= 0) return false;
    frame.width = info.bmWidth; frame.height = info.bmHeight;
    frame.pixels.resize(static_cast<size_t>(frame.width) * frame.height);
    BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = frame.width;
    bmi.bmiHeader.biHeight = -frame.height; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    HDC dc = ::GetDC(nullptr); const int rows = ::GetDIBits(dc, bitmap, 0, frame.height, frame.pixels.data(), &bmi, DIB_RGB_COLORS); ::ReleaseDC(nullptr, dc);
    return rows == frame.height;
}

bool CaptureScreenRegion(const RECT& region, Frame& frame) {
    const int width = region.right - region.left, height = region.bottom - region.top;
    if (width < 32 || height < 32) return false;
    HDC screen = ::GetDC(nullptr), memory = ::CreateCompatibleDC(screen);
    HBITMAP bitmap = ::CreateCompatibleBitmap(screen, width, height); HGDIOBJ old = ::SelectObject(memory, bitmap);
    const bool copied = ::BitBlt(memory, 0, 0, width, height, screen, region.left, region.top, SRCCOPY | CAPTUREBLT) != FALSE;
    ::SelectObject(memory, old); ::DeleteDC(memory); ::ReleaseDC(nullptr, screen);
    const bool read = copied && ReadBitmap(bitmap, frame); ::DeleteObject(bitmap); return read;
}

int Channel(unsigned int pixel, int shift) { return static_cast<int>((pixel >> shift) & 255); }
int Luma(unsigned int pixel) { return (Channel(pixel, 16) * 3 + Channel(pixel, 8) * 6 + Channel(pixel, 0)) / 10; }

int FrameDifference(const Frame& left, const Frame& right) {
    if (left.width != right.width || left.height != right.height) return 255;
    unsigned long long total = 0; int samples = 0;
    for (int y = 8; y < left.height - 8; y += 24) for (int x = 8; x < left.width - 8; x += 24) {
        const unsigned int a = left.pixels[static_cast<size_t>(y) * left.width + x];
        const unsigned int b = right.pixels[static_cast<size_t>(y) * right.width + x];
        total += abs(Channel(a, 16) - Channel(b, 16)) + abs(Channel(a, 8) - Channel(b, 8)) + abs(Channel(a, 0) - Channel(b, 0)); ++samples;
    }
    return samples ? static_cast<int>(total / static_cast<unsigned long long>(samples * 3)) : 255;
}

int FindAdvance(const Frame& previous, const Frame& current) {
    if (previous.width != current.width || previous.height != current.height) return 0;
    const int minShift = 6, maxShift = previous.height - 24; if (maxShift <= minShift) return 0;
    unsigned long long bestCost = ULLONG_MAX; int bestShift = 0, bestSamples = 0;
    const int topMargin = (std::max)(24, previous.height / 8);
    const int bottomMargin = (std::max)(16, previous.height / 12);
    for (int shift = minShift; shift <= maxShift; ++shift) {
        unsigned long long cost = 0; int samples = 0;
        for (int y = topMargin; y < previous.height - shift - bottomMargin; y += 7) for (int x = 12; x < previous.width - 12; x += 11) {
            const unsigned int a = previous.pixels[static_cast<size_t>(y + shift) * previous.width + x];
            const unsigned int b = current.pixels[static_cast<size_t>(y) * current.width + x];
            const int edgeA = abs(Luma(a) - Luma(previous.pixels[static_cast<size_t>(y + shift) * previous.width + x - 2])) + abs(Luma(a) - Luma(previous.pixels[static_cast<size_t>(y + shift - 2) * previous.width + x]));
            const int edgeB = abs(Luma(b) - Luma(current.pixels[static_cast<size_t>(y) * current.width + x - 2])) + abs(Luma(b) - Luma(current.pixels[static_cast<size_t>(y - 2) * current.width + x]));
            if ((std::max)(edgeA, edgeB) < 24) continue;
            cost += abs(static_cast<int>(a & 255) - static_cast<int>(b & 255));
            cost += abs(static_cast<int>((a >> 8) & 255) - static_cast<int>((b >> 8) & 255));
            cost += abs(static_cast<int>((a >> 16) & 255) - static_cast<int>((b >> 16) & 255)); ++samples;
        }
        if (samples && cost < bestCost) { bestCost = cost; bestShift = shift; bestSamples = samples; }
    }
    return bestShift && bestSamples >= 40 && bestCost / static_cast<unsigned long long>(bestSamples * 3) < 12 ? bestShift : 0;
}

HBITMAP MakeBitmap(const Frame& frame) {
    BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = frame.width;
    bmi.bmiHeader.biHeight = -frame.height; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr; HBITMAP bitmap = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) std::memcpy(bits, frame.pixels.data(), frame.pixels.size() * sizeof(unsigned int)); return bitmap;
}

class ScrollCaptureUi {
public:
    ScrollCaptureUi(const NskryHostContext& context, Frame first) : m_context(context), m_last(std::move(first)), m_probe(m_last), m_stitched(m_last) {}

    void Run() {
        RegisterClasses(); CreateOverlay(); CreatePanel(); m_lastAutoScroll = ::GetTickCount64();
        ::SetTimer(m_panel, kCaptureTimer, kCaptureIntervalMs, nullptr);
        MSG message{};
        while (!m_finished) {
            const BOOL status = ::GetMessageW(&message, nullptr, 0, 0);
            if (status <= 0) {
                if (status == 0) ::PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        if (m_panel) ::DestroyWindow(m_panel); if (m_overlay) ::DestroyWindow(m_overlay);
    }

private:
    static LRESULT CALLBACK PanelProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<ScrollCaptureUi*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<ScrollCaptureUi*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->m_panel = hwnd; }
        return self ? self->HandlePanel(message, wp, lp) : ::DefWindowProcW(hwnd, message, wp, lp);
    }
    static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<ScrollCaptureUi*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<ScrollCaptureUi*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->m_overlay = hwnd; }
        if (self && message == WM_PAINT) return self->PaintOverlay(); return ::DefWindowProcW(hwnd, message, wp, lp);
    }

    void RegisterClasses() {
        static bool registered = false; if (registered) return;
        const HINSTANCE module = ::GetModuleHandleW(L"nskry-scroll.dll");
        WNDCLASSEXW panel{}; panel.cbSize = sizeof(panel); panel.lpfnWndProc = PanelProc; panel.hInstance = module;
        panel.hCursor = ::LoadCursorW(nullptr, IDC_ARROW); panel.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); panel.lpszClassName = L"NskryScrollPanel"; ::RegisterClassExW(&panel);
        WNDCLASSEXW overlay = panel; overlay.lpfnWndProc = OverlayProc; overlay.hbrBackground = nullptr; overlay.lpszClassName = L"NskryScrollOverlay"; ::RegisterClassExW(&overlay); registered = true;
    }

    void CreateOverlay() {
        m_virtual = { ::GetSystemMetrics(SM_XVIRTUALSCREEN), ::GetSystemMetrics(SM_YVIRTUALSCREEN),
            ::GetSystemMetrics(SM_XVIRTUALSCREEN) + ::GetSystemMetrics(SM_CXVIRTUALSCREEN), ::GetSystemMetrics(SM_YVIRTUALSCREEN) + ::GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        m_overlay = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            L"NskryScrollOverlay", L"", WS_POPUP, m_virtual.left, m_virtual.top, m_virtual.right - m_virtual.left, m_virtual.bottom - m_virtual.top,
            nullptr, nullptr, ::GetModuleHandleW(L"nskry-scroll.dll"), this);
        ::SetLayeredWindowAttributes(m_overlay, RGB(255, 0, 255), 170, LWA_COLORKEY | LWA_ALPHA); ::ShowWindow(m_overlay, SW_SHOWNOACTIVATE);
    }

    void CreatePanel() {
        constexpr int width = 300;
        const int screenHeight = static_cast<int>(m_virtual.bottom - m_virtual.top);
        const int height = (std::min)(560, (std::max)(360, screenHeight - 40));
        int x = m_context.capturedRegion.right + 12, y = (std::max)(m_virtual.top + 12, m_context.capturedRegion.top);
        if (x + width > m_virtual.right - 12) x = m_context.capturedRegion.left - width - 12;
        if (x < m_virtual.left + 12) x = (std::max)(m_virtual.left + 12, m_context.capturedRegion.right - width - 12);
        if (y + height > m_virtual.bottom - 12) y = m_virtual.bottom - height - 12;
        m_panel = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"NskryScrollPanel", L"Long Screenshot",
            WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, m_context.mainHwnd, nullptr, ::GetModuleHandleW(L"nskry-scroll.dll"), this);
        RECT client{}; ::GetClientRect(m_panel, &client); const int controlsY = client.bottom - 42;
        m_autoButton = ::CreateWindowExW(0, L"BUTTON", L"Auto scroll", WS_CHILD | WS_VISIBLE, 10, controlsY, 105, 30, m_panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAutoButton)), nullptr, nullptr);
        ::CreateWindowExW(0, L"BUTTON", L"Copy", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 121, controlsY, 72, 30, m_panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCopyButton)), nullptr, nullptr);
        ::CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 199, controlsY, 72, 30, m_panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButton)), nullptr, nullptr);
        m_status = ::CreateWindowExW(0, L"STATIC", L"Scroll manually, or start auto scroll", WS_CHILD | WS_VISIBLE | SS_CENTER, 8, controlsY - 30, client.right - 16, 22, m_panel, nullptr, nullptr, nullptr);
        ::ShowWindow(m_panel, SW_SHOW); ::UpdateWindow(m_panel);
    }

    LRESULT HandlePanel(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_TIMER: Tick(); return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kAutoButton) ToggleAuto(); else if (LOWORD(wp) == kCopyButton) Finish(true); else if (LOWORD(wp) == kCancelButton) Finish(false); return 0;
        case WM_PAINT: PaintPanel(); return 0;
        case WM_CLOSE: Finish(false); return 0;
        }
        return ::DefWindowProcW(m_panel, message, wp, lp);
    }

    LRESULT PaintOverlay() {
        PAINTSTRUCT paint{}; HDC dc = ::BeginPaint(m_overlay, &paint); RECT client{}; ::GetClientRect(m_overlay, &client);
        HBRUSH dim = ::CreateSolidBrush(RGB(0, 0, 0)); ::FillRect(dc, &client, dim); ::DeleteObject(dim);
        RECT clear = m_context.capturedRegion; ::OffsetRect(&clear, -m_virtual.left, -m_virtual.top);
        HBRUSH key = ::CreateSolidBrush(RGB(255, 0, 255)); ::FillRect(dc, &clear, key); ::DeleteObject(key);
        HBRUSH border = ::CreateSolidBrush(RGB(0, 174, 255));
        RECT top{ clear.left - 2, clear.top - 2, clear.right + 2, clear.top };
        RECT bottom{ clear.left - 2, clear.bottom, clear.right + 2, clear.bottom + 2 };
        RECT left{ clear.left - 2, clear.top, clear.left, clear.bottom };
        RECT right{ clear.right, clear.top, clear.right + 2, clear.bottom };
        ::FillRect(dc, &top, border); ::FillRect(dc, &bottom, border); ::FillRect(dc, &left, border); ::FillRect(dc, &right, border);
        ::DeleteObject(border); ::EndPaint(m_overlay, &paint); return 0;
    }

    void PaintPanel() {
        PAINTSTRUCT paint{}; HDC dc = ::BeginPaint(m_panel, &paint); RECT client{}; ::GetClientRect(m_panel, &client); RECT preview{ 10, 10, client.right - 10, client.bottom - 82 };
        HBRUSH background = ::CreateSolidBrush(RGB(25, 26, 34)); ::FillRect(dc, &preview, background); ::DeleteObject(background);
        if (!m_stitched.pixels.empty()) {
            const int aw = preview.right - preview.left, ah = preview.bottom - preview.top;
            const double scale = (std::min)(static_cast<double>(aw) / m_stitched.width, static_cast<double>(ah) / m_stitched.height);
            const int dw = (std::max)(1, static_cast<int>(m_stitched.width * scale)), dh = (std::max)(1, static_cast<int>(m_stitched.height * scale));
            BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = m_stitched.width; bmi.bmiHeader.biHeight = -m_stitched.height; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
            ::SetStretchBltMode(dc, HALFTONE); ::StretchDIBits(dc, preview.left + (aw - dw) / 2, preview.top, dw, dh, 0, 0, m_stitched.width, m_stitched.height, m_stitched.pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        ::EndPaint(m_panel, &paint);
    }

    bool PanelOverlapsCapture() const { RECT panel{}, intersection{}; ::GetWindowRect(m_panel, &panel); return ::IntersectRect(&intersection, &panel, &m_context.capturedRegion) != FALSE; }

    void Tick() {
        if (m_auto && !m_waitingForSettle && ::GetTickCount64() - m_lastAutoScroll >= kAutoScrollIntervalMs) {
            POINT center{ (m_context.capturedRegion.left + m_context.capturedRegion.right) / 2, (m_context.capturedRegion.top + m_context.capturedRegion.bottom) / 2 };
            HWND target = ::WindowFromPoint(center); if (!target || target == m_panel || target == m_overlay) target = m_context.sourceHwnd;
            ::PostMessageW(target, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA * 2)), MAKELPARAM(center.x, center.y));
            m_waitingForSettle = true; m_stableSamples = 0; return;
        }
        const bool hide = PanelOverlapsCapture(); if (hide) { ::ShowWindow(m_panel, SW_HIDE); ::Sleep(15); }
        Frame next; const bool captured = CaptureScreenRegion(m_context.capturedRegion, next); if (hide) ::ShowWindow(m_panel, SW_SHOWNOACTIVATE); if (!captured) return;
        if (FrameDifference(m_probe, next) > 2) {
            m_probe = std::move(next); m_stableSamples = 0;
            ::SetWindowTextW(m_status, m_auto ? L"Scrolling… waiting for page to settle" : L"Scrolling manually…");
            return;
        }
        if (++m_stableSamples < 2) return;
        m_stableSamples = 0;
        if (FrameDifference(m_last, next) <= 2) {
            m_probe = std::move(next);
            if (m_waitingForSettle) {
                m_waitingForSettle = false; m_lastAutoScroll = ::GetTickCount64();
                if (++m_noProgressCount >= 2) { m_auto = false; ::SetWindowTextW(m_autoButton, L"Auto scroll"); ::SetWindowTextW(m_status, L"Reached the end of the page"); }
            }
            return;
        }
        const int advance = FindAdvance(m_last, next);
        if (!advance) {
            m_probe = std::move(next); m_waitingForSettle = false; m_auto = false;
            ::SetWindowTextW(m_autoButton, L"Auto scroll"); ::SetWindowTextW(m_status, L"Could not align this step · scroll a shorter distance");
            return;
        }
        if (m_stitched.pixels.size() + static_cast<size_t>(advance) * next.width > 100000000) { m_auto = false; ::SetWindowTextW(m_autoButton, L"Auto scroll"); ::SetWindowTextW(m_status, L"Maximum capture size reached"); return; }
        m_stitched.pixels.insert(m_stitched.pixels.end(), next.pixels.end() - static_cast<size_t>(advance) * next.width, next.pixels.end());
        m_stitched.height += advance; m_last = std::move(next); m_probe = m_last; ++m_frames;
        m_waitingForSettle = false; m_lastAutoScroll = ::GetTickCount64(); m_noProgressCount = 0;
        const std::wstring status = std::to_wstring(m_stitched.width) + L" × " + std::to_wstring(m_stitched.height) + L" · " + std::to_wstring(m_frames) + L" frames";
        ::SetWindowTextW(m_status, status.c_str()); ::InvalidateRect(m_panel, nullptr, FALSE);
    }

    void ToggleAuto() { m_auto = !m_auto; m_waitingForSettle = false; m_lastAutoScroll = m_auto ? 0 : ::GetTickCount64(); ::SetWindowTextW(m_autoButton, m_auto ? L"Pause" : L"Auto scroll"); ::SetWindowTextW(m_status, m_auto ? L"Auto scrolling slowly…" : L"Paused · manual scrolling enabled"); }
    void Finish(bool copy) {
        if (m_finished) return; ::KillTimer(m_panel, kCaptureTimer);
        if (copy) { HBITMAP bitmap = MakeBitmap(m_stitched); if (bitmap && m_context.copyBitmapToClipboard) { m_context.copyBitmapToClipboard(bitmap); ::DeleteObject(bitmap); } }
        m_finished = true; ::PostMessageW(m_panel, WM_NULL, 0, 0);
    }

    NskryHostContext m_context{}; Frame m_last, m_probe, m_stitched; RECT m_virtual{}; HWND m_panel{}, m_overlay{}, m_autoButton{}, m_status{};
    bool m_auto = false, m_finished = false, m_waitingForSettle = false;
    int m_frames = 1, m_stableSamples = 0, m_noProgressCount = 0; ULONGLONG m_lastAutoScroll{};
};
} // namespace

extern "C" NSKRY_API const NskryPluginInfo* nskry_plugin_info() { return &kInfo; }
extern "C" NSKRY_API bool nskry_plugin_init(const NskryHostContext*) { return true; }
extern "C" NSKRY_API void nskry_plugin_shutdown() {}
extern "C" NSKRY_API void nskry_plugin_execute(const NskryHostContext* context) {
    if (!context || !context->sourceHwnd || !context->capturedBitmap) { ::MessageBoxW(nullptr, L"Select a visible scrollable region first.", L"Long Screenshot", MB_ICONWARNING); return; }
    Frame first; if (!ReadBitmap(context->capturedBitmap, first)) return; ScrollCaptureUi ui(*context, std::move(first)); ui.Run();
}
