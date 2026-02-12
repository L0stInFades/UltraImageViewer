cmake_minimum_required(VERSION 3.25)
project(Afterglow VERSION 2.0.0 LANGUAGES CXX)

# C++20 with latest features
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform detection
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "UltraImageViewer only supports Windows")
endif()

# Architecture - x64 only
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only x64 builds are supported")
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Release optimization flags
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Maximum optimization for MSVC
    add_compile_options(/O2 /Ob2 /Oi /Ot /GL)
    add_link_options(/LTCG)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    add_compile_options(/fp:fast /Gw)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(/O2 /Ob2 /Oi /Ot /Zi)
    add_link_options(/DEBUG:FULL)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(/Od /RTC1 /Zi)
    add_link_options(/DEBUG:FULL)
endif()

# Compiler flags
add_compile_options(
    /W4
    /MP
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /wd4100
    /wd4201
)

# Windows macros
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# Source files
set(SOURCES
    src/main.cpp
    src/core/Application.cpp
    src/core/ImageDecoder.cpp
    src/core/MemoryManager.cpp
    src/core/CacheManager.cpp
    src/core/ThreadPool.cpp
    src/core/ImagePipeline.cpp
    src/core/SimdUtils.cpp
    src/rendering/Direct2DRenderer.cpp
    src/ui/CommandPalette.cpp
    src/ui/GestureHandler.cpp
    src/ui/ThumbnailStrip.cpp
    src/ui/ViewManager.cpp
    src/ui/GalleryView.cpp
    src/ui/ImageViewer.cpp
    src/ui/TransitionController.cpp
    src/animation/SpringAnimation.cpp
    src/animation/AnimationEngine.cpp
)

# Create executable
add_executable(afterglow WIN32 ${SOURCES} app.rc)

target_include_directories(afterglow PRIVATE include)

# Windows libraries
target_link_libraries(afterglow PRIVATE
    d2d1
    dcomp
    dxguid
    d3d11
    dwrite
    dxgi
    windowscodecs
    shcore
    runtimeobject
    Comctl32
    Gdi32
    User32
    Shell32
    Ole32
    oleaut32
    dwmapi
)

# Windows 11 manifests
set(MANIFEST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.manifest")
if(EXISTS ${MANIFEST_FILE})
    target_sources(afterglow PRIVATE ${MANIFEST_FILE})
    set_target_properties(afterglow PROPERTIES
        VS_DPI_AWARE "PerMonitor"
    )
endif()

# Installation
install(TARGETS afterglow
    RUNTIME DESTINATION bin
)

# Summary
message(STATUS "")
message(STATUS "==========================================")
message(STATUS "Afterglow Configuration")
message(STATUS "==========================================")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "==========================================")
