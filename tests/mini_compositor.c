/*
  SDLop test harness: minimal Wayland compositor for testing the
  pointer-constraints / relative-pointer integration deterministically.

  Implements just enough of: wl_compositor, wl_shm, wl_seat (pointer +
  keyboard), xdg-shell, zwp_pointer_constraints_v1,
  zwp_relative_pointer_manager_v1.

  With MINI_BUFFER_CHECK=1 it is instead a strict wl_shm compositor for
  tests/test_wayland_buffers.c (see "buffer check mode" below).
  MINI_VIEWPORTER=1 also offers wp_viewporter (SDLop then shows the clear
  colour as a 1x1 buffer scaled by a viewport).

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
#include "viewporter-server-protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

/* ---------------- buffer check mode ----------------
 *
 * Holds every committed wl_shm buffer like a real compositor (released at
 * the next "repaint" after it was replaced, repaints every 4 ms, plus one
 * 300 ms stall) and fails the run (exit 3) when the client
 *   - attaches a buffer it holds (no wl_buffer.release yet),
 *   - changes a held buffer's pixels between commit and release,
 *   - commits a frame whose pixels differ from the model below (exit 4).
 *
 * Frame model shared with test_wayland_buffers.c: pixel (0,0) carries
 * 0xA50000 | k for frame k. Frame k with k % 40 == 0 fills the whole
 * buffer with frame_color(k) (size changes happen only there); other
 * frames fill the band frame_band(k). The marker is drawn last. Buffers
 * without the marker (the clear colour, e.g. after a resize) are not
 * checked.
 */
#define MINI_TAG 0xA50000u

typedef struct
{
    void *map;
    size_t size;
    int refs;
} mini_pool;

typedef struct
{
    mini_pool *pool;
    int32_t offset, w, h, stride;
    bool held;
    bool current;
    uint64_t sum;
} mini_buffer;

static bool B_mode;
static struct wl_resource *B_pending;     /* attached, not committed */
static bool B_pending_set;
static struct wl_resource *B_held[16];
static int B_nheld;
static uint32_t *B_model;
static int B_w, B_h;
static int B_last = -1;
static int B_checked, B_commits, B_releases, B_max_held;
static bool B_stalled;
static struct wl_event_source *B_repaint;
/* double-buffered wl_surface / wp_viewport state of the toplevel: SDLop's
 * buffers are 1:1, so every buffer it commits must have scale 1 and the
 * normal transform, whatever the app left pending on the surface; the 1x1
 * clear buffer needs a viewport destination, full-size frames must not
 * have one */
static bool B_viewporter;
static int32_t B_scale = 1, B_scale_pending;
static int32_t B_transform, B_transform_pending;
static bool B_scale_set, B_transform_set;
static int32_t B_dest_w = -1, B_dest_h = -1, B_dest_pending_w, B_dest_pending_h;
static bool B_dest_set;
static int32_t B_clear_w, B_clear_h; /* last 1x1 clear destination */
static int B_clears, B_clears_1x1, B_app_geometry;

static uint32_t frame_color(int k)
{
    return ((uint32_t)k * 2654435761u) & 0xffffffu;
}

static void frame_band(int k, int W, int H, int *x, int *y, int *w, int *h)
{
    *w = W / 2;
    *h = 8;
    *x = (k * 13) % (W / 2);
    *y = (k * 37) % (H - 8);
}

static void mini_pool_unref(mini_pool *p)
{
    if (p && --p->refs == 0) {
        if (p->map) {
            munmap(p->map, p->size);
        }
        free(p);
    }
}

static uint32_t *mb_pixels(mini_buffer *b)
{
    return (uint32_t *)((uint8_t *)b->pool->map + b->offset);
}

static uint64_t mb_sum(mini_buffer *b)
{
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < b->h; y++) {
        const uint32_t *row = (const uint32_t *)((uint8_t *)mb_pixels(b) + (size_t)y * b->stride);
        for (int x = 0; x < b->w; x++) {
            h = (h ^ (row[x] & 0xffffffu)) * 1099511628211ull;
        }
    }
    return h;
}

static void B_fail(int code, const char *what)
{
    fprintf(stderr, "mini: BUFFER CHECK FAILED: %s\n", what);
    exit(code);
}

static void B_model_apply(int k)
{
    if (k % 40 == 0) {
        for (int i = 0; i < B_w * B_h; i++) {
            B_model[i] = frame_color(k);
        }
    } else {
        int x, y, w, h;
        frame_band(k, B_w, B_h, &x, &y, &w, &h);
        for (int j = y; j < y + h; j++) {
            for (int i = x; i < x + w; i++) {
                B_model[j * B_w + i] = frame_color(k);
            }
        }
    }
    B_model[0] = MINI_TAG | (uint32_t)k;
}

