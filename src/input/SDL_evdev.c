/*
  SDLop -- the async evdev input worker.

  One background thread owns every /dev/input/event* device, waits in epoll()
  and pushes raw records into the lock-free ring. This is the part that makes
  input "async": the thread is blocked in the kernel, so the moment a key or a
  mouse move happens the kernel wakes it up, it does one read() of up to 64
  events and returns to epoll. Nothing in the path takes a lock, allocates, or
  calls back into SDL, so an application that is busy rendering cannot add
  latency to input, and a slow application cannot stall input either (the ring
  absorbs the burst).

  Compared with SDL3's linux evdev support:
    * no libudev: hotplug is handled by re-scanning /dev/input once a second in
      the epoll timeout - no extra library, no dbus, no ~2 MB of code;
    * the devices are registered with epoll_event.data.ptr pointing straight at
      the device, so there is no per-event lookup table;
    * events are timestamped with CLOCK_MONOTONIC in the kernel (EVIOCSCLOCKID),
      which is the same clock SDL_GetTicksNS() uses, so the timestamps can be
      compared with zero arithmetic.

  Layout of the thread's data: everything is fixed size and preallocated at
  startup; the hot path performs no allocation at all.
*/

#include "../sdlop_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <linux/input.h>

#define SDLOP_MAX_INPUT_DEVICES 32
#define SDLOP_EVENT_BATCH 64
#define SDLOP_RESCAN_INTERVAL_MS 1000

typedef struct SDLOP_EvdevDevice
{
    int fd;
    Uint32 id;
    Uint8 cls;
    char path[64];
    char name[128];
    bool mouse_seen;
} SDLOP_EvdevDevice;

static pthread_t sdlop_input_thread;
static int sdlop_epoll_fd = -1;
static SDLOP_EvdevDevice sdlop_devices[SDLOP_MAX_INPUT_DEVICES];
static int sdlop_num_devices;
static Uint32 sdlop_next_device_id = 1;
static bool sdlop_evdev_running;
static pthread_mutex_t sdlop_evdev_lock = PTHREAD_MUTEX_INITIALIZER;
static Sint64 sdlop_timestamp_offset;      /* kernel monotonic ns -> SDL ticks ns */
static bool sdlop_evdev_quit;

/* ------------------------------------------------------------------------- */
/* Device discovery                                                          */
/* ------------------------------------------------------------------------- */

static bool sdlop_device_is_mouse(int fd)
{
    unsigned long bits;
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(bits)), &bits) < 0) {
        return false;
    }
    return (bits & ((1UL << REL_X) | (1UL << REL_Y))) != 0;
}

static bool sdlop_device_is_keyboard(int fd)
{
    unsigned long keys[KEY_MAX / (8 * sizeof(unsigned long)) + 1];

    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) {
        return false;
    }
#define SDLOP_HAS_KEY(k) (keys[(k) / (8 * sizeof(unsigned long))] & \
                          (1UL << ((k) % (8 * sizeof(unsigned long)))))
    return SDLOP_HAS_KEY(KEY_A) || SDLOP_HAS_KEY(KEY_Z) || SDLOP_HAS_KEY(KEY_ESC);
#undef SDLOP_HAS_KEY
}

