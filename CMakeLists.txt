cmake_minimum_required(VERSION 3.13)
option(SDLOP_SANITIZERS "Build with ASan+UBSan and tune leak checks for GPU tests" OFF)

project(SDLop VERSION 0.1.0 LANGUAGES C)

if (SDLOP_SANITIZERS)
  string(APPEND CMAKE_C_FLAGS " -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer")
  string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=address,undefined")
  string(APPEND CMAKE_SHARED_LINKER_FLAGS " -fsanitize=address,undefined")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SDLop_BUILD_EXAMPLES "Build SDLop examples" ON)
option(SDLop_BUILD_TESTS "Build SDLop tests" ON)
option(SDLop_BUILD_BENCHMARKS "Build SDLop benchmarks (incl. SDL3 comparison)" ON)

# Video drivers (Linux build). All are compiled in by default; disabling one
# drops both its sources and its build-time header probes (the drivers load
# their runtime libraries with dlopen, so a driver never adds link-time
# dependencies). The X11 driver is header-free: it needs NOTHING X11-related
# at build time.
option(SDLop_VIDEO_WAYLAND "Build the Wayland video driver" ON)
option(SDLop_VIDEO_KMSDRM "Build the KMSDRM video driver" ON)
option(SDLop_VIDEO_X11 "Build the X11 video driver" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_definitions(_GNU_SOURCE)
add_compile_options(-Wall -Wextra)

# ---------------------------------------------------------------------------
# Web (Emscripten) build: canvas + WebGL driver, no xkbcommon/evdev/wayland
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
  set(SDLop_WEB_SOURCES
    src/SDLop_core.c
    src/SDL_error.c
    src/SDL_log.c
    src/SDL_timer.c
    src/SDL_events.c
    src/SDL_properties.c
    src/SDL_keyboard_web.c
    src/SDL_keynames.c
    src/SDL_mouse.c
    src/SDL_video.c
    src/SDL_surface.c
    src/SDL_gl.c
    src/SDL_vulkan.c
    src/video/emscripten/sdlop_emscripten.c
    src/video/dummy/sdlop_dummy.c)

  add_library(sdlop STATIC ${SDLop_WEB_SOURCES})
  add_library(SDLop::SDLop ALIAS sdlop)
  target_include_directories(sdlop PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  target_include_directories(sdlop PRIVATE src)
  target_compile_definitions(sdlop PUBLIC SDLop_VIDEO_EMSCRIPTEN=1)
  target_link_libraries(sdlop PUBLIC m)

  if(SDLop_BUILD_TESTS)
    enable_testing()
    foreach(t test_ring test_events test_surface test_web)
      add_executable(${t} tests/${t}.c)
      target_link_libraries(${t} PRIVATE sdlop)
      target_include_directories(${t} PRIVATE src)
      target_compile_options(${t} PRIVATE -UNDEBUG)
      target_link_options(${t} PRIVATE
        -sENVIRONMENT=node,web -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1)
      if(t STREQUAL "test_ring")
        # SPSC ring test uses a worker thread
        target_compile_options(${t} PRIVATE -pthread)
        target_link_options(${t} PRIVATE -pthread -sPTHREAD_POOL_SIZE=2)
      endif()
      add_test(NAME ${t} COMMAND node ${t}.js)
    endforeach()
  endif()

  if(SDLop_BUILD_EXAMPLES)
    foreach(ex window surface_plasma)
      add_executable(${ex} examples/${ex}.c)
      target_link_libraries(${ex} PRIVATE sdlop m)
      target_link_options(${ex} PRIVATE
        -sALLOW_MEMORY_GROWTH=1 -sASYNCIFY
        "-sASYNCIFY_IGNORE_INDIRECT=1")
    endforeach()
  endif()

  return()
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# SDLop dlopen-s every optional library at run time (wayland-client,
# libX11, xkbcommon, EGL, libdrm, libgbm, libvulkan - see the *_dyn.c
# loaders), so the sdlop/sdlop_shared targets link ONLY pthread, dl and m.
# No -dev package may be required just to produce the library on systems
# where the corresponding library is absent.
#
# The checks below are build-time *header* probes for the drivers that are
# compiled into the core (mirrors SDL3: headers are needed to build a driver,
# the library is loaded with dlopen when the driver initializes):
#   wayland-client - headers + wayland.xml consumed by wayland-scanner
#   xkbcommon      - xkbcommon.h (enums/types for the dlopen'd symbols)
#   egl            - EGL/egl.h + eglext.h
#   gbm            - gbm.h
#   libdrm         - xf86drm.h + xf86drmMode.h (headers only: the libdrm
#                    library itself is dlopen'ed at run time - never linked)
# The X11 driver is the exception: it compiles against a frozen ABI subset
# declared in-repo (sdlop_x11_defs.h) and needs no X11 headers at all.
# libvulkan / glesv2 / wayland-server are needed ONLY by tests, examples and
# benchmarks; those targets are skipped (with a message) when absent - see
# below.
# ---------------------------------------------------------------------------
# pkg-config is only needed for driver *header* probes (wayland/kmsdrm,
# xkbcommon). A driverless/X11-only build (e.g. minimal sandboxes) needs
# nothing at all beyond a C compiler.
find_package(PkgConfig QUIET)
pkg_check_modules(XKBCOMMON xkbcommon)
if(NOT XKBCOMMON_FOUND)
  # libxkbcommon is dlopen'ed at run time, so only its (frozen) type/enum
  # values are needed to build: fall back to the vendored ABI subset when
  # the -dev headers are absent (minimal sandboxes). The real header is
  # always preferred when installed.
  message(STATUS "xkbcommon headers not found - using vendored ABI subset (src/compat)")
  set(XKBCOMMON_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src/compat)
endif()

if(SDLop_VIDEO_WAYLAND)
  pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
  find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)
  pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
  pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
  pkg_get_variable(WAYLAND_CLIENT_DIR wayland-client pkgdatadir)
endif()
if(SDLop_VIDEO_WAYLAND OR SDLop_VIDEO_KMSDRM)
  pkg_check_modules(EGL REQUIRED egl)
endif()
if(SDLop_VIDEO_KMSDRM)
  pkg_check_modules(GBM REQUIRED gbm)
  pkg_check_modules(LIBDRM REQUIRED libdrm)
endif()
# GLES2 / Vulkan / wayland-server: optional, for tests/examples/benchmarks
pkg_check_modules(GLESV2 glesv2)
find_package(Vulkan QUIET)
pkg_check_modules(WAYLAND_SERVER wayland-server)

set(WL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland-gen)
if(SDLop_VIDEO_WAYLAND)
file(MAKE_DIRECTORY ${WL_GEN_DIR})

add_custom_command(
  OUTPUT ${WL_GEN_DIR}/wayland-client-protocol.h ${WL_GEN_DIR}/wayland-client-protocol.c
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header
          ${WAYLAND_CLIENT_DIR}/wayland.xml
          ${WL_GEN_DIR}/wayland-client-protocol.h
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} code
          ${WAYLAND_CLIENT_DIR}/wayland.xml
          ${WL_GEN_DIR}/wayland-client-protocol.c
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
endif() # SDLop_VIDEO_WAYLAND

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(SDLop_SOURCES
  src/SDLop_core.c
  src/SDL_error.c
  src/SDL_log.c
  src/SDL_timer.c
  src/SDL_events.c
  src/SDL_properties.c
  src/SDL_keyboard.c
  src/SDL_keynames.c
  src/SDL_mouse.c
  src/SDL_video.c
  src/SDL_surface.c
  src/SDL_gl.c
  src/SDL_vulkan.c
  src/internal/sdlop_evdev.c
  src/sdlop_xkb_dyn.c
  src/video/dummy/sdlop_dummy.c
)

set(SDLop_DRIVER_DEFS "")
set(SDLop_DRIVER_INCS "")
set(SDLop_DRIVER_DEPS "")

if(SDLop_VIDEO_WAYLAND)
  list(APPEND SDLop_SOURCES
    src/video/wayland/sdlop_wayland.c
    src/video/wayland/sdlop_wayland_dyn.c
    src/video/wayland/sdlop_egl_dyn.c
    src/video/wayland/sdlop_wayland_gl.c
    src/video/wayland/sdlop_wayland_vulkan.c
    src/video/wayland/sdlop_wayland_protos.c)
  list(APPEND SDLop_DRIVER_DEFS SDLop_VIDEO_WAYLAND=1)
  list(APPEND SDLop_DRIVER_INCS src/video/wayland ${WL_GEN_DIR} ${WAYLAND_CLIENT_INCLUDE_DIRS})
  list(APPEND SDLop_DRIVER_DEPS sdlop_wayland_protocols)
endif()

if(SDLop_VIDEO_KMSDRM)
  list(APPEND SDLop_SOURCES
    src/video/kmsdrm/sdlop_kmsdrm.c
    src/video/kmsdrm/sdlop_kmsdrm_atomic.c
    src/video/kmsdrm/sdlop_kmsdrm_dyn.c
    src/video/kmsdrm/sdlop_kmsdrm_gl.c
    src/video/kmsdrm/sdlop_kmsdrm_vulkan.c
    src/video/kmsdrm/sdlop_xcursor.c)
  list(APPEND SDLop_DRIVER_DEFS SDLop_VIDEO_KMSDRM=1)
  list(APPEND SDLop_DRIVER_INCS src/video/kmsdrm ${GBM_INCLUDE_DIRS} ${LIBDRM_INCLUDE_DIRS})
endif()

if(SDLop_VIDEO_X11)
  # header-free: compiles against sdlop_x11_defs.h (frozen Xlib ABI) and
  # dlopen-s libX11.so.6 at run time - no X11 package needed at all
  list(APPEND SDLop_SOURCES
    src/video/x11/sdlop_x11.c
    src/video/x11/sdlop_x11_dyn.c
    src/video/x11/sdlop_x11_glx.c
    src/video/x11/sdlop_x11_vulkan.c)
  list(APPEND SDLop_DRIVER_DEFS SDLop_VIDEO_X11=1)
  list(APPEND SDLop_DRIVER_INCS src/video/x11)
endif()

if(NOT SDLop_VIDEO_WAYLAND AND NOT SDLop_VIDEO_KMSDRM AND NOT SDLop_VIDEO_X11)
  message(STATUS "SDLop: all display drivers disabled - building dummy-only")
endif()

# SDL3-style dynamic loading: wayland-client, libX11, xkbcommon and EGL are
# dlopen-ed at runtime, NOT linked - SDLop loads (and its other drivers work)
# on systems where those libraries are absent.
set(SDLop_LIB_DEPS
  pthread
  dl
  m)
# dlopen-ed at run time - the core targets link none of these libraries.
# Only their header search paths are needed.
set(SDLop_LIB_INCS
  ${XKBCOMMON_INCLUDE_DIRS}
  ${EGL_INCLUDE_DIRS}
  ${SDLop_DRIVER_INCS})

add_library(sdlop STATIC ${SDLop_SOURCES})
add_library(SDLop::SDLop ALIAS sdlop)
if(SDLop_DRIVER_DEPS)
  add_dependencies(sdlop ${SDLop_DRIVER_DEPS})
endif()

target_include_directories(sdlop PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(sdlop PRIVATE
  src
  ${SDLop_LIB_INCS})
target_link_libraries(sdlop PUBLIC ${SDLop_LIB_DEPS})
target_compile_definitions(sdlop PUBLIC ${SDLop_DRIVER_DEFS})

add_library(sdlop_shared SHARED ${SDLop_SOURCES})
if(SDLop_DRIVER_DEPS)
  add_dependencies(sdlop_shared ${SDLop_DRIVER_DEPS})
endif()
set_target_properties(sdlop_shared PROPERTIES OUTPUT_NAME SDLop)
target_include_directories(sdlop_shared PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(sdlop_shared PRIVATE
  src
  ${SDLop_LIB_INCS})
target_link_libraries(sdlop_shared PUBLIC ${SDLop_LIB_DEPS})
target_compile_definitions(sdlop_shared PUBLIC ${SDLop_DRIVER_DEFS})

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(SDLop_BUILD_EXAMPLES)
  foreach(ex window wasd raw_latency surface_plasma)
    add_executable(${ex} examples/${ex}.c)
    target_link_libraries(${ex} PRIVATE sdlop m)
  endforeach()

  if(Vulkan_FOUND)
    add_executable(vulkan_clear examples/vulkan_clear.c)
    target_link_libraries(vulkan_clear PRIVATE sdlop Vulkan::Vulkan m)
  else()
    message(STATUS "Vulkan not found - skipping vulkan_clear example")
  endif()

  # GL example talks GLES2 directly
  if(GLESV2_FOUND)
    add_executable(gl_triangle examples/gl_triangle.c)
    target_link_libraries(gl_triangle PRIVATE sdlop ${GLESV2_LIBRARIES})
    target_include_directories(gl_triangle PRIVATE ${GLESV2_INCLUDE_DIRS})
  else()
    message(STATUS "GLESv2 not found - skipping gl_triangle example")
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SDLop_BUILD_TESTS)
  enable_testing()
  foreach(t test_ring test_scancode test_events test_surface test_web)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE sdlop)
    target_include_directories(${t} PRIVATE src)
    target_compile_options(${t} PRIVATE -UNDEBUG)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()

  # Manual, externally driven trace tool. It is intentionally not registered
  # with CTest: it waits for a compositor-side input script after READY-INPUT.
  if(SDLop_VIDEO_WAYLAND)
    add_executable(behaviour_probe_wayland tools/behaviour_probe_wayland.c)
    target_link_libraries(behaviour_probe_wayland PRIVATE sdlop)
    target_include_directories(behaviour_probe_wayland PRIVATE src)

    # Reference compositor-side driver for the probe: injects the scripted
    # pointer/key sequence once READY-INPUT appears. Also not a CTest.
    add_executable(behaviour_probe_inject tools/behaviour_probe_inject.c
      ${WL_GEN_DIR}/wlr-virtual-pointer-protocol.c
      ${WL_GEN_DIR}/virtual-keyboard-protocol.c)
    add_dependencies(behaviour_probe_inject sdlop_wayland_protocols)
    target_include_directories(behaviour_probe_inject PRIVATE
      ${WL_GEN_DIR} ${WAYLAND_CLIENT_INCLUDE_DIRS} ${XKBCOMMON_INCLUDE_DIRS})
    target_link_libraries(behaviour_probe_inject PRIVATE
      ${WAYLAND_CLIENT_LIBRARIES} ${XKBCOMMON_LIBRARIES})
  endif()

  # integration tests that need real subsystems (auto-skip when unavailable)
  add_executable(test_evdev_uinput tests/test_evdev_uinput.c tests/uinput_synth.c)
  target_link_libraries(test_evdev_uinput PRIVATE sdlop)
  target_include_directories(test_evdev_uinput PRIVATE src tests)
  target_compile_options(test_evdev_uinput PRIVATE -UNDEBUG)
  add_test(NAME test_evdev_uinput COMMAND test_evdev_uinput)

  if(SDLop_VIDEO_WAYLAND)
    add_executable(test_wayland tests/test_wayland.c)
    target_link_libraries(test_wayland PRIVATE sdlop)
    target_include_directories(test_wayland PRIVATE src)
    target_compile_options(test_wayland PRIVATE -UNDEBUG)
    add_test(NAME test_wayland COMMAND test_wayland)
  endif()

  if(SDLop_VIDEO_KMSDRM)
    add_executable(test_kmsdrm tests/test_kmsdrm.c)
    target_link_libraries(test_kmsdrm PRIVATE sdlop)
    target_include_directories(test_kmsdrm PRIVATE src)
    target_compile_options(test_kmsdrm PRIVATE -UNDEBUG)
    add_test(NAME test_kmsdrm COMMAND test_kmsdrm)

    # KMS present-path benchmark (not a ctest gate; skips without KMS)
    add_executable(bench_kmsdrm tests/bench_kmsdrm.c)
    target_link_libraries(bench_kmsdrm PRIVATE sdlop)
    target_include_directories(bench_kmsdrm PRIVATE src)
    target_compile_options(bench_kmsdrm PRIVATE -UNDEBUG)
  endif()

  if(SDLop_VIDEO_X11)
    # X11 integration: runs against a real X server when DISPLAY is set
    # (Xvfb in CI), skips cleanly otherwise. Use tests/run_x11_test.sh for
    # a self-managed Xvfb / mini-server run.
    add_executable(test_x11 tests/test_x11.c)
    target_link_libraries(test_x11 PRIVATE sdlop)
    target_include_directories(test_x11 PRIVATE src)
    target_compile_options(test_x11 PRIVATE -UNDEBUG)
    add_test(NAME test_x11 COMMAND test_x11)

    # scripted end-to-end run under a managed server: Xvfb when present,
    # otherwise the in-repo pure-python mini server (which also injects a
    # deterministic input script the test asserts on)
    find_program(SH_EXECUTABLE sh)
    if(SH_EXECUTABLE)
      add_test(NAME test_x11_server
        COMMAND ${SH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_x11_test.sh)
      set_tests_properties(test_x11_server PROPERTIES
        ENVIRONMENT "TEST_X11_BIN=$<TARGET_FILE:test_x11>")
    endif()

    # graceful-failure paths (no X server needed: verifies clean SDL errors)
    add_executable(test_x11_nodisplay tests/test_x11_nodisplay.c)
    target_link_libraries(test_x11_nodisplay PRIVATE sdlop)
    target_include_directories(test_x11_nodisplay PRIVATE src)
    target_compile_options(test_x11_nodisplay PRIVATE -UNDEBUG)
    add_test(NAME test_x11_nodisplay COMMAND test_x11_nodisplay)
  endif()

  # test_gl drives EGL/GLES2 (wayland/kmsdrm GL path); the X11 driver uses
  # GLX instead, exercised by test_x11
  if(GLESV2_FOUND AND (SDLop_VIDEO_WAYLAND OR SDLop_VIDEO_KMSDRM))
    add_executable(test_gl tests/test_gl.c)
    target_link_libraries(test_gl PRIVATE sdlop ${GLESV2_LIBRARIES})
    target_include_directories(test_gl PRIVATE src ${GLESV2_INCLUDE_DIRS})
    target_compile_options(test_gl PRIVATE -UNDEBUG)
    add_test(NAME test_gl COMMAND test_gl)
  else()
    message(STATUS "GLESv2 not found - skipping test_gl")
  endif()

  # pointer lock integration test (runs against tests/mini_compositor.c)
  if(WAYLAND_SERVER_FOUND AND SDLop_VIDEO_WAYLAND)
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

    add_executable(test_touch tests/test_touch.c)
    target_link_libraries(test_touch PRIVATE sdlop m)
    target_include_directories(test_touch PRIVATE src)
    target_compile_options(test_touch PRIVATE -UNDEBUG)
    add_dependencies(test_touch mini_compositor)
    add_test(NAME test_touch COMMAND test_touch $<TARGET_FILE:mini_compositor>)
  else()
    message(STATUS "wayland-server not found - skipping mini_compositor, test_pointer_lock, test_touch")
  endif()

  add_executable(smoke_relative_mode tests/smoke_relative_mode.c)
  target_link_libraries(smoke_relative_mode PRIVATE sdlop)
  add_test(NAME smoke_relative_mode COMMAND smoke_relative_mode)

  if(GLESV2_FOUND AND (SDLop_VIDEO_WAYLAND OR SDLop_VIDEO_KMSDRM))
    add_executable(test_gl_matrix tests/test_gl_matrix.c)
    target_link_libraries(test_gl_matrix PRIVATE sdlop ${GLESV2_LIBRARIES})
    target_include_directories(test_gl_matrix PRIVATE src ${GLESV2_INCLUDE_DIRS})
    target_compile_options(test_gl_matrix PRIVATE -UNDEBUG)
    add_test(NAME test_gl_matrix COMMAND test_gl_matrix)
  else()
    message(STATUS "GLESv2 not found - skipping test_gl_matrix")
  endif()

  if(Vulkan_FOUND)
    add_executable(test_vulkan tests/test_vulkan.c)
    target_link_libraries(test_vulkan PRIVATE sdlop Vulkan::Vulkan)
    target_include_directories(test_vulkan PRIVATE src)
    target_compile_options(test_vulkan PRIVATE -UNDEBUG)
    add_test(NAME test_vulkan COMMAND test_vulkan)

    add_executable(test_vulkan_compute tests/test_vulkan_compute.c)
    target_link_libraries(test_vulkan_compute PRIVATE sdlop Vulkan::Vulkan)
    target_include_directories(test_vulkan_compute PRIVATE src tests)
    target_compile_options(test_vulkan_compute PRIVATE -UNDEBUG)
    add_test(NAME test_vulkan_compute COMMAND test_vulkan_compute)
  else()
    message(STATUS "Vulkan not found - skipping test_vulkan, test_vulkan_compute")
  endif()

  # wlr virtual-input protocols (vendored XMLs) for the sway end-to-end test
  if(SDLop_VIDEO_WAYLAND)
  add_custom_command(
    OUTPUT ${WL_GEN_DIR}/wlr-virtual-pointer-client-protocol.h ${WL_GEN_DIR}/wlr-virtual-pointer-protocol.c
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header
            ${CMAKE_SOURCE_DIR}/tests/protocols/wlr-virtual-pointer-unstable-v1.xml
            ${WL_GEN_DIR}/wlr-virtual-pointer-client-protocol.h
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
            ${CMAKE_SOURCE_DIR}/tests/protocols/wlr-virtual-pointer-unstable-v1.xml
            ${WL_GEN_DIR}/wlr-virtual-pointer-protocol.c
    DEPENDS ${CMAKE_SOURCE_DIR}/tests/protocols/wlr-virtual-pointer-unstable-v1.xml)

  add_custom_command(
    OUTPUT ${WL_GEN_DIR}/virtual-keyboard-client-protocol.h ${WL_GEN_DIR}/virtual-keyboard-protocol.c
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header
            ${CMAKE_SOURCE_DIR}/tests/protocols/virtual-keyboard-unstable-v1.xml
            ${WL_GEN_DIR}/virtual-keyboard-client-protocol.h
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
            ${CMAKE_SOURCE_DIR}/tests/protocols/virtual-keyboard-unstable-v1.xml
            ${WL_GEN_DIR}/virtual-keyboard-protocol.c
    DEPENDS ${CMAKE_SOURCE_DIR}/tests/protocols/virtual-keyboard-unstable-v1.xml)

  if(SDLop_VIDEO_WAYLAND AND WAYLAND_CLIENT_FOUND)
    add_executable(test_virtual_input tests/test_virtual_input.c
      ${WL_GEN_DIR}/wlr-virtual-pointer-protocol.c
      ${WL_GEN_DIR}/virtual-keyboard-protocol.c)
    target_link_libraries(test_virtual_input PRIVATE sdlop ${WAYLAND_CLIENT_LIBRARIES})
    target_include_directories(test_virtual_input PRIVATE src ${WL_GEN_DIR} ${WAYLAND_CLIENT_INCLUDE_DIRS})
    target_compile_options(test_virtual_input PRIVATE -UNDEBUG)
    add_test(NAME test_virtual_input COMMAND test_virtual_input)
  else()
    message(STATUS "wayland-client not found - skipping test_virtual_input")
  endif()
  endif() # SDLop_VIDEO_WAYLAND
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
  if(GLESV2_FOUND)
    add_executable(bench_render bench/bench_render.c)
    target_link_libraries(bench_render PRIVATE sdlop ${GLESV2_LIBRARIES})
    target_include_directories(bench_render PRIVATE ${GLESV2_INCLUDE_DIRS})
  else()
    message(STATUS "GLESv2 not found - skipping bench_render")
  endif()
  add_executable(bench_raw_latency bench/bench_raw_latency.c tests/uinput_synth.c)
  target_link_libraries(bench_raw_latency PRIVATE sdlop)
  target_include_directories(bench_raw_latency PRIVATE src tests)
endif()

# ABI parity gate: builds tools/abi_probe.c against these headers and against
# stock SDL3 headers (-DSDL3_REFERENCE_INCLUDE=/path/to/SDL3/include) and
# diffs every size/offset/enum value. Skipped when no reference is provided.
if (SDL3_REFERENCE_INCLUDE AND NOT EMSCRIPTEN)
  add_test(NAME abi_check COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_abi_check.sh ${SDL3_REFERENCE_INCLUDE})
endif()

# Wayland behaviour injector (virtual pointer + keyboard script driver);
# pairs with the shared behaviour_probe_wayland on the agent branch and
# tests/run_behaviour_wayland.sh
if (NOT EMSCRIPTEN AND SDLop_VIDEO_WAYLAND AND WAYLAND_CLIENT_FOUND AND XKBCOMMON_FOUND)
  add_executable(wl_injector tools/wl_injector.c ${WL_GEN_DIR}/virtual-keyboard-protocol.c ${WL_GEN_DIR}/wlr-virtual-pointer-protocol.c)
  target_include_directories(wl_injector PRIVATE ${WL_GEN_DIR} ${WAYLAND_CLIENT_INCLUDE_DIRS} ${XKBCOMMON_INCLUDE_DIRS})
  target_link_libraries(wl_injector PRIVATE ${WAYLAND_CLIENT_LIBRARIES} ${XKBCOMMON_LIBRARIES})
endif()

# API subset gate: every SDL_* symbol exported by SDLop must also be exported
# by stock SDL3 (strict-subset property). Skipped when no reference .so given.
if (SDL3_REFERENCE_LIB AND NOT EMSCRIPTEN)
  add_test(NAME api_subset_check
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/api_subset_check.sh
                   $<TARGET_FILE:sdlop_shared> ${SDL3_REFERENCE_LIB})
endif()

if (SDLOP_SANITIZERS AND NOT EMSCRIPTEN)
  # llvmpipe/lavapipe leak inside libLLVM under LSan (verified: leak frames are
  # entirely Mesa/LLVM internals); disable leak detection for the GPU tests
  # only - every other test keeps full leak checking
  foreach(t test_gl test_gl_matrix test_vulkan_compute)
    if (TARGET ${t})
      set_tests_properties(${t} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
    endif()
  endforeach()
  # test_surface asserts the alloc-failure contract (CreateSurface must
  # return NULL); ASan's allocator aborts on OOM unless allowed to return null
  set_tests_properties(test_surface
    PROPERTIES ENVIRONMENT "ASAN_OPTIONS=allocator_may_return_null=1")
endif()
