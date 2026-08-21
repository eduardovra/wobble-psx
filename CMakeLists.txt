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
# Reads the zip a game is usually distributed in. Small, permissive and
# self-contained, and it brings the archive index with it rather than
# just the decompression — so nothing here has to parse a zip by hand.
set(MINIZ_BUILD_TESTS OFF)
set(MINIZ_BUILD_EXAMPLES OFF)
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.1.2
)
# Reads the .chd a game is often kept as instead of a bin/cue. The
# format is a compressed image with a hunk index in front of it, so a
# sector in the middle costs one hunk of decompression and not a walk
# from the start — which is what makes it worth reading in place when
# a zip is not. Its own miniz is skipped: the target above already
# exists, and libchdr links whichever one it finds.
FetchContent_Declare(
    libchdr
    GIT_REPOSITORY https://github.com/rtissera/libchdr.git
    GIT_TAG v0.3.0
)
# miniz before libchdr, since that is the order the targets are needed.
FetchContent_MakeAvailable(SDL3 imgui doctest miniz libchdr)

# libchdr builds a benchmark of its own that nothing here runs.
set_target_properties(chdr-benchmark PROPERTIES EXCLUDE_FROM_ALL TRUE)

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
    src/archive.cpp
    src/bus.cpp
    src/cdrom.cpp
    src/chd.cpp
    src/console.cpp
    src/cpu.cpp
    src/debugger.cpp
    src/disasm.cpp
    src/disc.cpp
    src/dma.cpp
    src/exe.cpp
    src/gdbstub.cpp
    src/gpu.cpp
    src/gte.cpp
    src/irq.cpp
    src/log.cpp
    src/mdec.cpp
    src/memctrl.cpp
    src/raster.cpp
    src/savestate.cpp
    src/scheduler.cpp
    src/sio.cpp
    src/spu.cpp
    src/timers.cpp
)
target_include_directories(core PUBLIC src)
# miniz ships a zlib-compatible name layer this project never calls,
# and it warns about being unused in every file that includes it.
target_link_libraries(core PUBLIC miniz chdr-static)
target_compile_definitions(core PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

add_executable(wobble src/main.cpp)
target_link_libraries(wobble PRIVATE core imgui SDL3::SDL3)

# The same console with a debugger on the front and no display, so it
# runs anywhere a terminal does.
add_executable(wobble-dbg src/wobble-dbg.cpp)
target_link_libraries(wobble-dbg PRIVATE core)

add_executable(tests
    tests/main.cpp
    tests/test_archive.cpp
    tests/test_bus.cpp
    tests/test_cdrom.cpp
    tests/test_chd.cpp
    tests/test_cpu.cpp
    tests/test_debugger.cpp
    tests/test_disasm.cpp
    tests/test_disc.cpp
    tests/test_dma.cpp
    tests/test_exe.cpp
    tests/test_gdbstub.cpp
    tests/test_gpu.cpp
    tests/test_gte.cpp
    tests/test_irq.cpp
    tests/test_mdec.cpp
    tests/test_raster.cpp
    tests/test_scheduler.cpp
    tests/test_sio.cpp
    tests/test_spu.cpp
    tests/test_timers.cpp
    tests/test_timing.cpp
)
target_link_libraries(tests PRIVATE core doctest::doctest)

foreach(target core wobble wobble-dbg tests)
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
