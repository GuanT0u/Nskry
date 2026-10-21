#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional permission: Nskry Independent Open-Source Plugin Exception 1.0.
#include "nskry_plugin.h"
// Optional versioned extra capture actions. Metadata discovery NEVER calls DLLs.
// Primary execute remains unchanged; context and id are borrowed for this UI call.
struct NskryCaptureActionV1 {
    uint32_t structSize, version;
    const wchar_t *id;
    const NskryHostContext *context;
};
using NskryExecuteCaptureActionV1 = int32_t(NSKRY_CALL *)(const NskryCaptureActionV1 *);
// Optional plugin-owned settings UI, explicitly opened by the user on UI thread.
// Return nonzero if shown; must not start capture/encoder merely to show options.
using NskryShowSettingsV1 = int32_t(NSKRY_CALL *)(HWND owner);
