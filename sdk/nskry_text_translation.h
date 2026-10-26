#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional permission: Nskry Independent Open-Source Plugin Exception 1.0.
#include "nskry_plugin.h"
#include "nskry_text_validation.h"

// Optional, UI-thread-only text handoff. This is not a cross-plugin model API.
// API 2 structs and reserved fields are unchanged. All pointers are borrowed
// for the synchronous call; an accepting provider must copy before returning.
inline constexpr uint32_t NSKRY_TRANSLATION_MAX_TEXT = 16000;
enum NskryTranslationStatusV1 : int32_t {
    NSKRY_TRANSLATION_ACCEPTED = 1, // Task accepted, NOT a claim of finished translation.
    NSKRY_TRANSLATION_UNAVAILABLE = 0,
    NSKRY_TRANSLATION_FAILED = -1,
    NSKRY_TRANSLATION_BUSY = -2,
    NSKRY_TRANSLATION_INVALID = -3,
    NSKRY_TRANSLATION_CANCELED = -4
};
struct NskryTextTranslationV1 {
    uint32_t structSize, version;
    HWND owner; // Optional UI anchor; no lifetime/ownership transfer.
    const wchar_t *text;
    uint32_t textLength; // UTF-16 units, no embedded NUL, <=16000. Not necessarily terminated.
};
struct NskryTranslationResultV1 {
    uint32_t structSize, version;
    wchar_t *error;
    uint32_t errorCapacity; // Includes NUL; error must not contain source text or credentials.
};
using NskryTranslateTextV1 = int32_t(NSKRY_CALL *)(const NskryTextTranslationV1 *,
                                                   NskryTranslationResultV1 *);
struct NskryTextTranslationServiceV1 {
    uint32_t structSize, version;
    int32_t(NSKRY_CALL *available)(); // Metadata only; no DLL/model load or networking.
    NskryTranslateTextV1 translate;   // Explicit user action only. Host chooses enabled provider.
};
using NskryConnectTextTranslationV1 = int32_t(NSKRY_CALL *)(const NskryTextTranslationServiceV1 *);

inline void NskryTranslationError(NskryTranslationResultV1 *result, const wchar_t *message) {
    if (!result || result->structSize < sizeof(*result) || result->version != 1 || !result->error ||
        !result->errorCapacity)
        return;
    uint32_t i = 0;
    for (; i + 1 < result->errorCapacity && message[i]; ++i)
        result->error[i] = message[i];
    result->error[i] = 0;
}
inline bool NskryValidTranslation(const NskryTextTranslationV1 *request) {
    if (!request || request->structSize < sizeof(*request) || request->version != 1 || !request->text ||
        !request->textLength || request->textLength > NSKRY_TRANSLATION_MAX_TEXT)
        return false;
    return NskryValidText(request->text, request->textLength, NSKRY_TRANSLATION_MAX_TEXT);
}
