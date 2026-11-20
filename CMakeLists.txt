cmake_minimum_required(VERSION 3.16)
project(DropletDemo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(DropletDemo
    src/main.cpp
    src/renderer.cpp
    src/renderer.h
    src/geometry.h
    src/math_utils.h
)

target_compile_definitions(DropletDemo PRIVATE UNICODE _UNICODE)
target_link_libraries(DropletDemo PRIVATE user32 gdi32 d3d11 dxgi d3dcompiler)

add_custom_command(TARGET DropletDemo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:DropletDemo>/shaders"
)
