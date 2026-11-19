#!/bin/sh
# ABI check against stock SDL3 reference headers.
# Usage: run_abi_check.sh <sdl3-include-dir>
#
# 1. tools/abi_probe.c (struct layouts, offsets, selected constants) is
#    built against SDLop's headers and against the reference headers; the
#    outputs must be identical.
# 2. Every keycode / modifier / scancode constant SDLop declares
#    (SDLK_*, SDL_KMOD_*, SDL_SCANCODE_*) is printed by a generated probe
#    built against both header sets: a name SDL3 does not have fails to
#    compile, a wrong value shows up in the diff. This is what catches e.g.
#    SDLK_A having the value 'A' instead of SDL3's 'a'.
set -e
SDL3_INC="$1"
[ -n "$SDL3_INC" ] || { echo "usage: $0 /path/to/SDL3/include"; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CC="${CC:-cc}"

"$CC" -o "$TMP/probe_sdlop" "$ROOT/tools/abi_probe.c" -I"$ROOT/include"
"$CC" -o "$TMP/probe_sdl3"  "$ROOT/tools/abi_probe.c" -I"$SDL3_INC"
"$TMP/probe_sdl3"  > "$TMP/sdl3.txt"
"$TMP/probe_sdlop" > "$TMP/sdlop.txt"
if ! diff -u "$TMP/sdl3.txt" "$TMP/sdlop.txt"; then
    echo "abi_check: FAIL - ABI diverges from the SDL3 reference headers"
    exit 1
fi
n_layout=$(wc -l < "$TMP/sdl3.txt")

# --- 2. exhaustive keyboard constants ------------------------------------
{
    echo '#include <SDL3/SDL.h>'
    echo '#include <stdio.h>'
    echo 'int main(void) {'
    {
        grep -ohE '#define (SDLK|SDL_KMOD)_[A-Za-z0-9_]+' "$ROOT/include/SDL3/SDL_keycode.h" | awk '{print $2}'
        grep -ohE '^\s*SDL_SCANCODE_[A-Z0-9_]+\s*=' "$ROOT/include/SDL3/SDL_scancode.h" | tr -d ' =\t'
        grep -ohE '^\s*SDL_SCANCODE_COUNT\b' "$ROOT/include/SDL3/SDL_scancode.h" | tr -d ' \t'
    } | sort -u | grep -vE '^(SDLK_SCANCODE_MASK|SDLK_EXTENDED_MASK)$' | while read -r name; do
        printf '    printf("%%s=0x%%llx\\n", "%s", (unsigned long long)(%s));\n' "$name" "$name"
    done
    echo '    return 0;'
    echo '}'
} > "$TMP/keyprobe.c"
n_keys=$(grep -c printf "$TMP/keyprobe.c")
"$CC" -o "$TMP/key_sdlop" "$TMP/keyprobe.c" -I"$ROOT/include"
if ! "$CC" -o "$TMP/key_sdl3" "$TMP/keyprobe.c" -I"$SDL3_INC" 2> "$TMP/key_err.txt"; then
    grep -E 'error' "$TMP/key_err.txt" | head -20
    echo "abi_check: FAIL - SDLop declares keyboard constants SDL3 does not have"
    exit 1
fi
"$TMP/key_sdl3"  > "$TMP/keys_sdl3.txt"
"$TMP/key_sdlop" > "$TMP/keys_sdlop.txt"
if ! diff -u "$TMP/keys_sdl3.txt" "$TMP/keys_sdlop.txt"; then
    echo "abi_check: FAIL - keyboard constant values diverge from SDL3"
    exit 1
fi
echo "abi_check: PASS ($n_layout layout values + $n_keys keyboard constants identical to SDL3 reference)"
