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
  MINI_SELECTION=1 offers wl_data_device_manager and
  zwp_primary_selection_device_manager_v1 shared by any number of clients
  (selections go to every client, focus is not modelled) and exits when the
  last client disconnects - for tests/test_wayland_clipboard.c.
  MINI_DND=<data> (implies the data device) drags <data> onto the client's
  window once it has a buffer: data_offer with MINI_DND_MIMES (default
  text/uri-list), enter(10.5,20.25), motion(30,40); after the client's
  set_actions -> action(copy) and drop. The run fails (exit 6) unless the
  client read the data and called wl_data_offer.finish - or, with
  MINI_DND_REFUSE=1, unless it accepted nothing and read nothing.
  MINI_SCALE=<source>:<value> gives the client's window a preferred scale
  for tests/test_wayland_hidpi.c - fractional:<n/120> (offers
  wp_fractional_scale_manager_v1), buffer:<n> (wl_compositor v6
  preferred_buffer_scale) or output:<n> (a wl_output of that scale, entered
  by the window). MINI_SCALE_CHANGE=<value> switches to another value of the
  same source on SIGUSR1 (sent by the test when it is ready). Every window
  commit is checked (buffer size a multiple of the buffer scale, else exit
  7) and appended to MINI_GEOMETRY_LOG as
  "buffer=WxH scale=S dest=WxH" (dest -1x-1: no viewport destination).
  MINI_TEXT_INPUT=<log> offers zwp_text_input_manager_v3 for
  tests/test_wayland_textinput.c: once the window has a buffer it gets
  keyboard (and text-input) focus; every text-input commit is appended to
  <log> as "enabled=E hint=H purpose=P rect=X,Y,W,H". After the first
  enabled commit the "IME" sends preedit "日本語" (cursor bytes 3..6), done,
  then a long commit_string (MINI_TI_COMMIT), done.

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
#include "fractional-scale-v1-server-protocol.h"
#include "primary-selection-server-protocol.h"
#include "text-input-unstable-v3-server-protocol.h"

#include <signal.h>
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
static struct wl_resource *T_keyboard; /* text-input mode: keyboard to focus */
static void T_maybe_focus(void);

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

/* MINI_SCALE (see the header) */
static bool H_mode;
static char H_source[16];
static int H_value, H_change;
static int H_pending_w, H_pending_h; /* buffer attached to the toplevel, 0: none */
static bool H_pending_set;
static struct wl_resource *H_fractional; /* wp_fractional_scale_v1 of the window */
static struct wl_list H_outputs;        /* bound wl_output resources */
/* external linkage: an optimizer may drop stores to a never-read static,
 * which LeakSanitizer then reports as a leaked event source */
