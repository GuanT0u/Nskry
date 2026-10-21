// SPDX-License-Identifier: GPL-3.0-or-later
// Additional permission: Nskry Independent Open-Source Plugin Exception 1.0.
#pragma once
#include <cstdint>
#include <windows.h>

// Optional API 2 extension, negotiated before init through the named export.
// All session functions are called serially on the SAME MTA worker, never UI.
// No shared host D3D context is exposed. Handles own a private capture device.
// Physical desktop pixels, one monitor only, fixed screen region, not HWND-follow.
struct NskryCaptureDescriptionV1 {
    uint32_t structSize, version;
    uint32_t width, height, stride; // tightly packed top-down BGRA, even dimensions
};
struct NskryCaptureServiceV1 {
    uint32_t structSize, version;
    // max FPS 1..30; output preserves aspect, bounded by 1920x1080 (portrait allowed).
    // error/capacity caller-owned, includes terminator. Never retains caller memory.
    HRESULT(__cdecl *open)(RECT physicalRegion, uint32_t fps, void **session, NskryCaptureDescriptionV1 *output,
                           wchar_t *error, uint32_t capacity);
    // S_OK = new owned pixels copied into caller buffer; S_FALSE = no new frame.
    // Failure = stop capture; display/layout/device changes must not silently retarget.
    HRESULT(__cdecl *read)(void *session, uint8_t *bgra, uint32_t capacity);
    void(__cdecl *close)(void *session); // drains/closes before returning; never callback
};
using NskryConnectCaptureV1 = int32_t(__cdecl *)(const NskryCaptureServiceV1 *);

// Optional v2: separate layout/export; v1 and API 2 context are unchanged.
// Physical coordinates, same serialized MTA open/read/bounds/close ownership as v1.
// followWindow==nullptr captures fixed desktop pixels. Otherwise physicalRegion
// is relative to the initial DWM frame; movement follows the window, resize,
// minimization/close and DPI changes fail explicitly (never switch sources).
struct NskryCaptureRequestV2 {
    uint32_t structSize, version;
    RECT physicalRegion;
    uint32_t fps; // 1..60; actual delivery may be slower.
    HWND followWindow;
    uint32_t maxLongSide;  // 0: 1920, otherwise 2..1920.
    uint32_t maxShortSide; // 0: 1080, otherwise 2..1080.
};
struct NskryCaptureServiceV2 {
    uint32_t structSize, version;
    HRESULT(__cdecl *open)(const NskryCaptureRequestV2 *, void **, NskryCaptureDescriptionV1 *, wchar_t *, uint32_t);
    HRESULT(__cdecl *read)(void *, uint8_t *, uint32_t);
    HRESULT(__cdecl *bounds)(void *, RECT *); // Current physical recording bounds.
    void(__cdecl *close)(void *);
};
using NskryConnectCaptureV2 = int32_t(__cdecl *)(const NskryCaptureServiceV2 *);
// Plugin export: int32_t nskry_plugin_connect_capture_v1(const NskryCaptureServiceV1*);
// Copy descriptor, not pointer. Nonzero accepts. Init/shutdown must not warm capture.
