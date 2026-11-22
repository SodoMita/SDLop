/*
  SDLop - Linux evdev joystick backend.

  Enumerates /dev/input/event* through plain evdev ioctls (no libudev or
  libevdev dependency), reports connected devices to the public joystick /
  gamepad API and pumps axis, button and hat events from SDL_PumpEvents().

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/sdlop_internal.h"
#include "internal/sdlop_joystick_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SDLOP_MAX_EVDEV_JOYSTICKS 64
#define SDLOP_MAX_NAME 128
#define SDLOP_MAX_AXES 16
#define SDLOP_MAX_BUTTONS 64
#define SDLOP_MAX_HATS 4
/* SDL3's fallback detection interval. Inotify normally makes this irrelevant;
 * it is a safety net for missed notifications. */
#define SDLOP_RESCAN_NS (3000ull * 1000000ull)

typedef struct SDLOP_EvdevJoystick
{
    int fd;
    char path[PATH_MAX];
    dev_t rdev;
    SDL_Joystick *joystick;
    int hat_raw[SDLOP_MAX_HATS][2];
    int hat_min[SDLOP_MAX_HATS][2];
    int hat_max[SDLOP_MAX_HATS][2];
    struct SDLOP_EvdevJoystick *next;
} SDLOP_EvdevJoystick;

static SDLOP_EvdevJoystick *devices;
static bool initialized;
static Uint64 last_scan_ns;
static time_t last_input_dir_mtime;
static int inotify_fd = -1;

/* Non-joystick event nodes are expensive to probe: on this system opening and
 * closing all of the keyboard/mouse nodes takes ~140 ms. Remember devices that
 * failed the joystick probe so periodic rescans only stat() them. The
 * inode/ctime test makes a recreated node with the same event number get
 * probed again. */
typedef struct SDLOP_RejectedDevice
{
    char path[PATH_MAX];
    dev_t rdev;
    ino_t ino;
    struct timespec ctime;
    struct SDLOP_RejectedDevice *next;
} SDLOP_RejectedDevice;

static SDLOP_RejectedDevice *rejected;

static bool bit_set(const Uint8 *bits, size_t bytes, unsigned int bit)
{
    if (!bits || bit / 8 >= bytes) {
        return false;
    }
    return (bits[bit / 8] & (1u << (bit % 8))) != 0;
}

static Sint16 scale_axis(int value, int min, int max)
{
    if (max <= min) {
        return 0;
    }
    long long scaled = ((long long)(value - min) * 65535LL) / (max - min) - 32768LL;
    if (scaled < -32768) scaled = -32768;
    if (scaled > 32767) scaled = 32767;
    return (Sint16)scaled;
}

static Uint8 hat_from_raw(int x, int y, int xmin, int xmax, int ymin, int ymax)
{
    Uint8 value = SDL_HAT_CENTERED;
    int xmid = (xmin + xmax) / 2;
    int ymid = (ymin + ymax) / 2;
    int xdead = (xmax - xmin) / 4;
    int ydead = (ymax - ymin) / 4;
    if (x < xmid - xdead) value |= SDL_HAT_LEFT;
    if (x > xmid + xdead) value |= SDL_HAT_RIGHT;
    if (y < ymid - ydead) value |= SDL_HAT_UP;
    if (y > ymid + ydead) value |= SDL_HAT_DOWN;
    return value;
}

static SDL_GamepadType gamepad_type_from_name(const char *name)
{
    if (!name) {
        return SDL_GAMEPAD_TYPE_STANDARD;
    }
    if (strstr(name, "DualSense") || strstr(name, "PS5")) return SDL_GAMEPAD_TYPE_PS5;
    if (strstr(name, "DualShock 4") || strstr(name, "PS4")) return SDL_GAMEPAD_TYPE_PS4;
    if (strstr(name, "DualShock 3") || strstr(name, "PS3")) return SDL_GAMEPAD_TYPE_PS3;
    if (strstr(name, "Switch Pro")) return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
    if (strstr(name, "Joy-Con (L)")) return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT;
    if (strstr(name, "Joy-Con (R)")) return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT;
    if (strstr(name, "Xbox One") || strstr(name, "Xbox Series")) return SDL_GAMEPAD_TYPE_XBOXONE;
    if (strstr(name, "Xbox")) return SDL_GAMEPAD_TYPE_XBOX360;
    if (strstr(name, "GameCube")) return SDL_GAMEPAD_TYPE_GAMECUBE;
    if (strstr(name, "Steam")) return SDL_GAMEPAD_TYPE_STEAM;
    return SDL_GAMEPAD_TYPE_STANDARD;
}

