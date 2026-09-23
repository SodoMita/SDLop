/*
  SDLop test harness: minimal Wayland compositor for testing the
  pointer-constraints / relative-pointer integration deterministically.

  Implements just enough of: wl_compositor, wl_shm, wl_seat (pointer +
  keyboard), xdg-shell, zwp_pointer_constraints_v1,
  zwp_relative_pointer_manager_v1.

  Script (driven by client requests + timers):
    - surface commit + buffer attached        -> wl_pointer.enter(100,100) + frame
    - zwp lock_pointer request                -> zwp_locked_pointer.locked
    - 150 ms after locked                     -> relative_motion(10.5, -3.25) + frame
    - locked_pointer destroyed (client unlock)-> motion(600,450) + frame
    - client disconnect                       -> exit 0

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "xdg-shell-server-protocol.h"
#include "pointer-constraints-server-protocol.h"
#include "relative-pointer-server-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct mini
{
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wl_client *client;

    struct wl_resource *surface_res;   /* the single client surface */
    struct wl_resource *xdg_surface;
    struct wl_resource *xdg_toplevel;
    struct wl_resource *pointer;
    struct wl_resource *touch;
    struct wl_resource *locked;
    struct wl_resource *relpointer;

    bool toplevel_created;
    bool configure_sent;
    bool buffer_attached;
    bool entered;

    struct wl_event_source *rel_timer;   /* send relative motion */
    struct wl_event_source *abs_timer;   /* send absolute motion after unlock */
    struct wl_event_source *watchdog;
};

static struct mini M;

/* ---------------- generic no-op helpers ---------------- */

static void noop(void)
{
}

static void res_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}

/* ---------------- wl_buffer / wl_shm ---------------- */

static void buffer_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}


static const struct wl_buffer_interface buffer_impl = { buffer_destroy };

static void pool_create_buffer(struct wl_client *client, struct wl_resource *res,
                               uint32_t id, int32_t offset, int32_t width, int32_t height,
                               int32_t stride, uint32_t format)
{
    (void)client;
    (void)res;
    (void)offset;
    (void)width;
    (void)height;
    (void)stride;
    (void)format;
    struct wl_resource *buf = wl_resource_create(client, &wl_buffer_interface, 1, id);
    wl_resource_set_implementation(buf, &buffer_impl, NULL, NULL);
}

static void pool_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}


static void pool_resize(struct wl_client *client, struct wl_resource *res, int32_t size)
{
    (void)client;
    (void)res;
    (void)size;
}

static const struct wl_shm_pool_interface pool_impl = { pool_create_buffer, pool_destroy, pool_resize };

static void shm_create_pool(struct wl_client *client, struct wl_resource *res,
                            uint32_t id, int32_t fd, int32_t size)
{
    (void)res;
    (void)size;
    close(fd);
    struct wl_resource *pool = wl_resource_create(client, &wl_shm_pool_interface, 1, id);
    wl_resource_set_implementation(pool, &pool_impl, NULL, NULL);
}

static const struct wl_shm_interface shm_impl = { shm_create_pool, NULL /* release */ };

/* ---------------- wl_surface ---------------- */

static void surface_attach(struct wl_client *client, struct wl_resource *res,
                           struct wl_resource *buffer, int32_t sx, int32_t sy)
{
    (void)client;
    (void)res;
    (void)sx;
    (void)sy;
    M.buffer_attached = (buffer != NULL);
    if (buffer) {
        wl_buffer_send_release(buffer);
    }
}

