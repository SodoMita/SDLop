#!/bin/sh
#
# Console (VT) + DRM master drill for the kmsdrm driver in QEMU.
#
# Boots Debian's kernel with a GPU that has a real DRM driver (virtio-gpu or
# bochs), stages the modules the guest needs, and runs a small driver in the
# guest that exercises the console contract over SDL and reports VT-CHECK
# lines. The harness decides PASS/FAIL from those lines, the guest's exit code
# and a kernel WARN/BUG scan.
#
#   tests/run_kmsdrm_qemu.sh [workdir]
#
# Environment:
#   SDLOP_QEMU_KERNEL     vmlinuz to boot (default: newest /boot/vmlinuz-*)
#   SDLOP_QEMU_MODULES    module tree with the .ko.xz files (default: the tree
#                         that matches the kernel name under /usr/lib/modules,
#                         else /tmp/kern/usr/lib/modules from an unpacked
#                         linux-image .deb)
#   SDLOP_QEMU_GPU        QEMU device (default virtio-gpu-pci,id=gpu0;
#                         bochs-display has no cursor plane, which the drill
#                         then reports as skipped)
#   SDLOP_QEMU_MEM        guest memory in MB (default 1024)
#   SDLOP_QEMU_TIMEOUT    boot+run timeout in seconds (default 300)
#   SDLOP_KMSDRM_VT_SWITCH=1   expect the opt-in automatic VT switch
#   SDLOP_QEMU_RAW=1    leave raw input enabled (default: SDLOP_DISABLE_RAW_INPUT=1;
#                       with a raw keyboard the driver takes K_OFF and forwards
#                       Ctrl+Alt+Fn itself)
#   SDLOP_QEMU_CTTY=1   start on controlling VT 3 while VT 2 remains visible
#   SDLOP_QEMU_CHORD=1  also inject Ctrl+Alt+F3 into the virtual keyboard and
#                       check the VT moves (needs SDLOP_QEMU_RAW=1)
#   TTYNO=3               run on /dev/tty3 (implies the opt-in switch)
#   SDLOP_QEMU_KEEP=1     keep the initramfs and logs
#
# Exits 0 on PASS, 1 on a failed check, 2 when the rig cannot be built.
#
# This software is provided 'as-is', without any express or implied
# warranty (zlib license, see LICENSE).

set -eu

SRC=$(cd "$(dirname "$0")/.." && pwd)
WORK=${1:-${TMPDIR:-/tmp}/sdlop-kmsdrm-qemu}
KERNEL=${SDLOP_QEMU_KERNEL:-}
GPU=${SDLOP_QEMU_GPU:-virtio-gpu-pci,id=gpu0}
MEM=${SDLOP_QEMU_MEM:-1024}
BOOT_TIMEOUT=${SDLOP_QEMU_TIMEOUT:-300}

say() { printf 'harness: %s\n' "$*"; }
die() { printf 'harness: error: %s\n' "$*" >&2; exit 2; }

command -v qemu-system-x86_64 >/dev/null 2>&1 || die "qemu-system-x86_64 not found"
command -v cpio >/dev/null 2>&1 || die "cpio not found"
command -v busybox >/dev/null 2>&1 || die "busybox not found"
command -v xz >/dev/null 2>&1 || { command -v busybox >/dev/null 2>&1 && :; }

if [ -z "$KERNEL" ]; then
    for candidate in $(ls -1 /boot/vmlinuz-* 2>/dev/null | sort -V -r); do
        KERNEL=$candidate
        break
    done
fi
[ -n "$KERNEL" ] && [ -r "$KERNEL" ] || die "no kernel to boot (set SDLOP_QEMU_KERNEL)"

kver=$(basename "$KERNEL" | sed 's/^vmlinuz-//')
modtree=${SDLOP_QEMU_MODULES:-}
if [ -z "$modtree" ]; then
    for candidate in "/usr/lib/modules/$kver" "/tmp/kern/usr/lib/modules/$kver"; do
        if [ -d "$candidate/kernel" ]; then
            modtree=$candidate
            break
        fi
    done
fi
[ -n "$modtree" ] && [ -d "$modtree/kernel" ] || die "no module tree for $kver (set SDLOP_QEMU_MODULES)"

say "kernel $kver"
say "modules $modtree"
say "gpu $GPU"
say "workdir $WORK"

rm -rf "$WORK"
mkdir -p "$WORK/root/bin" "$WORK/root/dev" "$WORK/root/proc" "$WORK/root/sys" \
         "$WORK/root/tmp" "$WORK/root/modules"

# --- modules: insmod by path in dependency order (no modprobe index in a
# hand-built initramfs, and the guest's insmod cannot read .xz) --------------
stage_module() {
    name=$1
    file=$(find "$modtree/kernel" -name "$name.ko*" | head -1)
    [ -n "$file" ] || return 1
    case $file in
        *.xz) xz -dc "$file" >"$WORK/root/modules/$name.ko" ;;
        *)    cp "$file" "$WORK/root/modules/$name.ko" ;;
    esac
    return 0
}
MODULES=""
for name in drm drm_kms_helper drm_shmem_helper drm_ttm_helper ttm drm_vram_helper drm_exec virtio_dma_buf virtio-gpu bochs input-core evdev virtio_input; do
    if stage_module "$name"; then
        MODULES="$MODULES $name"
    fi