static bool duplicate_button(Uint16 *buttons, int count, Uint16 code)
{
    for (int i = 0; i < count; ++i) {
        if (buttons[i] == code) return true;
    }
    return false;
}

/* Open one event node and describe it. Returns true only for devices that
   look like joysticks or gamepads; keyboards, mice and touch devices are
   deliberately left to SDLop's keyboard/mouse backends. */
static bool probe_device(const char *path, SDLop_JoystickDeviceDesc *desc, char *name_out,
                         SDLop_JoystickAxisInfo *axes, Sint16 *axes_initial, int *num_axes,
                         Uint16 *buttons, bool *buttons_initial, int *num_buttons,
                         SDLop_JoystickHatInfo *hats, Uint8 *hats_initial,
                         int hat_min[SDLOP_MAX_HATS][2], int hat_max[SDLOP_MAX_HATS][2],
                         int *num_hats, int fd)
{
    static const Uint16 axis_codes[] = {
        ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ,
        ABS_THROTTLE, ABS_RUDDER, ABS_WHEEL, ABS_GAS, ABS_BRAKE
    };
    static const Uint16 hat_codes[SDLOP_MAX_HATS][2] = {
        { ABS_HAT0X, ABS_HAT0Y }, { ABS_HAT1X, ABS_HAT1Y },
        { ABS_HAT2X, ABS_HAT2Y }, { ABS_HAT3X, ABS_HAT3Y }
    };
    static const Uint16 button_codes[] = {
        BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH,
        BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE,
        BTN_THUMBL, BTN_THUMBR,
        BTN_TRIGGER, BTN_THUMB, BTN_THUMB2, BTN_TOP, BTN_TOP2,
        BTN_PINKIE, BTN_BASE, BTN_BASE2, BTN_BASE3, BTN_BASE4, BTN_BASE5, BTN_BASE6,
        BTN_TRIGGER_HAPPY1, BTN_TRIGGER_HAPPY2, BTN_TRIGGER_HAPPY3, BTN_TRIGGER_HAPPY4,
        BTN_TRIGGER_HAPPY5, BTN_TRIGGER_HAPPY6, BTN_TRIGGER_HAPPY7, BTN_TRIGGER_HAPPY8,
        BTN_TRIGGER_HAPPY9, BTN_TRIGGER_HAPPY10, BTN_TRIGGER_HAPPY11, BTN_TRIGGER_HAPPY12,
        BTN_TRIGGER_HAPPY13, BTN_TRIGGER_HAPPY14, BTN_TRIGGER_HAPPY15, BTN_TRIGGER_HAPPY16,
        BTN_TRIGGER_HAPPY17, BTN_TRIGGER_HAPPY18, BTN_TRIGGER_HAPPY19, BTN_TRIGGER_HAPPY20
    };

    Uint8 abs_bits[(ABS_MAX + 8) / 8];
    Uint8 key_bits[(KEY_MAX + 8) / 8];
    Uint8 key_state[(KEY_MAX + 8) / 8];
    char name[128];
    struct input_id ids;
    SDL_zero(abs_bits);
    SDL_zero(key_bits);
    SDL_zero(key_state);
    SDL_zero(name);
    SDL_zero(ids);

    if (ioctl(fd, EVIOCGID, &ids) < 0 ||
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0 ||
        ioctl(fd, EVIOCGKEY(sizeof(key_state)), key_state) < 0) {
        return false;
    }

    bool has_gamepad_button = bit_set(key_bits, sizeof(key_bits), BTN_GAMEPAD) ||
                              bit_set(key_bits, sizeof(key_bits), BTN_SOUTH) ||
                              bit_set(key_bits, sizeof(key_bits), BTN_A) ||
                              bit_set(key_bits, sizeof(key_bits), BTN_START) ||
                              bit_set(key_bits, sizeof(key_bits), BTN_SELECT);
    bool has_joystick_button = bit_set(key_bits, sizeof(key_bits), BTN_JOYSTICK) ||
                               bit_set(key_bits, sizeof(key_bits), BTN_TRIGGER) ||
                               has_gamepad_button;
    if (!has_joystick_button) {
        return false;
    }

    *num_axes = 0;
    for (size_t i = 0; i < SDL_arraysize(axis_codes); ++i) {
        Uint16 code = axis_codes[i];
        if (*num_axes >= SDLOP_MAX_AXES || !bit_set(abs_bits, sizeof(abs_bits), code)) {
            continue;
        }
        struct input_absinfo info;
        SDL_zero(info);
        if (ioctl(fd, EVIOCGABS(code), &info) < 0 || info.maximum <= info.minimum) {
            continue;
        }
        axes[*num_axes].code = code;
        axes[*num_axes].min = info.minimum;
        axes[*num_axes].max = info.maximum;
        axes_initial[*num_axes] = scale_axis(info.value, info.minimum, info.maximum);
        ++*num_axes;
    }

    *num_hats = 0;
    for (size_t i = 0; i < SDL_arraysize(hat_codes); ++i) {
        if (*num_hats >= SDLOP_MAX_HATS ||
            !bit_set(abs_bits, sizeof(abs_bits), hat_codes[i][0]) ||
            !bit_set(abs_bits, sizeof(abs_bits), hat_codes[i][1])) {
            continue;
        }
        struct input_absinfo xinfo, yinfo;
        SDL_zero(xinfo);
        SDL_zero(yinfo);
        if (ioctl(fd, EVIOCGABS(hat_codes[i][0]), &xinfo) < 0 ||
            ioctl(fd, EVIOCGABS(hat_codes[i][1]), &yinfo) < 0 ||
            xinfo.maximum < xinfo.minimum || yinfo.maximum < yinfo.minimum) {
            continue;
        }
        int h = *num_hats;
        hats[h].xcode = hat_codes[i][0];
        hats[h].ycode = hat_codes[i][1];
        hat_min[h][0] = xinfo.minimum; hat_max[h][0] = xinfo.maximum;
        hat_min[h][1] = yinfo.minimum; hat_max[h][1] = yinfo.maximum;
        hats_initial[h] = hat_from_raw(xinfo.value, yinfo.value,
                                       xinfo.minimum, xinfo.maximum,
                                       yinfo.minimum, yinfo.maximum);
        ++*num_hats;
    }

    *num_buttons = 0;
    for (size_t i = 0; i < SDL_arraysize(button_codes); ++i) {
        Uint16 code = button_codes[i];
        if (*num_buttons >= SDLOP_MAX_BUTTONS ||
            !bit_set(key_bits, sizeof(key_bits), code) ||
            duplicate_button(buttons, *num_buttons, code)) {
            continue;
        }
        buttons[*num_buttons] = code;
        buttons_initial[*num_buttons] = bit_set(key_state, sizeof(key_state), code);
        ++*num_buttons;
    }

    if (*num_axes < 2 && *num_hats == 0) {
        return false; /* not a joystick-like absolute device */
    }
    if (*num_buttons == 0) {
        return false;
    }

    bool is_gamepad = has_gamepad_button && (*num_axes >= 2 || *num_hats > 0);
    SDL_zero(*desc);
    /* desc is consumed after probe_device returns; copy the kernel name into
       caller-owned storage instead of leaving a dangling stack pointer. */
    SDL_snprintf(name_out, SDLOP_MAX_NAME, "%s", name[0] ? name : "Unknown Joystick");
    desc->name = name_out;
    desc->path = path;
    desc->bus = ids.bustype;
    desc->vendor = ids.vendor;
    desc->product = ids.product;
    desc->version = ids.version;
    desc->type = is_gamepad ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
    desc->is_gamepad = is_gamepad;
    desc->gamepad_type = gamepad_type_from_name(name);
    desc->axes = axes;
    desc->num_axes = *num_axes;
    desc->axes_initial = axes_initial;
    desc->buttons = buttons;
    desc->num_buttons = *num_buttons;
    desc->buttons_initial = buttons_initial;
    desc->hats = hats;
    desc->num_hats = *num_hats;
    desc->hats_initial = hats_initial;
    return true;
}

