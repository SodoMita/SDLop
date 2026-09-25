#!/bin/sh
# SDLop API subset gate.
#
# Usage: api_subset_check.sh <libSDLop.so> <stock libSDL3.so>
#
# Verifies that every SDL_* symbol SDLop exports is also exported by stock
# SDL3 -- the "SDL3/ is a strict subset" property. Catches SDL2-era names
# (SDL_GetKeyState, SDL_QuitRequested) or invented APIs being exported again.
# Note: symbol-level only; signature-level parity is the ABI probe's job.
set -eu

SDLOP_SO=${1:?usage: api_subset_check.sh <libSDLop.so> <libSDL3.so>}
SDL3_SO=${2:?usage: api_subset_check.sh <libSDLop.so> <libSDL3.so>}

extract() {
    nm -D --defined-only "$1" \
        | awk '$2=="T" || $2=="i" {sub(/@.*/,"",$3); print $3}' \
        | grep -E '^SDL_' | sort -u
}

sdlop_syms=$(extract "$SDLOP_SO")
sdl3_syms=$(extract "$SDL3_SO")

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
printf '%s\n' "$sdl3_syms" > "$tmp"
extra=$(printf '%s\n' "$sdlop_syms" | grep -Fxv -f "$tmp" || true)

if [ -n "$extra" ]; then
    echo "api_subset_check: FAIL - symbols exported by SDLop but NOT by stock SDL3:" >&2
    printf '%s\n' "$extra" >&2
    exit 1
fi

count=$(printf '%s\n' "$sdlop_syms" | wc -l)
sdl3_count=$(printf '%s\n' "$sdl3_syms" | wc -l)
echo "api_subset_check: PASS ($count SDLop exports are a strict subset of $sdl3_count stock SDL3 exports)"
