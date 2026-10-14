#include "core/plugin_manager.h"
#include <filesystem>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc!=4) return 2;
    const bool confirmed=argc==4 && !wcscmp(argv[3],L"v2");
    const bool legacy=argc==4 && !wcscmp(argv[3],L"legacy");
    using InputStatus=nskry::PluginEntry::InputReport::Status;
    // LOCALAPPDATA changes only in this test process. Never touch user plugins.
    const auto root = std::filesystem::path(argv[2]) /
        (L"plugin-runtime-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(root) || !SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str())) return 3;
    nskry::PluginRegistry registry;
    if (!registry.Initialize()) return 4;
    const auto dll = std::filesystem::absolute(argv[1]);
    nskry::PluginManifest manifest;
    manifest.manifestVersion = 1; manifest.id = L"org.test.runtime"; manifest.name = L"Runtime fixture";
    manifest.version = L"1.0.0"; manifest.entry = dll.filename().wstring(); manifest.architecture = L"x64";
    manifest.apiVersion = NSKRY_PLUGIN_API_VERSION; manifest.toolbarAction = manifest.postCapture = true;
    manifest.shortcuts.push_back({L"plugin.org.test.runtime.copy",L"Copy",L"Ctrl+C",manifest.id,manifest.name});
    manifest.shortcuts.push_back({L"plugin.org.test.runtime.launch",L"Launch",L"",manifest.id,manifest.name,nskry::input::Kind::Global});
    if(legacy)manifest.shortcuts.clear();
    if (!registry.UpsertInstalled(manifest, dll.parent_path().wstring(), nskry::PluginSource::ThirdParty)) return 5;
    auto& runtime = nskry::PluginManager::Instance();
    runtime.SetShortcutProvider([](const nskry::ShortcutDefinition&){return std::wstring(L"Ctrl+Shift+C");});
    NskryHostContext context{}; context.structSize = sizeof(context); context.apiVersion = NSKRY_PLUGIN_API_VERSION;
    runtime.Initialize(registry, context);
    struct ShutdownGuard { nskry::PluginManager& runtime; ~ShutdownGuard() { runtime.Shutdown(); } } guard{runtime};
    if (runtime.IsLoaded(manifest.id) || runtime.GetEnabledImageActions().size() != 1) return 6;
    if(runtime.GetInputReport(manifest.id).status!=InputStatus::NotLoaded || !runtime.RefreshInputConfiguration().empty())return 23;
    if (GetModuleHandleW(dll.filename().c_str())) return 14;
    HMODULE probe = LoadLibraryW(dll.c_str());
    struct ModuleGuard { HMODULE value; ~ModuleGuard() { if (value) FreeLibrary(value); } } probeGuard{probe};
    const auto shutdownCount = probe ? reinterpret_cast<int (NSKRY_CALL*)()>(GetProcAddress(probe, "nskry_test_shutdown_count")) : nullptr;
    if (!shutdownCount || !SetEnvironmentVariableW(L"NSKRY_TEST_FAIL_INIT", L"1")) return 15;
    const bool initFailureCleaned = !runtime.EnsureLoaded(manifest.id) && shutdownCount() == 1 && !runtime.IsLoaded(manifest.id);
    SetEnvironmentVariableW(L"NSKRY_TEST_FAIL_INIT", nullptr);
    if (!initFailureCleaned) return 16;
    if (!runtime.ExecutePlugin(manifest.id, context)) return 7;
    HMODULE module = GetModuleHandleW(dll.filename().c_str());
    const auto count = reinterpret_cast<int (NSKRY_CALL*)()>(GetProcAddress(module, "nskry_test_execute_count"));
    if (!count || count() != 1) return 8;
    const auto configured=reinterpret_cast<int (NSKRY_CALL*)()>(GetProcAddress(module,"nskry_test_configured"));
    if(!legacy && (!configured || !configured()))return 18;
    if(runtime.GetInputReport(manifest.id).status!=(confirmed?InputStatus::Applied:InputStatus::Unconfirmed))return 24;
    if(confirmed){
        for(const auto& [mode,status]:{std::pair{L"unsupported",InputStatus::Unsupported},std::pair{L"failed",InputStatus::Failed},std::pair{L"malformed",InputStatus::Failed}}){
            SetEnvironmentVariableW(L"NSKRY_TEST_INPUT_RESULT",mode);
            const auto report=runtime.RefreshInputConfiguration();
            if(report.size()!=1 || report.front().status!=status || report.front().message.empty())return 25;
        }
        SetEnvironmentVariableW(L"NSKRY_TEST_INPUT_RESULT",nullptr);
    }
    if(!SetEnvironmentVariableW(L"NSKRY_TEST_FAIL_INPUT",L"1"))return 19;
    const auto failures=runtime.RefreshInputConfiguration();
    SetEnvironmentVariableW(L"NSKRY_TEST_FAIL_INPUT",nullptr);
    if(failures.size()!=1 || failures[0].status!=(legacy?InputStatus::Unconfirmed:InputStatus::Failed))return 20;
    const auto recovered=runtime.RefreshInputConfiguration();
    if(recovered.size()!=1 || recovered[0].status!=(confirmed?InputStatus::Applied:InputStatus::Unconfirmed))return 26;
    const auto attributes = GetFileAttributesW(registry.RegistryPath().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !SetFileAttributesW(registry.RegistryPath().c_str(), attributes | FILE_ATTRIBUTE_READONLY)) return 12;
    const bool saveFailureRetainsState = !registry.SetEnabled(manifest.id, false) && registry.Find(manifest.id)->enabled;
    if (!SetFileAttributesW(registry.RegistryPath().c_str(), attributes) || !saveFailureRetainsState) return 13;
    if (!registry.SetEnabled(manifest.id, false) || runtime.EnsureLoaded(manifest.id) ||
        runtime.ExecutePlugin(manifest.id, context) || count() != 1 || !runtime.GetEnabledImageActions().empty()) return 9;
    if (!registry.SetEnabled(manifest.id, true) || !runtime.ExecutePlugin(manifest.id, context) || count() != 2) return 10;
    auto incompatible=manifest;incompatible.apiVersion=NSKRY_PLUGIN_API_VERSION-1;
    if(!registry.UpsertInstalled(incompatible,dll.parent_path().wstring(),nskry::PluginSource::ThirdParty))return 21;
    const auto editable=runtime.GetShortcutDefinitions();
    if(editable.size()!=(legacy?1:2) || editable[0].official || runtime.EnsureLoaded(manifest.id))return 22;
    runtime.Shutdown();
    if (runtime.IsLoaded(manifest.id) || registry.Find(manifest.id)->loaded || shutdownCount() != 2) return 11;
    FreeLibrary(probeGuard.value); probeGuard.value = nullptr;
    if (GetModuleHandleW(dll.filename().c_str())) return 17;
    std::cout << "Lazy loading, disabled-loaded rejection and re-enable passed.\n";
    return 0;
}
