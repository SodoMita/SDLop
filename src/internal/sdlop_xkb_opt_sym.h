/* SDLop - optional libxkbcommon symbols (X-macro, no include guard).
 * Each is looked up best-effort and may stay NULL:
 *  - compose (libxkbcommon >= 0.5): dead keys / Compose key text; used only
 *    when all compose entry points are present (SDLOP_Xkb_HaveCompose)
 *  - xkb_keysym_to_lower (>= 0.8): verifies key-name capitalization */
SDLOP_XKB_SYM(struct xkb_compose_table *, xkb_compose_table_new_from_locale, (struct xkb_context *, const char *, enum xkb_compose_compile_flags))
SDLOP_XKB_SYM(void, xkb_compose_table_unref, (struct xkb_compose_table *))
SDLOP_XKB_SYM(struct xkb_compose_state *, xkb_compose_state_new, (struct xkb_compose_table *, enum xkb_compose_state_flags))
SDLOP_XKB_SYM(void, xkb_compose_state_unref, (struct xkb_compose_state *))
SDLOP_XKB_SYM(enum xkb_compose_feed_result, xkb_compose_state_feed, (struct xkb_compose_state *, xkb_keysym_t))
SDLOP_XKB_SYM(void, xkb_compose_state_reset, (struct xkb_compose_state *))
SDLOP_XKB_SYM(enum xkb_compose_status, xkb_compose_state_get_status, (struct xkb_compose_state *))
SDLOP_XKB_SYM(int, xkb_compose_state_get_utf8, (struct xkb_compose_state *, char *, size_t))
SDLOP_XKB_SYM(xkb_keysym_t, xkb_keysym_to_lower, (xkb_keysym_t))