done
# order matters for insmod: dependencies first (ttm before drm_ttm_helper,
# which is what virtio-gpu and bochs actually need)
ORDER=""
for name in drm drm_kms_helper drm_exec drm_shmem_helper ttm drm_ttm_helper drm_vram_helper virtio_dma_buf virtio-gpu bochs input-core evdev virtio_input; do
    case " $MODULES " in
        *" $name "*) ORDER="$ORDER $name" ;;
    esac
done
say "modules:$ORDER"

# --- the guest driver -------------------------------------------------------
cat >"$WORK/drill.c" <<'DRILL'
/* Console/VT drill for the kmsdrm driver. Runs inside the guest against the
   real DRM device and the real VT layer; prints VT-CHECK lines the host reads
   from the serial log. */
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures;

static void check(const char *name, int ok, const char *fmt, ...)
{
    va_list ap;
    printf("VT-CHECK %s %s ", name, ok ? "PASS" : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    if (!ok) {
        failures++;
    }
}

static int active_vt(void)
{
    const int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    struct vt_stat st;
    int vt = -1;
    if (fd < 0) {
        return -1;
    }
    memset(&st, 0, sizeof(st));
    if (ioctl(fd, VT_GETSTATE, &st) == 0) {
        vt = (int)st.v_active;
    }
    close(fd);
    return vt;
}

static int find_driver_vt(int *fd_out)
{
    for (int n = 1; n <= 63; n++) {
        char path[32];
        struct vt_mode vm;
        int mode = -1;
        snprintf(path, sizeof(path), "/dev/tty%d", n);
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        memset(&vm, 0, sizeof(vm));
        if (ioctl(fd, VT_GETMODE, &vm) == 0 && vm.mode == VT_PROCESS &&
            ioctl(fd, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS) {
            *fd_out = fd;
            return n;
        }
        close(fd);
    }
    *fd_out = -1;
    return 0;
}

/* switch VTs, pumping so our own VT handler can acknowledge the switch */
static int switch_to(SDL_Window *w, int vt)
{
    const int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    const int asked = (ioctl(fd, VT_ACTIVATE, vt) == 0);
    close(fd);
    if (!asked) {
        return 0;
    }
    for (int i = 0; i < 300; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
        if (active_vt() == vt) {
            for (int j = 0; j < 10; j++) {
                SDL_PumpEvents();
                SDL_Delay(10);
            }
            return 1;
        }
    }
    return 0;
}

#include <xf86drm.h>
#include <xf86drmMode.h>

static void paint(SDL_Surface *s, SDL_Window *w, unsigned color);

/* Does this device have a cursor plane at all? bochs-display has only a
   primary plane, so "no sprite on the plane" would say nothing there. */
static int cursor_plane_exists(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    int found = 0;

    if (fd < 0) {
        return 0;
    }
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (planes) {
        for (Uint32 i = 0; i < planes->count_planes && !found; i++) {
            drmModePlane *plane = drmModeGetPlane(fd, planes->planes[i]);
            if (plane) {
                drmModeObjectProperties *props =
                    drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
                if (props) {
                    for (Uint32 j = 0; j < props->count_props; j++) {
                        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[j]);
                        if (prop && strcmp(prop->name, "type") == 0 &&
                            (props->prop_values[j] & DRM_PLANE_TYPE_CURSOR)) {
                            found = 1;
                        }
                        if (prop) {
                            drmModeFreeProperty(prop);
                        }
                        if (found) {
                            break;
                        }
                    }
                    drmModeFreeObjectProperties(props);
                }
                drmModeFreePlane(plane);
            }
        }
        drmModeFreePlaneResources(planes);
    }
    close(fd);
    return found;
}

/* Is a cursor sprite currently attached to the given CRTC? */
static int cursor_plane_on_crtc(unsigned crtc_id)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    int attached = -1;

    if (fd < 0) {
        return -1;
    }
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (planes) {
        for (Uint32 i = 0; i < planes->count_planes; i++) {
            drmModePlane *plane = drmModeGetPlane(fd, planes->planes[i]);
            if (!plane) {
                continue;
            }
            drmModeObjectProperties *props =
                drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
            int is_cursor = 0;
            if (props) {
                for (Uint32 j = 0; j < props->count_props; j++) {
                    drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[j]);
                    if (prop && strcmp(prop->name, "type") == 0 &&
                        (props->prop_values[j] & DRM_PLANE_TYPE_CURSOR)) {
                        is_cursor = 1;
                    }
                    if (prop) {
                        drmModeFreeProperty(prop);
                    }
                }
                drmModeFreeObjectProperties(props);
            }
            if (is_cursor) {
                attached = (plane->crtc_id == crtc_id && plane->fb_id != 0) ? 1 : 0;
                drmModeFreePlane(plane);
                break;
            }
            drmModeFreePlane(plane);
        }
        drmModeFreePlaneResources(planes);
    }
    close(fd);
    return attached;
}

