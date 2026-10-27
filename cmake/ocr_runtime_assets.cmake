# Internal build helper. Runtime assets are declared once by the native engine;
# each plugin stages its own files, without reading another plugin's package.
function(nskry_stage_ocr_runtime target destination stamp)
    get_target_property(sources nskry_ocr_inference NSKRY_RUNTIME_SOURCES)
    get_target_property(paths nskry_ocr_inference NSKRY_RUNTIME_PATHS)
    list(LENGTH sources source_count)
    list(LENGTH paths path_count)
    if(NOT source_count OR NOT source_count EQUAL path_count)
        message(FATAL_ERROR "Invalid native OCR runtime asset declaration")
    endif()
    set(commands)
    set(directories)
    set(outputs)
    math(EXPR last "${source_count} - 1")
    foreach(index RANGE ${last})
        list(GET sources ${index} source)
        list(GET paths ${index} path)
        get_filename_component(parent "${destination}/${path}" DIRECTORY)
        list(APPEND directories "${parent}")
        list(APPEND commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${source}" "${destination}/${path}")
        list(APPEND outputs "${destination}/${path}")
    endforeach()
    list(REMOVE_DUPLICATES directories)
    add_custom_command(OUTPUT "${stamp}"
        BYPRODUCTS ${outputs}
        COMMAND "${CMAKE_COMMAND}" -E make_directory ${directories}
        ${commands}
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${sources} "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        VERBATIM)
    add_custom_target(${target} DEPENDS "${stamp}")
endfunction()
