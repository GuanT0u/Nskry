#include "../../common/native_ui.h"
#include "nskry_plugin.h"
#include "nskry_input.h"
#include "nskry_text_translation.h"
#include <functional>

extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_configure_input_v1(const NskryInputConfigV1* config) {
    nskry::input::Configure(config);
}
extern "C" NSKRY_API int32_t NSKRY_CALL nskry_plugin_configure_input_v2(const NskryInputConfigV1* config,NskryInputResultV2* result) {
    using K=nskry::input::Kind;
    return nskry::input::ConfigureConfirmed(config,result,{{L"copy",K::Key},{L"copy_alt",K::Key},{L"cancel",K::Key},
        {L"zoom",K::Wheel},{L"pan_x",K::Wheel},{L"pan_y",K::Wheel}});
}
#include "../../common/ocr_client.h"
#include "ocr_layout.h"
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <wincodec.h>
#include <wrl/client.h>

static LRESULT CALLBACK OcrInputRelay(HWND window,UINT message,WPARAM key,LPARAM flags,UINT_PTR id,DWORD_PTR parent) {
    if(message==WM_KEYDOWN || message==WM_SYSKEYDOWN) {
        const bool copy=nskry::input::PluginMatches(L"copy",L"Ctrl+C",UINT(key)) ||
            nskry::input::PluginMatches(L"copy_alt",L"Ctrl+Insert",UINT(key));
        if(id==2 && copy){SendMessageW(window,WM_COPY,0,0);return 0;}
        if(id==1 && (copy || nskry::input::PluginMatches(L"cancel",L"Esc",UINT(key)))){
            SendMessageW(HWND(parent),message,key,flags);return 0;
        }
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,OcrInputRelay,id);
    return DefSubclassProc(window,message,key,flags);
}
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
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
constexpr UINT kProgressMessage = WM_APP + 0x4B2;
constexpr int kCancelOcrId = 1100;
constexpr int kTextModeId = 1001;
constexpr int kVisualModeId = 1002;
constexpr int kCopyAllId = 1003;
constexpr int kCopyTextId = 1013;
constexpr int kZoomOutId = 1004;
constexpr int kZoomInId = 1005;
constexpr int kHighlightId = 1006;
constexpr int kEditId = 1007;
constexpr int kSaveId = 1008;
constexpr int kRotateId = 1009;
constexpr int kEngineId = 1010;
constexpr int kTranslateId = 1011;
NskryTextTranslationServiceV1 s_translationService{};
using nskry::ocr::WordBox;
enum class OcrEngineKind { Accurate, Fast };
std::atomic<OcrEngineKind> s_preferredEngine{
    NSKRY_OCR_MODEL_ENGINE ? OcrEngineKind::Accurate : OcrEngineKind::Fast };

OcrEngineKind PreferredEngine() {
    wchar_t value[32]{};
    const DWORD count = ::GetEnvironmentVariableW(L"NSKRY_OCR_ENGINE", value, ARRAYSIZE(value));
    if (count && count < ARRAYSIZE(value)) {
        if (_wcsicmp(value, L"windows") == 0 || _wcsicmp(value, L"fast") == 0) return OcrEngineKind::Fast;
        if (_wcsicmp(value, L"accurate") == 0) return OcrEngineKind::Accurate;
    }
    return s_preferredEngine.load();
}

enum class OcrFailure { Unknown, LanguageMissing, ImagePreparation, ModelsUnavailable };

struct OcrJobResult {
    HBITMAP bitmap{};
    uint64_t generation{};
    int width{};
    int height{};
    RECT anchor{};
    std::wstring text;
    std::wstring error;
    OcrFailure failure = OcrFailure::Unknown;
    std::vector<WordBox> words;
    std::vector<RECT> lineHighlights;
    OcrEngineKind engine{OcrEngineKind::Fast};
    int32_t (*copyBitmapToClipboard)(HBITMAP){};
    void (*openBitmapEditor)(HBITMAP, int, int){};

    ~OcrJobResult() {
        if (bitmap) ::DeleteObject(bitmap);
    }
};

NskryAcquireOcrV1 acquireOcr{};
struct OcrRequest {
    std::shared_ptr<nskry::ocr_client::Lease> provider;
    HBITMAP bitmap{};
    int width{};
    int height{};
    RECT anchor{};
    bool rotate{};
    uint64_t generation{};
    OcrEngineKind engine{PreferredEngine()};
    int32_t (*copyBitmapToClipboard)(HBITMAP){};
    void (*openBitmapEditor)(HBITMAP, int, int){};