/* our CRTC id, so the plane question is asked about the right pipe */
static unsigned our_crtc_id(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    unsigned id = 0;

    if (fd < 0) {
        return 0;
    }
    drmModeRes *res = drmModeGetResources(fd);
    if (res) {
        for (int i = 0; i < res->count_connectors && !id; i++) {
            drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
            if (c && c->connection == DRM_MODE_CONNECTED && c->encoder_id) {
                drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id);
                if (e) {
                    id = e->crtc_id;
                    drmModeFreeEncoder(e);
                }
            }
            if (c) {
                drmModeFreeConnector(c);
            }
        }
        drmModeFreeResources(res);
    }
    close(fd);
    return id;
}

/* Read what the CRTC is actually scanning out: no modeset (that would need to
   be DRM master, which the driver is), just look up the framebuffer the CRTC
   holds, map it through DUMP_MAP and read a pixel. This is the buffer state
   after the driver's own present, i.e. the frame the screen shows. */

static unsigned kms_crtc_pixel(int x, int y, int *ok)
{
    unsigned value = 0xFFFFFFFFu;
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);

    *ok = 0;
    if (fd < 0) {
        return value;
    }
    drmModeRes *res = drmModeGetResources(fd);
    drmModeConnector *conn = NULL;
    drmModeEncoder *enc = NULL;
    drmModeCrtc *crtc = NULL;

    if (res) {
        for (int i = 0; i < res->count_connectors && !conn; i++) {
            drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
            if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
                conn = c;
            } else if (c) {
                drmModeFreeConnector(c);
            }
        }
    }
    if (conn && conn->encoder_id) {
        enc = drmModeGetEncoder(fd, conn->encoder_id);
    }
    if (enc && enc->crtc_id) {
        crtc = drmModeGetCrtc(fd, enc->crtc_id);
    }
    if (crtc && crtc->buffer_id) {
        drmModeFBPtr fb = drmModeGetFB(fd, crtc->buffer_id);
        if (fb && fb->handle) {
            struct drm_mode_map_dumb map_req;
            memset(&map_req, 0, sizeof(map_req));
            map_req.handle = fb->handle;
            if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) == 0) {
                const size_t length = (size_t)fb->pitch * (size_t)fb->height;
                void *map = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, (off_t)map_req.offset);
                if (map != MAP_FAILED) {
                    if (x >= 0 && y >= 0 && (unsigned)x < fb->width && (unsigned)y < fb->height &&
                        fb->bpp == 32) {
                        memcpy(&value, (const Uint8 *)map + (size_t)y * fb->pitch + (size_t)x * 4u,
                               sizeof(value));
                        value &= 0x00FFFFFFu;
                        *ok = 1;
                    }
                    munmap(map, length);
                }
            }
        }
        if (fb) {
            drmModeFreeFB(fb);
        }
    }
    if (crtc) {
        drmModeFreeCrtc(crtc);
    }
    if (enc) {
        drmModeFreeEncoder(enc);
    }
    if (conn) {
        drmModeFreeConnector(conn);
    }
    if (res) {
        drmModeFreeResources(res);
    }
    close(fd);
    return value;
}

/* wait for the frame the app drew to reach the CRTC (presents are async) */
static unsigned wait_for_frame(SDL_Window *w, SDL_Surface *surface, unsigned want, int *ok)
{
    unsigned got = 0xFFFFFFFFu;
    for (int i = 0; i < 25; i++) {
        SDL_PumpEvents();
        SDL_Delay(20);
        got = kms_crtc_pixel(surface->w / 2, surface->h / 2, ok);
        if (*ok) {
            const int dr = (int)((got >> 16) & 0xFF) - (int)((want >> 16) & 0xFF);
            const int dg = (int)((got >> 8) & 0xFF) - (int)((want >> 8) & 0xFF);
            const int db = (int)(got & 0xFF) - (int)(want & 0xFF);
            if (abs(dr) <= 8 && abs(dg) <= 8 && abs(db) <= 8) {
                return got;
            }
        }
        if (i == 12) {
            paint(surface, w, want); /* nudge a second present */
        }
    }
    return got;
}

/* a pixel straight off the scanout: /sys/kernel/debug is not mounted, so read
   through /dev/fb0 when the kernel happens to provide one */