static void surface_commit(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    (void)res;

    if (M.xdg_toplevel && !M.configure_sent) {
        M.configure_sent = true;
        struct wl_array empty = { 0, 0, NULL };
        xdg_toplevel_send_configure(M.xdg_toplevel, 0, 0, &empty);
        xdg_surface_send_configure(M.xdg_surface, wl_display_next_serial(M.display));
        printf("mini: sent initial configure\n");
        fflush(stdout);
    }
    if (M.buffer_attached && !M.entered && M.pointer) {
        M.entered = true;
        uint32_t serial = wl_display_next_serial(M.display);
        wl_pointer_send_enter(M.pointer, serial, M.surface_res,
                              wl_fixed_from_double(100.0), wl_fixed_from_double(100.0));
        wl_pointer_send_frame(M.pointer);
        printf("mini: sent pointer enter\n");
        fflush(stdout);
        if (M.touch) {
            /* scripted touch sequence: down(50,60) -> motion(70,80) -> up, id 7 */
            wl_touch_send_down(M.touch, serial, 0, M.surface_res, 7,
                               wl_fixed_from_double(50.0), wl_fixed_from_double(60.0));
            wl_touch_send_frame(M.touch);
            wl_touch_send_motion(M.touch, 1, 7,
                                 wl_fixed_from_double(70.0), wl_fixed_from_double(80.0));
            wl_touch_send_frame(M.touch);
            wl_touch_send_up(M.touch, wl_display_next_serial(M.display), 2, 7);
            wl_touch_send_frame(M.touch);
            printf("mini: sent touch down/motion/up\n");
            fflush(stdout);
        }
    }
}

static void surface_frame(struct wl_client *client, struct wl_resource *res, uint32_t callback_id)
{
    struct wl_resource *cb = wl_resource_create(client, &wl_callback_interface, 1, callback_id);
    wl_callback_send_done(cb, 0);
    wl_resource_destroy(cb);
}

static void surface_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = (void *)noop,
    .frame = surface_frame,
    .set_opaque_region = (void *)noop,
    .set_input_region = (void *)noop,
    .commit = surface_commit,
    .set_buffer_transform = (void *)noop,
    .set_buffer_scale = (void *)noop,
    .damage_buffer = (void *)noop,
};

/* ---------------- xdg shell ---------------- */

struct xdg_surface_state
{
    struct wl_resource *resource;
    struct wl_resource *toplevel;
};

static void xdg_toplevel_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = xdg_toplevel_destroy,
    .set_parent = (void *)noop,
    .set_title = (void *)noop,
    .set_app_id = (void *)noop,
    .show_window_menu = (void *)noop,
    .move = (void *)noop,
    .resize = (void *)noop,
    .set_max_size = (void *)noop,
    .set_min_size = (void *)noop,
    .set_maximized = (void *)noop,
    .unset_maximized = (void *)noop,
    .set_fullscreen = (void *)noop,
    .unset_fullscreen = (void *)noop,
    .set_minimized = (void *)noop,
};

static void xdg_surface_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}

static void xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *res, uint32_t id)
{
    (void)client;
    struct wl_resource *tl = wl_resource_create(client, &xdg_toplevel_interface, 1, id);
    wl_resource_set_implementation(tl, &toplevel_impl, res, NULL);
    M.xdg_surface = res;
    M.xdg_toplevel = tl;
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = NULL,
    .set_window_geometry = (void *)noop,
    .ack_configure = (void *)noop,
};

static void xdg_surface_res_destroy(struct wl_resource *res)
{
    struct xdg_surface_state *st = wl_resource_get_user_data(res);
    free(st);
}

static void wm_base_get_xdg_surface(struct wl_client *client, struct wl_resource *res,
                                    uint32_t id, struct wl_resource *surface)
{
    (void)res;
    struct xdg_surface_state *st = calloc(1, sizeof(*st));
    struct wl_resource *xs = wl_resource_create(client, &xdg_surface_interface, 1, id);
    st->resource = xs;
    wl_resource_set_implementation(xs, &xdg_surface_impl, st, xdg_surface_res_destroy);
    M.surface_res = surface;
}

static void wm_base_pong(struct wl_client *client, struct wl_resource *res, uint32_t serial)
{
    (void)client;
    (void)res;
    (void)serial;
}

static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = res_destroy,
    .create_positioner = NULL,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
};

/* ---------------- pointer constraints / relative pointer ---------------- */

static int rel_timer_cb(void *data)
{
    (void)data;
    if (M.relpointer) {
        zwp_relative_pointer_v1_send_relative_motion(
            M.relpointer, 0, 12345,
            wl_fixed_from_double(10.5), wl_fixed_from_double(-3.25),
            wl_fixed_from_double(10.5), wl_fixed_from_double(-3.25));
        printf("mini: sent relative_motion(10.5, -3.25)\n");
        fflush(stdout);
    }
    if (M.pointer) {
        wl_pointer_send_frame(M.pointer);
    }
    return 1; /* one shot */
}

