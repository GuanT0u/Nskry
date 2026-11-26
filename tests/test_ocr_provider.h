#pragma once
#include "nskry_ocr_layout.h"
#include <filesystem>

// Standalone DLL probes have no host registry. Inject the exact test provider.
inline std::filesystem::path TestOcrProvider() {
    wchar_t path[32768]{};
    if (GetEnvironmentVariableW(L"NSKRY_TEST_OCR_PROVIDER", path, 32768)) return path;
    GetModuleFileNameW(nullptr, path, 32768);
    return std::filesystem::path(path).parent_path() / L"plugins/nskry-ocr-provider/package/nskry-ocr-provider.dll";
}
inline bool ConnectTestOcr(HMODULE feature) {
    auto connect = reinterpret_cast<NskryConnectOcrV1>(GetProcAddress(feature, "nskry_plugin_connect_ocr_layout_v1"));
    if (!connect) return true; // Historical comparison packages predate the extension.
    return connect([](const wchar_t *, NskryOcrLeaseV1 *lease) -> int32_t {
        auto module = LoadLibraryExW(TestOcrProvider().c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) return NSKRY_OCR_UNAVAILABLE;
        auto recognize = reinterpret_cast<NskryRecognizeLayoutV1>(GetProcAddress(module, "nskry_plugin_ocr_layout_v1"));
        if (!recognize) { FreeLibrary(module); return NSKRY_OCR_UNAVAILABLE; }
        *lease = {module, recognize};
        return NSKRY_OCR_OK;
    });
}
