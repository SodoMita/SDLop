/*
  test_misc - assertions, GUID strings, message boxes (fake zenity),
  SDL_OpenURL (fake xdg-open), power info (fake sysfs), platform stubs.
*/
#define SDL_ASSERT_LEVEL 2
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            ++failures;                                               \
        }                                                             \
    } while (0)

static char tmpdir[256];

static void write_file(const char *dir, const char *name, const char *content, bool exec)
{
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(1);
    }
    fputs(content, f);
    fclose(f);
    if (exec) {
        chmod(path, 0755);
    }
}

static char *read_file(const char *path)
{
    static char buf[1024];
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static void test_guid(void)
{
    SDL_GUID g;
    for (int i = 0; i < 16; ++i) {
        g.data[i] = (Uint8)(i * 17 + 3);
    }
    char s[33];
    SDL_GUIDToString(g, s, sizeof(s));
    CHECK(strcmp(s, "031425364758697a8b9cadbecfe0f102") == 0, "guid string %s", s);
    SDL_GUID back = SDL_StringToGUID(s);
    CHECK(memcmp(&g, &back, sizeof(g)) == 0, "guid round trip");
    SDL_GUID upper = SDL_StringToGUID("031425364758697A8B9CADBECFE0F102");
    CHECK(memcmp(&g, &upper, sizeof(g)) == 0, "upper-case hex");
    char small[6];
    SDL_GUIDToString(g, small, sizeof(small));
    CHECK(strcmp(small, "0314") == 0, "truncated to whole bytes: %s", small);
    SDL_GUID odd = SDL_StringToGUID("abc");
    CHECK(odd.data[0] == 0xab && odd.data[1] == 0, "odd length ignores last nibble");
}

static int handler_calls = 0;
static SDL_AssertState SDLCALL my_handler(const SDL_AssertData *data, void *userdata)
{
    ++handler_calls;
    CHECK(userdata == (void *)&handler_calls, "handler userdata");
    CHECK(data && strstr(data->condition, "always_false"), "condition text %s", data ? data->condition : "?");
    return handler_calls == 1 ? SDL_ASSERTION_RETRY : SDL_ASSERTION_ALWAYS_IGNORE;
}

static void test_assert(void)
{
    void *ud = NULL;
    CHECK(SDL_GetAssertionHandler(&ud) == SDL_GetDefaultAssertionHandler() && ud == NULL, "default handler");
    SDL_SetAssertionHandler(my_handler, &handler_calls);
    volatile int always_false = 0;
    for (int i = 0; i < 3; ++i) {
        SDL_assert(always_false); /* retry once, then always ignore */
    }
    CHECK(handler_calls == 2, "handler called %d times", handler_calls);
    const SDL_AssertData *rep = SDL_GetAssertionReport();
    CHECK(rep && rep->trigger_count == 4 && rep->always_ignore, "report: count %u ignore %d",
          rep ? rep->trigger_count : 0, rep ? rep->always_ignore : 0);
    CHECK(rep && rep->linenum > 0 && strstr(rep->filename, "test_misc"), "report location");
    SDL_ResetAssertionReport();
    CHECK(SDL_GetAssertionReport() == NULL, "report reset");
    SDL_SetAssertionHandler(NULL, NULL);
    CHECK(SDL_GetAssertionHandler(NULL) == SDL_GetDefaultAssertionHandler(), "handler reset");

    /* default handler with the SDL_ASSERT hint: no prompt */
    SDL_SetHint(SDL_HINT_ASSERT, "ignore");
    SDL_assert(always_false);
    rep = SDL_GetAssertionReport();
    CHECK(rep && rep->trigger_count == 1 && !rep->always_ignore, "hint ignore");
    SDL_ResetAssertionReport();
    SDL_ResetHint(SDL_HINT_ASSERT);
}

static void test_messagebox(const char *orig_path)
{
    char path[1024];
    /* no zenity anywhere */
    SDL_setenv_unsafe("PATH", tmpdir, 1);
    CHECK(!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "t", "m", NULL), "no zenity: fails");
    CHECK(strstr(SDL_GetError(), "zenity"), "error names zenity: %s", SDL_GetError());

    /* fake zenity: records its arguments, "clicks" the second button */
    char script[1024];
    snprintf(script, sizeof(script),
             "#!/bin/sh\n"
             "if [ \"$1\" = --version ]; then echo 4.0.1; exit 0; fi\n"
             "printf '%%s\\n' \"$@\" > %s/zenity.args\n"
             "echo Second\n"
             "exit 1\n",
             tmpdir);
    write_file(tmpdir, "zenity", script, true);
    snprintf(path, sizeof(path), "%s:%s", tmpdir, orig_path);
    SDL_setenv_unsafe("PATH", path, 1);

    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 10, "First" },
        { 0, 20, "Second" },
    };
    SDL_MessageBoxData mb;
    SDL_zero(mb);
    mb.flags = SDL_MESSAGEBOX_WARNING;
    mb.title = "Title here";
    mb.message = "Message body";
    mb.numbuttons = 2;
    mb.buttons = buttons;
    int id = -5;
    CHECK(SDL_ShowMessageBox(&mb, &id), "message box: %s", SDL_GetError());
    CHECK(id == 20, "button id %d", id);
    snprintf(path, sizeof(path), "%s/zenity.args", tmpdir);
    const char *args = read_file(path);
    CHECK(args && strstr(args, "--question\n") && strstr(args, "--icon\ndialog-warning\n") &&
              strstr(args, "--title\nTitle here\n") && strstr(args, "--text\nMessage body\n") &&
              strstr(args, "--extra-button\nFirst\n--extra-button\nSecond\n"),
          "zenity args:\n%s", args ? args : "(none)");

    /* title from SDL_GetError() storage is copied before use */
    SDL_SetError("error-title");
    CHECK(SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, SDL_GetError(), "x", NULL), "simple box");
    args = read_file(path);
    CHECK(args && strstr(args, "--title\nerror-title\n") && strstr(args, "--extra-button\nOK\n"),
          "simple box args:\n%s", args ? args : "(none)");

    mb.numbuttons = 9;
    CHECK(!SDL_ShowMessageBox(&mb, &id), "too many buttons rejected");
    CHECK(!SDL_ShowMessageBox(NULL, &id), "NULL data rejected");
    SDL_setenv_unsafe("PATH", orig_path, 1);
}