static int abs_timer_cb(void *data)
{
    (void)data;
    if (M.pointer && M.surface_res) {
        wl_pointer_send_motion(M.pointer, 999,
                               wl_fixed_from_double(600.0), wl_fixed_from_double(450.0));
        wl_pointer_send_frame(M.pointer);
        printf("mini: sent absolute motion(600, 450) after unlock\n");
        fflush(stdout);
    }
    return 1;
}

static void locked_destroyed(struct wl_resource *res)
{
    (void)res;
    M.locked = NULL;
    printf("mini: locked_pointer destroyed (client unlocked)\n");
    fflush(stdout);
    if (M.abs_timer) {
        wl_event_source_timer_update(M.abs_timer, 150);
    }
}

static const struct zwp_locked_pointer_v1_interface locked_impl = {
    .destroy = res_destroy,
    .set_region = (void *)noop,
};

static void pc_lock_pointer(struct wl_client *client, struct wl_resource *res, uint32_t id,
                            struct wl_resource *surface, struct wl_resource *pointer,
                            struct wl_resource *region, uint32_t lifetime)
{
    (void)res;
    (void)surface;
    (void)region;
    (void)lifetime;
    M.pointer = pointer;
    struct wl_resource *locked = wl_resource_create(client, &zwp_locked_pointer_v1_interface, 1, id);
    wl_resource_set_implementation(locked, &locked_impl, NULL, locked_destroyed);
    M.locked = locked;
    zwp_locked_pointer_v1_send_locked(locked);
    printf("mini: lock_pointer -> locked\n");
    fflush(stdout);
    if (M.rel_timer) {
        wl_event_source_timer_update(M.rel_timer, 150);
    }
}

static const struct zwp_pointer_constraints_v1_interface pc_impl = {
    .destroy = res_destroy,
    .lock_pointer = pc_lock_pointer,
    .confine_pointer = NULL,
};

static void relpointer_destroyed(struct wl_resource *res)
{
    (void)res;
    M.relpointer = NULL;
}

static const struct zwp_relative_pointer_v1_interface relpointer_impl = {
    .destroy = res_destroy,
};

static void rpm_get_relative_pointer(struct wl_client *client, struct wl_resource *res,
                                     uint32_t id, struct wl_resource *pointer)
{
    (void)res;
    M.pointer = pointer;
    struct wl_resource *rp = wl_resource_create(client, &zwp_relative_pointer_v1_interface, 1, id);
    wl_resource_set_implementation(rp, &relpointer_impl, NULL, relpointer_destroyed);
    M.relpointer = rp;
    printf("mini: relative pointer created\n");
    fflush(stdout);
}

static const struct zwp_relative_pointer_manager_v1_interface rpm_impl = {
    .destroy = res_destroy,
    .get_relative_pointer = rpm_get_relative_pointer,
};

/* ---------------- wl_pointer / wl_keyboard / wl_seat ---------------- */

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = (void *)noop,
    .release = res_destroy,
};

static const struct wl_keyboard_interface keyboard_impl = {
    .release = res_destroy,
};

static void seat_get_pointer(struct wl_client *client, struct wl_resource *res, uint32_t id)
{
    (void)res;
    struct wl_resource *p = wl_resource_create(client, &wl_pointer_interface,
                                               wl_resource_get_version(res), id);
    wl_resource_set_implementation(p, &pointer_impl, NULL, NULL);
    M.pointer = p;
}

static const struct wl_touch_interface touch_impl = {
    .release = res_destroy,
};

static void seat_get_touch(struct wl_client *client, struct wl_resource *res, uint32_t id)
{
    (void)res;
    struct wl_resource *t = wl_resource_create(client, &wl_touch_interface,
                                               wl_resource_get_version(res), id);
    wl_resource_set_implementation(t, &touch_impl, NULL, NULL);
    M.touch = t;
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *res, uint32_t id)
{
    (void)res;
    struct wl_resource *k = wl_resource_create(client, &wl_keyboard_interface,
                                               wl_resource_get_version(res), id);
    wl_resource_set_implementation(k, &keyboard_impl, NULL, NULL);
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = res_destroy,
};

