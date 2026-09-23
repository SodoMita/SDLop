import re

# Parse evdev codes
ev = {}
for ln in open("/usr/include/linux/input-event-codes.h"):
    m = re.match(r"#define\s+(KEY_[A-Z0-9_]+|BTN_[A-Z0-9_]+)\s+(0x[0-9a-fA-F]+|\d+)\b", ln)
    if m:
        name, val = m.group(1), int(m.group(2), 0)
        if name not in ev:
            ev[name] = val
KEY_MAX = max(v for k,v in ev.items() if k.startswith("KEY_"))

# Parse SDL scancodes
sc = {}
for ln in open("/home/user/sdlop/include/SDL3/SDL_scancode.h"):
    m = re.match(r"\s*(SDL_SCANCODE_[A-Z0-9_]+)\s*=\s*(\d+)", ln)
    if m:
        sc[m.group(1)] = int(m.group(2))
sc_by_norm = {}
for k, v in sc.items():
    n = k[len("SDL_SCANCODE_"):]
    sc_by_norm.setdefault(n, v)

def norm(name):
    n = name[len("KEY_"):]
    # LEFT/RIGHT shorten only for modifier keys
    for long, short in (("LEFTCTRL","LCTRL"),("LEFTSHIFT","LSHIFT"),("LEFTALT","LALT"),
                        ("LEFTMETA","LGUI"),("RIGHTCTRL","RCTRL"),("RIGHTSHIFT","RSHIFT"),
                        ("RIGHTALT","RALT"),("RIGHTMETA","RGUI")):
        if n == long:
            return short
    # keypad digits: KP7 -> KP_7
    m = re.match(r"^KP([0-9])$", n)
    if m:
        return "KP_" + m.group(1)
    return n

# Explicit overrides: evdev name -> SDL scancode name
OVERRIDE = {
 "KEY_ESC":"ESCAPE","KEY_ENTER":"RETURN","KEY_KPENTER":"KP_ENTER",
 "KEY_102ND":"NONUSHASH","KEY_COMPOSE":"APPLICATION","KEY_SYSRQ":"PRINTSCREEN",
 "KEY_NUMLOCK":"NUMLOCKCLEAR","KEY_KPSLASH":"KP_DIVIDE","KEY_KPASTERISK":"KP_MULTIPLY",
 "KEY_KPMINUS":"KP_MINUS","KEY_KPPLUS":"KP_PLUS","KEY_KPDOT":"KP_PERIOD",
 "KEY_KPEQUAL":"KP_EQUALS","KEY_KPLEFTPAREN":"KP_LEFTPAREN","KEY_KPRIGHTPAREN":"KP_RIGHTPAREN",
 "KEY_LEFTBRACE":"LEFTBRACKET","KEY_RIGHTBRACE":"RIGHTBRACKET",
 "KEY_DOT":"PERIOD","KEY_EQUAL":"EQUALS","KEY_GRAVE":"GRAVE",
 "KEY_LEFTMETA":"LGUI","KEY_RIGHTMETA":"RGUI",
 "KEY_MUTE":"MUTE","KEY_VOLUMEDOWN":"VOLUMEDOWN","KEY_VOLUMEUP":"VOLUMEUP",
 "KEY_PLAYPAUSE":"AUDIOPLAY","KEY_STOPCD":"AUDIOSTOP","KEY_PREVIOUSSONG":"AUDIOPREV",
 "KEY_NEXTSONG":"AUDIONEXT","KEY_REWIND":"AUDIOREWIND","KEY_FASTFORWARD":"AUDIOFASTFORWARD",
 "KEY_EJECTCLOSECD":"EJECT","KEY_HANGEUL":"LANG1","KEY_HANJA":"LANG2",
 "KEY_KATAKANA":"LANG3","KEY_HIRAGANA":"LANG4","KEY_ZENKAKUHANKAKU":"LANG5",
 "KEY_RO":"INTERNATIONAL1","KEY_KATAKANAHIRAGANA":"INTERNATIONAL2","KEY_YEN":"INTERNATIONAL3",
 "KEY_HENKAN":"INTERNATIONAL4","KEY_MUHENKAN":"INTERNATIONAL5","KEY_KPJPCOMMA":"INTERNATIONAL6",
 "KEY_CALC":"CALCULATOR","KEY_BACK":"AC_BACK","KEY_FORWARD":"AC_FORWARD",
 "KEY_SEARCH":"AC_SEARCH","KEY_HOMEPAGE":"AC_HOME","KEY_BOOKMARKS":"AC_BOOKMARKS",
 "KEY_NEW":"AC_NEW","KEY_OPEN":"AC_OPEN","KEY_CLOSE":"AC_CLOSE","KEY_SAVE":"AC_SAVE",
 "KEY_PRINT":"PRINTSCREEN","KEY_CAMERA":"CAMERA","KEY_SLEEP":"SLEEP","KEY_WAKEUP":"WAKE",
 "KEY_MAIL":"MAIL","KEY_BRIGHTNESSDOWN":"BRIGHTNESSDOWN","KEY_BRIGHTNESSUP":"BRIGHTNESSUP",
 "KEY_KBDILLUMTOGGLE":"KBDILLUMTOGGLE","KEY_KBDILLUMDOWN":"KBDILLUMDOWN","KEY_KBDILLUMUP":"KBDILLUMUP",
 "KEY_DISPLAY_OFF":"DISPLAYSWITCH","KEY_MODE":"MODE",
}

