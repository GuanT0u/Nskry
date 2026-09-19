#include "nskry_plugin.h"

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
constexpr size_t kMaxFrameBytes = 128ull * 1024 * 1024;
constexpr size_t kMaxStitchedBytes = 192ull * 1024 * 1024;
constexpr size_t kMaxWorkingBytes = 512ull * 1024 * 1024;

struct Frame { int width{}, height{}; std::vector<unsigned int> pixels; };
NskryPluginInfo kInfo{ sizeof(NskryPluginInfo), NSKRY_PLUGIN_API_VERSION,
    L"Long Screenshot", L"nskry-scroll", L"0.5.0", L"Nskry Team",
    L"Interactive scrolling capture with live preview", nullptr, NSKRY_CAP_TOOLBAR_ACTION, {} };

bool PixelCount(int width, int height, size_t& count, size_t maximumBytes = kMaxFrameBytes) noexcept {
    if (width <= 0 || height <= 0) return false;
    const size_t w = static_cast<size_t>(width), h = static_cast<size_t>(height);
    if (w > (std::numeric_limits<size_t>::max)() / h) return false;
    count = w * h;
    return count <= maximumBytes / sizeof(unsigned int);
}

bool ValidFrame(const Frame& frame, size_t maximumBytes = kMaxFrameBytes) noexcept {
    size_t count = 0;
    return PixelCount(frame.width, frame.height, count, maximumBytes) && frame.pixels.size() == count;
}

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
    const bool read = copied && ReadBitmap(bitmap, frame); ::DeleteObject(bitmap); return read;
}

int Channel(unsigned int pixel, int shift) { return static_cast<int>((pixel >> shift) & 255); }
int Luma(unsigned int pixel) { return (Channel(pixel, 16) * 3 + Channel(pixel, 8) * 6 + Channel(pixel, 0)) / 10; }

int FrameDifference(const Frame& left, const Frame& right) {
    if (!ValidFrame(left) || !ValidFrame(right) || left.width != right.width || left.height != right.height) return 255;
    unsigned long long total = 0; int samples = 0;
    for (int y = 8; y < left.height - 8; y += 24) for (int x = 8; x < left.width - 8; x += 24) {
        const unsigned int a = left.pixels[static_cast<size_t>(y) * left.width + x];
        const unsigned int b = right.pixels[static_cast<size_t>(y) * right.width + x];
        total += abs(Channel(a, 16) - Channel(b, 16)) + abs(Channel(a, 8) - Channel(b, 8)) + abs(Channel(a, 0) - Channel(b, 0)); ++samples;
    }
    return samples ? static_cast<int>(total / static_cast<unsigned long long>(samples * 3)) : 255;
}

struct AlignmentScore { int shift{}; double error{ (std::numeric_limits<double>::infinity)() }; int samples{}; };

AlignmentScore ScoreShift(const Frame& previous, const Frame& current, int shift, int xStep, int yStep) noexcept {
    AlignmentScore result; result.shift = shift;
    const int topMargin = (std::max)(8, (std::min)(24, previous.height / 8));
    const int bottomMargin = (std::max)(8, (std::min)(16, previous.height / 12));
    const int yEnd = previous.height - shift - bottomMargin;
    if (yEnd <= topMargin || previous.width <= 24) return result;
    unsigned long long cost = 0; int samples = 0, possible = 0;
    for (int y = topMargin; y < yEnd; y += yStep) for (int x = 12; x < previous.width - 12; x += xStep) {
        ++possible;
        const size_t oldIndex = static_cast<size_t>(y + shift) * previous.width + x;
        const size_t newIndex = static_cast<size_t>(y) * current.width + x;
        const unsigned int a = previous.pixels[oldIndex], b = current.pixels[newIndex];
        const int edgeA = std::abs(Luma(a) - Luma(previous.pixels[oldIndex - 2])) +
            std::abs(Luma(a) - Luma(previous.pixels[oldIndex - static_cast<size_t>(previous.width) * 2]));
        const int edgeB = std::abs(Luma(b) - Luma(current.pixels[newIndex - 2])) +
            std::abs(Luma(b) - Luma(current.pixels[newIndex - static_cast<size_t>(current.width) * 2]));
        if ((std::max)(edgeA, edgeB) < 20) continue;
        cost += static_cast<unsigned long long>(std::abs(Channel(a, 0) - Channel(b, 0)));
        cost += static_cast<unsigned long long>(std::abs(Channel(a, 8) - Channel(b, 8)));
        cost += static_cast<unsigned long long>(std::abs(Channel(a, 16) - Channel(b, 16)));
        ++samples;
    }
    const int minimumSamples = (std::max)(12, possible / 40);
    if (samples >= minimumSamples)
        result.error = static_cast<double>(cost) / static_cast<double>(samples * 3);
    result.samples = samples; return result;
}

