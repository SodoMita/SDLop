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
  MINI_DND_REPEAT=<n> writes <data> n times plus a newline, with blocking
  writes (a source larger than the pipe capacity).
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
  MINI_CURSOR_LOG=<log> logs every cursor request for
  tests/test_wayland_cursor.c: "shape=N" (wp_cursor_shape_device_v1, offered
  unless MINI_CURSOR_SHAPE=0) and "cursor=surface|null" (wl_pointer).
  MINI_TOPLEVEL_LOG=<log> offers xdg_toplevel_icon_manager_v1 and
  xdg_wm_dialog_v1 for tests/test_wayland_toplevel.c and logs every
  set_icon as "icon=null" or "icon SIZExSIZE@SCALE:CENTER/CORNER ..."
  (pixels as 8-digit hex), and "dialog", "modal=1|0", "dialog destroyed".
  It also offers wp_alpha_modifier_v1: "alpha=FACTOR" is logged when a
  multiplier is committed, and zxdg_exporter_v2: toplevels get handles
  "mini-N", logged as "export mini-N" and "unexport mini-N". For the newest xdg surface it logs its opaque
  region ("opaque=set|none", also when it becomes the newest) and every
  attached shm buffer ("attach FORMAT pixel=AARRGGBB", the first pixel;
  FORMAT is XRGB or ARGB).
  With MINI_TOPLEVEL_LOG, xdg_toplevel move / resize / show_window_menu
  log "move serial=ok|bad", "resize edge=E serial=ok|bad" and
  "menu X,Y serial=ok|bad" (ok: the serial of the latest button press).
  MINI_POINTER_CMD=<file>: on SIGUSR2 the lines of <file> are run as
  pointer input on the window - "close" (the compositor's title-bar close
  button: xdg_toplevel.close), "motion X Y", "press BTN", "release BTN"
  (linux button codes), "wheel AXIS STEPS120" (a mouse wheel: source,
  value120 for wl_pointer v8+ or discrete for v5-7, axis = 15 units per
  detent), "natural AXIS STEPS120" (the same with relative direction
  inverted), "finger AXIS VALUE" (touchpad: continuous only), each
  followed by a frame. AXIS: 0 vertical, 1 horizontal.
  MINI_SEAT_VERSION=<n> caps the wl_seat global's version.
  MINI_TABLET=1 for tests/test_wayland_pen.c: offers zwp_tablet_manager_v2
  with one tablet and two tools (a pen with pressure+tilt, an eraser);
  MINI_POINTER_CMD lines then also drive the current tool: "tsel pen|eraser",
  "tprox" (proximity_in on the window), "tout", "tmotion X Y",
  "tpressure P" (0..65535), "ttilt X Y", "tdown", "tup", "tbutton CODE 1|0",
  "tremove" (tool removed) and "tframe" - no implicit frames here.
  MINI_GESTURES=<version> for tests/test_wayland_pinch.c: offers
  zwp_pointer_gestures_v1; MINI_POINTER_CMD lines "pinch_begin FINGERS",
  "pinch_update SCALE" and "pinch_end [1 = cancelled]" drive the pinch
  object of the pointer (begin targets the window surface).
  MINI_WARP_LOG=<log> for tests/test_wayland_warp.c: offers
  wp_pointer_warp_v1 (unless MINI_POINTER_WARP=0) and logs
  "warp X,Y serial=ok|bad", lock_pointer requests log "lock" and cursor
  position hints "hint X,Y" (the lock script below is off).

  MINI_RESIZE_LOG=<log> for tests/test_wayland_resize.c: frame callbacks
  of the window surface are held until its next commit (like a compositor
  that presents each commit), and the log gets "pool SIZE" per wl_shm_pool,
  "buffer WxH" per wl_buffer, "commit WxH" per commit of the window surface
  with a new buffer, and "ack WxH resizing=0|1" per ack_configure (the size
  of the acked configure). MINI_POINTER_CMD lines (no pointer needed):
  "rburst N W H DW DH" sends N toplevel configures in the resizing state,
  sizes W+i*DW x H+i*DH, and "rend W H" one without it; each logs
  "configure WxH resizing=0|1".

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
#include "cursor-shape-v1-server-protocol.h"
#include "xdg-toplevel-icon-v1-server-protocol.h"
#include "xdg-dialog-v1-server-protocol.h"
#include "alpha-modifier-v1-server-protocol.h"
#include "xdg-foreign-unstable-v2-server-protocol.h"
#include "pointer-warp-v1-server-protocol.h"
#include "tablet-v2-server-protocol.h"
#include "pointer-gestures-unstable-v1-server-protocol.h"

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
    struct wl_resource *keyboard;
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
    uint32_t format;
    bool held;
    bool current;
    uint64_t sum;
} mini_buffer;

static bool B_mode;
static const char *RZ_log; /* MINI_RESIZE_LOG */
static const char *TL_log; /* MINI_TOPLEVEL_LOG: icon buffers are read too */
static struct wl_resource *TL_alpha_surface; /* surface with a pending multiplier */
static uint32_t TL_alpha_pending;
static void TL_logs(const char *line);
static const char *W_log; /* MINI_WARP_LOG */
static uint32_t W_enter_serial;
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
struct wl_event_source *mini_pointer_cmd_source; /* MINI_POINTER_CMD */
static const char *P_cmd_path;
static uint32_t P_button_serial; /* latest button press */
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

/* ---------------- MINI_RESIZE_LOG ---------------- */

