# Console-driver review verification

These are VM integration drills, not mock ioctl tests. Run sequentially (each
VM defaults to 1 GiB). Build and VM workdirs should live outside the source tree.

Example setup, using an unpacked Debian kernel and its matching modules:

```sh
cmake -S . -B /tmp/sdlop-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/sdlop-build
export SDLOP_QEMU_BUILD=/tmp/sdlop-build
export SDLOP_QEMU_KERNEL=/tmp/kern/boot/vmlinuz-6.12.107+deb13-amd64
export SDLOP_QEMU_MODULES=/tmp/kern/usr/lib/modules/6.12.107+deb13-amd64
bash tests/run_kmsdrm_qemu.sh /tmp/kms-test
bash tests/run_fbdev_qemu.sh /tmp/fb-test
ctest --test-dir /tmp/sdlop-build --output-on-failure
```

Requires QEMU x86, a static BusyBox with `setsid`/`insmod`, cpio, gzip, xz,
a C compiler and the usual project build dependencies. The kmsdrm harness also
uses static libdrm for its inspection shims and stages shared libraries for the
driver's runtime loading. Chord injection requires `socat`. The scripts' headers
list the supported knobs.

## Recorded results (2026-11-11)

Kernel: Debian 6.12.107+deb13-amd64. QEMU `q35`, two virtual CPUs, no GUI.
Every row below returned `RESULT: PASS`, with no kernel WARN/BUG matches.
Counts are printed assertions, not coverage percentages.

| Harness | Extra environment | Pass / fail |
| --- | --- | --- |
| kmsdrm | default, virtio-gpu | 21 / 0 |
| kmsdrm | `SDLOP_QEMU_RAW=1 SDLOP_QEMU_CHORD=1` | 22 / 0 |
| kmsdrm | `TTYNO=3 SDLOP_KMSDRM_VT_SWITCH=1` | 21 / 0 |
| kmsdrm | `SDLOP_QEMU_GPU=bochs-display,id=gpu0` | 19 / 0 |
| kmsdrm | `SDLOP_KMSDRM_KEEP_MASTER=1` | 21 / 0 |
| kmsdrm | `SDLOP_QEMU_CTTY=1` | 23 / 0 |
| fbdev | `SDLOP_FBDEV_OWNED_CHECK=1` | 69 / 0 (includes 12 owned-VT checks) |
| fbdev | `SDLOP_FBDEV_RAW=1 SDLOP_QEMU_CHORD=1` | 60 / 0 |
| fbdev | `SDLOP_QEMU_CTTY=1` | 58 / 0 |
| fbdev | `SDLOP_QEMU_VGA=792` (24 bpp) | 57 / 0 |
| fbdev | `SDLOP_QEMU_VGA=836` (32 bpp) | 57 / 0 |

fbdev uses `-vga std`; the default VESA mode is 791, **16 bpp**. The serial log
reports the actual `/sys/class/graphics/fb0/bits_per_pixel`, including 24 and 32
for the two alternate-mode runs. No successful cirrus run is claimed.

Host `ctest`: **31/31 passed**. Some host tests return successfully when their
required hardware/display is absent; this is not 31 hardware integration tests.

## What the checks establish

- The default does not activate another VT. The controlling-terminal variant
  launches the test with controlling VT 3 while VT 2 is visible, verifies the
  driver selected VT 3 without switching, then explicitly switches to it.
- With raw input disabled the console keyboard mode is not set to `K_OFF`.
  With raw input enabled, a real evdev keyboard is present and the chord starts
  with `K_OFF`. QEMU injects Ctrl+Alt+F3, and the guest observes a move from VT 2
  to VT 3. This exercises SDLop's forwarding, not just the kernel's normal
  keyboard handler. Other function keys are not individually integration-tested.
- The switch-away and switch-back checks read real VT/keyboard modes. The DRM
  drill reads a pixel from the CRTC's framebuffer, not the application's RAM.
  fbdev reads `/dev/fb0` pixels. Both check restoration on quit.
- The fbdev second-window regression checks distinct RAM surfaces and that
  creating, painting and destroying one leaves the other's pixels intact.
  Cursor tests check drawing, app-buffer isolation and repaint restoration;
  they do not assert the informational pre-repaint cursor-trail count.
- `owned_vt.c` installs `KD_GRAPHICS` + `VT_PROCESS` on VT 2, starts fbdev, and
  reads state through a fresh fd. It checks mode, keyboard mode and VT ownership
  remain intact during and after SDL, and the original signal numbers during
  SDL. This is **fbdev arbitration evidence**, not a kmsdrm arbitration test.
- The software-window master check uses a fresh DRM fd to prove master was
  retained. The keep-master row does not exercise Vulkan handover. Bochs has
  no cursor plane: its cursor-plane phase is reported as a passing explanatory
  note, not as successful hardware-cursor coverage.

## Remaining limits

No physical-hardware run, Vulkan surface/master handover run, nonzero-panning
integration drill, or injected allocation/ioctl/thread-start failure run.
Pixel-layout rejection and panning bounds are code-validated, not covered by
synthetic framebuffer devices. Nonzero panning is honored at device open; live
external panning changes are not tracked. No claim of exhaustive cursor-trail,
signal-mask restoration or leak-sanitizer coverage.