static void test_openurl(const char *orig_path)
{
    char path[1024], script[2048], out[300];
    snprintf(out, sizeof(out), "%s/url.out", tmpdir);
    snprintf(script, sizeof(script),
             "#!/bin/sh\n"
             "echo \"$1 preload=${LD_PRELOAD:-none}\" > %s.tmp && mv %s.tmp %s\n",
             out, out, out);
    write_file(tmpdir, "xdg-open", script, true);
    snprintf(path, sizeof(path), "%s:%s", tmpdir, orig_path);
    SDL_setenv_unsafe("PATH", path, 1);
    const char *preload = getenv("LD_PRELOAD");
    char *saved_preload = preload ? strdup(preload) : NULL;
    if (!preload) {
        SDL_setenv_unsafe("LD_PRELOAD", "", 1); /* an empty value is harmless for the child */
    }
    CHECK(SDL_OpenURL("https://example.org/a?b=c"), "open url: %s", SDL_GetError());
    const char *got = NULL;
    for (int i = 0; i < 200 && !(got = read_file(out)); ++i) {
        SDL_Delay(10);
    }
    CHECK(got && strcmp(got, "https://example.org/a?b=c preload=none\n") == 0, "xdg-open got: %s",
          got ? got : "(nothing)");
    if (saved_preload) {
        SDL_setenv_unsafe("LD_PRELOAD", saved_preload, 1);
        free(saved_preload);
    } else {
        SDL_unsetenv_unsafe("LD_PRELOAD");
    }
    CHECK(!SDL_OpenURL(NULL), "NULL url rejected");
    SDL_setenv_unsafe("PATH", tmpdir, 1);
    unlink(out);
    char xdg[512];
    snprintf(xdg, sizeof(xdg), "%s/xdg-open", tmpdir);
    unlink(xdg);
    CHECK(!SDL_OpenURL("https://example.org"), "no xdg-open: fails");
    SDL_setenv_unsafe("PATH", orig_path, 1);
}

