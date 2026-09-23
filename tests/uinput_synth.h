/*
  Tiny synthetic evdev device via /dev/uinput, for tests and latency
  benchmarks. Not part of the library.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef uinput_synth_h_
#define uinput_synth_h_

#include <stdbool.h>
#include <stdint.h>

typedef struct UInputDevice UInputDevice;

/* Returns NULL when /dev/uinput is unavailable (test should skip). */
UInputDevice *uinput_synth_create(const char *name);
void uinput_synth_destroy(UInputDevice *dev);

void uinput_synth_key(UInputDevice *dev, uint16_t code, int32_t value);
void uinput_synth_rel(UInputDevice *dev, uint16_t code, int32_t value);
void uinput_synth_syn(UInputDevice *dev);

#endif /* uinput_synth_h_ */
