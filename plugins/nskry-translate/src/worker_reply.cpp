#include "worker_reply.h"
#include "core/json.h"
#include "translation_policy.h"

namespace nskry::translate {
namespace {
std::wstring ErrorMessage(std::wstring_view code) {
    if (code == L"pack_missing")
        return L"Language pack missing or incomplete. Choose its root folder containing en-zh and zh-en.";
    if (code == L"runtime_missing")
        return L"Translation runtime incomplete. Use the complete engine folder, not only its EXE.";
    if (code == L"pack_load")
        return L"Could not load the language pack. Check model compatibility, files and available memory.";
    if (code == L"input" || code == L"direction")
        return L"Unsupported source or direction. Use valid English/Chinese text within 16000 UTF-16 units.";
    if (code == L"work_limit")
        return L"Too many translation parts (maximum 128 inference calls). Select a smaller region. No "
               L"partial result was published.";
    if (code == L"input_tokens" || code == L"output_limit")
        return L"A text fragment exceeds the engine's token limit even after splitting. No partial result "
               L"was published.";
    if (code == L"empty_output")
        return L"The engine returned an empty fragment. Review the source; no partial result was published.";
    if (code == L"reply_limit")
        return L"Translation exceeded the reply size limit. Select a smaller region. No partial result was "
               L"published.";
    if (code == L"memory")
        return L"The translation worker ran out of memory. Close unused applications or use a smaller input.";
    return L"Offline engine failed. Check the complete runtime and language pack. No online fallback was "
           L"used.";
}
} // namespace

WorkerReply DecodeWorkerReply(std::wstring_view json, unsigned exitCode) {
    WorkerReply reply;
    reply.error = L"Invalid or interrupted engine reply. No partial result was published.";
    nskry::json::Value value;
    bool ok{};
    if (json.size() > MaxReplyBytes || !nskry::json::Parse(json, value) || !value.GetBool(L"ok", ok))
        return reply;
    unsigned protocol{};
    const bool versioned = value.Find(L"protocol") != nullptr;
    if (versioned && (!value.GetUnsigned(L"protocol", protocol) || protocol != 1))
        return reply;
    if (!ok) {
        if (exitCode != 1)
            return reply;
        std::wstring code;
        value.GetString(L"error_code", code);
        reply.error = ErrorMessage(code);
        return reply;
    }
    if (exitCode != 0 || !value.GetString(L"text", reply.text) ||
        reply.text.find(L'\0') != std::wstring::npos ||
        reply.text.find_first_not_of(L" \t\r\n") == std::wstring::npos ||
        (versioned && (!value.GetUnsigned(L"parts", reply.parts) || !reply.parts || reply.parts > 128))) {
        reply.text.clear();
        return reply;
    }
    reply.success = true;
    reply.error.clear();
    return reply;
}
} // namespace nskry::translate
