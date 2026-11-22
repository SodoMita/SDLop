/*
  SDLop - X11 (xcb) symbol list. X-macro, deliberately no include guard:
  included once for declarations (sdlop_x11_dyn.h) and once for the
  definitions + dlsym loop (sdlop_x11_dyn.c). Same idea as SDL3's
  SDL_x11sym.h, but every function pointer type is taken from the real
  prototype with __typeof__, so signatures can never drift.

    SDLOP_X11_MODULE(mod)     starts a library (see sdlop_x11_dyn.c)
    SDLOP_X11_SYM(mod, fn)    function -> X11_fn  (pointer)
    SDLOP_X11_DATA(mod, sym)  data     -> X11_sym (pointer to the object)

  Modules:
    XCB     libxcb.so.1             required - core protocol
    SHM     libxcb-shm.so.0         optional - MIT-SHM software framebuffer
    XKB     libxcb-xkb.so.1         optional - keymap/state events, detectable repeat
    XKBX11  libxkbcommon-x11.so.0   optional - server keymap -> xkbcommon
    XINPUT  libxcb-xinput.so.0      optional - XI2 raw motion (relative mouse)
    RANDR   libxcb-randr.so.0       optional - monitors, modes, refresh rates
    RENDER  libxcb-render.so.0      optional - ARGB cursors
    XFIXES  libxcb-xfixes.so.0      optional - clipboard owner-change events
    SYNC    libxcb-sync.so.1        optional - _NET_WM_SYNC_REQUEST resize sync
    SHAPE   libxcb-shape.so.0       optional - SDL_SetWindowShape input shapes, tooltips

  (libX11 is only needed as a fallback for EGL implementations without
  EGL_EXT_platform_xcb; sdlop_x11_gl.c dlopens it on demand.)

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

/* ---------------------------------------------------------------- XCB */
SDLOP_X11_MODULE(XCB)
SDLOP_X11_SYM(XCB, xcb_connect)
SDLOP_X11_SYM(XCB, xcb_disconnect)
SDLOP_X11_SYM(XCB, xcb_connection_has_error)
SDLOP_X11_SYM(XCB, xcb_get_file_descriptor)
SDLOP_X11_SYM(XCB, xcb_get_setup)
SDLOP_X11_SYM(XCB, xcb_setup_roots_iterator)
SDLOP_X11_SYM(XCB, xcb_screen_next)
SDLOP_X11_SYM(XCB, xcb_screen_allowed_depths_iterator)
SDLOP_X11_SYM(XCB, xcb_depth_next)
SDLOP_X11_SYM(XCB, xcb_depth_visuals_iterator)
SDLOP_X11_SYM(XCB, xcb_visualtype_next)
SDLOP_X11_SYM(XCB, xcb_setup_pixmap_formats)
SDLOP_X11_SYM(XCB, xcb_setup_pixmap_formats_length)
SDLOP_X11_SYM(XCB, xcb_generate_id)
SDLOP_X11_SYM(XCB, xcb_flush)
SDLOP_X11_SYM(XCB, xcb_poll_for_event)
SDLOP_X11_SYM(XCB, xcb_poll_for_queued_event)
SDLOP_X11_SYM(XCB, xcb_request_check)
SDLOP_X11_SYM(XCB, xcb_get_maximum_request_length)
SDLOP_X11_SYM(XCB, xcb_get_extension_data)
SDLOP_X11_SYM(XCB, xcb_intern_atom)
SDLOP_X11_SYM(XCB, xcb_intern_atom_reply)
SDLOP_X11_SYM(XCB, xcb_get_atom_name)
SDLOP_X11_SYM(XCB, xcb_get_atom_name_reply)
SDLOP_X11_SYM(XCB, xcb_get_atom_name_name)
SDLOP_X11_SYM(XCB, xcb_get_atom_name_name_length)
SDLOP_X11_SYM(XCB, xcb_create_window_checked)
SDLOP_X11_SYM(XCB, xcb_destroy_window)
SDLOP_X11_SYM(XCB, xcb_set_selection_owner)
SDLOP_X11_SYM(XCB, xcb_get_selection_owner)
SDLOP_X11_SYM(XCB, xcb_get_selection_owner_reply)
SDLOP_X11_SYM(XCB, xcb_grab_server)
SDLOP_X11_SYM(XCB, xcb_ungrab_server)
SDLOP_X11_SYM(XCB, xcb_convert_selection)
SDLOP_X11_SYM(XCB, xcb_map_window)
SDLOP_X11_SYM(XCB, xcb_unmap_window)
SDLOP_X11_SYM(XCB, xcb_configure_window)
SDLOP_X11_SYM(XCB, xcb_change_window_attributes)
SDLOP_X11_SYM(XCB, xcb_get_window_attributes)
SDLOP_X11_SYM(XCB, xcb_get_window_attributes_reply)
SDLOP_X11_SYM(XCB, xcb_change_property)
SDLOP_X11_SYM(XCB, xcb_delete_property)
SDLOP_X11_SYM(XCB, xcb_get_property)
SDLOP_X11_SYM(XCB, xcb_get_property_reply)
SDLOP_X11_SYM(XCB, xcb_get_property_value)
SDLOP_X11_SYM(XCB, xcb_get_property_value_length)
SDLOP_X11_SYM(XCB, xcb_send_event)
SDLOP_X11_SYM(XCB, xcb_translate_coordinates)
SDLOP_X11_SYM(XCB, xcb_translate_coordinates_reply)
SDLOP_X11_SYM(XCB, xcb_get_input_focus)
SDLOP_X11_SYM(XCB, xcb_get_input_focus_reply)
SDLOP_X11_SYM(XCB, xcb_set_input_focus)
SDLOP_X11_SYM(XCB, xcb_grab_pointer)
SDLOP_X11_SYM(XCB, xcb_grab_pointer_reply)
SDLOP_X11_SYM(XCB, xcb_ungrab_pointer)
SDLOP_X11_SYM(XCB, xcb_warp_pointer)
SDLOP_X11_SYM(XCB, xcb_create_gc)
SDLOP_X11_SYM(XCB, xcb_free_gc)
SDLOP_X11_SYM(XCB, xcb_put_image)
SDLOP_X11_SYM(XCB, xcb_create_pixmap)
SDLOP_X11_SYM(XCB, xcb_free_pixmap)
SDLOP_X11_SYM(XCB, xcb_create_cursor)
SDLOP_X11_SYM(XCB, xcb_create_glyph_cursor)
SDLOP_X11_SYM(XCB, xcb_free_cursor)
SDLOP_X11_SYM(XCB, xcb_open_font)
SDLOP_X11_SYM(XCB, xcb_close_font)
SDLOP_X11_SYM(XCB, xcb_create_colormap)
SDLOP_X11_SYM(XCB, xcb_free_colormap)
SDLOP_X11_SYM(XCB, xcb_clear_area)
SDLOP_X11_SYM(XCB, xcb_query_pointer)
SDLOP_X11_SYM(XCB, xcb_query_pointer_reply)
SDLOP_X11_SYM(XCB, xcb_grab_keyboard)
SDLOP_X11_SYM(XCB, xcb_grab_keyboard_reply)
SDLOP_X11_SYM(XCB, xcb_ungrab_keyboard)
SDLOP_X11_SYM(XCB, xcb_force_screen_saver)
SDLOP_X11_SYM(XCB, xcb_get_geometry)
SDLOP_X11_SYM(XCB, xcb_get_geometry_reply)

