set(stage "${CMAKE_CURRENT_BINARY_DIR}/package")
set(bundle "${CMAKE_SOURCE_DIR}/installer/bundled/nskry-ocr-provider.nskryplugin")
add_library(nskry_ocr_provider SHARED recognition.cpp ../nskry-ocr/src/ocr_arbitration.cpp)
target_include_directories(nskry_ocr_provider PRIVATE "${CMAKE_SOURCE_DIR}/sdk")
target_compile_definitions(nskry_ocr_provider PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN NSKRY_PLUGIN_EXPORTS
    NSKRY_OCR_MODEL_ENGINE=$<BOOL:${NSKRY_OCR_MODEL_ENGINE}>)
target_compile_options(nskry_ocr_provider PRIVATE /utf-8)
target_link_libraries(nskry_ocr_provider PRIVATE nskry_ocr_process nskry_ocr_layout windowsapp gdi32 user32)
set_target_properties(nskry_ocr_provider PROPERTIES OUTPUT_NAME nskry-ocr-provider RUNTIME_OUTPUT_DIRECTORY "${stage}")
set(files manifest.json nskry-ocr-provider.dll LICENSE COPYING PLUGIN_EXCEPTION)
set(deps)
if(NSKRY_OCR_MODEL_ENGINE)
    include("${CMAKE_SOURCE_DIR}/cmake/ocr_runtime_assets.cmake")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/assets.ready")
    nskry_stage_ocr_runtime(nskry_provider_assets "${stage}/engine" "${stamp}")
    add_custom_command(OUTPUT "${stage}/engine/nskry-ocr-worker.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nskry_ocr_worker>" "${stage}/engine/nskry-ocr-worker.exe"
        DEPENDS nskry_ocr_worker "$<TARGET_FILE:nskry_ocr_worker>" "${stamp}" VERBATIM)
    list(APPEND files engine)
    list(APPEND deps nskry_provider_assets "${stage}/engine/nskry-ocr-worker.exe" "${stamp}")
endif()
add_custom_command(OUTPUT "${bundle}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
        "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/COPYING" "${CMAKE_SOURCE_DIR}/PLUGIN_EXCEPTION" "${stage}"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${bundle}" --format=zip ${files}
    DEPENDS nskry_ocr_provider "$<TARGET_FILE:nskry_ocr_provider>" "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" ${deps}
        "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/COPYING" "${CMAKE_SOURCE_DIR}/PLUGIN_EXCEPTION"
    WORKING_DIRECTORY "${stage}" VERBATIM)
add_custom_target(nskry_ocr_provider_package ALL DEPENDS "${bundle}")
