# Further input correctness audit

Audited revision: **004373885024e54e8320a5704bd8f9acf7b2b1a8**.
Research only: no library, driver or normal CTest changes in this follow-up.
The two diagnostic tools deliberately return **1 when a gap is reproduced**;
they are not passing regression gates or proposed fixes.

**Status update:** the eight findings below are fixed by bccc4bd. See
[implementation, positive regressions and before/after evidence](input-lifecycle-fixes.md).
The original findings and diagnostic conventions remain here as historical
research; new positive CTest gates live in `test_input_lifecycle`,
`test_evdev_recovery` and `test_wayland_keyboard`.

## Prioritized findings

### 1. High: terminal raw-device errors leave held state and a busy worker

**Location:** `src/internal/sdlop_evdev.c:drain_device_events` (270),
`worker_main` (319), `SDLOP_RawInputIsSource`.

A read returning EOF/ENODEV simply ends that service attempt. The worker does
not retire the fd from epoll, clear that device's held keys/buttons, update
source availability, or trigger removal recovery. Persistent HUP/error readiness
therefore brings it back immediately. Device discovery is also startup-only.

`repro_input_audit unplug` presses A and left mouse, closes the pipe writer to
make HUP persist, and maps reads of that transport to ENODEV. Observed **62,679
failed reads in approximately 50 ms**, A/left still held, and raw still selected.
This count includes test-wrapper work; it is evidence of a retry loop, **not a
physical-device performance measurement**. Six raw probes also reproduce with
ASan+UBSan/leak checking; this probe then observed 43,310 failed reads.

The user's earlier physical test reported no stuck key on unplug. That does
not establish a retirement path in this revision: focus loss, explicit reset,
or a final native release may mask it. This controlled failure supplies none
of those. No physical unplug was performed here.

**Fix direction:** worker-owned terminal-fd retirement and a main-thread
removal/state-diff handshake, preserving other devices' held state and callback
lifetime rules. Do not emit synthetic native raw events or bypass focus. Treat
hot-add/reopen/source failover as an explicit lifecycle design, not a read retry.

### 2. High: software repeats ignore another client's keyboard grab

**Location:** `src/video/x11/sdlop_x11.c` FocusIn/Out handler (3287),
`x11_PumpEvents` (3473); `src/SDL_keyboard.c:SDLOP_KeyboardProcessRepeats` (839).

X11 FocusOut(Grab) sets `keyboard_grabbed_elsewhere` while keeping our keyboard
focus. Fresh raw presses are gated, but an existing software repeat is not.
The repeat generator directly re-enters `SDLOP_SendKeyboardKey`, so it bypasses
the raw keyboard-focus/grab gate.

`repro_input_audit repeat-grab` receives A through the actual raw worker, sets
the exact state produced by that X11 handler, makes one repeat due, and pumps:
**one repeated key and one text event** are delivered while the grab flag is
true. This reproduces adapter behavior; it is **not a live X server grab test**.
It repeats only a previously known key; this is not evidence of capturing new
keystrokes belonging to another client.

**Fix direction:** reconcile/cancel repeat on grab and ownership transitions,
with shared ownership checks for every repeat producer. Test focus, VT release,
grab/ungrab, key release and recovery together; do not re-enable filtered input.

### 3. Medium: dead-key composition leaks between text-input sessions

**Location:** `src/SDL_keyboard.c:SDLOP_KeyboardTranslateKey` (682),
`compose_key` (651); `src/SDL_keynames.c:SDL_StartTextInputWithProperties` (28),
`SDL_StopTextInput` (55), `SDL_ClearComposition` (96).

Text translation feeds compose on every key-down, even with text input off.
Start/stop do not reset it. `SDL_ClearComposition` returns success without doing
anything; its comment that no composition persists is contradicted by the
keyboard's persistent `compose_st`.

With a real German keymap/Compose table, all three sequences produce **é**
instead of a fresh **e** (`repro_input_audit compose`):

* text off -> dead acute -> start text -> E;
* text on -> dead acute -> stop/start text -> E;
* text on -> dead acute -> ClearComposition -> E.

The first two are lifecycle defects. The explicit clear probe also establishes
that the current successful no-op cannot clear SDLop's own compose sequence;
this is not a claim that every upstream backend exposes identical IME controls.

**Fix direction:** define composition ownership per text session, reset on
session transitions, and stop feeding composition for inactive text input.
Keep plain keycode/modifier tracking independent. SDL3's Wayland path checks
`SDL_TextInputActive` before press-text translation and resets compose during
text-input state changes (reference below).

### 4. Medium: Wayland repeat-disable and zero-delay settings are ignored

