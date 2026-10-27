add_library(nskry_translate SHARED src/nskry_translate.cpp src/offline_process.cpp src/worker_reply.cpp src/online_translation.cpp
    src/translation_http.cpp src/online_config.cpp src/online_settings.cpp)
target_link_libraries(nskry_translate PRIVATE nskry_json)
add_library(nskry_translation_recognition STATIC src/image_recognition.cpp)
target_include_directories(nskry_translation_recognition PUBLIC src)
target_link_libraries(nskry_translation_recognition PRIVATE nskry_ocr_layout)
target_compile_definitions(nskry_translation_recognition PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(nskry_translation_recognition PUBLIC windowsapp user32 gdi32)
if(MSVC)
    target_compile_options(nskry_translation_recognition PRIVATE /utf-8)
endif()
target_link_libraries(nskry_translate PRIVATE nskry_translation_recognition)
# Source/static reuse, NOT an installed-plugin dependency or cross-DLL call.
target_compile_definitions(nskry_translation_recognition PRIVATE NSKRY_TRANSLATE_MODEL_OCR=$<BOOL:${NSKRY_OCR_MODEL_ENGINE}>)
if(NSKRY_OCR_MODEL_ENGINE)
    target_link_libraries(nskry_translation_recognition PRIVATE nskry_ocr_process)
    # Identical owned assets/notices from the shared build declaration, not
    # copied from the OCR plugin's package or dependent on its staging target.
    set(translate_ocr_runtime_stamp "${CMAKE_CURRENT_BINARY_DIR}/ocr-runtime.ready")
    nskry_stage_ocr_runtime(nskry_translate_ocr_runtime "${CMAKE_CURRENT_BINARY_DIR}/package/ocr"
        "${translate_ocr_runtime_stamp}")
    set(translate_ocr_assets_stamp "${CMAKE_CURRENT_BINARY_DIR}/ocr-assets.ready")
    add_custom_command(OUTPUT "${translate_ocr_assets_stamp}"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/package/ocr/nskry-ocr-worker.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nskry_ocr_worker>"
            "${CMAKE_CURRENT_BINARY_DIR}/package/ocr/nskry-ocr-worker.exe"
        COMMAND ${CMAKE_COMMAND} -E touch "${translate_ocr_assets_stamp}"
        DEPENDS nskry_ocr_worker nskry_translate_ocr_runtime "$<TARGET_FILE:nskry_ocr_worker>"
            "${translate_ocr_runtime_stamp}"
        VERBATIM)
    add_custom_target(nskry_translate_ocr_assets DEPENDS "${translate_ocr_assets_stamp}")
    add_dependencies(nskry_translate nskry_translate_ocr_assets)
endif()
target_include_directories(nskry_translate PRIVATE "${CMAKE_SOURCE_DIR}/sdk" "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(nskry_translate PRIVATE NSKRY_PLUGIN_EXPORTS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINRT_LEAN_AND_MEAN)
target_link_libraries(nskry_translate PRIVATE windowsapp ole32 user32 gdi32 shell32 advapi32 crypt32 winhttp comdlg32)
if(MSVC)
    target_compile_options(nskry_translate PRIVATE /utf-8)
endif()
set_target_properties(nskry_translate PROPERTIES OUTPUT_NAME "nskry-translate"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/package/image-model.txt"
    CONTENT "$<BOOL:${NSKRY_OCR_MODEL_ENGINE}>\n")
add_custom_command(TARGET nskry_translate POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${CMAKE_CURRENT_SOURCE_DIR}/ONLINE_ENGINES.md" "${CMAKE_SOURCE_DIR}/LICENSE"
        "${CMAKE_SOURCE_DIR}/COPYING" "${CMAKE_SOURCE_DIR}/PLUGIN_EXCEPTION"
        "$<TARGET_FILE_DIR:nskry_translate>"
    VERBATIM)
# Runtime/model preparation is explicit and never a configure/build download.
# A releasable package additionally needs the runtime's third-party notices.
if(BUILD_TESTING)
    add_executable(NskryTranslationRecognitionTests "${CMAKE_SOURCE_DIR}/tests/translation_recognition_tests.cpp")
    target_link_libraries(NskryTranslationRecognitionTests PRIVATE nskry_translation_recognition)
    target_compile_definitions(NskryTranslationRecognitionTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        NSKRY_TRANSLATE_MODEL_OCR=$<BOOL:${NSKRY_OCR_MODEL_ENGINE}>)
    if(MSVC)
        target_compile_options(NskryTranslationRecognitionTests PRIVATE /utf-8)
    endif()
    add_dependencies(NskryTranslationRecognitionTests nskry_translate)
    add_test(NAME NskryTranslationRecognitionTests COMMAND NskryTranslationRecognitionTests $<TARGET_FILE_DIR:nskry_translate>)
    set_tests_properties(NskryTranslationRecognitionTests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    # Explicit public-image/package audit; never run external services in CTest.
    add_executable(NskryTranslationImageAudit EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/tests/translation_image_audit.cpp")
    target_link_libraries(NskryTranslationImageAudit PRIVATE nskry_json)
    target_include_directories(NskryTranslationImageAudit PRIVATE "${CMAKE_SOURCE_DIR}/sdk" "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(NskryTranslationImageAudit PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(NskryTranslationImageAudit PRIVATE windowsapp user32 gdi32 gdiplus advapi32 bcrypt psapi)
    if(MSVC)
        target_compile_options(NskryTranslationImageAudit PRIVATE /utf-8)
    endif()
    add_executable(NskryTranslationOnlineTests "${CMAKE_SOURCE_DIR}/tests/translation_online_tests.cpp"
        src/online_translation.cpp src/online_config.cpp src/translation_http.cpp)
    target_link_libraries(NskryTranslationOnlineTests PRIVATE nskry_json)
    target_include_directories(NskryTranslationOnlineTests PRIVATE src "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(NskryTranslationOnlineTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(NskryTranslationOnlineTests PRIVATE winhttp crypt32 advapi32 ws2_32 psapi)
    if(MSVC)
        target_compile_options(NskryTranslationOnlineTests PRIVATE /utf-8)
    endif()
    add_test(NAME NskryTranslationOnlineTests COMMAND NskryTranslationOnlineTests)
    set_tests_properties(NskryTranslationOnlineTests PROPERTIES TIMEOUT 15 RUN_SERIAL TRUE)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME NskryTranslationImageEvaluationTests COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/translation/test_image_evaluation.py")
        add_test(NAME NskryTranslationWorkerPolicyTests COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/translation_worker_policy_tests.py")
        set_tests_properties(NskryTranslationWorkerPolicyTests PROPERTIES TIMEOUT 15)
    endif()
    add_executable(NskryTranslationReplyTests "${CMAKE_SOURCE_DIR}/tests/translation_reply_tests.cpp"
        src/worker_reply.cpp)
    target_link_libraries(NskryTranslationReplyTests PRIVATE nskry_json)
    target_include_directories(NskryTranslationReplyTests PRIVATE src "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(NskryTranslationReplyTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(NskryTranslationReplyTests PRIVATE /utf-8)
    endif()
    add_test(NAME NskryTranslationReplyTests COMMAND NskryTranslationReplyTests)
    add_executable(NskryTranslationAsyncWaitTests "${CMAKE_SOURCE_DIR}/tests/translation_async_wait_tests.cpp")
    target_include_directories(NskryTranslationAsyncWaitTests PRIVATE src)
    target_compile_definitions(NskryTranslationAsyncWaitTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(NskryTranslationAsyncWaitTests PRIVATE windowsapp)
    add_test(NAME NskryTranslationAsyncWaitTests COMMAND NskryTranslationAsyncWaitTests)
    add_executable(NskryTranslationPluginTests "${CMAKE_SOURCE_DIR}/tests/translation_plugin_tests.cpp")
    target_include_directories(NskryTranslationPluginTests PRIVATE "${CMAKE_SOURCE_DIR}/sdk")
    target_compile_definitions(NskryTranslationPluginTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(NskryTranslationPluginTests PRIVATE windowsapp user32 gdi32 gdiplus advapi32)
    add_dependencies(NskryTranslationPluginTests nskry_translate)
    if(MSVC)
        target_compile_options(NskryTranslationPluginTests PRIVATE /utf-8)
    endif()
    add_test(NAME NskryTranslationPluginTests COMMAND NskryTranslationPluginTests $<TARGET_FILE:nskry_translate>)
    set_tests_properties(NskryTranslationPluginTests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    # Real model test is opt-in; ordinary build/test must not download models.
    set(NSKRY_TRANSLATE_TEST_MODELS "" CACHE PATH "Explicitly prepared offline translation language packs")
    if(NSKRY_TRANSLATE_TEST_MODELS)
        add_test(NAME NskryTranslationOfflineTests COMMAND NskryTranslationPluginTests
            $<TARGET_FILE:nskry_translate> "${NSKRY_TRANSLATE_TEST_MODELS}")
        set_tests_properties(NskryTranslationOfflineTests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
        add_executable(NskryOcrTranslationIntegrationTests "${CMAKE_SOURCE_DIR}/tests/ocr_translation_integration_tests.cpp")
        target_link_libraries(NskryOcrTranslationIntegrationTests PRIVATE nskry_plugin_runtime)
        target_include_directories(NskryOcrTranslationIntegrationTests PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/sdk")
        target_compile_definitions(NskryOcrTranslationIntegrationTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINRT_LEAN_AND_MEAN)
        target_link_libraries(NskryOcrTranslationIntegrationTests PRIVATE windowsapp user32 gdi32)
        if(MSVC)
            target_compile_options(NskryOcrTranslationIntegrationTests PRIVATE /utf-8)
        endif()
        add_dependencies(NskryOcrTranslationIntegrationTests nskry_translate nskry_ocr_package)
        add_test(NAME NskryOcrTranslationIntegrationTests COMMAND NskryOcrTranslationIntegrationTests
            $<TARGET_FILE:nskry_translate> $<TARGET_FILE:nskry_ocr> "${NSKRY_TRANSLATE_TEST_MODELS}" "${CMAKE_BINARY_DIR}")
        set_tests_properties(NskryOcrTranslationIntegrationTests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    endif()
endif()
