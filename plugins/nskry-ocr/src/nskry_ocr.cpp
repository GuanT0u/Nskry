#include "nskry_plugin.h"

#include <windowsx.h>
#include <commdlg.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kResultMessage = WM_APP + 0x4B1;
constexpr int kTextModeId = 1001;
constexpr int kVisualModeId = 1002;
constexpr int kCopyAllId = 1003;
constexpr int kZoomOutId = 1004;
constexpr int kZoomInId = 1005;
constexpr int kHighlightId = 1006;
constexpr int kEditId = 1007;
constexpr int kSaveId = 1008;
constexpr int kRotateId = 1009;
constexpr int kMaxPreviewDimension = 3200;

bool StartsWithIgnoreCase(const std::wstring& value, const wchar_t* prefix) {
    const size_t length = wcslen(prefix);
    return value.size() >= length && ::CompareStringOrdinal(value.c_str(), static_cast<int>(length),
        prefix, static_cast<int>(length), TRUE) == CSTR_EQUAL;
}

winrt::Windows::Media::Ocr::OcrEngine CreatePreferredOcrEngine() {
    // Windows' Simplified/Traditional Chinese OCR engines also recognise the
    // Latin characters commonly mixed into Chinese UI and documents. Prefer one
    // when installed; otherwise retain Windows' profile-language selection.
    for (const auto& language : winrt::Windows::Media::Ocr::OcrEngine::AvailableRecognizerLanguages()) {
        const std::wstring tag = language.LanguageTag().c_str();
        if (!StartsWithIgnoreCase(tag, L"zh")) continue;
        if (const auto engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromLanguage(language)) return engine;
    }
    return winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
}

struct WordBox {
    RECT rect{};
    std::wstring text;
    size_t line{};
};

struct OcrJobResult {
    HBITMAP bitmap{};
    int width{};
    int height{};
    RECT anchor{};
    std::wstring text;
    std::wstring error;
    std::vector<WordBox> words;
    int32_t (*copyBitmapToClipboard)(HBITMAP){};
    void (*openBitmapEditor)(HBITMAP, int, int){};

    ~OcrJobResult() {
        if (bitmap) ::DeleteObject(bitmap);
    }
};

NskryPluginInfo kInfo{ sizeof(NskryPluginInfo), NSKRY_PLUGIN_API_VERSION,
    L"Text recognition", L"nskry-ocr", L"0.5.0", L"Nskry Team",
    L"Offline text recognition using Windows OCR", nullptr,
    NSKRY_CAP_TOOLBAR_ACTION | NSKRY_CAP_POST_CAPTURE, {} };

HWND s_dispatchWindow{};
std::atomic_bool s_running{};
std::mutex s_workerMutex;
std::thread s_worker;
std::mutex s_pendingMutex;
std::vector<std::unique_ptr<OcrJobResult>> s_pendingResults;

bool GetBitmapSize(HBITMAP bitmap, int& width, int& height) {
    BITMAP info{};
    if (!bitmap || ::GetObjectW(bitmap, sizeof(info), &info) != sizeof(info) ||
        info.bmWidth <= 0 || info.bmHeight == 0) return false;
    width = info.bmWidth;
    height = std::abs(info.bmHeight);
    return height > 0;
}

HBITMAP CopyBitmap(HBITMAP bitmap) {
    return bitmap ? static_cast<HBITMAP>(::CopyImage(bitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION)) : nullptr;
}

