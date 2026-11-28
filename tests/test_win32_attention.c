/*
  SDLop test: window attention, synchronisation and ICC profile.

  Three small vtable entries that had no test. Each one had a bug that a test
  catches immediately and that a reader would not:

   - SDL_FlashWindow tested FlashWindowEx()'s return value for success. That
     call returns the window's *previous active state*, so it returns FALSE
     for exactly the case the function exists for: flashing a window that does
     not have focus. Every such flash set an SDL error.
   - SDL_SyncWindow was implemented as "false while the window has queued
     messages". Any unrelated pending message -- an ordinary WM_NULL -- made it
     report failure. Win32 applies window state synchronously, and stock SDL3
     has no Win32 implementation at all, so the core's "true" is correct.
   - SDL_FLASH_BRIEFLY flashed twice where stock flashes once, and
     SDL_FLASH_UNTIL_FOCUSED asked for FLASHW_ALL where stock asks for
     FLASHW_TRAY. Divergences from stock in a parity port are bugs unless
     proven otherwise.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include <SDL3/SDL.h>

#include <windows.h>
#include <stdio.h>

static int failures = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
            failures++;                                         \
        }                                                       \
    } while (0)

static HWND hwnd_of(SDL_Window *window)
{
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

/* --- SDL_FlashWindow -------------------------------------------------- */

static void test_flash_window(SDL_Window *window)
{
    SDL_Window *second = NULL;

    /* A window with focus: FlashWindowEx returns TRUE here, so the old
     * return-value-as-success bug was invisible. */
    CHECK(SDL_FlashWindow(window, SDL_FLASH_BRIEFLY), "flash briefly: %s", SDL_GetError());
    CHECK(SDL_FlashWindow(window, SDL_FLASH_UNTIL_FOCUSED), "flash until focused: %s", SDL_GetError());
    CHECK(SDL_FlashWindow(window, SDL_FLASH_CANCEL), "flash cancel: %s", SDL_GetError());

    /* The case that matters: a window that does not have focus, which is the
     * only time an application flashes. FlashWindowEx returns the previous
     * active state, i.e. FALSE, so this failed with an SDL error before. */
    second = SDL_CreateWindow("sdlop attention 2", 200, 150, 0);
    CHECK(second != NULL, "second window: %s", SDL_GetError());
    if (second) {
        SDL_ShowWindow(second);
        SetForegroundWindow(hwnd_of(second));
        SDL_PumpEvents();
        SDL_Delay(50);

        SDL_ClearError();
        CHECK(SDL_FlashWindow(window, SDL_FLASH_UNTIL_FOCUSED),
              "flash of an unfocused window: %s", SDL_GetError());
        CHECK(*SDL_GetError() == '\0',
              "flashing an unfocused window set an error: \"%s\"", SDL_GetError());
        SDL_FlashWindow(window, SDL_FLASH_CANCEL);
        SDL_DestroyWindow(second);
    }

    /* A window with no HWND cannot be flashed, and must say so rather than
     * silently succeeding. Checked through a hidden window left unshown is not
     * equivalent -- those still have an HWND -- so this is a sanity check that
     * the call is wired at all: SDL rejects NULL. */
    CHECK(!SDL_FlashWindow(NULL, SDL_FLASH_BRIEFLY), "flashing a NULL window should fail");
}

/* --- SDL_SyncWindow --------------------------------------------------- */

static void test_sync_window(SDL_Window *window)
{
    HWND hwnd = hwnd_of(window);

    /* Idle: nothing pending, so this always worked. */
    SDL_PumpEvents();
    CHECK(SDL_SyncWindow(window), "sync of an idle window: %s", SDL_GetError());

    /* Win32 applies window state synchronously, so even with a message sitting
     * in the queue there is nothing to wait for. The old implementation
     * returned false here. */
    PostMessageW(hwnd, WM_NULL, 0, 0);
    CHECK(SDL_SyncWindow(window), "sync with a pending message: %s", SDL_GetError());

    /* Window operations with the synchronous hint look at the return value of
     * SDL_SyncWindow internally, so a false there would surface as a failed
     * resize. */
    SDL_SetHint(SDL_HINT_VIDEO_SYNC_WINDOW_OPERATIONS, "1");
    CHECK(SDL_SetWindowSize(window, 320, 240), "resize with sync hint: %s", SDL_GetError());
    CHECK(SDL_SetWindowPosition(window, 40, 40), "move with sync hint: %s", SDL_GetError());
    CHECK(SDL_MaximizeWindow(window), "maximize with sync hint: %s", SDL_GetError());
    CHECK(SDL_RestoreWindow(window), "restore with sync hint: %s", SDL_GetError());
    SDL_ResetHint(SDL_HINT_VIDEO_SYNC_WINDOW_OPERATIONS);

    SDL_PumpEvents();
    CHECK(SDL_SyncWindow(window), "sync after the operations: %s", SDL_GetError());
}

/* --- SDL_GetWindowICCProfile ------------------------------------------ */

static void test_icc_profile(SDL_Window *window)
{
    size_t size = (size_t)-1;
    void *profile;

    /* A plain display usually has no profile installed, so a NULL return with
     * a clear error is a legitimate outcome, not a failure. What must hold
     * either way is that *size is set to 0 on failure and describes the buffer
     * on success, so a caller can never read past it. */
    profile = SDL_GetWindowICCProfile(window, &size);
    if (profile) {
        CHECK(size > 0, "ICC profile returned with a size of 0");
        CHECK(size >= 128, "ICC profile of %zu bytes is too small to be a profile", size);
        /* every ICC profile starts with its own size, big-endian, and the
         * file must be at least that long */
        if (size >= 4) {
            const unsigned char *bytes = (const unsigned char *)profile;
            const unsigned long declared = ((unsigned long)bytes[0] << 24) | ((unsigned long)bytes[1] << 16) |
                                           ((unsigned long)bytes[2] << 8) | (unsigned long)bytes[3];
            CHECK(declared == size,
                  "ICC profile declares %lu bytes but %zu were read", declared, size);
        }
        SDL_free(profile);
    } else {
        CHECK(size == 0, "ICC profile failed but report a size of %zu", size);
        CHECK(*SDL_GetError() != '\0', "ICC profile failed without setting an error");
        printf("  (this display has no ICC profile: %s)\n", SDL_GetError());
    }

    /* A NULL window is rejected by the core before the driver is reached, so
     * the size out-parameter is deliberately left untouched: the driver never
     * runs. This is checked because the driver dereferences the window without
     * a NULL check, exactly as stock SDL3 does -- the core's CHECK_WINDOW is
     * what keeps that safe. */
    size = (size_t)-1;
    SDL_ClearError();
    CHECK(SDL_GetWindowICCProfile(NULL, &size) == NULL,
          "ICC profile of a NULL window should fail");
    CHECK(*SDL_GetError() != '\0', "a NULL window was rejected without an error");
}

int main(void)
{
    SDL_Window *window = NULL;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    window = SDL_CreateWindow("sdlop attention", 300, 200, SDL_WINDOW_RESIZABLE);
    CHECK(window != NULL, "SDL_CreateWindow: %s", SDL_GetError());
    if (!window) {
        SDL_Quit();
        return 2;
    }
    SDL_ShowWindow(window);
    SetForegroundWindow(hwnd_of(window));
    SDL_PumpEvents();

    test_flash_window(window);
    test_sync_window(window);
    test_icc_profile(window);

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_attention: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_attention: PASS\n");
    return 0;
}
