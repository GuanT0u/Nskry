// SPDX-License-Identifier: GPL-3.0-or-later
// Additional permission: Nskry Independent Open-Source Plugin Exception 1.0.
#pragma once
#include <cstdint>
#include <windows.h>
struct NskryCommandV1 {
    uint32_t structSize, version;
    const wchar_t *id; // manifest-local command ID, borrowed during call
};
struct NskryCommandResultV1 {
    uint32_t structSize, version;
    wchar_t *error;
    uint32_t capacity;
};
enum NskryCommandStatusV1 : int32_t {
    NSKRY_COMMAND_OK,
    NSKRY_COMMAND_FAILED,
    NSKRY_COMMAND_UNSUPPORTED
};
using NskryCommandHandlerV1 = int32_t(__cdecl *)(const NskryCommandV1 *, NskryCommandResultV1 *);
// Optional export nskry_plugin_command_v1. UI thread, nonblocking; never throws.
// Host dispatches declared commands ONLY to already-loaded DLLs; no idle loading.
