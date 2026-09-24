# Reference data

Files in this directory are *reference* material, kept so the generators in
`tools/` can be re-run and so that anything generated from them can be verified
without network access. They come from upstream SDL3 (release-3.2.10, zlib
licence, Copyright (C) 1997-2025 Sam Lantinga and SDL contributors):

| file | origin |
|------|--------|
| `SDL3/*.h` | the 70 public SDL3 headers (`include/SDL3/`) |
| `scancodes_linux.h` | `src/events/scancodes_linux.h` (Linux evdev -> SDL scancode) |
| `scancode_names.txt` | extracted from `src/events/SDL_keymap.c` (`SDL_scancode_names[]`) |

To refresh them:

```sh
BASE=https://raw.githubusercontent.com/libsdl-org/SDL/release-3.2.10
curl -sS -o tools/reference/scancodes_linux.h $BASE/src/events/scancodes_linux.h
curl -sS -o /tmp/SDL_keymap.c               $BASE/src/events/SDL_keymap.c
python3 tools/gen_evdev.py --refresh-names    # rewrites scancode_names.txt
```

`tools/sdlop.py` (the header generator) reads `SDL3/` only; the names and the
evdev table are read by `tools/gen_keynames.py` and `tools/gen_evdev.py`.
