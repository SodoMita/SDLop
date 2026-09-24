#!/bin/sh
#
# Display geometry, HiDPI scale and hotplug against a real compositor.
#
# Needs a compositor that can describe and change more than one output and is
# scriptable: sway (wlroots). weston cannot create a second output, so this
# script configures the outputs itself and skips when sway is not running.
#
#     XDG_RUNTIME_DIR=/tmp/xdg-run WLR_BACKENDS=headless WLR_RENDERER=pixman \
#     WLR_HEADLESS_OUTPUTS=2 sway -c /dev/null -d &
#     make wayland-check
#
# Environment:
#   WAYLAND_DISPLAY / XDG_RUNTIME_DIR / SWAYSOCK   the session to test
#   WAYLAND_DISPLAY_CLIENT                         default build/tests/wayland_display
#   DISPLAY_SECONDS                                default 7

set -u

# Session + window-geometry helpers (swaymsg, tools/wl_inject).
. "$(dirname "$0")/wayland_setup.sh"

CLIENT=${WAYLAND_DISPLAY_CLIENT:-build/tests/wayland_display}
RUN_SECONDS=${DISPLAY_SECONDS:-7}
FAILURES=0

if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "wayland-display: WAYLAND_DISPLAY is not set -- skipping" >&2
    exit 0
fi
if ! command -v swaymsg >/dev/null 2>&1; then
    echo "wayland-display: swaymsg not installed -- skipping" >&2
    exit 0
fi
if ! swaymsg -t get_outputs >/dev/null 2>&1; then
    echo "wayland-display: not a running sway (swaymsg cannot reach it) -- skipping"
    exit 0
fi
[ -x "$CLIENT" ] || { echo "wayland-display: $CLIENT is missing" >&2; exit 2; }

check() { # check <description> <condition-result> [detail]
    if [ "$2" = "1" ]; then
        echo "  ok   $1${3:+ ($3)}"
    else
        echo "  FAIL $1${3:+ ($3)}"
        FAILURES=$((FAILURES + 1))
    fi
}

cfgtag() { printf '%s' "$1" | sed 's/[^A-Za-z0-9]/-/g'; }

# Driving a headless output through the compositor needs its IPC; without it
# this script cannot set up the two-output session it is about.
if ! have_sway; then
    echo "wayland-display: SKIP (no running sway to configure outputs with)" >&2
    exit 0
fi

if ! reset_outputs 800x600 1 0 1024x768 2 800; then
    echo "wayland-display: no sway session with two outputs -- skipping" >&2
    exit 0
fi
# The client's window has to be on the *second* output (the scale-2 one): on a
# tiling compositor the focused output decides where a new toplevel lands, and
# the unplug below is what has to move it back to the surviving one.
focus_output "$SECOND_OUTPUT"
$CLIENT --seconds "$RUN_SECONDS" --watch > /tmp/sdlop-wayland-display.$$.log 2>&1 &
client_pid=$!
sleep 2.5

OUT_1=$(swaymsg -t get_outputs -r | tr -d '\n')
START_DISPLAYS=$(grep '^DISPLAY start' /tmp/sdlop-wayland-display.$$.log | grep -c 'id=')

check "two outputs are reported as two displays" \
      "$([ "$START_DISPLAYS" = "2" ] && echo 1 || echo 0)" 
check "display 1 is 800x600 at 0,0" \
      "$(grep -q 'DISPLAY start .* bounds=0,0,800,600' /tmp/sdlop-wayland-display.$$.log && echo 1 || echo 0)"
check "display 2 is the scale-2 output in logical coordinates (512x384 at 800,0)" \
      "$(grep -q 'DISPLAY start .* bounds=800,0,512,384' /tmp/sdlop-wayland-display.$$.log && echo 1 || echo 0)"
check "the scale-2 display keeps content scale 1.0 (SDL3 default)" \
      "$(grep -q 'DISPLAY start .*bounds=800,0,512,384 .* density=2.00 content=1.00' /tmp/sdlop-wayland-display.$$.log && echo 1 || echo 0)"

# ---------------------------------------------------------------- hotplug --
swaymsg output "$SECOND_OUTPUT" unplug >/dev/null 2>&1
sleep 2
check "unplugging an output sends DISPLAY_REMOVED" \
      "$(grep -q '^EVENT display removed=' /tmp/sdlop-wayland-display.$$.log && echo 1 || echo 0)"
REMOVED_ID=$(grep -oE '^EVENT display removed=[0-9]+' /tmp/sdlop-wayland-display.$$.log | head -1 | cut -d= -f2)
check "the removed display is gone from the display list" \
      "$([ -n "$REMOVED_ID" ] && ! grep -q "DISPLAY after-removed id=$REMOVED_ID " /tmp/sdlop-wayland-display.$$.log && echo 1 || echo 0)" 
check "the window is re-homed when its display disappears" \
      "$(grep -q '^EVENT window display=' /tmp/sdlop-wayland-display.$$.log && echo 1 || echo 0)"

swaymsg create_output >/dev/null 2>&1
sleep 1
NEW=$(output_names | grep -vx "$FIRST_OUTPUT" | tail -1)
swaymsg output "$NEW" mode 1024x768 scale 2 position 800 0 >/dev/null 2>&1
sleep 1.5
check "replugging sends DISPLAY_ADDED" \
      "$(grep -q '^EVENT display added=' /tmp/sdlop-wayland-display.$$.log && echo 1 || echo 0)"
check "the new display gets its own ID (never a reused one)" \
      "$(grep -q '^EVENT display added=' /tmp/sdlop-wayland-display.$$.log && \
         [ "$(grep -oE 'added=[0-9]+' /tmp/sdlop-wayland-display.$$.log | cut -d= -f2 | sort -u | wc -l)" = "1" ] && \
         ! grep -q "removed=$(grep -oE 'added=[0-9]+' /tmp/sdlop-wayland-display.$$.log | head -1 | cut -d= -f2)" /tmp/sdlop-wayland-display.$$.log && echo 1 || echo 0)"
NEW_ID=$(grep -oE 'added=[0-9]+' /tmp/sdlop-wayland-display.$$.log | head -1 | cut -d= -f2)

wait $client_pid 2>/dev/null

# A monitor that comes back at a different mode and scale has to end up in the
# display list with that geometry, under the ID it was announced with (the
# snapshot taken when the event arrived predates sway applying the config).
check "the replugged display ends up with the scale-2 geometry" \
      "$([ -n "$NEW_ID" ] && grep -q "DISPLAY end id=$NEW_ID .* bounds=800,0,512,384" /tmp/sdlop-wayland-display.$$.log && echo 1 || echo 0)"
check "that display kept the ID it was announced with" \
      "$([ -n "$NEW_ID" ] && grep -q "DISPLAY end id=$NEW_ID " /tmp/sdlop-wayland-display.$$.log && echo 1 || echo 0)"
check "the client exits cleanly after all of that" \
      "$(grep -q '^WINDOW end' /tmp/sdlop-wayland-display.$$.log && echo 1 || echo 0)"

if [ "$FAILURES" = "0" ]; then
    rm -f /tmp/sdlop-wayland-display.$$.log
    echo "wayland-display: PASS"
    exit 0
fi
echo "wayland-display: $FAILURES failure(s); log kept in /tmp/sdlop-wayland-display.$$.log"
[ -n "${VERBOSE:-}" ] && cat /tmp/sdlop-wayland-display.$$.log
exit 1