static void seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *seat = wl_resource_create(client, &wl_seat_interface, version, id);
    wl_resource_set_implementation(seat, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities(seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH);
    if (version >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(seat, "mini");
    }
}

/* ---------------- wl_compositor / globals ---------------- */

static void compositor_create_surface(struct wl_client *client, struct wl_resource *res, uint32_t id)
{
    (void)res;
    struct wl_resource *s = wl_resource_create(client, &wl_surface_interface,
                                               wl_resource_get_version(res), id);
    wl_resource_set_implementation(s, &surface_impl, NULL, NULL);
    if (!M.surface_res) {
        M.surface_res = s;
    }
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = NULL,
};

static void compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *c = wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(c, &compositor_impl, NULL, NULL);
}

static void shm_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *s = wl_resource_create(client, &wl_shm_interface, version, id);
    wl_resource_set_implementation(s, &shm_impl, NULL, NULL);
    wl_shm_send_format(s, WL_SHM_FORMAT_XRGB8888);
    wl_shm_send_format(s, WL_SHM_FORMAT_ARGB8888);
}

static void wm_base_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &xdg_wm_base_interface, version, id);
    wl_resource_set_implementation(r, &wm_base_impl, NULL, NULL);
}

static void pc_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &zwp_pointer_constraints_v1_interface, version, id);
    wl_resource_set_implementation(r, &pc_impl, NULL, NULL);
}

static void rpm_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &zwp_relative_pointer_manager_v1_interface, version, id);
    wl_resource_set_implementation(r, &rpm_impl, NULL, NULL);
}

/* ---------------- lifecycle ---------------- */

static int watchdog_cb(void *data)
{
    (void)data;
    fprintf(stderr, "mini: watchdog timeout\n");
    exit(2);
}

static void client_destroyed(struct wl_listener *listener, void *data)
{
    (void)listener;
    (void)data;
    printf("mini: client disconnected, exiting\n");
    fflush(stdout);
    exit(0);
}

static struct wl_listener client_destroy_listener = { .notify = client_destroyed };

static void client_connected(struct wl_listener *listener, void *data)
{
    (void)listener;
    struct wl_client *client = data;
    M.client = client;
    wl_client_add_destroy_listener(client, &client_destroy_listener);
}

static struct wl_listener client_listener = { .notify = client_connected };

int main(void)
{
    M.display = wl_display_create();
    if (!M.display) {
        fprintf(stderr, "mini: wl_display_create failed\n");
        return 1;
    }
    M.loop = wl_display_get_event_loop(M.display);
    const char *name = getenv("WAYLAND_DISPLAY");
    if (wl_display_add_socket(M.display, name) != 0) {
        fprintf(stderr, "mini: add_socket(%s) failed\n", name ? name : "(default)");
        return 1;
    }

    wl_global_create(M.display, &wl_compositor_interface, 4, NULL, compositor_bind);
    wl_global_create(M.display, &wl_shm_interface, 1, NULL, shm_bind);
    wl_global_create(M.display, &wl_seat_interface, 9, NULL, seat_bind);
    wl_global_create(M.display, &xdg_wm_base_interface, 2, NULL, wm_base_bind);
    wl_global_create(M.display, &zwp_pointer_constraints_v1_interface, 1, NULL, pc_bind);
    wl_global_create(M.display, &zwp_relative_pointer_manager_v1_interface, 1, NULL, rpm_bind);

    wl_display_add_client_created_listener(M.display, &client_listener);

    M.rel_timer = wl_event_loop_add_timer(M.loop, rel_timer_cb, NULL);
    M.abs_timer = wl_event_loop_add_timer(M.loop, abs_timer_cb, NULL);
    M.watchdog = wl_event_loop_add_timer(M.loop, watchdog_cb, NULL);
    wl_event_source_timer_update(M.watchdog, 30000);

    printf("mini: listening on %s\n", name ? name : "(default)");
    fflush(stdout);

    wl_display_run(M.display);
    return 0;
}
