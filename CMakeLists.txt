cmake_minimum_required(VERSION 3.13)
project(SDLop VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SDLop_BUILD_EXAMPLES "Build SDLop examples" ON)
option(SDLop_BUILD_TESTS "Build SDLop tests" ON)
option(SDLop_BUILD_BENCHMARKS "Build SDLop benchmarks (incl. SDL3 comparison)" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_definitions(_GNU_SOURCE)
add_compile_options(-Wall -Wextra)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)

find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(WAYLAND_CLIENT_DIR wayland-client pkgdatadir)

set(WL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland-gen)
file(MAKE_DIRECTORY ${WL_GEN_DIR})

add_custom_command(
  OUTPUT ${WL_GEN_DIR}/wayland-client-protocol.h
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header
          ${WAYLAND_CLIENT_DIR}/wayland.xml
          ${WL_GEN_DIR}/wayland-client-protocol.h
  DEPENDS ${WAYLAND_CLIENT_DIR}/wayland.xml)

add_custom_command(
  OUTPUT ${WL_GEN_DIR}/xdg-shell-client-protocol.h ${WL_GEN_DIR}/xdg-shell-protocol.c
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header
          ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
          ${WL_GEN_DIR}/xdg-shell-client-protocol.h
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
          ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
          ${WL_GEN_DIR}/xdg-shell-protocol.c
  DEPENDS ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)

add_custom_target(sdlop_wayland_protocols
  DEPENDS ${WL_GEN_DIR}/wayland-client-protocol.h
          ${WL_GEN_DIR}/xdg-shell-client-protocol.h
          ${WL_GEN_DIR}/xdg-shell-protocol.c)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(SDLop_SOURCES
  src/SDLop_core.c
  src/SDL_error.c
  src/SDL_log.c
  src/SDL_timer.c
  src/SDL_events.c
  src/SDL_keyboard.c
  src/SDL_mouse.c
  src/SDL_video.c
  src/internal/sdlop_evdev.c
  src/video/wayland/sdlop_wayland.c
  src/video/dummy/sdlop_dummy.c
  ${WL_GEN_DIR}/xdg-shell-protocol.c
)

add_library(sdlop STATIC ${SDLop_SOURCES})
add_library(SDLop::SDLop ALIAS sdlop)
add_dependencies(sdlop sdlop_wayland_protocols)

target_include_directories(sdlop PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(sdlop PRIVATE
  src
  ${WL_GEN_DIR}
  ${WAYLAND_CLIENT_INCLUDE_DIRS}
  ${XKBCOMMON_INCLUDE_DIRS})
target_link_directories(sdlop PUBLIC ${WAYLAND_CLIENT_LIBRARY_DIRS} ${XKBCOMMON_LIBRARY_DIRS})
target_link_libraries(sdlop PUBLIC
  ${WAYLAND_CLIENT_LIBRARIES}
  ${XKBCOMMON_LIBRARIES}
  pthread)
target_compile_definitions(sdlop PUBLIC SDLop_VIDEO_WAYLAND=1)

add_library(sdlop_shared SHARED ${SDLop_SOURCES})
add_dependencies(sdlop_shared sdlop_wayland_protocols)
set_target_properties(sdlop_shared PROPERTIES OUTPUT_NAME SDLop)
target_include_directories(sdlop_shared PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(sdlop_shared PRIVATE
  src
  ${WL_GEN_DIR}
  ${WAYLAND_CLIENT_INCLUDE_DIRS}
  ${XKBCOMMON_INCLUDE_DIRS})
target_link_directories(sdlop_shared PUBLIC ${WAYLAND_CLIENT_LIBRARY_DIRS} ${XKBCOMMON_LIBRARY_DIRS})
target_link_libraries(sdlop_shared PUBLIC
  ${WAYLAND_CLIENT_LIBRARIES}
  ${XKBCOMMON_LIBRARIES}
  pthread)
target_compile_definitions(sdlop_shared PUBLIC SDLop_VIDEO_WAYLAND=1)

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(SDLop_BUILD_EXAMPLES)
  foreach(ex window wasd raw_latency)
    add_executable(${ex} examples/${ex}.c)
    target_link_libraries(${ex} PRIVATE sdlop)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SDLop_BUILD_TESTS)
  enable_testing()
  foreach(t test_ring test_scancode test_events)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE sdlop)
    target_include_directories(${t} PRIVATE src)
    target_compile_options(${t} PRIVATE -UNDEBUG)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()

  # integration tests that need real subsystems (auto-skip when unavailable)
  add_executable(test_evdev_uinput tests/test_evdev_uinput.c tests/uinput_synth.c)
  target_link_libraries(test_evdev_uinput PRIVATE sdlop)
  target_include_directories(test_evdev_uinput PRIVATE src tests)
  target_compile_options(test_evdev_uinput PRIVATE -UNDEBUG)
  add_test(NAME test_evdev_uinput COMMAND test_evdev_uinput)

  add_executable(test_wayland tests/test_wayland.c)
  target_link_libraries(test_wayland PRIVATE sdlop)
  target_include_directories(test_wayland PRIVATE src)
  target_compile_options(test_wayland PRIVATE -UNDEBUG)
  add_test(NAME test_wayland COMMAND test_wayland)
endif()

# ---------------------------------------------------------------------------
# Benchmarks (SDLop vs SDL3)
# ---------------------------------------------------------------------------
if(SDLop_BUILD_BENCHMARKS)
  pkg_check_modules(SDL3 sdl3)
  add_executable(bench_events bench/bench_events.c)
  target_link_libraries(bench_events PRIVATE sdlop)
  if(SDL3_FOUND)
    add_executable(bench_events_sdl3 bench/bench_events.c)
    target_compile_definitions(bench_events_sdl3 PRIVATE SDLOP_BENCH_SDL3=1)
    target_include_directories(bench_events_sdl3 PRIVATE ${SDL3_INCLUDE_DIRS})
    target_link_directories(bench_events_sdl3 PRIVATE ${SDL3_LIBRARY_DIRS})
    target_link_libraries(bench_events_sdl3 PRIVATE ${SDL3_LIBRARIES})
  else()
    message(STATUS "SDL3 not found - skipping comparison benchmark")
  endif()
  add_executable(bench_raw_latency bench/bench_raw_latency.c tests/uinput_synth.c)
  target_link_libraries(bench_raw_latency PRIVATE sdlop)
  target_include_directories(bench_raw_latency PRIVATE src tests)
endif()