    ~OcrRequest() { if (bitmap) ::DeleteObject(bitmap); }
};

NskryPluginInfo kInfo{ sizeof(NskryPluginInfo), NSKRY_PLUGIN_API_VERSION,
    L"Text recognition", L"nskry-ocr", L"0.7.0", L"Nskry Team",
    L"On-demand offline text recognition", nullptr,
    NSKRY_CAP_TOOLBAR_ACTION | NSKRY_CAP_POST_CAPTURE, {} };

HWND s_dispatchWindow{};
std::atomic_bool s_running{};
std::atomic_uint64_t s_generation{};
std::mutex s_workerMutex;
std::thread s_worker;
std::unique_ptr<OcrRequest> s_queuedRequest;
std::mutex s_pendingMutex;
std::vector<std::unique_ptr<OcrJobResult>> s_pendingResults;
struct ProgressInfo { uint64_t generation{}; RECT anchor{}; OcrEngineKind engine{OcrEngineKind::Fast}; };
ProgressInfo s_progressInfo;
HWND s_progressWindow{}; // UI-thread-owned; background work posts to dispatch.

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

void ProcessRequests(std::unique_ptr<OcrRequest> request) {
    while (request) {
        std::unique_ptr<OcrJobResult> result;
        try {
            result = std::make_unique<OcrJobResult>();
            result->bitmap = std::exchange(request->bitmap, nullptr);
            result->width = request->width;
            result->height = request->height;
            result->engine = request->engine;
            result->generation = request->generation;
            result->anchor = request->anchor;
            result->copyBitmapToClipboard = request->copyBitmapToClipboard;
            result->openBitmapEditor = request->openBitmapEditor;
            if (request->rotate) {
                if (auto rotated = RotateClockwise(result->bitmap, result->width, result->height)) {
                    DeleteObject(result->bitmap);
                    result->bitmap = rotated;
                    std::swap(result->width, result->height);
                }
            }
            wchar_t error[256]{};
            NskryOcrRequestV1 input{sizeof(input), 1, result->bitmap, request->engine == OcrEngineKind::Fast,
                result.get(),
                [](void *ctx) -> int32_t { return static_cast<OcrJobResult*>(ctx)->generation != s_generation.load(); },
                [](void *ctx, const NskryOcrLayoutV1 *out) -> int32_t {
                    if (!nskry::ocr_client::Valid(out)) return 0;
                    auto &r = *static_cast<OcrJobResult*>(ctx);
                    r.text.assign(out->text, out->length);
                    if (out->lineCount) r.lineHighlights.assign(out->lines, out->lines + out->lineCount);
                    for (uint32_t i = 0; i < out->wordCount; ++i) {
                        const auto &w = out->words[i];
                        r.words.push_back({w.bounds, r.text.substr(w.offset, w.length), w.line, w.joinPrevious != 0});
                    }
                    return 1;
                }, error, 256, request->generation};
            const auto status = request->provider ? request->provider->api.recognize(&input) : NSKRY_OCR_UNAVAILABLE;
            if (status != NSKRY_OCR_OK) {
                result->text.clear();
                result->words.clear();
                result->lineHighlights.clear();
                result->error = error[0] ? error : nskry::ocr_client::Message(status);
                if (status == NSKRY_OCR_LANGUAGE) result->failure = OcrFailure::LanguageMissing;
            }
        } catch (...) { if (result) result->error = L"OCR failed."; }
        request.reset();
        {
            std::lock_guard lock(s_workerMutex);
            if (s_queuedRequest) request = std::move(s_queuedRequest);
            else s_running = false;
        }
        if (result && result->generation == s_generation.load()) {
            std::lock_guard pending(s_pendingMutex);
            s_pendingResults.push_back(std::move(result));
            if (!::PostMessageW(s_dispatchWindow, kResultMessage, 0, 0)) s_pendingResults.pop_back();
        }
    }
}

void CloseProgress() {
    if (s_progressWindow) ::DestroyWindow(s_progressWindow);
    s_progressWindow = nullptr;
}

