#include "pch.h"
#include "core/json.h"
#include "core/settings_manager.h"

#include <fstream>
#include <sstream>

namespace nskry {
namespace {

const std::wstring kEmptyValue;

bool ReadOptionalString(const json::Value& object, std::wstring_view key, std::wstring& value) {
    const json::Value* member = object.Find(key);
    return !member || (member->AsString() && object.GetString(key, value));
}

bool ReadOptionalBool(const json::Value& object, std::wstring_view key, bool& value) {
    const json::Value* member = object.Find(key);
    return !member || (member->AsBool() && object.GetBool(key, value));
}
bool ReadVisualInt(const json::Value& object, std::wstring_view key, int& value, int maximum) {
    if (!object.Find(key)) return true;
    unsigned parsed{}; if (!object.GetUnsigned(key, parsed)) return false;
    value = static_cast<int>((std::min)(parsed, static_cast<unsigned>(maximum))); return true;
}

std::wstring GetLocalAppDataPath() {
    const DWORD required = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0) return {};
    std::wstring path(required, L'\0');
    ::GetEnvironmentVariableW(L"LOCALAPPDATA", path.data(), required);
    path.resize(required - 1);
    return path;
}

} // namespace

SettingsManager& SettingsManager::Instance() {
    static SettingsManager s_instance;
    return s_instance;
}

SettingsManager::SettingsManager() {
    const std::wstring localAppData = GetLocalAppDataPath();
    if (!localAppData.empty()) {
        m_configDirectory = localAppData + L"\\Nskry\\config";
        m_settingsPath = m_configDirectory + L"\\settings.json";
    }
    SetDefaults();
}

void SettingsManager::SetDefaults() {
    m_hotkeys.clear();
    for(const auto& row:CoreShortcuts()) m_hotkeys[row.id]=row.defaultShortcut;
    m_runAtStartup = false;
    m_notifications = true;
    m_showStartupHint = true;
    m_hideMonitorOnSourceFocus = false;
    m_visuals = {};
    m_interaction = {};
    m_theme = L"system";
    m_officialPluginCatalogUrl.clear();
    m_officialPluginsPageUrl.clear();
}

