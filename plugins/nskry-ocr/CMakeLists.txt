set(NSKRY_OCR_STAGE "${CMAKE_CURRENT_BINARY_DIR}/package")
set(NSKRY_OCR_BUNDLE "${CMAKE_SOURCE_DIR}/installer/bundled/nskry-ocr.nskryplugin")

add_library(nskry_ocr SHARED src/nskry_ocr.cpp)
target_compile_features(nskry_ocr PRIVATE cxx_std_20)
if (MSVC)
    target_compile_options(nskry_ocr PRIVATE /utf-8)
endif()
target_compile_definitions(nskry_ocr PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINRT_LEAN_AND_MEAN NSKRY_PLUGIN_EXPORTS)
target_include_directories(nskry_ocr PRIVATE ${CMAKE_SOURCE_DIR}/sdk)
target_link_libraries(nskry_ocr PRIVATE gdi32 user32 ole32 windowsapp msimg32 comdlg32 windowscodecs)
set_target_properties(nskry_ocr PROPERTIES OUTPUT_NAME "nskry-ocr" RUNTIME_OUTPUT_DIRECTORY "${NSKRY_OCR_STAGE}")

add_custom_command(OUTPUT "${NSKRY_OCR_BUNDLE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NSKRY_OCR_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "${NSKRY_OCR_STAGE}/manifest.json"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${NSKRY_OCR_BUNDLE}" --format=zip manifest.json nskry-ocr.dll
    DEPENDS nskry_ocr "$<TARGET_FILE:nskry_ocr>" "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    WORKING_DIRECTORY "${NSKRY_OCR_STAGE}"
    COMMENT "Packaging nskry-ocr.nskryplugin"
    VERBATIM)

add_custom_target(nskry_ocr_package ALL DEPENDS "${NSKRY_OCR_BUNDLE}")