struct wl_event_source *mini_scale_signal_source;
static bool H_entered;

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
    if ((B_mode || H_mode) && p) {
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
    if (B_mode || H_mode) {
        p = calloc(1, sizeof(*p));
        p->refs = 1;
        p->size = (size_t)size;
    }
    if (B_mode) { /* H_mode only needs buffer sizes */
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
    if (H_mode && res == M.surface_res) {
        mini_buffer *hb = buffer ? wl_resource_get_user_data(buffer) : NULL;
        H_pending_w = hb ? hb->w : 0;
        H_pending_h = hb ? hb->h : 0;
        H_pending_set = true;
    }
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

static void D_maybe_start(void);
static void H_commit(void);
static void H_send(int value);

static void surface_commit(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    (void)res;

    if (H_mode && res == M.surface_res) {
        H_commit();
    }

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
    D_maybe_start();
    T_maybe_focus();
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
    if ((B_mode || H_mode) && res == M.surface_res) {
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
    if ((B_mode || H_mode) && res == M.surface_res) {
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
    if (H_mode && strcmp(H_source, "buffer") == 0) {
        H_send(H_value);
    }
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
    T_keyboard = k;
    T_maybe_focus();
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
    if ((B_mode || H_mode) && wl_resource_get_user_data(res) == M.surface_res) {
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
    if ((B_mode || H_mode) && wl_resource_get_user_data(res) == M.surface_res) {
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

/* ---------------- selections (MINI_SELECTION=1) ---------------- */
/*
  One model for both wl_data_device (CLIPBOARD) and
  zwp_primary_selection_device_v1 (PRIMARY): a selection is a source
  resource; every device gets a fresh offer for it (data_offer, offer*,
  selection) when it changes and when the device is created. An offer
  forwards receive() to the source only while that source is still the
  selection (generation check).
*/

static bool S_mode;
static int S_clients;

typedef struct Sel
{
    bool primary;
    struct wl_list devices; /* device resources (wl_resource_get_link) */
    struct wl_resource *source;
    uint32_t gen;
} Sel;

static Sel S_clip = { false, { NULL, NULL }, NULL, 0 };
static Sel S_prim = { true, { NULL, NULL }, NULL, 0 };

#define S_MAX_MIMES 32
typedef struct SrcData
{
    Sel *sel;
    char *mimes[S_MAX_MIMES];
    int n;
} SrcData;

typedef struct OfferData
{
    Sel *sel;
    uint32_t gen;
} OfferData;

static void S_res_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}

static void S_offer_free(struct wl_resource *res)
{
    free(wl_resource_get_user_data(res));
}

static void S_offer_receive(struct wl_client *client, struct wl_resource *res, const char *mime, int32_t fd)
{
    (void)client;
    OfferData *od = wl_resource_get_user_data(res);
    Sel *sel = od->sel;
    if (sel->source && od->gen == sel->gen) {
        if (sel->primary) {
            zwp_primary_selection_source_v1_send_send(sel->source, mime, fd);
        } else {
            wl_data_source_send_send(sel->source, mime, fd);
        }
    }
    close(fd);
}

static void S_offer_accept(struct wl_client *client, struct wl_resource *res, uint32_t serial, const char *mime)
{
    (void)client;
    (void)res;
    (void)serial;
    (void)mime;
}

static void S_offer_finish(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    (void)res;
}

static void S_offer_set_actions(struct wl_client *client, struct wl_resource *res, uint32_t actions, uint32_t preferred)
{
    (void)client;
    (void)res;
    (void)actions;
    (void)preferred;
}

static const struct wl_data_offer_interface S_data_offer_impl = {
    .accept = S_offer_accept,
    .receive = S_offer_receive,
    .destroy = S_res_destroy,
    .finish = S_offer_finish,
    .set_actions = S_offer_set_actions,
};

static const struct zwp_primary_selection_offer_v1_interface S_prim_offer_impl = {
    .receive = S_offer_receive,
    .destroy = S_res_destroy,
};

static void S_send_selection(Sel *sel, struct wl_resource *device)
{
    struct wl_client *client = wl_resource_get_client(device);
    if (!sel->source) {
        if (sel->primary) {
            zwp_primary_selection_device_v1_send_selection(device, NULL);
        } else {
            wl_data_device_send_selection(device, NULL);
        }
        return;
    }
    SrcData *sd = wl_resource_get_user_data(sel->source);
    OfferData *od = calloc(1, sizeof(*od));
    od->sel = sel;
    od->gen = sel->gen;
    struct wl_resource *offer;
    if (sel->primary) {
        offer = wl_resource_create(client, &zwp_primary_selection_offer_v1_interface, wl_resource_get_version(device), 0);
        wl_resource_set_implementation(offer, &S_prim_offer_impl, od, S_offer_free);
        zwp_primary_selection_device_v1_send_data_offer(device, offer);
        for (int i = 0; i < sd->n; i++) {
            zwp_primary_selection_offer_v1_send_offer(offer, sd->mimes[i]);
        }
        zwp_primary_selection_device_v1_send_selection(device, offer);
    } else {
        offer = wl_resource_create(client, &wl_data_offer_interface, wl_resource_get_version(device), 0);
        wl_resource_set_implementation(offer, &S_data_offer_impl, od, S_offer_free);
        wl_data_device_send_data_offer(device, offer);
        for (int i = 0; i < sd->n; i++) {
            wl_data_offer_send_offer(offer, sd->mimes[i]);
        }
        wl_data_device_send_selection(device, offer);
    }
}

static void S_broadcast(Sel *sel)
{
    struct wl_resource *dev;
    wl_resource_for_each(dev, &sel->devices)
    {
        S_send_selection(sel, dev);
    }
}

static void S_set_selection(Sel *sel, struct wl_resource *source)
{
    if (sel->source && sel->source != source) {
        if (sel->primary) {
            zwp_primary_selection_source_v1_send_cancelled(sel->source);
        } else {
            wl_data_source_send_cancelled(sel->source);
        }
    }
    sel->source = source;
    sel->gen++;
    S_broadcast(sel);
}

/* sources */
static void S_source_free(struct wl_resource *res)
{
    SrcData *sd = wl_resource_get_user_data(res);
    if (sd->sel->source == res) {
        sd->sel->source = NULL;
        sd->sel->gen++;
        S_broadcast(sd->sel);
    }
    for (int i = 0; i < sd->n; i++) {
        free(sd->mimes[i]);
    }
    free(sd);
}

static void S_source_offer(struct wl_client *client, struct wl_resource *res, const char *mime)
{
    (void)client;
    SrcData *sd = wl_resource_get_user_data(res);
    if (sd->n < S_MAX_MIMES) {
        sd->mimes[sd->n++] = strdup(mime);
    }
}

static void S_source_set_actions(struct wl_client *client, struct wl_resource *res, uint32_t actions)
{
    (void)client;
    (void)res;
    (void)actions;
}

static const struct wl_data_source_interface S_data_source_impl = {
    .offer = S_source_offer,
    .destroy = S_res_destroy,
    .set_actions = S_source_set_actions,
};

static const struct zwp_primary_selection_source_v1_interface S_prim_source_impl = {
    .offer = S_source_offer,
    .destroy = S_res_destroy,
};

/* devices */
static void S_device_free(struct wl_resource *res)
{
    wl_list_remove(wl_resource_get_link(res));
}

static void S_device_set_selection(struct wl_client *client, struct wl_resource *res, struct wl_resource *source,
                                   uint32_t serial)
{
    (void)client;
    (void)res;
    (void)serial;
    S_set_selection(&S_clip, source);
}

static void S_prim_device_set_selection(struct wl_client *client, struct wl_resource *res, struct wl_resource *source,
                                        uint32_t serial)
{
    (void)client;
    (void)res;
    (void)serial;
    S_set_selection(&S_prim, source);
}

static void S_device_start_drag(struct wl_client *client, struct wl_resource *res, struct wl_resource *source,
                                struct wl_resource *origin, struct wl_resource *icon, uint32_t serial)
{
    (void)client;
    (void)res;
    (void)source;
    (void)origin;
    (void)icon;
    (void)serial;
}

static const struct wl_data_device_interface S_data_device_impl = {
    .start_drag = S_device_start_drag,
    .set_selection = S_device_set_selection,
    .release = S_res_destroy,
};

static const struct zwp_primary_selection_device_v1_interface S_prim_device_impl = {
    .set_selection = S_prim_device_set_selection,
    .destroy = S_res_destroy,
};

/* managers */
static struct wl_resource *S_new_source(struct wl_client *client, struct wl_resource *mgr, uint32_t id, Sel *sel)
{
    SrcData *sd = calloc(1, sizeof(*sd));
    sd->sel = sel;
    struct wl_resource *src;
    if (sel->primary) {
        src = wl_resource_create(client, &zwp_primary_selection_source_v1_interface, wl_resource_get_version(mgr), id);
        wl_resource_set_implementation(src, &S_prim_source_impl, sd, S_source_free);
    } else {
        src = wl_resource_create(client, &wl_data_source_interface, wl_resource_get_version(mgr), id);
        wl_resource_set_implementation(src, &S_data_source_impl, sd, S_source_free);
    }
    return src;
}

static void S_new_device(struct wl_client *client, struct wl_resource *mgr, uint32_t id, Sel *sel)
{
    struct wl_resource *dev;
    if (sel->primary) {
        dev = wl_resource_create(client, &zwp_primary_selection_device_v1_interface, wl_resource_get_version(mgr), id);
        wl_resource_set_implementation(dev, &S_prim_device_impl, NULL, S_device_free);
    } else {
        dev = wl_resource_create(client, &wl_data_device_interface, wl_resource_get_version(mgr), id);
        wl_resource_set_implementation(dev, &S_data_device_impl, NULL, S_device_free);
    }
    wl_list_insert(&sel->devices, wl_resource_get_link(dev));
    if (sel->source) {
        S_send_selection(sel, dev); /* what a client sees on keyboard focus */
    }
}

static void S_mgr_create_source(struct wl_client *client, struct wl_resource *res, uint32_t id)
{
    S_new_source(client, res, id, &S_clip);
}

static void S_mgr_get_device(struct wl_client *client, struct wl_resource *res, uint32_t id, struct wl_resource *seat)
{
    (void)seat;
    S_new_device(client, res, id, &S_clip);
}

static void S_prim_mgr_create_source(struct wl_client *client, struct wl_resource *res, uint32_t id)
{
    S_new_source(client, res, id, &S_prim);
}

static void S_prim_mgr_get_device(struct wl_client *client, struct wl_resource *res, uint32_t id,
                                  struct wl_resource *seat)
{
    (void)seat;
    S_new_device(client, res, id, &S_prim);
}

static const struct wl_data_device_manager_interface S_mgr_impl = {
    .create_data_source = S_mgr_create_source,
    .get_data_device = S_mgr_get_device,
};

static const struct zwp_primary_selection_device_manager_v1_interface S_prim_mgr_impl = {
    .create_source = S_prim_mgr_create_source,
    .get_device = S_prim_mgr_get_device,
    .destroy = S_res_destroy,
};

static void S_mgr_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wl_data_device_manager_interface, version, id);
    wl_resource_set_implementation(r, &S_mgr_impl, NULL, NULL);
}

static void S_prim_mgr_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &zwp_primary_selection_device_manager_v1_interface, version, id);
    wl_resource_set_implementation(r, &S_prim_mgr_impl, NULL, NULL);
}

/* ---------------- synthetic drag (MINI_DND) ---------------- */

static const char *D_data;
static bool D_refuse_expected;
static bool D_started, D_dropped, D_finished, D_accepted;
static int D_received;
static struct wl_resource *D_device, *D_offer;
static struct wl_event_source *D_timer, *D_drop_timer;

static void D_offer_accept(struct wl_client *client, struct wl_resource *res, uint32_t serial, const char *mime)
{
    (void)client;
    (void)res;
    (void)serial;
    D_accepted = mime != NULL;
    printf("mini: dnd accept %s\n", mime ? mime : "(none)");
    fflush(stdout);
}

static void D_offer_receive(struct wl_client *client, struct wl_resource *res, const char *mime, int32_t fd)
{
    (void)client;
    (void)res;
    size_t len = strlen(D_data), off = 0;
    while (off < len) {
        ssize_t n = write(fd, D_data + off, len - off);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
    }
    close(fd);
    D_received++;
    printf("mini: dnd receive %s (%zu bytes)\n", mime, off);
    fflush(stdout);
}

static void D_offer_finish(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    (void)res;
    D_finished = true;
    printf("mini: dnd finish\n");
    fflush(stdout);
}

static int D_drop_cb(void *data)
{
    (void)data;
    if (D_device && !D_dropped) {
        D_dropped = true;
        if (D_accepted) {
            wl_data_device_send_drop(D_device);
        } else {
            wl_data_device_send_leave(D_device); /* nothing accepted: the drag is cancelled */
        }
        printf("mini: dnd %s\n", D_accepted ? "drop" : "leave (cancelled)");
        fflush(stdout);
    }
    return 0;
}

static void D_offer_set_actions(struct wl_client *client, struct wl_resource *res, uint32_t actions, uint32_t preferred)
{
    (void)client;
    (void)preferred;
    uint32_t action = (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : 0;
    wl_data_offer_send_action(res, action);
    wl_event_source_timer_update(D_drop_timer, 50);
}

static void D_offer_destroyed(struct wl_resource *res)
{
    if (res == D_offer) {
        D_offer = NULL;
    }
}

static const struct wl_data_offer_interface D_offer_impl = {
    .accept = D_offer_accept,
    .receive = D_offer_receive,
    .destroy = S_res_destroy,
    .finish = D_offer_finish,
    .set_actions = D_offer_set_actions,
};

static int D_timer_cb(void *data)
{
    (void)data;
    struct wl_client *client = wl_resource_get_client(M.surface_res);
    struct wl_resource *dev;
    D_device = NULL;
    wl_resource_for_each(dev, &S_clip.devices)
    {
        if (wl_resource_get_client(dev) == client) {
            D_device = dev;
        }
    }
    if (!D_device) {
        fprintf(stderr, "mini: dnd: client has no wl_data_device\n");
        exit(6);
    }
    const uint32_t version = (uint32_t)wl_resource_get_version(D_device);
    D_offer = wl_resource_create(client, &wl_data_offer_interface, (int)version, 0);
    wl_resource_set_implementation(D_offer, &D_offer_impl, NULL, D_offer_destroyed);
    wl_data_device_send_data_offer(D_device, D_offer);
    const char *mimes = getenv("MINI_DND_MIMES") ? getenv("MINI_DND_MIMES") : "text/uri-list";
    char *list = strdup(mimes), *save = NULL;
    for (char *m = strtok_r(list, ",", &save); m; m = strtok_r(NULL, ",", &save)) {
        wl_data_offer_send_offer(D_offer, m);
    }
    free(list);
    if (version >= 3) {
        wl_data_offer_send_source_actions(D_offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                                                       WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    }
    wl_data_device_send_enter(D_device, wl_display_next_serial(M.display), M.surface_res, wl_fixed_from_double(10.5),
                              wl_fixed_from_double(20.25), D_offer);
    wl_data_device_send_motion(D_device, 1, wl_fixed_from_double(30.0), wl_fixed_from_double(40.0));
    if (version < 3) {
        wl_event_source_timer_update(D_drop_timer, 50);
    }
    printf("mini: dnd enter + motion (%s)\n", mimes);
    fflush(stdout);
    return 0;
}

static void D_maybe_start(void)
{
    if (D_data && !D_started && M.buffer_attached && M.surface_res) {
        D_started = true;
        wl_event_source_timer_update(D_timer, 100);
    }
}

/* ---------------- preferred scale (MINI_SCALE) ---------------- */

static void H_send_output_scale(struct wl_resource *o, int scale)
{
    if (wl_resource_get_version(o) >= WL_OUTPUT_SCALE_SINCE_VERSION) {
        wl_output_send_scale(o, scale);
    }
    if (wl_resource_get_version(o) >= WL_OUTPUT_DONE_SINCE_VERSION) {
        wl_output_send_done(o);
    }
}

static void H_send(int value)
{
    if (strcmp(H_source, "fractional") == 0) {
        if (H_fractional) {
            wp_fractional_scale_v1_send_preferred_scale(H_fractional, (uint32_t)value);
        }
    } else if (strcmp(H_source, "buffer") == 0) {
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
        if (M.surface_res && wl_resource_get_version(M.surface_res) >= WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION) {
            wl_surface_send_preferred_buffer_scale(M.surface_res, value);
        }
#endif
    } else {
        struct wl_resource *o;
        wl_resource_for_each(o, &H_outputs)
        {
            H_send_output_scale(o, value);
        }
    }
    printf("mini: preferred scale %s:%d\n", H_source, value);
    fflush(stdout);
}

static int H_signal_cb(int signal_number, void *data)
{
    (void)signal_number;
    (void)data;
    H_send(H_change);
    return 0;
}

static void H_commit(void)
{
    B_commit_geometry();
    if (!H_pending_set) {
        return;
    }
    H_pending_set = false;
    if (H_pending_w <= 0) {
        return;
    }
    if (H_pending_w % B_scale != 0 || H_pending_h % B_scale != 0) {
        fprintf(stderr, "mini: %dx%d buffer at buffer scale %d (protocol error)\n", H_pending_w, H_pending_h, B_scale);
        exit(7);
    }
    const char *log = getenv("MINI_GEOMETRY_LOG");
    FILE *f = log ? fopen(log, "a") : NULL;
    if (f) {
        fprintf(f, "buffer=%dx%d scale=%d dest=%dx%d\n", H_pending_w, H_pending_h, B_scale, B_dest_w, B_dest_h);
        fclose(f);
    }
    if (!H_entered && strcmp(H_source, "output") == 0) {
        struct wl_resource *o;
        wl_resource_for_each(o, &H_outputs)
        {
            if (wl_resource_get_client(o) == wl_resource_get_client(M.surface_res)) {
                wl_surface_send_enter(M.surface_res, o);
                H_entered = true;
            }
        }
    }
}

static void H_fractional_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}

static void H_fractional_res_destroy(struct wl_resource *res)
{
    if (res == H_fractional) {
        H_fractional = NULL;
    }
}

static const struct wp_fractional_scale_v1_interface H_fractional_impl = {
    .destroy = H_fractional_destroy,
};

static void H_mgr_get(struct wl_client *client, struct wl_resource *res, uint32_t id, struct wl_resource *surface)
{
    struct wl_resource *r = wl_resource_create(client, &wp_fractional_scale_v1_interface, wl_resource_get_version(res), id);
    wl_resource_set_implementation(r, &H_fractional_impl, surface, H_fractional_res_destroy);
    if (!H_fractional) {
        H_fractional = r; /* the window's surface is the first one asking */
        wp_fractional_scale_v1_send_preferred_scale(r, (uint32_t)H_value);
    }
}

static const struct wp_fractional_scale_manager_v1_interface H_mgr_impl = {
    .destroy = H_fractional_destroy,
    .get_fractional_scale = H_mgr_get,
};

static void H_mgr_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wp_fractional_scale_manager_v1_interface, (int)version, id);
    wl_resource_set_implementation(r, &H_mgr_impl, NULL, NULL);
}

static void H_output_release(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    wl_resource_destroy(res);
}

static const struct wl_output_interface H_output_impl = {
    .release = H_output_release,
};

static void H_output_res_destroy(struct wl_resource *res)
{
    wl_list_remove(wl_resource_get_link(res));
}

static void H_output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *o = wl_resource_create(client, &wl_output_interface, (int)version, id);
    wl_resource_set_implementation(o, &H_output_impl, NULL, H_output_res_destroy);
    wl_list_insert(&H_outputs, wl_resource_get_link(o));
    wl_output_send_geometry(o, 0, 0, 300, 200, WL_OUTPUT_SUBPIXEL_UNKNOWN, "mini", "mini", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(o, WL_OUTPUT_MODE_CURRENT, 1920, 1080, 60000);
    H_send_output_scale(o, H_value);
}

/* ---------------- text input mode (MINI_TEXT_INPUT) ---------------- */

static const char *T_log;
static struct wl_resource *T_ti;
static bool T_focused, T_scripted;
static uint32_t T_commits;
static struct
{
    int enabled;
    uint32_t hint, purpose;
    int32_t x, y, w, h;
} T_pending, T_state;

static void T_maybe_focus(void)
{
    if (!T_log || T_focused || !T_keyboard || !T_ti || !M.buffer_attached || !M.surface_res) {
        return;
    }
    T_focused = true;
    struct wl_array keys = { 0, 0, NULL };
    wl_keyboard_send_enter(T_keyboard, wl_display_next_serial(M.display), M.surface_res, &keys);
    zwp_text_input_v3_send_enter(T_ti, M.surface_res);
    printf("mini: sent keyboard + text-input enter\n");
    fflush(stdout);
}

static void T_enable(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    (void)res;
    /* enable resets the pending state (protocol) */
    memset(&T_pending, 0, sizeof(T_pending));
    T_pending.enabled = 1;
}

static void T_disable(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    (void)res;
    T_pending.enabled = 0;
}

static void T_surrounding(struct wl_client *client, struct wl_resource *res, const char *text, int32_t cursor,
                          int32_t anchor)
{
    (void)client;
    (void)res;
    (void)text;
    (void)cursor;
    (void)anchor;
}

static void T_change_cause(struct wl_client *client, struct wl_resource *res, uint32_t cause)
{
    (void)client;
    (void)res;
    (void)cause;
}

static void T_content_type(struct wl_client *client, struct wl_resource *res, uint32_t hint, uint32_t purpose)
{
    (void)client;
    (void)res;
    T_pending.hint = hint;
    T_pending.purpose = purpose;
}

static void T_cursor_rect(struct wl_client *client, struct wl_resource *res, int32_t x, int32_t y, int32_t w,
                          int32_t h)
{
    (void)client;
    (void)res;
    T_pending.x = x;
    T_pending.y = y;
    T_pending.w = w;
    T_pending.h = h;
}

static void T_commit(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    T_commits++;
    if (T_pending.enabled) {
        T_state = T_pending;
    } else {
        memset(&T_state, 0, sizeof(T_state));
    }
    FILE *f = fopen(T_log, "a");
    if (f) {
        fprintf(f, "enabled=%d hint=%u purpose=%u rect=%d,%d,%d,%d\n", T_state.enabled, T_state.hint,
                T_state.purpose, T_state.x, T_state.y, T_state.w, T_state.h);
        fclose(f);
    }
    if (T_state.enabled && !T_scripted) {
        T_scripted = true;
        const char *commit = getenv("MINI_TI_COMMIT") ? getenv("MINI_TI_COMMIT") : "text";
        zwp_text_input_v3_send_preedit_string(res, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", 3, 6);
        zwp_text_input_v3_send_done(res, T_commits);
        zwp_text_input_v3_send_commit_string(res, commit);
        zwp_text_input_v3_send_done(res, T_commits);
    }
}

static void T_ti_destroyed(struct wl_resource *res)
{
    if (res == T_ti) {
        T_ti = NULL;
    }
}

static const struct zwp_text_input_v3_interface T_ti_impl = {
    .destroy = res_destroy,
    .enable = T_enable,
    .disable = T_disable,
    .set_surrounding_text = T_surrounding,
    .set_text_change_cause = T_change_cause,
    .set_content_type = T_content_type,
    .set_cursor_rectangle = T_cursor_rect,
    .commit = T_commit,
};

static void T_get_text_input(struct wl_client *client, struct wl_resource *res, uint32_t id, struct wl_resource *seat)
{
    (void)seat;
    struct wl_resource *t = wl_resource_create(client, &zwp_text_input_v3_interface, wl_resource_get_version(res), id);
    wl_resource_set_implementation(t, &T_ti_impl, NULL, T_ti_destroyed);
    T_ti = t;
    T_maybe_focus();
}

static const struct zwp_text_input_manager_v3_interface T_mgr_impl = {
    .destroy = res_destroy,
    .get_text_input = T_get_text_input,
};

static void T_mgr_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &zwp_text_input_manager_v3_interface, version, id);
    wl_resource_set_implementation(r, &T_mgr_impl, NULL, NULL);
}

