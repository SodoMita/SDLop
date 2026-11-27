cmake_minimum_required(VERSION 3.13)
option(SDLOP_SANITIZERS "Build with ASan+UBSan and tune leak checks for GPU tests" OFF)

project(SDLop VERSION 0.2.0 LANGUAGES C)

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
option(SDLop_BUILD_TOOLS "Build developer tools (wl_injector, behaviour probes)" ${SDLop_BUILD_TESTS})

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
    src/SDL_stdlib.c
    src/SDL_error.c
    src/SDL_log.c
    src/SDL_timer.c
    src/SDL_events.c
    src/SDL_properties.c
    src/SDL_hints.c
    src/SDL_events_desc.c
    src/SDL_events_categories.c
    src/SDL_clipboard.c
    src/SDL_dropevents.c
    src/SDL_thread.c
    src/SDL_misc.c
    src/SDL_joystick.c
    src/SDL_joystick_dummy_backend.c
    src/SDL_haptic.c
    src/SDL_haptic_dummy.c
    src/SDL_filesystem.c
    src/SDL_main.c
    src/SDL_rect.c
    src/SDL_pixels.c
    src/SDL_blit.c
    src/SDL_blit_float.c
    src/SDL_rotate.c
    src/SDL_keyboard_web.c
    src/SDL_keynames.c
    src/SDL_mouse.c
    src/SDL_pen.c
    src/SDL_devices.c
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
# xkbcommon, EGL, libdrm, libgbm, libvulkan - see the *_dyn.c loaders), so the
# sdlop/sdlop_shared targets link ONLY pthread, dl and m. No -dev package may
# be required just to produce the library on systems where the corresponding
# library is absent.
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
#   xcb, xcb-shm, xcb-xkb, xcb-xinput, xcb-randr, xkbcommon-x11
#                  - X11 driver headers (optional: the driver is skipped when
#                    they are missing; SDLOP_VIDEO_X11=OFF disables it). The
#                    libraries are dlopen'ed when the driver initializes.
# libvulkan / glesv2 / wayland-server are needed ONLY by tests, examples and
# benchmarks; those targets are skipped (with a message) when absent - see
# below.
# ---------------------------------------------------------------------------
if(NOT WIN32)
  # POSIX system/header probes and optional driver dependencies. The Windows
  # build links only Win32 system libraries (see the WIN32 block below).
  include(CheckIncludeFile)
  include(CheckSymbolExists)
  include(CheckLibraryExists)
  include(CheckCSourceCompiles)
  find_package(Threads REQUIRED)
  find_package(PkgConfig QUIET)

  set(SDLop_EXTRA_POSIX_LIBS)
  set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)

  check_symbol_exists(memfd_create "sys/mman.h" SDLOP_HAVE_MEMFD_CREATE)
  check_symbol_exists(shm_open "sys/mman.h;fcntl.h" SDLOP_HAVE_SHM_OPEN)
  if(NOT SDLOP_HAVE_SHM_OPEN)
    check_library_exists(rt shm_open "" SDLOP_SHM_OPEN_IN_RT)
    if(SDLOP_SHM_OPEN_IN_RT)
      set(SDLOP_HAVE_SHM_OPEN 1)
      list(APPEND SDLop_EXTRA_POSIX_LIBS rt)
    endif()
  endif()
  check_symbol_exists(mkostemp "stdlib.h" SDLOP_HAVE_MKOSTEMP)
  check_symbol_exists(pipe2 "unistd.h;fcntl.h" SDLOP_HAVE_PIPE2)
  check_symbol_exists(eventfd "sys/eventfd.h" SDLOP_HAVE_EVENTFD)
  check_symbol_exists(posix_fallocate "fcntl.h" SDLOP_HAVE_POSIX_FALLOCATE)
  check_include_file(pthread_np.h SDLOP_HAVE_PTHREAD_NP_H)
  set(_saved_req_libs "${CMAKE_REQUIRED_LIBRARIES}")
  list(APPEND CMAKE_REQUIRED_LIBRARIES Threads::Threads)
  check_symbol_exists(clock_nanosleep "time.h" SDLOP_HAVE_CLOCK_NANOSLEEP)
  check_symbol_exists(sem_clockwait "semaphore.h;time.h" SDLOP_HAVE_SEM_CLOCKWAIT)
  check_symbol_exists(sem_clockwait_np "semaphore.h;time.h" SDLOP_HAVE_SEM_CLOCKWAIT_NP)
  check_symbol_exists(sem_timedwait "semaphore.h;time.h" SDLOP_HAVE_SEM_TIMEDWAIT)
  check_symbol_exists(pthread_condattr_setclock "pthread.h;time.h" SDLOP_HAVE_PTHREAD_CONDATTR_SETCLOCK)
  check_c_source_compiles("
    #include <pthread.h>
    int main(void) {
      #ifdef __APPLE__
      pthread_setname_np(\"\");
      #elif defined(__NetBSD__)
      pthread_setname_np(pthread_self(), \"%s\", \"\");
      #else
      pthread_setname_np(pthread_self(), \"\");
      #endif
      return 0;
    }" SDLOP_HAVE_PTHREAD_SETNAME_NP)
  if(SDLOP_HAVE_PTHREAD_NP_H)
    check_symbol_exists(pthread_set_name_np "pthread.h;pthread_np.h" SDLOP_HAVE_PTHREAD_SET_NAME_NP)
  else()
    set(SDLOP_HAVE_PTHREAD_SET_NAME_NP 0)
  endif()
  set(CMAKE_REQUIRED_LIBRARIES "${_saved_req_libs}")

  check_include_file(iconv.h SDLOP_HAVE_ICONV_H)
  set(SDLOP_HAVE_ICONV 0)
  set(SDLOP_ICONV_CONST_INBUF 0)
  if(SDLOP_HAVE_ICONV_H)
    check_symbol_exists(iconv_open "iconv.h" SDLOP_ICONV_IN_LIBC)
    if(SDLOP_ICONV_IN_LIBC)
      set(SDLOP_HAVE_ICONV 1)
    else()
      check_library_exists(iconv iconv_open "" SDLOP_ICONV_IN_LIBICONV)
      if(SDLOP_ICONV_IN_LIBICONV)
        set(SDLOP_HAVE_ICONV 1)
        list(APPEND SDLop_EXTRA_POSIX_LIBS iconv)
      endif()
    endif()
    if(SDLOP_HAVE_ICONV)
      set(_saved_flags "${CMAKE_REQUIRED_FLAGS}")
      set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror")
      check_c_source_compiles("
        #include <iconv.h>
        int main(void) {
          const char *in = 0; size_t il = 0; char *out = 0; size_t ol = 0;
          return (int)iconv((iconv_t)0, &in, &il, &out, &ol);
        }" SDLOP_ICONV_CONST_INBUF)
      set(CMAKE_REQUIRED_FLAGS "${_saved_flags}")
    endif()
  endif()
  unset(CMAKE_REQUIRED_DEFINITIONS)

  add_compile_definitions(SDLOP_CMAKE_PROBED=1)
  foreach(_probe
      SDLOP_HAVE_MEMFD_CREATE SDLOP_HAVE_SHM_OPEN SDLOP_HAVE_MKOSTEMP
      SDLOP_HAVE_PIPE2 SDLOP_HAVE_EVENTFD SDLOP_HAVE_POSIX_FALLOCATE
      SDLOP_HAVE_CLOCK_NANOSLEEP SDLOP_HAVE_SEM_CLOCKWAIT SDLOP_HAVE_SEM_CLOCKWAIT_NP
      SDLOP_HAVE_SEM_TIMEDWAIT SDLOP_HAVE_PTHREAD_CONDATTR_SETCLOCK
      SDLOP_HAVE_PTHREAD_NP_H SDLOP_HAVE_PTHREAD_SETNAME_NP SDLOP_HAVE_PTHREAD_SET_NAME_NP
      SDLOP_HAVE_ICONV)
    if(${_probe})
      add_compile_definitions(${_probe}=1)
    else()
      add_compile_definitions(${_probe}=0)
    endif()
  endforeach()
  if(SDLOP_ICONV_CONST_INBUF)
    add_compile_definitions(SDLOP_ICONV_CONST_INBUF=1)
  endif()

  option(SDLOP_VIDEO_WAYLAND "Build the Wayland video driver" ON)
  option(SDLOP_VIDEO_KMSDRM "Build the KMS/DRM video driver" ON)
  option(SDLOP_VIDEO_FBDEV "Build the Linux fbdev video driver" ON)
  option(SDLOP_VIDEO_X11 "Build the X11 video driver (needs xcb headers; libraries are dlopen'ed)" ON)

  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SDLOP_VIDEO_FBDEV OFF)
  endif()

  if(PkgConfig_FOUND)
    if(SDLOP_VIDEO_WAYLAND)
      pkg_check_modules(WAYLAND_CLIENT wayland-client)
      pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols)
    endif()
    pkg_check_modules(XKBCOMMON xkbcommon)
    pkg_check_modules(EGL egl)
    if(SDLOP_VIDEO_KMSDRM)
      pkg_check_modules(GBM gbm)
      pkg_check_modules(LIBDRM libdrm)
    endif()
    if(SDLOP_VIDEO_X11)
      pkg_check_modules(X11_XCB xcb xcb-shm xcb-xkb xcb-xinput xcb-randr xcb-render xkbcommon-x11)
      if(NOT X11_XCB_FOUND)
        message(STATUS "xcb headers not found - building without the X11 driver")
        set(SDLOP_VIDEO_X11 OFF)
      endif()
    endif()
    # GLES2 / wayland-server: optional, for tests/examples/benchmarks
    pkg_check_modules(GLESV2 glesv2)
    pkg_check_modules(WAYLAND_SERVER wayland-server)
  else()
    set(SDLOP_VIDEO_X11 OFF)
  endif()
  find_package(Vulkan QUIET)

  if(XKBCOMMON_FOUND)
    set(SDLOP_HAVE_XKBCOMMON 1)
  else()
    check_include_file("xkbcommon/xkbcommon.h" SDLOP_HAVE_XKB_H)
    check_include_file("xkbcommon/xkbcommon-compose.h" SDLOP_HAVE_XKB_COMPOSE_H)
    if(SDLOP_HAVE_XKB_H AND SDLOP_HAVE_XKB_COMPOSE_H)
      set(SDLOP_HAVE_XKBCOMMON 1)
    else()
      set(SDLOP_HAVE_XKBCOMMON 0)
      message(STATUS "xkbcommon headers not found - building with built-in US keymap fallback")
    endif()
  endif()
  add_compile_definitions(SDLOP_HAVE_XKBCOMMON=${SDLOP_HAVE_XKBCOMMON})

  if(EGL_FOUND)
    set(SDLOP_HAVE_EGL 1)
  else()
    check_include_file("EGL/egl.h" SDLOP_HAVE_EGL_H)
    check_include_file("EGL/eglext.h" SDLOP_HAVE_EGLEXT_H)
    if(SDLOP_HAVE_EGL_H AND SDLOP_HAVE_EGLEXT_H)
      set(SDLOP_HAVE_EGL 1)
    else()
      set(SDLOP_HAVE_EGL 0)
      message(STATUS "EGL headers not found - building without EGL/GL offload")
    endif()
  endif()
  add_compile_definitions(SDLOP_HAVE_EGL=${SDLOP_HAVE_EGL})

  if(SDLOP_VIDEO_KMSDRM)
    if(NOT (GBM_FOUND AND LIBDRM_FOUND))
      check_include_file("xf86drm.h" SDLOP_HAVE_XF86DRM_H)
      check_include_file("xf86drmMode.h" SDLOP_HAVE_XF86DRM_MODE_H)
      check_include_file("gbm.h" SDLOP_HAVE_GBM_H)
      if(NOT (SDLOP_HAVE_XF86DRM_H AND SDLOP_HAVE_XF86DRM_MODE_H AND SDLOP_HAVE_GBM_H))
        message(STATUS "libdrm/gbm headers not found - building without the KMSDRM driver")
        set(SDLOP_VIDEO_KMSDRM OFF)
      endif()
    endif()
  endif()

  if(SDLOP_VIDEO_WAYLAND)
    find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner)
    if(NOT (WAYLAND_CLIENT_FOUND AND WAYLAND_PROTOCOLS_FOUND AND WAYLAND_SCANNER_EXECUTABLE))
      message(STATUS "wayland-client/wayland-protocols/wayland-scanner not found - building without the Wayland driver")
      set(SDLOP_VIDEO_WAYLAND OFF)
    endif()
  endif()

if(SDLOP_VIDEO_WAYLAND)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(WAYLAND_CLIENT_DIR wayland-client pkgdatadir)

set(WL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland-gen)
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

# optional window-management protocols (bound when the compositor offers
# them): server-side decorations, idle inhibit, keyboard-shortcuts inhibit,
# activation/attention
set(SDLOP_WL_EXTRA_PROTOCOLS
  "xdg-decoration|unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
  "idle-inhibit|unstable/idle-inhibit/idle-inhibit-unstable-v1.xml"
  "keyboard-shortcuts-inhibit|unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml"
  "xdg-activation|staging/xdg-activation/xdg-activation-v1.xml"
  "viewporter|stable/viewporter/viewporter.xml"
  "primary-selection|unstable/primary-selection/primary-selection-unstable-v1.xml")
set(SDLOP_WL_EXTRA_OUTPUTS)
foreach(entry ${SDLOP_WL_EXTRA_PROTOCOLS})
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 proto_name)
  list(GET parts 1 proto_xml)
  add_custom_command(
    OUTPUT ${WL_GEN_DIR}/${proto_name}-client-protocol.h ${WL_GEN_DIR}/${proto_name}-protocol.c
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header
            ${WAYLAND_PROTOCOLS_DIR}/${proto_xml}
            ${WL_GEN_DIR}/${proto_name}-client-protocol.h
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
            ${WAYLAND_PROTOCOLS_DIR}/${proto_xml}
            ${WL_GEN_DIR}/${proto_name}-protocol.c
    DEPENDS ${WAYLAND_PROTOCOLS_DIR}/${proto_xml})
  list(APPEND SDLOP_WL_EXTRA_OUTPUTS
    ${WL_GEN_DIR}/${proto_name}-client-protocol.h ${WL_GEN_DIR}/${proto_name}-protocol.c)
endforeach()

# protocols newer than some distributions' wayland-protocols, vendored
# like stock SDL3 does (src/video/wayland/protocols)
set(SDLOP_WL_VENDORED_DIR ${CMAKE_SOURCE_DIR}/src/video/wayland/protocols)
set(SDLOP_WL_VENDORED_PROTOCOLS
  fractional-scale-v1
  text-input-unstable-v3
  tablet-v2
  cursor-shape-v1
  xdg-toplevel-icon-v1
  xdg-dialog-v1
  alpha-modifier-v1
  pointer-warp-v1
  pointer-gestures-unstable-v1
  xdg-foreign-unstable-v2)
foreach(proto_name ${SDLOP_WL_VENDORED_PROTOCOLS})
  set(proto_xml ${SDLOP_WL_VENDORED_DIR}/${proto_name}.xml)
  add_custom_command(
    OUTPUT ${WL_GEN_DIR}/${proto_name}-client-protocol.h ${WL_GEN_DIR}/${proto_name}-protocol.c
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${proto_xml} ${WL_GEN_DIR}/${proto_name}-client-protocol.h
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${proto_xml} ${WL_GEN_DIR}/${proto_name}-protocol.c
    DEPENDS ${proto_xml})
  add_custom_command(
    OUTPUT ${WL_GEN_DIR}/${proto_name}-server-protocol.h ${WL_GEN_DIR}/${proto_name}-server-protocol.c
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} server-header ${proto_xml} ${WL_GEN_DIR}/${proto_name}-server-protocol.h
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${proto_xml} ${WL_GEN_DIR}/${proto_name}-server-protocol.c
    DEPENDS ${proto_xml})
  list(APPEND SDLOP_WL_EXTRA_OUTPUTS
    ${WL_GEN_DIR}/${proto_name}-client-protocol.h ${WL_GEN_DIR}/${proto_name}-protocol.c
    ${WL_GEN_DIR}/${proto_name}-server-protocol.h ${WL_GEN_DIR}/${proto_name}-server-protocol.c)
