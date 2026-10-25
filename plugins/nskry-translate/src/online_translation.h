#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace nskry::translate {
enum class Provider { Offline, OpenAI, LibreTranslate, MyMemory };
struct OnlineConfig {
    std::wstring endpoint, model, key;
    bool legacyTokenLimit{};
    ~OnlineConfig();
};
struct OnlineRequest {
    std::wstring url, headers;
    std::string body;
    bool get{};
};
// These functions only inspect values; no network, registry or DLL discovery.
std::wstring ValidateEndpoint(std::wstring_view endpoint);
std::wstring ValidateOnlineConfig(Provider provider, const OnlineConfig &config);
OnlineRequest BuildOnlineRequest(Provider provider, const OnlineConfig &config, std::wstring_view source,
                                 bool toChinese);
std::wstring ParseOnlineReply(Provider provider, std::wstring_view json);
std::vector<std::wstring> SplitFreeOnlineText(std::wstring_view source);
// UI-thread configuration. One DPAPI-protected, versioned registry value per
// provider; saving preserves unknown fields and fails closed on corrupt data.
bool LoadOnlineConfig(Provider provider, OnlineConfig &config, std::wstring &error);
bool SaveOnlineConfig(Provider provider, const OnlineConfig &config, std::wstring &error);
bool ForgetOnlineConfig(Provider provider, std::wstring &error);
std::wstring RunOnline(Provider provider, const OnlineConfig &config, std::wstring_view source,
                       bool toChinese, HANDLE cancel);
} // namespace nskry::translate
