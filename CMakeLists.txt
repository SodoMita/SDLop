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
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLESV2 REQUIRED glesv2)

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

add_custom_command(
  OUTPUT ${WL_GEN_DIR}/pointer-constraints-client-protocol.h ${WL_GEN_DIR}/pointer-constraints-protocol.c
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header
          ${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
          ${WL_GEN_DIR}/pointer-constraints-client-protocol.h
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
          ${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
          ${WL_GEN_DIR}/pointer-constraints-protocol.c
  DEPENDS ${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml)

add_custom_command(
  OUTPUT ${WL_GEN_DIR}/relative-pointer-client-protocol.h ${WL_GEN_DIR}/relative-pointer-protocol.c
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header
          ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml
          ${WL_GEN_DIR}/relative-pointer-client-protocol.h
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
          ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml
          ${WL_GEN_DIR}/relative-pointer-protocol.c
  DEPENDS ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml)

# server-side codegen (used only by tests/test_pointer_lock's mini compositor)
add_custom_command(
  OUTPUT ${WL_GEN_DIR}/xdg-shell-server-protocol.h ${WL_GEN_DIR}/xdg-shell-server-protocol.c
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} server-header
          ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
          ${WL_GEN_DIR}/xdg-shell-server-protocol.h
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
          ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
          ${WL_GEN_DIR}/xdg-shell-server-protocol.c
  DEPENDS ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)

add_custom_command(
  OUTPUT ${WL_GEN_DIR}/pointer-constraints-server-protocol.h ${WL_GEN_DIR}/pointer-constraints-server-protocol.c
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} server-header
          ${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
          ${WL_GEN_DIR}/pointer-constraints-server-protocol.h
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
          ${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
          ${WL_GEN_DIR}/pointer-constraints-server-protocol.c
  DEPENDS ${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml)

add_custom_command(
  OUTPUT ${WL_GEN_DIR}/relative-pointer-server-protocol.h ${WL_GEN_DIR}/relative-pointer-server-protocol.c
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} server-header
          ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml
          ${WL_GEN_DIR}/relative-pointer-server-protocol.h
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
          ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml
          ${WL_GEN_DIR}/relative-pointer-server-protocol.c
  DEPENDS ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml)

add_custom_target(sdlop_wayland_protocols
  DEPENDS ${WL_GEN_DIR}/wayland-client-protocol.h
          ${WL_GEN_DIR}/xdg-shell-client-protocol.h
          ${WL_GEN_DIR}/xdg-shell-protocol.c
          ${WL_GEN_DIR}/pointer-constraints-client-protocol.h
          ${WL_GEN_DIR}/pointer-constraints-protocol.c
          ${WL_GEN_DIR}/relative-pointer-client-protocol.h
          ${WL_GEN_DIR}/relative-pointer-protocol.c
          ${WL_GEN_DIR}/xdg-shell-server-protocol.h
          ${WL_GEN_DIR}/xdg-shell-server-protocol.c
          ${WL_GEN_DIR}/pointer-constraints-server-protocol.h
          ${WL_GEN_DIR}/pointer-constraints-server-protocol.c
          ${WL_GEN_DIR}/relative-pointer-server-protocol.h
          ${WL_GEN_DIR}/relative-pointer-server-protocol.c)

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
  src/SDL_surface.c
  src/SDL_gl.c
  src/SDL_vulkan.c
  src/internal/sdlop_evdev.c
  src/video/wayland/sdlop_wayland.c
  src/video/wayland/sdlop_wayland_gl.c
  src/video/wayland/sdlop_wayland_vulkan.c
  src/video/dummy/sdlop_dummy.c
  ${WL_GEN_DIR}/xdg-shell-protocol.c
  ${WL_GEN_DIR}/pointer-constraints-protocol.c
  ${WL_GEN_DIR}/relative-pointer-protocol.c
)

set(SDLop_LIB_DEPS
  ${WAYLAND_CLIENT_LIBRARIES}
  ${XKBCOMMON_LIBRARIES}
  ${EGL_LIBRARIES}
  ${GLESV2_LIBRARIES}
  pthread
  dl)
set(SDLop_LIB_DIRS
  ${WAYLAND_CLIENT_LIBRARY_DIRS}
  ${XKBCOMMON_LIBRARY_DIRS}
  ${EGL_LIBRARY_DIRS}
  ${GLESV2_LIBRARY_DIRS})
set(SDLop_LIB_INCS
  ${WAYLAND_CLIENT_INCLUDE_DIRS}
  ${XKBCOMMON_INCLUDE_DIRS}
  ${EGL_INCLUDE_DIRS}
  ${GLESV2_INCLUDE_DIRS})

add_library(sdlop STATIC ${SDLop_SOURCES})
add_library(SDLop::SDLop ALIAS sdlop)
add_dependencies(sdlop sdlop_wayland_protocols)

target_include_directories(sdlop PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(sdlop PRIVATE
  src
  src/video/wayland
  ${WL_GEN_DIR}
  ${SDLop_LIB_INCS})
target_link_directories(sdlop PUBLIC ${SDLop_LIB_DIRS})
target_link_libraries(sdlop PUBLIC ${SDLop_LIB_DEPS})
target_compile_definitions(sdlop PUBLIC SDLop_VIDEO_WAYLAND=1)

add_library(sdlop_shared SHARED ${SDLop_SOURCES})
add_dependencies(sdlop_shared sdlop_wayland_protocols)
set_target_properties(sdlop_shared PROPERTIES OUTPUT_NAME SDLop)
target_include_directories(sdlop_shared PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(sdlop_shared PRIVATE
  src
  src/video/wayland
  ${WL_GEN_DIR}
  ${SDLop_LIB_INCS})
target_link_directories(sdlop_shared PUBLIC ${SDLop_LIB_DIRS})
target_link_libraries(sdlop_shared PUBLIC ${SDLop_LIB_DEPS})
target_compile_definitions(sdlop_shared PUBLIC SDLop_VIDEO_WAYLAND=1)

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(SDLop_BUILD_EXAMPLES)
  find_package(Vulkan REQUIRED)
  foreach(ex window wasd raw_latency surface_plasma)
    add_executable(${ex} examples/${ex}.c)
    target_link_libraries(${ex} PRIVATE sdlop m)
  endforeach()

  add_executable(vulkan_clear examples/vulkan_clear.c)
  target_link_libraries(vulkan_clear PRIVATE sdlop Vulkan::Vulkan m)

  # GL example talks GLES2 directly
  add_executable(gl_triangle examples/gl_triangle.c)
  target_link_libraries(gl_triangle PRIVATE sdlop ${GLESV2_LIBRARIES})
  target_include_directories(gl_triangle PRIVATE ${GLESV2_INCLUDE_DIRS})
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SDLop_BUILD_TESTS)
  enable_testing()
  foreach(t test_ring test_scancode test_events test_surface)
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

  add_executable(test_gl tests/test_gl.c)
  target_link_libraries(test_gl PRIVATE sdlop ${GLESV2_LIBRARIES})
  target_include_directories(test_gl PRIVATE src ${GLESV2_INCLUDE_DIRS})
  target_compile_options(test_gl PRIVATE -UNDEBUG)
  add_test(NAME test_gl COMMAND test_gl)

  find_package(Vulkan REQUIRED)
  # pointer lock integration test (runs against tests/mini_compositor.c)
  pkg_check_modules(WAYLAND_SERVER REQUIRED wayland-server)
  add_executable(mini_compositor tests/mini_compositor.c
    ${WL_GEN_DIR}/xdg-shell-server-protocol.c
    ${WL_GEN_DIR}/pointer-constraints-server-protocol.c
    ${WL_GEN_DIR}/relative-pointer-server-protocol.c)
  add_dependencies(mini_compositor sdlop_wayland_protocols)
  target_include_directories(mini_compositor PRIVATE ${WL_GEN_DIR} ${WAYLAND_SERVER_INCLUDE_DIRS})
  target_link_libraries(mini_compositor PRIVATE ${WAYLAND_SERVER_LIBRARIES})

  add_executable(test_pointer_lock tests/test_pointer_lock.c)
  target_link_libraries(test_pointer_lock PRIVATE sdlop m)
  target_include_directories(test_pointer_lock PRIVATE src)
  target_compile_options(test_pointer_lock PRIVATE -UNDEBUG)
  add_dependencies(test_pointer_lock mini_compositor)
  add_test(NAME test_pointer_lock COMMAND test_pointer_lock $<TARGET_FILE:mini_compositor>)

  add_executable(test_vulkan tests/test_vulkan.c)
  target_link_libraries(test_vulkan PRIVATE sdlop Vulkan::Vulkan)
  target_include_directories(test_vulkan PRIVATE src)
  target_compile_options(test_vulkan PRIVATE -UNDEBUG)
  add_test(NAME test_vulkan COMMAND test_vulkan)
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
  add_executable(bench_render bench/bench_render.c)
  target_link_libraries(bench_render PRIVATE sdlop ${GLESV2_LIBRARIES})
  target_include_directories(bench_render PRIVATE ${GLESV2_INCLUDE_DIRS})
  add_executable(bench_raw_latency bench/bench_raw_latency.c tests/uinput_synth.c)
  target_link_libraries(bench_raw_latency PRIVATE sdlop)
  target_include_directories(bench_raw_latency PRIVATE src tests)
endif()
