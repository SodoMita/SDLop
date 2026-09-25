/*
  SDLop -- the XKB key layout.

  A scancode is a physical key position and never needs a layout. A *keycode*
  (SDLK_*) and the text a key produces do: they depend on the keyboard layout and
  on the modifier state. This module owns that answer for every backend that can
  ask the machine what layout is in effect:

    * Wayland: the compositor sends the keymap over wl_keyboard.keymap();
    * X11: the X server's core keyboard keymap is read through
      libxkbcommon-x11, so the same xkbcommon machinery answers for both.

  Both paths end up as the same `SDLOP_KeyLayout` registered with the keyboard
  layer, so a key press produces the same keycode and text whichever platform
  delivered it. With no layout available (offscreen, a headless compositor that
  never sends a keymap, a machine without xkbcommon installed) the keyboard layer
  falls back to the generated "us" tables - exactly what SDL3 does.

  The keysym -> SDL_Keycode mapping lives here too: it is how a *symbol* becomes
  an SDL keycode, independently of where the symbol came from.

  Sources of a keymap, in order of preference:

    1. SDLOP_XKB_KEYMAP=<file>: an explicit XKB keymap file, for debugging and for
       sessions that never hand one over;
    2. what the platform reports (compositor / X server);
    3. the local XKB configuration (XKB_DEFAULT_LAYOUT and friends), which is what
       a headless or KMS setup has to fall back to;
    4. no layout at all.
*/

#include "../sdlop_internal.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef SDLOP_HAVE_XKBCOMMON

#include <xkbcommon/xkbcommon.h>
#ifdef SDLOP_HAVE_XKBCOMMON_X11
#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon-x11.h>

/* XGetXCBConnection() hands over the XCB connection Xlib is already using, so the
   keymap arrives over the connection the application has, not a second one. */
#ifdef SDLOP_HAVE_X11_XCB
#include <X11/Xlib-xcb.h>
#else
void *XGetXCBConnection(void *display);
#endif
#endif

/* xkbcommon keycodes are evdev keycodes offset by 8, on every Linux platform. */
#define SDLOP_XKB_KEYCODE_OFFSET 8

static struct xkb_context *sdlop_xkb_context;
static struct xkb_keymap *sdlop_xkb_keymap;
static struct xkb_state *sdlop_xkb_state;

static void sdlop_xkb_update_key(Uint32 evdev_code, bool down);
static SDL_Keycode sdlop_xkb_keycode(Uint32 evdev_code);
static int sdlop_xkb_text(Uint32 evdev_code, char *buffer, size_t size);

static const SDLOP_KeyLayout sdlop_xkb_layout = {
    sdlop_xkb_update_key,
    sdlop_xkb_keycode,
    sdlop_xkb_text,
};

/* ------------------------------------------------------------------------- */
/* Keysym -> SDL_Keycode                                                     */
/* ------------------------------------------------------------------------- */

typedef struct SDLOP_KeysymMapping
{
    Uint32 keysym;
    SDL_Keycode keycode;
} SDLOP_KeysymMapping;

/* Only the non-ASCII keysyms need a table; ASCII keysyms are their own keycode,
   which is exactly how SDL3 stores them. */
