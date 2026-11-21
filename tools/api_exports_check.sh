#!/bin/sh
# SDLop export gate.
#
# Usage: api_exports_check.sh <libSDLop.so> <SDLop include dir>
#
# Two-way check between the generated public headers and the shared library:
#   1. every function declared in include/SDL3/*.h is exported (a declared
#      but missing function would only fail when an app links against it);
#   2. every SDL_* function exported is declared (no undocumented exports;
#      tools/gen_headers.py already guarantees declarations exist in stock
#      SDL3, so together with (1) the export set is a subset of SDL3's).
set -eu

SO=${1:?usage: api_exports_check.sh <libSDLop.so> <include dir>}
INC=${2:?usage: api_exports_check.sh <libSDLop.so> <include dir>}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# declarations can span lines: join each header, then pick "SDLCALL name("
for h in "$INC"/SDL3/*.h; do
    tr '\n' ' ' < "$h" | grep -oE 'extern SDL_DECLSPEC [^;]*SDLCALL +SDL_[A-Za-z0-9_]+ *\(' \
        | sed -E 's/.*SDLCALL +(SDL_[A-Za-z0-9_]+) *\(/\1/' || true
done | sort -u > "$tmp/declared"

# ELF (.so/.dylib) or PE (.dll): a Windows DLL has no dynamic symbol table
# in the ELF sense, so ask objdump for its export directory instead.
if file -b "$SO" | grep -qi 'PE32'; then
    objdump -p "$SO" | awk '/^\[Ordinal\/Name Pointer\] Table/ {t=1; next}
                                 t && /^$/ {exit}
                                 t {print $NF}' | sort -u > "$tmp/exported"
else
    nm -D --defined-only "$SO" | awk '$2=="T" || $2=="i" {sub(/@.*/,"",$3); print $3}' \
        | sort -u > "$tmp/exported"
fi
grep -E '^SDL_' "$tmp/exported" > "$tmp/exported.sdl" && mv "$tmp/exported.sdl" "$tmp/exported"

missing=$(comm -23 "$tmp/declared" "$tmp/exported")
extra=$(comm -13 "$tmp/declared" "$tmp/exported")
status=0
if [ -n "$missing" ]; then
    echo "api_exports_check: FAIL - declared in the headers but not exported:" >&2
    printf '  %s\n' $missing >&2
    status=1
fi
if [ -n "$extra" ]; then
    echo "api_exports_check: FAIL - exported but not declared in the headers:" >&2
    printf '  %s\n' $extra >&2
    status=1
fi
[ $status -eq 0 ] && echo "api_exports_check: PASS ($(wc -l < "$tmp/declared") functions declared and exported)"
exit $status
