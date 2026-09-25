#!/bin/sh
# behaviour-check - the same scripted input, two libraries, one normalized trace.
#
#   make behaviour-check                        # SDLop vs the system SDL3
#   X11_DISPLAY=:99 make behaviour-check
#
# Both probes are built from tools/behaviour_probe.c - one against SDLop's
# headers and static library, one against the system SDL3. They run one after the
# other on the same X server, this script drives each of them with the same
# xdotool sequence (plus a second pass on a layout with dead keys), and the two
# traces are diffed after normalization.
#
# `diff` on the raw traces is not the verdict: SDLop and stock SDL3 are known to
# differ in a few places that are documented in docs/ROADMAP.md (window flags a
# driver sets for itself, the order of the first lifecycle events, an extra
# motion event stock synthesizes on a button press). Those lines are filtered out
# by the DOCUMENTED list below and reported as such; anything else fails.
set -u

CC=${CC:-cc}
BUILD=${BUILD:-build}
DISPLAY_NUM=${X11_DISPLAY:-:99}
export DISPLAY="$DISPLAY_NUM"
# This rig drives an X11 window with xdotool, so both probes have to open an X11
# window: a WAYLAND_DISPLAY left in the environment would make the probe (and
# possibly only one of the two, depending on each library's driver order) pick
# the Wayland backend, and then nothing lines up - xdotool cannot see a Wayland
# window and the two traces are not comparable at all.
export SDL_VIDEODRIVER=x11
unset WAYLAND_DISPLAY
TITLE=sdlop-behaviour
SECONDS_RUN=${SECONDS_RUN:-14}
DEADKEY_LAYOUT=${DEADKEY_LAYOUT:-de}
TMP=${TMPDIR:-/tmp}/behaviour-check

fail=0
ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$1"; }

# Ask the X server which layout it actually has. setxkbmap can exit 0 without the
# server changing anything (seen on an X server that refuses XkbSetMap: the
# command reports "Applied rules", the keymap stays as it was), and the layout
# this rig compares is the whole point of the second leg - so the layout is
# verified rather than assumed.
current_layout()
{
    setxkbmap -query 2>/dev/null | sed -n 's/^layout:[[:space:]]*//p' | head -1
}

layout_took()   # layout_took <layout>
{
    setxkbmap "$1" 2>/dev/null || return 1
    sleep 0.2
    [ "$(current_layout)" = "$1" ]
}
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; fail=1; }
skip() { printf '  SKIP  %s\n' "$1"; }
note() { printf '  note  %s\n' "$1"; }

command -v xdotool >/dev/null 2>&1 || { echo "behaviour-check: xdotool is required"; exit 77; }
if ! xdpyinfo >/dev/null 2>&1; then
    echo "behaviour-check: no X server on DISPLAY=$DISPLAY (start Xvfb $DISPLAY_NUM)"
    exit 77
fi

mkdir -p "$TMP" "$BUILD/tools"

LIBS="-lpthread -ldl -lm -lwayland-egl -lwayland-client -lX11 -lXext -lXi -lXrandr -lxkbcommon -lxkbcommon-x11 -lX11-xcb -lxcb"
$CC -std=gnu11 -O1 -g -Iinclude -o "$BUILD/tools/behaviour_sdlop" tools/behaviour_probe.c \
    "$BUILD/libSDLop.a" $LIBS || exit 1
if pkg-config --exists sdl3 2>/dev/null; then
    $CC -std=gnu11 -O1 -g $(pkg-config --cflags sdl3) -o "$BUILD/tools/behaviour_stock" \
        tools/behaviour_probe.c $(pkg-config --libs sdl3) || exit 1
    HAVE_STOCK=1
else
    HAVE_STOCK=0
fi

