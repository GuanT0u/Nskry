#include "core/plugin_manager.h"
#include "pch.h"
#include <stdexcept>

namespace nskry {

PluginManager &PluginManager::Instance() {
    static PluginManager s_instance;
    return s_instance;
}

void PluginManager::Initialize(PluginRegistry &registry, const NskryHostContext &hostContext) {
    if (m_initialized)
        return;
    m_registry = &registry;
    m_hostContext = hostContext;
    m_uiThread = GetCurrentThreadId();
    m_initialized = true;

    // Startup is deliberately opt-in. All normal plugins remain installed and
    // enabled, but consume no DLL/runtime memory until their command is used.
    for (const PluginRecord *record : m_registry->GetAll()) {
        if (record->enabled && record->manifest.loadPolicy == PluginLoadPolicy::Startup) {
            EnsureLoaded(record->manifest.id);
        }
    }
}

void PluginManager::Shutdown() {
    m_initialized = false; // No new text handoffs from closing clients.
    ReleaseImageRecognition(m_imageTicket); // Join before any DLL is unloaded, regardless of map order.
    for (auto it = m_plugins.begin(); it != m_plugins.end();) {
        const std::wstring id = it->first;
        ++it;
        Unload(id);
    }
    m_plugins.clear();
    m_registry = nullptr;
    m_hostContext = {};
    m_captureService = {};
    m_captureServiceV2 = {};
    m_initialized = false;
    m_uiThread = 0;
    m_graphicsProvider = {};
}

bool PluginManager::EnsureLoaded(const std::wstring &pluginId) {
    if (!m_initialized || !m_registry || pluginId.empty())
        return false;
    const PluginRecord *record = m_registry->Find(pluginId);
    if (!record || !record->enabled || !record->manifest.IsCompatibleWithHost())
        return false;
    // Existing windows may still hold an action snapshot after disable. A
    // loaded DLL is retained for safe shutdown, not permission to execute it.
    if (m_plugins.contains(pluginId))
        return true;
    return LoadPlugin(*record);
}

bool PluginManager::LoadPlugin(const PluginRecord &record) {
    const std::wstring dllPath = record.installPath + L"\\" + record.manifest.entry;
    const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    HMODULE module = ::LoadLibraryExW(dllPath.c_str(), nullptr, flags);
    if (!module)
        return false;

    const auto fnInfo = reinterpret_cast<decltype(&nskry_plugin_info)>(::GetProcAddress(module, "nskry_plugin_info"));
    const auto fnInit = reinterpret_cast<decltype(&nskry_plugin_init)>(::GetProcAddress(module, "nskry_plugin_init"));
    const auto fnExecute =
        reinterpret_cast<decltype(&nskry_plugin_execute)>(::GetProcAddress(module, "nskry_plugin_execute"));
    const auto fnShutdown =
        reinterpret_cast<decltype(&nskry_plugin_shutdown)>(::GetProcAddress(module, "nskry_plugin_shutdown"));
    const auto configureInput =
        reinterpret_cast<NskryConfigureInputV1>(::GetProcAddress(module, "nskry_plugin_configure_input_v1"));
    const auto configureInputV2 =
        reinterpret_cast<NskryConfigureInputV2>(::GetProcAddress(module, "nskry_plugin_configure_input_v2"));
    const auto connectCapture =
        reinterpret_cast<NskryConnectCaptureV1>(::GetProcAddress(module, "nskry_plugin_connect_capture_v1"));
    const auto connectCaptureV2 =
        reinterpret_cast<NskryConnectCaptureV2>(::GetProcAddress(module, "nskry_plugin_connect_capture_v2"));
    const auto command =
        reinterpret_cast<NskryCommandHandlerV1>(::GetProcAddress(module, "nskry_plugin_command_v1"));
    const auto captureAction = reinterpret_cast<NskryExecuteCaptureActionV1>(
        ::GetProcAddress(module, "nskry_plugin_capture_action_v1"));
    const auto showSettings = reinterpret_cast<NskryShowSettingsV1>(
        ::GetProcAddress(module, "nskry_plugin_show_settings_v1"));
    const auto translateText = reinterpret_cast<NskryTranslateTextV1>(
        ::GetProcAddress(module, "nskry_plugin_translate_text_v1"));
    const auto connectTranslation = reinterpret_cast<NskryConnectTextTranslationV1>(
        ::GetProcAddress(module, "nskry_plugin_connect_text_translation_v1"));
    const auto recognizeImage = reinterpret_cast<NskryRecognizeImageV1>(
        ::GetProcAddress(module, "nskry_plugin_recognize_image_v1"));
    const auto connectRecognition = reinterpret_cast<NskryConnectImageRecognitionV1>(
        ::GetProcAddress(module, "nskry_plugin_connect_image_recognition_v1"));
    if ((record.manifest.imageRecognitionApi && !recognizeImage) ||
        (record.manifest.imageRecognitionClientApi && !connectRecognition)) {
        ::FreeLibrary(module);
        return false;
    }
    if ((record.manifest.textTranslationApi && !translateText) ||
        (record.manifest.textTranslationClientApi && !connectTranslation)) {
        ::FreeLibrary(module);
        return false;
    }
    if ((record.manifest.actionApi && !captureAction) || (record.manifest.settingsUiApi && !showSettings)) {
        ::FreeLibrary(module);
        return false;
    }
    if ((record.manifest.commandApi && !command) ||
        (record.manifest.captureApi == 1 && (!connectCapture || m_captureService.version != 1)) ||
        (record.manifest.captureApi == 2 && (!connectCaptureV2 || m_captureServiceV2.version != 2))) {
        ::FreeLibrary(module);
        return false;
    }
    const bool localInput = std::any_of(record.manifest.shortcuts.begin(), record.manifest.shortcuts.end(),
                                        [](const ShortcutDefinition &row) { return row.kind != input::Kind::Global; });
    if (localInput && !configureInput && !configureInputV2) {
        ::FreeLibrary(module);
        return false;
    }
    if (!fnInfo || !fnInit || !fnExecute || !fnShutdown) {
        ::FreeLibrary(module);
        return false;
    }

    // This check is runtime ABI validation, not metadata discovery: all data
    // displayed before load still comes exclusively from manifest.json.
    const NskryPluginInfo *runtimeInfo = nullptr;
    bool initialized = false;
    bool initAttempted = false;
    try {
        runtimeInfo = fnInfo();
        initialized = runtimeInfo && runtimeInfo->structSize >= sizeof(NskryPluginInfo) &&
                      runtimeInfo->apiVersion == NSKRY_PLUGIN_API_VERSION && runtimeInfo->id && runtimeInfo->version &&
                      record.manifest.apiVersion == runtimeInfo->apiVersion && record.manifest.id == runtimeInfo->id &&
                      record.manifest.version == runtimeInfo->version;
        if (initialized) {
            if (m_graphicsProvider)
                m_graphicsProvider(m_hostContext);
            initAttempted = true;
            if (record.manifest.imageRecognitionClientApi) {
                const NskryImageRecognitionServiceV1 service{sizeof(service), 1,
                    [](const wchar_t* id) -> int32_t { return PluginManager::Instance().HasImageRecognition(id); },
                    [](const wchar_t* id, HBITMAP image, uint64_t* ticket) -> int32_t {
                        return PluginManager::Instance().BeginImageRecognition(id, image, ticket);
                    },
                    [](uint64_t ticket, NskryImageTextV1* result) -> int32_t {
                        return PluginManager::Instance().PollImageRecognition(ticket, result);
                    },
                    [](uint64_t ticket) { PluginManager::Instance().CancelImageRecognition(ticket); },
                    [](uint64_t ticket) { PluginManager::Instance().ReleaseImageRecognition(ticket); }};
                if (!connectRecognition(&service)) throw std::runtime_error("Image recognition extension rejected");
            }
            if (record.manifest.textTranslationClientApi) {
                const NskryTextTranslationServiceV1 service{sizeof(service), 1,
                    []() -> int32_t { return PluginManager::Instance().HasTextTranslation() ? 1 : 0; },
                    [](const NskryTextTranslationV1 *request, NskryTranslationResultV1 *result) -> int32_t {
                        return PluginManager::Instance().TranslateText(request, result);
                    }};
                if (!connectTranslation(&service)) throw std::runtime_error("Text translation extension rejected");
            }
            if ((record.manifest.captureApi == 1 && !connectCapture(&m_captureService)) ||
                (record.manifest.captureApi == 2 && !connectCaptureV2(&m_captureServiceV2)))
                throw std::runtime_error("Capture extension rejected");
            initialized = fnInit(&m_hostContext) != 0;
        }
    } catch (...) {
        initialized = false;
    }
    if (!initialized) {
        // A partially initialized plugin may already own windows/threads.
        // Give it its cleanup contract before releasing the DLL reference.
        if (initAttempted) {
            try {
                fnShutdown();
            } catch (...) {
            }
        }
        ::FreeLibrary(module);
        return false;
    }

    PluginEntry entry;
    entry.fnCaptureAction = captureAction;
    entry.fnShowSettings = showSettings;
    entry.fnTranslateText = translateText;
    entry.fnRecognizeImage = record.manifest.imageRecognitionApi ? recognizeImage : nullptr;
    entry.hModule = module;
    entry.id = record.manifest.id;
    entry.fnExecute = fnExecute;
    entry.fnShutdown = fnShutdown;
    entry.fnConfigureInput = configureInput;
    entry.fnConfigureInputV2 = configureInputV2;
    entry.fnCommand = command;
    try {
        entry.inputReport = ConfigureInput(entry, record);
    } catch (...) {
        try {
            fnShutdown();
        } catch (...) {
        }
        ::FreeLibrary(module);
        return false;
    }
    m_plugins.emplace(entry.id, std::move(entry));
    m_registry->SetLoaded(record.manifest.id, true);
    return true;
}

bool PluginManager::Unload(const std::wstring &pluginId) {
    const auto it = m_plugins.find(pluginId);
    if (it == m_plugins.end())
        return false;
    if (m_imageProvider == pluginId) ReleaseImageRecognition(m_imageTicket);
    if (it->second.fnShutdown) {
        try {
            it->second.fnShutdown();
        } catch (...) {
        }
    }
    if (it->second.hModule)
        ::FreeLibrary(it->second.hModule);
    m_plugins.erase(it);
    if (m_registry)
        m_registry->SetLoaded(pluginId, false);
    return true;
}

bool PluginManager::ExecutePlugin(const std::wstring &pluginId, const NskryHostContext &hostContext,
                                  const std::wstring &actionId) {
    if (!actionId.empty()) {
        const auto *record = m_registry ? m_registry->Find(pluginId) : nullptr;
        if (!record || std::none_of(record->manifest.captureActions.begin(), record->manifest.captureActions.end(),
                                   [&](const auto &a) { return a.id == actionId; }))
            return false;
    }
    if (!EnsureLoaded(pluginId))
        return false;
    const auto it = m_plugins.find(pluginId);
    if (it == m_plugins.end() || !it->second.fnExecute)
        return false;
    try {
        auto context = hostContext;
        if (m_graphicsProvider)
            m_graphicsProvider(context);
        if (!actionId.empty()) {
            NskryCaptureActionV1 action{sizeof(action), 1, actionId.c_str(), &context};
            return it->second.fnCaptureAction && it->second.fnCaptureAction(&action) != 0;
        }
        it->second.fnExecute(&context);
        return true;
    } catch (...) {
        return false;
    }
}

bool PluginManager::ShowPluginSettings(const std::wstring &id, HWND owner) {
    const auto *record = m_registry ? m_registry->Find(id) : nullptr;
    if (!record || !record->enabled || record->manifest.settingsUiApi != 1 || !EnsureLoaded(id))
        return false;
    const auto it = m_plugins.find(id);
    if (it == m_plugins.end() || !it->second.fnShowSettings)
        return false;
    try { return it->second.fnShowSettings(owner) != 0; }
    catch (...) { return false; }
}
bool PluginManager::IsLoaded(const std::wstring &pluginId) const {
    return m_plugins.contains(pluginId);
}

bool PluginManager::HasTextTranslation() const {
    if (GetCurrentThreadId() != m_uiThread.load() || !m_initialized || !m_registry) return false;
    try {
        for (const auto *record : m_registry->GetAll())
            if (record->enabled && record->manifest.textTranslationApi == 1 && record->manifest.IsCompatibleWithHost())
                return true;
    } catch (...) {} // Service callbacks must not throw across the plugin ABI.
    return false;
}

int32_t PluginManager::TranslateText(const NskryTextTranslationV1 *request, NskryTranslationResultV1 *result) {
    if (result && (result->structSize < sizeof(*result) || result->version != 1)) return NSKRY_TRANSLATION_INVALID;
    NskryTranslationError(result, L"");
    if (!NskryValidTranslation(request)) {
        NskryTranslationError(result, L"Enter nonempty valid text within 16000 UTF-16 units.");
        return NSKRY_TRANSLATION_INVALID;
    }
    if (!HasTextTranslation()) {
        NskryTranslationError(result, L"No enabled compatible text translation plugin. Update Core and enable/install a provider.");
        return NSKRY_TRANSLATION_UNAVAILABLE;
    }
    if (m_textDispatching) {
        NskryTranslationError(result, L"A translation handoff is being selected. Wait or cancel the provider menu.");
        return NSKRY_TRANSLATION_BUSY;
    }
    m_textDispatching = true;
    struct Dispatch { bool &value; ~Dispatch() { value = false; } } dispatch{m_textDispatching};
    try {
        const std::wstring copiedText(request->text, request->textLength);
        auto copiedRequest = *request;
        copiedRequest.text = copiedText.c_str();
        std::vector<std::pair<std::wstring, std::wstring>> providers;
        for (const auto *record : m_registry->GetAll())
            if (record->enabled && record->manifest.textTranslationApi == 1 && record->manifest.IsCompatibleWithHost())
                providers.emplace_back(record->manifest.id, record->manifest.name);
        size_t selected = 0;
        if (providers.size() > 1) {
            HMENU menu = CreatePopupMenu();
            if (!menu) return NSKRY_TRANSLATION_FAILED;
            struct Menu { HMENU value; ~Menu() { DestroyMenu(value); } } guard{menu};
            const size_t count = (std::min)(providers.size(), size_t(100));
            for (size_t i = 0; i < count; ++i) AppendMenuW(menu, MF_STRING, UINT_PTR(i + 1), providers[i].second.c_str());
            POINT point{}; GetCursorPos(&point);
            const auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                point.x, point.y, 0, request->owner ? request->owner : m_hostContext.mainHwnd, nullptr);
            if (!choice || choice > count) return NSKRY_TRANSLATION_CANCELED;
            selected = choice - 1;
        }
        // Registry may change while the provider menu pumps messages.
        const auto id = providers[selected].first;
        const auto *record = m_registry ? m_registry->Find(id) : nullptr;
        if (!m_initialized || !record || record->manifest.textTranslationApi != 1 || !EnsureLoaded(id)) {
            NskryTranslationError(result, L"The selected translation provider is unavailable or could not load.");
            return NSKRY_TRANSLATION_UNAVAILABLE;
        }
        const auto it = m_plugins.find(id);
        if (it == m_plugins.end() || !it->second.fnTranslateText) return NSKRY_TRANSLATION_UNAVAILABLE;
        const auto status = it->second.fnTranslateText(&copiedRequest, result);
        if (status >= NSKRY_TRANSLATION_CANCELED && status <= NSKRY_TRANSLATION_ACCEPTED) return status;
    } catch (...) {}
    NskryTranslationError(result, L"The translation provider failed to accept the request.");
    return NSKRY_TRANSLATION_FAILED;
}

