#define NSKRY_PLUGIN_EXPORTS
#include "nskry_plugin.h"
#include "nskry_input.h"
#include <stdexcept>
namespace {
NskryPluginInfo info{sizeof(NskryPluginInfo), NSKRY_PLUGIN_API_VERSION,
    L"Runtime fixture", L"org.test.runtime", L"1.0.0", L"Tests", L"Test only", nullptr,
    NSKRY_CAP_TOOLBAR_ACTION | NSKRY_CAP_POST_CAPTURE, {}};
int calls{};
int shutdowns{};
void ConfigureFixtureInput(const NskryInputConfigV1* config) {
    wchar_t fail[8]{};
    if(GetEnvironmentVariableW(L"NSKRY_TEST_FAIL_INPUT",fail,8))throw std::runtime_error("Fixture input failure");
    nskry::input::Configure(config);
}
}
extern "C" {
NSKRY_API const NskryPluginInfo* NSKRY_CALL nskry_plugin_info() { return &info; }
NSKRY_API int32_t NSKRY_CALL nskry_plugin_init(const NskryHostContext*) {
    wchar_t fail[8]{}; return GetEnvironmentVariableW(L"NSKRY_TEST_FAIL_INIT", fail, 8) ? 0 : 1;
}
NSKRY_API void NSKRY_CALL nskry_plugin_execute(const NskryHostContext*) { ++calls; }
NSKRY_API void NSKRY_CALL nskry_plugin_shutdown() { ++shutdowns; }
__declspec(dllexport) int NSKRY_CALL nskry_test_execute_count() { return calls; }
__declspec(dllexport) int NSKRY_CALL nskry_test_shutdown_count() { return shutdowns; }
NSKRY_API void NSKRY_CALL nskry_plugin_configure_input_v1(const NskryInputConfigV1* config){
    ConfigureFixtureInput(config);
}
__declspec(dllexport) int NSKRY_CALL nskry_test_configured() {
    auto it=nskry::input::pluginBindings.find(L"copy");return it!=nskry::input::pluginBindings.end() && it->second==L"Ctrl+Shift+C" &&
        !nskry::input::pluginBindings.contains(L"launch");
}
}