static void RZ_logs(const char *line)
{
    FILE *f = RZ_log ? fopen(RZ_log, "a") : NULL;
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

/* configures sent (serial -> size), frame callbacks waiting for a commit */
static struct
{
    uint32_t serial;
    int w, h;
    bool resizing;
} RZ_cfg[512];
static int RZ_ncfg;
static struct wl_resource *RZ_frames[64];
static int RZ_nframes;
static int RZ_attach_w, RZ_attach_h;
static bool RZ_attached;

static void RZ_frame_gone(struct wl_resource *res)
{
    for (int i = 0; i < RZ_nframes; i++) {
        if (RZ_frames[i] == res) {
            RZ_frames[i] = RZ_frames[--RZ_nframes];
            return;
        }
    }
}

/* the commit is "presented": its frame callbacks are done */
static void RZ_commit(void)
{
    if (RZ_attached) {
        char line[48];
        snprintf(line, sizeof(line), "commit %dx%d", RZ_attach_w, RZ_attach_h);
        RZ_logs(line);
        RZ_attached = false;
    }
    while (RZ_nframes > 0) {
        struct wl_resource *cb = RZ_frames[0];
        wl_callback_send_done(cb, 0);
        wl_resource_destroy(cb); /* RZ_frame_gone removes it */
    }
}

static void RZ_ack(uint32_t serial)
{
    for (int i = RZ_ncfg - 1; i >= 0; i--) {
        if (RZ_cfg[i].serial == serial) {
            char line[64];
            snprintf(line, sizeof(line), "ack %dx%d resizing=%d", RZ_cfg[i].w, RZ_cfg[i].h, RZ_cfg[i].resizing);
            RZ_logs(line);
            return;
        }
    }
    RZ_logs("ack other");
}

static void RZ_configure(int w, int h, bool resizing)
{
    if (!M.xdg_toplevel || !M.xdg_surface || RZ_ncfg >= (int)(sizeof(RZ_cfg) / sizeof(RZ_cfg[0]))) {
        return;
    }
    struct wl_array states;
    wl_array_init(&states);
    uint32_t *st = wl_array_add(&states, sizeof(uint32_t));
    *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    if (resizing) {
        st = wl_array_add(&states, sizeof(uint32_t));
        *st = XDG_TOPLEVEL_STATE_RESIZING;
    }
    xdg_toplevel_send_configure(M.xdg_toplevel, w, h, &states);
    wl_array_release(&states);
    const uint32_t serial = wl_display_next_serial(M.display);
    RZ_cfg[RZ_ncfg].serial = serial;
    RZ_cfg[RZ_ncfg].w = w;
    RZ_cfg[RZ_ncfg].h = h;
    RZ_cfg[RZ_ncfg].resizing = resizing;
    RZ_ncfg++;
    xdg_surface_send_configure(M.xdg_surface, serial);
    char line[64];
    snprintf(line, sizeof(line), "configure %dx%d resizing=%d", w, h, resizing);
    RZ_logs(line);
}

/* one MINI_POINTER_CMD line for MINI_RESIZE_LOG; false if it isn't one */
static bool RZ_command(const char *line)
{
    int n, w, h, dw, dh;
    if (!RZ_log) {
        return false;
    }
    if (sscanf(line, "rburst %d %d %d %d %d", &n, &w, &h, &dw, &dh) == 5) {
        for (int i = 0; i < n; i++) {
            RZ_configure(w + i * dw, h + i * dh, true);
        }
        return true;
    }
    if (sscanf(line, "rend %d %d", &w, &h) == 2) {
        RZ_configure(w, h, false);
        return true;
    }
    return false;
}

static void xdg_surface_ack(struct wl_client *client, struct wl_resource *res, uint32_t serial)
{
    (void)client;
    (void)res;
    if (RZ_log) {
        RZ_ack(serial);
    }
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
    if ((B_mode || H_mode || TL_log || RZ_log) && p) {
        b = calloc(1, sizeof(*b));
        b->pool = p;
        p->refs++;
        b->offset = offset;
        b->w = width;
        b->h = height;
        b->stride = stride;
        b->format = format;
    }
    wl_resource_set_implementation(buf, &buffer_impl, b, buffer_res_destroy);
    if (RZ_log) {
        char line[48];
        snprintf(line, sizeof(line), "buffer %dx%d", width, height);
        RZ_logs(line);
    }
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
    if (RZ_log) {
        char line[48];
        snprintf(line, sizeof(line), "pool %d", size);
        RZ_logs(line);
    }
    if (B_mode || H_mode || TL_log || RZ_log) {
        p = calloc(1, sizeof(*p));
        p->refs = 1;
        p->size = (size_t)size;
    }
    if (B_mode || TL_log) { /* H_mode only needs buffer sizes */
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

static void TL_logs(const char *line);

/* opaque region per surface (MINI_TOPLEVEL_LOG) */
static struct
{
    struct wl_resource *surface;
    bool opaque;
} TL_opaque[64];

#define TL_NUM_OPAQUE (int)(sizeof(TL_opaque) / sizeof(TL_opaque[0]))

/* slot of surface (or a new one with create), -1 if none */
static int TL_opaque_slot(struct wl_resource *surface, bool create)
{
    for (int i = 0; i < TL_NUM_OPAQUE; i++) {
        if (TL_opaque[i].surface == surface) {
            return i;
        }
    }
    for (int i = 0; create && i < TL_NUM_OPAQUE; i++) {
        if (!TL_opaque[i].surface) {
            TL_opaque[i].surface = surface;
            TL_opaque[i].opaque = false;
            return i;
        }
    }
    return -1;
}

static void TL_log_opaque(struct wl_resource *surface)
{
    const int i = TL_opaque_slot(surface, false);
    TL_logs(i >= 0 && TL_opaque[i].opaque ? "opaque=set" : "opaque=none");
}

static void surface_set_opaque_region(struct wl_client *client, struct wl_resource *res, struct wl_resource *region)
{
    (void)client;
    const int i = TL_log ? TL_opaque_slot(res, true) : -1;
    if (i >= 0) {
        TL_opaque[i].opaque = region != NULL;
        if (res == M.surface_res) {
            TL_log_opaque(res);
        }
    }
}

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
    if (RZ_log && res == M.surface_res) {
        mini_buffer *rb = buffer ? wl_resource_get_user_data(buffer) : NULL;
        RZ_attach_w = rb ? rb->w : 0;
        RZ_attach_h = rb ? rb->h : 0;
        RZ_attached = true;
    }
    if (TL_log && res == M.surface_res && buffer) {
        mini_buffer *tb = wl_resource_get_user_data(buffer);
        if (tb && tb->pool && tb->pool->map && (size_t)tb->offset + 4 <= tb->pool->size) {
            uint32_t px;
            memcpy(&px, (const uint8_t *)tb->pool->map + tb->offset, 4);
            char line[64];
            snprintf(line, sizeof(line), "attach %s pixel=%08x",
                     tb->format == WL_SHM_FORMAT_ARGB8888 ? "ARGB"
                     : tb->format == WL_SHM_FORMAT_XRGB8888 ? "XRGB"
                                                            : "other",
                     px);
            TL_logs(line);
        }
    }
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


/* wl_pointer.frame exists since v5: older pointers get none (each event
 * stands alone), like a real v4 compositor */
static void mini_pointer_frame(void)
{
    if (M.pointer && wl_resource_get_version(M.pointer) >= WL_POINTER_FRAME_SINCE_VERSION) {
        wl_pointer_send_frame(M.pointer);
    }
}

static void surface_commit(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    (void)res;

    if (H_mode && res == M.surface_res) {
        H_commit();
    }
    if (RZ_log && res == M.surface_res) {
        RZ_commit();
    }
    if (TL_alpha_surface && res == TL_alpha_surface) {
        char line[32];
        snprintf(line, sizeof(line), "alpha=%u", TL_alpha_pending);
        TL_logs(line);
        TL_alpha_surface = NULL;
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
        W_enter_serial = serial;
        wl_pointer_send_enter(M.pointer, serial, M.surface_res,
                              wl_fixed_from_double(100.0), wl_fixed_from_double(100.0));
        mini_pointer_frame();
        if (getenv("SDLOP_TEST_SCROLL")) {
            /* three frames, one wheel event each; the seat version decides
             * which axis flavour carries the step */
            wl_pointer_send_axis(M.pointer, 1, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(-1));
            if (wl_resource_get_version(M.pointer) >= 8) {
                wl_pointer_send_axis_value120(M.pointer, WL_POINTER_AXIS_VERTICAL_SCROLL, -120);
            } else if (wl_resource_get_version(M.pointer) >= 5) {
                wl_pointer_send_axis_discrete(M.pointer, WL_POINTER_AXIS_VERTICAL_SCROLL, -1);
            }
            mini_pointer_frame();
            wl_pointer_send_axis(M.pointer, 2, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_int(1));
            if (wl_resource_get_version(M.pointer) >= 8) {
                wl_pointer_send_axis_value120(M.pointer, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 60);
            } else if (wl_resource_get_version(M.pointer) >= 5) {
                wl_pointer_send_axis_discrete(M.pointer, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 2);
            }
            mini_pointer_frame();
            wl_pointer_send_axis(M.pointer, 3, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(-5));
            mini_pointer_frame();
        }
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
    if (RZ_log && res == M.surface_res && RZ_nframes < (int)(sizeof(RZ_frames) / sizeof(RZ_frames[0]))) {
        /* done with the next commit (it is "presented" right away) */
        wl_resource_set_implementation(cb, NULL, NULL, RZ_frame_gone);
        RZ_frames[RZ_nframes++] = cb;
        return;
    }
    wl_callback_send_done(cb, 0);
    wl_resource_destroy(cb);
}

static void surface_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    const int i = TL_opaque_slot(res, false);
    if (i >= 0) {
        TL_opaque[i].surface = NULL;
    }
    wl_resource_destroy(res);
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = (void *)noop,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
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

static const char *serial_ok(uint32_t serial)
{
    return serial && serial == P_button_serial ? "ok" : "bad";
}

static void toplevel_move(struct wl_client *client, struct wl_resource *res, struct wl_resource *seat, uint32_t serial)
{
    (void)client;
    (void)res;
    (void)seat;
    char line[64];
    snprintf(line, sizeof(line), "move serial=%s", serial_ok(serial));
    TL_logs(line);
}

static void toplevel_resize(struct wl_client *client, struct wl_resource *res, struct wl_resource *seat,
                            uint32_t serial, uint32_t edges)
{
    (void)client;
    (void)res;
    (void)seat;
    char line[64];
    snprintf(line, sizeof(line), "resize edge=%u serial=%s", edges, serial_ok(serial));
    TL_logs(line);
}

static void toplevel_menu(struct wl_client *client, struct wl_resource *res, struct wl_resource *seat,
                          uint32_t serial, int32_t x, int32_t y)
{
    (void)client;
    (void)res;
    (void)seat;
    char line[64];
    snprintf(line, sizeof(line), "menu %d,%d serial=%s", x, y, serial_ok(serial));
    TL_logs(line);
}

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = xdg_toplevel_destroy,
    .set_parent = (void *)noop,
    .set_title = (void *)noop,
    .set_app_id = (void *)noop,
    .show_window_menu = toplevel_menu,
    .move = toplevel_move,
    .resize = toplevel_resize,
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
    M.configure_sent = false; /* every new toplevel (re-shown windows) gets its initial configure */
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = NULL,
    .set_window_geometry = (void *)noop,
    .ack_configure = xdg_surface_ack,
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
    if (TL_log) {
        TL_log_opaque(surface);
    }
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
        mini_pointer_frame();
    }
    return 1; /* one shot */
}

static int abs_timer_cb(void *data)
{
    (void)data;
    if (M.pointer && M.surface_res) {
        wl_pointer_send_motion(M.pointer, 999,
                               wl_fixed_from_double(600.0), wl_fixed_from_double(450.0));
        mini_pointer_frame();
        printf("mini: sent absolute motion(600, 450) after unlock\n");
        fflush(stdout);
    }
    return 1;
}

static void W_logf(const char *fmt, double x, double y, const char *s)
{
    FILE *f = W_log ? fopen(W_log, "a") : NULL;
    if (f) {
        fprintf(f, fmt, x, y, s);
        fputc('\n', f);
        fclose(f);
    }
}

static void W_hint(struct wl_client *client, struct wl_resource *res, wl_fixed_t x, wl_fixed_t y)
{
    (void)client;
    (void)res;
    W_logf("hint %g,%g%s", wl_fixed_to_double(x), wl_fixed_to_double(y), "");
}

static void W_warp(struct wl_client *client, struct wl_resource *res, struct wl_resource *surface,
                   struct wl_resource *pointer, wl_fixed_t x, wl_fixed_t y, uint32_t serial)
{
    (void)client;
    (void)res;
    (void)surface;
    (void)pointer;
    W_logf("warp %g,%g serial=%s", wl_fixed_to_double(x), wl_fixed_to_double(y),
           serial == W_enter_serial ? "ok" : "bad");
}

static const struct wp_pointer_warp_v1_interface W_impl = {
    .destroy = res_destroy,
    .warp_pointer = W_warp,
};

static void W_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wp_pointer_warp_v1_interface, version, id);
    wl_resource_set_implementation(r, &W_impl, NULL, NULL);
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
    .set_cursor_position_hint = W_hint,
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
    W_logf("lock", 0, 0, NULL);
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

static const char *C_log; /* MINI_CURSOR_LOG */

static void C_logf(const char *fmt, unsigned v, const char *s)
{
    FILE *f = C_log ? fopen(C_log, "a") : NULL;
    if (f) {
        if (s) {
            fprintf(f, fmt, s);
        } else {
            fprintf(f, fmt, v);
        }
        fputc('\n', f);
        fclose(f);
    }
}

static void pointer_set_cursor(struct wl_client *client, struct wl_resource *res, uint32_t serial,
                               struct wl_resource *surface, int32_t hx, int32_t hy)
{
    (void)client;
    (void)res;
    (void)serial;
    (void)hx;
    (void)hy;
    C_logf("cursor=%s", 0, surface ? "surface" : "null");
}

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor,
    .release = res_destroy,
};

