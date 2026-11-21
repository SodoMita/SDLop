# Cross-compile SDLop for Windows with MinGW-w64.
#
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
#
# Set MINGW_TRIPLET to target 32-bit Windows (i686-w64-mingw32).

set(CMAKE_SYSTEM_NAME Windows)

if(NOT MINGW_TRIPLET)
  set(MINGW_TRIPLET x86_64-w64-mingw32)
endif()

find_program(MINGW_CC NAMES ${MINGW_TRIPLET}-gcc REQUIRED)
get_filename_component(MINGW_BIN "${MINGW_CC}" DIRECTORY)

set(CMAKE_C_COMPILER   "${MINGW_BIN}/${MINGW_TRIPLET}-gcc")
set(CMAKE_CXX_COMPILER "${MINGW_BIN}/${MINGW_TRIPLET}-g++")
set(CMAKE_RC_COMPILER  "${MINGW_BIN}/${MINGW_TRIPLET}-windres")
set(CMAKE_AR           "${MINGW_BIN}/${MINGW_TRIPLET}-ar")
set(CMAKE_RANLIB       "${MINGW_BIN}/${MINGW_TRIPLET}-ranlib")
set(CMAKE_STRIP        "${MINGW_BIN}/${MINGW_TRIPLET}-strip")

set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_TRIPLET})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Wine runs the tests here; see tests/README-Windows.md
set(CMAKE_CROSSCOMPILING_EMULATOR "wine" CACHE STRING "")
