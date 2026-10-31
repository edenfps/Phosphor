cmake_minimum_required(VERSION 3.20)
project(Phosphor CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(phosphor WIN32
    src/main.cpp
    src/common.cpp
    src/config.cpp
    src/display.cpp
    src/renderer.cpp
    src/image_io.cpp
    src/detect.cpp
    src/wgc.cpp
    src/stress.cpp
    src/phosphor.rc
)
target_compile_definitions(phosphor PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(phosphor PRIVATE d3d11 dxgi d3dcompiler windowscodecs shell32 user32 gdi32 ole32 dwmapi windowsapp)
# Copy the exe to bin/ so autostart and shortcuts don't depend on the build tree.
add_custom_command(TARGET phosphor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:phosphor> ${CMAKE_SOURCE_DIR}/bin/phosphor.exe)
if(MSVC)
    target_compile_options(phosphor PRIVATE /W3 /utf-8 /permissive-)
    set_property(TARGET phosphor PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
