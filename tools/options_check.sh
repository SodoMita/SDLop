#!/bin/sh
# SDLop options gate: every SDLOP_* option must be settable from code, so
# library sources read them with SDL_GetHint / SDLOP_HintFlag /
# SDLOP_HintFirst (environment or hint), never with getenv/SDL_getenv.
#
# Usage: options_check.sh <SDLop source dir>
set -eu
SRC=${1:?usage: options_check.sh <source dir>}
hits=$(grep -rnE '(getenv|getenv_unsafe)\( *"(SDLOP_[A-Z0-9_]+|TTYNO)"' "$SRC/src" || true)
if [ -n "$hits" ]; then
    echo "options_check: FAIL - SDLOP_* options read from the environment only (use SDL_GetHint/SDLOP_HintFlag):" >&2
    echo "$hits" >&2
    exit 1
fi
echo "options_check: PASS"
