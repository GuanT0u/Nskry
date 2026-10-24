#include "core/json.h"
#include "nskry_actions.h"
#include "nskry_plugin.h"
#include "nskry_text_translation.h"
#include "translation_policy.h"
#include "ocr_wait.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <shlobj.h>
#include <thread>
#include <vector>
#include <wincrypt.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

namespace {
using namespace nskry::translate;
HWND window{}, sourceEdit{}, resultEdit{}, packEdit{}, directionChoice{}, statusLabel{};
HMODULE module{};
bool closing{};
constexpr wchar_t Key[] = L"Software\\Nskry\\Plugins\\nskry-translate";
struct Task {
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread thread;
    std::atomic<bool> done{false};
    std::wstring result, error;
    std::wstring successMessage = L"Done. Experimental offline translation; review the result.";
    bool ocr{};
    ~Task() {
        SetEvent(cancel);
        if (thread.joinable())
            thread.join();
        if (cancel)
            CloseHandle(cancel);
    }
};
std::unique_ptr<Task> task;
bool BeginTranslation();
std::wstring Text(HWND control) {
    const auto size = GetWindowTextLengthW(control);
    std::wstring text(size + 1, 0);
    GetWindowTextW(control, text.data(), int(text.size()));
    text.resize(size);
    return text;
}
std::filesystem::path PluginDirectory() {
    wchar_t path[32768]{};
    const auto count = GetModuleFileNameW(module, path, 32768);
    if (!count || count >= 32768)
        winrt::throw_last_error();
    return std::filesystem::path(path).parent_path();
}
struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct ProtectedBlob {
    DATA_BLOB value{};
    ~ProtectedBlob() {
        if (value.pbData)
            LocalFree(value.pbData);
    }
};
std::wstring RunOffline(Task &work, const std::wstring &text, const std::wstring &root, bool toChinese) {
    PWSTR local{};
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local));
    std::filesystem::path parent;
    try {
        parent = std::filesystem::path(local) / L"Nskry" / L"cache" / L"translation";
    } catch (...) {
        CoTaskMemFree(local);
        throw;
    }
    CoTaskMemFree(local);
    std::filesystem::create_directories(parent);
    GUID id{};
    winrt::check_hresult(CoCreateGuid(&id));
    wchar_t name[40]{};
    StringFromGUID2(id, name, 40);
    const auto directory = parent / name;
    if (!std::filesystem::create_directory(directory))
        winrt::throw_hresult(E_FAIL);
    struct Files {
        std::filesystem::path root;
        ~Files() {
            DeleteFileW((root / L"input.txt").c_str());
            DeleteFileW((root / L"reply.json").c_str());
            RemoveDirectoryW(root.c_str());
        }
    } files{directory};
    std::string utf8;
    if (!nskry::json::ToUtf8(text, utf8))
        winrt::throw_hresult(E_INVALIDARG);
    {
        DATA_BLOB plain{DWORD(utf8.size()), reinterpret_cast<BYTE *>(utf8.data())};
        ProtectedBlob encrypted;
        if (!CryptProtectData(&plain, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                              &encrypted.value))
            winrt::throw_last_error();
        std::ofstream input(directory / L"input.txt", std::ios::binary);
        input.write(reinterpret_cast<const char *>(encrypted.value.pbData), encrypted.value.cbData);
        if (!input)
            winrt::throw_hresult(E_FAIL);
    }
    const auto executable = PluginDirectory() / L"engine" / L"nskry-translate-worker.exe";
    const auto quote = [](const std::wstring &value) {
        if (value.find(L'"') != value.npos)
            winrt::throw_hresult(E_INVALIDARG);
        // Windows argv quoting also escapes trailing backslashes.
        std::wstring result = L"\"" + value;
        for (auto i = value.rbegin(); i != value.rend() && *i == L'\\'; ++i)
            result += L'\\';
        return result + L"\"";
    };
    auto command = quote(executable.wstring()) + L" " + quote(root) + (toChinese ? L" en-zh " : L" zh-en ") +
                   quote((directory / L"input.txt").wstring()) + L" " +
                   quote((directory / L"reply.json").wstring());
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    if (!job.value)
        winrt::throw_last_error();
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    // Measured cold-load private peak is ~650MiB despite 77MiB INT8 weights.
    // Keep a hard bound with headroom for a full paragraph; never infer RAM
    // demand from download size. The process exits after this single request.
    limits.ProcessMemoryLimit = 1024ULL * 1024 * 1024;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        winrt::throw_last_error();
    SIZE_T attributeBytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<uint8_t> storage(attributeBytes);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
        winrt::throw_last_error();
    struct Attributes {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~Attributes() { DeleteProcThreadAttributeList(value); }
    } attributeGuard{attributes};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job.value, sizeof(HANDLE),
                                   nullptr, nullptr))
        winrt::throw_last_error();
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS |
                            EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process))
        winrt::throw_last_error();
    Handle child{process.hProcess}, thread{process.hThread};
    if (ResumeThread(thread.value) == DWORD(-1)) {
        TerminateProcess(child.value, ERROR_CANCELLED); // Only the child just created by this task.
        WaitForSingleObject(child.value, 2000);
        winrt::throw_hresult(E_FAIL);
    }
    HANDLE signals[]{work.cancel, child.value};
    const auto wait = WaitForMultipleObjects(2, signals, FALSE, TaskTimeoutMs);
    if (wait != WAIT_OBJECT_0 + 1) {
        TerminateJobObject(job.value, ERROR_CANCELLED);
        WaitForSingleObject(child.value, 2000);
        winrt::throw_hresult(HRESULT_FROM_WIN32(wait == WAIT_OBJECT_0 ? ERROR_CANCELLED : ERROR_TIMEOUT));
    }
    const auto reply = directory / L"reply.json";
    DWORD exitCode{};
    if (!GetExitCodeProcess(child.value, &exitCode))
        winrt::throw_last_error();
    if (exitCode)
        winrt::throw_hresult(HRESULT_FROM_WIN32(exitCode));
    if (std::filesystem::file_size(reply) > MaxReplyBytes)
        winrt::throw_hresult(E_BOUNDS);
    std::ifstream replyStream(reply, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(replyStream),
                                     std::istreambuf_iterator<char>()};
    DATA_BLOB encrypted{DWORD(bytes.size()), const_cast<BYTE *>(bytes.data())};
    ProtectedBlob plain;
    if (!CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &plain.value))
        winrt::throw_last_error();
    // JSON worker replies use ASCII escapes; no locale-dependent decoding.
    std::wstring json(plain.value.pbData, plain.value.pbData + plain.value.cbData);
    nskry::json::Value value;
    bool ok{};
    std::wstring output;
    if (!nskry::json::Parse(json, value) || !value.GetBool(L"ok", ok) || !ok ||
        !value.GetString(L"text", output))
        winrt::throw_hresult(E_FAIL);
    return output;
}
void FinishTask() {
    if (!task || !task->done.load())
        return;
    const bool canceled = WaitForSingleObject(task->cancel, 0) == WAIT_OBJECT_0;
    const bool autoTranslate = task->ocr && task->error.empty() && !canceled && !closing;
    const std::wstring message =
        canceled ? L"Canceled. No online fallback was used."
                 : (task->error.empty() ? task->successMessage : task->error);
    SetWindowTextW(statusLabel, message.c_str());
    if (!canceled)
        SetWindowTextW(task->ocr ? sourceEdit : resultEdit, task->result.c_str());
    task.reset();
    KillTimer(window, 1);
    EnableWindow(GetDlgItem(window, 1), TRUE);
    EnableWindow(sourceEdit, TRUE);
    EnableWindow(packEdit, TRUE);
    EnableWindow(directionChoice, TRUE);
    if (closing)
        DestroyWindow(window);
    else if (autoTranslate)
        BeginTranslation();
}
template <class Work> void Start(bool ocr, Work work) {
    if (task)
        return;
    task = std::make_unique<Task>();
    if (!task->cancel) {
        const auto error = GetLastError();
        task.reset();
        winrt::throw_hresult(HRESULT_FROM_WIN32(error));
    }
    task->ocr = ocr;
    auto *state = task.get();
    try {
        state->thread = std::thread([state, work = std::move(work)] {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Apartment {
                    ~Apartment() { winrt::uninit_apartment(); }
                } apartment;
                state->result = work(*state);
            } catch (const winrt::hresult_error &error) {
                state->error = (state->ocr ? L"Screenshot text recognition failed (code " : L"Offline translation failed (code ") +
                    std::to_wstring(error.code().value) + (state->ocr
                        ? L"). Check Windows OCR languages/image size, or paste source text."
                        : L"). Check the translation engine, language pack and input limits.");
            } catch (...) {
                state->error = L"Task failed or canceled. Check the engine, language pack and input limits.";
            }
            state->done.store(true);
        });
    } catch (...) {
        task.reset();
        throw;
    }
    EnableWindow(GetDlgItem(window, 1), FALSE);
    EnableWindow(sourceEdit, FALSE);
    EnableWindow(packEdit, FALSE);
    EnableWindow(directionChoice, FALSE);
    SetWindowTextW(resultEdit, L""); // Never offer a stale translation for a new request.
    SetWindowTextW(statusLabel, ocr ? L"Recognizing locally…" : L"Translating offline…");
    if (!SetTimer(window, 1, 50, nullptr)) {
        const auto error = GetLastError();
        task.reset();
        EnableWindow(GetDlgItem(window, 1), TRUE);
        EnableWindow(sourceEdit, TRUE);
        EnableWindow(packEdit, TRUE);
        EnableWindow(directionChoice, TRUE);
        winrt::throw_hresult(HRESULT_FROM_WIN32(error ? error : ERROR_NOT_ENOUGH_MEMORY));
    }
}
bool BeginTranslation() {
    if (task || closing)
        return false;
    SetWindowTextW(resultEdit, L"");
    auto text = Text(sourceEdit), root = Text(packEdit);
    if (ValidateText(text) != InputError::None) {
        SetWindowTextW(statusLabel, L"No valid text found. Edit the source (maximum 16000 UTF-16 units).");
        return false;
    }
    const auto selection = SendMessageW(directionChoice, CB_GETCURSEL, 0, 0);
    bool toChinese = selection == 1;
    if (selection == 0) {
        const auto language = DetectSourceLanguage(text);
        if (language != SourceLanguage::Chinese && language != SourceLanguage::English) {
            SetWindowTextW(statusLabel, L"Cannot choose an English/Chinese language pack for this text. "
                                        L"Check the source or choose a direction manually.");
            return false;
        }
        toChinese = language == SourceLanguage::English;
    }
    Start(false, [text, root, toChinese](Task &work) { return RunOffline(work, text, root, toChinese); });
    task->successMessage = toChinese ? L"English → 中文: done. Review the experimental translation."
                                     : L"中文 → English: done. Review the experimental translation.";
    SetWindowTextW(statusLabel, toChinese ? L"English → 中文: translating offline…"
                                          : L"中文 → English: translating offline…");
    return true;
}
std::wstring Recognize(Task &work, const std::vector<uint8_t> &pixels, int width, int height) {
    using namespace winrt::Windows;
    auto engine = Media::Ocr::OcrEngine::TryCreateFromLanguage(Globalization::Language(L"zh-Hans"));
    if (!engine)
        engine = Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine || width > int(Media::Ocr::OcrEngine::MaxImageDimension()) ||
        height > int(Media::Ocr::OcrEngine::MaxImageDimension()))
        winrt::throw_hresult(E_BOUNDS);
    Storage::Streams::DataWriter writer;
    writer.WriteBytes(pixels);
    auto bitmap = Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(
        writer.DetachBuffer(), Graphics::Imaging::BitmapPixelFormat::Bgra8, width, height,
        Graphics::Imaging::BitmapAlphaMode::Ignore);
    auto operation = engine.RecognizeAsync(bitmap);
    WaitForOcr(operation, work.cancel, TaskTimeoutMs);
    std::wstring text;
    for (const auto &line : operation.GetResults().Lines()) {
        if (!text.empty())
            text += L"\r\n";
        text += line.Text();
    }
    return text;
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    try {
        if (message == WM_SIZE) {
            RECT r{};
            GetClientRect(hwnd, &r);
            const int width = (std::max)(300L, r.right), height = (std::max)(200L, r.bottom);
            MoveWindow(packEdit, 8, 8, width - 160, 26, TRUE);
            MoveWindow(GetDlgItem(hwnd, 4), width - 144, 8, 136, 26, TRUE);
            MoveWindow(sourceEdit, 8, 42, (width - 24) / 2, height - 122, TRUE);
            MoveWindow(resultEdit, width / 2 + 4, 42, width / 2 - 12, height - 122, TRUE);
            MoveWindow(directionChoice, 8, height - 72, 120, 120, TRUE);
            MoveWindow(GetDlgItem(hwnd, 1), 136, height - 72, 100, 26, TRUE);
            MoveWindow(GetDlgItem(hwnd, 2), 244, height - 72, 100, 26, TRUE);
            MoveWindow(GetDlgItem(hwnd, 3), 352, height - 72, 110, 26, TRUE);
            MoveWindow(statusLabel, 8, height - 38, width - 16, 32, TRUE);
            return 0;
        }
        if (message == WM_GETMINMAXINFO) {
            auto *limits = reinterpret_cast<MINMAXINFO *>(lp);
            limits->ptMinTrackSize = {500, 300};
            return 0;
        }
        if (message == WM_TIMER) {
            FinishTask();
            return 0;
        }
        if (message == WM_CLOSE) {
            if (task) {
                closing = true;
                SetEvent(task->cancel);
                EnableWindow(hwnd, FALSE);
            } else
                DestroyWindow(hwnd);
            return 0;
        }
        if (message == WM_DESTROY) {
            // An owner can destroy this window without sending WM_CLOSE.
            // Never leave an orphaned task/timer waiting for a vanished UI.
            KillTimer(hwnd, 1);
            task.reset();
            return 0;
        }
        if (message == WM_NCDESTROY) {
            window = nullptr;
            return DefWindowProcW(hwnd, message, wp, lp);
        }
        if (message == WM_COMMAND) {
            if (LOWORD(wp) == 1 && !task) {
                BeginTranslation();
            }
            if (LOWORD(wp) == 2 && task)
                SetEvent(task->cancel);
            if (LOWORD(wp) == 3) {
                SendMessageW(resultEdit, EM_SETSEL, 0, -1);
                SendMessageW(resultEdit, WM_COPY, 0, 0);
            }
            if (LOWORD(wp) == 4) {
                const auto root = Text(packEdit);
                if (RegSetKeyValueW(HKEY_CURRENT_USER, Key, L"ModelRoot", REG_SZ, root.c_str(),
                                    DWORD((root.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
                    SetWindowTextW(statusLabel, L"Could not save language-pack path.");
                else
                    SetWindowTextW(statusLabel,
                                   L"Language-pack path saved. No download or model loading was started.");
            }
            return 0;
        }
    } catch (...) {
        SetWindowTextW(statusLabel, L"Operation failed; no online fallback was used.");
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
void Show(HWND owner) {
    if (window) {
        SetForegroundWindow(window);
        return;
    }
    WNDCLASSW wc{};
    wc.hInstance = module;
    wc.lpfnWndProc = Procedure;
    wc.lpszClassName = L"NskryTranslateV1";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    window = CreateWindowExW(0, wc.lpszClassName, L"Nskry — Translation (experimental offline MVP)",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 560, owner, nullptr,
                             module, nullptr);
    if (!window)
        winrt::throw_last_error();
    const auto child = [](const wchar_t *type, const wchar_t *text, DWORD style, int id) {
        return CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window,
                               reinterpret_cast<HMENU>(INT_PTR(id)), module, nullptr);
    };
    sourceEdit = child(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 10);
    resultEdit = child(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, 11);
    // Allow over-limit paste so validation rejects it explicitly, rather than
    // silently turning a truncated paste into a successful translation.
    SendMessageW(sourceEdit, EM_SETLIMITTEXT, MaxTextUnits * 2, 0);
    SendMessageW(resultEdit, EM_SETLIMITTEXT, MaxReplyBytes, 0);
    packEdit = child(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 12);
    directionChoice = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 13);
    SendMessageW(directionChoice, CB_ADDSTRING, 0, LPARAM(L"Auto (中/En)"));
    SendMessageW(directionChoice, CB_ADDSTRING, 0, LPARAM(L"English → 中文"));
    SendMessageW(directionChoice, CB_ADDSTRING, 0, LPARAM(L"中文 → English"));
    SendMessageW(directionChoice, CB_SETCURSEL, 0, 0);
    child(L"BUTTON", L"Translate", WS_TABSTOP, 1);
    child(L"BUTTON", L"Cancel", WS_TABSTOP, 2);
    child(L"BUTTON", L"Copy result", WS_TABSTOP, 3);
    child(L"BUTTON", L"Save pack path", WS_TABSTOP, 4);
    statusLabel = child(L"STATIC",
                        L"Select a region to recognize and translate automatically. You can edit the source "
                        L"and translate again.",
                        0, 14);
    wchar_t saved[32768]{};
    DWORD bytes = sizeof(saved);
    auto root = (PluginDirectory() / L"languages").wstring();
    if (RegGetValueW(HKEY_CURRENT_USER, Key, L"ModelRoot", RRF_RT_REG_SZ, nullptr, saved, &bytes) ==
        ERROR_SUCCESS)
        root = saved;
    SetWindowTextW(packEdit, root.c_str());
    closing = false;
    SendMessageW(window, WM_SIZE, 0, 0);
    ShowWindow(window, SW_SHOW);
}
NskryPluginInfo info{sizeof(info),
                     NSKRY_PLUGIN_API_VERSION,
                     L"Translation",
                     L"nskry-translate",
                     L"0.2.1",
                     L"Nskry Team",
                     L"Experimental on-demand offline English/Chinese translation.",
                     nullptr,
                     NSKRY_CAP_TOOLBAR_ACTION | NSKRY_CAP_POST_CAPTURE,
                     {}};
} // namespace
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
NSKRY_API const NskryPluginInfo *NSKRY_CALL nskry_plugin_info() { return &info; }
NSKRY_API int32_t NSKRY_CALL nskry_plugin_init(const NskryHostContext *) { return 1; }
extern "C" NSKRY_API int32_t NSKRY_CALL nskry_plugin_show_settings_v1(HWND owner) {
    try {
        Show(owner);
        return 1;
    } catch (...) {
        return 0;
    }
}
extern "C" NSKRY_API int32_t NSKRY_CALL nskry_plugin_translate_text_v1(const NskryTextTranslationV1 *request,
                                                                       NskryTranslationResultV1 *result) {
    if ((result && (result->structSize < sizeof(*result) || result->version != 1)) ||
        !NskryValidTranslation(request)) {
        NskryTranslationError(result, L"Translation requires nonempty valid text within 16000 UTF-16 units.");
        return NSKRY_TRANSLATION_INVALID;
    }
    try {
        Show(nullptr); // Independent window: closing OCR must not destroy this task.
        if (task || closing) {
            NskryTranslationError(
                result, L"A translation task is active. Cancel it or wait before sending more text.");
            return NSKRY_TRANSLATION_BUSY;
        }
        const std::wstring copied(request->text, request->textLength);
        SetWindowTextW(sourceEdit, copied.c_str());
        SendMessageW(directionChoice, CB_SETCURSEL, 0, 0);
        // OCR results are topmost; the translation must not open hidden behind them.
        SetWindowPos(window,
                     request->owner && (GetWindowLongPtrW(request->owner, GWL_EXSTYLE) & WS_EX_TOPMOST)
                         ? HWND_TOPMOST
                         : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        if (!BeginTranslation()) {
            NskryTranslationError(result, L"Automatic English/Chinese direction is unavailable for this "
                                          L"source. Choose a direction in the translation window.");
            return NSKRY_TRANSLATION_INVALID;
        }
        return NSKRY_TRANSLATION_ACCEPTED;
    } catch (...) {
        NskryTranslationError(result, L"Could not start the translation task. No online fallback was used.");
        return NSKRY_TRANSLATION_FAILED;
    }
}
NSKRY_API void NSKRY_CALL nskry_plugin_execute(const NskryHostContext *context) {
    if (!context || !context->capturedBitmap)
        return;
    try {
        Show(context->mainHwnd);
        if (task)
            return;
        SendMessageW(directionChoice, CB_SETCURSEL, 0, 0);
        BITMAP bitmap{};
        if (!GetObjectW(context->capturedBitmap, sizeof(bitmap), &bitmap) || bitmap.bmWidth < 1 ||
            bitmap.bmHeight < 1 || bitmap.bmWidth > 4096 || bitmap.bmHeight > 4096 ||
            uint64_t(bitmap.bmWidth) * bitmap.bmHeight > 4194304)
            winrt::throw_hresult(E_BOUNDS);
        std::vector<uint8_t> pixels(size_t(bitmap.bmWidth) * bitmap.bmHeight * 4);
        BITMAPINFO description{};
        description.bmiHeader = {sizeof(BITMAPINFOHEADER), bitmap.bmWidth, -bitmap.bmHeight, 1, 32, BI_RGB};
        HDC dc = GetDC(nullptr);
        if (!dc)
            winrt::throw_last_error();
        const auto rows = GetDIBits(dc, context->capturedBitmap, 0, bitmap.bmHeight, pixels.data(),
                                    &description, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        if (rows != bitmap.bmHeight)
            winrt::throw_hresult(E_FAIL);
        Start(true, [pixels = std::move(pixels), w = bitmap.bmWidth, h = bitmap.bmHeight](Task &work) {
            return Recognize(work, pixels, w, h);
        });
    } catch (...) {
        if (statusLabel)
            SetWindowTextW(statusLabel, L"Could not recognize the image. Enter source text manually.");
    }
}
NSKRY_API void NSKRY_CALL nskry_plugin_shutdown() {
    task.reset();
    if (window) {
        KillTimer(window, 1);
        DestroyWindow(window);
    }
    UnregisterClassW(L"NskryTranslateV1", module);
}