static void B_check_content(mini_buffer *b)
{
    const uint32_t *px = mb_pixels(b);
    const uint32_t marker = px[0] & 0xffffffu;
    if ((marker & 0xff0000u) != MINI_TAG) {
        B_clears++;
        return; /* clear colour (e.g. a resized surface before the app draws) */
    }
    const int k = (int)(marker & 0xffffu);
    if (B_clear_w > 0) {
        /* the clear colour covered the window the app now draws into */
        if (B_clear_w != b->w || B_clear_h != b->h) {
            fprintf(stderr, "mini: clear destination %dx%d, next frame %dx%d\n", B_clear_w, B_clear_h, b->w, b->h);
            B_fail(6, "1x1 clear colour was not scaled to the window size");
        }
        B_clear_w = B_clear_h = 0;
    }
    if (k <= B_last && B_last >= 0) {
        B_fail(4, "frame number went backwards");
    }
    if (B_last < 0 && k % 40 != 0) {
        B_fail(4, "first frame is not a full fill (client drew into a committed buffer?)");
    }
    if (b->w != B_w || b->h != B_h) {
        if (k % 40 != 0) {
            B_fail(4, "size changed on a frame that is not a full fill");
        }
        free(B_model);
        B_w = b->w;
        B_h = b->h;
        B_model = calloc((size_t)B_w * B_h, 4);
    }
    for (int i = (B_last < 0 ? k : B_last + 1); i <= k; i++) {
        B_model_apply(i);
    }
    B_last = k;
    for (int y = 0; y < B_h; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)px + (size_t)y * b->stride);
        for (int x = 0; x < B_w; x++) {
            if ((row[x] & 0xffffffu) != B_model[y * B_w + x]) {
                fprintf(stderr, "mini: frame %d pixel (%d,%d) = %06x, want %06x\n", k, x, y,
                        row[x] & 0xffffffu, B_model[y * B_w + x]);
                B_fail(4, "frame content does not match what the client drew");
            }
        }
    }
    B_checked++;
}

/* repaint: release every held buffer that is no longer current */
static int B_repaint_cb(void *data)
{
    (void)data;
    if (!B_stalled && B_checked >= 20) {
        /* once: keep everything for 300 ms (client must wait, not reuse) */
        B_stalled = true;
        wl_event_source_timer_update(B_repaint, 300);
        return 0;
    }
    int n = 0;
    for (int i = 0; i < B_nheld; i++) {
        struct wl_resource *r = B_held[i];
        mini_buffer *b = wl_resource_get_user_data(r);
        if (b->current) {
            B_held[n++] = r;
            continue;
        }
        if (mb_sum(b) != b->sum) {
            B_fail(3, "client modified a buffer before wl_buffer.release");
        }
        b->held = false;
        wl_buffer_send_release(r);
        B_releases++;
    }
    B_nheld = n;
    wl_event_source_timer_update(B_repaint, 4);
    return 0;
}

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