HBITMAP ResizeForOcr(HBITMAP source, int width, int height, int& outputWidth, int& outputHeight) {
    outputWidth = width;
    outputHeight = height;
    if (!source || width <= 0 || height <= 0) return nullptr;

    uint32_t engineMaximum = 0;
    try { engineMaximum = winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension(); }
    catch (...) { engineMaximum = kMaxPreviewDimension; }
    const int maximum = (std::max)(1, (std::min)(kMaxPreviewDimension, static_cast<int>(engineMaximum)));
    const int largestDimension = (std::max)(width, height);
    double scale = 1.0;
    if (largestDimension > maximum) scale = static_cast<double>(maximum) / largestDimension;
    // Small UI text benefits considerably from a larger raster. Keep the
    // temporary recognition image bounded, and never expose it to the result
    // window (which continues to own and display the original-size bitmap).
    else scale = (std::min)(4.0, static_cast<double>(maximum) / largestDimension);
    if (std::abs(scale - 1.0) < 0.01) return CopyBitmap(source);

    outputWidth = (std::max)(1, static_cast<int>(std::lround(width * scale)));
    outputHeight = (std::max)(1, static_cast<int>(std::lround(height * scale)));

    HDC screen = ::GetDC(nullptr);
    HDC src = screen ? ::CreateCompatibleDC(screen) : nullptr;
    HDC dst = screen ? ::CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = outputWidth;
    bmi.bmiHeader.biHeight = -outputHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP scaled = dst ? ::CreateDIBSection(dst, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    HGDIOBJ oldSrc = src && source ? ::SelectObject(src, source) : nullptr;
    HGDIOBJ oldDst = dst && scaled ? ::SelectObject(dst, scaled) : nullptr;
    const bool copied = oldSrc && oldSrc != HGDI_ERROR && oldDst && oldDst != HGDI_ERROR &&
        (::SetStretchBltMode(dst, HALFTONE), ::StretchBlt(dst, 0, 0, outputWidth, outputHeight,
                                                          src, 0, 0, width, height, SRCCOPY) != FALSE);
    if (oldSrc && oldSrc != HGDI_ERROR) ::SelectObject(src, oldSrc);
    if (oldDst && oldDst != HGDI_ERROR) ::SelectObject(dst, oldDst);
    if (src) ::DeleteDC(src);
    if (dst) ::DeleteDC(dst);
    if (screen) ::ReleaseDC(nullptr, screen);
    if (!copied) { if (scaled) ::DeleteObject(scaled); return nullptr; }
    return scaled;
}

HBITMAP RotateClockwise(HBITMAP source, int width, int height) {
    HDC screen = ::GetDC(nullptr);
    HDC input = screen ? ::CreateCompatibleDC(screen) : nullptr;
    HDC output = screen ? ::CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = height;
    info.bmiHeader.biHeight = -width;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* ignored{};
    HBITMAP rotated = output ? ::CreateDIBSection(output, &info, DIB_RGB_COLORS, &ignored, nullptr, 0) : nullptr;
    HGDIOBJ oldInput = input ? ::SelectObject(input, source) : nullptr;
    HGDIOBJ oldOutput = output && rotated ? ::SelectObject(output, rotated) : nullptr;
    POINT corners[3]{ { height, 0 }, { height, width }, { 0, 0 } };
    const bool copied = oldInput && oldInput != HGDI_ERROR && oldOutput && oldOutput != HGDI_ERROR &&
        ::PlgBlt(output, corners, input, 0, 0, width, height, nullptr, 0, 0);
    if (oldInput && oldInput != HGDI_ERROR) ::SelectObject(input, oldInput);
    if (oldOutput && oldOutput != HGDI_ERROR) ::SelectObject(output, oldOutput);
    if (input) ::DeleteDC(input); if (output) ::DeleteDC(output); if (screen) ::ReleaseDC(nullptr, screen);
    if (!copied && rotated) { ::DeleteObject(rotated); rotated = nullptr; }
    return rotated;
}

winrt::Windows::Graphics::Imaging::SoftwareBitmap ToSoftwareBitmap(HBITMAP bitmap) {
    int width{}, height{};
    if (!GetBitmapSize(bitmap, width, height)) throw winrt::hresult_invalid_argument();

    const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (byteCount == 0 || byteCount > UINT32_MAX) throw winrt::hresult_error(E_OUTOFMEMORY);

    // Feed Windows OCR a raw BGRA buffer.  The previous WIC -> PNG -> COM stream
    // round-trip crossed two independent stream wrappers and corrupted the process
    // heap on some Windows builds while the asynchronous decoder was consuming it.
    // A SoftwareBitmap owns this copied buffer and has no stream lifetime to outlive.
    winrt::Windows::Storage::Streams::Buffer pixels(static_cast<uint32_t>(byteCount));
    pixels.Length(static_cast<uint32_t>(byteCount));
    uint8_t* destination{};
    winrt::check_hresult(pixels.as<winrt::impl::IBufferByteAccess>()->Buffer(&destination));
    if (!destination) throw winrt::hresult_error(E_OUTOFMEMORY);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top-down, matching OCR coordinates
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = ::GetDC(nullptr);
    if (!screen) throw winrt::hresult_error(HRESULT_FROM_WIN32(::GetLastError()));
    const int rows = ::GetDIBits(screen, bitmap, 0, static_cast<UINT>(height), destination,
                                  &info, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, screen);
    if (rows != height) throw winrt::hresult_error(HRESULT_FROM_WIN32(::GetLastError()));

    auto converted = winrt::Windows::Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(
        pixels, winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
        width, height, winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Ignore);
    return converted;
}

std::unique_ptr<OcrJobResult> RunOcr(HBITMAP ocrBitmap, int ocrWidth, int ocrHeight,
                                     HBITMAP displayBitmap, int displayWidth, int displayHeight, RECT anchor,
                                     int32_t (*copyBitmap)(HBITMAP) = nullptr,
                                     void (*openEditor)(HBITMAP, int, int) = nullptr) {
    struct OcrBitmapGuard { HBITMAP value{}; ~OcrBitmapGuard() { if (value) ::DeleteObject(value); } } guard{ ocrBitmap };
    auto result = std::make_unique<OcrJobResult>();
    result->bitmap = displayBitmap;
    result->width = displayWidth;
    result->height = displayHeight;
    result->anchor = anchor;
    result->copyBitmapToClipboard = copyBitmap;
    result->openBitmapEditor = openEditor;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const auto engine = CreatePreferredOcrEngine();
        if (!engine) {
            result->error = L"Windows OCR has no language pack for your profile. Install an OCR language in Windows Settings, then try again.";
            return result;
        }
        const auto recognized = engine.RecognizeAsync(ToSoftwareBitmap(ocrBitmap)).get();
        const double coordinateScaleX = static_cast<double>(displayWidth) / ocrWidth;
        const double coordinateScaleY = static_cast<double>(displayHeight) / ocrHeight;
        // Use Windows OCR's line results rather than its flattened document text,
        // so the text-only view remains visually close to the selected content.
        size_t lineIndex = 0;
        for (const auto& line : recognized.Lines()) {
            if (!result->text.empty()) result->text += L"\r\n";
            result->text += line.Text().c_str();
            for (const auto& word : line.Words()) {
                const auto bounds = word.BoundingRect();
                WordBox box;
                box.rect.left = static_cast<LONG>(std::floor(bounds.X * coordinateScaleX));
                box.rect.top = static_cast<LONG>(std::floor(bounds.Y * coordinateScaleY));
                box.rect.right = static_cast<LONG>(std::ceil((bounds.X + bounds.Width) * coordinateScaleX));
                box.rect.bottom = static_cast<LONG>(std::ceil((bounds.Y + bounds.Height) * coordinateScaleY));
                box.rect.left = (std::clamp)(box.rect.left, 0L, static_cast<LONG>(displayWidth));
                box.rect.top = (std::clamp)(box.rect.top, 0L, static_cast<LONG>(displayHeight));
                box.rect.right = (std::clamp)(box.rect.right, box.rect.left, static_cast<LONG>(displayWidth));
                box.rect.bottom = (std::clamp)(box.rect.bottom, box.rect.top, static_cast<LONG>(displayHeight));
                box.text = word.Text().c_str();
                box.line = lineIndex;
                if (!box.text.empty() && box.rect.right > box.rect.left && box.rect.bottom > box.rect.top)
                    result->words.push_back(std::move(box));
            }
            ++lineIndex;
        }
        if (result->text.empty()) result->text = L"No text was found in this image.";
    } catch (const winrt::hresult_error& error) {
        result->error = L"Windows OCR could not recognize this image: " + std::wstring(error.message().c_str());
    } catch (const std::exception& error) {
        std::wstring message;
        const int needed = ::MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, nullptr, 0);
        if (needed > 1) {
            message.resize(needed);
            ::MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, message.data(), needed);
            message.pop_back();
        }
        result->error = L"Windows OCR could not recognize this image" + (message.empty() ? L"." : L": " + message);
    } catch (...) {
        result->error = L"Windows OCR could not recognize this image.";
    }
    return result;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    void* destination = ::GlobalLock(memory);
    if (!destination) { ::GlobalFree(memory); return false; }
    std::memcpy(destination, text.c_str(), bytes);
    ::GlobalUnlock(memory);
    if (!::OpenClipboard(owner)) { ::GlobalFree(memory); return false; }
    const bool copied = ::EmptyClipboard() && ::SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    ::CloseClipboard();
    if (!copied) ::GlobalFree(memory);
    return copied;
}