static const SDLOP_KeysymMapping sdlop_keysym_map[] = {
    { 0xFF08, SDLK_BACKSPACE }, { 0xFF09, SDLK_TAB },        { 0xFF0D, SDLK_RETURN },
    { 0xFF1B, SDLK_ESCAPE },    { 0xFF50, SDLK_HOME },       { 0xFF51, SDLK_LEFT },
    { 0xFF52, SDLK_UP },        { 0xFF53, SDLK_RIGHT },      { 0xFF54, SDLK_DOWN },
    { 0xFF55, SDLK_PAGEUP },    { 0xFF56, SDLK_PAGEDOWN },   { 0xFF57, SDLK_END },
    { 0xFF61, SDLK_PRINTSCREEN }, { 0xFF63, SDLK_INSERT },   { 0xFF67, SDLK_MENU },
    { 0xFF6A, SDLK_HELP },      { 0xFF7F, SDLK_NUMLOCKCLEAR }, { 0xFF8D, SDLK_KP_ENTER },
    { 0xFFAA, SDLK_KP_MULTIPLY }, { 0xFFAB, SDLK_KP_PLUS },  { 0xFFAD, SDLK_KP_MINUS },
    { 0xFFAE, SDLK_KP_PERIOD }, { 0xFFAF, SDLK_KP_DIVIDE },  { 0xFFB0, SDLK_KP_0 },
    { 0xFFB1, SDLK_KP_1 },      { 0xFFB2, SDLK_KP_2 },       { 0xFFB3, SDLK_KP_3 },
    { 0xFFB4, SDLK_KP_4 },      { 0xFFB5, SDLK_KP_5 },       { 0xFFB6, SDLK_KP_6 },
    { 0xFFB7, SDLK_KP_7 },      { 0xFFB8, SDLK_KP_8 },       { 0xFFB9, SDLK_KP_9 },
    { 0xFFBD, SDLK_KP_EQUALS }, { 0xFFE1, SDLK_LSHIFT },     { 0xFFE2, SDLK_RSHIFT },
    { 0xFFE3, SDLK_LCTRL },     { 0xFFE4, SDLK_RCTRL },      { 0xFFE5, SDLK_CAPSLOCK },
    { 0xFFE7, SDLK_LGUI },      { 0xFFE8, SDLK_RGUI },       { 0xFFE9, SDLK_LALT },
    { 0xFFEA, SDLK_RALT },      { 0xFFEB, SDLK_LGUI },       { 0xFFEC, SDLK_RGUI },
    { 0xFF13, SDLK_PAUSE },     { 0xFF14, SDLK_SCROLLLOCK },
};

#define SDLOP_F1_KEYCODE 0xFFBE