static void buffer_res_destroy(struct wl_resource *res)
{
    mini_buffer *b = wl_resource_get_user_data(res);
    if (!b) {
        return;
    }
    for (int i = 0; i < B_nheld; i++) {
        if (B_held[i] == res) {
            B_held[i] = B_held[--B_nheld];
            break;
        }
    }
    if (B_pending == res) {
        B_pending = NULL;
    }
    mini_pool_unref(b->pool);
    free(b);
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
    mini_buffer *b = NULL;
    mini_pool *p = wl_resource_get_user_data(res);
    if (B_mode && p) {
        b = calloc(1, sizeof(*b));
        b->pool = p;
        p->refs++;
        b->offset = offset;
        b->w = width;
        b->h = height;
        b->stride = stride;
    }
    wl_resource_set_implementation(buf, &buffer_impl, b, buffer_res_destroy);
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

static void pool_res_destroy(struct wl_resource *res)
{
    mini_pool_unref(wl_resource_get_user_data(res));
}

static void shm_create_pool(struct wl_client *client, struct wl_resource *res,
                            uint32_t id, int32_t fd, int32_t size)
{
    (void)res;
    mini_pool *p = NULL;
    if (B_mode) {
        p = calloc(1, sizeof(*p));
        p->refs = 1;
        p->size = (size_t)size;
        p->map = mmap(NULL, p->size, PROT_READ, MAP_SHARED, fd, 0);
        if (p->map == MAP_FAILED) {
            B_fail(5, "cannot map client shm pool");
        }
    }
    close(fd);
    struct wl_resource *pool = wl_resource_create(client, &wl_shm_pool_interface, 1, id);
    wl_resource_set_implementation(pool, &pool_impl, p, pool_res_destroy);
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
    if (B_mode && res != M.surface_res) {
        return; /* cursor etc.: not checked */
    }
    M.buffer_attached = (buffer != NULL);
    if (B_mode) {
        mini_buffer *b = buffer ? wl_resource_get_user_data(buffer) : NULL;
        if (b && b->held) {
            B_fail(3, "client attached a buffer the compositor still holds (no release yet)");
        }
        B_pending = buffer;
        B_pending_set = true;
        return;
    }
    if (buffer) {
        wl_buffer_send_release(buffer);
    }
}

static void B_commit_geometry(void)
{
    if (B_scale_set) {
        B_scale = B_scale_pending;
        B_scale_set = false;
    }
    if (B_transform_set) {
        B_transform = B_transform_pending;
        B_transform_set = false;
    }
    if (B_dest_set) {
        B_dest_w = B_dest_pending_w;
        B_dest_h = B_dest_pending_h;
        B_dest_set = false;
    }
}

static void B_check_geometry(const mini_buffer *b)
{
    if (B_scale != 1) {
        fprintf(stderr, "mini: %dx%d buffer committed at buffer scale %d\n", b->w, b->h, B_scale);
        B_fail(6, "SDLop buffer committed with a buffer scale other than 1");
    }
    if (B_transform != WL_OUTPUT_TRANSFORM_NORMAL) {
        fprintf(stderr, "mini: %dx%d buffer committed with transform %d\n", b->w, b->h, B_transform);
        B_fail(6, "SDLop buffer committed with a buffer transform");
    }
    if (b->w == 1 && b->h == 1) {
        if (B_dest_w <= 0 || B_dest_h <= 0) {
            B_fail(6, "1x1 clear buffer committed without a viewport destination");
        }
        B_clear_w = B_dest_w;
        B_clear_h = B_dest_h;
        B_clears_1x1++;
    } else if (B_dest_w != -1 || B_dest_h != -1) {
        B_fail(6, "full-size buffer committed with a viewport destination still set");
    }
}

static void B_commit(void)
{
    B_commit_geometry();
    if (!B_pending_set) {
        return;
    }
    B_pending_set = false;
    for (int i = 0; i < B_nheld; i++) {
        ((mini_buffer *)wl_resource_get_user_data(B_held[i]))->current = false;
    }
    if (!B_pending) {
        return;
    }
    mini_buffer *b = wl_resource_get_user_data(B_pending);
    B_check_geometry(b);
    B_check_content(b);
    b->sum = mb_sum(b);
    b->held = true;
    b->current = true;
    if (B_nheld >= 16) {
        B_fail(3, "client has more than 16 buffers in flight");
    }
    B_held[B_nheld++] = B_pending;
    if (B_nheld > B_max_held) {
        B_max_held = B_nheld;
    }
    B_commits++;
    B_pending = NULL;
}

static void surface_commit(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    (void)res;

    if (B_mode) {
        if (res != M.surface_res) {
            return;
        }
        B_commit();
    }
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
            /* scripted two-finger sequence:
             *   down7(50,60) -> down8(100,120) -> motion7(70,80) ->
             *   motion8(140,160) -> up7 -> cancel (ends finger 8) */
            wl_touch_send_down(M.touch, serial, 0, M.surface_res, 7,
                               wl_fixed_from_double(50.0), wl_fixed_from_double(60.0));
            wl_touch_send_frame(M.touch);
            wl_touch_send_down(M.touch, serial, 1, M.surface_res, 8,
                               wl_fixed_from_double(100.0), wl_fixed_from_double(120.0));
            wl_touch_send_frame(M.touch);
            wl_touch_send_motion(M.touch, 2, 7,
                                 wl_fixed_from_double(70.0), wl_fixed_from_double(80.0));
            wl_touch_send_frame(M.touch);
            wl_touch_send_motion(M.touch, 3, 8,
                                 wl_fixed_from_double(140.0), wl_fixed_from_double(160.0));
            wl_touch_send_frame(M.touch);
            wl_touch_send_up(M.touch, wl_display_next_serial(M.display), 4, 7);
            wl_touch_send_frame(M.touch);
            wl_touch_send_cancel(M.touch);
            wl_touch_send_frame(M.touch);
            printf("mini: sent touch 2-finger sequence + cancel\n");
            fflush(stdout);
        }
    }
}