int FindAdvance(const Frame& previous, const Frame& current, int maximumAdvance) {
    if (!ValidFrame(previous) || !ValidFrame(current) || previous.width != current.width || previous.height != current.height) return 0;
    constexpr int minShift = 4;
    // Keep a meaningful overlap for confidence scoring, but allow a normal
    // trackpad gesture to move most of the viewport between samples. Texture-
    // poor or repetitive pages are still rejected by the score/ambiguity test.
    const int minimumOverlap = (std::max)(48, previous.height * 30 / 100);
    const int maxShift = (std::min)(maximumAdvance, previous.height - minimumOverlap);
    if (maxShift < minShift) return 0;

    const int coarseStep = maxShift > 240 ? 8 : (maxShift > 80 ? 4 : 2);
    const int coarseX = previous.width > 1600 ? 32 : (previous.width > 700 ? 24 : 12);
    const int coarseY = previous.height > 1200 ? 24 : (previous.height > 500 ? 16 : 8);
    std::vector<AlignmentScore> coarse;
    coarse.reserve(static_cast<size_t>((maxShift - minShift) / coarseStep + 2));
    for (int shift = minShift; shift <= maxShift; shift += coarseStep)
        coarse.push_back(ScoreShift(previous, current, shift, coarseX, coarseY));
    if (coarse.empty() || coarse.back().shift != maxShift)
        coarse.push_back(ScoreShift(previous, current, maxShift, coarseX, coarseY));
    std::sort(coarse.begin(), coarse.end(), [](const AlignmentScore& a, const AlignmentScore& b) { return a.error < b.error; });

    std::vector<int> candidates;
    const size_t seedCount = (std::min<size_t>)(4, coarse.size());
    for (size_t i = 0; i < seedCount && std::isfinite(coarse[i].error); ++i) {
        const int first = (std::max)(minShift, coarse[i].shift - coarseStep);
        const int last = (std::min)(maxShift, coarse[i].shift + coarseStep);
        for (int shift = first; shift <= last; ++shift)
            if (std::find(candidates.begin(), candidates.end(), shift) == candidates.end()) candidates.push_back(shift);
    }
    if (candidates.empty()) return 0;

    std::vector<AlignmentScore> fine;
    fine.reserve(candidates.size());
    const int fineX = previous.width > 2400 ? 16 : 12;
    const int fineY = previous.height > 1600 ? 10 : 7;
    for (const int shift : candidates) fine.push_back(ScoreShift(previous, current, shift, fineX, fineY));
    std::sort(fine.begin(), fine.end(), [](const AlignmentScore& a, const AlignmentScore& b) { return a.error < b.error; });
    if (fine.empty() || !std::isfinite(fine.front().error) || fine.front().error >= 12.0) return 0;

    const AlignmentScore& best = fine.front();
    double secondError = (std::numeric_limits<double>::infinity)();
    for (const AlignmentScore& score : fine) {
        if (std::abs(score.shift - best.shift) >= 4 && std::isfinite(score.error)) { secondError = score.error; break; }
    }
    // Repetitive pages can produce several equally plausible offsets. Reject
    // those rather than appending at an arbitrary position.
    if (std::isfinite(secondError) && secondError - best.error < 0.8 && best.error > secondError * 0.92) return 0;
    return best.shift;
}

HBITMAP MakeBitmap(const Frame& frame) {
    if (!ValidFrame(frame, kMaxStitchedBytes)) return nullptr;
    BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = frame.width;
    bmi.bmiHeader.biHeight = -frame.height; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr; HBITMAP bitmap = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) { if (bitmap) ::DeleteObject(bitmap); return nullptr; }
    std::memcpy(bits, frame.pixels.data(), frame.pixels.size() * sizeof(unsigned int)); return bitmap;
}

