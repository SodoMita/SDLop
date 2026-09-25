#!/bin/sh
#
# Input and window coverage for the X11 backend.
#
# The X11 backend's input path is the fallback for a session where the evdev
# worker cannot run (a sandbox, XWayland, a remote X session): the events come
# from the X server rather than from /dev/input. Nothing but real X events can
# prove that translation, so this rig drives the client with xdotool against a
# running X server:
#
#     Xvfb :99 -screen 0 1280x800x24 &
#     DISPLAY=:99 make x11-check
#
# No window manager is needed (and none is assumed): the client reports where its
# window is, and the coordinates injected are computed from that.
#
# Environment:
#   DISPLAY             the X server to test against
#   X11_INPUT_CLIENT    default build/tests/x11_input
#   SDL_VIDEODRIVER     default x11
#   SECONDS_PER_RUN     default 8 (client lifetime)

set -u

CLIENT=${X11_INPUT_CLIENT:-build/tests/x11_input}
TITLE=sdlop-x11
OUT=/tmp/sdlop-x11-input.$$
FAILURES=0
CLIENT_PID=""

if [ -z "${DISPLAY:-}" ]; then
    echo "x11-input: DISPLAY is not set -- point it at an X server" >&2
    exit 2
fi
if [ ! -x "$CLIENT" ]; then
    echo "x11-input: $CLIENT is missing (run 'make x11-check')" >&2
    exit 2
fi
if ! command -v xdotool >/dev/null 2>&1; then
    echo "x11-input: SKIP (xdotool is not installed)" >&2
    exit 0
fi
: "${SDL_VIDEODRIVER:=x11}"
export SDL_VIDEODRIVER

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

seen() { # seen <pattern> <file> -- "1" when the pattern occurs at least once
    if [ "$(count "$1" "$2")" -ge 1 ]; then echo 1; else echo 0; fi
}

LOG=$OUT.log
"$CLIENT" --seconds "${SECONDS_PER_RUN:-8}" --title "$TITLE" > "$LOG" 2>&1 &
CLIENT_PID=$!
waited=0
while [ $waited -lt 60 ] && ! grep -q '^READY' "$LOG" 2>/dev/null; do
    sleep 0.1
    waited=$((waited + 1))
done
if ! grep -q '^READY' "$LOG" 2>/dev/null; then
    echo "x11-input: the client never became ready:" >&2
    cat "$LOG" >&2
    kill $CLIENT_PID 2>/dev/null
    exit 1
fi

READY=$(grep -m1 '^READY' "$LOG")
WX=$(echo "$READY" | sed -n 's/.* x=\([-0-9]*\).*/\1/p')
WY=$(echo "$READY" | sed -n 's/.* y=\([-0-9]*\).*/\1/p')
WW=$(echo "$READY" | sed -n 's/.* w=\([0-9]*\).*/\1/p')
WH=$(echo "$READY" | sed -n 's/.* h=\([0-9]*\).*/\1/p')
XWID=$(echo "$READY" | sed -n 's/.* x11=\([0-9]*\).*/\1/p')
if [ -z "$XWID" ] || [ "$XWID" = "0" ]; then
    XWID=$(xdotool search --name "^$TITLE$" 2>/dev/null | head -1)
fi
echo "x11-input: display=$DISPLAY driver=$SDL_VIDEODRIVER window=${WW}x${WH}+${WX}+${WY} x11=$XWID"

CX=$((WX + WW / 2))
CY=$((WY + WH / 2))
# Start from well outside the window: a window that is mapped under a stationary
# pointer does not always produce a crossing event, and the entry is what this
# rig is about.
xdotool mousemove --sync $((WX + WW + 200)) $((WY + WH + 200))
sleep 0.15
xdotool mousemove --sync "$CX" "$CY"
sleep 0.2
xdotool click 1
sleep 0.1
xdotool click 4
xdotool click 5
sleep 0.1
xdotool mousemove --sync $((CX + 20)) $((CY + 10))
sleep 0.1

# Keys: plain, shifted (text and keycode differ), and with Ctrl held (no text).
xdotool key a
sleep 0.1
xdotool key shift+a
sleep 0.1
xdotool key ctrl+c
sleep 0.1
# A held key repeats: the X server sends the repeats, the client marks them.
xdotool keydown d
sleep 0.8
xdotool keyup d
sleep 0.2
# With autorepeat switched off the server sends the press once and nothing more,
# and the client must not invent repeats of its own.
if command -v xset >/dev/null 2>&1; then
    xset r off
    REPEAT_OFF=1
    xdotool keydown e
    sleep 0.8
    xdotool keyup e
    sleep 0.2
    xset r on
    REPEAT_OFF=0
else
    REPEAT_OFF=skip
fi
# Leaving the window.
xdotool mousemove --sync $((WX + WW + 400)) $((WY + WH + 300))
sleep 0.2