table = [0] * (KEY_MAX + 1)
unmatched = []
for name, val in ev.items():
    if not name.startswith("KEY_") or val > KEY_MAX:
        continue
    target = None
    if name in OVERRIDE:
        target = sc_by_norm.get(OVERRIDE[name])
        if target is None:
            unmatched.append((name, "override-miss:" + OVERRIDE[name])); continue
    else:
        target = sc_by_norm.get(norm(name))
        if target is None:
            # try direct name
            target = sc_by_norm.get(name[len("KEY_"):])
        if target is None:
            unmatched.append((name, None)); continue
    table[val] = target

print("KEY_MAX =", KEY_MAX, "| mapped:", sum(1 for t in table if t), "| unmatched:", len(unmatched))
for n, why in sorted(unmatched, key=lambda p: ev[p[0]]):
    print("  unmatched:", n, ev[n], why or "")

# spot checks
def chk(evname, expected_sc):
    assert table[ev[evname]] == sc["SDL_SCANCODE_" + expected_sc], (evname, table[ev[evname]], sc["SDL_SCANCODE_" + expected_sc])
chk("KEY_ESC","ESCAPE"); chk("KEY_A","A"); chk("KEY_ENTER","RETURN"); chk("KEY_LEFTCTRL","LCTRL")
chk("KEY_SPACE","SPACE"); chk("KEY_KPENTER","KP_ENTER"); chk("KEY_102ND","NONUSHASH"); chk("KEY_UP","UP")
chk("KEY_LEFTSHIFT","LSHIFT"); chk("KEY_RIGHTALT","RALT"); chk("KEY_F1","F1"); chk("KEY_Z","Z")
print("spot checks OK")

with open("/home/user/sdlop/src/internal/scancode_evdev.h", "w") as f:
    f.write("""/* Generated table: Linux evdev key code -> SDL_Scancode.
 * 0 = no mapping (SDL_SCANCODE_UNKNOWN).
 * Generated from /usr/include/linux/input-event-codes.h + SDL3 scancode
 * names; see tools/gen_scancode_evdev.py.
 */
#ifndef scancode_evdev_h_
#define scancode_evdev_h_

#include <SDL3/SDL_scancode.h>
#include <stdint.h>

#define SDLOP_EVDEV_KEY_MAX %d

static const uint16_t sdlop_evdev_to_scancode[SDLOP_EVDEV_KEY_MAX + 1] = {
""" % KEY_MAX)
    for i in range(0, KEY_MAX + 1, 16):
        f.write("    " + ", ".join(str(t) for t in table[i:i+16]) + ",\n")
    f.write("""};

#endif /* scancode_evdev_h_ */
""")
print("table written,", KEY_MAX + 1, "entries")
