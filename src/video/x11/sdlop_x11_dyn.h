/*
  SDLop - dynamic (dlopen) loading of the X11 client libraries.

  Nothing is linked against libxcb and friends: SDLOP_X11_LoadSymbols()
  dlopen()s every module listed in sdlop_x11_sym.h and fills the X11_*
  pointers. libxcb itself is required; every other module is optional and
  the driver degrades feature by feature when one is missing (no MIT-SHM ->
  xcb_put_image, no XInput2 -> warp-based relative mouse, no RandR -> one
  display the size of the root window, no xkbcommon-x11 -> keymap from
  _XKB_RULES_NAMES).

  Library names can be overridden per module (SDLOP_LIB_XCB,
  SDLOP_LIB_XCB_SHM, SDLOP_LIB_XCB_XKB, SDLOP_LIB_XKBCOMMON_X11,
  SDLOP_LIB_XCB_XINPUT, SDLOP_LIB_XCB_RANDR,
  SDLOP_LIB_XCB_RENDER, SDLOP_LIB_XCB_XFIXES, SDLOP_LIB_XCB_SYNC,
  SDLOP_LIB_XCB_SHAPE). Setting an optional module's
  variable to "none" disables it, which is how the fallback paths are
  tested.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_x11_dyn_h_
#define sdlop_x11_dyn_h_

#include <stdbool.h>
#include <stdint.h>

/* the real prototypes: pointer types are derived from them via __typeof__ */
#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/xkb.h>
#include <xcb/xinput.h>
#include <xcb/randr.h>
#include <xcb/render.h>
#if defined(__has_include)
#if __has_include(<xcb/xfixes.h>)
#include <xcb/xfixes.h>
#define SDLOP_HAVE_XCB_XFIXES_H 1
#endif
#endif
#ifndef SDLOP_HAVE_XCB_XFIXES_H
/* libxcb-xfixes is dlopen-ed (optional); without its headers at build time
 * the few pieces used for clipboard owner-change events are declared here
 * exactly as xcb/xfixes.h has them */
typedef struct xcb_xfixes_query_version_cookie_t { unsigned int sequence; } xcb_xfixes_query_version_cookie_t;
typedef struct xcb_xfixes_query_version_reply_t {
    uint8_t response_type, pad0;
    uint16_t sequence;
    uint32_t length, major_version, minor_version;
    uint8_t pad1[16];
} xcb_xfixes_query_version_reply_t;
typedef struct xcb_xfixes_selection_notify_event_t {
    uint8_t response_type, subtype;
    uint16_t sequence;
    xcb_window_t window, owner;
    xcb_atom_t selection;
    xcb_timestamp_t timestamp, selection_timestamp;
    uint8_t pad0[8];
} xcb_xfixes_selection_notify_event_t;
#define XCB_XFIXES_SELECTION_NOTIFY 0
#define XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER 1
#define XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY 2
#define XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE 4
extern xcb_extension_t xcb_xfixes_id;
xcb_xfixes_query_version_cookie_t xcb_xfixes_query_version(xcb_connection_t *c, uint32_t client_major_version,
                                                           uint32_t client_minor_version);
xcb_xfixes_query_version_reply_t *xcb_xfixes_query_version_reply(xcb_connection_t *c,
                                                                 xcb_xfixes_query_version_cookie_t cookie,
                                                                 xcb_generic_error_t **e);
xcb_void_cookie_t xcb_xfixes_select_selection_input(xcb_connection_t *c, xcb_window_t window, xcb_atom_t selection,
                                                    uint32_t event_mask);
#endif
#if defined(__has_include)
#if __has_include(<xcb/sync.h>)
#include <xcb/sync.h>
#define SDLOP_HAVE_XCB_SYNC_H 1
#endif
#endif
#ifndef SDLOP_HAVE_XCB_SYNC_H
/* libxcb-sync is dlopen-ed (optional, _NET_WM_SYNC_REQUEST); declared here
 * exactly as xcb/sync.h has them when its headers are missing */
typedef uint32_t xcb_sync_counter_t;
typedef struct xcb_sync_int64_t {
    int32_t hi;
    uint32_t lo;
} xcb_sync_int64_t;
typedef struct xcb_sync_initialize_cookie_t { unsigned int sequence; } xcb_sync_initialize_cookie_t;
typedef struct xcb_sync_initialize_reply_t {
    uint8_t response_type, pad0;
    uint16_t sequence;
    uint32_t length;
    uint8_t major_version, minor_version;
    uint8_t pad1[22];
} xcb_sync_initialize_reply_t;
extern xcb_extension_t xcb_sync_id;
xcb_sync_initialize_cookie_t xcb_sync_initialize(xcb_connection_t *c, uint8_t desired_major_version,
                                                 uint8_t desired_minor_version);
xcb_sync_initialize_reply_t *xcb_sync_initialize_reply(xcb_connection_t *c, xcb_sync_initialize_cookie_t cookie,
                                                       xcb_generic_error_t **e);
xcb_void_cookie_t xcb_sync_create_counter(xcb_connection_t *c, xcb_sync_counter_t id, xcb_sync_int64_t initial_value);
xcb_void_cookie_t xcb_sync_destroy_counter(xcb_connection_t *c, xcb_sync_counter_t counter);
xcb_void_cookie_t xcb_sync_set_counter(xcb_connection_t *c, xcb_sync_counter_t counter, xcb_sync_int64_t value);
#endif
/* always there: xcb/xinput.h includes xfixes.h, which includes shape.h */
#include <xcb/shape.h>
#include <xkbcommon/xkbcommon-x11.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SDLOP_X11_Module
{
#define SDLOP_X11_MODULE(mod) SDLOP_X11_MOD_##mod,
#define SDLOP_X11_SYM(mod, fn)
#define SDLOP_X11_DATA(mod, sym)
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_MODULE
#undef SDLOP_X11_SYM
#undef SDLOP_X11_DATA
    SDLOP_X11_MOD_COUNT
} SDLOP_X11_Module;

/* loads libxcb (required) and every optional module that is available */
extern bool SDLOP_X11_LoadSymbols(void);
extern void SDLOP_X11_UnloadSymbols(void);
/* true when the module and all of its symbols resolved */
extern bool SDLOP_X11_HaveModule(SDLOP_X11_Module mod);
#define SDLOP_X11_HAVE(mod) SDLOP_X11_HaveModule(SDLOP_X11_MOD_##mod)

#define SDLOP_X11_MODULE(mod)
#define SDLOP_X11_SYM(mod, fn) extern __typeof__(fn) *X11_##fn;
#define SDLOP_X11_DATA(mod, sym) extern __typeof__(sym) *X11_##sym;
#include "sdlop_x11_sym.h"
#undef SDLOP_X11_MODULE
#undef SDLOP_X11_SYM
#undef SDLOP_X11_DATA

#ifdef __cplusplus
}
#endif

#endif /* sdlop_x11_dyn_h_ */
