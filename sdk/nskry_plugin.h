// ============================================================================
// Nskry Plugin SDK — Public Header
// ============================================================================
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional permission: Nskry Independent Open-Source Plugin Exception 1.0.
// See LICENSE, COPYING and PLUGIN_EXCEPTION in the Nskry source distribution.
// Independent open-source plugins may choose their own open-source license;
// incorporating this public SDK material alone does not impose GPL on them.
//
// Include this file in your plugin project to develop Nskry plugins.
// A plugin package is installed under %LOCALAPPDATA%\Nskry\plugins\<id>.
// The host reads manifest.json before loading this DLL, normally on first use.
//
// Minimal plugin example:
//
//   #define NSKRY_PLUGIN_EXPORTS
//   #include "nskry_plugin.h"
//
//   static NskryPluginInfo s_info = {
//       sizeof(NskryPluginInfo), NSKRY_PLUGIN_API_VERSION,
//       L"My Plugin", L"my-plugin", L"1.0.0", L"Author",
//       L"Description", nullptr, NSKRY_CAP_TOOLBAR_ACTION, {}
//   };
//
//   extern "C" {
//     NSKRY_API const NskryPluginInfo* nskry_plugin_info() { return &s_info; }
//     NSKRY_API int32_t nskry_plugin_init(const NskryHostContext*) { return 1; }
//     NSKRY_API void nskry_plugin_execute(const NskryHostContext* ctx) { /* ... */ }
//     NSKRY_API void nskry_plugin_shutdown() {}
//   }

#pragma once

#include <cstdint>
#include <windows.h>
#include <d3d11.h>

#define NSKRY_VERSION L"0.5.0"
#define NSKRY_PLUGIN_API_VERSION 2
#define NSKRY_CALL __cdecl

#ifdef NSKRY_PLUGIN_EXPORTS
#  define NSKRY_API __declspec(dllexport)
#else
#  define NSKRY_API __declspec(dllimport)
#endif

// ---- Plugin capability flags (bitmask) ----

enum NskryPluginCaps : uint32_t {
    NSKRY_CAP_TOOLBAR_ACTION  = 1 << 0,   // Adds a button to the screenshot toolbar
    NSKRY_CAP_POST_CAPTURE    = 1 << 1,   // Image actions: pin, monitor frame and host editor selection
    NSKRY_CAP_STANDALONE      = 1 << 2,   // Reserved: standalone command surfaces aren't implemented yet
};

// ---- Plugin metadata ----

struct NskryPluginInfo {
    uint32_t       structSize;    // sizeof(NskryPluginInfo)
    uint32_t       apiVersion;    // NSKRY_PLUGIN_API_VERSION
    const wchar_t* name;          // Display name, e.g. "OCR 文字识别"
    const wchar_t* id;            // Unique ID, e.g. "nskry-ocr"
    const wchar_t* version;       // Semver, e.g. "1.0.0"
    const wchar_t* author;        // Author name
    const wchar_t* description;   // Short description
    const wchar_t* iconSvg;       // SVG string for toolbar icon (optional, may be nullptr)
    uint32_t       caps;          // Combination of NskryPluginCaps
    uintptr_t      reserved[4]{}; // Must be zero; reserved for ABI-compatible growth
};

// ---- Host context passed to plugin functions ----

struct NskryHostContext {
    uint32_t             structSize;        // sizeof(NskryHostContext)
    uint32_t             apiVersion;        // NSKRY_PLUGIN_API_VERSION
    HWND                 mainHwnd;          // Hidden main message window
    ID3D11Device*        d3dDevice;         // Shared GPU device
    ID3D11DeviceContext*  d3dContext;        // Immediate context

    // Screenshot result (valid during nskry_plugin_execute)
    // Borrowed from the host. Plugins must not delete or transfer this handle.
    HBITMAP              capturedBitmap;     // GDI bitmap of the captured region
    RECT                 capturedRegion;     // Screen capture bounds; synthetic images may use a UI placement anchor
    HWND                 sourceHwnd;         // Captured window, or nullptr when no live source is available

    // Host utility callbacks. UI callbacks must run on the host UI thread.
    // API 2 provides these core services without any other plugin installed.
    // Plugins contribute metadata-driven actions; they must not mutate host
    // HWND controls or depend on another plugin's private window/class/DLL.
    void (*showNotification)(const wchar_t* msg, int durationMs);
    // Copies bitmap data; ownership remains with the caller.
    // Returns non-zero when at least one standard bitmap format was published.
    int32_t (*copyBitmapToClipboard)(HBITMAP hbmp);

    // Opens the CORE image crop/annotation workflow (not nskry-scroll).
    // Ownership of the bitmap is
    // transferred to the host; plugins must not use or delete it afterwards.
    void (*openBitmapEditor)(HBITMAP hbmp, int width, int height);

    uintptr_t            reserved[4]{};      // Must be zero
};

// ---- Functions every plugin DLL must export ----

extern "C" {
    /// Return plugin metadata. Called once at load time.
    NSKRY_API const NskryPluginInfo* NSKRY_CALL nskry_plugin_info();

    /// Initialize the plugin. Return false to abort loading.
    NSKRY_API int32_t NSKRY_CALL nskry_plugin_init(const NskryHostContext* ctx);

    /// Execute the plugin's primary action (toolbar button clicked).
    NSKRY_API void NSKRY_CALL nskry_plugin_execute(const NskryHostContext* ctx);

    /// Clean up before unloading, also after init was entered but failed.
    /// Must tolerate partial initialization; release threads/windows/callbacks.
    NSKRY_API void NSKRY_CALL nskry_plugin_shutdown();
}