class ScrollCaptureUi {
public:
    ScrollCaptureUi(const NskryHostContext& context, Frame first) : m_context(context), m_last(std::move(first)), m_probe(m_last), m_stitched(m_last) {}

    bool Run() {
        if (!RegisterClasses() || !CreateOverlay() || !CreatePanel()) { Cleanup(); return false; }
        m_lastAutoScroll = ::GetTickCount64();
        if (!::SetTimer(m_panel, kCaptureTimer, kCaptureIntervalMs, nullptr)) { Cleanup(); return false; }
        m_timerRunning = true;
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
        Cleanup(); return true;
    }

private:
    static LRESULT CALLBACK PanelProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<ScrollCaptureUi*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<ScrollCaptureUi*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->m_panel = hwnd; }
        if (!self) return ::DefWindowProcW(hwnd, message, wp, lp);
        try { return self->HandlePanel(message, wp, lp); }
        catch (const std::bad_alloc&) { self->Abort(L"Not enough memory to continue capture"); return 0; }
        catch (...) { self->Abort(L"Long screenshot stopped after an unexpected error"); return 0; }
    }
    static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) noexcept {
        auto* self = reinterpret_cast<ScrollCaptureUi*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<ScrollCaptureUi*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->m_overlay = hwnd; }
        if (!self || message != WM_PAINT) return ::DefWindowProcW(hwnd, message, wp, lp);
        try { return self->PaintOverlay(); }
        catch (...) { self->Abort(L"Long screenshot preview could not be drawn"); return 0; }
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

    bool CreatePanel() noexcept {
        constexpr int width = 364;
        const int screenHeight = static_cast<int>(m_virtual.bottom - m_virtual.top);
        const int height = (std::min)(560, (std::max)(360, screenHeight - 40));
        int x = m_context.capturedRegion.right + 12, y = (std::max)(m_virtual.top + 12, m_context.capturedRegion.top);
        if (x + width > m_virtual.right - 12) x = m_context.capturedRegion.left - width - 12;
        if (x < m_virtual.left + 12) x = (std::max)(m_virtual.left + 12, m_context.capturedRegion.right - width - 12);
        if (y + height > m_virtual.bottom - 12) y = m_virtual.bottom - height - 12;
        m_panel = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"NskryScrollPanel", L"Long Screenshot",
            WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, m_context.mainHwnd, nullptr, ModuleHandle(), this);
        if (!m_panel) return false;
        // Keep an in-range preview visible without baking it into BitBlt/WGC
        // captures. Older systems/drivers can reject the affinity; Tick then
        // falls back to briefly hiding only while the pixels are sampled.
        m_panelExcludedFromCapture = ::SetWindowDisplayAffinity(m_panel, WDA_EXCLUDEFROMCAPTURE) != FALSE;
        RECT client{}; if (!::GetClientRect(m_panel, &client) || client.right < 340 || client.bottom < 120) return false;
        const int controlsY = client.bottom - 42;
        m_autoButton = ::CreateWindowExW(0, L"BUTTON", L"Auto scroll", WS_CHILD | WS_VISIBLE, 10, controlsY, 105, 30, m_panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAutoButton)), nullptr, nullptr);
        HWND copyButton = ::CreateWindowExW(0, L"BUTTON", L"Copy", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 121, controlsY, 64, 30, m_panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCopyButton)), nullptr, nullptr);
        HWND editButton = ::CreateWindowExW(0, L"BUTTON", L"Edit", WS_CHILD | WS_VISIBLE, 191, controlsY, 64, 30, m_panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditButton)), nullptr, nullptr);
        HWND cancelButton = ::CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 261, controlsY, 76, 30, m_panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButton)), nullptr, nullptr);
        m_status = ::CreateWindowExW(0, L"STATIC", L"Scroll manually, or start auto scroll", WS_CHILD | WS_VISIBLE | SS_CENTER, 8, controlsY - 30, client.right - 16, 22, m_panel, nullptr, nullptr, nullptr);
        if (!m_autoButton || !copyButton || !editButton || !cancelButton || !m_status) return false;
        ::ShowWindow(m_panel, SW_SHOW); ::UpdateWindow(m_panel); return true;
    }

    LRESULT HandlePanel(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_TIMER: Tick(); return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kAutoButton) ToggleAuto(); else if (LOWORD(wp) == kCopyButton) CopyAndFinish(); else if (LOWORD(wp) == kEditButton) Edit(); else if (LOWORD(wp) == kCancelButton) Finish(); return 0;
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
        RECT client{}; ::GetClientRect(m_panel, &client); RECT preview{ 10, 10, client.right - 10, client.bottom - 82 };
        HBRUSH background = ::CreateSolidBrush(RGB(25, 26, 34)); if (background) { ::FillRect(dc, &preview, background); ::DeleteObject(background); }
        if (ValidFrame(m_stitched, kMaxStitchedBytes) && preview.right > preview.left && preview.bottom > preview.top) {
            const int aw = preview.right - preview.left, ah = preview.bottom - preview.top;
            const double scale = (std::min)(static_cast<double>(aw) / m_stitched.width, static_cast<double>(ah) / m_stitched.height);
            const int dw = (std::max)(1, static_cast<int>(m_stitched.width * scale)), dh = (std::max)(1, static_cast<int>(m_stitched.height * scale));
            BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = m_stitched.width; bmi.bmiHeader.biHeight = -m_stitched.height; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
            ::SetStretchBltMode(dc, HALFTONE); ::StretchDIBits(dc, preview.left + (aw - dw) / 2, preview.top, dw, dh, 0, 0, m_stitched.width, m_stitched.height, m_stitched.pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        ::EndPaint(m_panel, &paint);
    }

    bool PanelOverlapsCapture() const { RECT panel{}, intersection{}; return !m_panelExcludedFromCapture && m_panel && ::GetWindowRect(m_panel, &panel) && ::IntersectRect(&intersection, &panel, &m_context.capturedRegion) != FALSE; }

    void UpdateCaptureStatus() {
        const std::wstring status = std::to_wstring(m_stitched.width) + L" × " + std::to_wstring(m_stitched.height) + L" · " + std::to_wstring(m_frames) + L" frames";
        ::SetWindowTextW(m_status, status.c_str()); ::InvalidateRect(m_panel, nullptr, FALSE);
    }

    // Commits only a bounded, confidently aligned increment.  This is called
    // while the page is still moving as well as at its final resting point.
    bool AppendFrame(Frame&& next, int maximumAdvance) {
        const int advance = FindAdvance(m_last, next, maximumAdvance);
        if (!advance) return false;
        const size_t addedPixels = static_cast<size_t>(advance) * static_cast<size_t>(next.width);
        if (addedPixels > next.pixels.size() || m_stitched.pixels.size() > (std::numeric_limits<size_t>::max)() - addedPixels) return false;
        const size_t newPixels = m_stitched.pixels.size() + addedPixels;
        const size_t maxPixels = kMaxStitchedBytes / sizeof(unsigned int);
        if (newPixels > maxPixels) {
            m_auto = false; ::SetWindowTextW(m_autoButton, L"Auto scroll");
            ::SetWindowTextW(m_status, L"Maximum capture size reached"); return false;
        }
        if (newPixels > m_stitched.pixels.capacity()) {
            constexpr size_t growthPixels = (16ull * 1024 * 1024) / sizeof(unsigned int);
            const size_t targetCapacity = (std::min)(maxPixels, (std::max)(newPixels, m_stitched.pixels.capacity() + growthPixels));
            const size_t residentBytes = (m_stitched.pixels.capacity() + targetCapacity + m_last.pixels.capacity() +
                m_probe.pixels.capacity() + next.pixels.capacity()) * sizeof(unsigned int);
            if (residentBytes > kMaxWorkingBytes) {
                m_auto = false; ::SetWindowTextW(m_autoButton, L"Auto scroll");
                ::SetWindowTextW(m_status, L"Capture stopped at the memory safety limit"); return false;
            }
            m_stitched.pixels.reserve(targetCapacity);
        }
        m_stitched.pixels.insert(m_stitched.pixels.end(), next.pixels.end() - addedPixels, next.pixels.end());
        m_stitched.height += advance; m_last = std::move(next); m_probe = m_last; ++m_frames;
        m_noProgressCount = 0; m_autoStepAdvanced = true; UpdateCaptureStatus();
        return true;
    }

    void Tick() {
        if (m_auto && !m_waitingForSettle && ::GetTickCount64() - m_lastAutoScroll >= kAutoScrollIntervalMs) {
            POINT center{ (m_context.capturedRegion.left + m_context.capturedRegion.right) / 2, (m_context.capturedRegion.top + m_context.capturedRegion.bottom) / 2 };
            HWND target = ::WindowFromPoint(center); if (!target || target == m_panel || target == m_overlay) target = m_context.sourceHwnd;
            // A half-notch is deliberately small enough to avoid overshoot;
            // the shorter interval keeps automatic capture responsive.
            ::PostMessageW(target, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-kAutoWheelDelta)), MAKELPARAM(center.x, center.y));
            m_waitingForSettle = true; m_autoStepAdvanced = false; m_stableSamples = 0; return;
        }
        const bool hide = PanelOverlapsCapture(); if (hide) { ::ShowWindow(m_panel, SW_HIDE); ::Sleep(15); }
        Frame next; const bool captured = CaptureScreenRegion(m_context.capturedRegion, next); if (hide) ::ShowWindow(m_panel, SW_SHOWNOACTIVATE); if (!captured) return;
        if (FrameDifference(m_probe, next) > 2) {
            // Do not defer all work until scrolling stops.  Small deltas are
            // appended as they arrive. A live step may cover up to 70% of the
            // viewport, which handles ordinary wheel and trackpad gestures;
            // movements beyond the searchable overlap remain safely rejected.
            const int incrementalLimit = (std::max)(24, m_last.height * 70 / 100);
            if (!AppendFrame(std::move(next), incrementalLimit)) {
                m_probe = std::move(next);
                ::SetWindowTextW(m_status, m_auto ? L"Scrolling… collecting small increments" : L"Scrolling manually… collecting increments");
            }
            m_stableSamples = 0;
            return;
        }
        if (++m_stableSamples < 2) return;
        m_stableSamples = 0;
        if (FrameDifference(m_last, next) <= 2) {
            m_probe = std::move(next);
            if (m_waitingForSettle) {
                m_waitingForSettle = false; m_lastAutoScroll = ::GetTickCount64();
                if (!m_autoStepAdvanced && ++m_noProgressCount >= 2) { m_auto = false; ::SetWindowTextW(m_autoButton, L"Auto scroll"); ::SetWindowTextW(m_status, L"Reached the end of the page"); }
            }
            return;
        }
        // The resting frame is always checked too, so the last short movement
        // is not lost even if it happened just before the user released input.
        const int restingLimit = (std::max)(24, m_last.height * 70 / 100);
        if (!AppendFrame(std::move(next), restingLimit)) {
            m_probe = std::move(next); m_waitingForSettle = false; m_auto = false;
            ::SetWindowTextW(m_autoButton, L"Auto scroll"); ::SetWindowTextW(m_status, L"Could not align this step · scroll a shorter distance");
            return;
        }
        m_waitingForSettle = false; m_lastAutoScroll = ::GetTickCount64(); m_noProgressCount = 0;
    }

    void ToggleAuto() { m_auto = !m_auto; m_waitingForSettle = false; m_lastAutoScroll = m_auto ? 0 : ::GetTickCount64(); ::SetWindowTextW(m_autoButton, m_auto ? L"Pause" : L"Auto scroll"); ::SetWindowTextW(m_status, m_auto ? L"Auto scrolling slowly…" : L"Paused · manual scrolling enabled"); }

    void CopyAndFinish() {
        if (m_finished) return;
        if (!m_context.copyBitmapToClipboard) { ::SetWindowTextW(m_status, L"Clipboard service is unavailable"); return; }
        HBITMAP bitmap = MakeBitmap(m_stitched);
        if (!bitmap) { ::SetWindowTextW(m_status, L"Could not create the final bitmap"); return; }
        int32_t copied = 0;
        try { copied = m_context.copyBitmapToClipboard(bitmap); }
        catch (...) { ::DeleteObject(bitmap); throw; }
        ::DeleteObject(bitmap);
        if (!copied) { ::SetWindowTextW(m_status, L"Could not copy the image to the clipboard"); return; }
        Finish();
    }

    void Finish() noexcept {
        if (m_finished) return;
        if (m_timerRunning && m_panel) { ::KillTimer(m_panel, kCaptureTimer); m_timerRunning = false; }
        m_finished = true; if (m_panel) ::PostMessageW(m_panel, WM_NULL, 0, 0);
    }

    void Edit() {
        if (m_finished) return;
        if (!m_context.openBitmapEditor) { ::SetWindowTextW(m_status, L"Bitmap editor is unavailable"); return; }
        HBITMAP bitmap = MakeBitmap(m_stitched); if (!bitmap) { ::SetWindowTextW(m_status, L"Could not create the final bitmap"); return; }
        try { m_context.openBitmapEditor(bitmap, m_stitched.width, m_stitched.height); }
        catch (...) { ::DeleteObject(bitmap); throw; }
        Finish();
    }

    void Abort(const wchar_t* message) noexcept {
        m_auto = false;
        if (m_autoButton) ::SetWindowTextW(m_autoButton, L"Auto scroll");
        if (m_status && message) ::SetWindowTextW(m_status, message);
        Finish();
    }

    void Cleanup() noexcept {
        if (m_timerRunning && m_panel) { ::KillTimer(m_panel, kCaptureTimer); m_timerRunning = false; }
        if (m_panel && ::IsWindow(m_panel)) ::DestroyWindow(m_panel);
        m_panel = nullptr; m_autoButton = nullptr; m_status = nullptr;
        if (m_overlay && ::IsWindow(m_overlay)) ::DestroyWindow(m_overlay);
        m_overlay = nullptr;
    }

    NskryHostContext m_context{}; Frame m_last, m_probe, m_stitched; RECT m_virtual{}; HWND m_panel{}, m_overlay{}, m_autoButton{}, m_status{};
    bool m_auto = false, m_finished = false, m_waitingForSettle = false, m_autoStepAdvanced = false, m_timerRunning = false;
    bool m_panelExcludedFromCapture = false;
    int m_frames = 1, m_stableSamples = 0, m_noProgressCount = 0; ULONGLONG m_lastAutoScroll{};
};
} // namespace