bool SettingsManager::EnsureConfigDirectory() const {
    if (m_configDirectory.empty()) return false;

    const std::wstring localAppData = GetLocalAppDataPath();
    if (localAppData.empty()) return false;
    const std::wstring appDirectory = localAppData + L"\\Nskry";
    if (!::CreateDirectoryW(appDirectory.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) return false;
    if (!::CreateDirectoryW(m_configDirectory.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) return false;
    return true;
}

bool SettingsManager::Load() {
    SetDefaults();
    if (m_settingsPath.empty()) return false;

    if (::GetFileAttributesW(m_settingsPath.c_str()) == INVALID_FILE_ATTRIBUTES) return Save();

    json::Value root;
    if (!json::ParseUtf8File(m_settingsPath, root) || !root.IsObject()) return false;

    const json::Value* hotkeys = root.Find(L"hotkeys");
    const json::Value::Object* hotkeyObject = hotkeys ? hotkeys->AsObject() : nullptr;
    if (!hotkeyObject) return false;
    for (const auto& [commandId, shortcutValue] : *hotkeyObject) {
        const std::wstring* shortcut = shortcutValue.AsString();
        if (!shortcut || commandId.empty()) return false;
        m_hotkeys[commandId] = *shortcut;
    }

    const json::Value* general = root.Find(L"general");
    if (general && !general->IsObject()) return false;
    const json::Value& generalObject = general ? *general : root; // Accept the early flat settings shape.
    if (!ReadOptionalBool(generalObject, L"run_at_startup", m_runAtStartup) ||
        !ReadOptionalBool(generalObject, L"notifications", m_notifications) ||
        !ReadOptionalBool(generalObject, L"show_startup_hint", m_showStartupHint) ||
        !ReadOptionalBool(generalObject, L"hide_monitor_on_source_focus", m_hideMonitorOnSourceFocus) ||
        !ReadOptionalString(generalObject, L"theme", m_theme)) return false;

    const json::Value* updates = root.Find(L"updates");
    if (updates && !updates->IsObject()) return false;
    const json::Value& updatesObject = updates ? *updates : root;
    if (!ReadOptionalString(updatesObject, L"official_plugin_catalog_url", m_officialPluginCatalogUrl)) return false;
    if (!ReadOptionalString(updatesObject, L"official_plugins_page_url", m_officialPluginsPageUrl)) return false;
    if (const auto* v = root.Find(L"window_visuals")) {
        if (!v->IsObject()) return false;
        int shadowPreset = 1, cornerPreset = 0, cornerMode = 0;
        if (!ReadOptionalBool(*v, L"shadow_enabled", m_visuals.shadowEnabled) ||
            !ReadOptionalBool(*v, L"corners_enabled", m_visuals.cornersEnabled) ||
            !ReadVisualInt(*v, L"shadow_preset", shadowPreset, 2) || !ReadVisualInt(*v, L"corner_preset", cornerPreset, 2) ||
            !ReadVisualInt(*v, L"corner_mode", cornerMode, 2) ||
            !ReadVisualInt(*v, L"shadow_opacity", m_visuals.shadowOpacity, 50) ||
            !ReadVisualInt(*v, L"shadow_blur", m_visuals.shadowBlur, 8) ||
            !ReadVisualInt(*v, L"shadow_range", m_visuals.shadowRange, 24) ||
            !ReadVisualInt(*v, L"shadow_offset", m_visuals.shadowOffset, 6) ||
            !ReadVisualInt(*v, L"corner_radius", m_visuals.cornerRadius, 24) ||
            !ReadVisualInt(*v, L"corner_percent", m_visuals.cornerPercent, 15)) return false;
        m_visuals.shadowPreset = static_cast<VisualPreset>(shadowPreset);
        m_visuals.cornerPreset = static_cast<VisualPreset>(cornerPreset); m_visuals.cornerMode = static_cast<CornerMode>(cornerMode);
    }
    if(const auto* v=root.Find(L"interaction")) {
        if(!v->IsObject())return false;
        int borderColor=int(m_interaction.borderColor), cutColor=int(m_interaction.cutColor);
        if(!ReadOptionalBool(*v,L"dashed",m_interaction.dashed) || !ReadOptionalBool(*v,L"cut_edges",m_interaction.cutEdges) ||
            !ReadVisualInt(*v,L"border_width",m_interaction.borderWidth,6) ||
            !ReadVisualInt(*v,L"border_opacity",m_interaction.borderOpacity,100) ||
            !ReadVisualInt(*v,L"border_color",borderColor,0xffffff) ||
            !ReadVisualInt(*v,L"cut_style",m_interaction.cutStyle,3) ||
            !ReadVisualInt(*v,L"cut_opacity",m_interaction.cutOpacity,100) ||
            !ReadVisualInt(*v,L"cut_color",cutColor,0xffffff))return false;
        m_interaction.borderColor=COLORREF(borderColor);m_interaction.cutColor=COLORREF(cutColor);
        for(size_t i=0;i<m_interaction.cursors.size();++i) {
            int style=int(m_interaction.cursors[i].style);
            if(!ReadVisualInt(*v,L"cursor_"+std::to_wstring(i),style,6) ||
               !ReadOptionalString(*v,L"cursor_file_"+std::to_wstring(i),m_interaction.cursors[i].file))return false;
            m_interaction.cursors[i].style=CursorStyle(style);
        }
        m_interaction=NormalizeInteractionStyle(m_interaction);
    }
    return true;
}

bool SettingsManager::Save() const {
    if (!EnsureConfigDirectory() || m_settingsPath.empty()) return false;

    std::wostringstream output;
    output << L"{\n"
           << L"  \"general\": {\n"
           << L"    \"run_at_startup\": " << (m_runAtStartup ? L"true" : L"false") << L",\n"
           << L"    \"notifications\": " << (m_notifications ? L"true" : L"false") << L",\n"
           << L"    \"show_startup_hint\": " << (m_showStartupHint ? L"true" : L"false") << L",\n"
           << L"    \"hide_monitor_on_source_focus\": " << (m_hideMonitorOnSourceFocus ? L"true" : L"false") << L",\n"
           << L"    \"theme\": \"" << json::EscapeString(m_theme) << L"\"\n"
           << L"  },\n"
           << L"  \"updates\": {\n"
           << L"    \"official_plugin_catalog_url\": \"" << json::EscapeString(m_officialPluginCatalogUrl) << L"\",\n"
           << L"    \"official_plugins_page_url\": \"" << json::EscapeString(m_officialPluginsPageUrl) << L"\"\n"
           << L"  },\n"
           << L"  \"window_visuals\": {\n"
           << L"    \"shadow_enabled\": " << (m_visuals.shadowEnabled ? L"true" : L"false") << L",\n"
           << L"    \"corners_enabled\": " << (m_visuals.cornersEnabled ? L"true" : L"false") << L",\n"
           << L"    \"shadow_preset\": " << static_cast<int>(m_visuals.shadowPreset) << L",\n"
           << L"    \"corner_preset\": " << static_cast<int>(m_visuals.cornerPreset) << L",\n"
           << L"    \"corner_mode\": " << static_cast<int>(m_visuals.cornerMode) << L",\n"
           << L"    \"shadow_opacity\": " << m_visuals.shadowOpacity << L",\n"
           << L"    \"shadow_blur\": " << m_visuals.shadowBlur << L",\n"
           << L"    \"shadow_range\": " << m_visuals.shadowRange << L",\n"
           << L"    \"shadow_offset\": " << m_visuals.shadowOffset << L",\n"
           << L"    \"corner_radius\": " << m_visuals.cornerRadius << L",\n"
           << L"    \"corner_percent\": " << m_visuals.cornerPercent << L"\n  },\n"
           << L"  \"interaction\": {\n"
           << L"    \"dashed\": " << (m_interaction.dashed?L"true":L"false") << L",\n"
           << L"    \"border_width\": " << m_interaction.borderWidth << L",\n"
           << L"    \"border_opacity\": " << m_interaction.borderOpacity << L",\n"
           << L"    \"border_color\": " << m_interaction.borderColor << L",\n"
           << L"    \"cut_style\": " << m_interaction.cutStyle << L",\n"
           << L"    \"cut_opacity\": " << m_interaction.cutOpacity << L",\n"
           << L"    \"cut_color\": " << m_interaction.cutColor << L",\n"
           << L"    \"cut_edges\": " << (m_interaction.cutEdges?L"true":L"false");
    for(size_t i=0;i<m_interaction.cursors.size();++i)
        output<<L",\n    \"cursor_"<<i<<L"\": "<<int(m_interaction.cursors[i].style)
            <<L",\n    \"cursor_file_"<<i<<L"\": \""<<json::EscapeString(m_interaction.cursors[i].file)<<L"\"";
    output<<L"\n  },\n  \"hotkeys\": {\n";

    bool first = true;
    for (const auto& [commandId, shortcut] : m_hotkeys) {
        if (!first) output << L",\n";
        output << L"    \"" << json::EscapeString(commandId) << L"\": \"" << json::EscapeString(shortcut) << L"\"";
        first = false;
    }
    output << L"\n  }\n}\n";
    // Preserve future/extension fields on demand, without keeping a second JSON
    // tree resident. Only implemented settings override existing values.
    json::Value defaults, existing, changes;
    if (!json::Parse(LR"({"capture":{"default_save_directory":"","tray_double_click":"capture"},
        "updates":{"check_automatically":true,"include_prerelease":false}})", defaults) ||
        !json::Parse(output.str(), changes)) return false;
    const auto attributes = GetFileAttributesW(m_settingsPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
    } else if (!json::ParseUtf8File(m_settingsPath, existing) || !existing.IsObject()) {
        return false; // Never overwrite an unreadable/corrupt configuration with defaults.
    }
    std::wstring merged;
    if (!json::Serialize(json::MergeObjects(json::MergeObjects(defaults, existing.IsNull() ? defaults : existing), changes), merged))
        return false;
    std::string bytes;
    if (!json::ToUtf8(merged + L"\n", bytes)) return false;
    if(bytes.size()>MAXDWORD)return false;
    const auto temporary=m_settingsPath+L".tmp-"+std::to_wstring(GetCurrentProcessId());
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written{};const bool complete=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr) &&
        written==bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if(complete && MoveFileExW(temporary.c_str(),m_settingsPath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;
    DeleteFileW(temporary.c_str());return false;
}

const std::wstring& SettingsManager::GetHotkey(const std::wstring& commandId) const {
    const auto it = m_hotkeys.find(commandId);
    return it == m_hotkeys.end() ? kEmptyValue : it->second;
}

const std::wstring& SettingsManager::GetShortcutValue(const ShortcutDefinition& definition) const {
    const auto it = m_hotkeys.find(definition.id);
    return it == m_hotkeys.end() ? definition.defaultShortcut : it->second;
}

void SettingsManager::SetHotkey(const std::wstring& commandId, std::wstring shortcut) {
    if (commandId.empty()) return;
    m_hotkeys[commandId] = std::move(shortcut);
}

UserSettings SettingsManager::GetUserSettings() const {
    UserSettings result;
    result.runAtStartup = m_runAtStartup;
    result.notifications = m_notifications;
    result.showStartupHint = m_showStartupHint;
    result.hideMonitorOnSourceFocus = m_hideMonitorOnSourceFocus;
    result.visuals = m_visuals;
    result.interaction=m_interaction; result.shortcuts=m_hotkeys;
    result.theme = m_theme;
    result.captureShortcut = GetHotkey(L"core.capture");
    result.exitShortcut = GetHotkey(L"core.exit");
    return result;
}

void SettingsManager::SetUserSettings(const UserSettings& settings) {
    m_runAtStartup = settings.runAtStartup;
    m_notifications = settings.notifications;
    m_showStartupHint = settings.showStartupHint;
    m_hideMonitorOnSourceFocus = settings.hideMonitorOnSourceFocus;
    m_visuals = NormalizeVisualSettings(settings.visuals);
    m_interaction=NormalizeInteractionStyle(settings.interaction);
    m_hotkeys=settings.shortcuts;
    m_theme = settings.theme;
    SetHotkey(L"core.capture", settings.captureShortcut);
    SetHotkey(L"core.exit", settings.exitShortcut);
}

} // namespace nskry
