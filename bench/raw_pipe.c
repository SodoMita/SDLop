/*
  Rootless synthetic evdev transport for bench_raw_latency_pipe.

  Like tests/test_x11_raw_source.c, redirect device discovery to a private
  directory and return a pipe containing native struct input_event records.
  GNU ld --wrap keeps this substitution local to the benchmark executable;
  the real library worker, rings, callback dispatch and SDL APIs run unchanged.
  These are USERSPACE transport timings, not kernel evdev/hardware timings.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include "uinput_synth.h"
#include "input_transport.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct UInputDevice
{
    int read_fd;
    int write_fd;
    char directory[64];
};

static UInputDevice *active;

extern DIR *__real_opendir(const char *path);
extern int __real_open(const char *path, int flags, ...);
extern int __real_open64(const char *path, int flags, ...);
extern int __real_ioctl(int fd, unsigned long request, ...);

DIR *__wrap_opendir(const char *path)
{
    return __real_opendir(active && !strcmp(path, "/dev/input") ? active->directory : path);
}

static int open_synthetic(const char *path)
{
    if (active && !strcmp(path, "/dev/input/event0")) {
        return fcntl(active->read_fd, F_DUPFD_CLOEXEC, 0);
    }
    return -1;
}

int __wrap_open(const char *path, int flags, ...)
{
    if (active && !strcmp(path, "/dev/input/event0")) {
        return open_synthetic(path);
    }
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return __real_open(path, flags, mode);
}

int __wrap_open64(const char *path, int flags, ...)
{
    if (active && !strcmp(path, "/dev/input/event0")) {
        return open_synthetic(path);
    }
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return __real_open64(path, flags, mode);
}

static bool synthetic_fd(int fd)
{
    struct stat candidate, pipe;
    /* Match the pipe, not a cached descriptor number that might be reused
     * after a trial closes its worker-owned duplicate. Setup path only. */
    return active && fstat(fd, &candidate) == 0 && fstat(active->read_fd, &pipe) == 0 &&
           candidate.st_dev == pipe.st_dev && candidate.st_ino == pipe.st_ino;
}

static void set_bit(unsigned char *bits, size_t len, unsigned int bit)
{
    if (bit / 8 < len) {
        bits[bit / 8] |= (unsigned char)(1u << (bit % 8));
    }
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (!synthetic_fd(fd)) {
        return __real_ioctl(fd, request, arg);
    }
    if (_IOC_TYPE(request) == 'E' && _IOC_NR(request) >= 0x20 && _IOC_NR(request) < 0x40) {
        unsigned int ev = _IOC_NR(request) - 0x20;
        size_t len = _IOC_SIZE(request);
        unsigned char *bits = arg;
        memset(bits, 0, len);
        switch (ev) {
        case 0:
            set_bit(bits, len, EV_SYN);
            set_bit(bits, len, EV_KEY);
            set_bit(bits, len, EV_REL);
            break;
        case EV_KEY:
            set_bit(bits, len, KEY_W);
            set_bit(bits, len, KEY_A);
            set_bit(bits, len, BTN_LEFT);
            break;
        case EV_REL:
            set_bit(bits, len, REL_X);
            set_bit(bits, len, REL_Y);
            break;
        }
        return (int)len;
    }
    errno = ENOTTY;
    return -1;
}

UInputDevice *uinput_synth_create(const char *name)
{
    (void)name;
    if (active) {
        errno = EBUSY;
        return NULL;
    }
    UInputDevice *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return NULL;
    }
    dev->read_fd = dev->write_fd = -1;
    snprintf(dev->directory, sizeof(dev->directory), "/tmp/sdlop-raw-pipe-XXXXXX");
    if (!mkdtemp(dev->directory)) {
        free(dev);
        return NULL;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
        uinput_synth_destroy(dev);
        return NULL;
    }
    dev->read_fd = fds[0];
    dev->write_fd = fds[1];
    char path[128];
    snprintf(path, sizeof(path), "%s/event0", dev->directory);
    int file = __real_open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (file < 0) {
        uinput_synth_destroy(dev);
        return NULL;
    }
    close(file);
    active = dev;
    return dev;
}

void uinput_synth_destroy(UInputDevice *dev)
{
    if (!dev) {
        return;
    }
    if (active == dev) {
        active = NULL;
    }
    if (dev->read_fd >= 0) {
        close(dev->read_fd);
    }
    if (dev->write_fd >= 0) {
        close(dev->write_fd);
    }
    char path[128];
    snprintf(path, sizeof(path), "%s/event0", dev->directory);
    unlink(path);
    rmdir(dev->directory);
    free(dev);
}

static bool emit(UInputDevice *dev, uint16_t type, uint16_t code, int32_t value)
{
    if (!dev) {
        errno = EINVAL;
        return false;
    }
    struct input_event event;
    memset(&event, 0, sizeof(event));
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return false;
    }
    event.input_event_sec = ts.tv_sec;
    event.input_event_usec = ts.tv_nsec / 1000;
    event.type = type;
    event.code = code;
    event.value = value;
    return input_write_record(dev->write_fd, &event, sizeof(event));
}

bool uinput_synth_key(UInputDevice *dev, uint16_t code, int32_t value)
{
    return emit(dev, EV_KEY, code, value);
}

bool uinput_synth_rel(UInputDevice *dev, uint16_t code, int32_t value)
{
    return emit(dev, EV_REL, code, value);
}

bool uinput_synth_syn(UInputDevice *dev)
{
    return emit(dev, EV_SYN, SYN_REPORT, 0);
}
