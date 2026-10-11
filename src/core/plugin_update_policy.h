#pragma once
#include "core/plugin_registry.h"

namespace nskry {
struct PluginUpdate {
    std::wstring pluginId, name, currentVersion, version, packageUrl, sha256, releaseNotes;
    PluginSource source = PluginSource::ThirdParty;
};

// Pure feed policy: an official catalog can update official records only;
// a publisher's feed can update only the requested, non-official plugin ID.
bool ParsePluginUpdateFeed(const std::wstring& text,
    const std::vector<const PluginRecord*>& installed, PluginSource feedSource,
    const std::wstring& requestedId, std::vector<PluginUpdate>& updates, std::wstring* error = nullptr);
bool MatchesPluginUpdate(const PluginManifest& package, const PluginUpdate& update,
    std::wstring* error = nullptr);
bool IsPluginHttpsUrl(const std::wstring& url);
} // namespace nskry
