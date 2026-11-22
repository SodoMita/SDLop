/*
  test_options - every SDLOP_* environment knob is also an SDL hint, so code
  can choose it (README "Options: environment or code").

  The environment is changed with SDL_setenv_unsafe(): like SDL3, SDLop reads
  variables through SDL_GetEnvironment(), a snapshot taken on first use.

  Checks the precedence SDL_HINT_OVERRIDE > environment > normal hint >
  default on knobs whose effect is observable through the public API:
    - SDLOP_LOG_PRIORITY  (SDL_GetLogPriority)
    - SDLOP_FBDEV_DUMP / SDLOP_FBDEV_SIZE  (fbdev RAM scanout: display size)
    - SDLOP_RAW_INPUT     (X11 default is window-system input: no evdev worker,
                           also with a readable /dev/input; skipped without
                           $DISPLAY)
*/
#include <SDL3/SDL.h>
#include <SDL3/SDLop.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void test_log_priority(void)
{
    SDL_unsetenv_unsafe("SDLOP_LOG_PRIORITY");
    SDL_unsetenv_unsafe("SDL_LOGGING");

    CHECK(SDL_SetHint(SDLOP_HINT_LOG_PRIORITY, "debug"), "set hint");
    SDL_ResetLogPriorities();
    CHECK(SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION) == SDL_LOG_PRIORITY_DEBUG,
          "code-set SDLOP_LOG_PRIORITY applies (got %d)",
          (int)SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION));

    SDL_setenv_unsafe("SDLOP_LOG_PRIORITY", "warn", 1);
    SDL_ResetLogPriorities();
    CHECK(SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION) == SDL_LOG_PRIORITY_WARN,
          "environment overrides a normal hint (got %d)",
          (int)SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION));

    CHECK(SDL_SetHintWithPriority(SDLOP_HINT_LOG_PRIORITY, "info", SDL_HINT_OVERRIDE), "set override");
    SDL_ResetLogPriorities();
    CHECK(SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION) == SDL_LOG_PRIORITY_INFO,
          "SDL_HINT_OVERRIDE beats the environment (got %d)",
          (int)SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION));

    CHECK(SDL_ResetHint(SDLOP_HINT_LOG_PRIORITY), "reset hint");
    SDL_ResetLogPriorities();
    CHECK(SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION) == SDL_LOG_PRIORITY_WARN,
          "SDL_ResetHint falls back to the environment (got %d)",
          (int)SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION));

    SDL_unsetenv_unsafe("SDLOP_LOG_PRIORITY");
    SDL_ResetLogPriorities();
}

/* SDL_Init the fbdev RAM scanout and return the display width, or -1 */
static int fbdev_display_width(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init(fbdev dump) failed: %s\n", SDL_GetError());
        return -1;
    }
    int w = -1;
    const char *driver = SDL_GetCurrentVideoDriver();
    CHECK(driver && strcmp(driver, "fbdev") == 0, "driver chosen by hint is fbdev (got %s)",
          driver ? driver : "?");
    CHECK(!SDLop_RawInputAvailable(), "SDLOP_RAW_INPUT=0 set from code keeps evdev off");
    SDL_Rect r;
    if (SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &r)) {
        w = r.w;
    }
    SDL_Quit();
    return w;
}

/* SDL_Quit() drops all hints (like SDL3), so every round sets them again */
static void set_fbdev_hints(const char *dump)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "fbdev");
    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "0");
    SDL_SetHint(SDLOP_HINT_FBDEV_TAKEOVER, "0");
    SDL_SetHint(SDLOP_HINT_FBDEV_DUMP, dump);
    SDL_SetHint(SDLOP_HINT_FBDEV_SIZE, "64x48");
}

