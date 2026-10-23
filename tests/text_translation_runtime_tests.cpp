#include "core/plugin_manager.h"
#include "test_artifacts.h"
#include <iostream>
#include <thread>

int wmain(int argc, wchar_t **argv) {
    if (argc != 5)
        return 2;
    nskry::test::Artifacts artifacts(argv[4], L"text-handoff");
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", artifacts.Path().c_str()))
        return 3;
    nskry::PluginRegistry registry;
    if (!registry.Initialize())
        return 4;
    const auto providerDll = std::filesystem::absolute(argv[1]);
    const auto clientDll = std::filesystem::absolute(argv[2]);
    nskry::PluginManifest provider;
    provider.manifestVersion = 1;
    provider.apiVersion = 2;
    provider.id = L"org.test.runtime";
    provider.name = L"Text provider";
    provider.version = L"1.0.0";
    provider.entry = providerDll.filename().wstring();
    provider.architecture = L"x64";
    provider.textTranslationApi = 1;
    auto client = provider;
    client.id = L"org.test.text-client";
    client.entry = clientDll.filename().wstring();
    client.textTranslationApi = 0;
    client.textTranslationClientApi = 1;
    if (!registry.UpsertInstalled(client, clientDll.parent_path().wstring(), nskry::PluginSource::ThirdParty))
        return 5;
    NskryHostContext context{sizeof(context), NSKRY_PLUGIN_API_VERSION};
    auto &runtime = nskry::PluginManager::Instance();
    runtime.Initialize(registry, context);
    struct Stop {
        nskry::PluginManager &runtime;
        ~Stop() { runtime.Shutdown(); }
    } stop{runtime};
    bool ok = !runtime.HasTextTranslation() && runtime.EnsureLoaded(client.id);
    const auto module = GetModuleHandleW(clientDll.filename().c_str());
    const auto getService = reinterpret_cast<const NskryTextTranslationServiceV1 *(NSKRY_CALL *)()>(
        GetProcAddress(module, "nskry_test_translation_service"));
    if (!getService)
        return 6;
    const auto *service = getService();
    ok = service->available && service->translate && !service->available() && ok;
    const wchar_t text[] = {L'中', L'文'}; // Borrowed, deliberately not NUL terminated.
    NskryTextTranslationV1 request{sizeof(request), 1, nullptr, text, 2};
    wchar_t error[64]{};
    NskryTranslationResultV1 result{sizeof(result), 1, error, 64};
    ok = service->translate(&request, &result) == NSKRY_TRANSLATION_UNAVAILABLE && error[0] && ok;
    ok = registry.UpsertInstalled(provider, providerDll.parent_path().wstring(),
                                  nskry::PluginSource::ThirdParty) &&
         ok;
    ok = service->available() && !runtime.IsLoaded(provider.id) &&
         !GetModuleHandleW(providerDll.filename().c_str()) && ok;
    int32_t background = 99;
    std::thread wrongThread([&] { background = service->translate(&request, &result); });
    wrongThread.join();
    ok = background == NSKRY_TRANSLATION_UNAVAILABLE && !runtime.IsLoaded(provider.id) && ok;
    request.version = 2;
    ok = service->translate(&request, &result) == NSKRY_TRANSLATION_INVALID &&
         !runtime.IsLoaded(provider.id) && ok;
    request.version = 1;
    ok = service->translate(&request, &result) == NSKRY_TRANSLATION_ACCEPTED &&
         runtime.IsLoaded(provider.id) && ok;
    const auto received = reinterpret_cast<const wchar_t *(NSKRY_CALL *)()>(
        GetProcAddress(GetModuleHandleW(providerDll.filename().c_str()), "nskry_test_translation_received"));
    ok = received && std::wstring(received()) == L"中文" && ok;
    for (const auto &[mode, expected] :
         {std::pair{L"busy", NSKRY_TRANSLATION_BUSY}, std::pair{L"throw", NSKRY_TRANSLATION_FAILED},
          std::pair{L"malformed", NSKRY_TRANSLATION_FAILED}}) {
        SetEnvironmentVariableW(L"NSKRY_TEST_TRANSLATION_RESULT", mode);
        ok = service->translate(&request, &result) == expected && error[0] && ok;
    }
    SetEnvironmentVariableW(L"NSKRY_TEST_TRANSLATION_RESULT", nullptr);
    // A claimed extension without its export must fail before init, not crash
    // or be treated as a successful handoff to an old plugin.
    runtime.Unload(provider.id);
    const auto legacyDll = std::filesystem::absolute(argv[3]);
    auto missingExport = provider;
    missingExport.entry = legacyDll.filename().wstring();
    ok = registry.UpsertInstalled(missingExport, legacyDll.parent_path().wstring(), nskry::PluginSource::ThirdParty) &&
        service->translate(&request, &result) == NSKRY_TRANSLATION_UNAVAILABLE &&
        !runtime.IsLoaded(provider.id) && !GetModuleHandleW(legacyDll.filename().c_str()) && ok;
    ok = registry.UpsertInstalled(provider, providerDll.parent_path().wstring(), nskry::PluginSource::ThirdParty) &&
        service->translate(&request, &result) == NSKRY_TRANSLATION_ACCEPTED && ok;
    ok = registry.SetEnabled(provider.id, false) && !service->available() &&
         service->translate(&request, &result) == NSKRY_TRANSLATION_UNAVAILABLE && ok;
    ok = registry.SetEnabled(provider.id, true) && service->available() && ok;
    auto incompatible = provider;
    incompatible.apiVersion = 1;
    ok = registry.UpsertInstalled(incompatible, providerDll.parent_path().wstring(),
                                  nskry::PluginSource::ThirdParty) &&
         !service->available() && ok;
    // Copy callback values before shutdown unloads the client DLL's service storage.
    const auto savedService = *service;
    runtime.Shutdown();
    ok = !savedService.available() &&
         savedService.translate(&request, &result) == NSKRY_TRANSLATION_UNAVAILABLE && ok;
    std::cout << "Metadata-only text discovery, lazy dispatch, thread/version/disable/exit gates=" << ok
              << '\n';
    return artifacts.Finish(ok) && ok ? 0 : 1;
}
