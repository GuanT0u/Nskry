#include "core/json.h"
#include "online_translation.h"
#include <vector>
#include <wincrypt.h>

namespace nskry::translate {
namespace {
constexpr wchar_t Key[] = L"Software\\Nskry\\Plugins\\nskry-translate";
const wchar_t *Name(Provider provider) {
    return provider == Provider::OpenAI           ? L"OpenAIConfigV1"
           : provider == Provider::LibreTranslate ? L"LibreConfigV1"
                                                  : nullptr;
}
struct Blob {
    DATA_BLOB value{};
    ~Blob() {
        if (value.pbData) {
            SecureZeroMemory(value.pbData, value.cbData);
            LocalFree(value.pbData);
        }
    }
};
bool Read(Provider provider, nskry::json::Value &document, std::wstring &error) {
    const auto *name = Name(provider);
    if (!name) {
        error = L"This provider has no configurable credentials.";
        return false;
    }
    DWORD size{};
    auto status = RegGetValueW(HKEY_CURRENT_USER, Key, name, RRF_RT_REG_BINARY, nullptr, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        document = nskry::json::Value(nskry::json::Value::Object{});
        return true;
    }
    if (status != ERROR_SUCCESS || !size || size > 65536) {
        error = L"Saved online configuration could not be read; it was not replaced.";
        return false;
    }
    std::vector<BYTE> bytes(size);
    if (RegGetValueW(HKEY_CURRENT_USER, Key, name, RRF_RT_REG_BINARY, nullptr, bytes.data(), &size) !=
        ERROR_SUCCESS) {
        error = L"Saved online configuration changed or could not be read; try again.";
        return false;
    }
    DATA_BLOB encrypted{size, bytes.data()};
    Blob plain;
    if (!CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &plain.value) ||
        plain.value.cbData % sizeof(wchar_t)) {
        error = L"Saved online configuration cannot be decrypted for this Windows user. Forget it explicitly "
                L"before replacing it.";
        return false;
    }
    const std::wstring_view json(reinterpret_cast<const wchar_t *>(plain.value.pbData),
                                 plain.value.cbData / sizeof(wchar_t));
    unsigned schema{};
    if (!nskry::json::Parse(json, document) || !document.IsObject() ||
        !document.GetUnsigned(L"schema", schema) || schema != 1) {
        error = L"Saved online configuration has an unsupported or corrupt format; it was not replaced.";
        return false;
    }
    return true;
}
} // namespace

bool LoadOnlineConfig(Provider provider, OnlineConfig &config, std::wstring &error) {
    error.clear();
    nskry::json::Value document;
    if (!Read(provider, document, error))
        return false;
    config.endpoint.clear();
    config.model.clear();
    if (!config.key.empty())
        SecureZeroMemory(config.key.data(), config.key.size() * sizeof(wchar_t));
    config.key.clear();
    config.legacyTokenLimit = false;
    if (document.AsObject()->empty())
        return true;
    if (!document.GetString(L"endpoint", config.endpoint) || !document.GetString(L"model", config.model) ||
        !document.GetString(L"key", config.key) ||
        !document.GetBool(L"legacy_token_limit", config.legacyTokenLimit)) {
        error = L"Saved online configuration is incomplete; it was not replaced.";
        return false;
    }
    error = ValidateOnlineConfig(provider, config);
    return error.empty();
}

bool SaveOnlineConfig(Provider provider, const OnlineConfig &config, std::wstring &error) {
    error = ValidateOnlineConfig(provider, config);
    if (!error.empty())
        return false;
    nskry::json::Value document;
    if (!Read(provider, document, error))
        return false;
    nskry::json::Value::Object updates{{L"schema", nskry::json::Value(1.0)},
                                       {L"endpoint", nskry::json::Value(config.endpoint)},
                                       {L"model", nskry::json::Value(config.model)},
                                       {L"key", nskry::json::Value(config.key)},
                                       {L"legacy_token_limit", nskry::json::Value(config.legacyTokenLimit)}};
    std::wstring json;
    if (!nskry::json::Serialize(nskry::json::MergeObjects(document, nskry::json::Value(std::move(updates))),
                                json) ||
        json.size() * sizeof(wchar_t) > 60000) {
        error = L"Online configuration exceeds its storage limit; nothing was saved.";
        return false;
    }
    DATA_BLOB plain{DWORD(json.size() * sizeof(wchar_t)), reinterpret_cast<BYTE *>(json.data())};
    Blob encrypted;
    const bool protectedOk = CryptProtectData(&plain, nullptr, nullptr, nullptr, nullptr,
                                              CRYPTPROTECT_UI_FORBIDDEN, &encrypted.value);
    SecureZeroMemory(json.data(), json.size() * sizeof(wchar_t));
    if (!protectedOk || RegSetKeyValueW(HKEY_CURRENT_USER, Key, Name(provider), REG_BINARY,
                                        encrypted.value.pbData, encrypted.value.cbData) != ERROR_SUCCESS) {
        error = L"Could not protect/save online configuration. Previous configuration was not replaced.";
        return false;
    }
    return true;
}

bool ForgetOnlineConfig(Provider provider, std::wstring &error) {
    error.clear();
    const auto *name = Name(provider);
    if (!name) {
        error = L"This provider has no saved configuration.";
        return false;
    }
    const auto status = RegDeleteKeyValueW(HKEY_CURRENT_USER, Key, name);
    if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
        return true;
    error = L"Could not remove this provider's saved configuration.";
    return false;
}
} // namespace nskry::translate