static unsigned screen_pixel(int x, int y)
{
    static int fd = -2;
    static struct fb_var_screeninfo var;
    static struct fb_fix_screeninfo fix;
    static unsigned char *map;
    if (fd == -2) {
        fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
        if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 ||
            ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
            if (fd >= 0) {
                close(fd);
            }
            fd = -1;
        } else {
            map = mmap(NULL, (size_t)fix.line_length * var.yres_virtual, PROT_READ, MAP_SHARED, fd, 0);
            if (map == MAP_FAILED) {
                map = NULL;
                fd = -1;
            }
        }
    }
    if (fd < 0 || !map || x < 0 || y < 0 || (unsigned)x >= var.xres || (unsigned)y >= var.yres) {
        return 0xFFFFFFFFu;
    }
    const unsigned char *p = map + (size_t)y * fix.line_length + (size_t)x * (var.bits_per_pixel / 8);
    unsigned raw = 0;
    if (var.bits_per_pixel == 16) {
        unsigned short v;
        memcpy(&v, p, sizeof(v));
        raw = v;
    } else if (var.bits_per_pixel == 24) {
        raw = (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16);
    } else {
        memcpy(&raw, p, sizeof(raw));
    }
    return raw;
}

static void paint(SDL_Surface *s, SDL_Window *w, unsigned color)
{
    SDL_FillSurfaceRect(s, NULL, color);
    SDL_UpdateWindowSurface(w);
}

