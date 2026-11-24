/*
  SDLop test: Win32 window management (parenting, modality, opacity, z-order,
  focusability, flashing).

  Every function here was reachable, exported and completely untested on
  Windows: the Linux-only test_window_api.c touches most of them, and it does
  not run on this platform, so nothing else in the suite would notice if they
  silently stopped working. They are also the kind of thing a video driver gets
  wrong quietly -- the call returns true and the OS state never changes -- so
  each check below asserts the *Win32* consequence (the owner HWND, the
  WS_EX_TOPMOST bit, the layered alpha), not just the SDL-side round trip.

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

/* --- parenting ---------------------------------------------------------- */

static void test_parenting(SDL_Window *parent, SDL_Window *child)
{
    const HWND hparent = hwnd_of(parent);
    const HWND hchild = hwnd_of(child);
    CHECK(hparent && hchild, "no HWND from the window properties");
    if (!hparent || !hchild) {
        return;
    }

    CHECK(SDL_SetWindowParent(child, parent), "SDL_SetWindowParent: %s", SDL_GetError());
    CHECK(SDL_GetWindowParent(child) == parent, "SDL_GetWindowParent did not round-trip");
    /* The SDL-side accessor alone proves nothing: Win32 expresses parenting as
     * window ownership, so that is what has to be true. */
    CHECK(GetWindow(hchild, GW_OWNER) == hparent,
          "child's Win32 owner is %p, expected the parent's HWND %p",
          (void *)GetWindow(hchild, GW_OWNER), (void *)hparent);

    /* A modal window must have a parent; this is the documented contract.
     * Win32 expresses modality by *disabling the parent*, so that is the
     * assertion -- returning true without disabling anything, which this
     * driver used to do, leaves the parent fully clickable and the "modal"
     * dialog is not modal at all. */
    CHECK(SDL_ShowWindow(child), "SDL_ShowWindow(child): %s", SDL_GetError());
    CHECK(SDL_SetWindowModal(child, true), "SDL_SetWindowModal(true): %s", SDL_GetError());
    CHECK(IsWindowEnabled(hparent) == FALSE,
          "the parent HWND is still enabled while a modal child is shown");
    CHECK(SDL_SetWindowModal(child, false), "SDL_SetWindowModal(false): %s", SDL_GetError());
    CHECK(IsWindowEnabled(hparent) == TRUE,
          "the parent HWND stayed disabled after the modal flag was cleared");

    CHECK(SDL_SetWindowParent(child, NULL), "SDL_SetWindowParent(NULL): %s", SDL_GetError());
    CHECK(SDL_GetWindowParent(child) == NULL, "SDL_GetWindowParent after unparent");
    CHECK(GetWindow(hchild, GW_OWNER) == NULL,
          "the Win32 owner survived SDL_SetWindowParent(NULL): %p",
          (void *)GetWindow(hchild, GW_OWNER));

    /* Setting modal with no parent must be refused, not quietly accepted. */
    CHECK(!SDL_SetWindowModal(child, true),
          "SDL_SetWindowModal on a parentless window should fail");
}

/* --- opacity ------------------------------------------------------------ */