SDL_Keycode SDLOP_KeycodeFromKeysym(Uint32 keysym)
{
    size_t i;

    if (keysym == 0) {
        return SDLK_UNKNOWN;
    }
    if (keysym <= 0x7F) {
        /* ASCII maps to itself, case and all: which level of the layout the
           keysym came from is decided before it gets here (SDL keycodes are the
           *unshifted* ones - see sdlop_xkb_keycode()). */
        return (SDL_Keycode)keysym;
    }
    if (keysym < 0x100) {
        return (SDL_Keycode)keysym;                /* Latin-1 control range */
    }
    /* "Unicode keysyms" carry their code point in the low 21 bits (0x11000000 is
       the vendor spelling of the same idea). */
    if (keysym >= 0x1000000) {
        return (SDL_Keycode)(keysym & 0x1FFFFF);
    }
    if (keysym >= SDLOP_F1_KEYCODE && keysym < SDLOP_F1_KEYCODE + 35) {
        return (SDL_Keycode)(SDLK_F1 + (keysym - SDLOP_F1_KEYCODE));
    }
    /* The table has to be consulted before the arithmetic ranges below: the named
       keys (arrows, keypad, modifiers) live at 0xE000 and up, and the modifiers
       must come out as SDLK_LSHIFT & co. - values that carry SDLK_SCANCODE_MASK
       so that nothing treats them as text. */
    for (i = 0; i < sizeof(sdlop_keysym_map) / sizeof(sdlop_keysym_map[0]); i++) {
        if (sdlop_keysym_map[i].keysym == keysym) {
            return sdlop_keysym_map[i].keycode;
        }
    }
    /* The old Latin-2..4 keysym block: the character is 0x100 below the keysym. */
    if (keysym >= 0x100 && keysym < 0xE000) {
        return (SDL_Keycode)(keysym - 0x100);
    }
    return SDLK_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
/* Installing a keymap                                                       */
/* ------------------------------------------------------------------------- */

struct xkb_keymap *SDLOP_XKBKeymap(void)
{
    return sdlop_xkb_keymap;
}

struct xkb_state *SDLOP_XKBState(void)
{
    return sdlop_xkb_state;
}

bool SDLOP_XKBKeyRepeats(Uint32 evdev_code)
{
    if (!sdlop_xkb_keymap) {
        return false;
    }
    return xkb_keymap_key_repeats(sdlop_xkb_keymap, evdev_code + SDLOP_XKB_KEYCODE_OFFSET);
}

static bool sdlop_xkb_context_ensure(void)
{
    if (!sdlop_xkb_context) {
        sdlop_xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    }
    return sdlop_xkb_context != NULL;
}

/* Take ownership of a freshly compiled keymap and start answering keycode and
   text questions from it. */
static bool sdlop_xkb_install(struct xkb_keymap *keymap)
{
    struct xkb_state *state;

    if (!keymap) {
        return false;
    }
    state = xkb_state_new(keymap);
    if (!state) {
        xkb_keymap_unref(keymap);
        return false;
    }
    if (sdlop_xkb_state) {
        xkb_state_unref(sdlop_xkb_state);
    }
    if (sdlop_xkb_keymap) {
        xkb_keymap_unref(sdlop_xkb_keymap);
    }
    sdlop_xkb_keymap = keymap;
    sdlop_xkb_state = state;
    /* From here on, keycodes and text come from this layout instead of the
       built-in "us" tables. */
    SDLOP_SetKeyLayout(&sdlop_xkb_layout);
    SDLOP_SendKeymapChanged(SDL_GetTicksNS());
    return true;
}

bool SDLOP_XKBCompileFromString(const char *text, size_t size)
{
    struct xkb_keymap *keymap;
    char *copy;

    if (!text || !size || !sdlop_xkb_context_ensure()) {
        return false;
    }
    /* xkb_keymap_new_from_string() wants a NUL-terminated string and a keymap fd
       is not one, so the text is copied. */
    copy = (char *)SDLOP_Alloc(size + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, text, size);
    copy[size] = '\0';
    keymap = xkb_keymap_new_from_string(sdlop_xkb_context, copy,
                                        XKB_KEYMAP_FORMAT_TEXT_V1,
                                        XKB_KEYMAP_COMPILE_NO_FLAGS);
    SDLOP_Free(copy);
    return sdlop_xkb_install(keymap);
}

bool SDLOP_XKBCompileFromRules(const char *rules, const char *model, const char *layout,
                               const char *variant, const char *options)
{
    struct xkb_rule_names names;

    if (!sdlop_xkb_context_ensure()) {
        return false;
    }
    /* NULL fields mean "let xkbcommon decide": it applies the XKB_DEFAULT_*
       environment first and then its own evdev/pc105/us defaults. */
    names.rules = rules;
    names.model = model;
    names.layout = layout;
    names.variant = variant;
    names.options = options;
    return sdlop_xkb_install(xkb_keymap_new_from_names(sdlop_xkb_context, &names,
                                                       XKB_KEYMAP_COMPILE_NO_FLAGS));
}

/* Where the keymap being installed came from, for debug logging. */
static const char *sdlop_xkb_dump_path;

void SDLOP_XKBSetDumpPath(const char *path)
{
    sdlop_xkb_dump_path = (path && path[0]) ? path : NULL;
}

static void sdlop_xkb_dump(const char *text, size_t size)
{
    FILE *out;

    if (!sdlop_xkb_dump_path) {
        return;
    }
    out = fopen(sdlop_xkb_dump_path, "wb");
    if (!out) {
        SDLOP_LogWarn("sdlop: cannot write the keymap to %s", sdlop_xkb_dump_path);
        return;
    }
    fwrite(text, 1, size, out);
    fclose(out);
    SDLOP_LogInfo("sdlop: wrote the keymap to %s", sdlop_xkb_dump_path);
}

bool SDLOP_XKBLoadKeymapFD(int fd)
{
    char *map;
    size_t size;
    bool ok;

    if (fd < 0) {
        return false;
    }
    size = (size_t)lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    map = (char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return false;
    }
    sdlop_xkb_dump(map, size);
    ok = SDLOP_XKBCompileFromString(map, size);
    munmap(map, size);
    close(fd);
    return ok;
}

#ifdef SDLOP_HAVE_XKBCOMMON_X11
bool SDLOP_XKBLoadFromX11(void *xdisplay, int device_id)
{
    struct xkb_keymap *keymap;
    xcb_connection_t *connection;

    if (!xdisplay || !sdlop_xkb_context_ensure()) {
        return false;
    }
    connection = (xcb_connection_t *)XGetXCBConnection(xdisplay);
    if (!connection) {
        return false;
    }
    /* xkbcommon-x11 talks XCB and wants the XKB extension set up on that
       connection first; both calls are cheap and idempotent. */
    if (!xkb_x11_setup_xkb_extension(connection, 1, 0, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                                     NULL, NULL, NULL, NULL)) {
        return SDL_SetError("Couldn't set up the XKB extension on the X connection");
    }
    /* The server's core keyboard has a keymap like any other XKB device; asking
       for it through libxkbcommon-x11 gives exactly the layout the X server is
       using: characters, dead keys and all. */
    keymap = xkb_x11_keymap_new_from_device(sdlop_xkb_context, connection, device_id,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        return false;
    }
    if (sdlop_xkb_dump_path) {
        char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
        if (text) {
            sdlop_xkb_dump(text, strlen(text));
            free(text);
        }
    }
    /* The state is *not* device-synced (xkb_x11_state_new_from_device would track
       the server's own modifier state): this layout has to follow the keys the
       input path actually delivers, which may come from the evdev worker rather
       than from the X event stream. */
    return sdlop_xkb_install(keymap);
}

int SDLOP_XKBX11DeviceID(void *xdisplay)
{
    xcb_connection_t *connection;

    if (!xdisplay) {
        return -1;
    }
    connection = (xcb_connection_t *)XGetXCBConnection(xdisplay);
    if (!connection) {
        return -1;
    }
    /* The core keyboard can change (a hotplugged one can take over), so this is
       asked for again on every keymap change. */
    return xkb_x11_get_core_keyboard_device_id(connection);
}
#endif /* SDLOP_HAVE_XKBCOMMON_X11 */

/* Keys can reach us from the evdev worker rather than from the platform's own
   event stream, so the layout state has to follow the keys we actually see.
   Without this, holding shift would have no effect on keycodes or text on the
   async path. */
static void sdlop_xkb_update_key(Uint32 evdev_code, bool down)
{
    if (!sdlop_xkb_state) {
        return;
    }
    xkb_state_update_key(sdlop_xkb_state, evdev_code + SDLOP_XKB_KEYCODE_OFFSET,
                         down ? XKB_KEY_DOWN : XKB_KEY_UP);
}

/* SDL keycodes are the *unshifted* characters of the layout: with shift held,
   pressing A is SDLK_a plus SDL_KMOD_SHIFT in the event, and the "A" arrives as
   text. That is SDL3's rule (its author's words: "keycodes are the unshifted
   characters you get when the keys are pressed"), so this asks xkbcommon for
   level 0 of the current group instead of the state's shifted keysym.

   The two options SDL3 applies by default (SDL_HINT_KEYCODE_OPTIONS defaults to
   "french_numbers,latin_letters") are applied here too, so a French, Russian or
   Thai desktop reports the keycodes an application gets from stock SDL3. */
static const struct
{
    Uint32 from;
    SDL_Keycode to;
} sdlop_french_numbers[] = {
    /* The AZERTY number row is inverted: level 0 types the symbol, so SDL reports
       the digit that is one level up. */
    { '&', SDLK_1 }, { 0xE9, SDLK_2 }, { '"', SDLK_3 }, { '\'', SDLK_4 }, { '(', SDLK_5 },
    { '-', SDLK_6 }, { 0xE8, SDLK_7 }, { '_', SDLK_8 }, { 0xE7, SDLK_9 }, { 0xE0, SDLK_0 },
};

static SDL_Keycode sdlop_xkb_keycode(Uint32 evdev_code)
{
    const xkb_keysym_t *syms = NULL;
    xkb_keysym_t keysym = XKB_KEY_NoSymbol;
    xkb_keycode_t key;
    SDL_Keycode keycode;
    size_t i;

    if (!sdlop_xkb_state || !sdlop_xkb_keymap) {
        return SDLK_UNKNOWN;
    }
    key = evdev_code + SDLOP_XKB_KEYCODE_OFFSET;
    if (xkb_keymap_key_get_syms_by_level(sdlop_xkb_keymap, key,
                                         xkb_state_key_get_layout(sdlop_xkb_state, key), 0,
                                         &syms) > 0) {
        keysym = syms[0];
    }
    for (i = 0; i < SDL_arraysize(sdlop_french_numbers); i++) {
        if (keysym == sdlop_french_numbers[i].from) {
            keysym = (xkb_keysym_t)sdlop_french_numbers[i].to;
            break;
        }
    }
    keycode = SDLOP_KeycodeFromKeysym(keysym);

    /* latin_letters: a keyboard whose letter keys are not Latin reports the
       letter of the US layout for them, so W/A/S/D stay usable on a Russian or
       Thai desktop. Only letters are affected. */
    if (keycode >= 0x80) {
        SDL_Scancode scancode = SDLOP_ScancodeFromEvdevKeycode(evdev_code);

        if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
            keycode = (SDL_Keycode)(0x61 + (scancode - SDL_SCANCODE_A));   /* SDLK_a */
        }
    }
    return keycode;
}

