set(NSKRY_OCR_STAGE "${CMAKE_CURRENT_BINARY_DIR}/package")
set(NSKRY_OCR_BUNDLE "${CMAKE_SOURCE_DIR}/installer/bundled/nskry-ocr.nskryplugin")
option(NSKRY_OCR_MODEL_ENGINE "Build the on-demand native CPU OCR worker" ON)
if (NSKRY_OCR_MODEL_ENGINE)
    include(cmake/model_dependencies.cmake)
endif()

add_library(nskry_ocr_wire STATIC src/model/ocr_wire.cpp src/model/ctc_decode.cpp)
target_compile_features(nskry_ocr_wire PUBLIC cxx_std_20)
target_include_directories(nskry_ocr_wire PUBLIC src/model)
target_compile_definitions(nskry_ocr_wire PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if (MSVC)
    target_compile_options(nskry_ocr_wire PRIVATE /utf-8)
endif()

add_library(nskry_ocr SHARED src/nskry_ocr.cpp src/ocr_arbitration.cpp src/model/ocr_process.cpp)
target_compile_features(nskry_ocr PRIVATE cxx_std_20)
if (MSVC)
    target_compile_options(nskry_ocr PRIVATE /utf-8)
endif()
target_compile_definitions(nskry_ocr PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINRT_LEAN_AND_MEAN NSKRY_PLUGIN_EXPORTS
    NSKRY_OCR_MODEL_ENGINE=$<BOOL:${NSKRY_OCR_MODEL_ENGINE}>)
target_include_directories(nskry_ocr PRIVATE ${CMAKE_SOURCE_DIR}/sdk)
target_link_libraries(nskry_ocr PRIVATE nskry_ocr_wire gdi32 user32 ole32 windowsapp msimg32 comdlg32 windowscodecs)
set_target_properties(nskry_ocr PROPERTIES OUTPUT_NAME "nskry-ocr" RUNTIME_OUTPUT_DIRECTORY "${NSKRY_OCR_STAGE}")

set(NSKRY_OCR_PACKAGE_FILES manifest.json nskry-ocr.dll)
set(NSKRY_OCR_PACKAGE_DEPS nskry_ocr "$<TARGET_FILE:nskry_ocr>" "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json")
if (NSKRY_OCR_MODEL_ENGINE)
    add_library(nskry_ocr_inference STATIC src/model/ocr_inference.cpp "${NSKRY_CLIPPER_DIR}/src/clipper.cpp")
    target_compile_features(nskry_ocr_inference PUBLIC cxx_std_20)
    target_include_directories(nskry_ocr_inference PRIVATE "${NSKRY_CLIPPER_DIR}/src")
    target_include_directories(nskry_ocr_inference SYSTEM PRIVATE
        "${NSKRY_OCR_DEPS_DIR}/opencv-4.10.0/modules/core/include"
        "${NSKRY_OCR_DEPS_DIR}/opencv-4.10.0/modules/imgproc/include"
        "${CMAKE_BINARY_DIR}")
    target_link_libraries(nskry_ocr_inference PUBLIC nskry_ocr_wire nskry_ort opencv_imgproc)
    if (MSVC)
        target_compile_options(nskry_ocr_inference PRIVATE /utf-8)
        set_source_files_properties("${NSKRY_CLIPPER_DIR}/src/clipper.cpp" PROPERTIES COMPILE_OPTIONS /wd4828)
    endif()
    add_executable(nskry_ocr_worker src/model/ocr_worker.cpp)
    target_link_libraries(nskry_ocr_worker PRIVATE nskry_ocr_inference psapi)
    if (MSVC)
        target_compile_options(nskry_ocr_worker PRIVATE /utf-8)
    endif()
    set_target_properties(nskry_ocr_worker PROPERTIES OUTPUT_NAME "nskry-ocr-worker" RUNTIME_OUTPUT_DIRECTORY "${NSKRY_OCR_STAGE}/engine")
    foreach(model ch_PP-OCRv4_det_infer ch_PP-OCRv4_rec_infer)
        file(SHA256 "${NSKRY_OCR_MODELS_DIR}/${model}.onnx" model_hash)
        if (model MATCHES "_det_")
            set(expected "d2a7720d45a54257208b1e13e36a8479894cb74155a5efe29462512d42f49da9")
        else()
            set(expected "48fc40f24f6d2a207a2b1091d3437eb3cc3eb6b676dc3ef9c37384005483683b")
        endif()
        if (NOT model_hash STREQUAL expected)
            message(FATAL_ERROR "Unexpected OCR model hash: ${model}")
        endif()
    endforeach()
    set(engine_assets
        "${NSKRY_ORT_DIR}/lib/onnxruntime.dll" "${NSKRY_ORT_DIR}/lib/onnxruntime_providers_shared.dll"
        "${NSKRY_OCR_MODELS_DIR}/ch_PP-OCRv4_det_infer.onnx" "${NSKRY_OCR_MODELS_DIR}/ch_PP-OCRv4_rec_infer.onnx"
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${NSKRY_ORT_DIR}/LICENSE" "${NSKRY_ORT_DIR}/ThirdPartyNotices.txt"
        "${NSKRY_OCR_DEPS_DIR}/Boost-LICENSE.txt" "${NSKRY_OCR_DEPS_DIR}/RapidOCR-LICENSE.txt"
        "${NSKRY_OCR_DEPS_DIR}/opencv-4.10.0/LICENSE" "${NSKRY_OCR_DEPS_DIR}/opencv-4.10.0/3rdparty/zlib/LICENSE"
        "${NSKRY_OCR_DEPS_DIR}/opencv-4.10.0/modules/core/src/softfloat.cpp" "${NSKRY_CLIPPER_DIR}/src/clipper.hpp" "${NSKRY_CLIPPER_DIR}/LICENSE")
    set(NSKRY_OCR_ASSET_STAMP "${CMAKE_CURRENT_BINARY_DIR}/engine-assets.ready")
    add_custom_command(OUTPUT "${NSKRY_OCR_ASSET_STAMP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NSKRY_OCR_STAGE}/engine/models" "${NSKRY_OCR_STAGE}/engine/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NSKRY_ORT_DIR}/lib/onnxruntime.dll" "${NSKRY_ORT_DIR}/lib/onnxruntime_providers_shared.dll" "${NSKRY_OCR_STAGE}/engine"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NSKRY_OCR_MODELS_DIR}/ch_PP-OCRv4_det_infer.onnx" "${NSKRY_OCR_MODELS_DIR}/ch_PP-OCRv4_rec_infer.onnx" "${NSKRY_OCR_STAGE}/engine/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${NSKRY_OCR_STAGE}/engine/THIRD_PARTY_NOTICES.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NSKRY_ORT_DIR}/LICENSE" "${NSKRY_OCR_STAGE}/engine/licenses/onnxruntime-LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NSKRY_ORT_DIR}/ThirdPartyNotices.txt" "${NSKRY_OCR_STAGE}/engine/licenses/onnxruntime-ThirdPartyNotices.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NSKRY_OCR_DEPS_DIR}/opencv-4.10.0/LICENSE" "${NSKRY_OCR_STAGE}/engine/licenses/OpenCV-LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NSKRY_OCR_DEPS_DIR}/opencv-4.10.0/3rdparty/zlib/LICENSE" "${NSKRY_OCR_STAGE}/engine/licenses/zlib-LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NSKRY_OCR_DEPS_DIR}/opencv-4.10.0/modules/core/src/softfloat.cpp" "${NSKRY_OCR_STAGE}/engine/licenses/OpenCV-SoftFloat-source.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NSKRY_CLIPPER_DIR}/src/clipper.hpp" "${NSKRY_OCR_STAGE}/engine/licenses/clipper.hpp"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NSKRY_CLIPPER_DIR}/LICENSE" "${NSKRY_OCR_STAGE}/engine/licenses/pyclipper-LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NSKRY_OCR_DEPS_DIR}/Boost-LICENSE.txt" "${NSKRY_OCR_DEPS_DIR}/RapidOCR-LICENSE.txt" "${NSKRY_OCR_STAGE}/engine/licenses"
        COMMAND ${CMAKE_COMMAND} -E touch "${NSKRY_OCR_ASSET_STAMP}"
        DEPENDS ${engine_assets} VERBATIM)
    add_custom_target(nskry_ocr_assets DEPENDS "${NSKRY_OCR_ASSET_STAMP}")
    add_dependencies(nskry_ocr nskry_ocr_worker nskry_ocr_assets)
    list(APPEND NSKRY_OCR_PACKAGE_FILES engine)
    list(APPEND NSKRY_OCR_PACKAGE_DEPS nskry_ocr_worker "$<TARGET_FILE:nskry_ocr_worker>" "${NSKRY_OCR_ASSET_STAMP}")
endif()
add_custom_command(OUTPUT "${NSKRY_OCR_BUNDLE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NSKRY_OCR_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" "${NSKRY_OCR_STAGE}/manifest.json"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${NSKRY_OCR_BUNDLE}" --format=zip ${NSKRY_OCR_PACKAGE_FILES}
    DEPENDS ${NSKRY_OCR_PACKAGE_DEPS}
    WORKING_DIRECTORY "${NSKRY_OCR_STAGE}"
    COMMENT "Packaging nskry-ocr.nskryplugin"
    VERBATIM)

add_custom_target(nskry_ocr_package ALL DEPENDS "${NSKRY_OCR_BUNDLE}")
