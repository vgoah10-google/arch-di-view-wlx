# arch-di-view-wlx — Total Commander Lister plugin for archive inspection.
# Pure C++/Win32/GDI (no WebView, no external archiver DLLs). Parses ZIP, TAR
# and GZ/TGZ natively and renders a compression-ratio bar, a detail table and a
# vector tree diagram of the contents.
cmake_minimum_required(VERSION 3.20)
project(arch-di-view-wlx LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ArchDiView targets Windows only (Total Commander Lister).")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(arch-diview SHARED
    src/plugin.cpp
    src/view.cpp
    src/tree.cpp
    src/archive.cpp
    src/archive_zip.cpp
    src/archive_tar.cpp
    src/archive_gz.cpp
    src/archive_simple.cpp
    src/archive_classic.cpp
    src/archive_containers.cpp
    src/archive_old.cpp
    src/archive_modern.cpp
    src/archive_dll.cpp
    src/inflate.cpp
)

target_include_directories(arch-diview PRIVATE src)

target_compile_definitions(arch-diview PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

target_compile_options(arch-diview PRIVATE
    /W4 /utf-8 /EHsc
)

# Optional debug logging to %TEMP%/archdiview_debug.log for load failures.
option(ARCHDIVIEW_DEBUG "Enable debug logging" OFF)
if(ARCHDIVIEW_DEBUG)
    target_compile_definitions(arch-diview PRIVATE ARCHDIVIEW_DEBUG)
endif()

# Link only the classic Win32/GDI libraries — no WebView2, no COM archivers.
target_link_libraries(arch-diview PRIVATE
    user32
    gdi32
    shell32
)

# Total Commander loads the 64-bit build as .wlx64 and the 32-bit build as .wlx.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WLX_SUFFIX ".wlx64")
else()
    set(WLX_SUFFIX ".wlx")
endif()

set_target_properties(arch-diview PROPERTIES
    PREFIX ""
    SUFFIX "${WLX_SUFFIX}"
    OUTPUT_NAME "ArchDiView"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

install(TARGETS arch-diview RUNTIME DESTINATION .)

# ---- Test harness (parser smoke test, console) -------------------------
option(ARCHDIVIEW_BUILD_TESTS "Build the parser test harness" ON)
if(ARCHDIVIEW_BUILD_TESTS)
    add_executable(test_parse test/test_parse.cpp
        src/archive.cpp src/archive_zip.cpp src/archive_tar.cpp
        src/archive_gz.cpp src/archive_simple.cpp src/archive_classic.cpp
        src/archive_containers.cpp src/archive_old.cpp src/archive_modern.cpp
        src/archive_dll.cpp
        src/inflate.cpp src/tree.cpp)
    target_include_directories(test_parse PRIVATE src)
    target_compile_definitions(test_parse PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_parse PRIVATE /W4 /utf-8 /EHsc)
    target_link_libraries(test_parse PRIVATE user32 gdi32)

    add_executable(test_capture test/test_capture.cpp)
    target_compile_definitions(test_capture PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_capture PRIVATE /utf-8 /EHsc)
    target_link_libraries(test_capture PRIVATE user32 gdi32)
endif()
