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
/* Joystick with the given absolute axes and joystick-range buttons. */
UInputDevice *uinput_synth_create_joystick(const char *name,
                                           const uint16_t *axes, int num_axes,
                                           const uint16_t *buttons, int num_buttons);
void uinput_synth_destroy(UInputDevice *dev);

/* True only when the complete native record was written. EINTR retries;
 * backpressure waits are bounded. On failure false is returned with errno. */
bool uinput_synth_key(UInputDevice *dev, uint16_t code, int32_t value);
bool uinput_synth_abs(UInputDevice *dev, uint16_t code, int32_t value);
bool uinput_synth_rel(UInputDevice *dev, uint16_t code, int32_t value);
bool uinput_synth_syn(UInputDevice *dev);

#endif /* uinput_synth_h_ */