static void C_set_shape(struct wl_client *client, struct wl_resource *res, uint32_t serial, uint32_t shape)
{
    (void)client;
    (void)res;
    (void)serial;
    C_logf("shape=%u", shape, NULL);
}

static const struct wp_cursor_shape_device_v1_interface C_device_impl = {
    .destroy = res_destroy,
    .set_shape = C_set_shape,
};

static void C_get_device(struct wl_client *client, struct wl_resource *res, uint32_t id, struct wl_resource *obj)
{
    (void)obj;
    struct wl_resource *d = wl_resource_create(client, &wp_cursor_shape_device_v1_interface,
                                               wl_resource_get_version(res), id);
    wl_resource_set_implementation(d, &C_device_impl, NULL, NULL);
}

static const struct wp_cursor_shape_manager_v1_interface C_mgr_impl = {
    .destroy = res_destroy,
    .get_pointer = C_get_device,
    .get_tablet_tool_v2 = C_get_device,
};

static void C_mgr_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wp_cursor_shape_manager_v1_interface, version, id);
    wl_resource_set_implementation(r, &C_mgr_impl, NULL, NULL);
}

/* ---------------- zwp_tablet_manager_v2 (MINI_TABLET) ---------------- */

static struct wl_resource *TB_tools[2]; /* pen, eraser */
static struct wl_resource *TB_tablet;
static int TB_cur;

