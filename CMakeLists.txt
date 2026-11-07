cmake_minimum_required(VERSION 3.20)
project(fgtlayer C)

# The game's data structures hold 32-bit pointers: build 32-bit (i686) on Windows, like the Dreamcast.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(WARNING "64-bit build detected. Configure with a 32-bit compiler, e.g. -DCMAKE_C_COMPILER=i686-w64-mingw32-clang")
endif()

set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(RECOMP_DIR ${CMAKE_SOURCE_DIR}/build/recomp)
if(NOT EXISTS ${RECOMP_DIR}/sources.cmake)
  message(FATAL_ERROR "Run: python tools/recomp/recomp.py (generates build/recomp)")
endif()
include(${RECOMP_DIR}/sources.cmake)

add_library(recomp STATIC ${RECOMP_SOURCES})
target_include_directories(recomp PRIVATE port/core ${RECOMP_DIR})
# Generated code: huge functions full of gotos. -O1 keeps compile times sane.
target_compile_options(recomp PRIVATE -O1 -fno-strict-aliasing -w)
set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1")

add_executable(fgtlayer
  port/core/cpu.c
  port/core/hw.c
  port/core/bios.c
  port/core/gpu.c
  port/core/gpu_raster.c
  port/core/gpu_hd.c
  port/core/gte.c
  port/core/h8.c
  port/core/c352.c port/core/exsnd.c
  port/core/s12sound.c
  port/core/log.c
  port/pc/main.c
  port/pc/coro_win32.c
  port/pc/video_win32.c
  port/pc/audio_win32.c
  port/pc/script.c
  port/pc/pad_map.c
  port/pc/pad_xinput.c
  port/pc/video_d3d11.c
  port/pc/gpu_hd_d3d11.c
  port/pc/ui_d3d11.c
  port/pc/menu_host.c
  port/ui/menu.c
  port/ui/settings.c
  src/game/release.c
  src/game/file_override.c
  src/game/ex_mirror.c
  src/game/ex_ponytail.c
  src/game/roster.c src/game/ex_pullum.c
  src/game/arcade_settings.c)
target_include_directories(fgtlayer PRIVATE port/core src/game ${RECOMP_DIR})
target_compile_options(fgtlayer PRIVATE -Wall -fno-strict-aliasing)
target_include_directories(fgtlayer PRIVATE port/ui)
target_link_libraries(fgtlayer PRIVATE recomp gdi32 user32 winmm d3d11 dxgi ole32 windowscodecs)

# Controller mapping test (no hardware needed): ctest --test-dir build/pc
enable_testing()
add_executable(pad_map_test tests/pad_map_test.c port/pc/pad_map.c)
add_test(NAME pad_map COMMAND pad_map_test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
