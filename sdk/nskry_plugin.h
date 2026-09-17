// ============================================================================
// Nskry Plugin SDK — Public Header
// ============================================================================
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
//       L"My Plugin", L"my-plugin", L"1.0.0", L"Author",
//       L"Description", nullptr, NSKRY_CAP_TOOLBAR_ACTION
//   };
//
//   extern "C" {
//     NSKRY_API const NskryPluginInfo* nskry_plugin_info() { return &s_info; }
//     NSKRY_API bool nskry_plugin_init(const NskryHostContext*) { return true; }
//     NSKRY_API void nskry_plugin_execute(const NskryHostContext* ctx) { /* ... */ }
//     NSKRY_API void nskry_plugin_shutdown() {}
//   }

#pragma once

#include <cstdint>
#include <windows.h>
#include <d3d11.h>

#define NSKRY_VERSION L"0.4.0"
#define NSKRY_PLUGIN_API_VERSION 1

#ifdef NSKRY_PLUGIN_EXPORTS
#  define NSKRY_API __declspec(dllexport)
#else
#  define NSKRY_API __declspec(dllimport)
#endif

// ---- Plugin capability flags (bitmask) ----

enum NskryPluginCaps : uint32_t {
    NSKRY_CAP_TOOLBAR_ACTION  = 1 << 0,   // Adds a button to the screenshot toolbar
    NSKRY_CAP_POST_CAPTURE    = 1 << 1,   // Post-processing after capture (e.g. OCR)
    NSKRY_CAP_STANDALONE      = 1 << 2,   // Independent global feature (e.g. screen recording)
};

// ---- Plugin metadata ----

struct NskryPluginInfo {
    const wchar_t* name;          // Display name, e.g. "OCR 文字识别"
    const wchar_t* id;            // Unique ID, e.g. "nskry-ocr"
    const wchar_t* version;       // Semver, e.g. "1.0.0"
    const wchar_t* author;        // Author name
    const wchar_t* description;   // Short description
    const wchar_t* iconSvg;       // SVG string for toolbar icon (optional, may be nullptr)
    uint32_t       caps;          // Combination of NskryPluginCaps
};

// ---- Host context passed to plugin functions ----

struct NskryHostContext {
    HWND                 mainHwnd;          // Hidden main message window
    ID3D11Device*        d3dDevice;         // Shared GPU device
    ID3D11DeviceContext*  d3dContext;        // Immediate context

    // Screenshot result (valid during nskry_plugin_execute)
    HBITMAP              capturedBitmap;     // GDI bitmap of the captured region
    RECT                 capturedRegion;     // Screen coordinates of the capture
    HWND                 sourceHwnd;         // Window that was captured

    // Host utility callbacks
    void (*showNotification)(const wchar_t* msg, int durationMs);
    void (*copyBitmapToClipboard)(HBITMAP hbmp);
};

// ---- Functions every plugin DLL must export ----

extern "C" {
    /// Return plugin metadata. Called once at load time.
    NSKRY_API const NskryPluginInfo* nskry_plugin_info();

    /// Initialize the plugin. Return false to abort loading.
    NSKRY_API bool nskry_plugin_init(const NskryHostContext* ctx);

    /// Execute the plugin's primary action (toolbar button clicked).
    NSKRY_API void nskry_plugin_execute(const NskryHostContext* ctx);

    /// Clean up before unloading.
    NSKRY_API void nskry_plugin_shutdown();
}
