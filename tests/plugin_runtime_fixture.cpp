#define NSKRY_PLUGIN_EXPORTS
#include "nskry_plugin.h"
#include "nskry_input.h"
#include "nskry_commands.h"
#include "nskry_text_translation.h"
#include "nskry_image_recognition.h"
#include <string>
#include <stdexcept>
namespace {
NskryPluginInfo info{sizeof(NskryPluginInfo), NSKRY_PLUGIN_API_VERSION,
    L"Runtime fixture",
#ifdef NSKRY_TEST_TEXT_CLIENT
    L"org.test.text-client",
#else
    L"org.test.runtime",
#endif
    L"1.0.0", L"Tests", L"Test only", nullptr,
    NSKRY_CAP_TOOLBAR_ACTION | NSKRY_CAP_POST_CAPTURE, {}};
int calls{};
int shutdowns{};
NskryTextTranslationServiceV1 translationService{};
NskryImageRecognitionServiceV1 imageService{};
DWORD initThread{};
std::wstring receivedText;
void ThrowTranslationFixture() { throw std::runtime_error("Test translation failure"); }
void ThrowImageFixture() { throw std::runtime_error("Image fixture failure"); }
ID3D11Device *initDevice{}, *executeDevice{};
ID3D11DeviceContext *initContext{}, *executeContext{};
void ConfigureFixtureInput(const NskryInputConfigV1* config) {
    wchar_t fail[8]{};
    if(GetEnvironmentVariableW(L"NSKRY_TEST_FAIL_INPUT",fail,8))throw std::runtime_error("Fixture input failure");
    nskry::input::Configure(config);
}
}
extern "C" {
NSKRY_API const NskryPluginInfo* NSKRY_CALL nskry_plugin_info() { return &info; }
NSKRY_API int32_t NSKRY_CALL nskry_plugin_init(const NskryHostContext* ctx) {
    initThread = GetCurrentThreadId();
    initDevice = ctx->d3dDevice;
    initContext = ctx->d3dContext;
    wchar_t fail[8]{}; return GetEnvironmentVariableW(L"NSKRY_TEST_FAIL_INIT", fail, 8) ? 0 : 1;
}
NSKRY_API void NSKRY_CALL nskry_plugin_execute(const NskryHostContext* ctx) {
    executeDevice = ctx->d3dDevice;
    executeContext = ctx->d3dContext;
    ++calls;
}
__declspec(dllexport) int NSKRY_CALL nskry_test_graphics_match() {
    return initDevice && initContext && initDevice == executeDevice && initContext == executeContext;
}
NSKRY_API void NSKRY_CALL nskry_plugin_shutdown() { ++shutdowns; }
#ifndef NSKRY_TEST_NO_TEXT
NSKRY_API int32_t NSKRY_CALL nskry_plugin_connect_image_recognition_v1(const NskryImageRecognitionServiceV1* service) {
    if (!service || service->version != 1 || service->structSize < sizeof(*service)) return 0;
    imageService = *service; return 1;
}
__declspec(dllexport) const NskryImageRecognitionServiceV1* NSKRY_CALL nskry_test_image_service() { return &imageService; }
NSKRY_API int32_t NSKRY_CALL nskry_plugin_recognize_image_v1(const NskryImageInputV1* input, NskryImageTextV1* out) {
    if (GetCurrentThreadId() == initThread || !input || !NskryValidImageText(out)) return NSKRY_IMAGE_INVALID;
    wchar_t mode[24]{};
    GetEnvironmentVariableW(L"NSKRY_TEST_IMAGE_RESULT", mode, 24);
    if (!wcscmp(mode, L"wait")) {
        WaitForSingleObject(input->cancel, 5000);
        return NSKRY_IMAGE_CANCELED;
    }
    if (!wcscmp(mode, L"throw")) ThrowImageFixture();
    if (!wcscmp(mode, L"malformed")) return 99;
    if (!wcscmp(mode, L"badlength")) { out->textLength = NSKRY_IMAGE_MAX_TEXT + 1; return NSKRY_IMAGE_SUCCESS; }
    if (!wcscmp(mode, L"fail")) { NskryImageMessage(out, L"Fixture failed"); return NSKRY_IMAGE_FAILED; }
    BITMAP bitmap{};
    if (!GetObjectW(input->bitmap, sizeof(bitmap), &bitmap) || bitmap.bmWidth != input->width) return NSKRY_IMAGE_INVALID;
    lstrcpynW(out->text, L"provider image text", out->capacity);
    out->textLength = 19;
    return NSKRY_IMAGE_SUCCESS;
}
NSKRY_API int32_t NSKRY_CALL nskry_plugin_connect_text_translation_v1(const NskryTextTranslationServiceV1 *service) {
    if (!service || service->version != 1 || service->structSize < sizeof(*service)) return 0;
    translationService = *service;
    return 1;
}
NSKRY_API int32_t NSKRY_CALL nskry_plugin_translate_text_v1(const NskryTextTranslationV1 *request, NskryTranslationResultV1 *result) {
    if (!NskryValidTranslation(request)) return NSKRY_TRANSLATION_INVALID;
    wchar_t mode[24]{};
    GetEnvironmentVariableW(L"NSKRY_TEST_TRANSLATION_RESULT", mode, 24);
    if (!wcscmp(mode, L"throw")) ThrowTranslationFixture();
    if (!wcscmp(mode, L"busy")) {
        NskryTranslationError(result, L"Fixture busy");
        return NSKRY_TRANSLATION_BUSY;
    }
    if (!wcscmp(mode, L"malformed")) return 99;
    receivedText.assign(request->text, request->textLength);
    return NSKRY_TRANSLATION_ACCEPTED;
}
__declspec(dllexport) const NskryTextTranslationServiceV1 *NSKRY_CALL nskry_test_translation_service() { return &translationService; }
__declspec(dllexport) const wchar_t *NSKRY_CALL nskry_test_translation_received() { return receivedText.c_str(); }
#endif
__declspec(dllexport) int NSKRY_CALL nskry_test_execute_count() { return calls; }
__declspec(dllexport) int NSKRY_CALL nskry_test_shutdown_count() { return shutdowns; }
NSKRY_API int32_t NSKRY_CALL nskry_plugin_command_v1(const NskryCommandV1* request,NskryCommandResultV1*) {
    return request && request->version==1 && request->id && !wcscmp(request->id,L"stop") ? NSKRY_COMMAND_OK : NSKRY_COMMAND_UNSUPPORTED;
}
#ifndef NSKRY_TEST_NO_INPUT
NSKRY_API void NSKRY_CALL nskry_plugin_configure_input_v1(const NskryInputConfigV1* config){
    ConfigureFixtureInput(config);
}
#endif
#ifdef NSKRY_TEST_INPUT_V2
NSKRY_API int32_t NSKRY_CALL nskry_plugin_configure_input_v2(const NskryInputConfigV1* config,NskryInputResultV2* result){
    wchar_t mode[24]{};GetEnvironmentVariableW(L"NSKRY_TEST_INPUT_RESULT",mode,24);
    if(!wcscmp(mode,L"unsupported"))return NSKRY_INPUT_UNSUPPORTED;
    if(!wcscmp(mode,L"failed")){wcsncpy_s(result->error,result->errorCapacity,L"Fixture rejection",_TRUNCATE);return NSKRY_INPUT_FAILED;}
    if(!wcscmp(mode,L"malformed"))return 99;
    ConfigureFixtureInput(config);
    return nskry::input::ConfigureConfirmed(config,result,{{L"copy",nskry::input::Kind::Key}});
}
#endif
__declspec(dllexport) int NSKRY_CALL nskry_test_configured() {
    auto it=nskry::input::pluginBindings.find(L"copy");return it!=nskry::input::pluginBindings.end() && it->second==L"Ctrl+Shift+C" &&
        !nskry::input::pluginBindings.contains(L"launch");
}
}
