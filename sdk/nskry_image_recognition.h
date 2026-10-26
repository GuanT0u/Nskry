#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional permission: Nskry Independent Open-Source Plugin Exception 1.0.
#include "nskry_plugin.h"

// Optional extension, separate from the frozen API 2 structures. Local image
// recognition only: no UI, network, model prewarm or automatic fallback.
enum : int32_t {
    NSKRY_IMAGE_PENDING = 0,
    NSKRY_IMAGE_SUCCESS = 1,
    NSKRY_IMAGE_FAILED = -1,
    NSKRY_IMAGE_CANCELED = -2,
    NSKRY_IMAGE_UNAVAILABLE = -3,
    NSKRY_IMAGE_INVALID = -4,
    NSKRY_IMAGE_BUSY = -5
};
constexpr uint32_t NSKRY_IMAGE_MAX_TEXT = 16000;
struct NskryImageTextV1 {
    uint32_t structSize, version;
    wchar_t *text;
    uint32_t capacity, textLength; // Capacity includes NUL.
    wchar_t *error;
    uint32_t errorCapacity;
};
// Provider call: background MTA, borrowed task-owned bitmap/cancel handle.
// Must not touch UI, Registry, host context or shared D3D context. Check cancel
// and finish within 30 seconds; return only after owned workers have exited.
struct NskryImageInputV1 {
    uint32_t structSize, version;
    HBITMAP bitmap;
    int32_t width, height;
    HANDLE cancel;
};
using NskryRecognizeImageV1 = int32_t(NSKRY_CALL *)(const NskryImageInputV1 *, NskryImageTextV1 *);
// Host calls are UI-thread-only. Discovery does not load DLLs. begin copies
// bitmap synchronously; poll copies finished text to caller buffers. Release
// cancels/joins and invalidates the ticket. One active job, no idle polling.
// Cancel is nonblocking; call poll until terminal, then release. During shutdown
// the host cancels/joins BEFORE unloading any provider DLL. Tickets are not pointers.
struct NskryImageRecognitionServiceV1 {
    uint32_t structSize, version;
    int32_t(NSKRY_CALL *available)(const wchar_t *providerId);
    int32_t(NSKRY_CALL *begin)(const wchar_t *providerId, HBITMAP, uint64_t *ticket);
    int32_t(NSKRY_CALL *poll)(uint64_t ticket, NskryImageTextV1 *);
    void(NSKRY_CALL *cancel)(uint64_t ticket);
    void(NSKRY_CALL *release)(uint64_t ticket);
};
using NskryConnectImageRecognitionV1 = int32_t(NSKRY_CALL *)(const NskryImageRecognitionServiceV1 *);

inline bool NskryValidImageText(const NskryImageTextV1 *out) {
    return out && out->structSize >= sizeof(*out) && out->version == 1 && out->text &&
           out->capacity >= NSKRY_IMAGE_MAX_TEXT + 1 && out->error && out->errorCapacity >= 256;
}
inline void NskryImageMessage(NskryImageTextV1 *out, const wchar_t *text) {
    if (NskryValidImageText(out))
        lstrcpynW(out->error, text, out->errorCapacity);
}