endforeach()

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
  OUTPUT ${WL_GEN_DIR}/viewporter-server-protocol.h ${WL_GEN_DIR}/viewporter-server-protocol.c
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} server-header
          ${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml
          ${WL_GEN_DIR}/viewporter-server-protocol.h
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
          ${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml
          ${WL_GEN_DIR}/viewporter-server-protocol.c
  DEPENDS ${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml)

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
  OUTPUT ${WL_GEN_DIR}/primary-selection-server-protocol.h ${WL_GEN_DIR}/primary-selection-server-protocol.c
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} server-header
          ${WAYLAND_PROTOCOLS_DIR}/unstable/primary-selection/primary-selection-unstable-v1.xml
          ${WL_GEN_DIR}/primary-selection-server-protocol.h
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
          ${WAYLAND_PROTOCOLS_DIR}/unstable/primary-selection/primary-selection-unstable-v1.xml
          ${WL_GEN_DIR}/primary-selection-server-protocol.c
  DEPENDS ${WAYLAND_PROTOCOLS_DIR}/unstable/primary-selection/primary-selection-unstable-v1.xml)

add_custom_command(
  OUTPUT ${WL_GEN_DIR}/relative-pointer-server-protocol.h ${WL_GEN_DIR}/relative-pointer-server-protocol.c
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} server-header
          ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml
          ${WL_GEN_DIR}/relative-pointer-server-protocol.h
  COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code
          ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml
          ${WL_GEN_DIR}/relative-pointer-server-protocol.c
  DEPENDS ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml)

# wlr virtual-pointer / zwp virtual-keyboard (vendored XMLs, not part of
# wayland-protocols): used only by test_virtual_input and the developer
# tools, generated on demand when one of them is built
set(SDLOP_WL_VIRTUAL_POINTER_XML ${CMAKE_SOURCE_DIR}/tests/protocols/wlr-virtual-pointer-unstable-v1.xml)
set(SDLOP_WL_VIRTUAL_KEYBOARD_XML ${CMAKE_SOURCE_DIR}/tests/protocols/virtual-keyboard-unstable-v1.xml)
if (EXISTS ${SDLOP_WL_VIRTUAL_POINTER_XML} AND EXISTS ${SDLOP_WL_VIRTUAL_KEYBOARD_XML})
  set(SDLOP_WL_VIRTUAL_INPUT_PROTOCOLS ON)
  add_custom_command(
    OUTPUT ${WL_GEN_DIR}/wlr-virtual-pointer-client-protocol.h ${WL_GEN_DIR}/wlr-virtual-pointer-protocol.c
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${SDLOP_WL_VIRTUAL_POINTER_XML}
            ${WL_GEN_DIR}/wlr-virtual-pointer-client-protocol.h
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${SDLOP_WL_VIRTUAL_POINTER_XML}
            ${WL_GEN_DIR}/wlr-virtual-pointer-protocol.c
    DEPENDS ${SDLOP_WL_VIRTUAL_POINTER_XML})
  add_custom_command(
    OUTPUT ${WL_GEN_DIR}/virtual-keyboard-client-protocol.h ${WL_GEN_DIR}/virtual-keyboard-protocol.c
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${SDLOP_WL_VIRTUAL_KEYBOARD_XML}
            ${WL_GEN_DIR}/virtual-keyboard-client-protocol.h
    COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${SDLOP_WL_VIRTUAL_KEYBOARD_XML}
            ${WL_GEN_DIR}/virtual-keyboard-protocol.c
    DEPENDS ${SDLOP_WL_VIRTUAL_KEYBOARD_XML})
