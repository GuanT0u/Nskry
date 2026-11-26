#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional permission: Nskry Independent Open-Source Plugin Exception 1.0.
#include "nskry_plugin.h"

enum NskryOcrStatus : int32_t {
    NSKRY_OCR_OK = 1, NSKRY_OCR_EMPTY = 0, NSKRY_OCR_FAILED = -1, NSKRY_OCR_INVALID = -2,
    NSKRY_OCR_CANCELED = -3, NSKRY_OCR_TIMEOUT = -4, NSKRY_OCR_LANGUAGE = -5,
    NSKRY_OCR_UNAVAILABLE = -6, NSKRY_OCR_BUSY = -7
};
// Coordinates are original bitmap pixels, ranges are UTF-16 units in text.
struct NskryOcrWordV1 { RECT bounds; uint32_t offset, length, line, joinPrevious; };
struct NskryOcrLayoutV1 {
    const wchar_t *text;
    uint32_t length;
    const RECT *lines;
    uint32_t lineCount;
    const NskryOcrWordV1 *words;
    uint32_t wordCount;
};
struct NskryOcrRequestV1 {
    uint32_t structSize, version;
    HBITMAP bitmap; // Borrowed until synchronous recognition returns on a worker thread.
    uint32_t fast; // Explicit latency preference. Zero requests the accurate path.
    void *context;
    int32_t (NSKRY_CALL *canceled)(void *);
    // Called once on success. Copy borrowed data before returning. Zero rejects the result.
    int32_t (NSKRY_CALL *receive)(void *, const NskryOcrLayoutV1 *);
    wchar_t *error;
    uint32_t errorCapacity;
    uint64_t requestId;
};
using NskryRecognizeLayoutV1 = int32_t(NSKRY_CALL *)(const NskryOcrRequestV1 *);
// UI thread acquire. Each successful call owns one DLL reference until FreeLibrary,
// after recognition has returned. A provider is stateless between calls.
struct NskryOcrLeaseV1 { HMODULE module; NskryRecognizeLayoutV1 recognize; };
using NskryAcquireOcrV1 = int32_t(NSKRY_CALL *)(const wchar_t *preferredId, NskryOcrLeaseV1 *);
using NskryConnectOcrV1 = int32_t(NSKRY_CALL *)(NskryAcquireOcrV1);