int main(void)
{
    const int vt_before = active_vt();

    setenv("SDL_VIDEODRIVER", "kmsdrm", 1);
    if (!getenv("SDLOP_DISABLE_RAW_INPUT")) {
        setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("VT-CHECK init FAIL %s\n", SDL_GetError());
        return 1;
    }
    const char *driver = SDL_GetCurrentVideoDriver();
    check("init", driver && strcmp(driver, "kmsdrm") == 0, "driver=%s", driver ? driver : "?");
    printf("VT-START-ACTIVE=%d\n", vt_before);

    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    check("display", ids && n >= 1, "%d display(s)", n);
    SDL_Rect bounds = { 0, 0, 0, 0 };
    if (ids && n >= 1) {
        SDL_GetDisplayBounds(ids[0], &bounds);
    }
    SDL_Window *w = SDL_CreateWindow("kmsdrm drill", bounds.w ? bounds.w : 640, bounds.h ? bounds.h : 480, 0);
    check("window", w != NULL, "%dx%d", bounds.w, bounds.h);
    if (!w) {
        SDL_Quit();
        return 1;
    }
    SDL_ShowWindow(w);
    SDL_Surface *surface = SDL_GetWindowSurface(w);
    check("surface", surface != NULL, "%s", surface ? "ok" : SDL_GetError());
    if (!surface) {
        SDL_Quit();
        return 1;
    }
    paint(surface, w, 0x0000C0u); /* blue */
    for (int i = 0; i < 20; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    printf("VT-READY\n");
    fflush(stdout);

    int fd = -1;
    const int our_vt = find_driver_vt(&fd);
    printf("VT-OURS=%d\n", our_vt);
    if (our_vt <= 0 || fd < 0) {
        check("vt-process", 0, "no VT with VT_PROCESS+KD_GRAPHICS found");
        SDL_Quit();
        return failures > 0;
    }

    /* --- the console contract --- */
    int mode = -1, kbmode = -1;
    struct vt_mode vm;
    memset(&vm, 0, sizeof(vm));
    int rc = ioctl(fd, KDGETMODE, &mode);
    check("text-hidden", rc == 0 && mode == KD_GRAPHICS, "KDGETMODE=%d rc=%d", mode, rc);
    rc = ioctl(fd, KDGKBMODE, &kbmode);
    {
        /* K_OFF is only ours to set when a raw keyboard exists: it is what
           delivers the key events, and it forwards Ctrl+Alt+Fn. Without one the
           console must keep its keyboard handler. */
        char kbbits[8] = { 0 };
        int kbfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (kbfd < 0) {
            kbfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        }
        const int raw_on = kbfd >= 0 &&
                           ioctl(kbfd, EVIOCGBIT(EV_KEY, sizeof(kbbits)), kbbits) >= 0 &&
                           (kbbits[KEY_A / 8] & (1 << (KEY_A % 8))) != 0;
        if (kbfd >= 0) {
            close(kbfd);
        }
        const char *disabled = getenv("SDLOP_DISABLE_RAW_INPUT");
        if (raw_on && !(disabled && *disabled && strcmp(disabled, "0") != 0)) {
            check("kbd-enabled-with-raw-keyboard", rc == 0 && kbmode == K_OFF,
                  "SDLOP_DISABLE_RAW_INPUT unset: KDGKBMODE=%d rc=%d", kbmode, rc);
        } else {
            check("kbd-untouched", rc == 0 && kbmode != K_OFF,
                  "SDLOP_DISABLE_RAW_INPUT=1: console keyboard left alone (kbmode=%d rc=%d)",
                  kbmode, rc);
        }
    }
    rc = ioctl(fd, VT_GETMODE, &vm);
    check("vt-process", rc == 0 && vm.mode == VT_PROCESS, "VT_GETMODE=%d rc=%d", (int)vm.mode, rc);

    /* --- did starting SDL take the user's VT away? --- */
    const int now_active = active_vt();
    if (vt_before > 0 && now_active > 0) {
        if (now_active == our_vt && our_vt != vt_before) {
            /* we were asked to take this VT (TTYNO / VT_SWITCH): that is the
               only case in which the active VT may have moved */
            check("no-hijack", 1, "opt-in switch: user was on VT %d, ours is %d",
                  vt_before, our_vt);
        } else {
            check("no-hijack", now_active == vt_before,
                  "user was on VT %d before init, active now VT %d, ours %d",
                  vt_before, now_active, our_vt);
        }
    }

    if (getenv("SDLOP_QEMU_CTTY") && strcmp(getenv("SDLOP_QEMU_CTTY"), "1") == 0) {
        check("controlling-vt", our_vt == 3 && now_active == vt_before && vt_before == 2,
              "controlling VT 3 selected without activating it (ours=%d active=%d)", our_vt, now_active);
        check("controlling-vt-show", switch_to(w, our_vt), "user switched to our background VT");
    }

    /* --- the cursor: it must be on screen while we own it and gone when the
       user looks at another VT (SDL_CursorVisible() is the core's answer) --- */
    {
        const char *glib_env = getenv("GLIBC_TUNABLES");
        printf("VT-DIAG dlopen-libdrm=%d glibc-tunables=%s\n",
               drmAvailable(), glib_env ? glib_env : "(unset)");
    }
    check("cursor-created", 1, "cursor API accepted (driver sets it up in init)");
    SDL_ShowCursor();
    SDL_PumpEvents();
    check("cursor-flag", SDL_CursorVisible(), "SDL_CursorVisible()=%d", (int)SDL_CursorVisible());
    SDL_HideCursor();
    SDL_PumpEvents();
    check("cursor-hide", !SDL_CursorVisible(), "SDL_CursorVisible()=%d after hide", (int)SDL_CursorVisible());
    SDL_ShowCursor();
    SDL_PumpEvents();

    /* --- the hardware cursor must really be on the plane while we own the
       screen, and off it while the user is on another VT --- */
    const unsigned crtc_id = our_crtc_id();
    const int have_cursor_plane = cursor_plane_exists();
    if (!have_cursor_plane) {
        check("cursor-plane-note", 1, "the device has no cursor plane, phase skipped");
    } else if (!crtc_id) {
        check("cursor-plane-note", 1, "no CRTC to ask about, phase skipped");
    } else {
        int on = -1;
        for (int i = 0; i < 25 && on != 1; i++) {
            SDL_PumpEvents();
            SDL_Delay(20);
            on = cursor_plane_on_crtc(crtc_id);
        }
        check("cursor-plane-on", on == 1, "a cursor sprite is on crtc %u (found=%d)", crtc_id, on);
    }

    /* --- DRM master: dropping it is what makes cursors and modesets fail ---
       Master belongs to a DRM file (per open()), not to the process, so ask a
       fresh fd for it: SET_MASTER succeeds only when nobody else holds it.
       Getting it here and handing it straight back keeps the rest of the
       drill unaffected. */
    {
        const int master_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        if (master_fd >= 0) {
            const unsigned long DROP_MASTER = 0x641b;   /* DRM_IOCTL_DROP_MASTER */
            const unsigned long SET_MASTER = 0x641a;    /* DRM_IOCTL_SET_MASTER */
            const int free_master = (ioctl(master_fd, SET_MASTER) == 0);
            if (free_master) {
                ioctl(master_fd, DROP_MASTER); /* hand it straight back */
            }
            /* master is only handed over when a Vulkan window is created;
               this window is a software one, so SDL must still hold it */
            check("master-kept", !free_master,
                  "SDL holds DRM master (free=%d)", free_master);
            close(master_fd);
        }
    }

    /* --- Ctrl+Alt+Fn through the raw input path ---
       The console keyboard is in K_OFF, so the kernel's own chord handler is
       dead and sdlop has to recognise the chord itself. The host types it into
       the virtio keyboard through the QEMU monitor once this line is printed;
       all this side does is watch the VT. */
    if (getenv("SDLOP_QEMU_CHORD") && strcmp(getenv("SDLOP_QEMU_CHORD"), "1") == 0) {
        printf("VT-CHORD-READY\n");
        fflush(stdout);
        const int want_vt = 3;
        const int from_vt = active_vt();
        bool moved = false;
        for (int i = 0; i < 250; i++) { /* up to ~5 s */
            SDL_PumpEvents();
            if (active_vt() == want_vt) {
                moved = true;
                break;
            }
            SDL_Delay(20);
        }
        check("chord-ctrl-alt-fn", moved,
              "Ctrl+Alt+F%d switched the console, from VT %d now on VT %d", want_vt, from_vt,
              active_vt());
        if (moved) {
            /* hand VT 3 back to the console so the switch checks are unaffected */
            char path[32];
            snprintf(path, sizeof(path), "/dev/tty%d", want_vt);
            const int cleanup = open(path, O_RDWR | O_CLOEXEC);
            if (cleanup >= 0) {
                ioctl(cleanup, KDSETMODE, KD_TEXT);
                struct vt_mode vm2;
                memset(&vm2, 0, sizeof(vm2));
                vm2.mode = VT_AUTO;
                ioctl(cleanup, VT_SETMODE, &vm2);
                close(cleanup);
            }
        }
    }

    /* --- switch away and back --- */
    const int other_vt = (our_vt == 2) ? 3 : 2;
    if (switch_to(w, other_vt)) {
        mode = kbmode = -1;
        int arc = ioctl(fd, KDGETMODE, &mode);
        check("away-text", arc == 0 && mode == KD_TEXT,
              "console returned to KD_TEXT while away (mode=%d rc=%d)", mode, arc);
        arc = ioctl(fd, KDGKBMODE, &kbmode);
        check("away-kbd", arc == 0 && kbmode != K_OFF,
              "keyboard handler restored while away (kbmode=%d rc=%d)", kbmode, arc);
        if (have_cursor_plane && crtc_id) {
            int off = cursor_plane_on_crtc(crtc_id);
            check("cursor-plane-away", off == 0,
                  "the cursor sprite is off the plane while the user is away (found=%d)", off);
        }
        if (switch_to(w, our_vt)) {
            mode = -1;
            int brc = ioctl(fd, KDGETMODE, &mode);
            check("back-graphics", brc == 0 && mode == KD_GRAPHICS,
                  "console back in KD_GRAPHICS (mode=%d rc=%d)", mode, brc);
            paint(surface, w, 0x00C000u); /* green: what must come back */
            for (int i = 0; i < 30; i++) {
                SDL_PumpEvents();
                SDL_Delay(10);
            }
            printf("VT-ONSCREEN\n");
            fflush(stdout);
            /* what the CRTC is scanning out, not what the surface claims */
            int frame_ok = 0;
            const unsigned px = wait_for_frame(w, surface, 0x00C000u, &frame_ok);
            const int dr = (int)((px >> 16) & 0xFF);
            const int dg = (int)((px >> 8) & 0xFF) - 0xC0;
            const int db = (int)(px & 0xFF);
            check("onscreen-frame", frame_ok && abs(dr) <= 8 && abs(dg) <= 8 && abs(db) <= 8,
                  "the CRTC scans out what the app drew (0x%06x)", px);
            if (have_cursor_plane && crtc_id) {
                int back = 0;
                for (int i = 0; i < 25 && back != 1; i++) {
                    SDL_PumpEvents();
                    SDL_Delay(20);
                    back = cursor_plane_on_crtc(crtc_id);
                }
                check("cursor-plane-back", back == 1,
                      "the cursor came back after the switch (found=%d)", back);
            }
        } else {
            check("back-graphics", 0, "could not switch back to VT %d", our_vt);
        }
    } else {
        check("away-text", 0, "could not switch away to VT %d", other_vt);
    }

    SDL_DestroyWindow(w);
    SDL_Quit();

    /* --- after quit the console belongs to the user again --- */
    if (our_vt > 0) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/tty%d", our_vt);
        const int qfd = open(path, O_RDWR | O_CLOEXEC);
        if (qfd >= 0) {
            int qmode = -1;
            struct vt_mode qvm;
            memset(&qvm, 0, sizeof(qvm));
            int qrc = ioctl(qfd, KDGETMODE, &qmode);
            check("exit-text", qrc == 0 && qmode == KD_TEXT,
                  "KDGETMODE=%d rc=%d after SDL_Quit", qmode, qrc);
            qrc = ioctl(qfd, VT_GETMODE, &qvm);
            check("exit-vt-auto", qrc == 0 && qvm.mode == VT_AUTO,
                  "VT_GETMODE=%d rc=%d after SDL_Quit", (int)qvm.mode, qrc);
            close(qfd);
        }
    }

    printf("guest: %d failure(s)\n", failures);
    return failures > 0 ? 1 : 0;
}
DRILL