/* ---------------- lifecycle ---------------- */

static int watchdog_cb(void *data)
{
    (void)data;
    fprintf(stderr, "mini: watchdog timeout\n");
    exit(2);
}

static void client_destroyed(struct wl_listener *listener, void *data);
static struct wl_listener client_destroy_listener = { .notify = client_destroyed };

static void client_destroyed(struct wl_listener *listener, void *data)
{
    (void)data;
    if (listener != &client_destroy_listener) {
        /* the per-client listener of S_mode (already unlinked by the final
         * destroy-signal emission) */
        free(listener);
    }
    if (S_mode && --S_clients > 0) {
        printf("mini: client disconnected, %d left\n", S_clients);
        fflush(stdout);
        return;
    }
    if (D_data) {
        const bool ok = D_refuse_expected ? (D_dropped && !D_accepted && D_received == 0)
                                          : (D_dropped && D_received > 0 && D_finished);
        printf("mini: dnd %s (dropped=%d accepted=%d received=%d finished=%d)\n", ok ? "ok" : "FAILED", D_dropped,
               D_accepted, D_received, D_finished);
        fflush(stdout);
        exit(ok ? 0 : 6);
    }
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

static void client_connected(struct wl_listener *listener, void *data)
{
    (void)listener;
    struct wl_client *client = data;
    M.client = client;
    if (S_mode) {
        /* one listener struct per client (a wl_listener is a list node) */
        struct wl_listener *l = calloc(1, sizeof(*l));
        l->notify = client_destroyed;
        wl_client_add_destroy_listener(client, l);
        S_clients++;
        return;
    }
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
    /* MINI_SCALE_CHANGE trigger: blocked (signalfd) before the socket exists,
     * so the test can't signal too early */
    if (getenv("MINI_SCALE_CHANGE")) {
        mini_scale_signal_source = wl_event_loop_add_signal(M.loop, SIGUSR1, H_signal_cb, NULL);
    }
    const char *name = getenv("WAYLAND_DISPLAY");
    if (wl_display_add_socket(M.display, name) != 0) {
        fprintf(stderr, "mini: add_socket(%s) failed\n", name ? name : "(default)");
        return 1;
    }

    const char *hs = getenv("MINI_SCALE");
    if (hs && strchr(hs, ':')) {
        H_mode = true;
        snprintf(H_source, sizeof(H_source), "%.*s", (int)(strchr(hs, ':') - hs), hs);
        H_value = atoi(strchr(hs, ':') + 1);
        H_change = getenv("MINI_SCALE_CHANGE") ? atoi(getenv("MINI_SCALE_CHANGE")) : 0;
        wl_list_init(&H_outputs);
        if (strcmp(H_source, "fractional") == 0) {
            wl_global_create(M.display, &wp_fractional_scale_manager_v1_interface, 1, NULL, H_mgr_bind);
        } else if (strcmp(H_source, "output") == 0) {
            wl_global_create(M.display, &wl_output_interface, 2, NULL, H_output_bind);
        }
    }
    int compositor_version = 4;
    if (H_mode && strcmp(H_source, "buffer") == 0) {
        if (wl_compositor_interface.version < 6) {
            fprintf(stderr, "mini: libwayland-server lacks wl_compositor v6\n");
            return 77;
        }
        compositor_version = 6;
    }
    wl_global_create(M.display, &wl_compositor_interface, compositor_version, NULL, compositor_bind);
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

    D_data = getenv("MINI_DND");
    D_refuse_expected = getenv("MINI_DND_REFUSE") && atoi(getenv("MINI_DND_REFUSE"));
    if (D_data) {
        D_timer = wl_event_loop_add_timer(M.loop, D_timer_cb, NULL);
        D_drop_timer = wl_event_loop_add_timer(M.loop, D_drop_cb, NULL);
    }
    S_mode = (getenv("MINI_SELECTION") && atoi(getenv("MINI_SELECTION"))) || D_data;
    if (S_mode) {
        wl_list_init(&S_clip.devices);
        wl_list_init(&S_prim.devices);
        wl_global_create(M.display, &wl_data_device_manager_interface, 3, NULL, S_mgr_bind);
        wl_global_create(M.display, &zwp_primary_selection_device_manager_v1_interface, 1, NULL, S_prim_mgr_bind);
    }

    T_log = getenv("MINI_TEXT_INPUT");
    if (T_log) {
        wl_global_create(M.display, &zwp_text_input_manager_v3_interface, 1, NULL, T_mgr_bind);
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
