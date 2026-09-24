#!/bin/sh
#
# Helpers shared by the Wayland client tests (tests/wayland_input.sh and
# tests/wayland_display.sh). They do two things a client cannot do for itself:
#
#   * make the compositor session deterministic. The tests run against a
#     *headless* compositor whose outputs they create and destroy through
#     swaymsg, so every run starts by rebuilding the output set - a leftover
#     output from an earlier run would silently change what is measured.
#
#   * find the client window. A tiling compositor decides where a toplevel goes,
#     and wl_inject injects events in compositor-global coordinates, so the
#     window's position has to be read back from the compositor's own tree
#     instead of being assumed.
#
# Requires sway (swaymsg) when the compositor is sway. When swaymsg is not
# available or is not talking to this session, the helpers degrade to no-ops and
# the callers fall back to fixed coordinates.

output_names() {
    command -v swaymsg >/dev/null 2>&1 || return 0
    swaymsg -t get_outputs -r 2>/dev/null | grep -oE '"name": "[A-Za-z0-9_.-]+"' | cut -d'"' -f4
}

have_sway() {
    command -v swaymsg >/dev/null 2>&1 && swaymsg -t get_version >/dev/null 2>&1
}

# reset_outputs <mode1> <scale1> <x1> <mode2> <scale2> <x2>
# Rebuilds the session as exactly two outputs and exports FIRST_OUTPUT /
# SECOND_OUTPUT. Returns 1 when the compositor cannot provide two outputs.
reset_outputs() {
    have_sway || return 1

    for name in $(output_names); do
        swaymsg output "$name" unplug >/dev/null 2>&1
    done
    sleep 0.5
    swaymsg create_output >/dev/null 2>&1
    swaymsg create_output >/dev/null 2>&1
    sleep 1.5

    names=$(output_names)
    first=$(printf '%s\n' "$names" | sed -n 1p)
    second=$(printf '%s\n' "$names" | sed -n 2p)
    if [ -z "$first" ] || [ -z "$second" ]; then
        return 1
    fi
    swaymsg output "$first" mode "$1" scale "$2" position "$3" 0 >/dev/null 2>&1
    swaymsg output "$second" mode "$4" scale "$5" position "$6" 0 >/dev/null 2>&1
    sleep 0.5
    FIRST_OUTPUT=$first
    SECOND_OUTPUT=$second
    return 0
}

focus_output() { # focus_output <name>
    have_sway || return 0
    swaymsg focus output "$1" >/dev/null 2>&1
}

# The centre of the window with this title, in compositor coordinates, or empty.
window_centre() { # window_centre <title>
    have_sway || return 0
    swaymsg -t get_tree 2>/dev/null | python3 -c '
import json, sys
title = sys.argv[1]
def walk(node):
    if node.get("name") == title and node.get("rect"):
        r = node["rect"]
        print("%d %d" % (r["x"] + r["width"] // 2, r["y"] + r["height"] // 2))
        raise SystemExit
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        walk(child)
try:
    walk(json.load(sys.stdin))
except SystemExit:
    pass
' "$1"
}