static void surface_set_buffer_transform(struct wl_client *client, struct wl_resource *res, int32_t transform)
{
    (void)client;
    if (B_mode && res == M.surface_res) {
        B_transform_pending = transform;
        B_transform_set = true;
        if (transform != WL_OUTPUT_TRANSFORM_NORMAL) {
            B_app_geometry++;
        }
    }
}

static void surface_set_buffer_scale(struct wl_client *client, struct wl_resource *res, int32_t scale)
{
    (void)client;
    if (B_mode && res == M.surface_res) {
        B_scale_pending = scale;
        B_scale_set = true;
        if (scale != 1) {
            B_app_geometry++;
        }
    }
}

static void surface_frame(struct wl_client *client, struct wl_resource *res, uint32_t callback_id)
{
    (void)res;
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
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
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

/* ---------------- wp_viewporter (MINI_VIEWPORTER=1) ---------------- */

static void viewport_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    if (B_mode && wl_resource_get_user_data(res) == M.surface_res) {
        B_dest_pending_w = B_dest_pending_h = -1;
        B_dest_set = true;
    }
    wl_resource_destroy(res);
}

static void viewport_set_source(struct wl_client *client, struct wl_resource *res,
                                wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h)
{
    (void)client; (void)res; (void)x; (void)y; (void)w; (void)h;
}

static void viewport_set_destination(struct wl_client *client, struct wl_resource *res, int32_t w, int32_t h)
{
    (void)client;
    if (B_mode && wl_resource_get_user_data(res) == M.surface_res) {
        B_dest_pending_w = w;
        B_dest_pending_h = h;
        B_dest_set = true;
    }
}

static const struct wp_viewport_interface viewport_impl = {
    .destroy = viewport_destroy,
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};

static void viewporter_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}

static void viewporter_get_viewport(struct wl_client *client, struct wl_resource *res, uint32_t id,
                                    struct wl_resource *surface)
{
    (void)res;
    struct wl_resource *v = wl_resource_create(client, &wp_viewport_interface, 1, id);
    wl_resource_set_implementation(v, &viewport_impl, surface, NULL);
}

static const struct wp_viewporter_interface viewporter_impl = {
    .destroy = viewporter_destroy,
    .get_viewport = viewporter_get_viewport,
};

static void viewporter_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wp_viewporter_interface, version, id);
    wl_resource_set_implementation(r, &viewporter_impl, NULL, NULL);
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
    if (B_mode) {
        printf("mini: buffer check: %d commits, %d frames verified, %d releases, max %d held, "
               "%d clear-colour commits (%d as 1x1 viewport), %d app scale/transform requests\n",
               B_commits, B_checked, B_releases, B_max_held, B_clears + B_clears_1x1, B_clears_1x1,
               B_app_geometry);
        fflush(stdout);
        if (B_viewporter && B_clears_1x1 == 0) {
            fprintf(stderr, "mini: BUFFER CHECK FAILED: viewporter offered but no 1x1 clear colour committed\n");
            exit(4);
        }
        exit(B_checked > 0 ? 0 : 4);
    }
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
    /* v9 needs libwayland >= 1.22; clamp to what the library implements */
    wl_global_create(M.display, &wl_seat_interface,
                     wl_seat_interface.version < 9 ? wl_seat_interface.version : 9,
                     NULL, seat_bind);
    wl_global_create(M.display, &xdg_wm_base_interface, 2, NULL, wm_base_bind);
    wl_global_create(M.display, &zwp_pointer_constraints_v1_interface, 1, NULL, pc_bind);
    wl_global_create(M.display, &zwp_relative_pointer_manager_v1_interface, 1, NULL, rpm_bind);
    B_viewporter = getenv("MINI_VIEWPORTER") && atoi(getenv("MINI_VIEWPORTER"));
    if (B_viewporter) {
        wl_global_create(M.display, &wp_viewporter_interface, 1, NULL, viewporter_bind);
    }

    wl_display_add_client_created_listener(M.display, &client_listener);

    M.rel_timer = wl_event_loop_add_timer(M.loop, rel_timer_cb, NULL);
    M.abs_timer = wl_event_loop_add_timer(M.loop, abs_timer_cb, NULL);
    M.watchdog = wl_event_loop_add_timer(M.loop, watchdog_cb, NULL);
    B_mode = getenv("MINI_BUFFER_CHECK") && atoi(getenv("MINI_BUFFER_CHECK"));
    if (B_mode) {
        B_repaint = wl_event_loop_add_timer(M.loop, B_repaint_cb, NULL);
        wl_event_source_timer_update(B_repaint, 4);
    }
    wl_event_source_timer_update(M.watchdog, 30000);

    printf("mini: listening on %s\n", name ? name : "(default)");
    fflush(stdout);

    wl_display_run(M.display);
    return 0;
}
