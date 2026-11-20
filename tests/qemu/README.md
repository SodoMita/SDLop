# Console-driver review verification

These are VM integration drills, not mock ioctl tests. Run sequentially (each
VM defaults to 1 GiB). Build and VM workdirs should live outside the source tree.

Example setup, using an unpacked Debian kernel and its matching modules:

```sh
cmake -S . -B /tmp/sdlop-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/sdlop-build
export SDLOP_QEMU_BUILD=/tmp/sdlop-build
export SDLOP_QEMU_KERNEL=/tmp/kern/boot/vmlinuz-6.12.117+deb13-amd64
export SDLOP_QEMU_MODULES=/tmp/kern/usr/lib/modules/6.12.117+deb13-amd64
bash tests/run_kmsdrm_qemu.sh /tmp/kms-test
SDLOP_FBDEV_OWNED_CHECK=1 bash tests/run_fbdev_qemu.sh /tmp/fb-test
xvfb-run -a -s '-screen 0 1280x1024x24 -ac' \
  env SDLOP_TEST_STRICT=1 ctest --test-dir /tmp/sdlop-build --output-on-failure
```

Requires QEMU x86, a static BusyBox with `setsid`/`insmod`, cpio, gzip, xz,
a C compiler and the usual project build dependencies. The kmsdrm harness also
uses static libdrm for its inspection shims and stages shared libraries for the
driver's runtime loading. Chord injection requires `socat`. The scripts' headers
list the supported knobs. The uinput row needs the guest's `uinput` module.

## Recorded results — audit follow-up to `69644f2`

Kernel: Debian **6.12.117+deb13-amd64**. QEMU `q35`, two virtual CPUs, no GUI.
Every row below returned `RESULT: PASS`, with no kernel WARN/BUG matches.
Counts are printed assertions, not coverage percentages. These supersede the
previous revision's counts (that run used kernel 6.12.107).

| Harness | Extra environment | Pass / fail |
| --- | --- | --- |
| kmsdrm | default, virtio-gpu | 24 / 0 |
| kmsdrm | `SDLOP_QEMU_RAW=1 SDLOP_QEMU_CHORD=1` | 25 / 0 |
| kmsdrm | `TTYNO=3 SDLOP_KMSDRM_VT_SWITCH=1` | 24 / 0 |
| kmsdrm | `SDLOP_QEMU_GPU=bochs-display,id=gpu0` | 22 / 0 |
| kmsdrm | `SDLOP_QEMU_CTTY=1` | 26 / 0 |
| kmsdrm | `SDLOP_QEMU_INPUT_CHECK=1` | 25 / 0 |
| kmsdrm | `SDLOP_QEMU_VULKAN_CHECK=1` | 28 / 0 |
| kmsdrm | `SDLOP_QEMU_VULKAN_CHECK=1 SDLOP_KMSDRM_KEEP_MASTER=1` | 28 / 0 |
| fbdev | `SDLOP_FBDEV_OWNED_CHECK=1` | 92 / 0 (includes 30 owned-VT assertions) |
| fbdev | `SDLOP_FBDEV_RAW=1 SDLOP_QEMU_CHORD=1` | 65 / 0 |
| fbdev | `SDLOP_QEMU_CTTY=1` | 63 / 0 |
| fbdev | `SDLOP_QEMU_VGA=792` (24 bpp) | 62 / 0 |
| fbdev | `SDLOP_QEMU_VGA=836` (32 bpp) | 62 / 0 |

fbdev uses `-vga std`; the default VESA mode is 791, **16 bpp**. The serial log
reports the actual `/sys/class/graphics/fb0/bits_per_pixel`, including 24 and 32
for the two alternate-mode runs. No successful cirrus run is claimed.

Host strict Xvfb `ctest`: **33/33 passed** in this dependency configuration.
Some host tests return successfully when their required hardware/display is
absent; this is not 33 hardware integration tests. In particular, the host
uinput test self-skips; the QEMU uinput row explicitly requires a real PASS,
not SKIP. A separate ASan+UBSan build passed **3/3** targeted tests:
`test_vt_mask`, `test_fbdev_device`, and the fbdev RAM/PPM test.

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
  The cursor-trail regression now **asserts zero stale pixels** in a 48x48
  region around the previous cursor after moving it and presenting only a
  small dirty rectangle elsewhere. A full-frame repaint cannot mask the bug.
- `owned_vt.c` installs `KD_GRAPHICS` + `VT_PROCESS` on VT 2. The normal run
  compares a whole-framebuffer snapshot during SDL and after quit: show,
  present, cursor updates and quit must leave the owner's pixels untouched.
  A second `--allow` run explicitly sets `SDLOP_FBDEV_TAKEOVER=0` and verifies
  pixels do change. Each run also checks mode, keyboard mode, VT ownership and
  release/acquire signal numbers remain intact during and after SDL.
  This is **fbdev arbitration evidence**, not a kmsdrm arbitration test.
- Both console drills change unrelated signal-mask bits after init (unblock
  SIGUSR1, block SIGUSR2) and assert they survive quit while the driver-owned
  VT bits return to their pre-init states. `test_vt_mask` also checks that a
  VT signal already blocked before init stays blocked, while unrelated
  application block/unblock edits are preserved.
- The uinput row runs `test_evdev_uinput` with the dummy driver and a real
  synthetic kernel input device: global keys still reach callbacks and the
  raw polling ring, but not SDL key events/state without console ownership.
  Raw pointer-to-SDL delivery is also checked. This is not an X11/Wayland
  focus-loss integration test; the desktop keyboard policy is documented in
  the main README.
- The software-window master check uses a fresh DRM fd to prove master was
  retained. The Vulkan-window rows create an `SDL_WINDOW_VULKAN` window and
  verify master is available to a fresh fd by default but unavailable with
  `KEEP_MASTER=1`; a subsequent software window must reacquire it. The harness
  now explicitly forwards `KEEP_MASTER` into the guest. **No Vulkan instance,
  display surface, ICD acquisition or swapchain is created in these tests.**
- Bochs has no cursor plane: its cursor-plane phase is a passing explanatory
  note, not successful hardware-cursor coverage.

## Synthetic host framebuffer regression

`test_fbdev_device` wraps only `ioctl` in its statically linked test binary,
returns configurable FBIOGET screen info for a temporary file, and uses a real
shared mmap for scanout. It checks specific diagnostics for unsupported visuals,
10-bit channels, greyscale, empty/reversed/out-of-range/overlapping channels and
invalid panning. Accepted 16/24/32-bit modes are checked for correct packing and
writes at nonzero **initial** x/y offsets, leaving preceding memory untouched.
It never opens physical framebuffer or VT devices (`TAKEOVER=0`).

## Remaining limits

No physical-hardware run, Vulkan display-surface/swapchain run, live external
panning-change test, or injected allocation/ioctl/thread-start failure run.
Nonzero panning is honored at device open; live external changes are not tracked.
The dirty-rect cursor case and signal-mask regression are now asserted, but no
claim of exhaustive cursor, signal-race or leak-sanitizer coverage is made.