else()
  set(SDLOP_WL_VIRTUAL_INPUT_PROTOCOLS OFF)
endif()

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
          ${WL_GEN_DIR}/relative-pointer-server-protocol.c
          ${WL_GEN_DIR}/primary-selection-server-protocol.h
          ${WL_GEN_DIR}/primary-selection-server-protocol.c
          ${SDLOP_WL_EXTRA_OUTPUTS})
endif()  # SDLOP_VIDEO_WAYLAND

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
endif()  # NOT WIN32: POSIX dependencies

set(SDLop_SOURCES
  src/SDLop_core.c
  src/SDL_stdlib.c
  src/SDL_error.c
  src/SDL_log.c
  src/SDL_timer.c
  src/SDL_events.c
  src/SDL_properties.c
  src/SDL_hints.c
  src/SDL_events_desc.c
  src/SDL_events_categories.c
  src/SDL_clipboard.c
  src/SDL_dropevents.c
  src/SDL_thread.c
  src/SDL_misc.c
  src/SDL_joystick.c
  src/SDL_haptic.c
  src/SDL_haptic_dummy.c
  src/SDL_filesystem.c
  src/SDL_main.c
  src/SDL_rect.c
  src/SDL_pixels.c
  src/SDL_blit.c
  src/SDL_blit_float.c
  src/SDL_rotate.c
  src/SDL_keyboard.c
  src/SDL_keynames.c
  src/SDL_mouse.c
  src/SDL_pen.c
  src/SDL_devices.c
  src/SDL_video.c
  src/SDL_dbus.c
  src/SDL_ime_dbus.c
  src/SDL_desktop.c
  src/internal/sdlop_platform.c
  src/SDL_dialog.c
  src/SDL_surface.c
  src/SDL_gl.c
  src/SDL_vulkan.c
  src/internal/sdlop_evdev.c
  src/sdlop_xkb_dyn.c
  src/video/sdlop_gl_offload.c
  src/video/sdlop_vk_offload.c
  src/video/sdlop_egl_dyn.c
  src/video/dummy/sdlop_dummy.c
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND SDLop_SOURCES src/SDL_joystick_evdev.c)
else()
  list(APPEND SDLop_SOURCES src/SDL_joystick_dummy_backend.c)
endif()

set(SDLop_VIDEO_DEFS)
if(NOT WIN32)
  if(SDLOP_VIDEO_WAYLAND)
    list(APPEND SDLop_SOURCES
      src/video/wayland/sdlop_wayland.c
      src/video/wayland/sdlop_wayland_clipboard.c
      src/video/wayland/sdlop_wayland_textinput.c
      src/video/wayland/sdlop_wayland_tablet.c
      src/video/wayland/sdlop_wayland_dyn.c
      src/video/wayland/sdlop_wayland_gl.c
      src/video/wayland/sdlop_wayland_vulkan.c
      src/video/wayland/sdlop_wayland_protos.c)
    list(APPEND SDLop_VIDEO_DEFS SDLop_VIDEO_WAYLAND=1)
  endif()
  if(SDLOP_VIDEO_KMSDRM)
    list(APPEND SDLop_SOURCES
      src/video/kmsdrm/sdlop_kmsdrm.c
      src/video/kmsdrm/sdlop_kmsdrm_atomic.c
      src/video/kmsdrm/sdlop_kmsdrm_dyn.c
      src/video/kmsdrm/sdlop_kmsdrm_gl.c
      src/video/kmsdrm/sdlop_kmsdrm_vulkan.c)
    list(APPEND SDLop_VIDEO_DEFS SDLop_VIDEO_KMSDRM=1)
  endif()
  if(SDLOP_VIDEO_WAYLAND OR SDLOP_VIDEO_KMSDRM OR SDLOP_VIDEO_FBDEV)
    list(APPEND SDLop_SOURCES src/video/sdlop_xcursor.c)
  endif()
  if(SDLOP_VIDEO_FBDEV)
    list(APPEND SDLop_SOURCES src/video/fbdev/sdlop_fbdev.c)
    list(APPEND SDLop_VIDEO_DEFS SDLop_VIDEO_FBDEV=1)
  endif()
  if(SDLOP_VIDEO_X11)
    list(APPEND SDLop_SOURCES
      src/video/x11/sdlop_x11.c
      src/video/x11/sdlop_x11_dyn.c
      src/video/x11/sdlop_x11_gl.c
      src/video/x11/sdlop_x11_clipboard.c
      src/video/x11/sdlop_x11_dnd.c
      src/video/x11/sdlop_x11_xsettings.c
      src/video/x11/sdlop_x11_pen.c
      src/video/x11/sdlop_x11_xim.c
      src/video/x11/sdlop_x11_vulkan.c)
    list(APPEND SDLop_VIDEO_DEFS SDLop_VIDEO_X11=1)
  endif()
endif()

if(WIN32)
  # The Win32 driver, the POSIX-name shims it needs, and the VK -> SDL3 keycode
  # table. Everything above is POSIX-only and is removed here.
  list(APPEND SDLop_SOURCES
    src/internal/sdlop_win32_compat.c
    src/internal/sdlop_rawinput_win32.c
    src/SDL_win32_keymap.c
    src/video/windows/sdlop_windows.c
    src/video/windows/sdlop_windows_gl.c
    src/video/windows/sdlop_windows_ime.c
    src/video/windows/sdlop_windows_vulkan.c
    src/video/sdlop_gl_offload_win32.c
    src/video/sdlop_vk_offload_win32.c
    src/internal/sdlop_dbus_stub_win32.c)
  list(REMOVE_ITEM SDLop_SOURCES
    src/internal/sdlop_evdev.c
    src/sdlop_xkb_dyn.c
    src/video/sdlop_egl_dyn.c
    src/video/sdlop_gl_offload.c
    src/video/sdlop_vk_offload.c
    src/SDL_dbus.c)
  set(SDLop_VIDEO_DEFS SDLop_VIDEO_WINDOWS=1)
endif()

# SDL3-style dynamic loading: wayland-client, xkbcommon and EGL are dlopen-ed
# at runtime, NOT linked - SDLop loads (and its other drivers work) on systems
# where those libraries are absent.
if(WIN32)
  # dlopen/pthread are emulated on Win32; these are the system libraries the
  # Win32 driver and the compat layer call into.
  set(SDLop_LIB_DEPS
    user32
    gdi32
    opengl32
    ws2_32
    shell32
    ole32
    uuid
    version
    imm32
    winmm
    dwmapi
    setupapi)
else()
  set(SDLop_LIB_DEPS
    Threads::Threads
    ${CMAKE_DL_LIBS}
    ${SDLop_EXTRA_POSIX_LIBS}
    m)
endif()
# dlopen-ed at run time - the core targets link none of these libraries.
# Only their header search paths are needed.
set(SDLop_LIB_INCS
  ${WAYLAND_CLIENT_INCLUDE_DIRS}
  ${XKBCOMMON_INCLUDE_DIRS}
  ${EGL_INCLUDE_DIRS}
  ${GBM_INCLUDE_DIRS}
  ${LIBDRM_INCLUDE_DIRS}
  ${X11_XCB_INCLUDE_DIRS})

add_library(sdlop STATIC ${SDLop_SOURCES})
add_library(SDLop::SDLop ALIAS sdlop)
if(NOT WIN32 AND SDLOP_VIDEO_WAYLAND)
  add_dependencies(sdlop sdlop_wayland_protocols)
endif()