static void test_opacity(SDL_Window *window)
{
    const HWND hwnd = hwnd_of(window);
    CHECK(SDL_SetWindowOpacity(window, 0.5f), "SDL_SetWindowOpacity(0.5): %s", SDL_GetError());
    {
        const float got = SDL_GetWindowOpacity(window);
        CHECK(got > 0.45f && got < 0.55f, "opacity round-trip: got %.3f", got);
    }
    /* Win32 implements this with a layered window; if the style was never set
     * the window simply does not fade, whatever SDL reports. */
    CHECK((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0,
          "WS_EX_LAYERED is not set, so the alpha has no effect");

    /* Out-of-range values are clamped by SDL3, not turned into a Win32 error. */
    SDL_SetWindowOpacity(window, -1.0f);
    {
        const float got = SDL_GetWindowOpacity(window);
        CHECK(got >= 0.0f && got <= 1.0f, "negative opacity left %.3f", got);
    }
    CHECK(SDL_SetWindowOpacity(window, 1.0f), "SDL_SetWindowOpacity(1.0): %s", SDL_GetError());
}

/* --- z-order ------------------------------------------------------------ */

/* Deliberately a window that owns nothing. Wine refuses
 * SetWindowPos(HWND_TOPMOST) for a window with an owned window -- plain
 * Win32, reproduced without SDL: the call returns FALSE with
 * ERROR_INVALID_WINDOW_HANDLE and WS_EX_TOPMOST never appears. Native Windows
 * has no such restriction, so this is a Wine limitation and must not be
 * encoded as expected behaviour. */
static void test_always_on_top(void)
{
    SDL_Window *window = SDL_CreateWindow("sdlop z-order", 300, 200, 0);
    HWND hwnd;
    CHECK(window != NULL, "SDL_CreateWindow: %s", SDL_GetError());
    if (!window) {
        return;
    }
    hwnd = hwnd_of(window);

    CHECK(SDL_SetWindowAlwaysOnTop(window, true), "SDL_SetWindowAlwaysOnTop(true): %s", SDL_GetError());
    CHECK((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
          "WS_EX_TOPMOST not set after SDL_SetWindowAlwaysOnTop(true)");

    CHECK(SDL_SetWindowAlwaysOnTop(window, false), "SDL_SetWindowAlwaysOnTop(false): %s", SDL_GetError());
    CHECK((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0,
          "WS_EX_TOPMOST still set after SDL_SetWindowAlwaysOnTop(false)");

    SDL_DestroyWindow(window);
}

/* --- focusability and flashing ------------------------------------------ */

static void test_focusable_and_flash(SDL_Window *window)
{
    const HWND hwnd = hwnd_of(window);

    /* A non-focusable window gets WS_EX_NOACTIVATE exactly; losing it means
     * clicking the window steals focus from the application. */
    CHECK(SDL_SetWindowFocusable(window, false), "SDL_SetWindowFocusable(false): %s", SDL_GetError());
    CHECK((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0,
          "WS_EX_NOACTIVATE not set for a non-focusable window");
    CHECK(SDL_SetWindowFocusable(window, true), "SDL_SetWindowFocusable(true): %s", SDL_GetError());
    CHECK((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) == 0,
          "WS_EX_NOACTIVATE still set after SDL_SetWindowFocusable(true)");

    /* Flashing does not block and has no readable state; the only failure
     * mode reachable here is a crash or a false return. */
    CHECK(SDL_FlashWindow(window, SDL_FLASH_BRIEFLY), "SDL_FlashWindow(BRIEFLY): %s", SDL_GetError());
    CHECK(SDL_FlashWindow(window, SDL_FLASH_UNTIL_FOCUSED),
          "SDL_FlashWindow(UNTIL_FOCUSED): %s", SDL_GetError());
}

/* --- size constraints --------------------------------------------------- */

static void test_size_constraints(SDL_Window *window)
{
    CHECK(SDL_SetWindowMinimumSize(window, 200, 150), "SetWindowMinimumSize: %s", SDL_GetError());
    CHECK(SDL_SetWindowMaximumSize(window, 800, 600), "SetWindowMaximumSize: %s", SDL_GetError());

    /* Ask for a size below the minimum: the driver must clamp it, because the
     * Win32 window procedure enforces the same limits on user resizes. */
    CHECK(SDL_SetWindowSize(window, 50, 40), "SetWindowSize: %s", SDL_GetError());
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        CHECK(w >= 200 && h >= 150, "size was not clamped to the minimum: %dx%d", w, h);
    }
    CHECK(SDL_SetWindowSize(window, 4000, 3000), "SetWindowSize: %s", SDL_GetError());
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        CHECK(w <= 800 && h <= 600, "size was not clamped to the maximum: %dx%d", w, h);
    }
    CHECK(SDL_SetWindowMinimumSize(window, 0, 0), "clear minimum: %s", SDL_GetError());
    CHECK(SDL_SetWindowMaximumSize(window, 0, 0), "clear maximum: %s", SDL_GetError());
}

/* Destroying a window that was left modal must re-enable the parent: the
 * alternative is an application whose main window silently stops accepting
 * input for the rest of its life. */
static void test_modal_destroy_reenables_parent(void)
{
    SDL_Window *p = SDL_CreateWindow("sdlop modal parent", 300, 200, 0);
    SDL_Window *c = SDL_CreateWindow("sdlop modal child", 150, 100, SDL_WINDOW_UTILITY);
    CHECK(p && c, "could not create the modal pair: %s", SDL_GetError());
    if (!p || !c) {
        if (c) {
            SDL_DestroyWindow(c);
        }
        if (p) {
            SDL_DestroyWindow(p);
        }
        return;
    }
    CHECK(SDL_SetWindowParent(c, p), "SetWindowParent: %s", SDL_GetError());
    CHECK(SDL_ShowWindow(c), "ShowWindow: %s", SDL_GetError());
    CHECK(SDL_SetWindowModal(c, true), "SetWindowModal: %s", SDL_GetError());
    CHECK(IsWindowEnabled(hwnd_of(p)) == FALSE, "parent not disabled by the modal child");
    SDL_DestroyWindow(c);
    CHECK(IsWindowEnabled(hwnd_of(p)) == TRUE,
          "destroying a modal child left the parent disabled -- the application "
          "would never accept input again");
    SDL_DestroyWindow(p);
}

/* Hiding a modal window keeps the parent blocked and showing it again must
 * re-apply that; the core calls SetWindowModal(true) on show for this. */
static void test_modal_survives_hide_show(SDL_Window *parent, SDL_Window *child)
{
    CHECK(SDL_SetWindowParent(child, parent), "SetWindowParent: %s", SDL_GetError());
    CHECK(SDL_ShowWindow(child), "ShowWindow: %s", SDL_GetError());
    CHECK(SDL_SetWindowModal(child, true), "SetWindowModal: %s", SDL_GetError());
    SDL_HideWindow(child);
    CHECK(SDL_ShowWindow(child), "ShowWindow again: %s", SDL_GetError());
    CHECK(IsWindowEnabled(hwnd_of(parent)) == FALSE,
          "showing a modal window again did not re-disable the parent");
    CHECK(SDL_SetWindowModal(child, false), "SetWindowModal(false): %s", SDL_GetError());
    CHECK(IsWindowEnabled(hwnd_of(parent)) == TRUE, "parent left disabled");
}

int main(void)
{
    SDL_Window *parent = NULL;
    SDL_Window *child = NULL;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }

    parent = SDL_CreateWindow("sdlop parent", 400, 300, 0);
    CHECK(parent != NULL, "SDL_CreateWindow(parent): %s", SDL_GetError());
    child = SDL_CreateWindow("sdlop child", 200, 120, SDL_WINDOW_UTILITY);
    CHECK(child != NULL, "SDL_CreateWindow(child): %s", SDL_GetError());

    if (parent && child) {
        test_parenting(parent, child);
    }
    if (parent && child) {
        test_modal_survives_hide_show(parent, child);
    }
    test_modal_destroy_reenables_parent();
    if (parent) {
        test_opacity(parent);
        test_focusable_and_flash(parent);
        test_size_constraints(parent);
    }
    test_always_on_top();

    if (child) {
        SDL_DestroyWindow(child);
    }
    if (parent) {
        SDL_DestroyWindow(parent);
    }
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_windowmgmt: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_windowmgmt: PASS\n");
    return 0;
}
