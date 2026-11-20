#!/bin/sh
# Differential test for SDL_pixels.h / SDL_rect.h / SDL_surface.h.
#
#   run_surface_diff.sh <surface_diff built against SDLop> [<surface_diff built against stock SDL3>]
#
# tests/surface_diff.c prints one hash per case (every format pair x blend
# mode x colour/alpha mod x colorkey, conversions, fills, flips, rotations,
# scaling, tiling, 9-grid, premultiply, rect maths ...). With a reference
# binary the two outputs must be identical; without one the probe still
# runs every case as a crash / sanitizer smoke test.
#
# The reference runs with SDL_CPU_FEATURE_MASK=-all: stock SDL's SSE/AVX
# blitters round differently from its own C paths (SDLop implements the C
# paths), so the comparison is against SDL's portable code.
set -u
probe=$1
ref=${2:-}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

SDL_VIDEO_DRIVER=dummy "$probe" > "$out/sdlop.txt"
rc=$?
if [ $rc -ne 0 ] || [ "$(tail -n 1 "$out/sdlop.txt")" != done ]; then
    echo "FAIL: SDLop probe exited with $rc before finishing"
    tail -n 5 "$out/sdlop.txt"
    exit 1
fi
cases=$(grep -c '^[0-9]' "$out/sdlop.txt")

if [ -z "$ref" ]; then
    echo "surface_diff: $cases cases ran (no reference SDL3 - smoke test only)"
    exit 0
fi

SDL_VIDEO_DRIVER=dummy SDL_CPU_FEATURE_MASK=-all "$ref" > "$out/ref.txt"
rc=$?
if [ $rc -ne 0 ]; then
    echo "FAIL: reference probe exited with $rc"
    exit 1
fi

if ! diff "$out/ref.txt" "$out/sdlop.txt" > "$out/diff.txt"; then
    n=$(grep -c '^<' "$out/diff.txt")
    echo "FAIL: $n of $cases cases differ from stock SDL3 (< reference, > SDLop):"
    head -n 40 "$out/diff.txt"
    echo "rerun a case with SURFACE_DIFF_DUMP=<case number> to print its pixels"
    exit 1
fi
echo "surface_diff: all $cases cases identical to stock SDL3"