static bool device_known(const char *path, dev_t rdev)
{
    for (SDLOP_EvdevJoystick *d = devices; d; d = d->next) {
        if ((rdev && d->rdev == rdev) || strcmp(d->path, path) == 0) {
            return true;
        }
    }
    return false;
}

static bool rejected_matches(const SDLOP_RejectedDevice *r, const struct stat *st)
{
    return r->rdev == st->st_rdev && r->ino == st->st_ino &&
           r->ctime.tv_sec == st->st_ctim.tv_sec &&
           r->ctime.tv_nsec == st->st_ctim.tv_nsec;
}

static bool rejected_known(const char *path, const struct stat *st)
{
    for (SDLOP_RejectedDevice *r = rejected; r; r = r->next) {
        if (strcmp(r->path, path) == 0 && rejected_matches(r, st)) {
            return true;
        }
    }
    return false;
}

static void reject_device(const char *path, const struct stat *st)
{
    if (rejected_known(path, st)) {
        return;
    }
    SDLOP_RejectedDevice *r = (SDLOP_RejectedDevice *)SDL_calloc(1, sizeof(*r));
    if (!r) {
        return;
    }
    SDL_snprintf(r->path, sizeof(r->path), "%s", path);
    r->rdev = st->st_rdev;
    r->ino = st->st_ino;
    r->ctime = st->st_ctim;
    r->next = rejected;
    rejected = r;
}