static void TB_tool_set_cursor(struct wl_client *client, struct wl_resource *res, uint32_t serial,
                               struct wl_resource *surface, int32_t hx, int32_t hy)
{
    (void)client;
    (void)res;
    (void)serial;
    (void)surface;
    (void)hx;
    (void)hy;
}

static void TB_tool_gone(struct wl_resource *res)
{
    for (int i = 0; i < 2; i++) {
        if (TB_tools[i] == res) {
            TB_tools[i] = NULL;
        }
    }
}

static const struct zwp_tablet_tool_v2_interface TB_tool_impl = {
    .set_cursor = TB_tool_set_cursor,
    .destroy = res_destroy,
};

static void TB_tablet_gone(struct wl_resource *res)
{
    if (TB_tablet == res) {
        TB_tablet = NULL;
    }
}

static const struct zwp_tablet_v2_interface TB_tablet_impl = {
    .destroy = res_destroy,
};

static const struct zwp_tablet_seat_v2_interface TB_seat_impl = {
    .destroy = res_destroy,
};

static void TB_get_tablet_seat(struct wl_client *client, struct wl_resource *res, uint32_t id,
                               struct wl_resource *seat)
{
    (void)seat;
    struct wl_resource *ts = wl_resource_create(client, &zwp_tablet_seat_v2_interface, wl_resource_get_version(res), id);
    wl_resource_set_implementation(ts, &TB_seat_impl, NULL, NULL);
    struct wl_resource *tab = wl_resource_create(client, &zwp_tablet_v2_interface, wl_resource_get_version(res), 0);
    wl_resource_set_implementation(tab, &TB_tablet_impl, NULL, TB_tablet_gone);
    TB_tablet = tab;
    zwp_tablet_seat_v2_send_tablet_added(ts, tab);
    zwp_tablet_v2_send_name(tab, "mini tablet");
    zwp_tablet_v2_send_id(tab, 0x56a, 0x1);
    zwp_tablet_v2_send_done(tab);
    for (int i = 0; i < 2; i++) {
        struct wl_resource *tool =
            wl_resource_create(client, &zwp_tablet_tool_v2_interface, wl_resource_get_version(res), 0);
        wl_resource_set_implementation(tool, &TB_tool_impl, NULL, TB_tool_gone);
        zwp_tablet_seat_v2_send_tool_added(ts, tool);
        zwp_tablet_tool_v2_send_type(tool, i == 0 ? ZWP_TABLET_TOOL_V2_TYPE_PEN : ZWP_TABLET_TOOL_V2_TYPE_ERASER);
        zwp_tablet_tool_v2_send_hardware_id_wacom(tool, 0, 0x802 + (uint32_t)i);
        zwp_tablet_tool_v2_send_capability(tool, ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE);
        if (i == 0) {
            zwp_tablet_tool_v2_send_capability(tool, ZWP_TABLET_TOOL_V2_CAPABILITY_TILT);
        }
        zwp_tablet_tool_v2_send_done(tool);
        TB_tools[i] = tool;
    }
}

