cmake_minimum_required(VERSION 3.20)

project(PanoramaEngine VERSION 0.1.0 LANGUAGES C CXX)

set(PANORAMA_BUILD_EXAMPLES_DEFAULT OFF)
set(PANORAMA_BUILD_TESTS_DEFAULT OFF)
set(PANORAMA_MSVC_STATIC_RUNTIME_DEFAULT OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(PANORAMA_BUILD_EXAMPLES_DEFAULT ON)
    set(PANORAMA_BUILD_TESTS_DEFAULT ON)
    set(PANORAMA_MSVC_STATIC_RUNTIME_DEFAULT ON)
endif()

option(PANORAMA_BUILD_EXAMPLES "Build the standalone PanoramaEngine examples" ${PANORAMA_BUILD_EXAMPLES_DEFAULT})
option(PANORAMA_BUILD_WINDOW_EXAMPLE "Build the native Win32/X11 raster example" ON)
option(PANORAMA_BUILD_TESTS "Register the headless standalone lifecycle smoke test" ${PANORAMA_BUILD_TESTS_DEFAULT})
option(PANORAMA_MSVC_STATIC_RUNTIME "Use the static MSVC runtime, matching the sighmake build" ${PANORAMA_MSVC_STATIC_RUNTIME_DEFAULT})

if(PANORAMA_BUILD_TESTS)
    enable_testing()
endif()

if(MSVC AND PANORAMA_MSVC_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Keep both vendored dependencies deterministic and self-contained. Cache
# entries remain overridable by a parent project before add_subdirectory().
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "Install vendored QuickJS")
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "Build vendored QuickJS examples")
set(QJS_BUILD_CLI_STATIC OFF CACHE BOOL "Build vendored QuickJS CLI")
set(QJS_BUILD_LIBC ON CACHE BOOL "Include QuickJS libc support in qjs")

set(FT_DISABLE_ZLIB ON CACHE BOOL "Disable external zlib in vendored FreeType")
set(FT_DISABLE_BZIP2 ON CACHE BOOL "Disable external bzip2 in vendored FreeType")
set(FT_DISABLE_PNG ON CACHE BOOL "Disable external libpng in vendored FreeType")
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "Disable external HarfBuzz in vendored FreeType")
set(FT_DISABLE_BROTLI ON CACHE BOOL "Disable external Brotli in vendored FreeType")
set(FT_DISABLE_HVF ON CACHE BOOL "Disable external HVF in vendored FreeType")
set(SKIP_INSTALL_ALL ON CACHE BOOL "Do not install vendored FreeType")

set(PANORAMA_OWNS_QJS OFF)
if(NOT TARGET qjs)
    add_subdirectory(Thirdparty/quickjs-0.15.0)
    set(PANORAMA_OWNS_QJS ON)
endif()
if(NOT TARGET freetype)
    add_subdirectory(Thirdparty/freetype)
endif()

if(MSVC AND PANORAMA_OWNS_QJS)
    # Match the vendored sighmake target: these are known third-party warnings,
    # not useful diagnostics for a PanoramaEngine consumer build.
    target_compile_options(qjs PRIVATE
        /wd4098 /wd4132 /wd4146 /wd4334 /wd4668 /wd4701 /wd4702 /wd4703
    )
endif()

file(GLOB PANORAMA_ENGINE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB_RECURSE PANORAMA_ENGINE_HEADERS CONFIGURE_DEPENDS include/ui/panorama/*.hpp)

add_library(PanoramaEngine STATIC
    ${PANORAMA_ENGINE_SOURCES}
    ${PANORAMA_ENGINE_HEADERS}
)
add_library(PanoramaEngine::PanoramaEngine ALIAS PanoramaEngine)

target_compile_features(PanoramaEngine PUBLIC cxx_std_20)
target_include_directories(PanoramaEngine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(PanoramaEngine PUBLIC qjs freetype)

if(WIN32)
    target_compile_definitions(PanoramaEngine PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(MSVC)
    target_compile_options(PanoramaEngine PRIVATE /W3)
else()
    target_compile_options(PanoramaEngine PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PANORAMA_BUILD_TESTS)
    add_executable(PanoramaStandaloneTests tests/standalone_tests.cpp)
    target_compile_features(PanoramaStandaloneTests PRIVATE cxx_std_20)
    target_link_libraries(PanoramaStandaloneTests PRIVATE PanoramaEngine::PanoramaEngine)
    target_compile_definitions(PanoramaStandaloneTests PRIVATE
        PANORAMA_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(MSVC)
        target_compile_options(PanoramaStandaloneTests PRIVATE /W4)
    else()
        target_compile_options(PanoramaStandaloneTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME PanoramaStandaloneApi COMMAND PanoramaStandaloneTests)
endif()

if(PANORAMA_BUILD_EXAMPLES)
    function(panorama_add_console_example target source)
        add_executable(${target} ${source})
        target_compile_features(${target} PRIVATE cxx_std_20)
        target_link_libraries(${target} PRIVATE PanoramaEngine::PanoramaEngine)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endfunction()

    panorama_add_console_example(PanoramaExampleHelloLayout examples/01_hello_layout/main.cpp)
    panorama_add_console_example(PanoramaExampleSoftwareRaster examples/02_software_raster/main.cpp)
    panorama_add_console_example(PanoramaExampleScriptedUi examples/03_scripted_ui/main.cpp)
    if(PANORAMA_BUILD_TESTS)
        add_test(NAME PanoramaStandaloneViewLifecycle COMMAND PanoramaExampleScriptedUi)
    endif()

    if(PANORAMA_BUILD_WINDOW_EXAMPLE)
        if(WIN32)
            panorama_add_console_example(PanoramaExampleWindowRaster examples/04_window_raster/win32_main.cpp)
            target_compile_definitions(PanoramaExampleWindowRaster PRIVATE UNICODE _UNICODE)
            target_link_libraries(PanoramaExampleWindowRaster PRIVATE user32 gdi32)
        elseif(UNIX)
            find_package(X11 QUIET)
            if(X11_FOUND)
                panorama_add_console_example(PanoramaExampleWindowRaster examples/04_window_raster/posix_main.cpp)
                target_link_libraries(PanoramaExampleWindowRaster PRIVATE X11::X11)
            else()
                message(STATUS "X11 not found; skipping PanoramaExampleWindowRaster")
            endif()
        endif()
    endif()
endif()