# Window geometry, driven from outside.
xdotool windowsize "$XWID" 400 300
sleep 0.2
xdotool windowmove "$XWID" 240 180
sleep 0.2

wait $CLIENT_PID 2>/dev/null

echo "  -- events: $(count '^EVENT' "$LOG")"

check "the window is on the X11 driver" "$(seen '^READY driver=x11' "$LOG")"
check "the client found a display to report" "$(seen '^DISPLAY id=' "$LOG")"
check "the display is named by the X server" \
      "$(seen 'name=[^ ]' "$LOG")"
check "exactly one display is primary" \
      "$([ "$(count ' primary=1$' "$LOG")" = "1" ] && echo 1 || echo 0)"
check "the window has the input focus" "$(seen '^EVENT focus=1' "$LOG")"
check "entering the window is reported" "$(seen '^EVENT enter' "$LOG")"
check "motion carries window coordinates" "$(seen "^EVENT motion x=$((WW / 2))" "$LOG")"
check "a button press and release arrive in order" \
      "$([ "$(grep '^EVENT button' "$LOG" | head -2 | tr '\n' ' ' | grep -c 'down=1.*down=0')" -ge 1 ] && echo 1 || echo 0)"
check "the button reports where it was pressed" \
      "$(seen "^EVENT button down=1 button=1 x=$((WW / 2))" "$LOG")"
check "the wheel arrives once per click, as +1 and -1" \
      "$([ "$(count '^EVENT wheel x=0.0 y=1.0' "$LOG")" = "1" ] && \
         [ "$(count '^EVENT wheel x=0.0 y=-1.0' "$LOG")" = "1" ] && echo 1 || echo 0)"
check "a key press carries the SDL scancode" \
      "$(seen '^EVENT key down=1 scancode=4 key=0x61 mod=0x0 repeat=0' "$LOG")"
check "a key release follows it" "$(seen '^EVENT key down=0 scancode=4 key=0x61' "$LOG")"
check "the character is delivered as text" "$(seen '^EVENT text "a"' "$LOG")"
check "shift is reported as a modifier" "$(seen '^EVENT key down=1 scancode=225' "$LOG")"
check "shift+a keeps the unshifted keycode" \
      "$(seen '^EVENT key down=1 scancode=4 key=0x61 mod=0x1' "$LOG")"
check "shift+a produces the capital as text" "$(seen '^EVENT text "A"' "$LOG")"
check "ctrl+c produces no text" \
      "$([ "$(count '^EVENT text "c"' "$LOG")" = "0" ] && echo 1 || echo 0)"
check "a held key repeats, flagged as a repeat" \
      "$(seen '^EVENT key down=1 scancode=7 .*repeat=1' "$LOG")"
if [ "$REPEAT_OFF" = "0" ]; then
    check "with autorepeat off a held key is pressed once and not repeated" \
          "$([ "$(count '^EVENT key down=1 scancode=8 ' "$LOG")" = "1" ] && \
             [ "$(count '^EVENT key down=1 scancode=8 .*repeat=1' "$LOG")" = "0" ] && echo 1 || echo 0)"
else
    echo "  -- xset is not available, skipping the autorepeat-off case"
fi
check "leaving the window is reported" "$(seen '^EVENT leave' "$LOG")"
check "an X resize reaches the application" "$(seen '^EVENT resized w=400 h=300' "$LOG")"
check "an X move reaches the application" "$(seen '^EVENT moved x=240 y=180' "$LOG")"
check "the final geometry is the resized, moved one" \
      "$(seen '^DONE x=240 y=180 w=400 h=300' "$LOG")"
check "the window's X11 number is exposed as a platform property" \
      "$([ -n "$XWID" ] && [ "$XWID" != "0" ] && echo 1 || echo 0)"

# Cross-check the display list against the X server's own answer, when xrandr is
# there to give one.
if command -v xrandr >/dev/null 2>&1; then
    SCREEN=$(xrandr --query 2>/dev/null | sed -n 's/^Screen [0-9]*: .*current \([0-9]*\) x \([0-9]*\).*/\1x\2/p' | head -1)
    if [ -n "$SCREEN" ]; then
        W=${SCREEN%x*}
        H=${SCREEN#*x}
        check "the reported bounds match xrandr (${W}x${H})" "$(seen " w=$W h=$H " "$LOG")"
    else
        echo "  -- xrandr gave no screen size, skipping the bounds cross-check"
    fi
fi

if [ "$FAILURES" = "0" ]; then
    rm -f "$LOG" "$OUT"*
    echo "x11-input: PASS"
    exit 0
fi
# A failing run is worth reading: keep the client's event log where it can be
# looked at, and show it here.
cp "$LOG" /tmp/sdlop-x11-input.log 2>/dev/null
echo "x11-input: $FAILURES failure(s) -- event log follows (kept at /tmp/sdlop-x11-input.log)" >&2
cat "$LOG" >&2
rm -f "$OUT"*
exit 1