# ---------------------------------------------------------------------------
# driving: the sequence is written once and used for both binaries
# ---------------------------------------------------------------------------
drive()
{
    probe=$1
    switch_to=$2
    wid=$(xdotool search --name "$TITLE" | head -1)
    [ -n "$wid" ] || { echo "no window found for $TITLE" >&2; return 1; }

    # park the pointer outside the window first: entering the window is an event,
    # and "already inside" would silently drop it
    xdotool mousemove 1000 700
    xdotool windowfocus --sync "$wid" 2>/dev/null
    sleep 0.2

    eval "$(xdotool getwindowgeometry --shell "$wid")"    # X Y WIDTH HEIGHT
    cx=$((X + 30)); cy=$((Y + 30))

    sleep 0.15
    xdotool mousemove --sync $cx $cy            # enter + motion
    sleep 0.15
    xdotool key a
    sleep 0.15
    xdotool key shift+a
    sleep 0.15
    xdotool key ctrl+c
    sleep 0.15
    xdotool key alt+a
    sleep 0.15
    xdotool key Caps_Lock
    sleep 0.15
    xdotool key a
    sleep 0.15
    xdotool key Caps_Lock
    sleep 0.15
    # A live layout switch, with the window already open and focused: this is
    # what an application sees when the user picks another layout while it runs.
    # Both probes see the same switch at the same point of the sequence, and
    # everything that follows it (Return, Escape, the mouse block) uses keys and
    # buttons whose keycodes do not depend on the layout.
    setxkbmap "$switch_to"
    sleep 0.4
    # (If the server ignores the switch, the "both probes announced the live
    # layout switch" assertion in compare() is what notices: setxkbmap still
    # rewrites the root property, so an implementation that watches the keymap
    # change *and* one that watches the property both have to answer.)
    xdotool click 1
    sleep 0.15
    xdotool mousemove --sync $((cx + 25)) $((cy + 15))
    sleep 0.15
    xdotool click 3
    sleep 0.15
    xdotool click 4                            # wheel up
    sleep 0.15
    xdotool click 5                            # wheel down
    sleep 0.15
    xdotool key Return
    sleep 0.15
    xdotool key Escape
}

# run_one <binary> <label> <layout> <outfile>
run_one()
{
    bin=$1; label=$2; layout=$3; out=$4
    # Reload the layout before *every* run, also for the default one: both runs
    # then see the same layout. Caps Lock lives in the X server rather than in the
    # probe, so it is cleared explicitly - otherwise one run's final lock state
    # becomes the next run's starting state and the two traces are not comparable.
    setxkbmap "${layout:-us}" 2>/dev/null || return 2
    if xset q 2>/dev/null | grep -q 'Caps Lock: *on'; then
        xdotool key Caps_Lock
        sleep 0.2
    fi
    # Park the pointer outside any window this run will create, before the run
    # starts. Where the pointer is left over from the previous run decides which
    # MOUSE_ENTER/MOTION events a window sees while it is being created, so
    # without this the two traces are not comparable at all (measured, not
    # guessed: the same binary produced different traces run to run).
    xdotool mousemove 5 5
    sleep 0.2
    "$bin" --width 320 --height 240 --title "$TITLE" --seconds "$SECONDS_RUN" > "$out" 2>"$out.err" &
    pid=$!
    for i in $(seq 1 100); do
        grep -q READY-INPUT "$out" 2>/dev/null && break
        sleep 0.1
    done
    if ! grep -q READY-INPUT "$out" 2>/dev/null; then
        printf '  \033[31mFAIL\033[0m  %s never reached READY-INPUT (see %s)\n' "$label" "$out.err"
        kill $pid 2>/dev/null
        fail=1
        return 1
    fi
    # the live switch inside the run goes to the *other* layout, so both runs
    # start from the layout they were given and then really change it
    if [ "$layout" = "de" ]; then drive "$bin" us; else drive "$bin" de; fi
    for i in $(seq 1 200); do
        grep -q '^DONE' "$out" 2>/dev/null && break
        sleep 0.1
    done
    kill $pid 2>/dev/null
    wait $pid 2>/dev/null
    return 0
}

