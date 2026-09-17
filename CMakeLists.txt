cmake_minimum_required(VERSION 3.24)
project(Nskry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows 10 2004 (build 19041) minimum for programmatic WGC capture
set(CMAKE_SYSTEM_VERSION 10.0.19041.0)

add_executable(Nskry WIN32
    src/main.cpp
    src/core/command_registry.cpp
    src/core/hotkey_manager.cpp
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