static void prune_rejected_devices(void)
{
    for (SDLOP_RejectedDevice **link = &rejected; *link;) {
        SDLOP_RejectedDevice *r = *link;
        struct stat st;
        if (stat(r->path, &st) != 0 || !S_ISCHR(st.st_mode) || !rejected_matches(r, &st)) {
            *link = r->next;
            SDL_free(r);
        } else {
            link = &r->next;
        }
    }
}

static void clear_rejected_devices(void)
{
    while (rejected) {
        SDLOP_RejectedDevice *r = rejected;
        rejected = r->next;
        SDL_free(r);
    }
}

static void forget_rejected_device(const char *path)
{
    for (SDLOP_RejectedDevice **link = &rejected; *link;) {
        SDLOP_RejectedDevice *r = *link;
        if (strcmp(r->path, path) == 0) {
            *link = r->next;
            SDL_free(r);
        } else {
            link = &r->next;
        }
    }
}

static bool is_event_node(const char *name)
{
    if (!name || strncmp(name, "event", 5) != 0 || name[5] < '0' || name[5] > '9') {
        return false;
    }
    for (const char *p = name + 5; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

static void remove_device(SDLOP_EvdevJoystick *dev)
{
    SDL_LockJoysticks();
    SDLOP_EvdevJoystick **link = &devices;
    while (*link && *link != dev) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = dev->next;
    }
    SDL_UnlockJoysticks();

    SDL_Joystick *joystick = dev->joystick;
    if (joystick) {
        joystick->driverdata = NULL;
    }
    if (dev->fd >= 0) {
        close(dev->fd);
    }
    SDL_free(dev);
    if (joystick) {
        SDLOP_JoystickRemoveDevice(joystick);
    }
}

static void remove_device_by_path(const char *path)
{
    for (SDLOP_EvdevJoystick *d = devices; d; d = d->next) {
        if (strcmp(d->path, path) == 0) {
            remove_device(d);
            return;
        }
    }
}

static void add_device(const char *path)
{
    struct stat st;
    dev_t rdev = 0;
    bool have_stat = (stat(path, &st) == 0 && S_ISCHR(st.st_mode));
    if (have_stat) {
        rdev = st.st_rdev;
    }
    if (device_known(path, rdev)) {
        return;
    }
    if (have_stat && rejected_known(path, &st)) {
        return;
    }

    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return;
    }

    SDLop_JoystickDeviceDesc desc;
    char name[SDLOP_MAX_NAME];
    SDLop_JoystickAxisInfo axes[SDLOP_MAX_AXES];
    SDLop_JoystickHatInfo hats[SDLOP_MAX_HATS];
    Sint16 axes_initial[SDLOP_MAX_AXES];
    Uint16 buttons[SDLOP_MAX_BUTTONS];
    bool buttons_initial[SDLOP_MAX_BUTTONS];
    Uint8 hats_initial[SDLOP_MAX_HATS];
    int hat_min[SDLOP_MAX_HATS][2], hat_max[SDLOP_MAX_HATS][2];
    int num_axes = 0, num_buttons = 0, num_hats = 0;
    if (!probe_device(path, &desc, name, axes, axes_initial, &num_axes,
                      buttons, buttons_initial, &num_buttons,
                      hats, hats_initial, hat_min, hat_max, &num_hats, fd)) {
        if (have_stat) {
            reject_device(path, &st);
        }
        close(fd);
        return;
    }

    SDLOP_EvdevJoystick *dev = (SDLOP_EvdevJoystick *)SDL_calloc(1, sizeof(*dev));
    if (!dev) {
        close(fd);
        return;
    }
    dev->fd = fd;
    dev->rdev = rdev;
    SDL_snprintf(dev->path, sizeof(dev->path), "%s", path);
    for (int i = 0; i < num_hats; ++i) {
        dev->hat_min[i][0] = hat_min[i][0];
        dev->hat_max[i][0] = hat_max[i][0];
        dev->hat_min[i][1] = hat_min[i][1];
        dev->hat_max[i][1] = hat_max[i][1];
        dev->hat_raw[i][0] = hat_min[i][0];
        dev->hat_raw[i][1] = hat_min[i][1];
    }
    dev->joystick = SDLOP_JoystickAddDevice(&desc, dev);
    if (!dev->joystick) {
        close(fd);
        SDL_free(dev);
        return;
    }
    SDL_LockJoysticks();
    dev->next = devices;
    devices = dev;
    SDL_UnlockJoysticks();
}

