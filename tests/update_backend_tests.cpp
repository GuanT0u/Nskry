#include "core/plugin_update_manager.h"
#include "update/package_verifier.h"
#include <filesystem>
#include <iostream>
#include <string_view>
#include "test_artifacts.h"
using namespace nskry;
bool Check(bool ok, const char *message) {
    if (!ok)
        std::cerr << message << '\n';
    return ok;
}
int CheckOcrPackageLifecycle(const wchar_t *feature, const wchar_t *build, const wchar_t *provider, const wchar_t *translation) {
    nskry::test::Artifacts profile(build, L"ocr-package");
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.Path().c_str());
    PluginRegistry registry;
    PluginPackageManager packages(registry);
    constexpr wchar_t providerId[] = L"nskry-ocr-provider", featureId[] = L"nskry-ocr";
    bool ok = Check(registry.Initialize() && packages.Initialize() && registry.Load() &&
                    registry.GetAll().empty(), "Core-only profile starts with zero plugins");
    ok &= Check(packages.InstallPackage(feature, PluginSource::Official).needsOcrProvider &&
                registry.GetAll().empty(), "missing dependency is reported before install");
    ok &= Check(packages.InstallWithOcrProvider(feature, PluginSource::Official).success &&
                registry.Find(featureId) && registry.ResolveOcr() && registry.GetAll().size() == 2,
                "feature and its companion install together");
    ok &= Check(!GetModuleHandleW(L"nskry-ocr-provider.dll") &&
                !packages.Uninstall(providerId).success && !registry.SetEnabled(providerId, false),
                "installed provider remains unloaded and cannot be removed while required");
    const auto *before = registry.ResolveOcr();
    ok &= Check(packages.InstallPackage(translation, PluginSource::ThirdParty).success &&
                registry.ResolveOcr() == before && registry.GetAll().size() == 3,
                "both features share one installed provider");
    ok &= Check(packages.Uninstall(L"nskry-translate").success && packages.ApplyPendingOperations() &&
                registry.Load() && registry.Find(featureId) && registry.ResolveOcr(),
                "removing translation keeps extraction and the provider");
    before = registry.ResolveOcr();
    const auto package = packages.InstallWithOcrProvider(feature, PluginSource::Official);
    ok &= Check(package.success && package.restartRequired && registry.ResolveOcr() == before &&
                registry.GetAll().size() == 2, "reinstall reuses the existing provider");
    ok &= Check(registry.SetEnabled(featureId, false) && packages.ApplyPendingOperations() &&
                registry.Load() && registry.Find(featureId) && !registry.Find(featureId)->enabled,
                "restart applies update and preserves disabled preference");
    ok &= Check(!packages.Uninstall(providerId).success && packages.Uninstall(featureId).success &&
                packages.ApplyPendingOperations() && registry.Load() && !registry.Find(featureId) &&
                registry.Find(providerId), "uninstall feature keeps independent provider");
    ok &= Check(packages.Uninstall(providerId).success && packages.ApplyPendingOperations() &&
                registry.Load() && registry.GetAll().empty(), "explicit provider removal when unused");
    const auto companion = profile.Path() / L"nskry-ocr-provider.nskryplugin";
    const auto translationCopy = profile.Path() / L"translate.nskryplugin";
    std::filesystem::copy_file(translation, translationCopy);
    ok &= Check(!packages.InstallWithOcrProvider(translationCopy.wstring(), PluginSource::ThirdParty).success &&
                registry.GetAll().empty(), "missing companion leaves the profile unchanged");
    std::filesystem::copy_file(provider, companion);
    ok &= Check(packages.InstallWithOcrProvider(translationCopy.wstring(), PluginSource::ThirdParty).success &&
                !registry.Find(featureId) && registry.Find(L"nskry-translate") && registry.GetAll().size() == 2,
                "translation alone installs without extraction");
    ok &= Check(packages.Uninstall(L"nskry-translate").success && packages.ApplyPendingOperations() &&
                registry.Load() &&
                packages.Uninstall(providerId).success &&
                !packages.InstallPackage(feature, PluginSource::Official).success,
                "cannot install a feature against a provider pending removal");
    return profile.Finish(ok) && ok ? 0 : 1;
}
int wmain(int argc, wchar_t **argv) {
    if (argc == 7 && std::wstring_view(argv[1]) == L"--ocr-upgrade") {
        nskry::test::Artifacts profile(argv[2], L"ocr-upgrade");
        SetEnvironmentVariableW(L"LOCALAPPDATA", profile.Path().c_str());
        PluginRegistry registry;
        PluginPackageManager packages(registry);
        const std::wstring id = argv[6];
        const bool extraction = id == L"nskry-ocr";
        const auto *oldVersion = extraction ? L"0.6.6" : L"0.3.4";
        const auto *newVersion = extraction ? L"0.7.0" : L"0.4.0";
        bool ok = Check(registry.Initialize() && packages.Initialize() && registry.Load(), "upgrade init");
        ok &= Check(packages.InstallPackage(argv[3], PluginSource::Local).success &&
                    registry.Find(id) && registry.Find(id)->manifest.version == oldVersion,
                    "install baseline feature package");
        ok &= Check(registry.SetEnabled(id, false) &&
                    packages.InstallPackage(argv[4], PluginSource::Local).needsOcrProvider &&
                    packages.PendingRestartActions().empty() &&
                    registry.Find(id)->manifest.version == oldVersion,
                    "dependency failure leaves old package in place");
        ok &= Check(packages.InstallPackage(argv[5], PluginSource::Local).success &&
                    packages.InstallPackage(argv[4], PluginSource::Local).restartRequired &&
                    packages.ApplyPendingOperations() && registry.Load() && registry.Find(id) &&
                    registry.Find(id)->manifest.version == newVersion && !registry.Find(id)->enabled &&
                    !registry.Find(extraction ? L"nskry-translate" : L"nskry-ocr") &&
                    !std::filesystem::exists(std::filesystem::path(registry.Find(id)->installPath) / (extraction ? L"engine" : L"ocr")),
                    "feature upgrade preserves disabled state and removes duplicate runtime");
        return profile.Finish(ok) && ok ? 0 : 1;
    }
    if (argc == 5) return CheckOcrPackageLifecycle(argv[1], argv[2], argv[3], argv[4]);
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