# ---------------------------------------------------------------------------
# normalization: what a difference in the trace is allowed to be
# ---------------------------------------------------------------------------
# Documented, deliberate differences; everything else fails. Each rule is a
# comment with its reason, because a rule here is a difference hidden from the
# diff.
#
#   STATE flags=...            SDLop reports the flags it has when the window is
#                              shown; the two drivers focus the window at
#                              slightly different moments, so whether INPUT_FOCUS
#                              is already in the word differs.
#   FROMNAMES ... [<=0x...]    stock SDL3 itself answers SDL_GetKeyFromName() for
#                              a shifted symbol differently per driver (0x3c on
#                              X11, 0x2c offscreen); SDLop answers with the US
#                              layout's base key on every driver.
#   STATE flags=...
#                              SDLop reports the flags the window has when it is
#                              shown; stock reports its own at the same moment, and
#                              the two drivers focus the window at slightly
#                              different points of the creation.
#   FROMNAMES-SYMBOLS ...      not a rule so much as a report: this line names
#                              the symbols whose answer depends on the active
#                              layout, and it is printed rather than compared.
#                              stock SDL3 looks the character up in the keymap of
#                              the layout that is active (German "?" -> 0xdf,
#                              "less" -> 0x3c), SDLop keeps its answer in the US
#                              tables on every layout, so it says 0x2f and 0x2c.
#                              Comparing it would mean either implementing that
#                              lookup or hiding a real difference; the roadmap
#                              keeps it as a known one.
#   MOTION with no movement    stock drops those itself ("Drop events that don't
#                              change state") and SDLop never sends them, but the
#                              two disagree about when the pointer counts as
#                              having a position.
#   KEYMAP_CHANGED             how many of these events a layout switch produces,
#                              and when they arrive, belongs to the X server's
#                              idea of this client, not to the library.
#                              Measured on this server: one `setxkbmap` sends
#                              stock SDL3 three core MappingNotify events at the
#                              switch (three XRefreshKeyboardMapping calls, all
#                              request=MappingKeyboard), and a stray fourth one
#                              only after the *next* key press, so whether one of
#                              them lands before or after READY-INPUT depends on
#                              the run. SDLop receives none of them: this server
#                              sends core MappingNotify only to clients that never
#                              spoke XKB to it, and SDLop's connection must speak
#                              XKB (its keymap comes from xkbcommon-x11), so it
#                              answers the root property the server rewrites
#                              instead - one announcement per switch (see
#                              src/video/SDL_x11.c, PropertyNotify). The count and
#                              the position of these lines are therefore not
#                              compared; that both probes announce the switch *at
#                              all* is, and it is asserted separately in
#                              compare().
#
#                              The lines are removed here rather than collapsed so
#                              that the stray event, which arrives on its own, is
#                              covered by the same rule.
documented_patterns()
{
    # Chained with pipes, not listed as separate commands: separate commands in a
    # function body would *each* read the function's own stdin, so only the first
    # one would ever see the trace (the rest find it already consumed and pass
    # nothing through) - which is how a rule here can look right and do nothing.
    grep -vE '^STATE flags=' |
        grep -vE '^FROMNAMES-SYMBOLS ' |
        grep -vE '^EVENT input MOTION .*xrel=0\.0 yrel=0\.0$' |
        grep -vE '^EVENT input KEYMAP_CHANGED$'
}

# The window-lifecycle phase is compared as a *set* rather than in sequence: when
# the server tells a window that it has been mapped, exposed and focused depends
# on the order the X requests were issued in (stock SDL3's X11_ShowWindow waits
# for MapNotify and pumps the event queue inside SDL_CreateWindow, SDLop maps the
# window and lets the application's first pump deal with it), and no application
# can depend on that order. Everything the application itself causes - the input
# events, and the window operations the probe performs - is compared in order.
normalize() { sed -e 's/[[:space:]]*$//' -e '/^$/d' "$1" | documented_patterns | grep -v '^EVENT phase1 '; }

segment()        { sed -n "/^EVENT $1/p" "$2"; }                     # one phase of a trace
segment_sorted() { sed -n "/^EVENT $1/p" "$2" | sort; }
# The lifecycle phase is compared raw, but the same documented rule about
# KEYMAP_CHANGED applies to it: a stray server-deferred event can land before
# READY-INPUT as easily as after it.
phase_events()   { sed -n "/^EVENT $1/p" "$2" | grep -vE '^EVENT .* KEYMAP_CHANGED$'; }

phase_report()
{
    phase=$1; sdlop=$2; stock=$3
    phase_events "$phase" "$stock" > "$TMP/p_stock.txt"
    phase_events "$phase" "$sdlop" > "$TMP/p_sdlop.txt"
    sort "$TMP/p_stock.txt" > "$TMP/p_stock.sorted"
    sort "$TMP/p_sdlop.txt" > "$TMP/p_sdlop.sorted"
    n_sdlop=$(grep -c . "$TMP/p_sdlop.txt")
    n_stock=$(grep -c . "$TMP/p_stock.txt")
    difference=$(diff "$TMP/p_stock.sorted" "$TMP/p_sdlop.sorted")

    if [ -z "$difference" ]; then
        if cmp -s "$TMP/p_stock.txt" "$TMP/p_sdlop.txt"; then
            printf '  %-7s identical order and set (%s events)\n' "$phase" "$n_sdlop"
        else
            printf '  %-7s the same %s events, in a different order\n' "$phase" "$n_sdlop"
        fi
        return 0
    fi
    printf '  %-7s different sets (%s vs %s events):\n' "$phase" "$n_sdlop" "$n_stock"
    printf '%s\n' "$difference" | sed -n 's/^</   stock only:/p; s/^>/   sdlop only:/p' |
        sed 's/^/      /' | head -12
    return 1
}

