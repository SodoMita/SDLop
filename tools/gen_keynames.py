#!/usr/bin/env python3
"""
gen_keynames.py -- build the key/scancode name tables used by SDL_GetKeyName(),
SDL_GetScancodeName(), SDL_GetKeyFromName() and SDL_GetScancodeFromName().

Names follow SDL3's convention ("A", "Space", "Left Shift", "Keypad 1", "F12"...).
The tables are generated from the enum/macro values in the SDLop headers, so they
always cover every key the API can produce.

Usage: tools/gen_keynames.py [-o src/events/sdlop_keynames.h]
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INC = os.path.join(ROOT, "include", "SDL3")

PUNCT = {
    "EXCLAIM": "!", "DBLAPOSTROPHE": '"', "HASH": "#", "DOLLAR": "$", "PERCENT": "%",
    "AMPERSAND": "&", "APOSTROPHE": "'", "LEFTPAREN": "(", "RIGHTPAREN": ")",
    "ASTERISK": "*", "PLUS": "+", "COMMA": ",", "MINUS": "-", "PERIOD": ".",
    "SLASH": "/", "COLON": ":", "SEMICOLON": ";", "LESS": "<", "EQUALS": "=",
    "GREATER": ">", "QUESTION": "?", "AT": "@", "LEFTBRACKET": "[", "BACKSLASH": "\\",
    "RIGHTBRACKET": "]", "CARET": "^", "UNDERSCORE": "_", "GRAVE": "`",
    "KP_DIVIDE": "Keypad /", "KP_MULTIPLY": "Keypad *", "KP_MINUS": "Keypad -",
    "KP_PLUS": "Keypad +", "KP_ENTER": "Keypad Enter", "KP_EQUALS": "Keypad =",
    "KP_PERIOD": "Keypad .", "KP_COMMA": "Keypad ,", "KP_DECIMAL": "Keypad .",
}

WORDS = {
    "RETURN": "Return", "ESCAPE": "Escape", "BACKSPACE": "Backspace", "TAB": "Tab",
    "SPACE": "Space", "CAPSLOCK": "CapsLock", "SCROLLLOCK": "ScrollLock",
    "PRINTSCREEN": "PrintScreen", "PAUSE": "Pause", "INSERT": "Insert", "HOME": "Home",
    "PAGEUP": "PageUp", "DELETE": "Delete", "END": "End", "PAGEDOWN": "PageDown",
    "RIGHT": "Right", "LEFT": "Left", "DOWN": "Down", "UP": "Up",
    "NUMLOCKCLEAR": "Numlock/Clear", "CLEAR": "Clear",
    "LCTRL": "Left Ctrl", "LSHIFT": "Left Shift", "LALT": "Left Alt", "LGUI": "Left GUI",
    "RCTRL": "Right Ctrl", "RSHIFT": "Right Shift", "RALT": "Right Alt", "RGUI": "Right GUI",
    "MODE": "AltGr", "HELP": "Help", "MENU": "Menu", "APPLICATION": "Application",
    "POWER": "Power", "SLEEP": "Sleep", "WAKE": "Wake", "EXECUTE": "Execute",
    "SELECT": "Select", "STOP": "Stop", "AGAIN": "Again", "UNDO": "Undo", "CUT": "Cut",
    "COPY": "Copy", "PASTE": "Paste", "FIND": "Find", "MUTE": "Mute", "VOLUMEUP": "VolumeUp",
    "VOLUMEDOWN": "VolumeDown", "STANDBY": "Standby", "CANCEL": "Cancel",
    "PRIOR": "Prior", "SEPARATOR": "Separator", "OUT": "Out", "OPER": "Oper",
    "KP_CLEAR": "Keypad Clear", "KP_CLEARENTRY": "Keypad ClearEntry",
    "KP_BACKSPACE": "Keypad Backspace", "KP_TAB": "Keypad Tab",
    "KP_SPACE": "Keypad Space", "KP_EXCLAM": "Keypad !", "KP_HASH": "Keypad #",
    "KP_HASHHASH": "Keypad ##", "KP_PARENLEFT": "Keypad (", "KP_PARENRIGHT": "Keypad )",
    "KP_000": "Keypad 000", "KP_BINARY": "Keypad Binary", "KP_OCTAL": "Keypad Octal",
    "KP_DECIMAL2": "Keypad Decimal", "KP_HEX": "Keypad Hex", "KP_A": "Keypad A",
    "KP_XOR": "Keypad XOR", "KP_PERCENT": "Keypad %", "KP_AT": "Keypad @",
    "KP_AMPERSAND": "Keypad &", "KP_DBLAMPERSAND": "Keypad &&",
    "KP_VERTICALBAR": "Keypad |", "KP_DBLVERTICALBAR": "Keypad ||",
    "KP_COLON": "Keypad :", "KP_POWER": "Keypad ^",
    "AC_FORWARD": "AC Forward", "AC_BACK": "AC Back", "AC_REFRESH": "AC Refresh",
    "AC_STOP": "AC Stop", "AC_BOOKMARKS": "AC Bookmarks", "AC_HOME": "AC Home",
    "AC_SEARCH": "AC Search",
}


SHIFT_PAIRS = {
    "1": "!", "2": "@", "3": "#", "4": "$", "5": "%", "6": "^", "7": "&",
    "8": "*", "9": "(", "0": ")", "-": "_", "=": "+", "[": "{", "]": "}",
    "\\": "|", ";": ":", "'": "\"", "`": "~", ",": "<", ".": ">", "/": "?",
}


def upstream_names():
    """SDL3's own scancode names (tools/reference/scancode_names.txt), so
    SDL_GetScancodeFromName("Volume Up") behaves exactly like SDL3's."""
    path = os.path.join(ROOT, "tools/reference/scancode_names.txt")
    table = {}
    if not os.path.exists(path):
        return table
    for line in open(path):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        index, _, name = line.partition("\t")
        try:
            table[int(index)] = name
        except ValueError:
            continue
    return table


