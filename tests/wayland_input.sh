#!/bin/sh
#
# Pointer coverage for a compositor that has no input devices.
#
# A headless compositor (sway with WLR_BACKENDS=headless, weston with the
# headless backend) creates outputs but no seat capabilities, so the pointer path
# cannot be exercised against it at all. tools/wl_inject supplies the events
# through wlroots' virtual pointer protocol, which turns such a compositor into a
# usable test rig:
#
#     XDG_RUNTIME_DIR=/tmp/xdg-run WLR_BACKENDS=headless WLR_RENDERER=pixman \
#     WLR_HEADLESS_OUTPUTS=2 sway -c /dev/null -d &
#     make wayland-check
#
# The injected coordinates are compositor-global, and a tiling compositor picks
# both the output and the window geometry, so the window is located through the
# compositor's own tree (swaymsg) instead of being assumed.
#
# Environment:
#   WAYLAND_DISPLAY / XDG_RUNTIME_DIR   the compositor to test against
#   WAYLAND_INPUT_CLIENT                default build/tests/wayland_input
#   WAYLAND_INJECT                      default build/tools/wl_inject
#   SDL_VIDEODRIVER                     default wayland
#   SECONDS_PER_RUN                     default 5 (client lifetime)

set -u

# Session + window-geometry helpers (swaymsg, tools/wl_inject).
. "$(dirname "$0")/wayland_setup.sh"

CLIENT=${WAYLAND_INPUT_CLIENT:-build/tests/wayland_input}
INJECT=${WAYLAND_INJECT:-build/tools/wl_inject}
RUN_SECONDS=${SECONDS_PER_RUN:-5}
TITLE=sdlop-input
OUT=/tmp/sdlop-wayland-input.$$
FAILURES=0
CLIENT_PID=""

if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "wayland-input: WAYLAND_DISPLAY is not set -- point it at a compositor" >&2
    exit 2
fi
for f in "$CLIENT" "$INJECT"; do
    if [ ! -x "$f" ]; then
        echo "wayland-input: $f is missing (run 'make wayland-check')" >&2
        exit 2
    fi
done

# No virtual pointer means no way to inject anything: weston and other
# non-wlroots compositors can host the client but cannot be driven.
if ! "$INJECT" --check >/dev/null 2>&1; then
    echo "wayland-input: SKIP (the compositor has no zwlr_virtual_pointer_manager_v1)" >&2
    exit 0
fi

check() { # check <description> <condition-result>
    if [ "$2" = "1" ]; then
        echo "  ok   $1"
    else
        echo "  FAIL $1"
        FAILURES=$((FAILURES + 1))
    fi
}

count() { # count <pattern> <file>
    grep -c "$1" "$2" 2>/dev/null || true
}

start_client() { # start_client <args> <log>
    $CLIENT $1 --seconds "$RUN_SECONDS" --title "$TITLE" > "$2" 2>&1 &
    CLIENT_PID=$!
    waited=0
    while [ $waited -lt 60 ] && ! grep -q '^READY' "$2" 2>/dev/null; do
        sleep 0.1
        waited=$((waited + 1))
    done
    if ! grep -q '^READY' "$2" 2>/dev/null; then
        echo "  FAIL client never became ready"
        cat "$2"
        kill $CLIENT_PID 2>/dev/null
        FAILURES=$((FAILURES + 1))
        return 1
    fi
    sleep 1.5   # let the compositor map and focus the toplevel
    return 0
}

finish_client() {
    wait $CLIENT_PID 2>/dev/null
    CLIENT_PID=""
}

echo "wayland-input: display=$WAYLAND_DISPLAY driver=${SDL_VIDEODRIVER:-auto}"

# A deterministic session: two outputs at 0,0 and 800,0, the *first* one focused
# so that a new toplevel reliably lands where the injected coordinates expect it.
if reset_outputs 800x600 1 0 800x600 1 800; then
    focus_output "$FIRST_OUTPUT"
else
    echo "  -- no sway session to shape, using the outputs as they are"
fi