extern "C" NSKRY_API const NskryPluginInfo* NSKRY_CALL nskry_plugin_info() { return &kInfo; }
extern "C" NSKRY_API int32_t NSKRY_CALL nskry_plugin_init(const NskryHostContext* context) {
    return context && context->structSize >= sizeof(NskryHostContext) &&
        context->apiVersion == NSKRY_PLUGIN_API_VERSION ? 1 : 0;
}
extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_shutdown() {}
extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_execute(const NskryHostContext* context) {
    try {
        if (!context || context->structSize < sizeof(NskryHostContext) ||
            context->apiVersion != NSKRY_PLUGIN_API_VERSION || !context->sourceHwnd ||
            !::IsWindow(context->sourceHwnd) || !context->capturedBitmap) {
            ::MessageBoxW(nullptr, L"Select a visible scrollable region first.", L"Long Screenshot", MB_ICONWARNING); return;
        }
        const int regionWidth = context->capturedRegion.right - context->capturedRegion.left;
        const int regionHeight = context->capturedRegion.bottom - context->capturedRegion.top;
        if (regionWidth < 64 || regionHeight < 96) {
            ::MessageBoxW(context->mainHwnd, L"Select a region at least 64 × 96 pixels for reliable scrolling alignment.", L"Long Screenshot", MB_ICONWARNING); return;
        }
        Frame first;
        if (!ReadBitmap(context->capturedBitmap, first) || first.width != regionWidth || first.height != regionHeight ||
            first.pixels.size() * sizeof(unsigned int) * 3 > kMaxWorkingBytes) {
            ::MessageBoxW(context->mainHwnd, L"The selected region could not be prepared safely.", L"Long Screenshot", MB_ICONERROR); return;
        }
        ScrollCaptureUi ui(*context, std::move(first));
        if (!ui.Run()) ::MessageBoxW(context->mainHwnd, L"Long screenshot controls could not be initialized.", L"Long Screenshot", MB_ICONERROR);
    } catch (const std::bad_alloc&) {
        ::MessageBoxW(context ? context->mainHwnd : nullptr, L"Not enough memory to start long screenshot capture.", L"Long Screenshot", MB_ICONERROR);
    } catch (...) {
        ::MessageBoxW(context ? context->mainHwnd : nullptr, L"Long screenshot stopped after an unexpected error.", L"Long Screenshot", MB_ICONERROR);
    }
}
