/*
  SDLop test: behaviour that applications built against stock SDL3 rely on
  outside of windowing proper (found by running real apps on SDLop as a
  libSDL3.so.0 drop-in, see README "Drop-in use"):

    - SDL_CreateWindow() without SDL_Init() initializes video + events
      (IOCCC 2025 "cable"); SDL_WasInit goes 0 -> VIDEO|EVENTS
    - init flag dependencies: GAMEPAD -> JOYSTICK -> EVENTS
    - joystick / gamepad API with no devices (Dear ImGui's SDL3 backend):
      empty lists, failing opens, harmless NULL handles, string helpers
    - SDL_GetPrefPath / SDL_GetBasePath / SDL_GetUserFolder and the
      filesystem helpers of SDL_filesystem.h (cottonpaper sdl3dviewer)

  Runs on the dummy video driver: no display server needed.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                          \
    do {                                                          \
        if (!(cond)) {                                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);  \
            fprintf(stderr, __VA_ARGS__);                         \
            fprintf(stderr, " [SDL: %s]\n", SDL_GetError());      \
            failures++;                                           \
        }                                                         \
    } while (0)

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void test_implicit_init(void)
{
    CHECK(SDL_WasInit(0) == 0, "nothing initialized at start (0x%x)", (unsigned)SDL_WasInit(0));
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_Window *w = SDL_CreateWindow("implicit", 64, 64, 0);
    CHECK(w != NULL, "SDL_CreateWindow without SDL_Init returns a window");
    CHECK(SDL_WasInit(0) == (SDL_INIT_VIDEO | SDL_INIT_EVENTS), "flags after implicit init: 0x%x (want 0x%x)",
          (unsigned)SDL_WasInit(0), (unsigned)(SDL_INIT_VIDEO | SDL_INIT_EVENTS));
    CHECK(SDL_GetCurrentVideoDriver() && strcmp(SDL_GetCurrentVideoDriver(), "dummy") == 0, "driver is dummy");
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
    }
    if (w) {
        SDL_DestroyWindow(w);
    }
    /* a failing implicit init must not crash and must return NULL */
    SDL_Quit();
    CHECK(SDL_WasInit(0) == 0, "SDL_Quit after implicit init clears flags");
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "no-such-driver");
    w = SDL_CreateWindow("bad driver", 64, 64, 0);
    CHECK(w == NULL, "implicit init with an unknown driver fails cleanly");
    CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0, "video not marked initialized after failure");
    SDL_Quit();
}

static void test_init_dependencies(void)
{
    CHECK(SDL_Init(SDL_INIT_GAMEPAD), "SDL_Init(GAMEPAD)");
    SDL_InitFlags f = SDL_WasInit(0);
    CHECK((f & (SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS)) ==
              (SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS),
          "GAMEPAD implies JOYSTICK and EVENTS (0x%x)", (unsigned)f);
    SDL_Quit();
}