static void sdlop_scan_devices(void)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *entry;

    if (!dir) {
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        int fd, index;
        char path[128];

        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        if (sdlop_num_devices >= SDLOP_MAX_INPUT_DEVICES) {
            break;
        }
        SDL_snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

        for (index = 0; index < sdlop_num_devices; index++) {
            if (strcmp(sdlop_devices[index].path, path) == 0) {
                break;
            }
        }
        if (index < sdlop_num_devices) {
            continue;                        /* already known */
        }

        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;                        /* not readable: needs privileges */
        }
        {
            int clock_id = CLOCK_MONOTONIC;
            /* kernel timestamps in the same clock as SDL_GetTicksNS() */
            ioctl(fd, EVIOCSCLOCKID, &clock_id);
        }

        {
            SDLOP_EvdevDevice *device = &sdlop_devices[sdlop_num_devices];
            memset(device, 0, sizeof(*device));
            device->fd = fd;
            device->id = sdlop_next_device_id++;
            SDL_strlcpy(device->path, path, sizeof(device->path));
            if (ioctl(fd, EVIOCGNAME(sizeof(device->name) - 1), device->name) < 0) {
                device->name[0] = '\0';
            }
            if (sdlop_device_is_mouse(fd)) {
                device->cls = SDLOP_DEVICE_MOUSE;
            } else if (sdlop_device_is_keyboard(fd)) {
                device->cls = SDLOP_DEVICE_KEYBOARD;
            } else {
                close(fd);
                continue;                    /* touchscreen, tablet, ...: out of scope */
            }

            {
                struct epoll_event ev;
                memset(&ev, 0, sizeof(ev));
                ev.events = EPOLLIN;
                ev.data.ptr = device;        /* direct pointer: no lookup table */
                if (epoll_ctl(sdlop_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                    close(fd);
                    continue;
                }
            }
            sdlop_num_devices++;
            SDLOP_OnDeviceAdded(device->id, (SDLOP_InputDeviceClass)device->cls, device->name);
        }
    }
    closedir(dir);
}

static void sdlop_remove_device(SDLOP_EvdevDevice *device)
{
    int index = (int)(device - sdlop_devices);

    epoll_ctl(sdlop_epoll_fd, EPOLL_CTL_DEL, device->fd, NULL);
    close(device->fd);
    device->fd = -1;
    SDLOP_OnDeviceRemoved(device->id);
    if (index < sdlop_num_devices - 1) {
        memmove(&sdlop_devices[index], &sdlop_devices[index + 1],
                (size_t)(sdlop_num_devices - index - 1) * sizeof(sdlop_devices[0]));
    }
    sdlop_num_devices--;
}

/* ------------------------------------------------------------------------- */
/* Reader thread                                                             */
/* ------------------------------------------------------------------------- */

/* Kernel CLOCK_MONOTONIC nanoseconds -> SDL_GetTicksNS() domain. */
static Uint64 sdlop_kernel_ns_to_sdl(Uint64 kernel_ns)
{
    Sint64 converted = (Sint64)kernel_ns + sdlop_timestamp_offset;
    return converted > 0 ? (Uint64)converted : 0;
}

