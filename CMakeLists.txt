cmake_minimum_required(VERSION 3.24)
project(Nskry LANGUAGES CXX)
# Keep host/test output locations stable when OpenCV sets legacy bin defaults.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows 10 2004 (build 19041) minimum for programmatic WGC capture
set(CMAKE_SYSTEM_VERSION 10.0.19041.0)

add_executable(Nskry WIN32
    src/main.cpp
    src/core/command_registry.cpp
    src/core/hotkey_manager.cpp
    src/core/json.cpp
    src/core/plugin_manifest.cpp
    src/core/plugin_manager.cpp
    src/core/plugin_package_manager.cpp
    src/core/plugin_update_manager.cpp
    src/core/plugin_registry.cpp
    src/core/settings_manager.cpp
    src/interop/window_enumerator.cpp
    src/capture/d3d_device.cpp
    src/capture/capture_session.cpp
    src/ui/annotation/annotation_engine.cpp
    src/ui/pip_window.cpp
    src/ui/pin_window.cpp
    src/ui/long_image_crop_window.cpp
    src/ui/precision_loupe.cpp
    src/ui/selection_window.cpp
    src/ui/settings/settings_window.cpp
    src/update/package_verifier.cpp
    src/update/downloader.cpp
    src/update/semver.cpp
)

target_include_directories(Nskry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} src sdk)

