#!/usr/bin/env python3
"""
Generate SDLop's public headers from the stock SDL3 headers.

    tools/gen_headers.py /path/to/SDL3-3.4.20/include

SDLop's API is a strict subset of SDL3's. Rather than hand-writing that
subset (which drifts: wrong values, missing extern "C", ...), every header
is the upstream header with the declarations of functions SDLop does *not*
implement removed. Types, enums, macros, inline helpers and documentation
stay byte-identical to upstream, so applications see exactly SDL3's ABI and
calling an unimplemented function fails at compile time, not at link time.

  tools/sdlop_api.txt   functions SDLop implements (one per line, # comments)
  HEADERS (below)       headers shipped (closure of what SDL.h needs)

What is removed:
  - `extern SDL_DECLSPEC ... SDLCALL SDL_Name(...);` for names not in the
    API list, together with their doc comment
  - SDL_FORCE_INLINE helpers whose body calls a removed function
App-provided entry points (SDL_main.h: SDL_AppInit, SDL_main, ...) are kept.

tests/run_abi_check.sh verifies the result against the same reference headers
(signatures, layouts, constants) and that every declared function is exported.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "include", "SDL3")

# Headers SDLop ships. The "types only" ones carry no implemented functions
# but are needed for SDL_Event's full union (joystick, audio, pen, ...) and
# other types referenced by the implemented headers.
HEADERS = [
    "SDL.h",  # generated separately (aggregate)
    "SDL_begin_code.h", "SDL_close_code.h", "SDL_platform_defines.h",
    "SDL_platform.h", "SDL_stdinc.h", "SDL_endian.h", "SDL_assert.h",
    "SDL_version.h", "SDL_error.h", "SDL_init.h", "SDL_hints.h", "SDL_log.h",
    "SDL_timer.h", "SDL_properties.h", "SDL_rect.h", "SDL_pixels.h",
    "SDL_blendmode.h", "SDL_surface.h", "SDL_video.h", "SDL_events.h",
    "SDL_keyboard.h", "SDL_keycode.h", "SDL_scancode.h", "SDL_mouse.h",
    "SDL_touch.h", "SDL_clipboard.h", "SDL_misc.h", "SDL_messagebox.h",
    "SDL_vulkan.h", "SDL_main.h", "SDL_main_impl.h",
    # types only (referenced by the above)
    "SDL_atomic.h", "SDL_audio.h", "SDL_camera.h", "SDL_gamepad.h",
    "SDL_guid.h", "SDL_iostream.h", "SDL_joystick.h", "SDL_mutex.h",
    "SDL_pen.h", "SDL_power.h", "SDL_sensor.h", "SDL_thread.h",
]

# Provided by the application, not the library.
APP_PROVIDED = {"SDL_main", "SDL_AppInit", "SDL_AppIterate", "SDL_AppEvent", "SDL_AppQuit"}

NOTE = (" *\n"
        " * SDLop: generated from SDL {ver} by tools/gen_headers.py - declarations of\n"
        " * functions SDLop does not implement are removed; everything else is\n"
        " * upstream's text. Do not edit by hand.\n")


def load_api():
    names = set()
    with open(os.path.join(HERE, "sdlop_api.txt")) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                names.add(line)
    return names


def find_decl_start(text, pos):
    """Start of the statement containing pos, including a directly
    preceding /** doc */ comment."""
    # back up to the previous statement terminator / directive / comment end
    start = text.rfind("\n", 0, pos) + 1
    # include preceding doc comment (only blank lines in between)
    before = text[:start]
    m = re.search(r"/\*\*(?:(?!\*/).)*\*/[ \t]*\n(?:[ \t]*\n)*\Z", before, re.S)
    if m:
        start = m.start()
        # take the comment's indentation with it
        start = text.rfind("\n", 0, start) + 1
    return start


def strip_functions(text, api, removed):
    out = []
    i = 0
    # extern declarations
    decl = re.compile(r"^[ \t]*extern\s+(?:SDL_DECLSPEC|SDLMAIN_DECLSPEC)\b[^;{]*?SDLCALL\s*\**\s*(SDL_\w+)\s*\([^;{]*;[ \t]*\n",
                      re.M | re.S)
    for m in decl.finditer(text):
        name = m.group(1)
        if name in api or name in APP_PROVIDED:
            continue
        s = find_decl_start(text, m.start())
        if s < i:
            s = m.start()
        out.append(text[i:s])
        i = m.end()
        removed.add(name)
    out.append(text[i:])
    return "".join(out)


def strip_inlines(text, removed):
    """Drop SDL_FORCE_INLINE function definitions that call removed functions."""
    pat = re.compile(r"^SDL_FORCE_INLINE\b[^{;]*?\b(SDL_\w+)\s*\([^{;]*\)\s*\{", re.M | re.S)
    while True:
        changed = False
        for m in pat.finditer(text):
            # find the matching closing brace
            depth = 0
            j = m.end() - 1
            while j < len(text):
                if text[j] == "{":
                    depth += 1
                elif text[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            body = text[m.end():j]
            calls = set(re.findall(r"\b(SDL_\w+)\s*\(", body))
            if calls & removed:
                s = find_decl_start(text, m.start())
                e = text.find("\n", j) + 1
                text = text[:s] + text[e:]
                removed.add(m.group(1))
                changed = True
                break
        if not changed:
            return text


def add_note(text, ver):
    # after the license block's first line of the leading comment
    m = re.match(r"/\*.*?\*/", text, re.S)
    if not m:
        return text
    body = m.group(0)
    return body[:-2].rstrip() + "\n" + NOTE.format(ver=ver) + " */" + text[m.end():]


def gen_sdl_h(ver):
    inc = [h for h in HEADERS if h not in ("SDL.h", "SDL_begin_code.h", "SDL_close_code.h",
                                           "SDL_platform_defines.h", "SDL_main.h", "SDL_main_impl.h")]
    lines = ["/*",
             "  SDLop - a lean, fast, SDL3-compatible windowing + input library.",
             "",
             "  Aggregate header: `#include <SDL3/SDL.h>` like SDL3. Generated from",
             "  SDL %s by tools/gen_headers.py; the subsystems SDLop does not" % ver,
             "  implement (audio, joystick, threads, ...) are present as types only.",
             "",
             "  This software is provided 'as-is', without any express or implied",
             "  warranty (zlib license, see LICENSE).",
             "*/",
             "",
             "#ifndef SDL_h_",
             "#define SDL_h_",
             ""]
    lines += ["#include <SDL3/%s>" % h for h in inc]
    lines += ["",
              "/* SDLop's own version (the SDL API level is SDL_VERSION, SDL_version.h) */",
              "#define SDLOP_MAJOR_VERSION 0",
              "#define SDLOP_MINOR_VERSION 2",
              "#define SDLOP_PATCHLEVEL    0",
              "",
              "#endif /* SDL_h_ */",
              ""]
    return "\n".join(lines)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gen_headers.py /path/to/SDL3/include")
    ref = sys.argv[1]
    if os.path.isdir(os.path.join(ref, "SDL3")):
        ref = os.path.join(ref, "SDL3")
    vh = open(os.path.join(ref, "SDL_version.h")).read()
    ver = ".".join(re.search(r"#define SDL_%s_VERSION\s+(\d+)" % k, vh).group(1)
                   for k in ("MAJOR", "MINOR", "MICRO"))
    api = load_api()
    removed_all = set()
    texts = {}
    for h in HEADERS:
        if h == "SDL.h":
            continue
        texts[h] = strip_functions(open(os.path.join(ref, h)).read(), api, removed_all)
    # inline helpers may call functions from other headers: iterate globally
    while True:
        n = len(removed_all)
        for h in texts:
            texts[h] = strip_inlines(texts[h], removed_all)
        if len(removed_all) == n:
            break
    declared = set()
    for h, text in texts.items():
        declared |= set(re.findall(r"SDLCALL\s+\**\s*(SDL_\w+)\s*\(", text))
        with open(os.path.join(OUT, h), "w") as f:
            f.write(add_note(text, ver))
    with open(os.path.join(OUT, "SDL.h"), "w") as f:
        f.write(gen_sdl_h(ver))
    missing = sorted(api - declared)
    if missing:
        sys.exit("gen_headers: names in sdlop_api.txt that SDL %s does not declare: %s" % (ver, " ".join(missing)))
    print("gen_headers: SDL %s, %d headers, %d functions declared, %d removed"
          % (ver, len(HEADERS), len(declared - APP_PROVIDED), len(removed_all)))


if __name__ == "__main__":
    main()
