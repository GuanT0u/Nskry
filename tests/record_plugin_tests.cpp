#include "core/plugin_manager.h"
#include <filesystem>
#include <iostream>
namespace {
int captureCalls{};
HRESULT __cdecl Open(RECT, uint32_t, void **, NskryCaptureDescriptionV1 *, wchar_t *, uint32_t) {
    ++captureCalls;
    return E_FAIL;
}
HRESULT __cdecl Read(void *, uint8_t *, uint32_t) {
    ++captureCalls;
    return E_FAIL;
}
void __cdecl Close(void *) {
    ++captureCalls;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 4)
        return 2;
    const auto dll = std::filesystem::absolute(argv[1]);
    const auto root =
        std::filesystem::path(argv[3]) /
        (L"record-plugin-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(root);
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str()))
        return 3;
    nskry::PluginManifest manifest;
    std::wstring error;
    if (!nskry::PluginManifest::LoadFromFile(argv[2], manifest, &error))
        return 4;
    nskry::PluginRegistry registry;
    if (!registry.Initialize() ||
        !registry.UpsertInstalled(manifest, dll.parent_path().wstring(), nskry::PluginSource::Local))
        return 5;
    NskryHostContext context{sizeof(context), 2};
    auto &manager = nskry::PluginManager::Instance();
    manager.Initialize(registry, context);
    struct Cleanup {
        nskry::PluginManager &value;
        ~Cleanup() {
            value.Shutdown();
        }
    } cleanup{manager};
    auto rows = manager.GetShortcutDefinitions();
    if (rows.size() != 2 || !rows[1].pluginCommand || rows[1].kind != nskry::input::Kind::Global ||
        GetModuleHandleW(dll.filename().c_str()))
        return 6;
    if (manager.ExecuteCommand(manifest.id, L"stop", error) || manager.IsLoaded(manifest.id))
        return 7;
    // Required optional service absent: reject cleanly without init/capture.
    if (manager.EnsureLoaded(manifest.id) || GetModuleHandleW(dll.filename().c_str()))
        return 8;
    NskryCaptureServiceV1 service{sizeof(service), 1, Open, Read, Close};
    manager.SetCaptureService(service);
    if (!manager.EnsureLoaded(manifest.id) || captureCalls)
        return 9;
    if (manager.ExecuteCommand(manifest.id, L"stop", error) || error.empty() || captureCalls)
        return 10;
    auto module = GetModuleHandleW(dll.filename().c_str());
    auto connect = reinterpret_cast<NskryConnectCaptureV1>(GetProcAddress(module, "nskry_plugin_connect_capture_v1"));
    service.version = 99;
    if (!connect || connect(&service))
        return 11;
    manager.Shutdown();
    manager.Shutdown();
    if (captureCalls || GetModuleHandleW(dll.filename().c_str()))
        return 12;
    std::cout << "Record metadata-only discovery, optional extension rejection, lazy init (no capture/prewarm), "
                 "command and DLL cleanup passed.\n";
    return 0;
}