static void make_battery(const char *root, const char *name, const char *type, const char *status, const char *cap,
                         const char *tte)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", root, name);
    mkdir(dir, 0755);
    write_file(dir, "type", type, false);
    if (status) {
        write_file(dir, "status", status, false);
    }
    if (cap) {
        write_file(dir, "capacity", cap, false);
    }
    if (tte) {
        write_file(dir, "time_to_empty_now", tte, false);
    }
}

static void test_power(void)
{
    char root[512];
    snprintf(root, sizeof(root), "%s/power", tmpdir);
    mkdir(root, 0755);
    SDL_setenv_unsafe("SDLOP_POWER_SUPPLY_PATH", root, 1);
    int secs = 0, pct = 0;
    CHECK(SDL_GetPowerInfo(&secs, &pct) == SDL_POWERSTATE_NO_BATTERY && secs == -1 && pct == -1,
          "empty dir: no battery");
    make_battery(root, "AC", "Mains\n", NULL, NULL, NULL);
    make_battery(root, "BAT0", "Battery\n", "Discharging\n", "57\n", "3600\n");
    make_battery(root, "BAT1", "Battery\n", "Discharging\n", "90\n", "1200\n");
    make_battery(root, "hidpp_battery_0", "Battery\n", "Discharging\n", "100\n", "99999\n");
    write_file(root, "hidpp_battery_0/scope", "Device\n", false);
    SDL_PowerState st = SDL_GetPowerInfo(&secs, &pct);
    CHECK(st == SDL_POWERSTATE_ON_BATTERY && secs == 3600 && pct == 57, "battery: state %d secs %d pct %d", st,
          secs, pct);
    CHECK(SDL_GetPowerInfo(NULL, NULL) == SDL_POWERSTATE_ON_BATTERY, "NULL outputs");
    char bad[512];
    snprintf(bad, sizeof(bad), "%s/nonexistent", tmpdir);
    SDL_setenv_unsafe("SDLOP_POWER_SUPPLY_PATH", bad, 1);
    CHECK(SDL_GetPowerInfo(&secs, &pct) == SDL_POWERSTATE_UNKNOWN && secs == -1 && pct == -1, "no sysfs: unknown");
    SDL_unsetenv_unsafe("SDLOP_POWER_SUPPLY_PATH");
}

/* declared by SDL_main.h only for Windows/GDK, but exported on every
 * platform by stock SDL3 (returning "unsupported") */
extern SDL_DECLSPEC bool SDLCALL SDL_RegisterApp(const char *name, Uint32 style, void *hInst);
extern SDL_DECLSPEC void SDLCALL SDL_UnregisterApp(void);
extern SDL_DECLSPEC void SDLCALL SDL_GDKSuspendComplete(void);

static void test_stubs(void)
{
    CHECK(!SDL_RegisterApp("x", 0, NULL), "RegisterApp unsupported");
    SDL_UnregisterApp();
    SDL_GDKSuspendComplete();
    CHECK(SDL_GetPenDeviceType(1) == SDL_PEN_DEVICE_TYPE_INVALID, "no pens");
}

int main(void)
{
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/sdlop_test_misc_XXXXXX");
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        return 1;
    }
    const char *p = getenv("PATH");
    char *orig_path = strdup(p ? p : "/usr/bin:/bin");

    test_guid();
    test_assert();
    test_stubs();
    test_power();
    test_messagebox(orig_path);
    test_openurl(orig_path);
    SDL_Quit();

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0) {
        fprintf(stderr, "cleanup failed\n");
    }
    free(orig_path);
    if (failures) {
        printf("test_misc: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_misc: OK\n");
    return 0;
}
