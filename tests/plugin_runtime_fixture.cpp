#define NSKRY_PLUGIN_EXPORTS
#include "nskry_plugin.h"
namespace {
NskryPluginInfo info{sizeof(NskryPluginInfo), NSKRY_PLUGIN_API_VERSION,
    L"Runtime fixture", L"org.test.runtime", L"1.0.0", L"Tests", L"Test only", nullptr,
    NSKRY_CAP_TOOLBAR_ACTION | NSKRY_CAP_POST_CAPTURE, {}};
int calls{};
int shutdowns{};
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
}