LRESULT CALLBACK ProgressProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_CREATE) {
        HWND label = ::CreateWindowExW(0, L"STATIC", nskry::plugin_ui::Text(L"Recognizing text…", L"正在识别文字…"), WS_CHILD | WS_VISIBLE | SS_CENTER,
            12, 12, 300, 24, hwnd, nullptr, nullptr, nullptr);
        HWND button = ::CreateWindowExW(0, L"BUTTON", nskry::plugin_ui::Text(L"Cancel", L"取消"), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            122, 46, 80, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelOcrId)), nullptr, nullptr);
        for (HWND child : {label, button}) if (child)
            ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(::GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        if(button)::SetWindowSubclass(button,OcrInputRelay,1,DWORD_PTR(hwnd));
        return 0;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == kCancelOcrId) ||
        ((message == WM_KEYDOWN || message==WM_SYSKEYDOWN) && nskry::input::PluginMatches(L"cancel",L"Esc",UINT(wp)))) {
        uint64_t expected = static_cast<uint64_t>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        const uint64_t canceledGeneration = expected;
        if (s_generation.compare_exchange_strong(expected, expected + 1)) {
            std::lock_guard lock(s_workerMutex);
            if (s_queuedRequest && s_queuedRequest->generation <= canceledGeneration) s_queuedRequest.reset();
        }
        ::DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_NCDESTROY && s_progressWindow == hwnd) s_progressWindow = nullptr;
    return ::DefWindowProcW(hwnd, message, wp, lp);
}

void ShowProgress(const ProgressInfo& info) {
    if (info.engine == OcrEngineKind::Fast) { CloseProgress(); return; }
    constexpr const wchar_t* name = L"NskryOcrProgress";
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = ProgressProc;
    wc.hInstance = ::GetModuleHandleW(nullptr); wc.lpszClassName = name;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    ::RegisterClassExW(&wc);
    MONITORINFO monitor{sizeof(monitor)};
    RECT work{0, 0, 1280, 720};
    if (::GetMonitorInfoW(::MonitorFromRect(&info.anchor, MONITOR_DEFAULTTONEAREST), &monitor)) work = monitor.rcWork;
    const int x = (std::clamp)((info.anchor.left + info.anchor.right) / 2 - 170, work.left, (std::max)(work.left, work.right - 340));
    const int y = (std::clamp)((info.anchor.top + info.anchor.bottom) / 2 - 60, work.top, (std::max)(work.top, work.bottom - 120));
    if (!s_progressWindow) s_progressWindow = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, name,
        nskry::plugin_ui::Text(L"Nskry — Text recognition", L"Nskry — 文字识别"), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        x, y, 340, 120, nullptr, nullptr, wc.hInstance, reinterpret_cast<void*>(static_cast<uintptr_t>(info.generation)));
    else {
        ::SetWindowLongPtrW(s_progressWindow, GWLP_USERDATA, static_cast<LONG_PTR>(info.generation));
        ::SetWindowPos(s_progressWindow, HWND_TOPMOST, x, y, 340, 120, SWP_NOACTIVATE);
    }
    if (s_progressWindow) ::ShowWindow(s_progressWindow, SW_SHOWNOACTIVATE);
}

