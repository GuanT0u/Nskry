#pragma once

#include <string>
#include <vector>
#include "core/input_settings.h"

namespace nskry {

enum class PluginLoadPolicy { OnDemand, Startup };

struct PluginIconPoint { float x, y; };
using PluginToolbarIcon = std::vector<std::vector<PluginIconPoint>>;
inline constexpr size_t kMaxPluginIconStrokePoints = 16;

/// Metadata that is safe to read before loading a third-party DLL.
struct PluginManifest {
    int manifestVersion = 0;
    std::wstring id;
    std::wstring name;
    std::wstring toolbarLabelZhCn;
    PluginToolbarIcon toolbarIcon;
    std::wstring version;
    std::wstring author;
    std::wstring description;
    std::wstring entry;
    std::wstring architecture;
    unsigned int apiVersion = 0;
    PluginLoadPolicy loadPolicy = PluginLoadPolicy::OnDemand;
    std::wstring nskryMinVersion;
    std::wstring nskryMaxVersion;
    std::wstring homepage;
    std::wstring repository;
    std::wstring updateUrl;
    bool toolbarAction = false;
    bool postCapture = false;
    unsigned int commandApi = 0, captureApi = 0, actionApi = 0, settingsUiApi = 0;
    unsigned int textTranslationApi = 0, textTranslationClientApi = 0;
    unsigned int ocrLayoutApi = 0, ocrLayoutClientApi = 0;
    unsigned int imageRecognitionApi = 0, imageRecognitionClientApi = 0;
    struct CaptureAction {
        std::wstring id, label, labelZhCn;
        PluginToolbarIcon icon;
    };
    std::vector<CaptureAction> captureActions;
    std::vector<ShortcutDefinition> shortcuts;

    static bool LoadFromFile(const std::wstring& path, PluginManifest& manifest, std::wstring* error = nullptr);
    static bool IsSafePluginId(const std::wstring& id);
    static bool IsSafeEntryName(const std::wstring& entry);

    bool IsCompatibleWithHost(std::wstring* error = nullptr) const;
};

} // namespace nskry