# ---------------------------------------------------------------- absolute --
LOG=$OUT.absolute.log
if start_client "" "$LOG"; then
    CENTRE=$(window_centre "$TITLE")
    if [ -z "$CENTRE" ]; then
        CX=${WINDOW_CX:-300}
        CY=${WINDOW_CY:-260}
        CX=${WINDOW_CX:-300}
        CY=${WINDOW_CY:-260}
        echo "  -- cannot read the compositor tree, injecting at $CX,$CY"
    else
        CX=${CENTRE% *}
        CY=${CENTRE#* }
        echo "  -- window centre at $CX,$CY"
    fi
    cat > "$OUT.absolute.script" <<EOF
sleep 200
cursor $CX $CY
sleep 80
move 12 7
sleep 80
move 12 7
sleep 120
button left down
sleep 60
button left up
sleep 120
wheel 3
sleep 60
cursor $((CX + 900)) $((CY + 900))
sleep 150
EOF
    $INJECT "$OUT.absolute.script" > "$OUT.absolute.inject" 2>&1
    finish_client
    EVENT_LOG=$LOG
    echo "  -- absolute: $(count '^EVENT' "$EVENT_LOG") events"
    check "window was entered (mouse enter)" \
          "$([ "$(count '^EVENT enter' "$EVENT_LOG")" -ge 1 ] && echo 1 || echo 0)"
    check "motion events carry window-local coordinates" \
          "$(grep -qE '^EVENT motion x=[0-9]{1,3}\.[0-9] y=[0-9]{1,3}\.[0-9]' "$EVENT_LOG" && echo 1 || echo 0)"
    check "an absolute move still reports xrel/yrel" \
          "$([ "$(count 'rel=1' "$EVENT_LOG")" -ge 1 ] && echo 1 || echo 0)"
    check "left button down and up" \
          "$([ "$(count '^EVENT button down=1 button=1' "$EVENT_LOG")" -ge 1 ] && \
             [ "$(count '^EVENT button down=0 button=1' "$EVENT_LOG")" -ge 1 ] && echo 1 || echo 0)"
    check "wheel events arrive" \
          "$([ "$(count '^EVENT wheel' "$EVENT_LOG")" -ge 1 ] && echo 1 || echo 0)"
    check "leaving the window sends mouse leave" \
          "$([ "$(count '^EVENT leave' "$EVENT_LOG")" -ge 1 ] && echo 1 || echo 0)"
fi

# ---------------------------------------------------------------- relative --
LOG=$OUT.relative.log
if start_client "--relative" "$LOG"; then
    CENTRE=$(window_centre "$TITLE")
    if [ -n "$CENTRE" ]; then
        CX=${CENTRE% *}
        CY=${CENTRE#* }
    else
        CX=300
        CY=260
    fi
    cat > "$OUT.relative.script" <<EOF
sleep 200
cursor $CX $CY
sleep 100
move 25 10
sleep 100
move -15 5
sleep 100
move 40 -20
sleep 200
EOF
    $INJECT "$OUT.relative.script" > "$OUT.relative.inject" 2>&1
    finish_client
    EVENT_LOG=$LOG
    echo "  -- relative: $(count '^EVENT' "$EVENT_LOG") events"
    check "relative mode reports the injected deltas" \
          "$(grep -qE '^EVENT motion .*xrel=25\.0 yrel=10\.0 rel=1' "$EVENT_LOG" && echo 1 || echo 0)"
    check "relative mode reports a negative delta" \
          "$(grep -qE '^EVENT motion .*xrel=-15\.0 yrel=5\.0 rel=1' "$EVENT_LOG" && echo 1 || echo 0)"
    check "relative mode still exits cleanly" \
          "$(grep -q '^DONE relative=1' "$EVENT_LOG" && echo 1 || echo 0)"
fi

if [ "$FAILURES" = "0" ]; then
    rm -f "$OUT".*
    echo "wayland-input: PASS"
    exit 0
fi
echo "wayland-input: $FAILURES failure(s); logs kept in $OUT.*"
if [ -n "${VERBOSE:-}" ]; then
    for f in "$OUT"*; do
        echo "== $f"
        cat "$f"
    done
fi
exit 1