/* ---------------------------------------------------------------- SHM */
SDLOP_X11_MODULE(SHM)
SDLOP_X11_DATA(SHM, xcb_shm_id)
SDLOP_X11_SYM(SHM, xcb_shm_query_version)
SDLOP_X11_SYM(SHM, xcb_shm_query_version_reply)
SDLOP_X11_SYM(SHM, xcb_shm_attach_checked)
SDLOP_X11_SYM(SHM, xcb_shm_detach)
SDLOP_X11_SYM(SHM, xcb_shm_put_image)

/* ---------------------------------------------------------------- XKB */
SDLOP_X11_MODULE(XKB)
SDLOP_X11_DATA(XKB, xcb_xkb_id)
SDLOP_X11_SYM(XKB, xcb_xkb_use_extension)
SDLOP_X11_SYM(XKB, xcb_xkb_use_extension_reply)
SDLOP_X11_SYM(XKB, xcb_xkb_select_events_aux_checked)
SDLOP_X11_SYM(XKB, xcb_xkb_per_client_flags)
SDLOP_X11_SYM(XKB, xcb_xkb_per_client_flags_reply)
SDLOP_X11_SYM(XKB, xcb_xkb_get_state)
SDLOP_X11_SYM(XKB, xcb_xkb_get_state_reply)
SDLOP_X11_SYM(XKB, xcb_xkb_get_controls)
SDLOP_X11_SYM(XKB, xcb_xkb_get_controls_reply)

/* ------------------------------------------------------------- XKBX11 */
SDLOP_X11_MODULE(XKBX11)
SDLOP_X11_SYM(XKBX11, xkb_x11_setup_xkb_extension)
SDLOP_X11_SYM(XKBX11, xkb_x11_get_core_keyboard_device_id)
SDLOP_X11_SYM(XKBX11, xkb_x11_keymap_new_from_device)