bool SaveBitmapAsPng(HWND owner, HBITMAP bitmap) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = owner; dialog.lpstrFile = path;
    dialog.nMaxFile = ARRAYSIZE(path); dialog.lpstrFilter = L"PNG image (*.png)\0*.png\0";
    dialog.lpstrDefExt = L"png"; dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!::GetSaveFileNameW(&dialog)) return false;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICBitmap> source;
    if (FAILED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source))) return false;
    ComPtr<IWICStream> stream; if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> encoder; if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame; ComPtr<IPropertyBag2> options;
    return SUCCEEDED(encoder->CreateNewFrame(&frame, &options)) && SUCCEEDED(frame->Initialize(options.Get())) &&
        SUCCEEDED(frame->WriteSource(source.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

class OcrResultWindow {
public:
    explicit OcrResultWindow(std::unique_ptr<OcrJobResult> result) : m_result(std::move(result)) {}
    ~OcrResultWindow() {
        if (m_font) ::DeleteObject(m_font);
        if (m_boldFont) ::DeleteObject(m_boldFont);
    }

    bool Show() {
        RegisterClass();
        const RECT work = WorkArea();
        const int monitorWidth = work.right - work.left;
        const int monitorHeight = work.bottom - work.top;
        const double targetArea = static_cast<double>(monitorWidth) * monitorHeight * 0.413;
        const int width = (std::max)(640, static_cast<int>(std::sqrt(targetArea * 16.0 / 9.0)));
        const int height = (std::max)(360, static_cast<int>(std::lround(width * 9.0 / 16.0)));
        const int x = work.left + (monitorWidth - width) / 2;
        const int y = work.top + (monitorHeight - height) / 2;
        m_hwnd = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, kClassName, L"Nskry — Text recognition",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
                WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
            x, y, width, height, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
        if (!m_hwnd) return false;
        m_windowOwnsLifetime = true;
        ::ShowWindow(m_hwnd, SW_SHOW);
        ::SetForegroundWindow(m_hwnd);
        return true;
    }

private:
    static constexpr wchar_t kClassName[] = L"NskryOcrResult";
    static inline std::once_flag s_classOnce;

    static void RegisterClass() {
        std::call_once(s_classOnce, [] {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WndProc;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            wc.lpszClassName = kClassName;
            ::RegisterClassExW(&wc);
        });
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
            auto* created = static_cast<OcrResultWindow*>(create->lpCreateParams);
            created->m_hwnd = hwnd;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
        }
        auto* self = reinterpret_cast<OcrResultWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCDESTROY && self) {
            const bool deleteAfterDestroy = self->m_windowOwnsLifetime;
            self->m_hwnd = nullptr;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            const LRESULT value = ::DefWindowProcW(hwnd, message, wp, lp);
            if (deleteAfterDestroy) delete self;
            return value;
        }
        return self ? self->HandleMessage(message, wp, lp) : ::DefWindowProcW(hwnd, message, wp, lp);
    }

    RECT WorkArea() const {
        MONITORINFO monitor{ sizeof(monitor) };
        if (::GetMonitorInfoW(::MonitorFromRect(&m_result->anchor, MONITOR_DEFAULTTONEAREST), &monitor)) return monitor.rcWork;
        return { 0, 0, 1280, 720 };
    }

    void CreateControls() {
        m_font = ::CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        m_boldFont = ::CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        m_zoomOutButton = Button(L"−", kZoomOutId);
        m_zoomInButton = Button(L"+", kZoomInId);
        m_scaleLabel = ::CreateWindowExW(0, L"STATIC", L"100%", WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 58, 40, m_hwnd, nullptr, nullptr, nullptr);
        m_highlightButton = Button(L"Highlight", kHighlightId);
        m_editButton = Button(L"Edit", kEditId);
        m_copyButton = Button(L"Copy", kCopyAllId);
        m_saveButton = Button(L"Save", kSaveId);
        m_rotateButton = Button(L"Rotate 90°", kRotateId);
        m_textEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", m_result->error.empty() ? m_result->text.c_str() : m_result->error.c_str(),
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY | WS_VSCROLL | WS_HSCROLL,
            0, 0, 1, 1, m_hwnd, nullptr, nullptr, nullptr);
        for (HWND child : { m_zoomOutButton, m_zoomInButton, m_scaleLabel, m_highlightButton, m_editButton,
                            m_copyButton, m_saveButton, m_rotateButton, m_textEdit })
            if (child) ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        LayoutControls();
    }

    HWND Button(const wchar_t* label, int id) {
        return ::CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE,
            0, 0, 1, 1, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    }

    RECT CanvasRect() const {
        RECT client{}; ::GetClientRect(m_hwnd, &client);
        const int textWidth = (std::clamp)(m_textPaneWidth, 180, (std::max)(180, static_cast<int>(client.right) - 320));
        const int leftWidth = (std::max)(320, static_cast<int>(client.right) - textWidth);
        return { 0, 0, leftWidth, (std::max)(0, static_cast<int>(client.bottom) - 40) };
    }

    void LayoutControls() {
        RECT client{}; ::GetClientRect(m_hwnd, &client);
        if (m_textPaneWidth == 0) m_textPaneWidth = (std::max)(180, static_cast<int>(client.right * 0.235));
        const RECT canvas = CanvasRect();
        if (m_scale == 0.0 && m_result->width > 0 && m_result->height > 0) {
            m_scale = (std::min)(1.0, (std::min)(static_cast<double>(canvas.right - canvas.left) / m_result->width,
                                                 static_cast<double>(canvas.bottom - canvas.top) / m_result->height));
        }
        const int textLeft = canvas.right;
        if (m_textEdit) ::SetWindowPos(m_textEdit, nullptr, textLeft + 8, 8,
            (std::max)(1, static_cast<int>(client.right) - textLeft - 16),
            (std::max)(1, static_cast<int>(client.bottom) - 16), SWP_NOZORDER | SWP_NOACTIVATE);
        int x = 8;
        const int y = canvas.bottom;
        const auto place = [&](HWND control, int width) { if (control) ::SetWindowPos(control, nullptr, x, y, width, 40, SWP_NOZORDER | SWP_NOACTIVATE); x += width + 4; };
        place(m_zoomOutButton, 34); place(m_scaleLabel, 58); place(m_zoomInButton, 34);
        place(m_highlightButton, 78); place(m_editButton, 52); place(m_copyButton, 54);
        place(m_saveButton, 54); place(m_rotateButton, 88);
        UpdateScaleLabel();
    }

    RECT PreviewRect() const {
        const RECT canvas = CanvasRect();
        if (!m_result->bitmap || m_result->width <= 0 || m_result->height <= 0) return {};
        const double fit = (std::min)(1.0, (std::min)(static_cast<double>(canvas.right - canvas.left) / m_result->width,
                                                       static_cast<double>(canvas.bottom - canvas.top) / m_result->height));
        const double scale = m_scale > 0.0 ? m_scale : fit;
        const int width = (std::max)(1, static_cast<int>(std::lround(m_result->width * scale)));
        const int height = (std::max)(1, static_cast<int>(std::lround(m_result->height * scale)));
        const int cx = (canvas.left + canvas.right) / 2 + static_cast<int>(std::lround(m_panX));
        const int cy = (canvas.top + canvas.bottom) / 2 + static_cast<int>(std::lround(m_panY));
        return { cx - width / 2, cy - height / 2, cx - width / 2 + width, cy - height / 2 + height };
    }

    RECT ToClientRect(const RECT& source) const {
        const RECT preview = PreviewRect();
        if (preview.right <= preview.left || m_result->width <= 0 || m_result->height <= 0) return {};
        const double sx = static_cast<double>(preview.right - preview.left) / m_result->width;
        const double sy = static_cast<double>(preview.bottom - preview.top) / m_result->height;
        return { preview.left + static_cast<int>(source.left * sx), preview.top + static_cast<int>(source.top * sy),
                 preview.left + static_cast<int>(source.right * sx), preview.top + static_cast<int>(source.bottom * sy) };
    }

    void DrawVisual(HDC dc) {
        const RECT canvas = CanvasRect();
        HBRUSH background = ::CreateSolidBrush(RGB(30, 31, 36));
        ::FillRect(dc, &canvas, background);
        ::DeleteObject(background);
        if (!m_result->error.empty()) {
            ::SetBkMode(dc, TRANSPARENT); ::SetTextColor(dc, RGB(240, 240, 240));
            RECT messageArea = canvas;
            ::DrawTextW(dc, m_result->error.c_str(), -1, &messageArea, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
            return;
        }
        const RECT preview = PreviewRect();
        if (!m_result->bitmap || preview.right <= preview.left) return;
        HDC memory = ::CreateCompatibleDC(dc);
        HGDIOBJ old = memory ? ::SelectObject(memory, m_result->bitmap) : nullptr;
        if (old && old != HGDI_ERROR) {
            ::SetStretchBltMode(dc, HALFTONE);
            ::StretchBlt(dc, preview.left, preview.top, preview.right - preview.left, preview.bottom - preview.top,
                         memory, 0, 0, m_result->width, m_result->height, SRCCOPY);
            ::SelectObject(memory, old);
        }
        if (memory) ::DeleteDC(memory);

        for (size_t index = 0; index < m_result->words.size(); ++index) {
            const RECT rect = ToClientRect(m_result->words[index].rect);
            const bool selected = std::find(m_selected.begin(), m_selected.end(), index) != m_selected.end();
            if (!m_highlights && !selected) continue;
            const COLORREF color = selected ? ::GetSysColor(COLOR_HIGHLIGHT) : RGB(63, 156, 255);
            HDC overlay = ::CreateCompatibleDC(dc);
            HBITMAP overlayBitmap = overlay ? ::CreateCompatibleBitmap(dc, 1, 1) : nullptr;
            HGDIOBJ oldOverlay = overlayBitmap ? ::SelectObject(overlay, overlayBitmap) : nullptr;
            if (oldOverlay && oldOverlay != HGDI_ERROR) {
                HBRUSH fill = ::CreateSolidBrush(color);
                RECT pixel{ 0, 0, 1, 1 };
                ::FillRect(overlay, &pixel, fill);
                ::DeleteObject(fill);
                const BLENDFUNCTION blend{ AC_SRC_OVER, 0, static_cast<BYTE>(selected ? 118 : 64), 0 };
                ::AlphaBlend(dc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                    overlay, 0, 0, 1, 1, blend);
                ::SelectObject(overlay, oldOverlay);
            }
            if (overlayBitmap) ::DeleteObject(overlayBitmap);
            if (overlay) ::DeleteDC(overlay);
            HPEN pen = ::CreatePen(PS_SOLID, 1, selected ? ::GetSysColor(COLOR_HIGHLIGHT) : RGB(170, 215, 255));
            HGDIOBJ oldPen = ::SelectObject(dc, pen);
            HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
            ::Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
            ::SelectObject(dc, oldPen); ::SelectObject(dc, oldBrush);
            ::DeleteObject(pen);
        }
        ::SetBkMode(dc, TRANSPARENT); ::SetTextColor(dc, RGB(190, 190, 190));
        const wchar_t* hint = L"Wheel: zoom · Shift+wheel: horizontal pan · Alt+wheel: vertical pan";
        ::TextOutW(dc, canvas.left + 10, canvas.top + 10, hint, static_cast<int>(wcslen(hint)));
    }

    void UpdateSelection(POINT first, POINT last) {
        const auto wordAt = [&](POINT point) -> size_t {
            for (size_t index = 0; index < m_result->words.size(); ++index) {
                RECT word = ToClientRect(m_result->words[index].rect);
                ::InflateRect(&word, 3, 3);
                if (::PtInRect(&word, point)) return index;
            }
            return static_cast<size_t>(-1);
        };
        const size_t firstWord = wordAt(first), lastWord = wordAt(last);
        m_selected.clear();
        if (firstWord == static_cast<size_t>(-1) || lastWord == static_cast<size_t>(-1)) return;
        const size_t begin = (std::min)(firstWord, lastWord), end = (std::max)(firstWord, lastWord);
        for (size_t index = begin; index <= end; ++index) m_selected.push_back(index);
    }

    void CopySelected() {
        std::wstring text;
        size_t previousLine = static_cast<size_t>(-1);
        for (const size_t index : m_selected) {
            if (!text.empty()) text += m_result->words[index].line == previousLine ? L" " : L"\r\n";
            text += m_result->words[index].text;
            previousLine = m_result->words[index].line;
        }
        if (!text.empty()) CopyTextToClipboard(m_hwnd, text);
    }

    void UpdateScaleLabel() {
        if (m_scaleLabel) {
            const int percent = static_cast<int>(std::lround((m_scale > 0.0 ? m_scale : 1.0) * 100.0));
            ::SetWindowTextW(m_scaleLabel, (std::to_wstring(percent) + L"%").c_str());
        }
    }

    void ZoomAt(POINT point, double multiplier) {
        const RECT canvas = CanvasRect();
        const RECT image = PreviewRect();
        const double oldScale = m_scale > 0.0 ? m_scale :
            (std::min)(1.0, (std::min)(static_cast<double>(canvas.right) / m_result->width, static_cast<double>(canvas.bottom) / m_result->height));
        const double imageX = (point.x - image.left) / oldScale;
        const double imageY = (point.y - image.top) / oldScale;
        m_scale = (std::clamp)(oldScale * multiplier, 0.05, 8.0);
        const int centerX = (canvas.left + canvas.right) / 2;
        const int centerY = (canvas.top + canvas.bottom) / 2;
        m_panX = point.x - centerX - (imageX - m_result->width / 2.0) * m_scale;
        m_panY = point.y - centerY - (imageY - m_result->height / 2.0) * m_scale;
        UpdateScaleLabel();
        ::InvalidateRect(m_hwnd, &canvas, FALSE);
    }

    void Pan(double dx, double dy) {
        m_panX += dx; m_panY += dy;
        const RECT canvas = CanvasRect();
        ::InvalidateRect(m_hwnd, &canvas, FALSE);
    }

    LRESULT HandleMessage(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_CREATE: CreateControls(); return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kZoomOutId) { RECT canvas = CanvasRect(); ZoomAt({ (canvas.left + canvas.right) / 2, (canvas.top + canvas.bottom) / 2 }, 1.0 / 1.2); return 0; }
            if (LOWORD(wp) == kZoomInId) { RECT canvas = CanvasRect(); ZoomAt({ (canvas.left + canvas.right) / 2, (canvas.top + canvas.bottom) / 2 }, 1.2); return 0; }
            if (LOWORD(wp) == kHighlightId) { m_highlights = !m_highlights; ::SetWindowTextW(m_highlightButton, m_highlights ? L"Highlight: on" : L"Highlight: off"); ::InvalidateRect(m_hwnd, nullptr, FALSE); return 0; }
            if (LOWORD(wp) == kCopyAllId) { if (m_result->copyBitmapToClipboard) m_result->copyBitmapToClipboard(m_result->bitmap); return 0; }
            if (LOWORD(wp) == kEditId) {
                if (m_result->openBitmapEditor) {
                    if (HBITMAP copy = CopyBitmap(m_result->bitmap)) m_result->openBitmapEditor(copy, m_result->width, m_result->height);
                }
                return 0;
            }
            if (LOWORD(wp) == kSaveId) { SaveBitmapAsPng(m_hwnd, m_result->bitmap); return 0; }
            if (LOWORD(wp) == kRotateId) {
                HBITMAP rotated = RotateClockwise(m_result->bitmap, m_result->width, m_result->height);
                if (!rotated || s_running.exchange(true)) { if (rotated) ::DeleteObject(rotated); return 0; }
                int ocrWidth{}, ocrHeight{};
                HBITMAP ocrBitmap = ResizeForOcr(rotated, m_result->height, m_result->width, ocrWidth, ocrHeight);
                if (!ocrBitmap) { ::DeleteObject(rotated); s_running = false; return 0; }
                const RECT anchor = m_result->anchor;
                const auto copyCallback = m_result->copyBitmapToClipboard;
                const auto editCallback = m_result->openBitmapEditor;
                const HWND dispatch = s_dispatchWindow;
                const int rotatedWidth = m_result->height, rotatedHeight = m_result->width;
                { std::lock_guard lock(s_workerMutex); if (s_worker.joinable()) s_worker.join();
                  s_worker = std::thread([rotated, rotatedWidth, rotatedHeight, ocrBitmap, ocrWidth, ocrHeight, anchor, copyCallback, editCallback, dispatch] {
                    auto result = RunOcr(ocrBitmap, ocrWidth, ocrHeight, rotated, rotatedWidth, rotatedHeight,
                                         anchor, copyCallback, editCallback);
                    { std::lock_guard pending(s_pendingMutex); s_pendingResults.push_back(std::move(result));
                      if (!::PostMessageW(dispatch, kResultMessage, 0, 0)) s_pendingResults.pop_back(); }
                    s_running = false;
                  }); }
                ::DestroyWindow(m_hwnd);
                return 0;
            }
            break;
        case WM_SIZE:
            LayoutControls();
            ::InvalidateRect(m_hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
            if (limits) {
                limits->ptMinTrackSize.x = (std::max)(limits->ptMinTrackSize.x, 640L);
                limits->ptMinTrackSize.y = (std::max)(limits->ptMinTrackSize.y, 360L);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC target = ::BeginPaint(m_hwnd, &paint);
            RECT client{}; ::GetClientRect(m_hwnd, &client);
            HDC buffer = ::CreateCompatibleDC(target);
            HBITMAP bitmap = buffer ? ::CreateCompatibleBitmap(target, client.right, client.bottom) : nullptr;
            HGDIOBJ previous = bitmap ? ::SelectObject(buffer, bitmap) : nullptr;
            if (previous && previous != HGDI_ERROR) {
                HBRUSH background = ::CreateSolidBrush(RGB(24, 24, 27));
                ::FillRect(buffer, &client, background);
                ::DeleteObject(background);
                DrawVisual(buffer);
                ::BitBlt(target, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
                ::SelectObject(buffer, previous);
            } else {
                DrawVisual(target);
            }
            if (bitmap) ::DeleteObject(bitmap);
            if (buffer) ::DeleteDC(buffer);
            ::EndPaint(m_hwnd, &paint); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEWHEEL: {
            POINT point{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ::ScreenToClient(m_hwnd, &point);
            const RECT image = PreviewRect();
            if (!::PtInRect(&image, point)) break;
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (::GetKeyState(VK_SHIFT) < 0) Pan(delta / 3.0, 0);
            else if (::GetKeyState(VK_MENU) < 0) Pan(0, delta / 3.0);
            else ZoomAt(point, delta > 0 ? 1.12 : 1.0 / 1.12);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (!m_result->error.empty()) break;
            { const RECT image = PreviewRect(); const POINT point{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
              if (!::PtInRect(&image, point)) break; }
            ::SetFocus(m_hwnd);
            m_dragSelecting = true;
            m_dragStart = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            m_dragCurrent = m_dragStart;
            m_selected.clear();
            ::SetCapture(m_hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (m_dragSelecting) {
                m_dragCurrent = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                UpdateSelection(m_dragStart, m_dragCurrent);
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (m_dragSelecting) {
                ::ReleaseCapture();
                m_dragSelecting = false;
                m_dragCurrent = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                UpdateSelection(m_dragStart, m_dragCurrent);
                if (m_selected.empty()) {
                    for (size_t index = 0; index < m_result->words.size(); ++index) {
                        const RECT word = ToClientRect(m_result->words[index].rect);
                        if (::PtInRect(&word, m_dragCurrent)) { m_selected.push_back(index); break; }
                    }
                }
                ::InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            m_dragSelecting = false;
            return 0;
        case WM_KEYDOWN:
            if ((wp == 'C' || wp == VK_INSERT) && (::GetKeyState(VK_CONTROL) & 0x8000)) { CopySelected(); return 0; }
            break;
        case WM_SETCURSOR: {
            POINT point{}; ::GetCursorPos(&point); ::ScreenToClient(m_hwnd, &point);
            const RECT image = PreviewRect();
            if (::PtInRect(&image, point)) { ::SetCursor(::LoadCursorW(nullptr, IDC_IBEAM)); return TRUE; }
            break;
        }
        case WM_CLOSE:
            ::DestroyWindow(m_hwnd);
            return 0;
        }
        return ::DefWindowProcW(m_hwnd, message, wp, lp);
    }

    HWND m_hwnd{};
    HWND m_zoomOutButton{};
    HWND m_zoomInButton{};
    HWND m_scaleLabel{};
    HWND m_highlightButton{};
    HWND m_editButton{};
    HWND m_copyButton{};
    HWND m_saveButton{};
    HWND m_rotateButton{};
    HWND m_textEdit{};
    HFONT m_font{};
    HFONT m_boldFont{};
    std::unique_ptr<OcrJobResult> m_result;
    bool m_highlights{ true };
    bool m_windowOwnsLifetime{};
    bool m_dragSelecting{};
    POINT m_dragStart{};
    POINT m_dragCurrent{};
    std::vector<size_t> m_selected;
    double m_scale{};
    double m_panX{};
    double m_panY{};
    int m_textPaneWidth{};
};

void ShowResult(std::unique_ptr<OcrJobResult> result) {
    if (!result) return;
    auto* window = new (std::nothrow) OcrResultWindow(std::move(result));
    if (!window || !window->Show()) delete window;
}

LRESULT CALLBACK DispatchProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == kResultMessage) {
        std::unique_ptr<OcrJobResult> result;
        {
            std::lock_guard lock(s_pendingMutex);
            if (s_pendingResults.empty()) return 0;
            result = std::move(s_pendingResults.back());
            s_pendingResults.pop_back();
        }
        ShowResult(std::move(result));
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, wp, lp);
}

bool CreateDispatchWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DispatchProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NskryOcrDispatch";
    ::RegisterClassExW(&wc);
    s_dispatchWindow = ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                          HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    return s_dispatchWindow != nullptr;
}

} // namespace

extern "C" NSKRY_API const NskryPluginInfo* NSKRY_CALL nskry_plugin_info() { return &kInfo; }

extern "C" NSKRY_API int32_t NSKRY_CALL nskry_plugin_init(const NskryHostContext* context) {
    return context && context->structSize >= sizeof(NskryHostContext) &&
        context->apiVersion == NSKRY_PLUGIN_API_VERSION && CreateDispatchWindow() ? 1 : 0;
}

extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_execute(const NskryHostContext* context) {
    if (!context || context->structSize < sizeof(NskryHostContext) || !context->capturedBitmap || !s_dispatchWindow) return;
    if (s_running.exchange(true)) {
        if (context->showNotification) context->showNotification(L"Text recognition is already running.", 2200);
        return;
    }
    int width{}, height{};
    if (!GetBitmapSize(context->capturedBitmap, width, height)) {
        s_running = false;
        if (context->showNotification) context->showNotification(L"The selected image is not available for text recognition.", 2500);
        return;
    }
    int scaledWidth{}, scaledHeight{};
    HBITMAP displayCopy = CopyBitmap(context->capturedBitmap);
    HBITMAP ocrCopy = ResizeForOcr(context->capturedBitmap, width, height, scaledWidth, scaledHeight);
    if (!displayCopy || !ocrCopy) {
        if (displayCopy) ::DeleteObject(displayCopy);
        if (ocrCopy) ::DeleteObject(ocrCopy);
        s_running = false;
        if (context->showNotification) context->showNotification(L"Could not prepare this image for text recognition.", 2500);
        return;
    }
    if (context->showNotification) context->showNotification(L"Recognizing text…", 2200);
    const RECT anchor = context->capturedRegion;
    const HWND dispatch = s_dispatchWindow;
    std::lock_guard lock(s_workerMutex);
    if (s_worker.joinable()) s_worker.join();
    try {
        const auto copyCallback = context->copyBitmapToClipboard;
        const auto editCallback = context->openBitmapEditor;
        s_worker = std::thread([displayCopy, width, height, ocrCopy, scaledWidth, scaledHeight, anchor, dispatch, copyCallback, editCallback] {
            std::unique_ptr<OcrJobResult> result = RunOcr(ocrCopy, scaledWidth, scaledHeight,
                displayCopy, width, height, anchor, copyCallback, editCallback);
            {
                std::lock_guard lock(s_pendingMutex);
                s_pendingResults.push_back(std::move(result));
                if (!::PostMessageW(dispatch, kResultMessage, 0, 0)) {
                    // The dispatch window is only destroyed during shutdown;
                    // release the completed bitmap instead of retaining it.
                    s_pendingResults.pop_back();
                }
            }
            s_running = false;
        });
    } catch (...) {
        ::DeleteObject(displayCopy);
        ::DeleteObject(ocrCopy);
        s_running = false;
        if (context->showNotification) context->showNotification(L"Could not start the text recognition worker.", 2500);
    }
}

extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_shutdown() {
    std::lock_guard lock(s_workerMutex);
    if (s_worker.joinable()) s_worker.join();
    s_running = false;
    {
        std::lock_guard pendingLock(s_pendingMutex);
        s_pendingResults.clear();
    }
    if (s_dispatchWindow) {
        ::DestroyWindow(s_dispatchWindow);
        s_dispatchWindow = nullptr;
    }
}