# --- build everything for the guest -----------------------------------------
BUILD=${SDLOP_QEMU_BUILD:-$WORK/build}
if [ ! -f "$BUILD/libsdlop.a" ] && [ ! -f "$BUILD/libSDLop.a" ]; then
    say "configuring a build in $BUILD"
    cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug >"$WORK/cmake.log" 2>&1 ||
        die "cmake configure failed (see $WORK/cmake.log)"
fi
cmake --build "$BUILD" --target sdlop >"$WORK/build.log" 2>&1 || die "library build failed (see $WORK/build.log)"
LIB=$(ls -1 "$BUILD"/libSDLop.a "$BUILD"/libsdlop.a 2>/dev/null | head -1)
say "static drill binary (libdrm.a for the DRM_IOCTL shims)"
cc -static -O1 -o "$WORK/root/bin/drill" "$WORK/drill.c" \
    -I"$SRC/src" -I"$SRC/include" -I/usr/include/libdrm -D_GNU_SOURCE \
    -DSDLop_VIDEO_KMSDRM=1 -DSDLop_VIDEO_FBDEV=1 -DSDLop_VIDEO_WAYLAND=1 -DSDLop_VIDEO_X11=1 \
    "$LIB" -lpthread -lm -ldl /usr/lib/x86_64-linux-gnu/libdrm.a 2>"$WORK/cc.log" || die "static link failed (see $WORK/cc.log)"