static const struct zwp_tablet_manager_v2_interface TB_mgr_impl = {
    .get_tablet_seat = TB_get_tablet_seat,
    .destroy = res_destroy,
};

static void TB_mgr_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &zwp_tablet_manager_v2_interface, version, id);
    wl_resource_set_implementation(r, &TB_mgr_impl, NULL, NULL);
}

/* one MINI_POINTER_CMD line for the tablet; false if it isn't one */
static bool TB_command(const char *line)
{
    if (strncmp(line, "tsel ", 5) == 0) {
        TB_cur = strncmp(line + 5, "eraser", 6) == 0 ? 1 : 0;
        return true;
    }
    if (line[0] != 't' || strncmp(line, "tsel", 4) == 0) {
        return false;
    }
    struct wl_resource *tool = TB_tools[TB_cur];
    double x, y;
    unsigned v, st;
    if (!tool) {
        return true;
    }
    if (strncmp(line, "tprox", 5) == 0) {
        if (M.surface_res && TB_tablet) {
            zwp_tablet_tool_v2_send_proximity_in(tool, wl_display_next_serial(M.display), TB_tablet, M.surface_res);
        }
    } else if (strncmp(line, "tout", 4) == 0) {
        zwp_tablet_tool_v2_send_proximity_out(tool);
    } else if (sscanf(line, "tmotion %lf %lf", &x, &y) == 2) {
        zwp_tablet_tool_v2_send_motion(tool, wl_fixed_from_double(x), wl_fixed_from_double(y));
    } else if (sscanf(line, "tpressure %u", &v) == 1) {
        zwp_tablet_tool_v2_send_pressure(tool, v);
    } else if (sscanf(line, "ttilt %lf %lf", &x, &y) == 2) {
        zwp_tablet_tool_v2_send_tilt(tool, wl_fixed_from_double(x), wl_fixed_from_double(y));
    } else if (strncmp(line, "tdown", 5) == 0) {
        zwp_tablet_tool_v2_send_down(tool, wl_display_next_serial(M.display));
    } else if (strncmp(line, "tup", 3) == 0) {
        zwp_tablet_tool_v2_send_up(tool);
    } else if (sscanf(line, "tbutton %u %u", &v, &st) == 2) {
        zwp_tablet_tool_v2_send_button(tool, wl_display_next_serial(M.display), v,
                                       st ? ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED
                                          : ZWP_TABLET_TOOL_V2_BUTTON_STATE_RELEASED);
    } else if (strncmp(line, "tremove", 7) == 0) {
        zwp_tablet_tool_v2_send_removed(tool);
    } else if (strncmp(line, "tframe", 6) == 0) {
        zwp_tablet_tool_v2_send_frame(tool, 1000);
    }
    return true;
}

/* ---------------- zwp_pointer_gestures_v1 (MINI_GESTURES) ---------------- */

static struct wl_resource *GS_pinch;

static void GS_pinch_gone(struct wl_resource *res)
{
    if (GS_pinch == res) {
        GS_pinch = NULL;
    }
}

static const struct zwp_pointer_gesture_pinch_v1_interface GS_pinch_impl = {
    .destroy = res_destroy,
};
static const struct zwp_pointer_gesture_swipe_v1_interface GS_swipe_impl = {
    .destroy = res_destroy,
};
static const struct zwp_pointer_gesture_hold_v1_interface GS_hold_impl = {
    .destroy = res_destroy,
};

static void GS_get_swipe(struct wl_client *client, struct wl_resource *res, uint32_t id, struct wl_resource *pointer)
{
    (void)pointer;
    struct wl_resource *r = wl_resource_create(client, &zwp_pointer_gesture_swipe_v1_interface,
                                               wl_resource_get_version(res), id);
    wl_resource_set_implementation(r, &GS_swipe_impl, NULL, NULL);
}

static void GS_get_pinch(struct wl_client *client, struct wl_resource *res, uint32_t id, struct wl_resource *pointer)
{
    (void)pointer;
    struct wl_resource *r = wl_resource_create(client, &zwp_pointer_gesture_pinch_v1_interface,
                                               wl_resource_get_version(res), id);
    wl_resource_set_implementation(r, &GS_pinch_impl, NULL, GS_pinch_gone);
    GS_pinch = r;
}

static void GS_get_hold(struct wl_client *client, struct wl_resource *res, uint32_t id, struct wl_resource *pointer)
{
    (void)pointer;
    struct wl_resource *r = wl_resource_create(client, &zwp_pointer_gesture_hold_v1_interface,
                                               wl_resource_get_version(res), id);
    wl_resource_set_implementation(r, &GS_hold_impl, NULL, NULL);
}

static const struct zwp_pointer_gestures_v1_interface GS_impl = {
    .get_swipe_gesture = GS_get_swipe,
    .get_pinch_gesture = GS_get_pinch,
    .release = res_destroy,
    .get_hold_gesture = GS_get_hold,
};

static void GS_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &zwp_pointer_gestures_v1_interface, version, id);
    wl_resource_set_implementation(r, &GS_impl, NULL, NULL);
}