static void test_gamepad(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), "SDL_Init(VIDEO|GAMEPAD)");

    int count = -1;
    SDL_JoystickID *pads = SDL_GetGamepads(&count);
    CHECK(pads != NULL && count == 0 && pads[0] == 0, "SDL_GetGamepads: empty, 0-terminated (count %d)", count);
    SDL_free(pads);
    pads = SDL_GetGamepads(NULL);
    CHECK(pads != NULL, "NULL count accepted");
    SDL_free(pads);
    count = -1;
    SDL_JoystickID *sticks = SDL_GetJoysticks(&count);
    CHECK(sticks != NULL && count == 0, "SDL_GetJoysticks: empty (count %d)", count);
    SDL_free(sticks);
    CHECK(!SDL_HasGamepad() && !SDL_HasJoystick(), "no devices");

    SDL_ClearError();
    SDL_Gamepad *pad = SDL_OpenGamepad(1);
    CHECK(pad == NULL && SDL_GetError()[0] != '\0', "SDL_OpenGamepad fails with an error");
    CHECK(SDL_OpenJoystick(1) == NULL, "SDL_OpenJoystick fails");
    /* what ImGui does with the (NULL) result: must not crash */
    CHECK(SDL_GetGamepadAxis(NULL, SDL_GAMEPAD_AXIS_LEFTX) == 0, "axis of NULL pad");
    CHECK(!SDL_GetGamepadButton(NULL, SDL_GAMEPAD_BUTTON_SOUTH), "button of NULL pad");
    SDL_CloseGamepad(NULL);
    SDL_CloseJoystick(NULL);
    CHECK(SDL_GetGamepadName(NULL) == NULL, "name of NULL pad");
    CHECK(SDL_GetGamepadType(NULL) == SDL_GAMEPAD_TYPE_UNKNOWN, "type of NULL pad");
    CHECK(!SDL_RumbleGamepad(NULL, 1, 1, 10), "rumble NULL pad fails");
    SDL_UpdateGamepads();
    SDL_UpdateJoysticks();

    /* events enabled flags round-trip */
    SDL_SetGamepadEventsEnabled(false);
    CHECK(!SDL_GamepadEventsEnabled(), "gamepad events disabled");
    SDL_SetGamepadEventsEnabled(true);
    CHECK(SDL_GamepadEventsEnabled(), "gamepad events enabled");

    /* pure helpers behave like stock */
    CHECK(SDL_GetGamepadButtonFromString("leftshoulder") == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "button from string");
    CHECK(strcmp(SDL_GetGamepadStringForButton(SDL_GAMEPAD_BUTTON_DPAD_UP), "dpup") == 0, "string for button");
    CHECK(SDL_GetGamepadAxisFromString("+righttrigger") == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, "axis from string");
    CHECK(strcmp(SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_LEFTY), "lefty") == 0, "string for axis");
    CHECK(SDL_GetGamepadTypeFromString("ps5") == SDL_GAMEPAD_TYPE_PS5, "type from string");
    CHECK(strcmp(SDL_GetGamepadStringForType(SDL_GAMEPAD_TYPE_XBOXONE), "xboxone") == 0, "string for type");
    CHECK(SDL_GetGamepadStringForType(SDL_GAMEPAD_TYPE_UNKNOWN) == NULL, "no string for unknown type");
    CHECK(SDL_GetGamepadButtonLabelForType(SDL_GAMEPAD_TYPE_PS4, SDL_GAMEPAD_BUTTON_SOUTH) ==
              SDL_GAMEPAD_BUTTON_LABEL_CROSS, "PS label");
    CHECK(SDL_GetGamepadButtonLabelForType(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, SDL_GAMEPAD_BUTTON_SOUTH) ==
              SDL_GAMEPAD_BUTTON_LABEL_B, "Nintendo label");
    CHECK(SDL_GetGamepadButtonLabelForType(SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_BUTTON_NORTH) ==
              SDL_GAMEPAD_BUTTON_LABEL_Y, "Xbox label");

    /* GUID info: standard layout bus 0x03, vendor 0x045e, product 0x028e */
    SDL_GUID g;
    memset(&g, 0, sizeof(g));
    g.data[0] = 0x03;
    g.data[2] = 0x34; g.data[3] = 0x12; /* crc */
    g.data[4] = 0x5e; g.data[5] = 0x04;
    g.data[8] = 0x8e; g.data[9] = 0x02;
    g.data[12] = 0x14; g.data[13] = 0x01;
    Uint16 vendor = 0, product = 0, version = 0, crc = 0;
    SDL_GetJoystickGUIDInfo(g, &vendor, &product, &version, &crc);
    CHECK(vendor == 0x045e && product == 0x028e && version == 0x0114 && crc == 0x1234,
          "GUID info %04x %04x %04x %04x", vendor, product, version, crc);

    /* mappings are accepted (loading gamecontrollerdb.txt must not fail) */
    CHECK(SDL_AddGamepadMapping("030000005e0400008e02000014010000,X360 Controller,a:b0,b:b1,") == 1,
          "AddGamepadMapping");
    CHECK(SDL_AddGamepadMapping(NULL) == -1, "AddGamepadMapping(NULL)");
    SDL_Quit();
}

typedef struct
{
    int seen;
    bool slash_ok;
} EnumState;

static SDL_EnumerationResult SDLCALL enum_cb(void *userdata, const char *dirname, const char *fname)
{
    EnumState *st = (EnumState *)userdata;
    size_t n = strlen(dirname);
    st->slash_ok = n > 0 && dirname[n - 1] == '/' && (n < 2 || dirname[n - 2] != '/');
    if (strcmp(fname, "a.txt") == 0 || strcmp(fname, "sub") == 0) {
        st->seen++;
    }
    return SDL_ENUM_CONTINUE;
}

