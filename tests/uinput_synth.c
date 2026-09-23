/*
  Tiny synthetic evdev device via /dev/uinput, for tests and latency
  benchmarks. Not part of the library.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "uinput_synth.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct UInputDevice
{
    int fd;
};

UInputDevice *uinput_synth_create(const char *name)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    }
    if (fd < 0) {
        return NULL;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        close(fd);
        return NULL;
    }
    /* a few keys + mouse axes/buttons */
    uint16_t keys[] = { KEY_ESC, KEY_W, KEY_A, KEY_S, KEY_D, KEY_Q, KEY_ENTER, KEY_SPACE };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, keys[i]) < 0) {
            close(fd);
            return NULL;
        }
    }
    uint16_t btns[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, btns[i]) < 0) {
            close(fd);
            return NULL;
        }
    }
    uint16_t rels[] = { REL_X, REL_Y, REL_WHEEL, REL_HWHEEL };
    for (size_t i = 0; i < sizeof(rels) / sizeof(rels[0]); i++) {
        if (ioctl(fd, UI_SET_RELBIT, rels[i]) < 0) {
            close(fd);
            return NULL;
        }
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "%s", name ? name : "sdlop-synth");
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1234;
    setup.id.product = 0x5678;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return NULL;
    }

    UInputDevice *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
        return NULL;
    }
    dev->fd = fd;
    /* give udev/the kernel a moment to create /dev/input/eventN */
    usleep(200 * 1000);
    return dev;
}

void uinput_synth_destroy(UInputDevice *dev)
{
    if (!dev) {
        return;
    }
    ioctl(dev->fd, UI_DEV_DESTROY);
    close(dev->fd);
    free(dev);
}

static void emit(UInputDevice *dev, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        ie.input_event_sec = ts.tv_sec;
        ie.input_event_usec = (suseconds_t)(ts.tv_nsec / 1000);
    }
    ie.type = type;
    ie.code = code;
    ie.value = value;
    ssize_t unused = write(dev->fd, &ie, sizeof(ie));
    (void)unused;
}

void uinput_synth_key(UInputDevice *dev, uint16_t code, int32_t value)
{
    emit(dev, EV_KEY, code, value);
}

void uinput_synth_rel(UInputDevice *dev, uint16_t code, int32_t value)
{
    emit(dev, EV_REL, code, value);
}

void uinput_synth_syn(UInputDevice *dev)
{
    emit(dev, EV_SYN, SYN_REPORT, 0);
}
