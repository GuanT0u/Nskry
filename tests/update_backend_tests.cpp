#include "core/plugin_update_manager.h"
#include "update/package_verifier.h"
#include <filesystem>
#include <iostream>
using namespace nskry;
bool Check(bool ok, const char *message) {
    if (!ok)
        std::cerr << message << '\n';
    return ok;
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 3)
        return 2;
    const auto base = std::filesystem::absolute(argv[2]);
    const auto root =
        base / (L"update-backend-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(root) || !SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str()))
        return 3;
    struct Cleanup {
        std::filesystem::path root, base;
        ~Cleanup() {
            if (root.parent_path() == base)
                std::filesystem::remove_all(root);
        }
    } cleanup{root, base};
    bool ok = true;
    PluginRegistry registry;
    ok &= Check(registry.Initialize(), "test registry init");
    PluginPackageManager packages(registry);
    ok &= Check(packages.Initialize(), "package init");
    PluginUpdateManager manager(registry, packages);
    std::wstring error;
    PluginManifest installed;
    installed.manifestVersion = 1;
    installed.id = L"org.test.update";
    installed.name = L"Update fixture";
    installed.version = L"1.0.0";
    installed.entry = L"fixture.dll";
    installed.architecture = L"x64";
    installed.apiVersion = 2;
    installed.toolbarAction = true;
    ok &= Check(
        registry.UpsertInstalled(installed, registry.PluginRoot() + L"\\org.test.update", PluginSource::ThirdParty),
        "test install metadata");
    // Work captures values before mutation, with no URL/network necessary.
    auto check = manager.MakeCheckWork(L"");
    auto changed = installed;
    changed.name = L"Changed metadata";
    registry.UpsertInstalled(changed, registry.PluginRoot() + L"\\org.test.update", PluginSource::ThirdParty);
    const auto result = check({});
    ok &= Check(result.message.find(L"Update fixture") != std::wstring::npos &&
                    result.message.find(L"Changed metadata") == std::wstring::npos,
                "background check consumes immutable registry snapshot");
    registry.UpsertInstalled(installed, registry.PluginRoot() + L"\\org.test.update", PluginSource::ThirdParty);
    std::stop_source stop;
    stop.request_stop();
    VerifiedPackage verified;
    ok &= Check(!PackageVerifier::VerifyNskryPluginArchive(argv[1], verified, &error, stop.get_token()) &&
                    !PluginPackageManager::PreparePackage(argv[1], registry.AppRoot(), &error, stop.get_token()),
                "cancelled verification/preparation");
    DownloadedUpdate download;
    download.package = PluginPackageManager::PreparePackage(argv[1], registry.AppRoot(), &error);
    if (!download.package) {
        std::wcerr << error;
        return 4;
    }
    PluginUpdate update;
    update.pluginId = installed.id;
    update.currentVersion = L"1.0.0";
    update.version = L"1.1.0";
    update.source = PluginSource::ThirdParty;
    update.packageUrl = L"https://invalid.example/update.nskryplugin";
    changed = installed;
    changed.version = L"1.2.0";
    registry.UpsertInstalled(changed, registry.PluginRoot() + L"\\org.test.update", PluginSource::ThirdParty);
    ok &= Check(!manager.CommitDownload(update, download).success, "version revalidated at main-thread commit");
    registry.UpsertInstalled(installed, registry.PluginRoot() + L"\\org.test.update", PluginSource::Official);
    ok &= Check(!manager.CommitDownload(update, download).success, "source cannot change during download");
    registry.UpsertInstalled(installed, registry.PluginRoot() + L"\\org.test.update", PluginSource::ThirdParty);
    auto wrong = update;
    wrong.version = L"1.2.0";
    ok &= Check(!manager.CommitDownload(wrong, download).success, "prepared package identity matches selected update");
    auto committed = manager.CommitDownload(update, download);
    ok &= Check(committed.success && committed.restartRequired &&
                    registry.Find(installed.id)->manifest.version == L"1.0.0",
                "commit only stages; no live DLL update");
    const auto oldStage = download.package->path;
    download.package.reset();
    ok &= Check(!std::filesystem::exists(oldStage), "prepared directory moved without leaked inspection tree");
    ok &= Check(packages.ApplyPendingOperations(&error) && registry.Load() &&
                    registry.Find(installed.id)->manifest.version == L"1.1.0",
                "restart application path preserves package lifecycle");
    {
        auto unused = PluginPackageManager::PreparePackage(argv[1], registry.AppRoot(), &error);
        if (!unused)
            return 5;
        const auto path = unused->path;
        unused.reset();
        ok &= Check(!std::filesystem::exists(path), "discarded prepared package releases all temporary files");
    }
    return ok ? 0 : 1;
}