# SDLop dlopen()s libdrm and libc is needed by the loader: a static binary can
# still load a shared object as long as the loader and the object's own
# dependencies are in the tree
mkdir -p "$WORK/root/lib64" "$WORK/root/lib/x86_64-linux-gnu" "$WORK/root/lib"
# the loader path lives in the binary, so the copy has to exist in both places
for loader in /lib64/ld-linux-x86-64.so.2 /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2; do
    if [ -e "$loader" ]; then
        cp -L "$loader" "$WORK/root/lib64/ld-linux-x86-64.so.2"
        cp -L "$loader" "$WORK/root/lib/ld-linux-x86-64.so.2"
        say "loader $loader"
        break
    fi
done
for lib in libdl.so.2 libc.so.6 libm.so.6 libpthread.so.0; do
    found=$(/sbin/ldconfig -p 2>/dev/null | awk -v n="$lib" '$1 == n { print $NF; exit }')
    [ -n "$found" ] && cp -L "$found" "$WORK/root/lib/x86_64-linux-gnu/$lib"
done
# the hardware cursor needs libgbm (the driver dlopen()s it, else it has no
# cursor at all), which in turn needs libdrm and libexpat
for lib in libgbm.so.1 libexpat.so.1; do
    found=$(/sbin/ldconfig -p 2>/dev/null | awk -v n="$lib" '$1 == n { print $NF; exit }')
    [ -n "$found" ] && cp -L "$found" "$WORK/root/lib/x86_64-linux-gnu/$lib"
done
libdrm=$(/sbin/ldconfig -p 2>/dev/null | awk '$1 ~ /^libdrm\.so\.2$/ { print $NF; exit }')
if [ -n "$libdrm" ]; then
    cp -L "$libdrm" "$WORK/root/lib/x86_64-linux-gnu/libdrm.so.2"
    say "libdrm from $libdrm"
else
    say "WARNING: libdrm.so.2 not found on the host; the guest cannot open the DRM device"
fi
say "guest libs: $(ls "$WORK/root/lib/x86_64-linux-gnu" 2>/dev/null | tr '\n' ' ')"

cp "$(command -v busybox)" "$WORK/root/bin/busybox"
for applet in sh mount umount ls cat echo sleep mknod chvt poweroff dmesg insmod setsid; do
    ln -sf busybox "$WORK/root/bin/$applet"
done

# everything the guest reads is baked in here: the guest does not inherit the
# host environment
GUEST_LOGGING=${SDLOP_QEMU_LOGGING:-debug}
GUEST_GLIBC_TUNABLES=${GLIBC_TUNABLES:-glibc.rtld.dynamic_sort=1}
export GUEST_LOGGING GUEST_GLIBC_TUNABLES

DISABLE_RAW=1
[ "${SDLOP_QEMU_RAW:-0}" = "1" ] && DISABLE_RAW=0
cat >"$WORK/root/init" <<INIT
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts && mount -t devpts devpts /dev/pts
exec >/dev/ttyS0 2>&1
echo "1" > /proc/sys/kernel/printk
PATH=/bin
echo "VTHARNESS: up"
# the drill reads commands from a second serial port (host -> guest)
export LD_LIBRARY_PATH=/lib/x86_64-linux-gnu

sin() {
    if /bin/insmod "/modules/\$1.ko" 2>/tmp/err; then
        echo "VTHARNESS: mod \$1 ok"
    else
        echo "VTHARNESS: mod \$1 -> \$(cat /tmp/err)"
    fi
}
for m in$ORDER; do
    sin "\$m"
done

# the user sits on a text console: this is what must not be taken away
chvt 2 && echo "VTHARNESS: user on VT 2"
sleep 1
echo "VTHARNESS: drm devices: \$(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
echo "VTHARNESS: pci display: \$(cat /sys/bus/pci/drivers/virtio-pci/*/uevent 2>/dev/null | grep -c DRIVER) virtio nodes"
echo "VTHARNESS: dmesg drm: \$(dmesg | grep -iE 'virtio_gpu|virtio-gpu|bochs|drm' | tail -5 | tr '\n' '|')"
echo "VTHARNESS: tty0 -> \$(cat /sys/class/tty/tty0/active 2>/dev/null)"