UPSTREAM_NAMES = None


def pretty(ident):
    """SDL3-style display name for a SDLK_/SDL_SCANCODE_ identifier."""
    body = ident
    if body in PUNCT:
        return PUNCT[body]
    if body in WORDS:
        return WORDS[body]
    m = re.fullmatch(r"F(\d+)", body)
    if m:
        return "F" + m.group(1)
    m = re.fullmatch(r"KP_(\d+)", body)
    if m:
        return "Keypad " + m.group(1)
    if len(body) == 1 and body.isalnum():
        return body.upper()
    return body


def parse_scancodes():
    text = open(os.path.join(INC, "SDL_scancode.h")).read()
    body = re.search(r"typedef enum SDL_Scancode\s*\{(.*?)\}\s*SDL_Scancode;", text, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    table = {}
    for m in re.finditer(r"(SDL_SCANCODE_[A-Z0-9_]+)\s*=\s*(\d+)", body):
        name = m.group(1)[len("SDL_SCANCODE_"):]
        if name == "COUNT":
            continue
        table[int(m.group(2))] = name
    return table


def parse_keycodes(scan):
    text = open(os.path.join(INC, "SDL_keycode.h")).read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    table = {}
    for m in re.finditer(r"^#define\s+SDLK_([A-Z0-9_a-z]+)\s+([^/\n]+)$", text, re.M):
        name, value = m.group(1), m.group(2).strip()
        if name in ("EXTENDED_MASK", "SCANCODE_MASK", "SCANCODE_TO_KEYCODE"):
            continue
        try:
            if value.startswith("'"):
                code = ord(value.strip("'").replace("\\r", "\r").replace("\\t", "\t")
                           .replace("\\b", "\b").replace("\\x1B", "\x1b").replace("\\\\", "\\"))
            else:
                code = int(value.rstrip("uUlL"), 0)
        except ValueError:
            # SDLK_* values like SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F1) are
            # derived from the scancode enum, so resolve them through the
            # scancode table instead of skipping the key.
            derived = re.match(r"SDL_SCANCODE_TO_KEYCODE\(\s*SDL_SCANCODE_([A-Z0-9_]+)\s*\)", value)
            if derived:
                wanted = derived.group(1)
                code = next((sc | (1 << 30) for sc, n in scan.items() if n == wanted), 0)
            else:
                continue
        if code == 0 and name != "UNKNOWN":
            continue
        if name not in table:
            table[name] = code
    return table


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", default=os.path.join(ROOT, "src/generated/sdlop_keynames.h"))
    args = ap.parse_args()

    global UPSTREAM_NAMES
    UPSTREAM_NAMES = upstream_names()
    scan = parse_scancodes()
    keys = parse_keycodes(scan)
    name_to_code = {pretty(n).lower(): c for n, c in keys.items()}
    name_to_scancode = {}
    for sc, scname in scan.items():
        name_to_scancode[name_to_code.get((UPSTREAM_NAMES.get(sc) or pretty(scname)).lower(), -1)] = sc
    max_scancode = max(scan) + 1

    out = []
    out.append("/*")
    out.append("  SDLop -- key and scancode name tables (GENERATED by tools/gen_keynames.py).")
    out.append("")
    out.append("  Names follow SDL3's convention, so key names in config files and in")
    out.append("  SDL_GetScancodeFromName() lookups match what SDL3 users expect.")
    out.append("*/")
    out.append("")
    out.append("#ifndef SDLOP_KEYNAMES_H")
    out.append("#define SDLOP_KEYNAMES_H")
    out.append("")
    out.append("#include <SDL3/SDL.h>")
    out.append("")
    out.append("#define SDLOP_NUM_SCANCODES %d" % max_scancode)
    out.append("")
    out.append("typedef struct SDLOP_KeyName")
    out.append("{")
    out.append("    SDL_Keycode code;")
    out.append("    const char *name;")
    out.append("} SDLOP_KeyName;")
    out.append("")
    out.append("/* Keys with a name, sorted case-insensitively by name so that")
    out.append("   SDL_GetKeyFromName() can binary search. Keys without a name (the")
    out.append("   extended SDLK_* values that only exist as scancode | SCANCODE_MASK)")
    out.append("   are not listed: SDL_GetKeyName() falls back to \"Unknown\" for them,")
    out.append("   exactly like SDL3 does. */")
    scancode_name_for_keycode = {}
    for sc, scname in scan.items():
        code = name_to_code.get(scname.lower())
        if code is not None and code in keys.values():
            scancode_name_for_keycode.setdefault(code, scname)
    keyitems = sorted(((UPSTREAM_NAMES.get(name_to_scancode.get(c, -1)) or pretty(n), c)
                       for n, c in keys.items()), key=lambda kv: kv[0].lower())
    out.append("/* the tables are shared by several translation units, so silence the")
    out.append("   unavoidable 'defined but not used' warning in the ones that need less */")
    out.append("#if defined(__GNUC__)")
    out.append("#define SDLOP_TABLE __attribute__((unused))")
    out.append("#else")
    out.append("#define SDLOP_TABLE")
    out.append("#endif")
    out.append("")
    out.append("SDLOP_TABLE static const SDLOP_KeyName sdlop_keynames[] = {")
    for name, code in keyitems:
        esc = name.replace("\\", "\\\\").replace('"', '\\"')
        out.append('    { %s, "%s" },' % (hex(code), esc))
    out.append("};")
    out.append("")
    out.append("SDLOP_TABLE static const char *sdlop_scancode_names[SDLOP_NUM_SCANCODES] = {")
    for i in range(max_scancode):
        if i in scan:
            name = (UPSTREAM_NAMES or {}).get(i) or pretty(scan[i])
            out.append('    "%s", ' % name.replace("\\", "\\\\").replace('"', '\\"'))
        else:
            out.append("    NULL,")
    out.append("};")
    out.append("")
    out.append("/* scancode -> keycode: the SDLK_* value when the key has a name for it,")
    out.append("   otherwise the SDL_SCANCODE_TO_KEYCODE() encoding SDL3 uses */")
    out.append("SDLOP_TABLE static const SDL_Keycode sdlop_keycode_for_scancode[SDLOP_NUM_SCANCODES] = {")
    name_to_code = {pretty(n).lower(): c for n, c in keys.items()}
    for i in range(max_scancode):
        if i in scan:
            base = name_to_code.get(pretty(scan[i]).lower())
            out.append("    %s, /* %s */" % ("0x%08Xu" % base if base else "0x%08Xu" % (i | (1 << 30)), scan[i]))
        else:
            out.append("    0,")
    out.append("};")
    out.append("")
    out.append("/* the character each key produces with Shift held (US layout, as SDL3's")
    out.append("   built-in keymap does); 0 when the key is unaffected by Shift */")
    out.append("SDLOP_TABLE static const SDL_Keycode sdlop_shifted_keycode_for_scancode[SDLOP_NUM_SCANCODES] = {")
    for i in range(max_scancode):
        shifted = 0
        if i in scan:
            base = name_to_code.get(pretty(scan[i]).lower())
            if base and 0x61 <= base <= 0x7A:
                shifted = base - 0x20
            elif base:
                sp = SHIFT_PAIRS.get(pretty(scan[i]))
                if sp:
                    shifted = name_to_code.get(sp.lower(), 0)
        out.append("    %s," % ("0x%08Xu" % shifted if shifted else "0"))
    out.append("};")
    out.append("")
    out.append("/* scancodes that share a name with another key need explicit lookups */")
    out.append("SDLOP_TABLE static const SDLOP_KeyName sdlop_scancode_names_by_name[] = {")
    sn = sorted(((pretty(v), k) for k, v in scan.items()), key=lambda kv: kv[0].lower())
    for name, code in sn:
        out.append('    { %d, "%s" },' % (code, name.replace("\\", "\\\\").replace('"', '\\"')))
    out.append("};")
    out.append("")
    out.append("#endif /* SDLOP_KEYNAMES_H */")
    open(args.output, "w").write("\n".join(out) + "\n")
    print("wrote %s: %d scancodes, %d keycodes, %d scancode names"
          % (args.output, len(scan), len(keyitems), len(sn)))


if __name__ == "__main__":
    sys.exit(main())
