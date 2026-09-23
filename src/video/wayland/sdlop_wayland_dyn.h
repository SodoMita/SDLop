/*
  SDLop - dynamic (dlopen) loading of libwayland-client, SDL3-style.

  Nothing is linked against libwayland-client: SDLOP_Wayland_LoadSymbols()
  dlopen()s "libwayland-client.so.0" (override: SDLOP_LIB_WAYLAND) and fills
  the SDLOP_WL_* function pointers below. After the #defines, every call
  site (including the static inlines of wayland-client-protocol.h and the
  wayland-scanner-generated protocol code) goes through the pointers.

  Include this INSTEAD of <wayland-client.h>. It must be included before
  any generated *-client-protocol.h.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_wayland_dyn_h_
#define sdlop_wayland_dyn_h_

#include <stdbool.h>
#include <stdint.h>

struct wl_interface;
struct wl_proxy;
struct wl_event_queue;
struct wl_registry;
struct wl_display;

/* Must be included BEFORE the #defines below, so the real prototypes are
 * declared under their real names (the pointers use SDLOP_WL_ names). */
#include <wayland-client-core.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool SDLOP_Wayland_LoadSymbols(void);
extern void SDLOP_Wayland_UnloadSymbols(void);

#define SDLOP_WAYLAND_SYM(rc, fn, params)  \
    typedef rc(*SDLOP_DYNWL_##fn) params;  \
    extern SDLOP_DYNWL_##fn SDLOP_WL_##fn;
/* interface descriptors (wl_registry_interface etc.) are NOT dlsym'd: the
 * scanner-generated wayland-client-protocol.c (compiled in
 * sdlop_wayland_protos.c with the macros below active) defines the objects
 * under the SDLOP_WL_*_obj names directly - compile-time data, .rodata-safe.
 * The #defines below rename both those definitions and every &wl_x_interface
 * reference in generated code and headers consistently. */
#include "sdlop_wayland_sym.h"

#ifdef __cplusplus
}
#endif

/* Redirect every call site (and the interface data references in the
 * generated protocol code) through the loaded pointers. */
#define wl_display_connect              (*SDLOP_WL_wl_display_connect)
#define wl_display_connect_to_fd        (*SDLOP_WL_wl_display_connect_to_fd)
#define wl_display_disconnect           (*SDLOP_WL_wl_display_disconnect)
#define wl_display_get_fd               (*SDLOP_WL_wl_display_get_fd)
#define wl_display_roundtrip            (*SDLOP_WL_wl_display_roundtrip)
#define wl_display_roundtrip_queue      (*SDLOP_WL_wl_display_roundtrip_queue)
#define wl_display_read_events          (*SDLOP_WL_wl_display_read_events)
#define wl_display_cancel_read          (*SDLOP_WL_wl_display_cancel_read)
#define wl_display_prepare_read         (*SDLOP_WL_wl_display_prepare_read)
#define wl_display_prepare_read_queue   (*SDLOP_WL_wl_display_prepare_read_queue)
#define wl_display_dispatch             (*SDLOP_WL_wl_display_dispatch)
#define wl_display_dispatch_pending     (*SDLOP_WL_wl_display_dispatch_pending)
#define wl_display_dispatch_queue       (*SDLOP_WL_wl_display_dispatch_queue)
#define wl_display_dispatch_queue_pending (*SDLOP_WL_wl_display_dispatch_queue_pending)
#define wl_display_flush                (*SDLOP_WL_wl_display_flush)
#define wl_display_get_error            (*SDLOP_WL_wl_display_get_error)
#define wl_display_get_protocol_error   (*SDLOP_WL_wl_display_get_protocol_error)
#define wl_display_create_queue         (*SDLOP_WL_wl_display_create_queue)
#define wl_display_create_queue_with_name (*SDLOP_WL_wl_display_create_queue_with_name)
#define wl_event_queue_destroy          (*SDLOP_WL_wl_event_queue_destroy)

#define wl_proxy_marshal                     (*SDLOP_WL_wl_proxy_marshal)
#define wl_proxy_marshal_constructor         (*SDLOP_WL_wl_proxy_marshal_constructor)
#define wl_proxy_marshal_constructor_versioned (*SDLOP_WL_wl_proxy_marshal_constructor_versioned)
#define wl_proxy_marshal_flags               (*SDLOP_WL_wl_proxy_marshal_flags)
#define wl_proxy_create                      (*SDLOP_WL_wl_proxy_create)
#define wl_proxy_create_wrapper              (*SDLOP_WL_wl_proxy_create_wrapper)
#define wl_proxy_wrapper_destroy             (*SDLOP_WL_wl_proxy_wrapper_destroy)
#define wl_proxy_destroy                     (*SDLOP_WL_wl_proxy_destroy)
#define wl_proxy_add_listener                (*SDLOP_WL_wl_proxy_add_listener)
#define wl_proxy_get_listener                (*SDLOP_WL_wl_proxy_get_listener)
#define wl_proxy_set_user_data               (*SDLOP_WL_wl_proxy_set_user_data)
#define wl_proxy_get_user_data               (*SDLOP_WL_wl_proxy_get_user_data)
#define wl_proxy_get_version                 (*SDLOP_WL_wl_proxy_get_version)
#define wl_proxy_get_id                      (*SDLOP_WL_wl_proxy_get_id)
#define wl_proxy_get_class                   (*SDLOP_WL_wl_proxy_get_class)
#define wl_proxy_set_queue                   (*SDLOP_WL_wl_proxy_set_queue)
#define wl_proxy_set_tag                     (*SDLOP_WL_wl_proxy_set_tag)
#define wl_proxy_get_tag                     (*SDLOP_WL_wl_proxy_get_tag)

#define wl_registry_interface SDLOP_WL_wl_registry_interface_obj
#define wl_compositor_interface SDLOP_WL_wl_compositor_interface_obj
#define wl_shm_interface SDLOP_WL_wl_shm_interface_obj
#define wl_shm_pool_interface SDLOP_WL_wl_shm_pool_interface_obj
#define wl_buffer_interface SDLOP_WL_wl_buffer_interface_obj
#define wl_callback_interface SDLOP_WL_wl_callback_interface_obj
#define wl_seat_interface SDLOP_WL_wl_seat_interface_obj
#define wl_keyboard_interface SDLOP_WL_wl_keyboard_interface_obj
#define wl_pointer_interface SDLOP_WL_wl_pointer_interface_obj
#define wl_touch_interface SDLOP_WL_wl_touch_interface_obj
#define wl_surface_interface SDLOP_WL_wl_surface_interface_obj
#define wl_region_interface SDLOP_WL_wl_region_interface_obj
#define wl_output_interface SDLOP_WL_wl_output_interface_obj

/* Inlines + core protocol structs, compiled against the macros above */
#include <wayland-client-protocol.h>

#endif /* sdlop_wayland_dyn_h_ */