static void test_filesystem(void)
{
    char tmpl[] = "/tmp/sdlop_fs_XXXXXX";
    char *root = mkdtemp(tmpl);
    CHECK(root != NULL, "mkdtemp");
    if (!root) {
        return;
    }
    char path[512], path2[512];

    /* pref path: XDG_DATA_HOME/org/app/, created, trailing slash */
    SDL_setenv_unsafe("XDG_DATA_HOME", root, 1);
    char *pref = SDL_GetPrefPath("My Org", "My App");
    snprintf(path, sizeof(path), "%s/My Org/My App/", root);
    CHECK(pref && strcmp(pref, path) == 0, "pref path '%s' (want '%s')", pref ? pref : "(null)", path);
    CHECK(pref && is_dir(pref), "pref path exists");
    SDL_free(pref);
    pref = SDL_GetPrefPath(NULL, "Solo");
    snprintf(path, sizeof(path), "%s/Solo/", root);
    CHECK(pref && strcmp(pref, path) == 0, "pref path without org '%s'", pref ? pref : "(null)");
    SDL_free(pref);
    CHECK(SDL_GetPrefPath("x", NULL) == NULL, "NULL app rejected");
    /* fallback: $HOME/.local/share */
    SDL_unsetenv_unsafe("XDG_DATA_HOME");
    snprintf(path, sizeof(path), "%s/home", root);
    mkdir(path, 0700);
    SDL_setenv_unsafe("HOME", path, 1);
    pref = SDL_GetPrefPath("o", "a");
    snprintf(path2, sizeof(path2), "%s/home/.local/share/o/a/", root);
    CHECK(pref && strcmp(pref, path2) == 0 && is_dir(path2), "HOME fallback '%s'", pref ? pref : "(null)");
    SDL_free(pref);

    /* user folders */
    const char *home = SDL_GetUserFolder(SDL_FOLDER_HOME);
    snprintf(path2, sizeof(path2), "%s/home/", root);
    CHECK(home && strcmp(home, path2) == 0, "home folder '%s'", home ? home : "(null)");
    snprintf(path, sizeof(path), "%s/home/.config", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/home/.config/user-dirs.dirs", root);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs("# test\nXDG_DOCUMENTS_DIR=\"$HOME/Docs\"\n", f);
        fclose(f);
    }
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    snprintf(path2, sizeof(path2), "%s/home/Docs/", root);
    CHECK(docs && strcmp(docs, path2) == 0, "documents folder '%s'", docs ? docs : "(null)");
    CHECK(SDL_GetUserFolder(SDL_FOLDER_SAVEDGAMES) == NULL, "no saved-games folder on XDG");

    /* base path: directory of this executable, with trailing slash */
    const char *base = SDL_GetBasePath();
    CHECK(base && base[0] == '/' && base[strlen(base) - 1] == '/', "base path '%s'", base ? base : "(null)");
    if (base) {
        snprintf(path, sizeof(path), "%stest_dropin_compat", base);
        CHECK(access(path, X_OK) == 0, "executable found in base path (%s)", path);
    }

    /* directory operations */
    snprintf(path, sizeof(path), "%s/d1/d2/d3", root);
    CHECK(SDL_CreateDirectory(path) && is_dir(path), "CreateDirectory with parents");
    CHECK(SDL_CreateDirectory(path), "CreateDirectory on an existing dir succeeds");
    snprintf(path, sizeof(path), "%s/d1/a.txt", root);
    f = fopen(path, "w");
    if (f) {
        fputs("hello", f);
        fclose(f);
    }
    SDL_PathInfo info;
    CHECK(SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE && info.size == 5 && info.modify_time > 0,
          "GetPathInfo file");
    snprintf(path2, sizeof(path2), "%s/d1", root);
    CHECK(SDL_GetPathInfo(path2, &info) && info.type == SDL_PATHTYPE_DIRECTORY, "GetPathInfo dir");
    CHECK(!SDL_GetPathInfo("/nonexistent/xyz", &info) && info.type == SDL_PATHTYPE_NONE, "GetPathInfo missing");

    EnumState st = { 0, false };
    snprintf(path2, sizeof(path2), "%s/d1//", root);
    CHECK(SDL_EnumerateDirectory(path2, enum_cb, &st) && st.seen == 1 && st.slash_ok,
          "enumerate 'd1//' sees a.txt, dirname ends in one '/' (seen %d)", st.seen);
    snprintf(path2, sizeof(path2), "%s/d1/d2/sub", root);
    SDL_CreateDirectory(path2);
    snprintf(path2, sizeof(path2), "%s/d1/d2/a.txt", root);
    CHECK(SDL_CopyFile(path, path2), "CopyFile");
    CHECK(SDL_GetPathInfo(path2, &info) && info.size == 5, "copied size");
    st.seen = 0;
    snprintf(path2, sizeof(path2), "%s/d1/d2", root);
    CHECK(SDL_EnumerateDirectory(path2, enum_cb, &st) && st.seen == 2 && st.slash_ok,
          "enumerate sees a.txt and sub with 'dir/' names (seen %d)", st.seen);

    snprintf(path2, sizeof(path2), "%s/d1/b.txt", root);
    CHECK(SDL_RenamePath(path, path2) && access(path, F_OK) != 0 && access(path2, F_OK) == 0, "RenamePath");
    CHECK(SDL_RemovePath(path2) && access(path2, F_OK) != 0, "RemovePath file");
    CHECK(SDL_RemovePath(path2), "RemovePath of a missing path succeeds");

    char *cwd = SDL_GetCurrentDirectory();
    CHECK(cwd && cwd[0] == '/' && cwd[strlen(cwd) - 1] == '/', "current directory '%s'", cwd ? cwd : "(null)");
    SDL_free(cwd);

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        fprintf(stderr, "warning: could not remove %s\n", root);
    }
    SDL_Quit();
}

int main(void)
{
    test_implicit_init();
    test_init_dependencies();
    test_gamepad();
    test_filesystem();
    if (failures) {
        printf("test_dropin_compat: FAIL (%d)\n", failures);
        return 1;
    }
    printf("test_dropin_compat: PASS\n");
    return 0;
}
