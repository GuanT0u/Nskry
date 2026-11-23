#pragma once
#include "core/settings_manager.h"
#include "nskry_ui_preferences.h"

namespace nskry {
inline constexpr UINT WM_NSKRY_SETTINGS_DIRTY = WM_APP + 0x197;
inline constexpr UINT WM_NSKRY_REFRESH_UI = WM_APP + 0x198;
inline bool DesktopHighContrast() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON);
}

inline NskryUiPreferencesV1 DesktopUiPreferences() {
    const auto &settings = SettingsManager::Instance();
    const auto &language = settings.Language();
    const auto &theme = settings.Theme();
    const LANGID resolved = language == L"zh-CN" ? MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)
                          : language == L"en" ? MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)
                          : ::GetUserDefaultUILanguage();
    DWORD useLight = 1;
    DWORD bytes = sizeof(useLight);
    if (theme != L"light" && theme != L"dark")
        ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                       L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &useLight, &bytes);
    const bool dark = theme == L"dark" || (theme != L"light" && !useLight);
    return {sizeof(NskryUiPreferencesV1), resolved, dark ? NSKRY_UI_THEME_DARK : NSKRY_UI_THEME_LIGHT};
}

inline bool DesktopUiChinese() {
    const auto &language = SettingsManager::Instance().Language();
    return language == L"zh-CN" ||
           (language != L"en" && PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE);
}
inline const wchar_t* UiText(const wchar_t* en, const wchar_t* zh) {
    return DesktopUiChinese() ? zh : en;
}
struct DesktopPalette {
    COLORREF window{}, panel{}, hover{}, border{}, text{}, secondary{}, accent{}, onAccent{}, danger{}, onDanger{};
};

inline DesktopPalette GetDesktopPalette() {
    if (DesktopHighContrast()) {
        const auto face = ::GetSysColor(COLOR_BTNFACE);
        const auto text = ::GetSysColor(COLOR_BTNTEXT);
        const auto highlight = ::GetSysColor(COLOR_HIGHLIGHT);
        const auto highlightText = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
        return {::GetSysColor(COLOR_WINDOW), face, face, text, text, text,
                highlight, highlightText, face, text};
    }
    if (DesktopUiPreferences().theme == NSKRY_UI_THEME_DARK)
        return {RGB(32,32,32), RGB(43,43,43), RGB(60,60,60), RGB(131,131,131),
                RGB(245,245,245), RGB(198,198,198), RGB(160,205,255), RGB(24,24,24),
                RGB(255,179,184), RGB(32,32,32)};
    return {RGB(247,247,247), RGB(255,255,255), RGB(232,238,245), RGB(115,115,115),
            RGB(32,32,32), RGB(80,80,80), RGB(0,89,164), RGB(255,255,255),
            RGB(167,33,40), RGB(255,255,255)};
}
} // namespace nskry