static void scan_devices(void)
{
    struct stat dir_st;
    bool have_dir_st = (stat("/dev/input", &dir_st) == 0);

    last_scan_ns = SDL_GetTicksNS();
    if (have_dir_st) {
        last_input_dir_mtime = dir_st.st_mtime;
    }

    /* Drop cached negative probes for nodes that vanished or were recreated. */
    prune_rejected_devices();

    /* Remove devices whose node disappeared. */
    for (SDLOP_EvdevJoystick *dev = devices; dev;) {
        SDLOP_EvdevJoystick *next = dev->next;
        struct stat st;
        if (stat(dev->path, &st) != 0 || (dev->rdev && st.st_rdev != dev->rdev)) {
            remove_device(dev);
        }
        dev = next;
    }

    DIR *dir = opendir("/dev/input");
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_event_node(entry->d_name)) {
            continue;
        }
        char path[PATH_MAX];
        SDL_snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        add_device(path);
    }
    closedir(dir);
}

/* SDL3's fallback: only walk /dev/input again when the directory changed, and
 * never more often than the detection interval. Inotify makes this a safety
 * net rather than the primary hotplug mechanism. */
static void maybe_scan_devices(void)
{
    Uint64 now = SDL_GetTicksNS();
    if (now - last_scan_ns < SDLOP_RESCAN_NS) {
        return;
    }
    struct stat st;
    if (stat("/dev/input", &st) == 0 && st.st_mtime == last_input_dir_mtime) {
        last_scan_ns = now;
        return;
    }
    scan_devices();
}

static void init_inotify(void)
{
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd < 0) {
        return;
    }
    /* SDL3 also watches IN_ATTRIB: udev may change permissions after node
     * creation, which is exactly when a previously unreadable node becomes
     * openable. */
    if (inotify_add_watch(inotify_fd, "/dev/input",
                          IN_CREATE | IN_DELETE | IN_MOVE | IN_ATTRIB) < 0) {
        close(inotify_fd);
        inotify_fd = -1;
    }
}

static void quit_inotify(void)
{
    if (inotify_fd >= 0) {
        close(inotify_fd);
        inotify_fd = -1;
    }
}

static void drain_inotify(void)
{
    if (inotify_fd < 0) {
        return;
    }

    union
    {
        struct inotify_event event;
        char storage[4096];
    } buf;
    bool overflow = false;

    for (;;) {
        ssize_t bytes = read(inotify_fd, &buf, sizeof(buf));
        if (bytes <= 0) {
            break;
        }
        size_t remain = (size_t)bytes;
        char *p = buf.storage;
        while (remain >= sizeof(struct inotify_event)) {
            struct inotify_event *ev = (struct inotify_event *)p;
            size_t len = sizeof(struct inotify_event) + ev->len;
            if (len > remain) {
                break;
            }
            if (ev->mask & IN_Q_OVERFLOW) {
                overflow = true;
            } else if (ev->len > 0 && is_event_node(ev->name)) {
                char path[PATH_MAX];
                SDL_snprintf(path, sizeof(path), "/dev/input/%s", ev->name);
                if (ev->mask & (IN_CREATE | IN_MOVED_TO | IN_ATTRIB)) {
                    forget_rejected_device(path);
                    add_device(path);
                } else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
                    forget_rejected_device(path);
                    remove_device_by_path(path);
                }
            }
            p += len;
            remain -= len;
        }
    }
    if (overflow) {
        scan_devices();
    }
}