bool PluginManager::ExecuteCommand(const std::wstring &id, const std::wstring &local, std::wstring &error) {
    error.clear();
    const auto loaded = m_plugins.find(id);
    const auto *record = m_registry ? m_registry->Find(id) : nullptr;
    if (!record || record->manifest.commandApi != 1 || loaded == m_plugins.end() || !loaded->second.fnCommand) {
        error = L"No active plugin session for this command.";
        return false;
    }
    const auto full = L"plugin." + id + L"." + local;
    if (std::none_of(record->manifest.shortcuts.begin(), record->manifest.shortcuts.end(),
                     [&](const auto &row) { return row.pluginCommand && row.id == full; })) {
        error = L"Undeclared plugin command.";
        return false;
    }
    wchar_t text[512]{};
    NskryCommandV1 request{sizeof(request), 1, local.c_str()};
    NskryCommandResultV1 result{sizeof(result), 1, text, 512};
    int32_t status = NSKRY_COMMAND_FAILED;
    try {
        status = loaded->second.fnCommand(&request, &result);
    } catch (...) {
    }
    text[511] = 0;
    if (status == NSKRY_COMMAND_OK)
        return true;
    error = text[0] ? text : L"Plugin command failed or is unsupported.";
    return false;
}

std::vector<ShortcutDefinition> PluginManager::GetShortcutDefinitions() const {
    std::vector<ShortcutDefinition> out;
    if (!m_registry)
        return out;
    for (const auto *record : m_registry->GetAll()) {
        // Editing metadata is safe even for disabled/incompatible packages.
        // EnsureLoaded/action enumeration remains the execution compatibility gate.
        auto rows = record->manifest.shortcuts;
        if (rows.empty() && record->manifest.toolbarAction)
            rows.push_back({L"plugin." + record->manifest.id + L".launch", L"Capture and run " + record->manifest.name,
                            L"", record->manifest.id, record->manifest.name, input::Kind::Global, false, true});
        for (auto &row : rows) {
            row.official = record->source == PluginSource::Official;
            out.push_back(std::move(row));
        }
    }
    return out;
}
PluginEntry::InputReport PluginManager::ConfigureInput(PluginEntry &entry, const PluginRecord &record) {
    using S = PluginEntry::InputReport::Status;
    PluginEntry::InputReport report{record.manifest.id, L"Not confirmed: plugin provides no acknowledgment.",
                                    S::Unconfirmed};
    if (!entry.fnConfigureInput && !entry.fnConfigureInputV2)
        return report;
    const auto &rows = record.manifest.shortcuts;
    std::vector<std::wstring> values;
    std::vector<NskryInputBindingV1> bindings;
    values.reserve(rows.size());
    bindings.reserve(rows.size());
    for (const auto &row : rows)
        values.push_back(row.kind == input::Kind::Global
                             ? L""
                             : (m_shortcutProvider ? m_shortcutProvider(row) : row.defaultShortcut));
    const auto prefix = L"plugin." + record.manifest.id + L".";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind == input::Kind::Global)
            continue; // Capture launch and explicit global commands belong to the host.
        if (!rows[i].id.starts_with(prefix))
            throw std::invalid_argument("Invalid plugin shortcut namespace");
        bindings.push_back({rows[i].id.c_str() + prefix.size(), values[i].c_str()});
    }
    NskryInputConfigV1 config{sizeof(config), 1, uint32_t(bindings.size()), bindings.data()};
    if (entry.fnConfigureInputV2) {
        wchar_t error[512]{};
        NskryInputResultV2 result{sizeof(result), 2, error, 512};
        const auto status = entry.fnConfigureInputV2(&config, &result);
        error[511] = 0;
        report.message = error;
        switch (status) {
        case NSKRY_INPUT_APPLIED:
            report.status = S::Applied;
            report.message = L"Confirmed applied by plugin.";
            break;
        case NSKRY_INPUT_UNSUPPORTED:
            report.status = S::Unsupported;
            if (report.message.empty())
                report.message = L"Plugin does not support this input configuration.";
            break;
        case NSKRY_INPUT_FAILED:
            report.status = S::Failed;
            if (report.message.empty())
                report.message = L"Plugin rejected input configuration.";
            break;
        default:
            report.status = S::Failed;
            report.message = L"Plugin returned an invalid input acknowledgment.";
            break;
        }
    } else
        entry.fnConfigureInput(&config);
    return report;
}
std::vector<PluginEntry::InputReport> PluginManager::RefreshInputConfiguration() {
    std::vector<PluginEntry::InputReport> reports;
    if (!m_registry)
        return reports;
    for (auto &[id, entry] : m_plugins)
        if (const auto *record = m_registry->Find(id)) {
            try {
                entry.inputReport = ConfigureInput(entry, *record);
            } catch (...) {
                entry.inputReport = {id, L"Plugin input configuration threw an exception.",
                                     PluginEntry::InputReport::Status::Failed};
            }
            reports.push_back(entry.inputReport);
        }
    return reports;
}
PluginEntry::InputReport PluginManager::GetInputReport(const std::wstring &id) const {
    auto entry = m_plugins.find(id);
    return entry == m_plugins.end() ? PluginEntry::InputReport{id, L"Not loaded: confirmation has not been queried.",
                                                               PluginEntry::InputReport::Status::NotLoaded}
                                    : entry->second.inputReport;
}

std::vector<PluginToolbarAction> PluginManager::GetEnabledToolbarActions() const {
    std::vector<PluginToolbarAction> actions;
    if (!m_registry)
        return actions;
    for (const PluginRecord *record : m_registry->GetAll()) {
        if (record->enabled && record->manifest.toolbarAction && record->manifest.IsCompatibleWithHost()) {
            actions.push_back({record->manifest.id, record->manifest.name});
            for (const auto &a : record->manifest.captureActions)
                actions.push_back({record->manifest.id, a.label, a.id});
        }
    }
    return actions;
}

std::vector<PluginImageAction> PluginManager::GetEnabledImageActions() const {
    std::vector<PluginImageAction> actions;
    if (!m_registry)
        return actions;
    for (const PluginRecord *record : m_registry->GetAll()) {
        if (record->enabled && record->manifest.postCapture && record->manifest.IsCompatibleWithHost())
            actions.push_back({record->manifest.id, record->manifest.name});
    }
    return actions;
}

} // namespace nskry
