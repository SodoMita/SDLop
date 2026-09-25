#!/bin/sh
# Build tools/abi_probe.c twice - against SDLop's headers and against stock
# SDL3 reference headers - and diff. Usage: run_abi_check.sh <sdl3-include-dir>
set -e
SDL3_INC="$1"
[ -n "$SDL3_INC" ] || { echo "usage: $0 /path/to/SDL3/include"; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
gcc -o "$TMP/probe_sdlop" "$ROOT/tools/abi_probe.c" -I"$ROOT/include"
gcc -o "$TMP/probe_sdl3"  "$ROOT/tools/abi_probe.c" -I"$SDL3_INC"
"$TMP/probe_sdl3"  > "$TMP/sdl3.txt"
"$TMP/probe_sdlop" > "$TMP/sdlop.txt"
if diff -u "$TMP/sdl3.txt" "$TMP/sdlop.txt"; then
    echo "abi_check: PASS ($(wc -l < "$TMP/sdl3.txt") values identical to SDL3 reference)"
else
    echo "abi_check: FAIL - ABI diverges from the SDL3 reference headers"
    exit 1
fi