/* ------------------------------------------------------------- XINPUT */
SDLOP_X11_MODULE(XINPUT)
SDLOP_X11_DATA(XINPUT, xcb_input_id)
SDLOP_X11_SYM(XINPUT, xcb_input_xi_query_version)
SDLOP_X11_SYM(XINPUT, xcb_input_xi_query_version_reply)
SDLOP_X11_SYM(XINPUT, xcb_input_xi_select_events_checked)
SDLOP_X11_SYM(XINPUT, xcb_input_raw_button_press_valuator_mask)
SDLOP_X11_SYM(XINPUT, xcb_input_raw_button_press_axisvalues_raw)

/* -------------------------------------------------------------- RANDR */
SDLOP_X11_MODULE(RANDR)
SDLOP_X11_DATA(RANDR, xcb_randr_id)
SDLOP_X11_SYM(RANDR, xcb_randr_query_version)
SDLOP_X11_SYM(RANDR, xcb_randr_query_version_reply)
SDLOP_X11_SYM(RANDR, xcb_randr_get_monitors)
SDLOP_X11_SYM(RANDR, xcb_randr_get_monitors_reply)
SDLOP_X11_SYM(RANDR, xcb_randr_get_monitors_monitors_iterator)
SDLOP_X11_SYM(RANDR, xcb_randr_monitor_info_next)
SDLOP_X11_SYM(RANDR, xcb_randr_monitor_info_outputs)
SDLOP_X11_SYM(RANDR, xcb_randr_monitor_info_outputs_length)
SDLOP_X11_SYM(RANDR, xcb_randr_get_screen_resources_current)
SDLOP_X11_SYM(RANDR, xcb_randr_get_screen_resources_current_reply)
SDLOP_X11_SYM(RANDR, xcb_randr_get_screen_resources_current_modes)
SDLOP_X11_SYM(RANDR, xcb_randr_get_screen_resources_current_modes_length)
SDLOP_X11_SYM(RANDR, xcb_randr_get_screen_resources_current_outputs)
SDLOP_X11_SYM(RANDR, xcb_randr_get_screen_resources_current_outputs_length)
SDLOP_X11_SYM(RANDR, xcb_randr_get_output_info)
SDLOP_X11_SYM(RANDR, xcb_randr_get_output_info_reply)
SDLOP_X11_SYM(RANDR, xcb_randr_get_output_info_modes)
SDLOP_X11_SYM(RANDR, xcb_randr_get_output_info_modes_length)
SDLOP_X11_SYM(RANDR, xcb_randr_get_crtc_info)
SDLOP_X11_SYM(RANDR, xcb_randr_get_crtc_info_reply)
SDLOP_X11_SYM(RANDR, xcb_randr_select_input)

/* ------------------------------------------------------------- RENDER */
SDLOP_X11_MODULE(RENDER)
SDLOP_X11_DATA(RENDER, xcb_render_id)
SDLOP_X11_SYM(RENDER, xcb_render_query_pict_formats)
SDLOP_X11_SYM(RENDER, xcb_render_query_pict_formats_reply)
SDLOP_X11_SYM(RENDER, xcb_render_query_pict_formats_formats_iterator)
SDLOP_X11_SYM(RENDER, xcb_render_pictforminfo_next)
SDLOP_X11_SYM(RENDER, xcb_render_create_picture)
SDLOP_X11_SYM(RENDER, xcb_render_free_picture)
SDLOP_X11_SYM(RENDER, xcb_render_create_cursor)

/* ------------------------------------------------------------- XFIXES */
SDLOP_X11_MODULE(XFIXES)
SDLOP_X11_DATA(XFIXES, xcb_xfixes_id)
SDLOP_X11_SYM(XFIXES, xcb_xfixes_query_version)
SDLOP_X11_SYM(XFIXES, xcb_xfixes_query_version_reply)
SDLOP_X11_SYM(XFIXES, xcb_xfixes_select_selection_input)

/* --------------------------------------------------------------- SYNC */
SDLOP_X11_MODULE(SYNC)
SDLOP_X11_DATA(SYNC, xcb_sync_id)
SDLOP_X11_SYM(SYNC, xcb_sync_initialize)
SDLOP_X11_SYM(SYNC, xcb_sync_initialize_reply)
SDLOP_X11_SYM(SYNC, xcb_sync_create_counter)
SDLOP_X11_SYM(SYNC, xcb_sync_destroy_counter)
SDLOP_X11_SYM(SYNC, xcb_sync_set_counter)

/* -------------------------------------------------------------- SHAPE */
SDLOP_X11_MODULE(SHAPE)
SDLOP_X11_DATA(SHAPE, xcb_shape_id)
SDLOP_X11_SYM(SHAPE, xcb_shape_query_version)
SDLOP_X11_SYM(SHAPE, xcb_shape_query_version_reply)
SDLOP_X11_SYM(SHAPE, xcb_shape_rectangles)
SDLOP_X11_SYM(SHAPE, xcb_shape_mask)
