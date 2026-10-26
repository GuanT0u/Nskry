#include "core/plugin_manager.h"
#include "test_artifacts.h"
#include <array>
#include <thread>

int wmain(int argc, wchar_t **argv) {
    if (argc != 5)
        return 2;
    nskry::test::Artifacts artifacts(argv[4], L"image-recognition");
    SetEnvironmentVariableW(L"LOCALAPPDATA", artifacts.Path().c_str());
    nskry::PluginRegistry registry;
    if (!registry.Initialize())
        return 3;
    const auto providerDll = std::filesystem::absolute(argv[1]),
               clientDll = std::filesystem::absolute(argv[2]);
    nskry::PluginManifest provider;
    provider.manifestVersion = 1;
    provider.apiVersion = 2;
    provider.id = L"org.test.runtime";
    provider.name = L"Image provider";
    provider.version = L"1.0.0";
    provider.entry = providerDll.filename().wstring();
    provider.architecture = L"x64";
    provider.imageRecognitionApi = 1;
    auto client = provider;
    client.id = L"org.test.text-client";
    client.entry = clientDll.filename().wstring();
    client.imageRecognitionApi = 0;
    client.imageRecognitionClientApi = 1;
    if (!registry.UpsertInstalled(client, clientDll.parent_path().wstring(), nskry::PluginSource::ThirdParty))
        return 4;
    auto &runtime = nskry::PluginManager::Instance();
    NskryHostContext context{sizeof(context), 2};
    runtime.Initialize(registry, context);
    struct Stop {
        nskry::PluginManager &runtime;
        ~Stop() {
            runtime.Shutdown();
        }
    } stop{runtime};
    if (!runtime.EnsureLoaded(client.id))
        return 5;
    const auto getter = reinterpret_cast<const NskryImageRecognitionServiceV1 *(NSKRY_CALL *)()>(
        GetProcAddress(GetModuleHandleW(clientDll.filename().c_str()), "nskry_test_image_service"));
    if (!getter)
        return 6;
    const auto service = *getter(); // Save host pointers, not DLL-owned storage.
    bool ok = !service.available(provider.id.c_str());
    ok = registry.UpsertInstalled(provider, providerDll.parent_path().wstring(),
                                  nskry::PluginSource::ThirdParty) &&
         ok;
    ok = service.available(provider.id.c_str()) && !runtime.IsLoaded(provider.id) &&
         !GetModuleHandleW(providerDll.filename().c_str()) && ok;
    auto bitmap = CreateBitmap(40, 30, 1, 32, nullptr);
    struct Image {
        HBITMAP value;
        ~Image() {
            DeleteObject(value);
        }
    } image{bitmap};
    uint64_t ticket{};
    int32_t status{};
    std::thread wrong([&] { status = service.begin(provider.id.c_str(), bitmap, &ticket); });
    wrong.join();
    ok = status == NSKRY_IMAGE_UNAVAILABLE && !runtime.IsLoaded(provider.id) && ok;
    ok = service.begin(provider.id.c_str(), nullptr, &ticket) == NSKRY_IMAGE_INVALID && !ticket && ok;
    std::array<wchar_t, NSKRY_IMAGE_MAX_TEXT + 1> text{};
    wchar_t error[256]{};
    NskryImageTextV1 out{sizeof(out), 1, text.data(), uint32_t(text.size()), 0, error, 256};
    const auto complete = [&] {
        const auto deadline = GetTickCount64() + 3000;
        int32_t state{};
        do {
            state = service.poll(ticket, &out);
            if (state != NSKRY_IMAGE_PENDING)
                return state;
            Sleep(2);
        } while (GetTickCount64() < deadline);
        return int32_t(99);
    };
    ok = service.begin(provider.id.c_str(), bitmap, &ticket) == NSKRY_IMAGE_PENDING && ticket && ok;
    ok = complete() == NSKRY_IMAGE_SUCCESS && std::wstring(text.data()) == L"provider image text" && ok;
    out.version = 2;
    ok = service.poll(ticket, &out) == NSKRY_IMAGE_INVALID && ok;
    out.version = 1;
    service.release(ticket);
    ok = service.poll(ticket, &out) == NSKRY_IMAGE_UNAVAILABLE && ok;
    for (const auto *mode : {L"throw", L"malformed", L"badlength", L"fail"}) {
        SetEnvironmentVariableW(L"NSKRY_TEST_IMAGE_RESULT", mode);
        ok = service.begin(provider.id.c_str(), bitmap, &ticket) == NSKRY_IMAGE_PENDING && ok;
        ok = complete() == NSKRY_IMAGE_FAILED && ok;
        service.release(ticket);
    }
    SetEnvironmentVariableW(L"NSKRY_TEST_IMAGE_RESULT", L"wait");
    ok = service.begin(provider.id.c_str(), bitmap, &ticket) == NSKRY_IMAGE_PENDING && ok;
    uint64_t second{};
    ok = service.begin(provider.id.c_str(), bitmap, &second) == NSKRY_IMAGE_BUSY && !second && ok;
    // Disable stops new jobs but does not destroy active provider code/resources.
    registry.SetEnabled(provider.id, false);
    ok = !service.available(provider.id.c_str()) && service.poll(ticket, &out) == NSKRY_IMAGE_PENDING && ok;
    service.cancel(ticket);
    ok = complete() == NSKRY_IMAGE_CANCELED && ok;
    const auto stale = ticket;
    service.release(ticket);
    registry.SetEnabled(provider.id, true);
    ok = service.begin(provider.id.c_str(), bitmap, &ticket) == NSKRY_IMAGE_PENDING && ticket != stale && ok;
    service.cancel(stale);
    service.release(stale);
    ok = service.poll(ticket, &out) == NSKRY_IMAGE_PENDING && ok;
    // Provider unload and full shutdown join before FreeLibrary, even if busy.
    runtime.Unload(provider.id);
    ok = service.poll(ticket, &out) == NSKRY_IMAGE_UNAVAILABLE &&
         !GetModuleHandleW(providerDll.filename().c_str()) && ok;
    const auto legacy = std::filesystem::absolute(argv[3]);
    provider.entry = legacy.filename().wstring();
    registry.UpsertInstalled(provider, legacy.parent_path().wstring(), nskry::PluginSource::ThirdParty);
    ok = service.begin(provider.id.c_str(), bitmap, &ticket) == NSKRY_IMAGE_UNAVAILABLE && !ticket && ok;
    provider.entry = providerDll.filename().wstring();
    registry.UpsertInstalled(provider, providerDll.parent_path().wstring(), nskry::PluginSource::ThirdParty);
    ok = service.begin(provider.id.c_str(), bitmap, &ticket) == NSKRY_IMAGE_PENDING && ok;
    runtime.Shutdown();
    ok = !service.available(provider.id.c_str()) && service.poll(ticket, &out) == NSKRY_IMAGE_UNAVAILABLE &&
         ok;
    SetEnvironmentVariableW(L"NSKRY_TEST_IMAGE_RESULT", nullptr);
    std::cout << "Image service metadata/lazy load, thread/version/disable gates, bounded jobs, "
                 "cancellation/unload/exit="
              << ok << '\n';
    return artifacts.Finish(ok) && ok ? 0 : 1;
}