compare()
{
    layout=$1; tag=$2
    sdlop="$TMP/trace_sdlop$tag.txt"; stock="$TMP/trace_stock$tag.txt"
    printf '\n== layout %s\n' "$layout"
    # Refuse to compare a leg the server did not actually switch to: both traces
    # would come from the default layout, the diff would pass, and the thing this
    # leg exists for - a layout whose symbols and dead keys are not US - would not
    # have been tested at all. That is the failure mode this check is for.
    if ! layout_took "$layout"; then
        skip "this X server did not switch to '$layout' (it reports '$(current_layout)'): setxkbmap exits 0 but the keymap does not change here, so the leg would compare the default layout with itself"
        return
    fi
    run_one "$BUILD/tools/behaviour_sdlop" SDLop "$layout" "$sdlop" || return
    if [ "$HAVE_STOCK" != 1 ]; then
        skip "stock SDL3 development files are not installed - SDLop trace in $sdlop"
        return
    fi
    run_one "$BUILD/tools/behaviour_stock" "stock SDL3" "$layout" "$stock" || return

    # 0. both probes must have used the same video driver, or the diff below is
    #    comparing two different backends
    sdlop_driver=$(sed -n 's/^PROBE driver=\([^ ]*\).*/\1/p' "$sdlop")
    stock_driver=$(sed -n 's/^PROBE driver=\([^ ]*\).*/\1/p' "$stock")
    if [ "$sdlop_driver" != "$stock_driver" ]; then
        bad "the probes used different video drivers ($sdlop_driver vs $stock_driver) - refusing to compare"
        return
    fi

    # 1. both probes must have announced the live layout switch the drive()
    #    sequence makes. The number and position of those events are the
    #    server's business (documented above), but *having* them is not: a
    #    library that ignores a layout switch fails here.
    switch_events=1
    for trace in "$sdlop" "$stock"; do
        if ! grep -q '^EVENT input KEYMAP_CHANGED$' "$trace"; then
            bad "$(basename "$trace"): the layout was switched mid-run and no SDL_EVENT_KEYMAP_CHANGED came out"
            switch_events=0
        fi
    done
    [ "$switch_events" = 1 ] && ok "both probes announced the live layout switch"

    # 2. the input events and the window operations, in order, once the
    #    documented rules are applied: this is the part an application sees
    normalize "$sdlop" > "$sdlop.n"; normalize "$stock" > "$stock.n"
    if diff -u "$stock.n" "$sdlop.n" > "$TMP/diff$tag.txt"; then
        stock_symbols=$(grep '^FROMNAMES-SYMBOLS ' "$stock" | sed 's/^FROMNAMES-SYMBOLS //')
        sdlop_symbols=$(grep '^FROMNAMES-SYMBOLS ' "$sdlop" | sed 's/^FROMNAMES-SYMBOLS //')
        if [ "$stock_symbols" = "$sdlop_symbols" ]; then
            ok "the layout-dependent symbol names agree too: $sdlop_symbols"
        else
            note "symbol names follow the layout in stock, the US tables in SDLop:"
            note "  stock: $stock_symbols"
            note "  SDLop: $sdlop_symbols"
        fi
        segment input "$stock" > "$TMP/i_stock.txt"
        ok "same input and window-operation events, in order ($(grep -c . "$TMP/i_stock.txt") input events)"
    else
        bad "input or window-operation events differ - see $TMP/diff$tag.txt"
        sed -n '1,30p' "$TMP/diff$tag.txt" | sed 's/^/      /'
    fi

    # 3. the lifecycle phase, as a set: this is where the two drivers place
    #    SHOWN/EXPOSED/FOCUS_* differently, which is why it is reported apart
    if phase_report phase1 "$sdlop" "$stock"; then
        ok "the window lifecycle events match as a set"
    else
        bad "the window lifecycle events differ"
    fi
}

compare "us"     ""
compare "$DEADKEY_LAYOUT" "_$DEADKEY_LAYOUT"      # a layout with dead keys

setxkbmap us 2>/dev/null
printf '\n%s\n' "$( [ $fail = 0 ] && echo 'behaviour-check: PASS' || echo 'behaviour-check: FAIL')"
exit $fail