**Location:** `src/video/wayland/sdlop_wayland.c:keyboard_repeat_info` (1002),
`src/internal/sdlop_evdev.c:SDLOP_RawInputSetRepeatInfo` (1172).

The listener accepts only `rate > 0 && delay > 0`, and the common setter also
cannot represent zero delay. Wayland explicitly defines **rate=0 as disabled**
and permits delay=0. Very large positive rates also integer-divide to interval
zero and leave the previous setting instead of a defined clamped rate.

`repro_wayland_keyboard_audit repeat-info` calls the **actual static listener**:
rate=0 still allows one due repeat; a subsequent rate=20/delay=0 leaves the old
500 ms/33 ms settings instead of 0 ms/50 ms. No compositor or timing sleep is
needed for this probe.

**Fix direction:** an explicit repeat-enabled state, legal zero delay, and
bounded rate conversion; update or cancel already-running repeats consistently.
This affects both seat keys and raw keys using compositor repeat settings.

### 5. Medium: raw high-resolution wheels still double-count detents

**Location:** `src/internal/sdlop_evdev.c:pump_event`, legacy wheel cases (851),
high-resolution cases (857), SYN_REPORT flush (874).

`repro_input_audit wheel` sends high-res +120 and legacy +1 in one report:
**two SDL wheel events, total y=2.0**, for one detent. Kernel documentation says
both representations may be emitted; one is not additional physical movement.

This is a **reconfirmed, previously documented raw-path gap**, not a regression
in the last Wayland fix. Wayland value120/discrete arbitration is a different
path and its fix does not select evdev wheel capabilities.

**Fix direction:** per-device/per-axis high-res capability selection, keeping
native callbacks unmodified; test legacy-only, paired, fractional and damaged
reports. SDL3's evdev implementation already makes that selection.

### 6. Medium: partial raw wheel frames bypass focus checks at flush

**Location:** `src/internal/sdlop_evdev.c:pump_event`, EV_REL focus gate and
EV_SYN high-resolution wheel flush (874).

`repro_input_audit wheel-focus` receives high-res +60 while focused, pumps that
partial frame, then removes both focus pointers and delivers SYN_REPORT:
**one wheel event with y=0.5 is still queued without focus**. The frame flush
does not check current focus or discard the accumulator on loss. Queue pressure
can split an otherwise contiguous report across pumps; backends also update
focus between pumps.

**Fix direction:** explicitly handle partial-frame ownership on focus/grab
transitions and revalidate at delivery. Do not broaden ordinary input ownership.

### 7. Medium/low: key union tracking uses native code, not SDL scancode

**Location:** `src/internal/sdlop_evdev.c:pump_key` (888),
`src/internal/scancode_evdev.h`.

The union correctly handles the **same native code** on multiple devices, but
not different native codes mapped to the same SDL scancode. `KEY_SYSRQ` (99)
and `KEY_PRINT` (210) both map to PRINTSCREEN. After both are down, releasing
only SYSRQ produces a key-up and clears SDL PRINTSCREEN even though PRINT is
still physically down (`repro_input_audit key-alias`). Same result with the
actual worker in optimized and sanitized builds.

This is a rare alias case, not a failure of the existing same-code dual-keyboard
regression. The button adapter already has special alias handling for
BTN_TOUCH/BTN_LEFT; keyboard union needs similarly well-defined SDL state.

**Fix direction:** union by delivered logical scancode while retaining each
native device/code state for snapshots; avoid an extra worker hot-path cost.

### 8. Medium: Wayland keyboard-enter discards already-held modifiers

**Location:** `src/video/wayland/sdlop_wayland.c:keyboard_enter` (931).

The callback explicitly ignores its `keys` array. The isolated actual-listener
probe `repro_wayland_keyboard_audit enter-held` advertises LEFTSHIFT held on
enter: focus is gained, but `SDL_GetKeyboardState()[LSHIFT]` stays false.
A later modifiers event can fix effective text modifiers, but cannot repair the
physical keyboard-state array by itself. SDL3's Wayland enter handler restores
modifier keys from this list, deliberately not arbitrary held action keys.
Any fix should preserve that distinction rather than replay every key as a
fresh action.

This **revalidates the earlier focus/lifecycle snapshot gap** on the seat path;
it does not justify synthesizing focus. Raw startup/focus restoration is a
separate native snapshot problem. Focus must first be legitimately granted by
the compositor, and synthetic restoration must not generate text/repeat presses.

## Evidence and limits

* Optimized results: [diagnostic output](benchmarks/2026-11-21-input-audit-repros.txt).
* Six worker/core modes with ASan+UBSan, leaks enabled:
  [sanitized output](benchmarks/2026-11-21-input-audit-repros-asan.txt).
