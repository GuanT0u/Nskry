set(NSKRY_SCROLL_STAGE "${CMAKE_CURRENT_BINARY_DIR}/package")
set(NSKRY_SCROLL_BUNDLE "${CMAKE_SOURCE_DIR}/installer/bundled/nskry-scroll.nskryplugin")

add_library(nskry_scroll SHARED src/nskry_scroll.cpp)
target_compile_features(nskry_scroll PRIVATE cxx_std_20)
if (MSVC)
    target_compile_options(nskry_scroll PRIVATE /utf-8)
endif()
target_compile_definitions(nskry_scroll PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX NSKRY_PLUGIN_EXPORTS)
target_include_directories(nskry_scroll PRIVATE ${CMAKE_SOURCE_DIR}/sdk)
target_link_libraries(nskry_scroll PRIVATE gdi32 user32)
set_target_properties(nskry_scroll PROPERTIES OUTPUT_NAME "nskry-scroll" RUNTIME_OUTPUT_DIRECTORY "${NSKRY_SCROLL_STAGE}")

add_custom_command(OUTPUT "${NSKRY_SCROLL_BUNDLE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NSKRY_SCROLL_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "${NSKRY_SCROLL_STAGE}/manifest.json"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${NSKRY_SCROLL_BUNDLE}" --format=zip manifest.json nskry-scroll.dll
    DEPENDS nskry_scroll "$<TARGET_FILE:nskry_scroll>" "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    WORKING_DIRECTORY "${NSKRY_SCROLL_STAGE}"
    COMMENT "Packaging nskry-scroll.nskryplugin"
    VERBATIM)

add_custom_target(nskry_scroll_package ALL DEPENDS "${NSKRY_SCROLL_BUNDLE}")
