cmake_minimum_required(VERSION 3.24)
project(wobble-psx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# clang-tidy reads compile_commands.json to know how each file is
# built, so always emit it.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

set(SDL_TEST_LIBRARY OFF)
set(SDL_EXAMPLES OFF)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.18
)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
)
FetchContent_MakeAvailable(SDL3 imgui doctest)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# The emulated machine, with no dependency on SDL or any other host
# library, so the tests can link it without a window system.
add_library(core STATIC
    src/bus.cpp
    src/cpu.cpp
    src/dma.cpp
    src/gpu.cpp
    src/irq.cpp
    src/log.cpp
    src/scheduler.cpp
)
target_include_directories(core PUBLIC src)

add_executable(wobble src/main.cpp)
target_link_libraries(wobble PRIVATE core imgui SDL3::SDL3)

add_executable(tests
    tests/main.cpp
    tests/test_bus.cpp
    tests/test_cpu.cpp
    tests/test_dma.cpp
    tests/test_gpu.cpp
    tests/test_irq.cpp
    tests/test_scheduler.cpp
)
target_link_libraries(tests PRIVATE core doctest::doctest)

foreach(target core wobble tests)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined)
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined)
    endif()
endforeach()

enable_testing()
add_test(NAME tests COMMAND tests)