* The two Wayland listener probes ran optimized only. Including the whole
  driver with ASan retains otherwise discarded globals, requiring additional
  backend symbols; that isolated sanitized link attempt failed. No sanitized
  or live-compositor claim is made for those two modes.
* GCC 12.2, Linux x86-64. Rootless pipes, real native worker/library, mocked
  terminal read error; backend grab state is injected on the main thread.
  Real xkbcommon 1.5.0, US/German layout and Compose data. The Compose probe
  asserts a known-good acute+E sequence first, so absent data is a setup failure,
  not a falsely reproduced lifecycle bug.
* No production code changed, so no new latency/throughput claim or timing gate
  is made. The earlier PR-wide performance gate remains uncleared.

Not counted as new findings: ordinary raw repeat text already passes on this
revision; Ctrl/Alt text suppression is already implemented by `filter_text`;
ABS mouse/pen discovery is already implemented by 0043738; same-code multi-device
union and SYN_DROPPED repair have existing passing tests. Missing IME and
multitouch translation are declared feature limitations, not silently fixed
or relabeled here. Stale compositor modifier masks racing raw state, multiwindow
touch targeting and live VT/grab transitions warrant separate integration work;
this audit does not elevate them to experimentally confirmed failures.

## Reference sources inspected

* [Linux v6.12 event-codes documentation](https://github.com/torvalds/linux/blob/v6.12/Documentation/input/event-codes.rst#L194-L216): legacy/high-resolution wheel equivalence.
* [Wayland 1.23.1 keyboard protocol](https://github.com/wayland-mirror/wayland/blob/1.23.1/protocol/wayland.xml): keyboard enter held-key array, repeat_info legal values and disabling.
* SDL3 **release-3.4.20**, commit `f00dc919ca35b9756316e86c4777392de442f9b2`
  (repository's API reference, not a claim about latest release):
  [Wayland key/repeat handling](https://github.com/libsdl-org/SDL/blob/f00dc919ca35b9756316e86c4777392de442f9b2/src/video/wayland/SDL_waylandevents.c#L2239-L2344),
  [compose lifetime](https://github.com/libsdl-org/SDL/blob/f00dc919ca35b9756316e86c4777392de442f9b2/src/video/wayland/SDL_waylandkeyboard.c#L88-L109),
  [evdev wheel selection](https://github.com/libsdl-org/SDL/blob/f00dc919ca35b9756316e86c4777392de442f9b2/src/core/linux/SDL_evdev.c#L461-L482).

## Running the diagnostics

The raw diagnostic links against the normal **static** SDLop build. Existing
project development headers/libraries suffice; no new runtime dependency:

```sh
cc -D_GNU_SOURCE -std=c11 -O2 -g -Iinclude -Isrc -Itests \
  tools/repro_input_audit.c build/libsdlop.a \
  -Wl,--wrap=opendir -Wl,--wrap=open -Wl,--wrap=open64 \
  -Wl,--wrap=ioctl -Wl,--wrap=read -ldl -lm -pthread -o build/repro_input_audit
for mode in unplug repeat-grab wheel wheel-focus key-alias compose; do
  build/repro_input_audit "$mode"
  echo "exit=$?" # 1 means reproduced, not a successful regression gate
done
```

Use real system US/German xkb and locale Compose data, or set `LD_LIBRARY_PATH`
and `XCOMPOSEFILE` to local equivalents. An absent layout/table aborts setup.
For ASan+UBSan, link the sanitized static library and compile the probe with
the same sanitizer flags; keep read/discovery wrappers.

The listener diagnostic intentionally includes the private Wayland translation
unit and drops unused code via linker GC. It needs a **separate headless archive**
(compile the CMake `SDLop_SOURCES` list excluding `/wayland/` and `/kmsdrm/`, with
video backend defines off except optional fbdev), plus generated Wayland headers.
Do not link this diagnostic to the normal all-backend library: its duplicate
Wayland symbols defeat isolation. Local command after preparing that archive:

```sh
cc -D_GNU_SOURCE -std=c11 -O2 -g -ffunction-sections -fdata-sections \
  -Iinclude -Isrc -Ibuild/wayland-gen $(pkg-config --cflags wayland-client egl xkbcommon) \
  tools/repro_wayland_keyboard_audit.c build-audit/libsdlop.a \
  -Wl,--gc-sections -ldl -lm -pthread -o build-audit/repro_wayland_keyboard_audit
build-audit/repro_wayland_keyboard_audit repeat-info # expected exit 1
build-audit/repro_wayland_keyboard_audit enter-held  # expected exit 1
```

A future fix should convert the relevant diagnostic into an ordinary positive
regression, one focused change at a time. Recommended order: terminal-device
retirement, grab-aware repeat ownership, text-session composition, repeat-info
semantics, wheel frame ownership/selection, then alias/focus restoration.
