#include "../../common/native_ui.h"
#include "core/json.h"
#include "nskry_actions.h"
#include "nskry_plugin.h"
#include "nskry_text_translation.h"
#include "nskry_image_recognition.h"
#include "image_recognition.h"
#include "online_settings.h"
#include "online_translation.h"
#include "translation_policy.h"
#include "offline_process.h"
#include <atomic>
#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#include <winrt/Windows.Foundation.h>

namespace {
using namespace nskry::translate;
HWND window{}, sourceEdit{}, resultEdit{}, packEdit{}, directionChoice{}, statusLabel{}, providerChoice{};
HMODULE module{};
HFONT uiFont{};
bool closing{};
bool starting{}, uploadCanceled{};
NskryImageRecognitionServiceV1 recognitionService{};
bool useOcrPlugin{};
bool hasOfflineEngine{};
HWND taskDispatch{}; // Exists only while a task needs completion delivery.
void FinishTask();
void CloseTaskDispatch() {
    if (taskDispatch) {
        KillTimer(taskDispatch, 1);
        DestroyWindow(taskDispatch);
        taskDispatch = nullptr;
    }
}
LRESULT CALLBACK TaskProcedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_TIMER && wp == 1) {
        try {
            FinishTask();
        } catch (...) {
            SetWindowTextW(statusLabel, nskry::plugin_ui::Text(L"Could not deliver the task result. Close or cancel to retry cleanup.", L"无法显示任务结果。可关闭或取消以重试清理。"));
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
void OpenTaskDispatch() {
    WNDCLASSW wc{};
    wc.hInstance = module;
    wc.lpfnWndProc = TaskProcedure;
    wc.lpszClassName = L"NskryTranslateTaskV1";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        winrt::throw_last_error();
    taskDispatch = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, nullptr);
    if (!taskDispatch)
        winrt::throw_last_error();
    if (!SetTimer(taskDispatch, 1, 50, nullptr)) {
        const auto error = GetLastError();
        CloseTaskDispatch();
        winrt::throw_hresult(HRESULT_FROM_WIN32(error ? error : ERROR_NOT_ENOUGH_MEMORY));
    }
}
constexpr wchar_t Key[] = L"Software\\Nskry\\Plugins\\nskry-translate";
struct Task {
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread thread;
    std::atomic<bool> done{false};
    std::wstring result, error;
    std::wstring successMessage = L"Done. Experimental offline translation; review the result.";
    bool ocr{};
    bool online{};
    unsigned parts{};
    uint64_t recognitionTicket{};
    std::vector<wchar_t> recognitionBuffer; // Allocated once for an opt-in OCR task, not each timer tick.
    ~Task() {
        SetEvent(cancel);
        if (thread.joinable())
            thread.join();
        if (recognitionTicket && recognitionService.release)
            recognitionService.release(recognitionTicket); // UI thread, before any provider DLL unload.
        if (cancel)
            CloseHandle(cancel);
    }
};
std::unique_ptr<Task> task;
bool BeginTranslation();
std::optional<Provider> SelectedProvider() {
    const auto selected = SendMessageW(providerChoice, CB_GETCURSEL, 0, 0);
    if (selected < 0 || selected > static_cast<int>(Provider::MyMemory) ||
        (selected == 0 && !hasOfflineEngine))
        return std::nullopt;
    return static_cast<Provider>(selected);
}
const wchar_t *ChooseEngineHint() {
    return nskry::plugin_ui::Text(L"Choose an online engine, then click Translate. MyMemory needs no setup.",
                                  L"请选择在线引擎，再点击「翻译」。MyMemory 无需配置。");
}
void RefreshProviderControls() {
    const auto provider = SelectedProvider();
    const bool offline = provider == Provider::Offline;
    EnableWindow(packEdit, !task && offline);
    EnableWindow(GetDlgItem(window, 4), !task && offline);
    EnableWindow(GetDlgItem(window, 21), !task && provider.has_value());
    const bool hasResult = !task && GetWindowTextLengthW(resultEdit) > 0;
    EnableWindow(GetDlgItem(window, 3), hasResult);
    EnableWindow(GetDlgItem(window, 5), hasResult);
    // An older Core must still let a persisted opt-in be switched OFF.
    EnableWindow(GetDlgItem(window, 22), !task && (recognitionService.available || useOcrPlugin));
    const bool available = recognitionService.available && recognitionService.available(L"nskry-ocr");
    SetWindowTextW(GetDlgItem(window, 23),
                   !recognitionService.available ? L"OCR integration requires an updated Core. Built-in "
                                                   L"recognition still works when this option is off."
                   : available ? L"OCR plugin available. Uses its selected mode; review recognition before "
                                 L"relying on a translation."
                               : L"For another recognition option, install/enable an updated OCR plugin. "
                                 L"Translation also works without it.");
}
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
void FinishTask() {
    if (task && task->recognitionTicket && !task->done.load()) {
        if (WaitForSingleObject(task->cancel, 0) == WAIT_OBJECT_0)
            recognitionService.cancel(task->recognitionTicket);
        auto &text = task->recognitionBuffer;
        wchar_t error[256]{};
        NskryImageTextV1 output{sizeof(output), 1, text.data(), uint32_t(text.size()), 0, error, 256};
        const auto status = recognitionService.poll(task->recognitionTicket, &output);
        if (status == NSKRY_IMAGE_PENDING)
            return;
        if (status == NSKRY_IMAGE_SUCCESS && ValidateText(text.data()) == InputError::None)
            task->result = text.data();
        else if (status == NSKRY_IMAGE_CANCELED)
            SetEvent(task->cancel);
        else
            task->error = error[0] ? error
                                   : L"OCR plugin unavailable or failed. No fallback was used. Uncheck the "
                                     L"OCR option to use built-in recognition.";
        task->done.store(true);
    }
    if (!task || !task->done.load())
        return;
    if (!window) {
        // Owner destruction must not wait for provider/worker cancellation on
        // the UI thread. Keep one bounded task until it has actually finished.
        task.reset();
        CloseTaskDispatch();
        closing = false;
        return;
    }
    const bool canceled = WaitForSingleObject(task->cancel, 0) == WAIT_OBJECT_0;
    const bool autoTranslate = task->ocr && task->error.empty() && !canceled && !closing;
    std::wstring message =
        canceled ? (task->online ? nskry::plugin_ui::Text(
                                     L"Canceled. The server may already have processed the submitted text.",
                                     L"已取消。服务端可能已处理发送的文字。")
                                 : nskry::plugin_ui::Text(L"Canceled.", L"已取消。"))
                 : (task->error.empty() ? task->successMessage : task->error);
    if (!canceled && task->error.empty() && task->parts > 1)
        message += std::wstring(nskry::plugin_ui::Text(L" (", L"（")) + std::to_wstring(task->parts) +
                   nskry::plugin_ui::Text(L" bounded parts)", L" 段）");
    SetWindowTextW(statusLabel, message.c_str());
    if (!canceled)
        SetWindowTextW(task->ocr ? sourceEdit : resultEdit, task->result.c_str());
    task.reset();
    CloseTaskDispatch();
    EnableWindow(GetDlgItem(window, 1), TRUE);
    EnableWindow(sourceEdit, TRUE);
    EnableWindow(packEdit, TRUE);
    EnableWindow(directionChoice, TRUE);
    EnableWindow(providerChoice, TRUE);
    EnableWindow(GetDlgItem(window, 21), TRUE);
    RefreshProviderControls();
    if (closing)
        DestroyWindow(window);
    else if (autoTranslate)
        BeginTranslation();
}
template <class Work> void Start(bool ocr, Work work, bool online = false, uint64_t recognitionTicket = 0) {
    if (task)
        return;
    task = std::make_unique<Task>();
    if (!task->cancel) {
        const auto error = GetLastError();
        task.reset();
        winrt::throw_hresult(HRESULT_FROM_WIN32(error));
    }
    task->ocr = ocr;
    task->online = online;
    task->recognitionTicket = recognitionTicket;
    auto *state = task.get();
    try {
        OpenTaskDispatch(); // Establish completion ownership before starting work.
        if (recognitionTicket)
            state->recognitionBuffer.resize(NSKRY_IMAGE_MAX_TEXT + 1);
        if (!recognitionTicket)
            state->thread = std::thread([state, work = std::move(work), ui = nskry::plugin_ui::preferences] {
                nskry::plugin_ui::SetPreferences(&ui);
                try {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    struct Apartment {
                        ~Apartment() {
                            winrt::clear_factory_cache();
                            winrt::uninit_apartment();
                        }
                    } apartment;
                    state->result = work(*state);
                } catch (const TranslationFailure &error) {
                    state->error = error.message;
                } catch (const std::wstring &error) {
                    state->error = error; // Only locally constructed, sanitized provider/transport messages.
                } catch (const winrt::hresult_error &error) {
                    if (error.code() == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
                        state->error = nskry::plugin_ui::Text(
                            L"The task timed out. Try a smaller region or shorter text.",
                            L"任务超时。请缩小截图范围或减少原文字数。");
                    } else {
                        state->error =
                            (state->ocr ? L"Screenshot text recognition failed (code "
                                        : L"Translation failed (code ") +
                            std::to_wstring(error.code().value) +
                            (state->ocr
                                 ? L"). Check the local OCR runtime/image size, or paste source text."
                                 : L"). Check the translation engine, language pack and input limits.");
                    }
                } catch (...) {
                    state->error = nskry::plugin_ui::Text(
                        L"Task failed. Check the engine, language pack and input limits.",
                        L"任务失败。请检查引擎、语言包和输入长度。");
                }
                state->done.store(true);
            });
    } catch (...) {
        task.reset();
        CloseTaskDispatch();
        throw;
    }
    EnableWindow(GetDlgItem(window, 1), FALSE);
    EnableWindow(sourceEdit, FALSE);
    EnableWindow(packEdit, FALSE);
    EnableWindow(directionChoice, FALSE);
    EnableWindow(providerChoice, FALSE);
    EnableWindow(GetDlgItem(window, 21), FALSE);
    EnableWindow(GetDlgItem(window, 22), FALSE);
    EnableWindow(GetDlgItem(window, 4), FALSE);
    SetWindowTextW(resultEdit, L""); // Never offer a stale translation for a new request.
    EnableWindow(GetDlgItem(window, 3), FALSE);
    EnableWindow(GetDlgItem(window, 5), FALSE);
    SetWindowTextW(statusLabel, ocr      ? nskry::plugin_ui::Text(L"Recognizing locally…", L"正在本地识别…")
                                : online ? nskry::plugin_ui::Text(L"Translating online…", L"正在在线翻译…")
                                         : nskry::plugin_ui::Text(L"Translating offline…", L"正在离线翻译…"));
}
bool BeginTranslation() {
    if (task || closing || starting)
        return false;
    starting = true;
    uploadCanceled = false;
    struct Starting {
        ~Starting() {
            starting = false;
        }
    } guard;
    SetWindowTextW(resultEdit, L"");
    RefreshProviderControls();
    const auto chosen = SelectedProvider();
    if (!chosen) {
        SetWindowTextW(statusLabel, ChooseEngineHint());
        return false;
    }
    auto text = Text(sourceEdit), root = Text(packEdit);
    if (ValidateText(text) != InputError::None) {
        SetWindowTextW(statusLabel, nskry::plugin_ui::Text(L"No valid text found. Edit the source (maximum 16000 UTF-16 units).", L"未找到有效文字。请编辑原文，最多 16000 个 UTF-16 代码单元。"));
        return false;
    }
    const auto selection = SendMessageW(directionChoice, CB_GETCURSEL, 0, 0);
    bool toChinese = selection == 1;
    if (selection == 0) {
        const auto language = DetectSourceLanguage(text);
        if (language != SourceLanguage::Chinese && language != SourceLanguage::English) {
            SetWindowTextW(statusLabel, nskry::plugin_ui::Text(
                L"Could not detect Chinese or English. Review the source or choose a direction.",
                L"无法识别中英文方向。请检查原文或手动选择方向。"));
            return false;
        }
        toChinese = language == SourceLanguage::English;
    }
    const auto provider = *chosen;
    if (provider == Provider::Offline) {
        Start(false, [text, root, toChinese](Task &work) {
            auto result = RunOffline(PluginDirectory(), work.cancel, text, root, toChinese);
            work.parts = result.parts;
            return std::move(result.text);
        });
    } else {
        OnlineConfig config;
        std::wstring error;
        if (provider != Provider::MyMemory && !LoadOnlineConfig(provider, config, error)) {
            SetWindowTextW(statusLabel, error.c_str());
            return false;
        }
        if (provider != Provider::MyMemory && config.endpoint.empty()) {
            SetWindowTextW(statusLabel, nskry::plugin_ui::Text(
                L"Open Configure engine to enter the provider's HTTPS endpoint.",
                L"请打开「配置引擎」，填写服务提供的 HTTPS 端点。"));
            return false;
        }
        error = ValidateOnlineConfig(provider, config);
        if (!error.empty()) {
            SetWindowTextW(statusLabel, error.c_str());
            return false;
        }
        if (provider == Provider::MyMemory) {
            try {
                SplitFreeOnlineText(text); // Validate before asking permission or sending anything.
            } catch (const std::wstring &error) {
                SetWindowTextW(statusLabel, error.c_str());
                return false;
            }
            config.endpoint = L"https://api.mymemory.translated.net/get";
        }
        const auto prompt =
            nskry::plugin_ui::Text(L"Send the source text shown on the left (", L"将左侧显示的原文（") + std::to_wstring(text.size()) +
            nskry::plugin_ui::Text(L" UTF-16 units) to:\r\n", L" 个 UTF-16 代码单元）发送至：\r\n") + config.endpoint +
            nskry::plugin_ui::Text(
                L"\r\n\r\nThe request contains this text and language direction, plus the configured model, "
                L"translation instructions and credentials where applicable. The screenshot and previous tasks are not sent. "
                L"This provider may charge fees, retain content or share it under its own policy. "
                L"Canceling cannot recall text already sent. No automatic retry/fallback is used.\r\n\r\n",
                L"\r\n\r\n请求将发送这段文字和翻译方向，以及适用的模型、翻译指令和凭据。截图与历史任务内容会留在本机。"
                L"服务商可能收费，并按自身政策留存或共享内容。取消无法撤回已发送的文字。请求不会自动重试或切换服务。\r\n\r\n") +
            (provider == Provider::MyMemory
                 ? std::wstring(nskry::plugin_ui::Text(L"MyMemory is a third-party free service with quotas (currently 5000 "
                                L"characters/day anonymously). "
                                L"Its terms describe long-term storage and third-party processing. Text is "
                                L"sent in URL query parameters. "
                                L"Do not submit sensitive content. Terms: "
                                L"https://mymemory.translated.net/terms-and-conditions\r\n\r\n",
                                L"MyMemory 是有额度的第三方免费服务，目前匿名用户每天最多 5000 字符。条款说明了长期存储与第三方处理。"
                                L"文字通过 URL 查询参数发送，请勿提交敏感内容。条款："
                                L"https://mymemory.translated.net/terms-and-conditions\r\n\r\n"))
                 : std::wstring(
                       nskry::plugin_ui::Text(L"Check the provider's privacy and billing policy before continuing.\r\n\r\n", L"继续前请核对服务商的隐私和计费政策。\r\n\r\n"))) +
            nskry::plugin_ui::Text(L"Send this text now?", L"现在发送这段文字吗？");
        if (MessageBoxW(window, prompt.c_str(), nskry::plugin_ui::Text(L"Confirm online text upload", L"确认在线上传"),
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES ||
            !IsWindow(window) || closing) {
            uploadCanceled = true;
            if (IsWindow(window))
                SetWindowTextW(statusLabel, nskry::plugin_ui::Text(L"Upload not authorized. No online request was sent.", L"已取消上传。"));
            return false;
        }
        Start(
            false,
            [text, config, provider, toChinese](Task &work) {
                return RunOnline(provider, config, text, toChinese, work.cancel);
            },
            true);
    }
    task->successMessage = toChinese
        ? nskry::plugin_ui::Text(L"English → Chinese complete.", L"英文 → 中文翻译完成。")
        : nskry::plugin_ui::Text(L"Chinese → English complete.", L"中文 → 英文翻译完成。");
    return true;
}

void SaveResult() {
    const auto text = Text(resultEdit);
    if (text.empty() || task)
        return;
    wchar_t path[32768] = L"translation.txt";
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"UTF-8 text (*.txt)\0*.txt\0All files\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog))
        return;
    std::string utf8;
    if (!nskry::json::ToUtf8(text, utf8))
        return;
    std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    file.write(utf8.data(), std::streamsize(utf8.size()));
    file.close();
    SetWindowTextW(statusLabel, file ? nskry::plugin_ui::Text(L"Translation saved as UTF-8 text.", L"已将译文保存为 UTF-8 文本。")
                                     : nskry::plugin_ui::Text(L"Could not write the complete translation file.", L"无法完整写入译文文件。"));
}
void RefreshFont(HWND hwnd) {
    HFONT font = nskry::plugin_ui::CreateMessageFont(hwnd);
    if (!font) return;
    ::EnumChildWindows(hwnd, [](HWND child, LPARAM value) -> BOOL {
        ::SendMessageW(child, WM_SETFONT, value, TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));
    if (uiFont) ::DeleteObject(uiFont);
    uiFont = font;
}

LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    try {
        if (message == WM_ERASEBKGND)
            return nskry::plugin_ui::Background(hwnd, wp, nskry::plugin_ui::preferences.theme);
        if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT)
            return nskry::plugin_ui::ControlColors(wp, nskry::plugin_ui::preferences.theme,
                message == WM_CTLCOLOREDIT || reinterpret_cast<HWND>(lp) == resultEdit);
        if (message == WM_DPICHANGED) {
            const auto& r = *reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                           SWP_NOACTIVATE | SWP_NOZORDER);
            RefreshFont(hwnd);
            return 0;
        }
        const UINT windowDpi = ::GetDpiForWindow(hwnd);
        const UINT dpi = windowDpi ? windowDpi : 96;
        const auto dip = [dpi](int value) { return ::MulDiv(value, dpi, 96); };
        if (message == WM_SIZE) {
            RECT r{};
            GetClientRect(hwnd, &r);
            const int width = (std::max)(300, ::MulDiv(r.right, 96, dpi));
            const int height = (std::max)(200, ::MulDiv(r.bottom, 96, dpi));
            const auto move = [&](HWND child, int x, int y, int w, int h, BOOL repaint) {
                if (child) ::MoveWindow(child, dip(x), dip(y), dip(w), dip(h), repaint);
            };
            const int packRow = hasOfflineEngine ? 36 : 0;
            move(providerChoice, 8, 8, 280, 180, TRUE);
            move(GetDlgItem(hwnd, 21), 300, 8, 180, 26, TRUE);
            move(packEdit, 8, 42, width - 160, 26, TRUE);
            move(GetDlgItem(hwnd, 4), width - 144, 42, 136, 26, TRUE);
            move(GetDlgItem(hwnd, 22), 8, 42 + packRow, width - 16, 26, TRUE);
            move(GetDlgItem(hwnd, 23), 8, 70 + packRow, width - 16, 24, TRUE);
            move(GetDlgItem(hwnd, 24), 8, 100 + packRow, (width - 24) / 2, 22, TRUE);
            move(GetDlgItem(hwnd, 25), width / 2 + 4, 100 + packRow, width / 2 - 12, 22, TRUE);
            move(sourceEdit, 8, 122 + packRow, (width - 24) / 2, height - 202 - packRow, TRUE);
            move(resultEdit, width / 2 + 4, 122 + packRow, width / 2 - 12, height - 202 - packRow, TRUE);
            move(directionChoice, 8, height - 72, 120, 120, TRUE);
            move(GetDlgItem(hwnd, 1), 136, height - 72, 100, 26, TRUE);
            move(GetDlgItem(hwnd, 2), 244, height - 72, 100, 26, TRUE);
            move(GetDlgItem(hwnd, 3), 352, height - 72, 110, 26, TRUE);
            move(GetDlgItem(hwnd, 5), 470, height - 72, 110, 26, TRUE);
            move(statusLabel, 8, height - 38, width - 16, 32, TRUE);
            return 0;
        }
        if (message == WM_GETMINMAXINFO) {
            auto *limits = reinterpret_cast<MINMAXINFO *>(lp);
            limits->ptMinTrackSize = {dip(650), dip(410)};
            return 0;
        }
        if (message == WM_ACTIVATE && window)
            RefreshProviderControls();
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
            // Completion delivery belongs to the temporary message window,
            // not this UI. DLL shutdown still joins before unloading code.
            if (task) {
                closing = true;
                SetEvent(task->cancel);
                if (task->recognitionTicket)
                    recognitionService.cancel(task->recognitionTicket);
            }
            return 0;
        }
        if (message == WM_NCDESTROY) {
            if (uiFont) { ::DeleteObject(uiFont); uiFont = nullptr; }
            window = nullptr;
            sourceEdit = resultEdit = packEdit = directionChoice = statusLabel = providerChoice = nullptr;
            return DefWindowProcW(hwnd, message, wp, lp);
        }
        if (message == WM_COMMAND) {
            if (LOWORD(wp) == 22 && !task) {
                const DWORD enabled = SendMessageW(GetDlgItem(hwnd, 22), BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (enabled &&
                    (!recognitionService.available || !recognitionService.available(L"nskry-ocr"))) {
                    SendMessageW(GetDlgItem(hwnd, 22), BM_SETCHECK,
                                 useOcrPlugin ? BST_CHECKED : BST_UNCHECKED, 0);
                    SetWindowTextW(statusLabel,
                                   L"Install/enable an updated OCR plugin first. No plugin was loaded.");
                    return 0;
                }
                if (RegSetKeyValueW(HKEY_CURRENT_USER, Key, L"UseOcrPluginV1", REG_DWORD, &enabled,
                                    sizeof(enabled)) == ERROR_SUCCESS)
                    useOcrPlugin = enabled != 0;
                else {
                    SendMessageW(GetDlgItem(hwnd, 22), BM_SETCHECK,
                                 useOcrPlugin ? BST_CHECKED : BST_UNCHECKED, 0);
                    SetWindowTextW(statusLabel, nskry::plugin_ui::Text(L"Could not save recognition preference.", L"无法保存识别偏好。"));
                }
            }
            if (LOWORD(wp) == 10 && HIWORD(wp) == EN_CHANGE && !task) {
                // A previous translation must not look current after the source
                // has been edited. Programmatic OCR completion happens while
                // its task still exists and is not invalidated here.
                SetWindowTextW(resultEdit, L"");
                RefreshProviderControls();
            }
            if (LOWORD(wp) == 20 && HIWORD(wp) == CBN_SELCHANGE && !task && !starting) {
                const auto chosen = SelectedProvider();
                if (chosen) {
                    const DWORD selected = static_cast<DWORD>(*chosen);
                    const bool saved = RegSetKeyValueW(HKEY_CURRENT_USER, Key, L"PreferredProviderV1",
                                                       REG_DWORD, &selected, sizeof(selected)) == ERROR_SUCCESS;
                    SetWindowTextW(statusLabel, saved
                        ? nskry::plugin_ui::Text(L"Engine selected. Click Translate to use it.",
                                                 L"已选择引擎。点击「翻译」开始。")
                        : nskry::plugin_ui::Text(L"Engine selected for this window. Could not save preference.",
                                                 L"引擎已在当前窗口切换，无法保存偏好。"));
                } else {
                    SetWindowTextW(statusLabel, ChooseEngineHint());
                }
                RefreshProviderControls();
            }
            if (LOWORD(wp) == 1 && !task) {
                BeginTranslation();
            }
            if (LOWORD(wp) == 2 && task)
                SetEvent(task->cancel);
            if (LOWORD(wp) == 3) {
                SendMessageW(resultEdit, EM_SETSEL, 0, -1);
                SendMessageW(resultEdit, WM_COPY, 0, 0);
            }
            if (LOWORD(wp) == 5)
                SaveResult();
            if (LOWORD(wp) == 21 && !task && !starting) {
                const auto provider = SelectedProvider();
                if (provider == Provider::OpenAI || provider == Provider::LibreTranslate)
                    ShowOnlineSettings(module, hwnd, *provider);
                else if (provider)
                    SetWindowTextW(
                        statusLabel,
                        provider == Provider::Offline
                            ? nskry::plugin_ui::Text(L"Set the offline language-pack folder below.",
                                                     L"请在下方设置离线语言包目录。")
                            : nskry::plugin_ui::Text(
                                L"MyMemory requires no setup. Review its terms and confirm each upload.",
                                L"MyMemory 无需配置。请阅读其条款，并逐次确认上传。"));
            }
            if (LOWORD(wp) == 4 && SelectedProvider() == Provider::Offline) {
                const auto root = Text(packEdit);
                if (RegSetKeyValueW(HKEY_CURRENT_USER, Key, L"ModelRoot", REG_SZ, root.c_str(),
                                    DWORD((root.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
                    SetWindowTextW(statusLabel, nskry::plugin_ui::Text(L"Could not save language-pack path.", L"无法保存语言包路径。"));
                else
                    SetWindowTextW(statusLabel,
                                   nskry::plugin_ui::Text(L"Language-pack path saved. No download or model loading was started.", L"已保存语言包路径。"));
            }
            return 0;
        }
    } catch (const std::wstring &error) {
        SetWindowTextW(statusLabel, error.c_str());
    } catch (...) {
        SetWindowTextW(statusLabel, nskry::plugin_ui::Text(L"Operation failed; no online fallback was used.", L"操作失败。请求未切换至在线引擎。"));
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
void Show(HWND owner) {
    if (!window && task)
        throw TranslationFailure{nskry::plugin_ui::Text(L"The previous task is still canceling. Try again shortly.", L"上一项任务正在取消，请稍后重试。")};
    if (window) {
        if (IsIconic(window))
            ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        return;
    }
    std::error_code fileError;
    hasOfflineEngine = std::filesystem::is_regular_file(OfflineWorkerPath(PluginDirectory()), fileError);
    WNDCLASSW wc{};
    wc.hInstance = module;
    wc.lpfnWndProc = Procedure;
    wc.lpszClassName = L"NskryTranslateV1";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    window = CreateWindowExW(0, wc.lpszClassName, nskry::plugin_ui::Text(L"Nskry — Translation (experimental)", L"Nskry — 翻译（实验版）"), WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 900, 560, owner, nullptr, module, nullptr);
    if (!window)
        winrt::throw_last_error();
    const auto child = [](const wchar_t *type, const wchar_t *text, DWORD style, int id) {
        return CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window,
                               reinterpret_cast<HMENU>(INT_PTR(id)), module, nullptr);
    };
    child(L"STATIC", nskry::plugin_ui::Text(L"Source text", L"原文"), SS_LEFT, 24);
    sourceEdit = child(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 10);
    child(L"STATIC", nskry::plugin_ui::Text(L"Translation", L"译文"), SS_LEFT, 25);
    resultEdit = child(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, 11);
    // Allow over-limit paste so validation rejects it explicitly, rather than
    // silently turning a truncated paste into a successful translation.
    SendMessageW(sourceEdit, EM_SETLIMITTEXT, MaxTextUnits * 2, 0);
    SendMessageW(resultEdit, EM_SETLIMITTEXT, MaxReplyBytes, 0);
    packEdit = child(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 12);
    directionChoice = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 13);
    SendMessageW(directionChoice, CB_ADDSTRING, 0, LPARAM(nskry::plugin_ui::Text(L"Auto (中/En)", L"自动识别（中／英）")));
    SendMessageW(directionChoice, CB_ADDSTRING, 0, LPARAM(nskry::plugin_ui::Text(L"English → 中文", L"英文 → 中文")));
    SendMessageW(directionChoice, CB_ADDSTRING, 0, LPARAM(nskry::plugin_ui::Text(L"中文 → English", L"中文 → 英文")));
    SendMessageW(directionChoice, CB_SETCURSEL, 0, 0);
    child(L"BUTTON", nskry::plugin_ui::Text(L"Translate", L"翻译"), WS_TABSTOP, 1);
    child(L"BUTTON", nskry::plugin_ui::Text(L"Cancel", L"取消"), WS_TABSTOP, 2);
    child(L"BUTTON", nskry::plugin_ui::Text(L"Copy translation", L"复制译文"), WS_TABSTOP, 3);
    child(L"BUTTON", nskry::plugin_ui::Text(L"Save pack path", L"保存语言包路径"), WS_TABSTOP, 4);
    child(L"BUTTON", nskry::plugin_ui::Text(L"Save text", L"保存译文"), WS_TABSTOP, 5);
    providerChoice = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 20);
    const auto *firstEngine = hasOfflineEngine
        ? nskry::plugin_ui::Text(L"Offline · English/Chinese", L"离线 · 英文／中文")
        : nskry::plugin_ui::Text(L"Choose an online engine", L"请选择在线引擎");
    for (const auto *label : {firstEngine,
                              nskry::plugin_ui::Text(L"OpenAI-compatible (experimental)", L"OpenAI 兼容（实验）"),
                              nskry::plugin_ui::Text(L"LibreTranslate (experimental)", L"LibreTranslate（实验）"),
                              nskry::plugin_ui::Text(L"MyMemory (free, quotas apply)", L"MyMemory（免费，有额度）")})
        SendMessageW(providerChoice, CB_ADDSTRING, 0, LPARAM(label));
    DWORD preferred{}, preferredSize = sizeof(preferred);
    if (RegGetValueW(HKEY_CURRENT_USER, Key, L"PreferredProviderV1", RRF_RT_REG_DWORD, nullptr, &preferred,
                     &preferredSize) != ERROR_SUCCESS ||
        preferred > static_cast<DWORD>(Provider::MyMemory))
        preferred = 0;
    // Index zero is Offline when installed and an explicit-choice prompt otherwise.
    // This preserves persisted provider values across package variants.
    SendMessageW(providerChoice, CB_SETCURSEL, preferred, 0);
    child(L"BUTTON", nskry::plugin_ui::Text(L"Configure engine", L"配置引擎"), WS_TABSTOP, 21);
    child(L"BUTTON", nskry::plugin_ui::Text(L"Use installed OCR plugin for screenshot translation (optional)", L"使用已安装的文字识别插件处理截图（可选）"),
          BS_AUTOCHECKBOX | WS_TABSTOP, 22);
    child(L"STATIC", L"", 0, 23);
    DWORD useProvider{}, preferenceBytes = sizeof(useProvider);
    useOcrPlugin = RegGetValueW(HKEY_CURRENT_USER, Key, L"UseOcrPluginV1", RRF_RT_REG_DWORD, nullptr,
                                &useProvider, &preferenceBytes) == ERROR_SUCCESS &&
                   useProvider == 1;
    SendMessageW(GetDlgItem(window, 22), BM_SETCHECK, useOcrPlugin ? BST_CHECKED : BST_UNCHECKED, 0);
    statusLabel = child(L"STATIC", SelectedProvider()
        ? nskry::plugin_ui::Text(L"Select a region or paste source text to translate.",
                                 L"框选截图区域或粘贴原文即可翻译。")
        : ChooseEngineHint(), 0, 14);
    wchar_t saved[32768]{};
    DWORD bytes = sizeof(saved);
    auto root = (PluginDirectory() / L"languages").wstring();
    if (RegGetValueW(HKEY_CURRENT_USER, Key, L"ModelRoot", RRF_RT_REG_SZ, nullptr, saved, &bytes) ==
        ERROR_SUCCESS)
        root = saved;
    SetWindowTextW(packEdit, root.c_str());
    if (!sourceEdit || !resultEdit || !packEdit || !directionChoice || !providerChoice || !statusLabel ||
        !GetDlgItem(window, 1) || !GetDlgItem(window, 2) || !GetDlgItem(window, 3) ||
        !GetDlgItem(window, 4) || !GetDlgItem(window, 5) || !GetDlgItem(window, 21) ||
        !GetDlgItem(window, 22) || !GetDlgItem(window, 23)) {
        DestroyWindow(window);
        winrt::throw_hresult(E_OUTOFMEMORY);
    }
    closing = false;
    if (!hasOfflineEngine) {
        ShowWindow(packEdit, SW_HIDE);
        ShowWindow(GetDlgItem(window, 4), SW_HIDE);
    }
    RefreshProviderControls();
    RefreshFont(window);
    nskry::plugin_ui::AttachTabNavigation(window);
    SendMessageW(window, WM_SIZE, 0, 0);
    ShowWindow(window, SW_SHOW);
}
NskryPluginInfo info{sizeof(info),
                     NSKRY_PLUGIN_API_VERSION,
                     L"Translation",
                     L"nskry-translate",
                     L"0.3.4",
                     L"Nskry Team",
                     L"Experimental on-demand Chinese/English translation with optional online engines.",
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
NSKRY_API const NskryPluginInfo *NSKRY_CALL nskry_plugin_info() {
    return &info;
}
NSKRY_API int32_t NSKRY_CALL nskry_plugin_init(const NskryHostContext *) {
    return 1;
}
extern "C" NSKRY_API int32_t NSKRY_CALL
nskry_plugin_connect_image_recognition_v1(const NskryImageRecognitionServiceV1 *service) {
    if (!service || service->structSize < sizeof(*service) || service->version != 1 || !service->available ||
        !service->begin || !service->poll || !service->cancel || !service->release)
        return 0;
    recognitionService = *service;
    return 1;
}
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
        if (task || closing || starting) {
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
        if (!SelectedProvider()) {
            SetWindowTextW(statusLabel, ChooseEngineHint());
            return NSKRY_TRANSLATION_ACCEPTED; // Source is ready for a user-selected engine.
        }
        if (!BeginTranslation()) {
            if (uploadCanceled) {
                NskryTranslationError(result, L"Online upload was not authorized; no request was sent.");
                return NSKRY_TRANSLATION_CANCELED;
            }
            const auto error = Text(statusLabel);
            NskryTranslationError(result, error.c_str());
            return NSKRY_TRANSLATION_INVALID;
        }
        return NSKRY_TRANSLATION_ACCEPTED;
    } catch (const std::wstring &error) {
        if (statusLabel)
            SetWindowTextW(statusLabel, error.c_str());
        NskryTranslationError(result, error.c_str());
        return NSKRY_TRANSLATION_FAILED;
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
        if (task || closing || starting)
            return;
        SendMessageW(directionChoice, CB_SETCURSEL, 0, 0);
        SetWindowTextW(sourceEdit, L"");
        SetWindowTextW(resultEdit, L"");
        if (useOcrPlugin) {
            uint64_t ticket{};
            if (!recognitionService.available || !recognitionService.available(L"nskry-ocr") ||
                recognitionService.begin(L"nskry-ocr", context->capturedBitmap, &ticket) !=
                    NSKRY_IMAGE_PENDING) {
                SetWindowTextW(statusLabel,
                               L"Selected OCR plugin is missing, disabled, busy or incompatible. "
                               L"Enable/update it, or uncheck the OCR option. No fallback was used.");
                return;
            }
            try {
                Start(true, [](Task &) { return std::wstring{}; }, false, ticket);
            } catch (...) {
                recognitionService.release(ticket);
                throw;
            }
            return;
        }
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
            return RecognizeImage(pixels, w, h, PluginDirectory(), work.cancel);
        });
    } catch (...) {
        if (statusLabel)
            SetWindowTextW(statusLabel, nskry::plugin_ui::Text(L"Could not recognize the image. Enter source text manually.", L"无法识别图片。可以手动输入原文。"));
    }
}
NSKRY_API void NSKRY_CALL nskry_plugin_shutdown() {
    task.reset();
    CloseTaskDispatch();
    CloseOnlineSettings(module);
    if (window) {
        KillTimer(window, 1);
        DestroyWindow(window);
    }
    UnregisterClassW(L"NskryTranslateV1", module);
    UnregisterClassW(L"NskryTranslateTaskV1", module);
    recognitionService = {};
}

extern "C" NSKRY_API void NSKRY_CALL nskry_plugin_set_ui_preferences_v1(const NskryUiPreferencesV1* ui) {
    nskry::plugin_ui::SetPreferences(ui);
}