target_include_directories(sdlop PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(sdlop PRIVATE
  src
  src/video/wayland
  src/video/kmsdrm
  ${WL_GEN_DIR}
  ${SDLop_LIB_INCS})
target_link_libraries(sdlop PUBLIC ${SDLop_LIB_DEPS})
target_compile_definitions(sdlop PUBLIC ${SDLop_VIDEO_DEFS})

add_library(sdlop_shared SHARED ${SDLop_SOURCES})
if(NOT WIN32 AND SDLOP_VIDEO_WAYLAND)
  add_dependencies(sdlop_shared sdlop_wayland_protocols)
endif()
set_target_properties(sdlop_shared PROPERTIES OUTPUT_NAME SDLop)
if(WIN32)
  # Make the DLL export list a property of the code, not of the linker.
  # Without this SDL_DECLSPEC is empty on Windows and the exports come from
  # auto-export, which not every linker pairs with tools/sdlop.sym: GNU ld
  # applies the version script to PE and lld ignores it silently, so the same
  # tree could export 318 internal SDLOP_* symbols or not depending on who
  # linked it. With DLL_EXPORT every exported function says so itself.
  target_compile_definitions(sdlop_shared PRIVATE DLL_EXPORT)
  # The two SDL_SetLinuxThreadPriority* functions exist in the Windows DLL as
  # "only available on Linux" stubs, so they need a dllexport declaration to
  # define against. This macro only adds those two declarations; it does not
  # make them visible to applications, which is why it is private here and is
  # not part of the installed/public header set.
  target_compile_definitions(sdlop_shared PRIVATE SDLOP_EXPORT_LINUX_API)
endif()
target_include_directories(sdlop_shared PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(sdlop_shared PRIVATE
  src
  src/video/wayland
  src/video/kmsdrm
  ${WL_GEN_DIR}
  ${SDLop_LIB_INCS})
target_link_libraries(sdlop_shared PUBLIC ${SDLop_LIB_DEPS})
target_compile_definitions(sdlop_shared PUBLIC ${SDLop_VIDEO_DEFS})

# Drop-in packaging for applications built against stock SDL3.
#  - SDLOP_SYMBOL_VERSIONING (default ON): export only SDL_*, tagged with
#    stock SDL3's version node SDL3_0.0.0 (tools/sdlop.sym). With it, a
#    libSDL3.so.0 -> libSDLop.so symlink loads without the dynamic loader's
#    "no version information available" warning.
#  - SDLOP_SDL3_SONAME (default OFF): build the shared library as
#    libSDL3.so.0.<minor>.<micro> with SONAME libSDL3.so.0, exactly like
#    stock SDL3, so it can replace it in a library path. Opt-in because it
#    conflicts with an installed SDL3.
option(SDLOP_SYMBOL_VERSIONING "Tag exports with SDL3's version node SDL3_0.0.0 and hide internals" ON)
option(SDLOP_SDL3_SONAME "Name the shared library libSDL3.so.0 (SONAME) like stock SDL3" OFF)
if (SDLOP_SYMBOL_VERSIONING AND NOT APPLE)
  target_link_options(sdlop_shared PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/tools/sdlop.sym")
  set_property(TARGET sdlop_shared APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/sdlop.sym)
endif()
if (SDLOP_SDL3_SONAME)
  file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/include/SDL3/SDL_version.h _sdl_minor REGEX "^#define SDL_MINOR_VERSION +[0-9]+")
  file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/include/SDL3/SDL_version.h _sdl_micro REGEX "^#define SDL_MICRO_VERSION +[0-9]+")
  string(REGEX REPLACE ".* ([0-9]+)$" "\\1" _sdl_minor "${_sdl_minor}")
  string(REGEX REPLACE ".* ([0-9]+)$" "\\1" _sdl_micro "${_sdl_micro}")
  set_target_properties(sdlop_shared PROPERTIES OUTPUT_NAME SDL3 SOVERSION 0 VERSION 0.${_sdl_minor}.${_sdl_micro})
  message(STATUS "SDLop shared library: libSDL3.so.0.${_sdl_minor}.${_sdl_micro} (SONAME libSDL3.so.0)")
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(SDLop_BUILD_EXAMPLES)
  # raw_latency installs a SIGINT handler, which Win32 does not have
  if(WIN32)
    set(SDLOP_EXAMPLES window surface_plasma)
  else()
    set(SDLOP_EXAMPLES window wasd raw_latency surface_plasma)
  endif()
  foreach(ex ${SDLOP_EXAMPLES})
    add_executable(${ex} examples/${ex}.c)
    target_link_libraries(${ex} PRIVATE sdlop m)
  endforeach()

  if(Vulkan_FOUND)
    add_executable(vulkan_clear examples/vulkan_clear.c)
    target_link_libraries(vulkan_clear PRIVATE sdlop Vulkan::Vulkan m)

    add_executable(vulkan_stress examples/vulkan_stress.c)
    target_link_libraries(vulkan_stress PRIVATE sdlop Vulkan::Vulkan m)

    # Same stress scene against a stock SDL3 build, for direct comparison.
    # The reference SDL3 must be built with Vulkan enabled; CI's headless
    # reference intentionally is not, so this target is not exercised there.
    if (SDL3_REFERENCE_INCLUDE AND SDL3_REFERENCE_LIB)
      add_executable(vulkan_stress_sdl3 examples/vulkan_stress.c)
      target_include_directories(vulkan_stress_sdl3 BEFORE PRIVATE ${SDL3_REFERENCE_INCLUDE})
      target_link_libraries(vulkan_stress_sdl3 PRIVATE ${SDL3_REFERENCE_LIB} Vulkan::Vulkan m)
      get_filename_component(_sdl3_ref_lib_dir ${SDL3_REFERENCE_LIB} DIRECTORY)
      set_target_properties(vulkan_stress_sdl3 PROPERTIES BUILD_RPATH ${_sdl3_ref_lib_dir})
    endif()
  else()
    message(STATUS "Vulkan not found - skipping Vulkan examples")
  endif()

  # GL example talks GLES2 directly
  if(GLESV2_FOUND)
    add_executable(gl_triangle examples/gl_triangle.c)
    target_link_libraries(gl_triangle PRIVATE sdlop ${GLESV2_LIBRARIES})
    target_include_directories(gl_triangle PRIVATE ${GLESV2_INCLUDE_DIRS})

    add_executable(gl_gpu_stress examples/gl_gpu_stress.c)
    target_link_libraries(gl_gpu_stress PRIVATE sdlop ${GLESV2_LIBRARIES})
    target_include_directories(gl_gpu_stress PRIVATE ${GLESV2_INCLUDE_DIRS})
  else()
    message(STATUS "GLESv2 not found - skipping GL examples")
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SDLop_BUILD_TESTS)
  enable_testing()
  # Portable tests build and pass on every platform SDLop targets. The rest
  # drive POSIX-only backends (evdev, fbdev, KMS, X11, Wayland, D-Bus) and are
  # registered further down.
  if(WIN32)
    set(SDLOP_PORTABLE_TESTS test_ring test_events test_layouts
        test_core test_main_callbacks test_threads test_iconv)
  else()
    set(SDLOP_PORTABLE_TESTS test_ring test_events test_surface
        test_web test_keycodes test_layouts test_core test_main_callbacks
        test_misc test_threads test_vt_mask test_options
        test_dropin_compat test_iconv test_dbus test_ime_dbus test_ime_txn)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      list(APPEND SDLOP_PORTABLE_TESTS test_scancode test_raw_translate)
    endif()
    if(SDLOP_VIDEO_FBDEV)
      list(APPEND SDLOP_PORTABLE_TESTS test_fbdev_device)
    endif()
  endif()
  foreach(t ${SDLOP_PORTABLE_TESTS})
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE sdlop)
    target_include_directories(${t} PRIVATE src)
    target_compile_options(${t} PRIVATE -UNDEBUG)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
  set_tests_properties(test_core PROPERTIES RUN_SERIAL TRUE)
  if(NOT WIN32 AND SDLOP_VIDEO_FBDEV)
    target_link_options(test_fbdev_device PRIVATE -Wl,--wrap=ioctl)
  endif()

  if(WIN32)
    # The Win32 driver, WGL, Vulkan and the VK -> SDL3 keycode table.
    foreach(t test_win32_keycodes test_win32_scancodes test_win32_icc
              test_win32_raw_keyboard test_win32_raw_input
              test_win32_raw_input_events test_win32_raw_coexist
              test_win32_raw_keyboard_once)
      add_executable(${t} tests/${t}.c)
      target_link_libraries(${t} PRIVATE sdlop)
      target_include_directories(${t} PRIVATE src)
      target_compile_options(${t} PRIVATE -UNDEBUG)
      add_test(NAME ${t} COMMAND ${t})
    endforeach()
    set_tests_properties(test_win32_scancodes PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    # These two inject keyboard input through SendInput and assert exactly one
    # delivery: run beside a test that steals the foreground window and the
    # injected keys go to the other window, so every event count reads 0.
    set_tests_properties(test_win32_raw_keyboard_once test_win32_raw_input_events
                         PROPERTIES RUN_SERIAL TRUE)
    # These inject input or drive global OS state (clipboard, foreground
    # window, system-wide button mask), so they must not run beside anything.
    foreach(t test_win32_cursor test_win32_hittest test_win32_shape
              test_win32_close test_win32_windowsize test_win32_windowmgmt
              test_win32_fileops
              test_win32_raw_keyboard_e2e test_win32_raw_mouse_e2e
              test_win32_gl test_win32_vulkan test_win32_clipboard
              test_win32_ime test_win32_attention)
      add_executable(${t} tests/${t}.c)
      target_link_libraries(${t} PRIVATE sdlop)
      target_include_directories(${t} PRIVATE src)
      target_compile_options(${t} PRIVATE -UNDEBUG)
      add_test(NAME ${t} COMMAND ${t})
      set_tests_properties(${t} PROPERTIES RUN_SERIAL TRUE)
    endforeach()
    target_link_libraries(test_win32_shape PRIVATE gdi32)
    target_link_libraries(test_win32_gl PRIVATE opengl32)
    target_link_libraries(test_win32_ime PRIVATE imm32)
    set_tests_properties(test_win32_gl test_win32_vulkan PROPERTIES SKIP_RETURN_CODE 77)
  endif()

  if(NOT WIN32)  # POSIX-only tests: dummy+xcb/wayland/egl integrations
    # SDL 3.4 window/cursor/grab/device API on the dummy driver (and on X11
    # below, verified from a second xcb connection)
    add_executable(test_window_api tests/test_window_api.c)
    target_link_libraries(test_window_api PRIVATE sdlop)
    target_compile_options(test_window_api PRIVATE -UNDEBUG)
    add_test(NAME test_window_api COMMAND test_window_api dummy)
    if(SDLOP_VIDEO_WAYLAND)
      # Wayland run: needs a compositor (WAYLAND_DISPLAY), skips otherwise
      add_test(NAME test_window_api_wayland COMMAND test_window_api wayland)
    endif()
    if((SDLOP_VIDEO_WAYLAND OR SDLOP_VIDEO_X11) AND SDLOP_HAVE_EGL)
      # SDL_EGL_* / SDL_GL_ExtensionSupported against Mesa (skips without a
      # display server; the X11 run is registered with the X11 tests)
      add_executable(test_egl_api tests/test_egl_api.c)
      target_link_libraries(test_egl_api PRIVATE sdlop)
      target_compile_options(test_egl_api PRIVATE -UNDEBUG)
      if(SDLOP_VIDEO_WAYLAND)
        add_test(NAME test_egl_api_wayland COMMAND test_egl_api wayland)
      endif()
    endif()
    # the generated headers must be valid C++ with C linkage
    include(CheckLanguage)
    check_language(CXX)
    if(CMAKE_CXX_COMPILER)
      enable_language(CXX)
      add_executable(test_cxx_link tests/test_cxx_link.cpp)
      target_link_libraries(test_cxx_link PRIVATE sdlop)
      set_target_properties(test_cxx_link PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
      target_compile_options(test_cxx_link PRIVATE -Wall -Wextra -pedantic -Werror -UNDEBUG)
      add_test(NAME test_cxx_link COMMAND test_cxx_link)
    endif()
    # same keycode checks through the built-in US table (no libxkbcommon)
    add_test(NAME test_keycodes_noxkb COMMAND test_keycodes)
    set_tests_properties(test_keycodes_noxkb PROPERTIES ENVIRONMENT "SDLOP_LIB_XKB=none")

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # integration tests that need real subsystems (auto-skip when unavailable)
      add_executable(test_evdev_uinput tests/test_evdev_uinput.c tests/uinput_synth.c)
      target_link_libraries(test_evdev_uinput PRIVATE sdlop)
      target_include_directories(test_evdev_uinput PRIVATE src tests)
      target_compile_options(test_evdev_uinput PRIVATE -UNDEBUG)
      add_test(NAME test_evdev_uinput COMMAND test_evdev_uinput)

      # End-to-end evdev joystick/gamepad backend through uinput.
      add_executable(test_evdev_joystick tests/test_evdev_joystick.c tests/uinput_synth.c)
      target_link_libraries(test_evdev_joystick PRIVATE sdlop)
      target_include_directories(test_evdev_joystick PRIVATE src tests)
      target_compile_options(test_evdev_joystick PRIVATE -UNDEBUG)
      add_test(NAME test_evdev_joystick COMMAND test_evdev_joystick)
      set_tests_properties(test_evdev_joystick PROPERTIES TIMEOUT 20)
    endif()

    if(SDLOP_VIDEO_WAYLAND)
      add_executable(test_wayland tests/test_wayland.c)
      target_link_libraries(test_wayland PRIVATE sdlop)
      target_include_directories(test_wayland PRIVATE src)
      target_compile_options(test_wayland PRIVATE -UNDEBUG)
      add_test(NAME test_wayland COMMAND test_wayland)
    endif()

    # X11 driver integration test (needs a running X server, e.g. xvfb-run;
    # skips without DISPLAY). Only the *test* links xcb/xcb-xtest - it drives
    # input through XTest and verifies state from its own connection.
    if(SDLOP_VIDEO_X11)
      pkg_check_modules(X11_TEST_DEPS xcb xcb-xtest)
      if(X11_TEST_DEPS_FOUND)
        add_executable(test_x11 tests/test_x11.c)
        target_link_libraries(test_x11 PRIVATE sdlop ${X11_TEST_DEPS_LIBRARIES})
        target_include_directories(test_x11 PRIVATE ${X11_TEST_DEPS_INCLUDE_DIRS})
        target_compile_options(test_x11 PRIVATE -UNDEBUG)
        add_test(NAME test_x11 COMMAND test_x11)
        target_compile_definitions(test_window_api PRIVATE SDLOP_TEST_HAVE_XCB=1)
        target_link_libraries(test_window_api PRIVATE ${X11_TEST_DEPS_LIBRARIES})
        target_include_directories(test_window_api PRIVATE ${X11_TEST_DEPS_INCLUDE_DIRS})
        add_test(NAME test_window_api_x11 COMMAND test_window_api x11)
        set_tests_properties(test_window_api_x11 PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        if(SDLOP_HAVE_EGL)
          add_test(NAME test_egl_api_x11 COMMAND test_egl_api x11)
        endif()
        # same suite through the xcb_put_image fallback (no MIT-SHM)
        add_test(NAME test_x11_noshm COMMAND test_x11)
        # and with only libxcb itself: no SHM / XKB / XInput2 / RandR, i.e.
        # put_image, core keymap + timestamp repeat detection, warp-based
        # relative mode and a single root-window display
        add_test(NAME test_x11_minimal COMMAND test_x11)
        set_tests_properties(test_x11 test_x11_noshm test_x11_minimal PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        # system clipboard / primary selection against other X clients (raw
        # xcb requestor + owner thread), with and without XFixes
        add_executable(test_x11_clipboard tests/test_x11_clipboard.c)
        target_link_libraries(test_x11_clipboard PRIVATE sdlop ${X11_TEST_DEPS_LIBRARIES} Threads::Threads)
        target_include_directories(test_x11_clipboard PRIVATE ${X11_TEST_DEPS_INCLUDE_DIRS})
        target_compile_options(test_x11_clipboard PRIVATE -UNDEBUG)
        add_test(NAME test_x11_clipboard COMMAND test_x11_clipboard)
        add_test(NAME test_x11_clipboard_noxfixes COMMAND test_x11_clipboard)
        set_tests_properties(test_x11_clipboard test_x11_clipboard_noxfixes PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        set_tests_properties(test_x11_clipboard_noxfixes PROPERTIES ENVIRONMENT "SDLOP_LIB_XCB_XFIXES=none")
        # SDL_SetWindowHitTest / SDL_ShowWindowSystemMenu: EWMH messages to the root
        add_executable(test_x11_hittest tests/test_x11_hittest.c)
        target_link_libraries(test_x11_hittest PRIVATE sdlop ${X11_TEST_DEPS_LIBRARIES})
        target_include_directories(test_x11_hittest PRIVATE ${X11_TEST_DEPS_INCLUDE_DIRS})
        target_compile_options(test_x11_hittest PRIVATE -UNDEBUG)
        add_test(NAME test_x11_hittest COMMAND test_x11_hittest)
        set_tests_properties(test_x11_hittest PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        # SDL_WINDOW_TRANSPARENT: ARGB visual, surface alpha read back
        add_executable(test_x11_transparent tests/test_x11_transparent.c)
        target_link_libraries(test_x11_transparent PRIVATE sdlop ${X11_TEST_DEPS_LIBRARIES})
        target_include_directories(test_x11_transparent PRIVATE ${X11_TEST_DEPS_INCLUDE_DIRS})
        target_compile_options(test_x11_transparent PRIVATE -UNDEBUG)
        add_test(NAME test_x11_transparent COMMAND test_x11_transparent)
        add_test(NAME test_x11_transparent_noshm COMMAND test_x11_transparent)
        set_tests_properties(test_x11_transparent test_x11_transparent_noshm PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        set_tests_properties(test_x11_transparent_noshm PROPERTIES ENVIRONMENT "SDLOP_X11_NO_SHM=1")
        # SDL_SetWindowShape: XShape input shapes (the test itself needs xcb-shape)
        pkg_check_modules(X11_SHAPE_TEST_DEPS xcb xcb-shape)
        if(X11_SHAPE_TEST_DEPS_FOUND)
          add_executable(test_x11_shape tests/test_x11_shape.c)
          target_link_libraries(test_x11_shape PRIVATE sdlop ${X11_SHAPE_TEST_DEPS_LIBRARIES})
          target_include_directories(test_x11_shape PRIVATE ${X11_SHAPE_TEST_DEPS_INCLUDE_DIRS})
          target_compile_options(test_x11_shape PRIVATE -UNDEBUG)
          add_test(NAME test_x11_shape COMMAND test_x11_shape)
          add_test(NAME test_x11_shape_nomodule COMMAND test_x11_shape)
          set_tests_properties(test_x11_shape test_x11_shape_nomodule PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
          set_tests_properties(test_x11_shape_nomodule PROPERTIES ENVIRONMENT "SDLOP_LIB_XCB_SHAPE=none")
        else()
          message(STATUS "xcb-shape not found: test_x11_shape not built")
        endif()
        # SDL_GetWindowICCProfile / SDL_EVENT_WINDOW_ICCPROF_CHANGED (_ICC_PROFILE)
        add_executable(test_x11_icc tests/test_x11_icc.c)
        target_link_libraries(test_x11_icc PRIVATE sdlop ${X11_TEST_DEPS_LIBRARIES})
        target_include_directories(test_x11_icc PRIVATE ${X11_TEST_DEPS_INCLUDE_DIRS})
        target_compile_options(test_x11_icc PRIVATE -UNDEBUG)
        add_test(NAME test_x11_icc COMMAND test_x11_icc)
        set_tests_properties(test_x11_icc PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        # SDL_SetWindowMouseRect via XFixes pointer barriers (+ warp-back fallback)
        pkg_check_modules(X11_XFIXES_TEST xcb-xfixes)
        if(X11_XFIXES_TEST_FOUND)
          add_executable(test_x11_barriers tests/test_x11_barriers.c)
          target_link_libraries(test_x11_barriers PRIVATE sdlop ${X11_TEST_DEPS_LIBRARIES} ${X11_XFIXES_TEST_LIBRARIES})
          target_include_directories(test_x11_barriers PRIVATE ${X11_TEST_DEPS_INCLUDE_DIRS} ${X11_XFIXES_TEST_INCLUDE_DIRS})
          target_compile_options(test_x11_barriers PRIVATE -UNDEBUG)
          add_test(NAME test_x11_barriers COMMAND test_x11_barriers)
          add_test(NAME test_x11_barriers_nomodule COMMAND test_x11_barriers)
          set_tests_properties(test_x11_barriers test_x11_barriers_nomodule PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
          set_tests_properties(test_x11_barriers_nomodule PROPERTIES ENVIRONMENT "SDLOP_LIB_XCB_XFIXES=none")
        endif()
        # exclusive fullscreen modes via XRandR (adds a user mode to Xvfb)
        pkg_check_modules(X11_RANDR_TEST xcb-randr)
        if(X11_RANDR_TEST_FOUND)
          add_executable(test_x11_modes tests/test_x11_modes.c)
          target_link_libraries(test_x11_modes PRIVATE sdlop ${X11_TEST_DEPS_LIBRARIES} ${X11_RANDR_TEST_LIBRARIES})
          target_include_directories(test_x11_modes PRIVATE ${X11_TEST_DEPS_INCLUDE_DIRS} ${X11_RANDR_TEST_INCLUDE_DIRS})
          target_compile_options(test_x11_modes PRIVATE -UNDEBUG)
          add_test(NAME test_x11_modes COMMAND test_x11_modes)
          set_tests_properties(test_x11_modes PROPERTIES RUN_SERIAL TRUE TIMEOUT 90)
        endif()
        # XSETTINGS content scale against a raw xcb settings manager
        add_executable(test_x11_xsettings tests/test_x11_xsettings.c)
        target_link_libraries(test_x11_xsettings PRIVATE sdlop m ${X11_TEST_DEPS_LIBRARIES})
        target_include_directories(test_x11_xsettings PRIVATE ${X11_TEST_DEPS_INCLUDE_DIRS})
        target_compile_options(test_x11_xsettings PRIVATE -UNDEBUG)
        add_test(NAME test_x11_xsettings COMMAND test_x11_xsettings)
        add_test(NAME test_x11_xsettings_off COMMAND test_x11_xsettings)
        set_tests_properties(test_x11_xsettings test_x11_xsettings_off PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        set_tests_properties(test_x11_xsettings_off PROPERTIES ENVIRONMENT "SDLOP_X11_NO_XSETTINGS=1")
        # _NET_WM_SYNC_REQUEST resize sync (the test itself needs xcb-sync)
        pkg_check_modules(X11_SYNC_TEST_DEPS xcb xcb-sync)
        if(X11_SYNC_TEST_DEPS_FOUND)
          add_executable(test_x11_sync tests/test_x11_sync.c)
          target_link_libraries(test_x11_sync PRIVATE sdlop ${X11_SYNC_TEST_DEPS_LIBRARIES})
          target_include_directories(test_x11_sync PRIVATE ${X11_SYNC_TEST_DEPS_INCLUDE_DIRS})
          target_compile_options(test_x11_sync PRIVATE -UNDEBUG)
          add_test(NAME test_x11_sync COMMAND test_x11_sync)
          add_test(NAME test_x11_sync_nomodule COMMAND test_x11_sync)
          set_tests_properties(test_x11_sync test_x11_sync_nomodule PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
          set_tests_properties(test_x11_sync_nomodule PROPERTIES ENVIRONMENT "SDLOP_LIB_XCB_SYNC=none")
        else()
          message(STATUS "xcb-sync not found: test_x11_sync not built")
        endif()
        # XDND drag-and-drop target against a raw xcb drag source
        add_executable(test_x11_dnd tests/test_x11_dnd.c)
        target_link_libraries(test_x11_dnd PRIVATE sdlop ${X11_TEST_DEPS_LIBRARIES})
        target_include_directories(test_x11_dnd PRIVATE ${X11_TEST_DEPS_INCLUDE_DIRS})
        target_compile_options(test_x11_dnd PRIVATE -UNDEBUG)
        add_test(NAME test_x11_dnd COMMAND test_x11_dnd)
        set_tests_properties(test_x11_dnd PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        # XIM input method against a fake XIM server (xcb-imdkit, test only)
        pkg_check_modules(XIM_TEST_DEPS xcb-imdkit)
        if(XIM_TEST_DEPS_FOUND)
          add_executable(test_x11_xim tests/test_x11_xim.c)
          target_link_libraries(test_x11_xim PRIVATE sdlop ${X11_TEST_DEPS_LIBRARIES} ${XIM_TEST_DEPS_LIBRARIES})
          target_include_directories(test_x11_xim PRIVATE ${X11_TEST_DEPS_INCLUDE_DIRS} ${XIM_TEST_DEPS_INCLUDE_DIRS})
          target_compile_options(test_x11_xim PRIVATE -UNDEBUG)
          add_test(NAME test_x11_xim COMMAND test_x11_xim root)
          add_test(NAME test_x11_xim_composition COMMAND test_x11_xim composition)
          set_tests_properties(test_x11_xim test_x11_xim_composition PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        else()
          message(STATUS "xcb-imdkit not found: test_x11_xim not built")
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
          # SDLOP_RAW_INPUT on X11: optional, never mixed (fake /dev/input in-process)
          add_executable(test_x11_raw_source tests/test_x11_raw_source.c)
          target_link_libraries(test_x11_raw_source PRIVATE sdlop ${X11_TEST_DEPS_LIBRARIES} ${CMAKE_DL_LIBS})
          target_include_directories(test_x11_raw_source PRIVATE ${X11_TEST_DEPS_INCLUDE_DIRS})
          target_compile_options(test_x11_raw_source PRIVATE -UNDEBUG)
          add_test(NAME test_x11_raw_source COMMAND test_x11_raw_source)
          set_tests_properties(test_x11_raw_source PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        endif()
        # desktop GL on X11 is GLX (GLEW & co. need glXGetCurrentDisplay);
        # GLES / SDL_VIDEO_FORCE_EGL use EGL. libGL is dlopen-ed at run time.
        add_executable(test_glx tests/test_glx.c)
        target_link_libraries(test_glx PRIVATE sdlop ${CMAKE_DL_LIBS})
        target_compile_options(test_glx PRIVATE -UNDEBUG)
        add_test(NAME test_glx COMMAND test_glx)
        set_tests_properties(test_glx PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        set_tests_properties(test_x11_noshm PROPERTIES ENVIRONMENT "SDLOP_X11_NO_SHM=1")
        set_tests_properties(test_x11_minimal PROPERTIES ENVIRONMENT
          "SDLOP_LIB_XCB_SHM=none;SDLOP_LIB_XCB_XKB=none;SDLOP_LIB_XKBCOMMON_X11=none;SDLOP_LIB_XCB_XINPUT=none;SDLOP_LIB_XCB_RANDR=none;SDLOP_LIB_XCB_RENDER=none;SDLOP_LIB_XCB_XFIXES=none")
      endif()
      # keyboard layouts end-to-end (setxkbmap + XKB group locking; the test
      # restores the server layout). Skips where the keymap cannot be set.
      pkg_check_modules(X11_LAYOUT_DEPS xcb xcb-xtest xcb-xkb)
      if(X11_LAYOUT_DEPS_FOUND)
        add_executable(test_x11_layout tests/test_x11_layout.c)
        target_link_libraries(test_x11_layout PRIVATE sdlop ${X11_LAYOUT_DEPS_LIBRARIES})
        target_include_directories(test_x11_layout PRIVATE ${X11_LAYOUT_DEPS_INCLUDE_DIRS})
        target_compile_options(test_x11_layout PRIVATE -UNDEBUG)
        add_test(NAME test_x11_layout COMMAND test_x11_layout)
        set_tests_properties(test_x11_layout PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
      endif()
      # XI2.2 multitouch: an in-test X protocol proxy injects touch events
      # (Xvfb has no touch devices)
      pkg_check_modules(X11_TOUCH_DEPS xcb xcb-xinput)
      if(X11_TOUCH_DEPS_FOUND)
        add_executable(test_x11_touch tests/test_x11_touch.c)
        target_link_libraries(test_x11_touch PRIVATE sdlop ${X11_TOUCH_DEPS_LIBRARIES} Threads::Threads m)
        target_include_directories(test_x11_touch PRIVATE ${X11_TOUCH_DEPS_INCLUDE_DIRS})
        target_compile_options(test_x11_touch PRIVATE -UNDEBUG)
        add_test(NAME test_x11_touch COMMAND test_x11_touch)
        set_tests_properties(test_x11_touch PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        # XI2 pens: the same kind of proxy adds a fake tablet to XIQueryDevice
        add_executable(test_x11_pen tests/test_x11_pen.c)
        target_link_libraries(test_x11_pen PRIVATE sdlop ${X11_TOUCH_DEPS_LIBRARIES} Threads::Threads m)
        target_include_directories(test_x11_pen PRIVATE ${X11_TOUCH_DEPS_INCLUDE_DIRS})
        target_compile_options(test_x11_pen PRIVATE -UNDEBUG)
        add_test(NAME test_x11_pen COMMAND test_x11_pen on)
        add_test(NAME test_x11_pen_off COMMAND test_x11_pen off)
        set_tests_properties(test_x11_pen test_x11_pen_off PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
      endif()
    endif()

    if(SDLOP_VIDEO_FBDEV)
      # framebuffer (fbdev) driver: needs no device - SDLOP_FBDEV_DUMP makes the
      # driver render into RAM and dump each presented frame as a PPM we check
      add_executable(test_fbdev tests/test_fbdev.c)
      target_link_libraries(test_fbdev PRIVATE sdlop)
      target_include_directories(test_fbdev PRIVATE src)
      target_compile_options(test_fbdev PRIVATE -UNDEBUG)
      add_test(NAME test_fbdev COMMAND test_fbdev)
      set_tests_properties(test_fbdev PROPERTIES
        ENVIRONMENT "SDLOP_DISABLE_RAW_INPUT=1;SDLOP_TEST_STRICT=1;SDLOP_FBDEV_SIZE=320x240;SDLOP_FBDEV_DUMP=${CMAKE_CURRENT_BINARY_DIR}/test_fbdev_frame.ppm"
        TIMEOUT 60)
    endif()

    if(SDLOP_VIDEO_KMSDRM)
      add_executable(test_kmsdrm tests/test_kmsdrm.c)
      target_link_libraries(test_kmsdrm PRIVATE sdlop)
      target_include_directories(test_kmsdrm PRIVATE src)
      target_compile_options(test_kmsdrm PRIVATE -UNDEBUG)
      add_test(NAME test_kmsdrm COMMAND test_kmsdrm)
    endif()

    add_executable(test_gl_offload tests/test_gl_offload.c)
    target_link_libraries(test_gl_offload PRIVATE sdlop)
    target_include_directories(test_gl_offload PRIVATE src)
    target_compile_options(test_gl_offload PRIVATE -UNDEBUG)
    add_test(NAME test_gl_offload COMMAND test_gl_offload)

    add_executable(test_vk_offload tests/test_vk_offload.c)
    target_link_libraries(test_vk_offload PRIVATE sdlop)
    target_include_directories(test_vk_offload PRIVATE src)
    target_compile_options(test_vk_offload PRIVATE -UNDEBUG)
    add_test(NAME test_vk_offload COMMAND test_vk_offload)

    if(SDLOP_VIDEO_KMSDRM)
      # KMS present-path benchmark (not a ctest gate; skips without KMS)
      add_executable(bench_kmsdrm tests/bench_kmsdrm.c)
      target_link_libraries(bench_kmsdrm PRIVATE sdlop)
      target_include_directories(bench_kmsdrm PRIVATE src)
      target_compile_options(bench_kmsdrm PRIVATE -UNDEBUG)
    endif()

    if(GLESV2_FOUND)
      add_executable(test_gl tests/test_gl.c)
      target_link_libraries(test_gl PRIVATE sdlop ${GLESV2_LIBRARIES})
      target_include_directories(test_gl PRIVATE src ${GLESV2_INCLUDE_DIRS})
      target_compile_options(test_gl PRIVATE -UNDEBUG)
      add_test(NAME test_gl COMMAND test_gl)
    else()
      message(STATUS "GLESv2 not found - skipping test_gl")
    endif()

    # pointer lock integration test (runs against tests/mini_compositor.c)
    if(SDLOP_VIDEO_WAYLAND AND WAYLAND_SERVER_FOUND)
      add_executable(mini_compositor tests/mini_compositor.c
        ${WL_GEN_DIR}/xdg-shell-server-protocol.c
        ${WL_GEN_DIR}/pointer-constraints-server-protocol.c
        ${WL_GEN_DIR}/relative-pointer-server-protocol.c
        ${WL_GEN_DIR}/viewporter-server-protocol.c
        ${WL_GEN_DIR}/primary-selection-server-protocol.c
        ${WL_GEN_DIR}/fractional-scale-v1-server-protocol.c
        ${WL_GEN_DIR}/text-input-unstable-v3-server-protocol.c
        ${WL_GEN_DIR}/tablet-v2-server-protocol.c
        ${WL_GEN_DIR}/cursor-shape-v1-server-protocol.c
        ${WL_GEN_DIR}/xdg-toplevel-icon-v1-server-protocol.c
        ${WL_GEN_DIR}/xdg-dialog-v1-server-protocol.c
        ${WL_GEN_DIR}/alpha-modifier-v1-server-protocol.c
        ${WL_GEN_DIR}/pointer-warp-v1-server-protocol.c
        ${WL_GEN_DIR}/pointer-gestures-unstable-v1-server-protocol.c
        ${WL_GEN_DIR}/xdg-foreign-unstable-v2-server-protocol.c)
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

      # wl_buffer release contract of software presentation
      add_executable(test_wayland_buffers tests/test_wayland_buffers.c)
      target_include_directories(test_wayland_buffers PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS})
      target_link_libraries(test_wayland_buffers PRIVATE sdlop ${WAYLAND_CLIENT_LIBRARIES})
      target_compile_options(test_wayland_buffers PRIVATE -UNDEBUG)
      add_dependencies(test_wayland_buffers mini_compositor)
      add_test(NAME test_wayland_buffers COMMAND test_wayland_buffers $<TARGET_FILE:mini_compositor>)
      add_test(NAME test_wayland_buffers_viewporter COMMAND test_wayland_buffers $<TARGET_FILE:mini_compositor> viewporter)

      # interactive resizes: configure throttling, lazy window surface resize
      add_executable(test_wayland_resize tests/test_wayland_resize.c)
      target_link_libraries(test_wayland_resize PRIVATE sdlop)
      target_compile_options(test_wayland_resize PRIVATE -UNDEBUG)
      add_dependencies(test_wayland_resize mini_compositor)
      add_test(NAME test_wayland_resize COMMAND test_wayland_resize $<TARGET_FILE:mini_compositor>)

      # clipboard + primary selection between two SDL processes
      add_executable(test_wayland_clipboard tests/test_wayland_clipboard.c)
      target_link_libraries(test_wayland_clipboard PRIVATE sdlop)
      target_compile_options(test_wayland_clipboard PRIVATE -UNDEBUG)
      add_dependencies(test_wayland_clipboard mini_compositor)
      add_test(NAME test_wayland_clipboard COMMAND test_wayland_clipboard $<TARGET_FILE:mini_compositor>)
      set_tests_properties(test_wayland_clipboard PROPERTIES TIMEOUT 60)

      # drag-and-drop target (synthetic drags from the mini compositor)
      add_executable(test_wayland_dnd tests/test_wayland_dnd.c)
      target_link_libraries(test_wayland_dnd PRIVATE sdlop)
      target_compile_options(test_wayland_dnd PRIVATE -UNDEBUG)
      add_dependencies(test_wayland_dnd mini_compositor)
      # HiDPI: fractional / preferred buffer / output scales
      add_executable(test_wayland_cursor tests/test_wayland_cursor.c)
      target_link_libraries(test_wayland_cursor PRIVATE sdlop)
      target_compile_options(test_wayland_cursor PRIVATE -UNDEBUG)
      add_dependencies(test_wayland_cursor mini_compositor)
      foreach(cursor_mode shape noshape theme)
        add_test(NAME test_wayland_cursor_${cursor_mode}
                 COMMAND test_wayland_cursor $<TARGET_FILE:mini_compositor> ${cursor_mode})
        set_tests_properties(test_wayland_cursor_${cursor_mode} PROPERTIES TIMEOUT 60)
      endforeach()

      add_executable(test_wayland_toplevel tests/test_wayland_toplevel.c)
      target_link_libraries(test_wayland_toplevel PRIVATE sdlop)
      target_compile_options(test_wayland_toplevel PRIVATE -UNDEBUG)
      add_dependencies(test_wayland_toplevel mini_compositor)
      foreach(toplevel_mode all none)
        add_test(NAME test_wayland_toplevel_${toplevel_mode}
                 COMMAND test_wayland_toplevel $<TARGET_FILE:mini_compositor> ${toplevel_mode})
        set_tests_properties(test_wayland_toplevel_${toplevel_mode} PROPERTIES TIMEOUT 60)
      endforeach()

      add_executable(test_wayland_pen tests/test_wayland_pen.c)
      target_link_libraries(test_wayland_pen PRIVATE sdlop m)
      target_compile_options(test_wayland_pen PRIVATE -UNDEBUG)
      add_dependencies(test_wayland_pen mini_compositor)
      foreach(pen_mode on off)
        add_test(NAME test_wayland_pen_${pen_mode} COMMAND test_wayland_pen $<TARGET_FILE:mini_compositor> ${pen_mode})
        set_tests_properties(test_wayland_pen_${pen_mode} PROPERTIES TIMEOUT 60)
      endforeach()

      add_executable(test_wayland_wheel tests/test_wayland_wheel.c)
      target_link_libraries(test_wayland_wheel PRIVATE sdlop m)
      target_compile_options(test_wayland_wheel PRIVATE -UNDEBUG)
      add_dependencies(test_wayland_wheel mini_compositor)
      foreach(wheel_mode v9 v7 v4)
        add_test(NAME test_wayland_wheel_${wheel_mode}
                 COMMAND test_wayland_wheel $<TARGET_FILE:mini_compositor> ${wheel_mode})
        set_tests_properties(test_wayland_wheel_${wheel_mode} PROPERTIES TIMEOUT 60)
      endforeach()
      add_executable(test_wayland_pinch tests/test_wayland_pinch.c)
      target_link_libraries(test_wayland_pinch PRIVATE sdlop m)
      target_compile_options(test_wayland_pinch PRIVATE -UNDEBUG)
      add_dependencies(test_wayland_pinch mini_compositor)
      foreach(pinch_mode v3 v1 off)
        add_test(NAME test_wayland_pinch_${pinch_mode}
                 COMMAND test_wayland_pinch $<TARGET_FILE:mini_compositor> ${pinch_mode})
        set_tests_properties(test_wayland_pinch_${pinch_mode} PROPERTIES TIMEOUT 60)
      endforeach()

      add_executable(test_wayland_warp tests/test_wayland_warp.c)
      target_link_libraries(test_wayland_warp PRIVATE sdlop)
      target_compile_options(test_wayland_warp PRIVATE -UNDEBUG)
      add_dependencies(test_wayland_warp mini_compositor)
      foreach(warp_mode warp hint)
        add_test(NAME test_wayland_warp_${warp_mode}
                 COMMAND test_wayland_warp $<TARGET_FILE:mini_compositor> ${warp_mode})
        set_tests_properties(test_wayland_warp_${warp_mode} PROPERTIES TIMEOUT 60)
      endforeach()

      add_executable(test_wayland_textinput tests/test_wayland_textinput.c)
      target_link_libraries(test_wayland_textinput PRIVATE sdlop)
      target_compile_options(test_wayland_textinput PRIVATE -UNDEBUG)
      add_dependencies(test_wayland_textinput mini_compositor)
      add_test(NAME test_wayland_textinput COMMAND test_wayland_textinput $<TARGET_FILE:mini_compositor>)
      set_tests_properties(test_wayland_textinput PROPERTIES TIMEOUT 60)

      add_executable(test_wayland_hidpi tests/test_wayland_hidpi.c)
      target_link_libraries(test_wayland_hidpi PRIVATE sdlop)
      target_compile_options(test_wayland_hidpi PRIVATE -UNDEBUG)
      add_dependencies(test_wayland_hidpi mini_compositor)
      foreach(hidpi_mode fractional lowdpi buffer output noviewport)
        add_test(NAME test_wayland_hidpi_${hidpi_mode}
                 COMMAND test_wayland_hidpi $<TARGET_FILE:mini_compositor> ${hidpi_mode})
        set_tests_properties(test_wayland_hidpi_${hidpi_mode} PROPERTIES TIMEOUT 60)
      endforeach()

      foreach(dnd_mode files text bigtext refuse disabled)
        add_test(NAME test_wayland_dnd_${dnd_mode} COMMAND test_wayland_dnd $<TARGET_FILE:mini_compositor> ${dnd_mode})
        set_tests_properties(test_wayland_dnd_${dnd_mode} PROPERTIES TIMEOUT 60)
      endforeach()
    else()
      message(STATUS "wayland-server not found - skipping mini_compositor, test_pointer_lock, test_touch, test_wayland_buffers")
    endif()

    add_executable(smoke_relative_mode tests/smoke_relative_mode.c)
    target_link_libraries(smoke_relative_mode PRIVATE sdlop)
    add_test(NAME smoke_relative_mode COMMAND smoke_relative_mode)

    if(GLESV2_FOUND)
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

      add_executable(test_vk_offload_live tests/test_vk_offload_live.c)
      target_link_libraries(test_vk_offload_live PRIVATE sdlop Vulkan::Vulkan)
      target_include_directories(test_vk_offload_live PRIVATE src)
      target_compile_options(test_vk_offload_live PRIVATE -UNDEBUG)
      add_test(NAME test_vk_offload_live COMMAND test_vk_offload_live)
    else()
      message(STATUS "Vulkan not found - skipping test_vulkan, test_vulkan_compute, test_vk_offload_live")
    endif()

    if(SDLOP_VIDEO_WAYLAND AND WAYLAND_CLIENT_FOUND AND SDLOP_HAVE_XKBCOMMON AND SDLOP_WL_VIRTUAL_INPUT_PROTOCOLS)
      add_executable(test_virtual_input tests/test_virtual_input.c
        ${WL_GEN_DIR}/wlr-virtual-pointer-protocol.c
        ${WL_GEN_DIR}/virtual-keyboard-protocol.c)
      target_link_libraries(test_virtual_input PRIVATE sdlop ${WAYLAND_CLIENT_LIBRARIES})
      target_include_directories(test_virtual_input PRIVATE src ${WL_GEN_DIR} ${WAYLAND_CLIENT_INCLUDE_DIRS})
      target_compile_options(test_virtual_input PRIVATE -UNDEBUG)
      add_test(NAME test_virtual_input COMMAND test_virtual_input)
    else()
      message(STATUS "wayland-client, xkbcommon or the virtual-input protocol XMLs not found - skipping test_virtual_input")
    endif()
  endif()

  # ---------------------------------------------------------------------------
  # Benchmarks (SDLop vs SDL3)
  # ---------------------------------------------------------------------------
  if(SDLop_BUILD_BENCHMARKS AND NOT WIN32)  # benchmarks diff against system SDL3
    if(PkgConfig_FOUND)
      pkg_check_modules(SDL3 sdl3)
    endif()
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
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      add_executable(bench_raw_latency bench/bench_raw_latency.c tests/uinput_synth.c)
      target_link_libraries(bench_raw_latency PRIVATE sdlop)
      target_include_directories(bench_raw_latency PRIVATE src tests)
    endif()
  endif()

  # ABI parity gate: builds tools/abi_probe.c against these headers and against
  # stock SDL3 headers (-DSDL3_REFERENCE_INCLUDE=/path/to/SDL3/include) and
  # diffs every size/offset/enum value. Skipped when no reference is provided.
  if (SDL3_REFERENCE_INCLUDE AND NOT EMSCRIPTEN)
    # Invoked through `sh` so a lost exec bit cannot turn the gate into a
    # CTest failure (or worse, a vacuous pass); the mode is preserved too.
    add_test(NAME abi_check COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_abi_check.sh ${SDL3_REFERENCE_INCLUDE})
  endif()

  # Developer tools (not CTests). SDLop_BUILD_TOOLS=OFF skips them; the
  # injectors are also skipped (with a message) when wayland-client,
  # libxkbcommon or the vendored virtual-input protocol XMLs are missing.
  if (SDLop_BUILD_TOOLS AND NOT EMSCRIPTEN)
    # Manual, externally driven trace tool: waits for a compositor-side input
    # script after READY-INPUT.
    add_executable(behaviour_probe_wayland tools/behaviour_probe_wayland.c)
    target_link_libraries(behaviour_probe_wayland PRIVATE sdlop)
    target_include_directories(behaviour_probe_wayland PRIVATE src)

    if (SDLOP_VIDEO_WAYLAND AND WAYLAND_CLIENT_FOUND AND XKBCOMMON_FOUND AND SDLOP_WL_VIRTUAL_INPUT_PROTOCOLS)
      # Reference compositor-side driver for the probe: injects the scripted
      # pointer/key sequence once READY-INPUT appears.
      add_executable(behaviour_probe_inject tools/behaviour_probe_inject.c
        ${WL_GEN_DIR}/wlr-virtual-pointer-protocol.c
        ${WL_GEN_DIR}/virtual-keyboard-protocol.c)
      add_dependencies(behaviour_probe_inject sdlop_wayland_protocols)
      target_include_directories(behaviour_probe_inject PRIVATE
        ${WL_GEN_DIR} ${WAYLAND_CLIENT_INCLUDE_DIRS} ${XKBCOMMON_INCLUDE_DIRS})
      target_link_libraries(behaviour_probe_inject PRIVATE
        ${WAYLAND_CLIENT_LIBRARIES} ${XKBCOMMON_LIBRARIES})

      # Wayland behaviour injector (virtual pointer + keyboard script driver);
      # pairs with tests/run_behaviour_wayland.sh
      add_executable(wl_injector tools/wl_injector.c ${WL_GEN_DIR}/virtual-keyboard-protocol.c ${WL_GEN_DIR}/wlr-virtual-pointer-protocol.c)
      target_include_directories(wl_injector PRIVATE ${WL_GEN_DIR} ${WAYLAND_CLIENT_INCLUDE_DIRS} ${XKBCOMMON_INCLUDE_DIRS})
      target_link_libraries(wl_injector PRIVATE ${WAYLAND_CLIENT_LIBRARIES} ${XKBCOMMON_LIBRARIES})
    else()
      message(STATUS "wayland-client, libxkbcommon or the virtual-input protocol XMLs not found - skipping wl_injector, behaviour_probe_inject")
    endif()
  endif()

  # Options gate: SDLOP_* options are read through hints (settable from code)
  if (SDLop_BUILD_TESTS AND NOT EMSCRIPTEN)
    add_test(NAME options_check COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/options_check.sh ${CMAKE_CURRENT_SOURCE_DIR})
  endif()

  # Export gate: every function declared in include/SDL3 is exported by the
  # shared library and nothing undeclared is exported.
  if (SDLop_BUILD_TESTS AND NOT EMSCRIPTEN)
    add_test(NAME api_exports_check
             COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/api_exports_check.sh
                     $<TARGET_FILE:sdlop_shared> ${CMAKE_CURRENT_SOURCE_DIR}/include)
  endif()

  # Surface differential test: tests/surface_diff.c hashes the result of every
  # blit / conversion / fill / transform case. Built against stock SDL3 as well
  # when both -DSDL3_REFERENCE_INCLUDE and -DSDL3_REFERENCE_LIB are given, the
  # outputs must match exactly; otherwise it runs as a smoke test.
  # (Windows has no run_surface_diff.sh: sh scripts are POSIX-only here.)
  if (SDLop_BUILD_TESTS AND NOT EMSCRIPTEN AND NOT WIN32)
    add_executable(surface_diff tests/surface_diff.c)
    target_link_libraries(surface_diff PRIVATE sdlop)
    if (SDL3_REFERENCE_INCLUDE AND SDL3_REFERENCE_LIB)
      add_executable(surface_diff_ref tests/surface_diff.c)
      target_include_directories(surface_diff_ref BEFORE PRIVATE ${SDL3_REFERENCE_INCLUDE})
      target_link_libraries(surface_diff_ref PRIVATE ${SDL3_REFERENCE_LIB})
      get_filename_component(_sdl3_ref_dir ${SDL3_REFERENCE_LIB} DIRECTORY)
      set_target_properties(surface_diff_ref PROPERTIES BUILD_RPATH ${_sdl3_ref_dir})
      add_test(NAME surface_diff
               COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_surface_diff.sh
                       $<TARGET_FILE:surface_diff> $<TARGET_FILE:surface_diff_ref>)
    else()
      add_test(NAME surface_diff
               COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_surface_diff.sh $<TARGET_FILE:surface_diff>)
    endif()
  endif()

  # API subset gate: every SDL_* symbol exported by SDLop must also be exported
  # by stock SDL3 (strict-subset property). Skipped when no reference .so given.
  if (SDL3_REFERENCE_LIB AND NOT EMSCRIPTEN)
    add_test(NAME api_subset_check
             COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/api_subset_check.sh
                     $<TARGET_FILE:sdlop_shared> ${SDL3_REFERENCE_LIB})
  endif()

  if (SDLOP_SANITIZERS AND NOT EMSCRIPTEN)
    # llvmpipe/lavapipe leak inside libLLVM under LSan (verified: leak frames are
    # entirely Mesa/LLVM internals); disable leak detection for the GPU tests
    # only - every other test keeps full leak checking
    foreach(t test_gl test_gl_matrix test_glx test_vulkan_compute test_egl_api_x11 test_egl_api_wayland)
      if (TEST ${t})
        set_tests_properties(${t} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
      endif()
    endforeach()
    # test_vulkan / test_vk_offload_live / test_gl_offload keep leak checking,
    # minus allocations made inside Mesa / the Vulkan loader (tests/lsan_gpu.supp)
    foreach(t test_vulkan test_vk_offload_live test_gl_offload)
      if (TARGET ${t})
        set_tests_properties(${t} PROPERTIES ENVIRONMENT
          "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan_gpu.supp:print_suppressions=0")
      endif()
    endforeach()
    # test_surface asserts the alloc-failure contract (CreateSurface must
    # return NULL); ASan's allocator aborts on OOM unless allowed to return null
    set_tests_properties(test_surface
      PROPERTIES ENVIRONMENT "ASAN_OPTIONS=allocator_may_return_null=1")
  endif()  # NOT WIN32
endif()