/* one MINI_POINTER_CMD line for the pinch; false if it isn't one */
static bool GS_command(const char *line)
{
    unsigned n;
    double scale;
    if (strncmp(line, "pinch_", 6) != 0) {
        return false;
    }
    if (!GS_pinch) {
        return true;
    }
    const uint32_t t = 2000;
    if (sscanf(line, "pinch_begin %u", &n) == 1) {
        if (M.surface_res) {
            zwp_pointer_gesture_pinch_v1_send_begin(GS_pinch, wl_display_next_serial(M.display), t, M.surface_res, n);
        }
    } else if (sscanf(line, "pinch_update %lf", &scale) == 1) {
        zwp_pointer_gesture_pinch_v1_send_update(GS_pinch, t, 0, 0, wl_fixed_from_double(scale), 0);
    } else if (strncmp(line, "pinch_end", 9) == 0) {
        zwp_pointer_gesture_pinch_v1_send_end(GS_pinch, wl_display_next_serial(M.display), t, atoi(line + 9) ? 1 : 0);
    }
    return true;
}

/* ---------------- xdg_toplevel_icon_manager_v1 (MINI_TOPLEVEL_LOG) ---------------- */

static void TL_logs(const char *line)
{
    FILE *f = TL_log ? fopen(TL_log, "a") : NULL;
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

typedef struct
{
    char desc[512]; /* " WxH@S:center/corner" per buffer */
} TL_Icon;

static void TL_icon_free(struct wl_resource *res)
{
    free(wl_resource_get_user_data(res));
}

static void TL_icon_set_name(struct wl_client *client, struct wl_resource *res, const char *name)
{
    (void)client;
    TL_Icon *icon = wl_resource_get_user_data(res);
    const size_t n = strlen(icon->desc);
    snprintf(icon->desc + n, sizeof(icon->desc) - n, " name=%s", name);
}

static void TL_icon_add_buffer(struct wl_client *client, struct wl_resource *res, struct wl_resource *buffer,
                               int32_t scale)
{
    (void)client;
    TL_Icon *icon = wl_resource_get_user_data(res);
    const mini_buffer *b = wl_resource_get_user_data(buffer);
    if (!b || !b->pool || !b->pool->map) {
        return;
    }
    const int w = b->w, h = b->h;
    const uint8_t *px = (const uint8_t *)b->pool->map + b->offset;
    const uint32_t center = *(const uint32_t *)(px + (size_t)(h / 2) * (size_t)b->stride + (size_t)(w / 2) * 4);
    const uint32_t corner = *(const uint32_t *)px;
    const size_t n = strlen(icon->desc);
    snprintf(icon->desc + n, sizeof(icon->desc) - n, " %dx%d@%d:%08x/%08x", w, h, scale, center, corner);
}

static const struct xdg_toplevel_icon_v1_interface TL_icon_impl = {
    .destroy = res_destroy,
    .set_name = TL_icon_set_name,
    .add_buffer = TL_icon_add_buffer,
};

static void TL_create_icon(struct wl_client *client, struct wl_resource *res, uint32_t id)
{
    TL_Icon *icon = calloc(1, sizeof(*icon));
    struct wl_resource *r = wl_resource_create(client, &xdg_toplevel_icon_v1_interface, wl_resource_get_version(res), id);
    wl_resource_set_implementation(r, &TL_icon_impl, icon, TL_icon_free);
}

static void TL_set_icon(struct wl_client *client, struct wl_resource *res, struct wl_resource *toplevel,
                        struct wl_resource *icon)
{
    (void)client;
    (void)res;
    (void)toplevel;
    char line[600];
    if (icon) {
        snprintf(line, sizeof(line), "icon%s", ((TL_Icon *)wl_resource_get_user_data(icon))->desc);
    } else {
        snprintf(line, sizeof(line), "icon=null");
    }
    TL_logs(line);
}

static const struct xdg_toplevel_icon_manager_v1_interface TL_icon_mgr_impl = {
    .destroy = res_destroy,
    .create_icon = TL_create_icon,
    .set_icon = TL_set_icon,
};

static void TL_icon_mgr_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &xdg_toplevel_icon_manager_v1_interface, version, id);
    wl_resource_set_implementation(r, &TL_icon_mgr_impl, NULL, NULL);
    xdg_toplevel_icon_manager_v1_send_icon_size(r, 48);
    xdg_toplevel_icon_manager_v1_send_done(r);
}

static void TL_dialog_destroy(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    TL_logs("dialog destroyed");
    wl_resource_destroy(res);
}

static void TL_dialog_set_modal(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    (void)res;
    TL_logs("modal=1");
}

static void TL_dialog_unset_modal(struct wl_client *client, struct wl_resource *res)
{
    (void)client;
    (void)res;
    TL_logs("modal=0");
}

static const struct xdg_dialog_v1_interface TL_dialog_impl = {
    .destroy = TL_dialog_destroy,
    .set_modal = TL_dialog_set_modal,
    .unset_modal = TL_dialog_unset_modal,
};

static void TL_get_dialog(struct wl_client *client, struct wl_resource *res, uint32_t id, struct wl_resource *toplevel)
{
    (void)toplevel;
    struct wl_resource *r = wl_resource_create(client, &xdg_dialog_v1_interface, wl_resource_get_version(res), id);
    wl_resource_set_implementation(r, &TL_dialog_impl, NULL, NULL);
    TL_logs("dialog");
}

static const struct xdg_wm_dialog_v1_interface TL_dialog_mgr_impl = {
    .destroy = res_destroy,
    .get_xdg_dialog = TL_get_dialog,
};

static void TL_dialog_mgr_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &xdg_wm_dialog_v1_interface, version, id);
    wl_resource_set_implementation(r, &TL_dialog_mgr_impl, NULL, NULL);
}

