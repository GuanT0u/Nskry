# One build of the host runtime for the executable and its integration tests.
# Native plugins may reuse the pure JSON library, but never link the host
# runtime/registry targets: host interaction goes through the public SDK.
add_library(nskry_json STATIC "${CMAKE_SOURCE_DIR}/src/core/json.cpp")
add_library(nskry_plugin_metadata STATIC
    "${CMAKE_SOURCE_DIR}/src/core/plugin_registry.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/plugin_manifest.cpp"
    "${CMAKE_SOURCE_DIR}/src/update/semver.cpp")
add_library(nskry_plugin_runtime STATIC
    "${CMAKE_SOURCE_DIR}/src/core/plugin_manager.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/plugin_image_recognition.cpp")
foreach(target nskry_json nskry_plugin_metadata nskry_plugin_runtime)
    target_include_directories(${target} PUBLIC "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/sdk")
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINRT_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
endforeach()
target_link_libraries(nskry_plugin_metadata PUBLIC nskry_json)
target_link_libraries(nskry_plugin_runtime PUBLIC nskry_plugin_metadata user32 gdi32 ole32 windowsapp)

# Configure-time regression gate for the shipped plugin link graphs. Native
# code is not sandboxed; this prevents accidental host linkage in this build.
function(nskry_check_plugin_boundary plugin)
    if(NOT TARGET "${plugin}")
        message(FATAL_ERROR "Missing plugin target in boundary check: ${plugin}")
    endif()
    set(pending "${plugin}")
    set(visited)
    while(pending)
        list(POP_FRONT pending current)
        if(current IN_LIST visited OR NOT TARGET "${current}")
            continue()
        endif()
        if(current STREQUAL "nskry_plugin_runtime" OR current STREQUAL "nskry_plugin_metadata")
            message(FATAL_ERROR "${plugin} must use the SDK, not host runtime/registry linkage: ${current}")
        endif()
        list(APPEND visited "${current}")
        foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(links "${current}" "${property}")
            foreach(link IN LISTS links)
                if(link MATCHES "nskry_plugin_(runtime|metadata)")
                    message(FATAL_ERROR "${plugin} must use the SDK, not host runtime/registry linkage: ${current} -> ${link}")
                endif()
                string(REGEX REPLACE "^\\$<LINK_ONLY:([^>]+)>$" "\\1" link "${link}")
                if(TARGET "${link}")
                    get_target_property(alias "${link}" ALIASED_TARGET)
                    if(alias)
                        set(link "${alias}")
                    endif()
                    list(APPEND pending "${link}")
                endif()
            endforeach()
        endforeach()
    endwhile()
endfunction()