target_compile_definitions(Nskry PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINRT_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

if (MSVC)
    target_compile_options(Nskry PRIVATE /utf-8)
endif()

target_link_libraries(Nskry PRIVATE
    d3d11
    dxgi
    dwmapi
    windowsapp
    shcore
    msimg32
    gdiplus
    shlwapi
    d3dcompiler
    comctl32
    advapi32
    winhttp
    bcrypt
)

# Precompiled header
target_precompile_headers(Nskry PRIVATE src/pch.h)

# Official plugins use the same packaged form that Settings and Setup consume.
add_subdirectory(plugins/nskry-scroll)
add_subdirectory(plugins/nskry-ocr)

if (BUILD_TESTING)
    add_executable(NskryOcrWireTests tests/ocr_wire_ctc_tests.cpp)
    target_link_libraries(NskryOcrWireTests PRIVATE nskry_ocr_wire)
    if (MSVC)
        target_compile_options(NskryOcrWireTests PRIVATE /utf-8)
    endif()
    add_test(NAME NskryOcrWireTests COMMAND NskryOcrWireTests)
    add_executable(NskryOcrFakeWorker tests/ocr_fake_worker.cpp)
    target_compile_definitions(NskryOcrFakeWorker PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_executable(NskryOcrProcessFailureTests tests/ocr_process_failure_tests.cpp plugins/nskry-ocr/src/model/ocr_process.cpp)
    target_link_libraries(NskryOcrProcessFailureTests PRIVATE nskry_ocr_wire gdi32 user32)
    add_test(NAME NskryOcrProcessFailureTests COMMAND NskryOcrProcessFailureTests $<TARGET_FILE:NskryOcrFakeWorker>)
    set_tests_properties(NskryOcrProcessFailureTests PROPERTIES TIMEOUT 15)
    if (NSKRY_OCR_MODEL_ENGINE)
        add_executable(NskryOcrModelTests tests/ocr_model_tests.cpp src/core/json.cpp plugins/nskry-ocr/src/model/ocr_process.cpp)
        target_include_directories(NskryOcrModelTests PRIVATE src)
        target_link_libraries(NskryOcrModelTests PRIVATE nskry_ocr_wire gdiplus gdi32 user32)
        if (MSVC)
            target_compile_options(NskryOcrModelTests PRIVATE /utf-8)
        endif()
        add_dependencies(NskryOcrModelTests nskry_ocr_worker nskry_ocr_assets)
        add_test(NAME NskryOcrModelExplorer COMMAND NskryOcrModelTests $<TARGET_FILE:nskry_ocr_worker>
            "${CMAKE_SOURCE_DIR}/tests/ocr/fixtures/explorer_dark_zh.png" --regions
            "${CMAKE_SOURCE_DIR}/tests/ocr/fixtures/explorer_dark_zh.regions.json" 1 "${CMAKE_BINARY_DIR}/ocr_model_explorer_test.json")
        add_test(NAME NskryOcrModelParagraph COMMAND NskryOcrModelTests $<TARGET_FILE:nskry_ocr_worker>
            "${CMAKE_SOURCE_DIR}/tests/ocr/fixtures/paragraph_dark_zh.png" --content
            "${CMAKE_SOURCE_DIR}/tests/ocr/fixtures/paragraph_dark_zh.expected.txt" 1 "${CMAKE_BINARY_DIR}/ocr_model_paragraph_test.json")
        set_tests_properties(NskryOcrModelExplorer NskryOcrModelParagraph PROPERTIES TIMEOUT 30)
    endif()
    # Opt-in diagnostic matrix; never runs as part of normal build/CTest.
    add_executable(NskryOcrNativeProbe EXCLUDE_FROM_ALL tests/ocr_native_probe.cpp)
    target_compile_definitions(NskryOcrNativeProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_link_libraries(NskryOcrNativeProbe PRIVATE gdiplus windowsapp)
    if (MSVC)
        target_compile_options(NskryOcrNativeProbe PRIVATE /utf-8)
    endif()

    add_executable(NskryCoreTests
        tests/json_manifest_tests.cpp
        src/core/json.cpp
        src/core/plugin_manifest.cpp
        src/update/semver.cpp
    )
    target_include_directories(NskryCoreTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} src sdk)
    target_compile_definitions(NskryCoreTests PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        WINRT_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    if (MSVC)
        target_compile_options(NskryCoreTests PRIVATE /utf-8)
    endif()
    add_test(NAME NskryCoreTests COMMAND NskryCoreTests)

    add_executable(NskryOcrSmokeTests tests/ocr_plugin_smoke.cpp)
    target_include_directories(NskryOcrSmokeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
    target_compile_definitions(NskryOcrSmokeTests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    if (MSVC)
        target_compile_options(NskryOcrSmokeTests PRIVATE /utf-8)
    endif()
    target_link_libraries(NskryOcrSmokeTests PRIVATE gdi32 user32 gdiplus)
    add_dependencies(NskryOcrSmokeTests nskry_ocr)
    add_test(NAME NskryOcrSmokeTests
        COMMAND NskryOcrSmokeTests $<TARGET_FILE:nskry_ocr>)
    set_tests_properties(NskryOcrSmokeTests PROPERTIES TIMEOUT 25)
    add_test(NAME NskryOcrFastSmoke
        COMMAND NskryOcrSmokeTests $<TARGET_FILE:nskry_ocr>)
    set_tests_properties(NskryOcrFastSmoke PROPERTIES TIMEOUT 25 ENVIRONMENT "NSKRY_OCR_ENGINE=fast")
    if (NSKRY_OCR_MODEL_ENGINE)
        add_test(NAME NskryOcrWorkflow COMMAND NskryOcrSmokeTests $<TARGET_FILE:nskry_ocr> --workflow)
        set_tests_properties(NskryOcrWorkflow PROPERTIES TIMEOUT 90)
    endif()
    add_test(NAME NskryOcrExplorerFixture
        COMMAND NskryOcrSmokeTests $<TARGET_FILE:nskry_ocr> --fixture
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ocr/fixtures/explorer_dark_zh.png" 1)
    set_tests_properties(NskryOcrExplorerFixture PROPERTIES TIMEOUT 25)

    add_executable(NskryOcrArbitrationTests
        tests/ocr_arbitration_tests.cpp
        plugins/nskry-ocr/src/ocr_arbitration.cpp)
    target_include_directories(NskryOcrArbitrationTests PRIVATE plugins/nskry-ocr/src)
    target_compile_definitions(NskryOcrArbitrationTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    if (MSVC)
        target_compile_options(NskryOcrArbitrationTests PRIVATE /utf-8)
    endif()
    add_test(NAME NskryOcrArbitrationTests COMMAND NskryOcrArbitrationTests)
endif()