bool QueueOcr(std::unique_ptr<OcrRequest> request) {
    request->provider = nskry::ocr_client::Acquire(acquireOcr);
    if (!request || !request->bitmap) return false;
    {
        std::lock_guard lock(s_workerMutex);
        request->generation = ++s_generation;
        s_progressInfo = {request->generation, request->anchor, request->engine};
        if (s_running) s_queuedRequest = std::move(request);
        else {
            if (s_worker.joinable()) s_worker.join();
            s_running = true;
            try { s_worker = std::thread(ProcessRequests, std::move(request)); }
            catch (...) { s_running = false; return false; }
        }
    }
    ::PostMessageW(s_dispatchWindow, kProgressMessage, 0, 0);
    return true;
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
        m_hwnd = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, kClassName,
            m_result->engine == OcrEngineKind::Accurate ? nskry::plugin_ui::Text(L"Nskry — Text recognition (Accurate)", L"Nskry — 文字识别（精确）") : nskry::plugin_ui::Text(L"Nskry — Text recognition (Fast)", L"Nskry — 文字识别（快速）"),
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
    static void RegisterClass() {
        // Registration must work again after shutdown/re-init, not just once
        // per DLL lifetime. Existing registration is harmlessly reused.
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        ::RegisterClassExW(&wc);
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

    const wchar_t* FailureHint() const {
        using nskry::plugin_ui::Text;
        switch (m_result->failure) {
        case OcrFailure::LanguageMissing:
            return Text(L"Install an OCR language in Windows Settings, then retry.", L"请在 Windows 设置中安装 OCR 语言后重试。");
        case OcrFailure::ImagePreparation:
            return Text(L"Could not prepare the image. Try a different region.", L"无法准备图片，请尝试重新框选。");
        case OcrFailure::ModelsUnavailable:
            return Text(L"Offline OCR is unavailable. Reinstall the plugin or switch to Fast OCR.",
                        L"离线识别不可用。请重新安装插件，或切换为快速识别。");
        default:
            if (!m_result->error.empty()) return m_result->error.c_str();
            return Text(L"Recognition failed. Try a smaller region or switch engines.", L"识别失败。请缩小选区或切换识别引擎。");
        }
    }

    int Dip(int value) const { return ::MulDiv(value, m_dpi, 96); }

    void SetDpi(UINT dpi) {
        m_dpi = dpi ? dpi : 96;
        HFONT font = nskry::plugin_ui::CreateMessageFont(m_hwnd);
        if (font) {
            ::EnumChildWindows(m_hwnd, [](HWND child, LPARAM fontValue) -> BOOL {
                ::SendMessageW(child, WM_SETFONT, fontValue, TRUE);
                return TRUE;
            }, reinterpret_cast<LPARAM>(font));
            if (m_font) ::DeleteObject(m_font);
            m_font = font;
        }
    }

    void CreateControls() {
        SetDpi(::GetDpiForWindow(m_hwnd));
        m_zoomOutButton = Button(L"−", kZoomOutId);
        m_zoomInButton = Button(L"+", kZoomInId);
        m_scaleLabel = ::CreateWindowExW(0, L"STATIC", L"100%", WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 58, 40, m_hwnd, nullptr, nullptr, nullptr);
        m_highlightButton = Button(nskry::plugin_ui::Text(L"Highlight", L"高亮"), kHighlightId);
        m_editButton = Button(nskry::plugin_ui::Text(L"Edit", L"编辑"), kEditId);
        m_copyButton = Button(nskry::plugin_ui::Text(L"Copy image", L"复制图片"), kCopyAllId);
        m_copyTextButton = Button(nskry::plugin_ui::Text(L"Copy text", L"复制原文"), kCopyTextId);
        ::EnableWindow(m_copyTextButton, m_result->error.empty() && !m_result->text.empty());
        m_textLabel = ::CreateWindowExW(0, L"STATIC",
            m_result->error.empty() ? nskry::plugin_ui::Text(L"Recognized text", L"识别原文") :
                nskry::plugin_ui::Text(L"Error details", L"错误详情"), WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 1, 1, m_hwnd, nullptr, nullptr, nullptr);
        m_saveButton = Button(nskry::plugin_ui::Text(L"Save", L"保存"), kSaveId);
        m_rotateButton = Button(nskry::plugin_ui::Text(L"Rotate 90°", L"旋转 90°"), kRotateId);
        m_translateButton = Button(nskry::plugin_ui::Text(L"Translate", L"翻译"), kTranslateId);
        if (NSKRY_OCR_MODEL_ENGINE)
            m_engineButton = Button(m_result->engine == OcrEngineKind::Accurate ? nskry::plugin_ui::Text(L"Fast OCR", L"快速识别") : nskry::plugin_ui::Text(L"Accurate OCR", L"精确识别"), kEngineId);
        m_textEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", !m_result->error.empty() ? m_result->error.c_str() : !m_result->text.empty() ? m_result->text.c_str() :
            nskry::plugin_ui::Text(L"No text found. Try a different region.", L"未找到文字，请尝试重新框选。"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            0, 0, 1, 1, m_hwnd, nullptr, nullptr, nullptr);
        for (HWND child : { m_zoomOutButton, m_zoomInButton, m_scaleLabel, m_highlightButton, m_editButton,
                            m_copyButton, m_saveButton, m_rotateButton, m_engineButton, m_translateButton, m_textEdit, m_copyTextButton, m_textLabel })
            if (child) ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        for(HWND child:{m_zoomOutButton,m_zoomInButton,m_highlightButton,m_editButton,m_copyButton,m_saveButton,m_rotateButton,m_engineButton,m_translateButton,m_copyTextButton})
            if(child)::SetWindowSubclass(child,OcrInputRelay,1,DWORD_PTR(m_hwnd));
        if(m_textEdit)::SetWindowSubclass(m_textEdit,OcrInputRelay,2,DWORD_PTR(m_hwnd));
        nskry::plugin_ui::AttachTabNavigation(m_hwnd);
        LayoutControls();
        UpdateTranslationButton();
    }

    void UpdateTranslationButton() {
        const bool available = s_translationService.available && s_translationService.translate &&
            s_translationService.available() != 0 && m_result->error.empty() && !m_result->lineHighlights.empty();
        ::EnableWindow(m_translateButton, available);

    }

    HWND Button(const wchar_t* label, int id) {
        return ::CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_MULTILINE,
            0, 0, 1, 1, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    }

    RECT CanvasRect() const {
        RECT client{}; ::GetClientRect(m_hwnd, &client);
        const int minimumText = (std::min)(Dip(180), static_cast<int>(client.right) / 3);
        const int textWidth = (std::clamp)(m_textPaneWidth, minimumText, (std::max)(minimumText, static_cast<int>(client.right) / 2));
        return {0, 0, client.right - textWidth, (std::max)(0, static_cast<int>(client.bottom) - Dip(44))};
    }

    void LayoutControls() {
        RECT client{}; ::GetClientRect(m_hwnd, &client);
        if (m_textPaneWidth == 0) m_textPaneWidth = (std::max)(Dip(180), static_cast<int>(client.right * 0.25));
        const RECT canvas = CanvasRect();
        if (m_scale == 0.0 && m_result->width > 0 && m_result->height > 0) {
            m_scale = (std::min)(1.0, (std::min)(static_cast<double>(canvas.right - canvas.left) / m_result->width,
                                                 static_cast<double>(canvas.bottom - canvas.top) / m_result->height));
        }
        const int textLeft = canvas.right;
        const int paneWidth = (std::max)(1, static_cast<int>(client.right) - textLeft - Dip(16));
        const int actionWidth = (std::max)(1, (paneWidth - Dip(6)) / 2);
        const int footer = (std::max)(Dip(8), static_cast<int>(client.bottom) - Dip(40));
        const auto move = [&](HWND control, int x, int y, int width, int height) {
            if (control) ::SetWindowPos(control, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        };
        move(m_textLabel, textLeft + Dip(8), Dip(8), paneWidth, Dip(24));
        move(m_textEdit, textLeft + Dip(8), Dip(34), paneWidth, (std::max)(1, footer - Dip(42)));
        move(m_copyTextButton, textLeft + Dip(8), footer, actionWidth, Dip(32));
        move(m_translateButton, textLeft + Dip(14) + actionWidth, footer, (std::max)(1, paneWidth - actionWidth - Dip(6)), Dip(32));
        int x = Dip(8);
        const int y = canvas.bottom;
        const auto place = [&](HWND control, int width) { if (control) ::SetWindowPos(control, nullptr, x, y, Dip(width), Dip(40), SWP_NOZORDER | SWP_NOACTIVATE); x += Dip(width + 4); };
        const bool narrow = canvas.right < Dip(620);
        place(m_zoomOutButton, narrow ? 30 : 34); place(m_scaleLabel, narrow ? 52 : 58); place(m_zoomInButton, narrow ? 30 : 34);
        ::SetWindowTextW(m_highlightButton, narrow ? (m_highlights ? nskry::plugin_ui::Text(L"HL: on", L"高亮开") : nskry::plugin_ui::Text(L"HL: off", L"高亮关")) :
            (m_highlights ? nskry::plugin_ui::Text(L"Highlight", L"高亮") : nskry::plugin_ui::Text(L"Highlight: off", L"高亮关闭")));
        place(m_highlightButton, narrow ? 64 : 78); place(m_editButton, narrow ? 44 : 52); place(m_copyButton, narrow ? 65 : 78);
        place(m_saveButton, narrow ? 44 : 54);
        ::SetWindowTextW(m_rotateButton, narrow ? L"↻ 90°" : nskry::plugin_ui::Text(L"Rotate 90°", L"旋转 90°"));
        place(m_rotateButton, narrow ? 52 : 88);
        m_engineAtTop = m_engineButton && x + Dip(106) > canvas.right;
        if (m_engineAtTop) ::SetWindowPos(m_engineButton, nullptr, canvas.right - Dip(112), Dip(4), Dip(106), Dip(28), SWP_NOZORDER | SWP_NOACTIVATE);
        else place(m_engineButton, 106);
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
        const auto colors = nskry::plugin_ui::Colors(nskry::plugin_ui::preferences.theme);
        ::FillRect(dc, &canvas, nskry::plugin_ui::Brush(dc, colors.panel));
        if (!m_result->error.empty()) {
            ::SetBkMode(dc, TRANSPARENT); ::SetTextColor(dc, colors.text);
            RECT messageArea = canvas;
            ::DrawTextW(dc, FailureHint(), -1, &messageArea, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
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

        const int saved = ::SaveDC(dc);
        if (saved) ::IntersectClipRect(dc, canvas.left, canvas.top, canvas.right, canvas.bottom);
        HDC overlay = ::CreateCompatibleDC(dc);
        HBITMAP pixelBitmap = overlay ? ::CreateCompatibleBitmap(dc, 1, 1) : nullptr;
        HGDIOBJ previous = pixelBitmap ? ::SelectObject(overlay, pixelBitmap) : nullptr;
        if (previous && previous != HGDI_ERROR) {
            const auto fillRects = [&](const std::vector<RECT> &rects, COLORREF color, BYTE alpha) {
                HBRUSH brush = ::CreateSolidBrush(color);
                RECT pixel{0,0,1,1};
                ::FillRect(overlay, &pixel, brush);
                ::DeleteObject(brush);
                for (const auto &source : rects) {
                    const auto rect = ToClientRect(source);
                    if (rect.right <= rect.left || rect.bottom <= rect.top) continue;
                    const BLENDFUNCTION blend{AC_SRC_OVER,0,alpha,0};
                    ::AlphaBlend(dc, rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top,
                        overlay, 0,0,1,1,blend);
                }
            };
            if (m_highlights) fillRects(m_result->lineHighlights, RGB(63,156,255), 48);
            fillRects(nskry::ocr::SelectionRects(m_result->words, m_selected), ::GetSysColor(COLOR_HIGHLIGHT), 118);
            ::SelectObject(overlay, previous);
        }
        if (pixelBitmap) ::DeleteObject(pixelBitmap);
        if (overlay) ::DeleteDC(overlay);
        if (saved) ::RestoreDC(dc, saved);
        ::SetBkMode(dc, TRANSPARENT); ::SetTextColor(dc, colors.text);
        const wchar_t* hint = nskry::plugin_ui::Text(L"Zoom / pan shortcuts: Settings → Hotkeys → Text recognition", L"缩放和平移快捷键可在「设置 → 快捷键 → 文字识别」中修改");
        RECT hintArea{canvas.left + 10, canvas.top + 8, canvas.right - (m_engineAtTop ? 120 : 10), canvas.top + 30};
        ::DrawTextW(dc, hint, -1, &hintArea, DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
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
            if (!text.empty()) {
                if (m_result->words[index].line != previousLine) text += L"\r\n";
                else if (!m_result->words[index].joinPrevious) text += L" ";
            }
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
        case WM_ACTIVATE:
            UpdateTranslationButton();
            break;
        case WM_COMMAND:
            if (LOWORD(wp) == kTranslateId) {
                // Copy before the host may show a provider menu/pump messages.
                // That menu can outlive this OCR window; do not access self after dispatch.
                const auto owner = m_hwnd;
                const std::wstring text = m_result->text;
                const NskryTextTranslationV1 request{sizeof(request), 1, owner, text.c_str(), uint32_t(text.size())};
                wchar_t error[512]{};
                NskryTranslationResultV1 result{sizeof(result), 1, error, 512};
                const auto translate = s_translationService.translate;
                const auto status = translate ? translate(&request, &result) : NSKRY_TRANSLATION_UNAVAILABLE;
                error[511] = 0;
                if (status != NSKRY_TRANSLATION_ACCEPTED && status != NSKRY_TRANSLATION_CANCELED && ::IsWindow(owner))
                    ::MessageBoxW(owner, error[0] ? error : nskry::plugin_ui::Text(L"Enable/install a text translation plugin and use a matching Core build.", L"请启用或安装翻译插件，并使用兼容的 Nskry 版本。"),
                        nskry::plugin_ui::Text(L"Nskry — Translation", L"Nskry — 翻译"), MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (LOWORD(wp) == kZoomOutId) { RECT canvas = CanvasRect(); ZoomAt({ (canvas.left + canvas.right) / 2, (canvas.top + canvas.bottom) / 2 }, 1.0 / 1.2); return 0; }
            if (LOWORD(wp) == kZoomInId) { RECT canvas = CanvasRect(); ZoomAt({ (canvas.left + canvas.right) / 2, (canvas.top + canvas.bottom) / 2 }, 1.2); return 0; }
            if (LOWORD(wp) == kHighlightId) { m_highlights = !m_highlights; LayoutControls(); ::InvalidateRect(m_hwnd, nullptr, FALSE); return 0; }
            if (LOWORD(wp) == kCopyTextId) {
                if (m_result->error.empty() && !m_result->text.empty())
                    CopyTextToClipboard(m_hwnd, m_result->text);
                return 0;
            }
            if (LOWORD(wp) == kCopyAllId) { if (m_result->copyBitmapToClipboard) m_result->copyBitmapToClipboard(m_result->bitmap); return 0; }
            if (LOWORD(wp) == kEditId) {
                if (m_result->openBitmapEditor) {
                    if (HBITMAP copy = CopyBitmap(m_result->bitmap)) m_result->openBitmapEditor(copy, m_result->width, m_result->height);
                }
                return 0;
            }
            if (LOWORD(wp) == kSaveId) { SaveBitmapAsPng(m_hwnd, m_result->bitmap); return 0; }
            if (LOWORD(wp) == kRotateId || LOWORD(wp) == kEngineId) {
                auto request = std::make_unique<OcrRequest>();
                request->bitmap = CopyBitmap(m_result->bitmap);
                request->width = m_result->width;
                request->height = m_result->height;
                request->anchor = m_result->anchor;
                request->rotate = LOWORD(wp) == kRotateId;
                request->engine = LOWORD(wp) == kEngineId ?
                    (m_result->engine == OcrEngineKind::Accurate ? OcrEngineKind::Fast : OcrEngineKind::Accurate) : m_result->engine;
                if (LOWORD(wp) == kEngineId) s_preferredEngine = request->engine;
                request->copyBitmapToClipboard = m_result->copyBitmapToClipboard;
                request->openBitmapEditor = m_result->openBitmapEditor;
                if (QueueOcr(std::move(request))) ::DestroyWindow(m_hwnd);
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            const auto &bounds = *reinterpret_cast<const RECT*>(lp);
            const UINT next = LOWORD(wp) ? LOWORD(wp) : 96;
            m_textPaneWidth = ::MulDiv(m_textPaneWidth, next, m_dpi);
            SetDpi(next);
            ::SetWindowPos(m_hwnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutControls();
            return 0;
        }
        case WM_SIZE:
            LayoutControls();
            ::InvalidateRect(m_hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
            if (limits) {
                limits->ptMinTrackSize.x = (std::max)(limits->ptMinTrackSize.x, static_cast<LONG>(Dip(640)));
                limits->ptMinTrackSize.y = (std::max)(limits->ptMinTrackSize.y, static_cast<LONG>(Dip(360)));
            }
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            return nskry::plugin_ui::ControlColors(wp, nskry::plugin_ui::preferences.theme,
                                                  reinterpret_cast<HWND>(lp) == m_textEdit);
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC target = ::BeginPaint(m_hwnd, &paint);
            RECT client{}; ::GetClientRect(m_hwnd, &client);
            HDC buffer = ::CreateCompatibleDC(target);
            HBITMAP bitmap = buffer ? ::CreateCompatibleBitmap(target, client.right, client.bottom) : nullptr;
            HGDIOBJ previous = bitmap ? ::SelectObject(buffer, bitmap) : nullptr;
            if (previous && previous != HGDI_ERROR) {
                const auto colors = nskry::plugin_ui::Colors(nskry::plugin_ui::preferences.theme);
                ::FillRect(buffer, &client, nskry::plugin_ui::Brush(buffer, colors.window));
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
            if (nskry::input::PluginMatches(L"pan_x",L"Shift+Wheel",0,nskry::input::Kind::Wheel)) Pan(delta / 3.0, 0);
            else if (nskry::input::PluginMatches(L"pan_y",L"Alt+Wheel",0,nskry::input::Kind::Wheel)) Pan(0, delta / 3.0);
            else if(nskry::input::PluginMatches(L"zoom",L"Wheel",0,nskry::input::Kind::Wheel)) ZoomAt(point, delta > 0 ? 1.12 : 1.0 / 1.12);
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
        case WM_KEYDOWN:case WM_SYSKEYDOWN:
            if (nskry::input::PluginMatches(L"copy",L"Ctrl+C",UINT(wp)) || nskry::input::PluginMatches(L"copy_alt",L"Ctrl+Insert",UINT(wp))) { CopySelected(); return 0; }
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
    HWND m_engineButton{};
    HWND m_textEdit{}, m_textLabel{}, m_copyTextButton{};
    HWND m_translateButton{};
    HFONT m_font{};
    std::unique_ptr<OcrJobResult> m_result;
    bool m_highlights{ true };
    bool m_engineAtTop{};
    bool m_windowOwnsLifetime{};
    bool m_dragSelecting{};
    POINT m_dragStart{};
    POINT m_dragCurrent{};
    std::vector<size_t> m_selected;
    double m_scale{};
    double m_panX{};
    double m_panY{};
    UINT m_dpi = 96;
    int m_textPaneWidth{};
};

void ShowResult(std::unique_ptr<OcrJobResult> result) {
    if (!result) return;
    auto* window = new (std::nothrow) OcrResultWindow(std::move(result));
    if (!window || !window->Show()) delete window;
}

LRESULT CALLBACK DispatchProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == kProgressMessage) {
        ProgressInfo info;
        { std::lock_guard lock(s_workerMutex); info = s_progressInfo; }
        if (info.generation == s_generation.load() && s_running.load()) ShowProgress(info);
        return 0;
    }
    if (message == kResultMessage) {
        std::unique_ptr<OcrJobResult> result;
        {
            std::lock_guard lock(s_pendingMutex);
            if (s_pendingResults.empty()) return 0;
            result = std::move(s_pendingResults.back());
            s_pendingResults.pop_back();
        }
        {
            std::lock_guard lock(s_workerMutex);
            if (!s_running && s_worker.joinable()) s_worker.join();
        }
        if (result && result->generation == s_generation.load()) {
            CloseProgress();
            ShowResult(std::move(result));
        }
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

extern "C" NSKRY_API int32_t NSKRY_CALL nskry_plugin_connect_ocr_layout_v1(NskryAcquireOcrV1 acquire) {
    acquireOcr = acquire;
    return acquire != nullptr;
}

extern "C" NSKRY_API int32_t NSKRY_CALL nskry_plugin_connect_text_translation_v1(
    const NskryTextTranslationServiceV1 *service) {
    if (!service || service->structSize < sizeof(*service) || service->version != 1 ||
        !service->available || !service->translate) return 0;
    s_translationService = *service;
    return 1;
}

extern "C" NSKRY_API int32_t NSKRY_CALL nskry_plugin_init(const NskryHostContext* context) {
    return context && context->structSize >= sizeof(NskryHostContext) &&
        context->apiVersion == NSKRY_PLUGIN_API_VERSION && CreateDispatchWindow() ? 1 : 0;
}

extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_execute(const NskryHostContext* context) {
    if (!context || context->structSize < sizeof(NskryHostContext) || !context->capturedBitmap || !s_dispatchWindow) return;
    int width{}, height{};
    if (!GetBitmapSize(context->capturedBitmap, width, height)) {
        if (context->showNotification) context->showNotification(nskry::plugin_ui::Text(L"The selected image is not available for text recognition.", L"所选图片不可用于文字识别。"), 2500);
        return;
    }
    auto request = std::make_unique<OcrRequest>();
    request->bitmap = CopyBitmap(context->capturedBitmap);
    request->width = width;
    request->height = height;
    request->anchor = context->capturedRegion;
    request->copyBitmapToClipboard = context->copyBitmapToClipboard;
    request->openBitmapEditor = context->openBitmapEditor;
    if (!request->bitmap) {
        if (context->showNotification) context->showNotification(nskry::plugin_ui::Text(L"Could not prepare this image for text recognition.", L"无法准备要识别的图片。"), 2500);
        return;
    }
    if (!QueueOcr(std::move(request))) {
        if (context->showNotification) context->showNotification(nskry::plugin_ui::Text(L"Could not start the text recognition worker.", L"无法启动文字识别。"), 2500);
        return;
    }
    if (context->showNotification) context->showNotification(nskry::plugin_ui::Text(L"Recognizing text…", L"正在识别文字…"), 2200);
}

extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_shutdown() {
    s_translationService = {};
    CloseProgress();
    std::thread worker;
    {
        std::lock_guard lock(s_workerMutex);
        ++s_generation;
        s_queuedRequest.reset();
        worker = std::move(s_worker);
    }
    if (worker.joinable()) worker.join();
    s_running = false;
    {
        std::lock_guard pendingLock(s_pendingMutex);
        s_pendingResults.clear();
    }
    if (s_dispatchWindow) {
        // Result windows own bitmaps and DLL callbacks. Close them on the
        // dispatch/UI thread before the host releases this plugin.
        const DWORD uiThread = ::GetWindowThreadProcessId(s_dispatchWindow, nullptr);
        ::EnumThreadWindows(uiThread, [](HWND window, LPARAM) -> BOOL {
            wchar_t name[64]{};
            ::GetClassNameW(window, name, ARRAYSIZE(name));
            if (wcscmp(name, L"NskryOcrResult") == 0) ::DestroyWindow(window);
            return TRUE;
        }, 0);
        ::DestroyWindow(s_dispatchWindow);
        s_dispatchWindow = nullptr;
    }
    // Registered classes also retain DLL procedure pointers. Remove them only
    // after their windows/worker have gone, before the host can FreeLibrary.
    for (const auto* name : {L"NskryOcrResult", L"NskryOcrProgress", L"NskryOcrDispatch"})
        ::UnregisterClassW(name, ::GetModuleHandleW(nullptr));
}

extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_set_ui_preferences_v1(const NskryUiPreferencesV1* ui) {
    nskry::plugin_ui::SetPreferences(ui);
}