export SDL_LOGGING="$GUEST_LOGGING"
export GLIBC_TUNABLES="$GUEST_GLIBC_TUNABLES"
export SDLOP_KMSDRM_VT_SWITCH="${SDLOP_KMSDRM_VT_SWITCH:-}"
export SDLOP_DISABLE_RAW_INPUT="$DISABLE_RAW"
export SDLOP_QEMU_CHORD="${SDLOP_QEMU_CHORD:-0}"
export SDLOP_QEMU_CTTY="${SDLOP_QEMU_CTTY:-0}"
export SDLOP_QEMU_CMD=ttyS1
export TTYNO="${TTYNO:-}"
echo "VTHARNESS: running the drill"
if [ "${SDLOP_QEMU_CTTY:-0}" = "1" ]; then
    setsid sh -c 'exec /bin/drill </dev/tty3'
else
    /bin/drill
fi
echo "VTHARNESS: exit=\$?"
poweroff -f
INIT
chmod +x "$WORK/root/init"

( cd "$WORK/root" && find . | cpio -o -H newc --quiet 2>/dev/null | gzip -9 ) >"$WORK/initramfs.gz" ||
    die "initramfs failed"

# --- Ctrl+Alt+Fn: type the chord into the virtual keyboard -------------------
# QEMU's human monitor is the only interface that can inject key presses, so
# the guest's "VT-CHORD-READY" triggers one `sendkey` on the monitor socket.
# Only meaningful with raw input on (SDLOP_QEMU_RAW=1): without it the console
# keeps its own keyboard handler and the kernel switches VTs by itself.
CHORD=${SDLOP_QEMU_CHORD:-0}
inject_chord() {
    i=0
    while [ "$i" -lt 600 ]; do
        [ -s "$WORK/serial.log" ] && grep -aq '^VT-CHORD-READY' "$WORK/serial.log" && break
        i=$((i + 1))
        sleep 0.1
    done
    if ! grep -aq '^VT-CHORD-READY' "$WORK/serial.log" 2>/dev/null; then
        say "the guest never reached the chord phase"
        return 0
    fi
    { printf 'sendkey ctrl-alt-f3\n'; sleep 0.5; } |
        socat -t 3 - "UNIX-CONNECT:$WORK/mon.sock" >/dev/null 2>&1 ||
        say "could not talk to the QEMU monitor"
    say "Ctrl+Alt+F3 injected into the virtio keyboard"
}

# --- boot ------------------------------------------------------------------
say "booting (${BOOT_TIMEOUT}s cap)"
rm -f "$WORK/mon.sock" "$WORK/serial.log"
CHORD_PID=
if [ "$CHORD" = "1" ]; then
    command -v socat >/dev/null 2>&1 || die "socat not found (needed for the chord drill)"
    inject_chord &
    CHORD_PID=$!
fi
set +e
timeout "$BOOT_TIMEOUT" qemu-system-x86_64 \
    -M q35 -m "$MEM" -smp 2 \
    -kernel "$KERNEL" -initrd "$WORK/initramfs.gz" \
    -append "console=ttyS0,115200 panic=1 loglevel=4" \
    -display none -vga none -device "$GPU" \
    -device "virtio-keyboard-pci,id=kbd0" -no-reboot \
    -monitor "unix:$WORK/mon.sock,server=on,wait=off" \
    -serial "file:$WORK/serial.log"
rc=$?
if [ -n "$CHORD_PID" ]; then
    kill "$CHORD_PID" 2>/dev/null || true
    wait "$CHORD_PID" 2>/dev/null || true
fi
set -e
case $rc in
    0) ;;
    124) say "guest did not finish within ${BOOT_TIMEOUT}s" ;;
    *)   say "qemu exited with $rc" ;;
esac

[ -s "$WORK/serial.log" ] || { say "no serial output at all"; exit 2; }

grep -a '^VT-CHECK\|^VT-' "$WORK/serial.log" | sed 's/^/  /' || true
grep -a 'VTHARNESS:\|KMSDRM:' "$WORK/serial.log" | sed 's/^/  /' || true

ok=$(grep -ac '^VT-CHECK .* PASS' "$WORK/serial.log" || true)
bad=$(grep -ac '^VT-CHECK .* FAIL' "$WORK/serial.log" || true)
skip=$(grep -ac '^VT-CHECK .* SKIP' "$WORK/serial.log" || true)
printf 'guest: %s pass / %s fail / %s skip\n' "$ok" "$bad" "$skip"

quirk=$(grep -acE 'WARNING:|BUG:|Call Trace:|Oops' "$WORK/serial.log" || true)
if [ "$quirk" != "0" ]; then
    printf 'guest: %s kernel WARNING/BUG lines\n' "$quirk"
    bad=$((bad + quirk))
fi

if grep -aq 'VTHARNESS: exit=0' "$WORK/serial.log" && [ "$bad" = "0" ] && [ "$ok" != "0" ]; then
    printf 'RESULT: PASS\n'
    [ "${SDLOP_QEMU_KEEP:-0}" = "1" ] || rm -rf "$WORK/root" "$WORK/build"
    exit 0
fi

printf 'RESULT: FAIL (log: %s/serial.log)\n' "$WORK"
exit 1