static void TL_alpha_set(struct wl_client *client, struct wl_resource *res, uint32_t factor)
{
    (void)client;
    TL_alpha_surface = wl_resource_get_user_data(res);
    TL_alpha_pending = factor;
}

static const struct wp_alpha_modifier_surface_v1_interface TL_alpha_impl = {
    .destroy = res_destroy,
    .set_multiplier = TL_alpha_set,
};

static void TL_alpha_get_surface(struct wl_client *client, struct wl_resource *res, uint32_t id,
                                 struct wl_resource *surface)
{
    struct wl_resource *r =
        wl_resource_create(client, &wp_alpha_modifier_surface_v1_interface, wl_resource_get_version(res), id);
    wl_resource_set_implementation(r, &TL_alpha_impl, surface, NULL);
}

static const struct wp_alpha_modifier_v1_interface TL_alpha_mgr_impl = {
    .destroy = res_destroy,
    .get_surface = TL_alpha_get_surface,
};

static void TL_alpha_mgr_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wp_alpha_modifier_v1_interface, version, id);
    wl_resource_set_implementation(r, &TL_alpha_mgr_impl, NULL, NULL);
}

/* zxdg_exporter_v2 (MINI_TOPLEVEL_LOG): handles "mini-N", logged as
 * "export mini-N" / "unexport mini-N" */
static int TL_nexports;

static const struct zxdg_exported_v2_interface TL_exported_impl = {
    .destroy = res_destroy,
};

static void TL_exported_destroyed(struct wl_resource *res)
{
    char line[48];
    snprintf(line, sizeof(line), "unexport mini-%d", (int)(intptr_t)wl_resource_get_user_data(res));
    TL_logs(line);
}

static void TL_export_toplevel(struct wl_client *client, struct wl_resource *res, uint32_t id,
                               struct wl_resource *surface)
{
    (void)surface;
    struct wl_resource *r = wl_resource_create(client, &zxdg_exported_v2_interface, wl_resource_get_version(res), id);
    const int n = ++TL_nexports;
    wl_resource_set_implementation(r, &TL_exported_impl, (void *)(intptr_t)n, TL_exported_destroyed);
    char handle[32], line[48];
    snprintf(handle, sizeof(handle), "mini-%d", n);
    snprintf(line, sizeof(line), "export %s", handle);
    TL_logs(line);
    zxdg_exported_v2_send_handle(r, handle);
}

static const struct zxdg_exporter_v2_interface TL_exporter_impl = {
    .destroy = res_destroy,
    .export_toplevel = TL_export_toplevel,
};

static void TL_exporter_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &zxdg_exporter_v2_interface, version, id);
    wl_resource_set_implementation(r, &TL_exporter_impl, NULL, NULL);
}

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

