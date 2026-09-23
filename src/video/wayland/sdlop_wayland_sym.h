/*
  SDLop - libwayland-client symbol list (X-macro, included 2x: once for
  declarations in sdlop_wayland_dyn.h, once for definitions/loading in
  sdlop_wayland_dyn.c). Same pattern as SDL3's SDL_waylandsym.h.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

/* --- display ------------------------------------------------------ */
SDLOP_WAYLAND_SYM(struct wl_display *, wl_display_connect, (const char *))
SDLOP_WAYLAND_SYM(struct wl_display *, wl_display_connect_to_fd, (int))
SDLOP_WAYLAND_SYM(void, wl_display_disconnect, (struct wl_display *))
SDLOP_WAYLAND_SYM(int, wl_display_get_fd, (struct wl_display *))
SDLOP_WAYLAND_SYM(int, wl_display_roundtrip, (struct wl_display *))
SDLOP_WAYLAND_SYM(int, wl_display_roundtrip_queue, (struct wl_display *, struct wl_event_queue *))
SDLOP_WAYLAND_SYM(int, wl_display_read_events, (struct wl_display *))
SDLOP_WAYLAND_SYM(void, wl_display_cancel_read, (struct wl_display *))
SDLOP_WAYLAND_SYM(int, wl_display_prepare_read, (struct wl_display *))
SDLOP_WAYLAND_SYM(int, wl_display_prepare_read_queue, (struct wl_display *, struct wl_event_queue *))
SDLOP_WAYLAND_SYM(int, wl_display_dispatch, (struct wl_display *))
SDLOP_WAYLAND_SYM(int, wl_display_dispatch_pending, (struct wl_display *))
SDLOP_WAYLAND_SYM(int, wl_display_dispatch_queue, (struct wl_display *, struct wl_event_queue *))
SDLOP_WAYLAND_SYM(int, wl_display_dispatch_queue_pending, (struct wl_display *, struct wl_event_queue *))
SDLOP_WAYLAND_SYM(int, wl_display_flush, (struct wl_display *))
SDLOP_WAYLAND_SYM(int, wl_display_get_error, (struct wl_display *))
SDLOP_WAYLAND_SYM(uint32_t, wl_display_get_protocol_error, (struct wl_display *, const struct wl_interface **, uint32_t *))
SDLOP_WAYLAND_SYM(struct wl_event_queue *, wl_display_create_queue, (struct wl_display *))
SDLOP_WAYLAND_SYM(struct wl_event_queue *, wl_display_create_queue_with_name, (struct wl_display *, const char *))
SDLOP_WAYLAND_SYM(void, wl_event_queue_destroy, (struct wl_event_queue *))

/* --- proxies ------------------------------------------------------ */
SDLOP_WAYLAND_SYM(void, wl_proxy_marshal, (struct wl_proxy *, uint32_t, ...))
SDLOP_WAYLAND_SYM(struct wl_proxy *, wl_proxy_marshal_constructor, (struct wl_proxy *, uint32_t, const struct wl_interface *, ...))
SDLOP_WAYLAND_SYM(struct wl_proxy *, wl_proxy_marshal_constructor_versioned, (struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, ...))
SDLOP_WAYLAND_SYM(struct wl_proxy *, wl_proxy_marshal_flags, (struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, uint32_t, ...))
SDLOP_WAYLAND_SYM(struct wl_proxy *, wl_proxy_create, (struct wl_proxy *, const struct wl_interface *))
SDLOP_WAYLAND_SYM(struct wl_proxy *, wl_proxy_create_wrapper, (void *))
SDLOP_WAYLAND_SYM(void, wl_proxy_wrapper_destroy, (void *))
SDLOP_WAYLAND_SYM(void, wl_proxy_destroy, (struct wl_proxy *))
SDLOP_WAYLAND_SYM(int, wl_proxy_add_listener, (struct wl_proxy *, void (**)(void), void *))
SDLOP_WAYLAND_SYM(const void *, wl_proxy_get_listener, (struct wl_proxy *))
SDLOP_WAYLAND_SYM(void, wl_proxy_set_user_data, (struct wl_proxy *, void *))
SDLOP_WAYLAND_SYM(void *, wl_proxy_get_user_data, (struct wl_proxy *))
SDLOP_WAYLAND_SYM(uint32_t, wl_proxy_get_version, (struct wl_proxy *))
SDLOP_WAYLAND_SYM(uint32_t, wl_proxy_get_id, (struct wl_proxy *))
SDLOP_WAYLAND_SYM(const char *, wl_proxy_get_class, (struct wl_proxy *))
SDLOP_WAYLAND_SYM(void, wl_proxy_set_queue, (struct wl_proxy *, struct wl_event_queue *))
SDLOP_WAYLAND_SYM(void, wl_proxy_set_tag, (struct wl_proxy *, const char *const *))
SDLOP_WAYLAND_SYM(const char *const *, wl_proxy_get_tag, (struct wl_proxy *))

/* --- core interfaces (data symbols) -------------------------------- */