static void *sdlop_input_thread_main(void *arg)
{
    struct epoll_event events[SDLOP_MAX_INPUT_DEVICES];
    struct input_event buffer[SDLOP_EVENT_BATCH];
    Uint64 last_scan = 0;

    (void)arg;
    SDLOP_LogInfo("sdlop: async input thread started (%d device%s)", sdlop_num_devices,
                  sdlop_num_devices == 1 ? "" : "s");

    while (!sdlop_evdev_quit) {
        int n = epoll_wait(sdlop_epoll_fd, events, SDLOP_MAX_INPUT_DEVICES, 200);
        int i;
        bool pushed = false;

        for (i = 0; i < n; i++) {
            SDLOP_EvdevDevice *device = (SDLOP_EvdevDevice *)events[i].data.ptr;
            ssize_t bytes;

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                sdlop_remove_device(device);
                continue;
            }

            bytes = read(device->fd, buffer, sizeof(buffer));
            if (bytes <= 0) {
                if (bytes < 0 && errno != EAGAIN && errno != EINTR) {
                    sdlop_remove_device(device);
                }
                continue;
            }

            {
                ssize_t count = bytes / (ssize_t)sizeof(struct input_event);
                ssize_t j;
                for (j = 0; j < count; j++) {
                    SDLOP_RawInputRecord record;
                    Uint64 kernel_ns;

                    if (buffer[j].type == EV_SYN || buffer[j].type == EV_MSC) {
                        continue;
                    }
                    kernel_ns = (Uint64)buffer[j].input_event_sec * 1000000000ULL +
                                (Uint64)buffer[j].input_event_usec * 1000ULL;
                    memset(&record, 0, sizeof(record));
                    record.timestamp_ns = sdlop_kernel_ns_to_sdl(kernel_ns);
                    record.device_id = device->id;
                    record.type = buffer[j].type;
                    record.code = buffer[j].code;
                    record.value = buffer[j].value;
                    record.device_class = device->cls;
                    record.from_evdev = true;
                    SDLOP_PushRawInput(&record);
                    pushed = true;
                }
            }
        }

        if (pushed) {
            SDLOP_SignalEvents();
        }

        /* hotplug: a full rescan is ~10 syscalls and only happens once a second */
        if (SDL_GetTicks() - last_scan > SDLOP_RESCAN_INTERVAL_MS) {
            last_scan = SDL_GetTicks();
            sdlop_scan_devices();
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Startup / shutdown                                                        */
/* ------------------------------------------------------------------------- */

static bool sdlop_evdev_init(void)
{
    const char *hint = SDL_GetHint("SDL_EVDEV_DEVICES");
    struct epoll_event ev;
    int fd;

    SDLOP_MaybeInitRawInput();

    sdlop_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (sdlop_epoll_fd < 0) {
        return SDL_SetError("epoll_create1() failed: %s", strerror(errno));
    }
    (void)hint;

    sdlop_scan_devices();
    if (sdlop_num_devices == 0) {
        close(sdlop_epoll_fd);
        sdlop_epoll_fd = -1;
        return false;
    }

    /* one eventfd so the thread can be woken to quit promptly */
    fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;
    if (fd >= 0) {
        epoll_ctl(sdlop_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }

    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        sdlop_timestamp_offset = (Sint64)SDL_GetTicksNS() -
                                 (Sint64)((Uint64)ts.tv_sec * 1000000000ULL + (Uint64)ts.tv_nsec);
    }

    sdlop_evdev_quit = false;
    if (pthread_create(&sdlop_input_thread, NULL, sdlop_input_thread_main, NULL) != 0) {
        close(sdlop_epoll_fd);
        sdlop_epoll_fd = -1;
        return SDL_SetError("Couldn't create the input thread");
    }
    sdlop_evdev_running = true;
    return true;
}

bool SDLOP_InitAsyncInput(void)
{
    const char *test_path;
    bool ok;

    pthread_mutex_lock(&sdlop_evdev_lock);
    if (sdlop_evdev_running) {
        pthread_mutex_unlock(&sdlop_evdev_lock);
        return true;
    }

    test_path = getenv("SDLOP_TEST_INPUT");
    if (test_path && test_path[0]) {
        ok = SDLOP_InitTestInput(test_path);
    } else {
        ok = sdlop_evdev_init();
        if (!ok) {
            /* No readable /dev/input (XWayland, flatpak, no privileges): the
               windowing backend keeps delivering input itself. Not an error. */
            SDLOP_LogDebug("sdlop: no evdev devices readable, using platform input");
            SDL_ClearError();
        }
    }
    pthread_mutex_unlock(&sdlop_evdev_lock);
    return true;
}

void SDLOP_QuitAsyncInput(void)
{
    pthread_mutex_lock(&sdlop_evdev_lock);
    if (sdlop_evdev_running) {
        sdlop_evdev_quit = true;
        pthread_join(sdlop_input_thread, NULL);
        sdlop_evdev_running = false;
    }
    if (sdlop_epoll_fd >= 0) {
        close(sdlop_epoll_fd);
        sdlop_epoll_fd = -1;
    }
    for (int i = 0; i < sdlop_num_devices; i++) {
        if (sdlop_devices[i].fd >= 0) {
            close(sdlop_devices[i].fd);
        }
    }
    sdlop_num_devices = 0;
    SDLOP_QuitTestInput();
    pthread_mutex_unlock(&sdlop_evdev_lock);
}