/* regions (opaque / input): accepted, contents not tracked */
static void region_rect(struct wl_client *client, struct wl_resource *res, int32_t x, int32_t y, int32_t w,
                        int32_t h)
{
    (void)client;
    (void)res;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

static const struct wl_region_interface region_impl = {
    .destroy = res_destroy,
    .add = region_rect,
    .subtract = region_rect,
};

static void compositor_create_region(struct wl_client *client, struct wl_resource *res, uint32_t id)
{
    (void)res;
    struct wl_resource *r = wl_resource_create(client, &wl_region_interface, 1, id);
    wl_resource_set_implementation(r, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
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
static int D_repeat = 1; /* MINI_DND_REPEAT: D_data repeated, then "\n" */
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
    /* a plain blocking writer, like most sources: a short write or EAGAIN
     * (a receiver that made the pipe nonblocking) truncates the transfer */
    size_t len = strlen(D_data), off = 0, total = 0;
    for (int r = 0; r < D_repeat; r++) {
        off = 0;
        while (off < len) {
            ssize_t n = write(fd, D_data + off, len - off);
            if (n <= 0) {
                break;
            }
            off += (size_t)n;
        }
        total += off;
        if (off < len) {
            break;
        }
    }
    if (D_repeat > 1 && off == len && write(fd, "\n", 1) == 1) {
        total++;
    }
    off = total;
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

static int P_signal_cb(int signal_number, void *data)
{
    (void)signal_number;
    (void)data;
    FILE *f = P_cmd_path ? fopen(P_cmd_path, "r") : NULL;
    if (!f) {
        return 0;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        double x, y;
        unsigned btn;
        if (TB_command(line) || GS_command(line) || RZ_command(line)) {
            continue;
        }
        /* the compositor's close button: no pointer needed */
        if (strncmp(line, "close", 5) == 0 && (line[5] == '\n' || line[5] == '\r' || line[5] == '\0')) {
            if (M.xdg_toplevel) {
                xdg_toplevel_send_close(M.xdg_toplevel);
            }
            continue;
        }
        if (!M.pointer) {
            break;
        }
        int axis, steps;
        double value;
        const int pv = wl_resource_get_version(M.pointer);
        if (sscanf(line, "wheel %d %d", &axis, &steps) == 2) {
            /* a mouse wheel like libinput reports it: steps in 1/120ths of
             * a detent, 15 units of continuous value per detent */
            if (pv >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION) {
                wl_pointer_send_axis_source(M.pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
            }
            if (pv >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION) {
                wl_pointer_send_axis_value120(M.pointer, (uint32_t)axis, steps);
            } else if (pv >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION && steps % 120 == 0) {
                wl_pointer_send_axis_discrete(M.pointer, (uint32_t)axis, steps / 120);
            }
            wl_pointer_send_axis(M.pointer, 1000, (uint32_t)axis, wl_fixed_from_double(steps / 120.0 * 15.0));
        } else if (sscanf(line, "natural %d %d", &axis, &steps) == 2) { /* wheel, inverted relative direction */
            if (pv >= WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION) {
                wl_pointer_send_axis_relative_direction(M.pointer, (uint32_t)axis,
                                                        WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED);
            }
            if (pv >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION) {
                wl_pointer_send_axis_value120(M.pointer, (uint32_t)axis, steps);
            }
            wl_pointer_send_axis(M.pointer, 1000, (uint32_t)axis, wl_fixed_from_double(steps / 120.0 * 15.0));
        } else if (sscanf(line, "finger %d %lf", &axis, &value) == 2) { /* touchpad: continuous only */
            if (pv >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION) {
                wl_pointer_send_axis_source(M.pointer, WL_POINTER_AXIS_SOURCE_FINGER);
            }
            wl_pointer_send_axis(M.pointer, 1000, (uint32_t)axis, wl_fixed_from_double(value));
        } else if (sscanf(line, "motion %lf %lf", &x, &y) == 2) {
            wl_pointer_send_motion(M.pointer, 1000, wl_fixed_from_double(x), wl_fixed_from_double(y));
        } else if (sscanf(line, "press %u", &btn) == 1) {
            P_button_serial = wl_display_next_serial(M.display);
            wl_pointer_send_button(M.pointer, P_button_serial, 1000, btn, WL_POINTER_BUTTON_STATE_PRESSED);
        } else if (sscanf(line, "release %u", &btn) == 1) {
            wl_pointer_send_button(M.pointer, wl_display_next_serial(M.display), 1000, btn,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
        } else {
            continue;
        }
        mini_pointer_frame();
    }
    fclose(f);
    wl_display_flush_clients(M.display);
    return 0;
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
        /* one transaction carrying both strings, the preedit first on the
         * wire: done must apply the commit before the new preedit */
        zwp_text_input_v3_send_preedit_string(res, "nx", 2, 2);
        zwp_text_input_v3_send_commit_string(res, "Y");
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
    /* data-offer pipes and client sockets: a client that went away must not
     * kill the compositor mid-test (a standalone process, nothing inherits) */
    signal(SIGPIPE, SIG_IGN);
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
    P_cmd_path = getenv("MINI_POINTER_CMD");
    if (P_cmd_path) {
        mini_pointer_cmd_source = wl_event_loop_add_signal(M.loop, SIGUSR2, P_signal_cb, NULL);
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
    int seat_version = wl_seat_interface.version < 9 ? wl_seat_interface.version : 9;
    if (getenv("MINI_SEAT_VERSION")) {
        const int want = atoi(getenv("MINI_SEAT_VERSION"));
        seat_version = want >= 1 && want < seat_version ? want : seat_version;
    }
    wl_global_create(M.display, &wl_seat_interface, seat_version, NULL, seat_bind);
    wl_global_create(M.display, &xdg_wm_base_interface, 2, NULL, wm_base_bind);
    wl_global_create(M.display, &zwp_pointer_constraints_v1_interface, 1, NULL, pc_bind);
    wl_global_create(M.display, &zwp_relative_pointer_manager_v1_interface, 1, NULL, rpm_bind);
    B_viewporter = getenv("MINI_VIEWPORTER") && atoi(getenv("MINI_VIEWPORTER"));
    if (B_viewporter) {
        wl_global_create(M.display, &wp_viewporter_interface, 1, NULL, viewporter_bind);
    }

    D_data = getenv("MINI_DND");
    if (getenv("MINI_DND_REPEAT") && atoi(getenv("MINI_DND_REPEAT")) > 1) {
        D_repeat = atoi(getenv("MINI_DND_REPEAT"));
    }
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

    C_log = getenv("MINI_CURSOR_LOG");
    if (C_log && !(getenv("MINI_CURSOR_SHAPE") && atoi(getenv("MINI_CURSOR_SHAPE")) == 0)) {
        wl_global_create(M.display, &wp_cursor_shape_manager_v1_interface, 1, NULL, C_mgr_bind);
    }
    TL_log = getenv("MINI_TOPLEVEL_LOG");
    RZ_log = getenv("MINI_RESIZE_LOG");
    if (TL_log) {
        wl_global_create(M.display, &xdg_toplevel_icon_manager_v1_interface, 1, NULL, TL_icon_mgr_bind);
        wl_global_create(M.display, &xdg_wm_dialog_v1_interface, 1, NULL, TL_dialog_mgr_bind);
        wl_global_create(M.display, &wp_alpha_modifier_v1_interface, 1, NULL, TL_alpha_mgr_bind);
        wl_global_create(M.display, &zxdg_exporter_v2_interface, 1, NULL, TL_exporter_bind);
    }
    T_log = getenv("MINI_TEXT_INPUT");
    if (T_log) {
        wl_global_create(M.display, &zwp_text_input_manager_v3_interface, 1, NULL, T_mgr_bind);
    }
    if (getenv("MINI_GESTURES") && atoi(getenv("MINI_GESTURES")) > 0) {
        const int v = atoi(getenv("MINI_GESTURES"));
        wl_global_create(M.display, &zwp_pointer_gestures_v1_interface, v > 3 ? 3 : v, NULL, GS_bind);
    }
    if (getenv("MINI_TABLET") && strcmp(getenv("MINI_TABLET"), "0") != 0) {
        wl_global_create(M.display, &zwp_tablet_manager_v2_interface, 1, NULL, TB_mgr_bind);
    }

    wl_display_add_client_created_listener(M.display, &client_listener);

    W_log = getenv("MINI_WARP_LOG");
    if (W_log) {
        if (!(getenv("MINI_POINTER_WARP") && atoi(getenv("MINI_POINTER_WARP")) == 0)) {
            wl_global_create(M.display, &wp_pointer_warp_v1_interface, 1, NULL, W_bind);
        }
    } else { /* the pointer-lock script */
        M.rel_timer = wl_event_loop_add_timer(M.loop, rel_timer_cb, NULL);
        M.abs_timer = wl_event_loop_add_timer(M.loop, abs_timer_cb, NULL);
    }
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