static void test_fbdev_options(const char *dump)
{
    SDL_unsetenv_unsafe("SDL_VIDEODRIVER");
    SDL_unsetenv_unsafe("SDLOP_FBDEV_DUMP");
    SDL_unsetenv_unsafe("SDLOP_FBDEV_SIZE");
    SDL_unsetenv_unsafe("SDLOP_RAW_INPUT");
    SDL_unsetenv_unsafe("SDLOP_DISABLE_RAW_INPUT");

    /* everything chosen from code, nothing in the environment */
    set_fbdev_hints(dump);
    int w = fbdev_display_width();
    CHECK(w == 64, "code-set SDLOP_FBDEV_SIZE=64x48 (display width %d)", w);

    SDL_setenv_unsafe("SDLOP_FBDEV_SIZE", "80x60", 1);
    set_fbdev_hints(dump);
    w = fbdev_display_width();
    CHECK(w == 80, "environment SDLOP_FBDEV_SIZE beats a normal hint (display width %d)", w);

    set_fbdev_hints(dump);
    SDL_SetHintWithPriority(SDLOP_HINT_FBDEV_SIZE, "96x72", SDL_HINT_OVERRIDE);
    w = fbdev_display_width();
    CHECK(w == 96, "SDL_HINT_OVERRIDE beats the environment (display width %d)", w);

    SDL_unsetenv_unsafe("SDLOP_FBDEV_SIZE");
    SDL_ResetHints();
}

static void test_x11_raw_default(void)
{
    if (!getenv("DISPLAY") || !*getenv("DISPLAY")) {
        printf("test_options: X11 part SKIP (no DISPLAY)\n");
        return;
    }
    SDL_unsetenv_unsafe("SDLOP_RAW_INPUT");
    SDL_unsetenv_unsafe("SDLOP_DISABLE_RAW_INPUT");
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("test_options: X11 part SKIP (%s)\n", SDL_GetError());
        SDL_ResetHints();
        return;
    }
    CHECK(!SDLop_RawInputAvailable(), "X11 defaults to window-system input (no evdev worker)");
    SDL_Quit(); /* also drops the hints */

    /* the code may ask for raw input; without a readable keyboard *and*
       pointer SDLop must fall back to window-system input, not fail */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    SDL_SetHint(SDLOP_HINT_RAW_INPUT, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        /* The X server can be briefly unavailable under a parallel ctest run
         * (measured: "Could not connect to X display :100"). Hint precedence
         * is not an X11 property, so a busy display is a skip, not a failure. */
        printf("test_options: X11 part SKIP (%s)\n", SDL_GetError());
        SDL_unsetenv_unsafe("SDLOP_RAW_INPUT");
        SDL_ResetHints();
        return;
    }
    printf("test_options: X11 with SDLOP_RAW_INPUT=1 -> evdev %s\n",
           SDLop_RawInputAvailable() ? "is the input source" : "unavailable, window-system input");
    SDL_Quit();

    /* SDL_HINT_OVERRIDE beats an environment request */
    SDL_setenv_unsafe("SDLOP_RAW_INPUT", "1", 1);
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    SDL_SetHintWithPriority(SDLOP_HINT_RAW_INPUT, "0", SDL_HINT_OVERRIDE);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("test_options: X11 part SKIP (%s)\n", SDL_GetError());
        SDL_unsetenv_unsafe("SDLOP_RAW_INPUT");
        SDL_ResetHints();
        return;
    }
    CHECK(!SDLop_RawInputAvailable(), "override SDLOP_RAW_INPUT=0 beats the environment");
    SDL_Quit();
    SDL_unsetenv_unsafe("SDLOP_RAW_INPUT");
    SDL_ResetHints();
}

int main(void)
{
    char dump[256];
    snprintf(dump, sizeof(dump), "/tmp/sdlop_test_options_%d.ppm", (int)getpid());

    test_log_priority();
    test_fbdev_options(dump);
    test_x11_raw_default();
    unlink(dump);

    if (failures) {
        fprintf(stderr, "test_options: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_options: OK\n");
    return 0;
}