static bool read_events(SDLOP_EvdevJoystick *dev)
{
    struct input_event events[64];
    ssize_t got = read(dev->fd, events, sizeof(events));
    if (got <= 0) {
        return false;
    }
    SDL_Joystick *joystick = dev->joystick;
    if (!joystick) {
        return false;
    }
    bool changed = false;
    int count = (int)(got / sizeof(events[0]));
    for (int i = 0; i < count; ++i) {
        const struct input_event *ev = &events[i];
        if (ev->type == EV_ABS) {
            bool is_x = false;
            int hat = SDLOP_JoystickHatIndex(joystick, ev->code, &is_x);
            if (hat >= 0 && hat < SDLOP_MAX_HATS) {
                dev->hat_raw[hat][is_x ? 0 : 1] = ev->value;
                Uint8 value = hat_from_raw(dev->hat_raw[hat][0], dev->hat_raw[hat][1],
                                           dev->hat_min[hat][0], dev->hat_max[hat][0],
                                           dev->hat_min[hat][1], dev->hat_max[hat][1]);
                SDLOP_JoystickSendHat(joystick, (Uint8)hat, value);
                changed = true;
            } else {
                int axis = SDLOP_JoystickAxisIndex(joystick, ev->code);
                if (axis >= 0) {
                    SDLOP_JoystickSendAxis(joystick, (Uint8)axis,
                                           scale_axis(ev->value, joystick->axis_min[axis], joystick->axis_max[axis]));
                    changed = true;
                }
            }
        } else if (ev->type == EV_KEY) {
            int button = SDLOP_JoystickButtonIndex(joystick, ev->code);
            if (button >= 0) {
                SDLOP_JoystickSendButton(joystick, (Uint8)button, ev->value != 0);
                changed = true;
            }
        }
    }
    return changed;
}

bool SDLOP_InitJoysticks(void)
{
    if (initialized) {
        return true;
    }
    initialized = true;
    last_scan_ns = 0;
    init_inotify();
    scan_devices();
    return true;
}

void SDLOP_QuitJoysticks(void)
{
    if (!initialized) {
        return;
    }
    while (devices) {
        remove_device(devices);
    }
    clear_rejected_devices();
    quit_inotify();
    SDLOP_JoystickQuitAllDevices();
    initialized = false;
}

bool SDLOP_JoysticksInitialized(void)
{
    return initialized;
}

int SDLOP_JoystickGetEventFDs(int *fds, int max_fds)
{
    int count = 0;
    if (inotify_fd >= 0 && max_fds > 0) {
        fds[count++] = inotify_fd;
    }
    for (SDLOP_EvdevJoystick *d = devices; d && count < max_fds; d = d->next) {
        fds[count++] = d->fd;
    }
    return count;
}

void SDLOP_JoystickPump(void)
{
    if (!initialized) {
        return;
    }
    drain_inotify();
    maybe_scan_devices();

    struct pollfd pfds[SDLOP_MAX_EVDEV_JOYSTICKS];
    SDLOP_EvdevJoystick *devs[SDLOP_MAX_EVDEV_JOYSTICKS];
    int count = 0;
    for (SDLOP_EvdevJoystick *d = devices; d && count < SDLOP_MAX_EVDEV_JOYSTICKS; d = d->next) {
        pfds[count].fd = d->fd;
        pfds[count].events = POLLIN;
        pfds[count].revents = 0;
        devs[count] = d;
        ++count;
    }
    if (count == 0 || poll(pfds, (nfds_t)count, 0) <= 0) {
        return;
    }

    bool any_changed = false;
    for (int i = 0; i < count; ++i) {
        if (pfds[i].revents & POLLIN) {
            any_changed |= read_events(devs[i]);
        }
        if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            remove_device(devs[i]);
            devs[i] = NULL; /* remove_device() freed it */
        }
    }
    if (any_changed) {
        for (int i = 0; i < count; ++i) {
            if (!devs[i] || !devs[i]->joystick) continue;
            SDL_Joystick *joystick = devs[i]->joystick;
            SDLOP_JoystickSendDeviceEvent(joystick, SDL_EVENT_JOYSTICK_UPDATE_COMPLETE);
            if (joystick->is_gamepad) {
                SDLOP_JoystickSendDeviceEvent(joystick, SDL_EVENT_GAMEPAD_UPDATE_COMPLETE);
            }
        }
    }
}
