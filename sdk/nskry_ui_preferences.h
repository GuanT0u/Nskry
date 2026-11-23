// SPDX-License-Identifier: GPL-3.0-or-later
// Additional permission: Nskry Independent Open-Source Plugin Exception 1.0.
#pragma once
#include "nskry_plugin.h"

enum NskryUiTheme : uint32_t {
    NSKRY_UI_THEME_SYSTEM,
    NSKRY_UI_THEME_LIGHT,
    NSKRY_UI_THEME_DARK,
};

struct NskryUiPreferencesV1 {
    uint32_t structSize;
    // Resolved Windows UI language. A zero value means follow Windows.
    LANGID language;
    NskryUiTheme theme;
};

// Optional notification on the host UI thread before an action/settings call.
using NskrySetUiPreferencesV1 = void (NSKRY_CALL *)(const NskryUiPreferencesV1*);
inline constexpr char kNskrySetUiPreferencesV1[] = "nskry_plugin_set_ui_preferences_v1";

namespace nskry::plugin_ui {
inline thread_local NskryUiPreferencesV1 preferences{};
inline void SetPreferences(const NskryUiPreferencesV1* value) noexcept {
    preferences = value && value->structSize >= sizeof(preferences) ? *value : NskryUiPreferencesV1{};
}

inline const wchar_t* Text(const wchar_t* en, const wchar_t* zh) noexcept {
    const auto language = preferences.language ? preferences.language : ::GetUserDefaultUILanguage();
    return PRIMARYLANGID(language) == LANG_CHINESE ? zh : en;
}
} // namespace nskry::plugin_ui
