add_library(nskry_translate SHARED src/nskry_translate.cpp "${CMAKE_SOURCE_DIR}/src/core/json.cpp")
target_include_directories(nskry_translate PRIVATE "${CMAKE_SOURCE_DIR}/sdk" "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(nskry_translate PRIVATE NSKRY_PLUGIN_EXPORTS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINRT_LEAN_AND_MEAN)
target_link_libraries(nskry_translate PRIVATE windowsapp ole32 user32 gdi32 shell32 advapi32 crypt32)
if(MSVC)
    target_compile_options(nskry_translate PRIVATE /utf-8)
endif()
set_target_properties(nskry_translate PROPERTIES OUTPUT_NAME "nskry-translate"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package")
add_custom_command(TARGET nskry_translate POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${CMAKE_SOURCE_DIR}/LICENSE"
        "${CMAKE_SOURCE_DIR}/COPYING" "${CMAKE_SOURCE_DIR}/PLUGIN_EXCEPTION"
        "$<TARGET_FILE_DIR:nskry_translate>"
    VERBATIM)
# Runtime/model preparation is explicit and never a configure/build download.
# A releasable package additionally needs the runtime's third-party notices.
if(BUILD_TESTING)
    add_executable(NskryTranslationPluginTests "${CMAKE_SOURCE_DIR}/tests/translation_plugin_tests.cpp")
    target_include_directories(NskryTranslationPluginTests PRIVATE "${CMAKE_SOURCE_DIR}/sdk")
    target_compile_definitions(NskryTranslationPluginTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(NskryTranslationPluginTests PRIVATE windowsapp user32 gdi32)
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
        add_executable(NskryOcrTranslationIntegrationTests "${CMAKE_SOURCE_DIR}/tests/ocr_translation_integration_tests.cpp"
            "${CMAKE_SOURCE_DIR}/src/core/plugin_manager.cpp" "${CMAKE_SOURCE_DIR}/src/core/plugin_registry.cpp"
            "${CMAKE_SOURCE_DIR}/src/core/plugin_manifest.cpp" "${CMAKE_SOURCE_DIR}/src/core/json.cpp"
            "${CMAKE_SOURCE_DIR}/src/update/semver.cpp")
        target_include_directories(NskryOcrTranslationIntegrationTests PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/sdk")
        target_compile_definitions(NskryOcrTranslationIntegrationTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINRT_LEAN_AND_MEAN)
        target_link_libraries(NskryOcrTranslationIntegrationTests PRIVATE windowsapp user32 gdi32)
        if(MSVC)
            target_compile_options(NskryOcrTranslationIntegrationTests PRIVATE /utf-8)
        endif()
        add_dependencies(NskryOcrTranslationIntegrationTests nskry_translate nskry_ocr_package)
        add_test(NAME NskryOcrTranslationIntegrationTests COMMAND NskryOcrTranslationIntegrationTests
            $<TARGET_FILE:nskry_translate> $<TARGET_FILE:nskry_ocr> "${NSKRY_TRANSLATE_TEST_MODELS}" "${CMAKE_BINARY_DIR}")
        set_tests_properties(NskryOcrTranslationIntegrationTests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    endif()
endif()
