#include "translation_http.h"
#include "translation_policy.h"
#include <array>
#include <atomic>
#include <winhttp.h>

namespace nskry::translate {
namespace {
struct Internet {
    HINTERNET value{};
    ~Internet() {
        if (value)
            WinHttpCloseHandle(value);
    }
};
struct Event {
    HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Event() {
        if (value)
            CloseHandle(value);
    }
};
struct Session {
    Event callbacksDone;
    HINTERNET value{};
    bool notified{};
    ~Session() {
        if (value) {
            WinHttpCloseHandle(value);
            // Unlike signaling from inside HANDLE_CLOSING, this system-owned
            // event is set AFTER the session's last callback has returned.
            // The DLL can therefore be unloaded after its task joins.
            if (notified)
                WaitForSingleObject(callbacksDone.value, INFINITE);
        }
    }
};
struct State {
    Event ready, closed;
    std::atomic<DWORD> status{}, bytes{};
};
void CALLBACK Status(HINTERNET, DWORD_PTR context, DWORD status, void *, DWORD length) {
    auto *state = reinterpret_cast<State *>(context);
    if (!state)
        return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        SetEvent(state->closed.value);
    } else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
               status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
               status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE ||
               status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        state->bytes = length;
        state->status = status;
        SetEvent(state->ready.value);
    }
}
struct Request {
    HINTERNET value{};
    State &state;
    bool callbacks{};
    ~Request() {
        if (value) {
            WinHttpCloseHandle(value);
            // Final HANDLE_CLOSING is the callback lifetime fence. All state,
            // read buffers and request data outlive it, including on cancel.
            if (callbacks)
                WaitForSingleObject(state.closed.value, INFINITE);
        }
    }
};
void Require(bool ok, const wchar_t *error) {
    if (!ok)
        throw std::wstring(error);
}
} // namespace

std::wstring FetchTranslation(const OnlineRequest &input, HANDLE cancel, unsigned timeoutMs) {
    Require(cancel && WaitForSingleObject(cancel, 0) == WAIT_TIMEOUT, L"Online translation canceled.");
    Require(timeoutMs && timeoutMs <= TaskTimeoutMs, L"Online translation timed out.");
    const auto validation = ValidateEndpoint(input.url);
    Require(validation.empty(), validation.c_str());
    // MyMemory's generated query is the only allowed query; configuration URLs
    // are separately rejected before request construction.
    const auto deadline = GetTickCount64() + timeoutMs;
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = components.dwUrlPathLength = components.dwExtraInfoLength = DWORD(-1);
    Require(WinHttpCrackUrl(input.url.c_str(), 0, 0, &components), L"Invalid online endpoint.");
    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength)
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    // Context/read storage outlive the entire session and all callbacks, not
    // merely the notification that the request handle is closing.
    State state;
    std::array<char, 32768> buffer{};
    Session session;
    session.value = WinHttpOpen(L"Nskry-Translation/0.3", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    Require(session.value && WinHttpSetTimeouts(session.value, 8000, 8000, 15000, 15000),
            L"Could not initialize HTTPS transport.");
    Require(session.callbacksDone.value && WinHttpSetOption(session.value, WINHTTP_OPTION_UNLOAD_NOTIFY_EVENT,
                                                            &session.callbacksDone.value, sizeof(HANDLE)),
            L"Could not establish the network callback lifetime fence.");
    session.notified = true;
    BOOL noGlobalPool = TRUE;
    Require(WinHttpSetOption(session.value, WINHTTP_OPTION_DISABLE_GLOBAL_POOLING, &noGlobalPool,
                             sizeof(noGlobalPool)),
            L"Could not isolate the translation network session.");
    Internet connection{WinHttpConnect(session.value, host.c_str(), components.nPort, 0)};
    Request request{connection.value ? WinHttpOpenRequest(connection.value, input.get ? L"GET" : L"POST",
                                                          path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                     : nullptr,
                    state};
    Require(request.value && state.ready.value && state.closed.value, L"Could not create online request.");
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&state);
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    Require(
        WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) &&
            WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) &&
            WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) &&
            WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon)) &&
            WinHttpSetStatusCallback(request.value, Status,
                                     WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
                                     0) != WINHTTP_INVALID_STATUS_CALLBACK,
        L"Could not configure online request safety.");
    request.callbacks = true;
    auto wait = [&](BOOL issued, DWORD expected) {
        Require(issued || GetLastError() == ERROR_IO_PENDING, L"HTTPS operation could not start.");
        const auto now = GetTickCount64();
        Require(now < deadline, L"Online translation timed out after its request budget.");
        HANDLE events[]{cancel, state.ready.value};
        const auto result = WaitForMultipleObjects(2, events, FALSE, DWORD(deadline - now));
        Require(result != WAIT_OBJECT_0,
                L"Online translation canceled. The server may already have processed the request.");
        Require(result != WAIT_TIMEOUT,
                L"Online translation timed out. The server may already have processed the request.");
        Require(result == WAIT_OBJECT_0 + 1 && state.status == expected,
                L"HTTPS request failed (network, TLS or timeout). No retry was sent.");
    };
    wait(WinHttpSendRequest(request.value, input.headers.c_str(), DWORD(input.headers.size()),
                            input.body.empty() ? nullptr : const_cast<char *>(input.body.data()),
                            DWORD(input.body.size()), DWORD(input.body.size()), context),
         WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
    wait(WinHttpReceiveResponse(request.value, nullptr), WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
    DWORD status{}, size = sizeof(status);
    Require(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX),
            L"Missing HTTP status.");
    if (status != 200) {
        if (status == 401 || status == 403)
            throw std::wstring(L"Online access denied. Check your key, account and provider permissions.");
        if (status == 429)
            throw std::wstring(
                L"Provider quota or rate limit reached. No automatic retry or fallback was sent.");
        if (status >= 300 && status < 400)
            throw std::wstring(L"Endpoint redirected. Redirects are blocked to protect text and credentials; "
                               L"configure the final HTTPS URL.");
        throw std::wstring(L"Online provider returned HTTP ") + std::to_wstring(status) +
            L". Check endpoint, model and API compatibility.";
    }
    std::string bytes;
    for (;;) {
        Require(WaitForSingleObject(cancel, 0) == WAIT_TIMEOUT, L"Online translation canceled.");
        wait(WinHttpReadData(request.value, buffer.data(), DWORD(buffer.size()), nullptr),
             WINHTTP_CALLBACK_STATUS_READ_COMPLETE);
        const DWORD count = state.bytes;
        if (!count)
            break;
        Require(count <= buffer.size() && bytes.size() <= MaxReplyBytes - count,
                L"Online reply exceeds 2 MiB; no partial result was published.");
        bytes.append(buffer.data(), count);
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), nullptr, 0);
    Require(length > 0, L"Provider reply is empty or is not valid UTF-8.");
    std::wstring result(length, 0);
    Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), result.data(),
                                length) == length,
            L"Provider reply could not be decoded.");
    return result;
}
} // namespace nskry::translate