static int sdlop_xkb_text(Uint32 evdev_code, char *buffer, size_t size)
{
    int len;

    if (!sdlop_xkb_state || !buffer || size < 2) {
        return 0;
    }
    /* xkb_state_key_get_utf8() applies the current modifier state (and handles
       dead keys, which resolve to nothing until composed), which is exactly the
       text the key types. It returns 0 when the key produces nothing. */
    len = xkb_state_key_get_utf8(sdlop_xkb_state, evdev_code + SDLOP_XKB_KEYCODE_OFFSET,
                                 buffer, size);
    if (len <= 0 || (size_t)len >= size) {
        buffer[0] = '\0';
        return 0;
    }
    return len;
}

/* ------------------------------------------------------------------------- */
/* Lifetime                                                                  */
/* ------------------------------------------------------------------------- */

static bool sdlop_xkb_override_active(void)
{
    const char *path = SDL_getenv("SDLOP_XKB_KEYMAP");
    return path && path[0];
}

static void sdlop_xkb_load_override(void)
{
    const char *path = SDL_getenv("SDLOP_XKB_KEYMAP");
    FILE *file;
    char *buffer;
    long size;

    if (!path || !path[0]) {
        return;
    }
    file = fopen(path, "rb");
    if (!file) {
        SDLOP_LogWarn("sdlop: cannot read the keymap '%s'", path);
        return;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    buffer = (size > 0) ? (char *)SDLOP_Alloc((size_t)size) : NULL;
    if (buffer && fread(buffer, 1, (size_t)size, file) == (size_t)size) {
        if (SDLOP_XKBCompileFromString(buffer, (size_t)size)) {
            SDLOP_LogInfo("sdlop: using the keymap from %s", path);
        }
    } else {
        SDLOP_LogWarn("sdlop: could not read the keymap '%s'", path);
    }
    SDLOP_Free(buffer);
    fclose(file);
}

bool SDLOP_XKBOverrideActive(void)
{
    return sdlop_xkb_override_active();
}

void SDLOP_XKBInit(void)
{
    SDLOP_XKBSetDumpPath(SDL_getenv("SDLOP_XKB_DUMP"));
    if (sdlop_xkb_override_active()) {
        sdlop_xkb_load_override();
        return;
    }
    /* The local XKB configuration (XKB_DEFAULT_LAYOUT and friends) is a far
       better guess than the built-in us tables for a session that never tells us
       about its keyboard (a headless compositor, a test harness). */
    SDLOP_XKBCompileFromRules(NULL, NULL, NULL, NULL, NULL);
}

void SDLOP_XKBQuit(void)
{
    if (sdlop_xkb_state) {
        xkb_state_unref(sdlop_xkb_state);
        sdlop_xkb_state = NULL;
    }
    if (sdlop_xkb_keymap) {
        xkb_keymap_unref(sdlop_xkb_keymap);
        sdlop_xkb_keymap = NULL;
    }
    if (sdlop_xkb_context) {
        xkb_context_unref(sdlop_xkb_context);
        sdlop_xkb_context = NULL;
    }
}

#endif /* SDLOP_HAVE_XKBCOMMON */
