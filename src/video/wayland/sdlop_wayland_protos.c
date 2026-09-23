/*
  SDLop - translation unit for the wayland-scanner-generated protocol code.

  The generated .c files are #included (not compiled directly) so the
  dynamic symbol indirection of sdlop_wayland_dyn.h is in effect before
  their inline wrappers and interface references are compiled - exactly
  the ordering SDL3 relies on for its dynamic Wayland loading.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_wayland_dyn.h"

/* core protocol code: defines wl_*_interface descriptors under the
 * SDLOP_WL_*_obj names (macros active), so nothing needs dlsym'd data */
#include "wayland-client-protocol.c"
#include "xdg-shell-protocol.c"
#include "pointer-constraints-protocol.c"
#include "relative-pointer-protocol.c"
