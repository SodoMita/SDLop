/*
  SDLop test: Win32 IME composition events.

  Text input already worked without any of this: when an IME commits, Windows
  delivers the characters as WM_CHAR and the driver turns them into
  SDL_EVENT_TEXT_INPUT. What was missing was the composition *itself*, so an
  application showing an in-progress composition (a search box, a chat input)
  got nothing and could not tell a composition was running.

  No real IME is needed to test the plumbing: an IME is just a process that
  sends the window these messages, and Imm32 lets a test set the composition
  string directly. Both are used here.

  What this cannot cover: the behaviour of a real Japanese/Chinese IME (which
  candidate lists it opens, how it commits). That is the IME's half of the
  contract; this test pins SDL's half.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/
#include <SDL3/SDL.h>

#include <windows.h>
#include <imm.h>
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

/* Collect every editing event since the last call. Returns how many arrived
 * and, through `last`, the final one. */
static int drain_editing(SDL_Event *last)
{
    SDL_Event event;
    int count = 0;

    SDL_PumpEvents();
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_TEXT_EDITING) {
            count++;
            if (last) {
                *last = event;
            }
        }
    }
    return count;
}

static void settle(void)
{
    /* the messages are posted to this thread's window, so a short wait plus a
     * pump is what delivers them */
    SDL_Delay(60);
    SDL_PumpEvents();
}

int main(void)
{
    SDL_Window *window = NULL;
    HWND hwnd = NULL;
    SDL_Event last;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    window = SDL_CreateWindow("sdlop ime", 300, 200, 0);
    CHECK(window != NULL, "SDL_CreateWindow: %s", SDL_GetError());
    if (!window) {
        SDL_Quit();
        return 2;
    }
    SDL_ShowWindow(window);
    SDL_PumpEvents();
    hwnd = hwnd_of(window);
    CHECK(hwnd != NULL, "no HWND");
    if (!hwnd) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 2;
    }

    /* --- before text input is active, composition must be ignored ------- */
    PostMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    settle();
    CHECK(drain_editing(NULL) == 0,
          "a composition event was delivered while text input was not active");

    CHECK(SDL_StartTextInput(window), "SDL_StartTextInput: %s", SDL_GetError());
    settle();
    drain_editing(NULL);

    /* --- starting a composition reports an empty composition ------------ */
    PostMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    settle();
    {
        const int n = drain_editing(&last);
        CHECK(n >= 1, "WM_IME_STARTCOMPOSITION produced no editing event");
        if (n >= 1) {
            CHECK(last.edit.text != NULL && last.edit.text[0] == '\0',
                  "a new composition should report empty text, got \"%s\"",
                  last.edit.text ? last.edit.text : "(null)");
        }
    }

    /* --- the composition string itself, through Imm32 ------------------- */
    {
        HIMC himc = ImmGetContext(hwnd);
        if (himc) {
            /* two hiragana: one code point is not enough to catch a conversion
             * that only handles ASCII or one byte per character */
            WCHAR text[] = { 0x3042, 0x3044, 0 };
            const BOOL set = ImmSetCompositionStringW(himc, SCS_SETSTR, text,
                                                      (DWORD)(2 * sizeof(WCHAR)), NULL, 0);
            ImmReleaseContext(hwnd, himc);
            CHECK(set, "ImmSetCompositionStringW failed: %lu", (unsigned long)GetLastError());

            if (set) {
                /* Setting the string makes this Wine emit its own composition
                 * messages; post one explicitly as well, as a real IME would.
                 * The driver de-duplicates identical compositions, so however
                 * many messages arrive, the text must be reported correctly. */
                PostMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
                settle();
                {
                    const int n = drain_editing(&last);
                    CHECK(n >= 1, "WM_IME_COMPOSITION produced no editing event");
                    if (n >= 1) {
                        const char *want = "\xE3\x81\x82\xE3\x81\x84"; /* あい */
                        CHECK(last.edit.text && SDL_strcmp(last.edit.text, want) == 0,
                              "composition text is \"%s\", expected \"%s\"",
                              last.edit.text ? last.edit.text : "(null)", want);
                    }
                }
            }
        } else {
            CHECK(0, "no IME context, so the composition path cannot be exercised");
        }
    }

    /* --- ending the composition clears it ------------------------------ */
    PostMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    settle();
    {
        const int n = drain_editing(&last);
        CHECK(n >= 1, "WM_IME_ENDCOMPOSITION produced no editing event");
        if (n >= 1) {
            CHECK(last.edit.text != NULL && last.edit.text[0] == '\0',
                  "ending a composition should clear the text, got \"%s\"",
                  last.edit.text ? last.edit.text : "(null)");
        }
    }

    /* --- a committed result clears it and does NOT duplicate the text --- */
    PostMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
    settle();
    drain_editing(&last);
    {
        /* GCS_RESULTSTR also arrives as WM_CHAR; the driver must not also send
         * it as editing text, or every committed character appears twice */
        SDL_Event event;
        int inputs = 0;
        SDL_PumpEvents();
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                inputs++;
            }
        }
        CHECK(inputs == 0, "a GCS_RESULTSTR message produced %d text input event(s)", inputs);
    }

    CHECK(SDL_StopTextInput(window), "SDL_StopTextInput: %s", SDL_GetError());

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_ime: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_win32_ime: PASS\n");
    return 0;
}
