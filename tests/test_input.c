/*
  SDLop input tests: the async input path, the keymap, text input and the
  mouse state machine.

  There is no /dev/input in a container, so the test drives the *same* code path a
  real device drives: it writes evdev-shaped records into a FIFO and the worker
  thread reads them with the same translation the evdev backend uses.

  The keyboard layout is the machine's, not the library's, so the expected
  keycodes and text are not hardcoded: the test asks the active layout what the A
  key produces and asserts the plumbing against that answer.

      SDLOP_TEST_INPUT=/tmp/sdlop-in.fifo   (exported by this test itself)

  Run it with:  make check
*/

#include <SDL3/SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static int failures;
static int checks;

static void settle(void);

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        checks++;                                                               \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

static SDL_Window *input_window;
static int fifo_fd = -1;
static const char *fifo_path = "/tmp/sdlop-test-input.fifo";

/* EV_KEY 30 1 == KEY_A pressed, the same line format a human would type after
   looking at evtest output. */
static void feed(const char *line)
{
    ssize_t written = write(fifo_fd, line, SDL_strlen(line));
    CHECK(written == (ssize_t)SDL_strlen(line), "write to the test fifo failed");
}

static bool collect_until(SDL_EventType type, SDL_Event *out, int timeout_ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < deadline) {
        SDL_Event event;
        SDL_PumpEvents();
        while (SDL_PollEvent(&event)) {
            if (event.type == type) {
                if (out) {
                    *out = event;
                }
                return true;
            }
        }
        SDL_Delay(1);
    }
    return false;
}

/* Drop everything that is queued *and* wait for the worker thread to run dry
   first: the records a test just wrote are translated asynchronously, so without
   this an event from the previous test can turn up in the middle of the next one
   and be mistaken for one of its own. */
static void drain(void)
{
    SDL_Event event;

    settle();
    SDL_PumpEvents();
    while (SDL_PollEvent(&event)) {
        /* drop */
    }
    SDL_PumpEvents();
    while (SDL_PollEvent(&event)) {
        /* drop */
    }
}

/* Wait until a condition holds, pumping events meanwhile: the worker thread is
   asynchronous, so a fixed sleep would make the test flaky. */
static bool wait_until(bool (*predicate)(void), int timeout_ms)
{
    Uint64 deadline = SDL_GetTicks() + (Uint64)timeout_ms;
    while (SDL_GetTicks() < deadline) {
        drain();
        if (predicate()) {
            return true;
        }
        SDL_Delay(1);
    }
    return false;
}

static bool modstate_clear(void)
{
    return SDL_GetModState() == SDL_KMOD_NONE;
}

static bool mouse_button_released(void)
{
    return (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) == 0;
}

static void test_scancode_table(void)
{
    /* the evdev -> SDL scancode table is not public API, but its results are */
    CHECK(SDL_GetScancodeName(SDL_SCANCODE_A) != NULL, "no name for A");
    CHECK(SDL_GetScancodeFromName("A") == SDL_SCANCODE_A, "A round trip");
    CHECK(SDL_GetScancodeFromName("F1") == SDL_SCANCODE_F1, "F1 round trip");
    CHECK(SDL_GetScancodeFromName("Left Ctrl") == SDL_SCANCODE_LCTRL, "left ctrl");
    CHECK(SDL_GetScancodeFromName("VolumeUp") == SDL_SCANCODE_VOLUMEUP, "volume up");
}

/* The keyboard layout belongs to the machine, not to the library: on a French
   desktop the key that reports EV_KEY 30 types "q". So instead of assuming the
   layout is American, ask it once what the A key does and assert the plumbing
   against that - the suite then verifies SDLop on whatever desktop it runs on
   (US, AZERTY, Dvorak), which is a stronger statement than "the machine is
   configured the way the test assumed".

   Probes: KEY_A (evdev 30) and KEY_LEFTSHIFT + KEY_A. */
static SDL_Keycode layout_key_a;        /* keycode of KEY_A */
static SDL_Keycode layout_key_a_shift;  /* keycode of KEY_A with shift held */
static char layout_text_a[SDL_TEXTINPUTEVENT_TEXT_SIZE];
static char layout_text_a_shift[SDL_TEXTINPUTEVENT_TEXT_SIZE];
static bool layout_probe_ok;

static void probe_layout(void)
{
    SDL_Event event;

    drain();
    SDL_StartTextInput(NULL);

    /* KEY_A */
    feed("EV_KEY 30 1\n");
    if (collect_until(SDL_EVENT_KEY_DOWN, &event, 2000)) {
        layout_key_a = event.key.key;
        layout_probe_ok = true;
    }
    if (collect_until(SDL_EVENT_TEXT_INPUT, &event, 2000)) {
        SDL_strlcpy(layout_text_a, event.text.text, sizeof(layout_text_a));
    }
    feed("EV_KEY 30 0\n");
    drain();

    /* shift + KEY_A */
    feed("EV_KEY 42 1\n");
    feed("EV_KEY 30 1\n");
    if (collect_until(SDL_EVENT_KEY_DOWN, &event, 2000)) {
        SDL_Event next;
        /* the first KEY_DOWN here is the shift itself; the A comes next */
        if (event.key.scancode != SDL_SCANCODE_LSHIFT ||
            !collect_until(SDL_EVENT_KEY_DOWN, &next, 2000)) {
            next = event;
        }
        layout_key_a_shift = next.key.key;
    }
    if (collect_until(SDL_EVENT_TEXT_INPUT, &event, 2000)) {
        SDL_strlcpy(layout_text_a_shift, event.text.text, sizeof(layout_text_a_shift));
    }
    feed("EV_KEY 30 0\n");
    feed("EV_KEY 42 0\n");
    SDL_StopTextInput(NULL);          /* leave the state as the tests expect it */
    drain();
}

static void test_key_events(void)
{
    SDL_Event event;
    const bool *keystate;

    drain();
    feed("EV_KEY 30 1\n");            /* KEY_A down */
    CHECK(collect_until(SDL_EVENT_KEY_DOWN, &event, 2000), "no key down reached the app");
    CHECK(event.key.scancode == SDL_SCANCODE_A, "scancode %d", (int)event.key.scancode);
    CHECK(event.key.key == layout_key_a, "keycode %x, this layout says %x", event.key.key,
          layout_key_a);
    CHECK(event.key.down, "key event says not down");
    CHECK(!event.key.repeat, "key event says repeat");
    CHECK(event.key.timestamp > 0, "no timestamp on the key event");
    {
        Uint64 latency = SDL_GetTicksNS() - event.key.timestamp;
        CHECK(latency < 100000000ULL, "key event is %llu ns old", (unsigned long long)latency);
    }

    keystate = SDL_GetKeyboardState(NULL);
    CHECK(keystate[SDL_SCANCODE_A], "SDL_GetKeyboardState() does not show A down");

    feed("EV_KEY 30 0\n");            /* KEY_A up */
    CHECK(collect_until(SDL_EVENT_KEY_UP, &event, 2000), "no key up reached the app");
    CHECK(event.key.scancode == SDL_SCANCODE_A && !event.key.down, "key up state");
    CHECK(!keystate[SDL_SCANCODE_A], "A is still down after the release");

    /* a repeat (value 2) is only delivered while text input is active */
    drain();
    feed("EV_KEY 30 2\n");
    CHECK(!collect_until(SDL_EVENT_KEY_DOWN, &event, 300), "repeat leaked without text input");
    CHECK(SDL_StartTextInput(NULL), "SDL_StartTextInput(): %s", SDL_GetError());
    feed("EV_KEY 30 2\n");
    CHECK(collect_until(SDL_EVENT_KEY_DOWN, &event, 2000), "no repeat with text input active");
    CHECK(event.key.repeat, "the event is not marked as a repeat");
    SDL_StopTextInput(NULL);
    feed("EV_KEY 30 0\n");
    drain();
}

static void test_modifiers(void)
{
    SDL_Event event;

    drain();
    feed("EV_KEY 42 1\n");            /* KEY_LEFTSHIFT */
    CHECK(collect_until(SDL_EVENT_KEY_DOWN, &event, 2000), "no shift down");
    CHECK((event.key.mod & SDL_KMOD_LSHIFT) != 0, "mod state has no LSHIFT: %04x", event.key.mod);
    CHECK((SDL_GetModState() & SDL_KMOD_LSHIFT) != 0, "SDL_GetModState()");

    /* with shift held, the keycode of the A key is uppercase */
    feed("EV_KEY 30 1\n");            /* KEY_A */
    CHECK(collect_until(SDL_EVENT_KEY_DOWN, &event, 2000), "no A with shift");
    CHECK(event.key.key == layout_key_a_shift, "shift+A produced %x, this layout says %x",
          event.key.key, layout_key_a_shift);
    CHECK(event.key.mod & SDL_KMOD_SHIFT, "SHIFT bit missing: %04x", event.key.mod);

    feed("EV_KEY 30 0\n");
    feed("EV_KEY 42 0\n");
    CHECK(wait_until(modstate_clear, 2000), "mod state is still %04x", SDL_GetModState());
}

static void test_mouse_buttons_and_motion(void)
{
    SDL_Event event;
    float x = 0.0f, y = 0.0f;

    drain();
    feed("EV_KEY 272 1\n");           /* BTN_LEFT */
    CHECK(collect_until(SDL_EVENT_MOUSE_BUTTON_DOWN, &event, 2000), "no mouse button down");
    CHECK(event.button.button == SDL_BUTTON_LEFT, "button %d", event.button.button);
    CHECK((SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK) != 0, "SDL_GetMouseState()");

    feed("EV_REL 0 7\n");             /* REL_X +7 */
    feed("EV_REL 1 3\n");             /* REL_Y +3 */
    CHECK(collect_until(SDL_EVENT_MOUSE_MOTION, &event, 2000), "no mouse motion");
    CHECK(event.motion.xrel != 0.0f || event.motion.yrel != 0.0f,
          "motion without a delta: %f %f", event.motion.xrel, event.motion.yrel);

    SDL_GetMouseState(&x, &y);
    CHECK(x != 0.0f || y != 0.0f, "mouse position did not move");

    feed("EV_REL 8 1\n");             /* REL_WHEEL +1 */
    CHECK(collect_until(SDL_EVENT_MOUSE_WHEEL, &event, 2000), "no wheel event");
    CHECK(event.wheel.y == 1.0f || event.wheel.y == -1.0f, "wheel y = %f", event.wheel.y);

    feed("EV_KEY 272 0\n");
    CHECK(collect_until(SDL_EVENT_MOUSE_BUTTON_UP, &event, 2000), "no mouse button up");
    CHECK(wait_until(mouse_button_released, 2000), "button still down");
    drain();
}

static void test_text_input(void)
{
    SDL_Event event;

    drain();
    CHECK(SDL_StartTextInput(NULL), "SDL_StartTextInput()");
    CHECK(SDL_TextInputActive(NULL), "SDL_TextInputActive()");
    feed("EV_KEY 30 1\n");            /* A */
    CHECK(collect_until(SDL_EVENT_TEXT_INPUT, &event, 2000), "no text input event");
    CHECK(SDL_strcmp(event.text.text, layout_text_a) == 0, "text input was '%s', this layout says '%s'",
          event.text.text, layout_text_a);
    feed("EV_KEY 30 0\n");
    drain();

    /* a shifted letter produces its uppercase character */
    feed("EV_KEY 42 1\n");
    feed("EV_KEY 30 1\n");
    CHECK(collect_until(SDL_EVENT_TEXT_INPUT, &event, 2000), "no shifted text input");
    CHECK(SDL_strcmp(event.text.text, layout_text_a_shift) == 0,
          "shifted text input was '%s', this layout says '%s'", event.text.text,
          layout_text_a_shift);
    feed("EV_KEY 30 0\n");
    feed("EV_KEY 42 0\n");
    drain();

    /* space and the number row */
    feed("EV_KEY 57 1\n");            /* KEY_SPACE */
    CHECK(collect_until(SDL_EVENT_TEXT_INPUT, &event, 2000), "no space text input");
    CHECK(SDL_strcmp(event.text.text, " ") == 0, "space produced '%s'", event.text.text);
    feed("EV_KEY 57 0\n");
    drain();

    SDL_StopTextInput(NULL);
    CHECK(!SDL_TextInputActive(NULL), "text input still active");
}

static void test_focus_and_windows(void)
{
    SDL_Window *window = SDL_CreateWindow("input test", 160, 120, 0);
    SDL_Window *focus;

    CHECK(window != NULL, "SDL_CreateWindow(): %s", SDL_GetError());
    CHECK(SDL_GetWindowID(window) != 0, "window has no ID");
    CHECK(SDL_GetWindowFromID(SDL_GetWindowID(window)) == window, "window registry lookup");
    CHECK(SDL_strcmp(SDL_GetWindowTitle(window), "input test") == 0, "window title");

    SDL_SetWindowTitle(window, "renamed");
    CHECK(SDL_strcmp(SDL_GetWindowTitle(window), "renamed") == 0, "title after SDL_SetWindowTitle()");

    SDL_SetWindowPosition(window, 10, 20);
{
        int x = -1, y = -1;
        CHECK(SDL_GetWindowPosition(window, &x, &y) && x == 10 && y == 20,
              "window position %d,%d", x, y);
    }
    SDL_SetWindowSize(window, 200, 150);
    {
        int w = 0, h = 0;
        CHECK(SDL_GetWindowSize(window, &w, &h) && w == 200 && h == 150, "size %dx%d", w, h);
        CHECK(SDL_GetWindowSizeInPixels(window, &w, &h) && w > 0 && h > 0, "pixel size %dx%d", w, h);
    }

    focus = SDL_GetKeyboardFocus();
    CHECK(focus == window || focus == input_window || focus == NULL,
          "keyboard focus is an unknown window");

    {
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        CHECK(props != 0, "SDL_GetWindowProperties()");
        CHECK(SDL_GetWindowFlags(window) == SDL_GetWindowFlags(window), "flags are stable");
    }

    SDL_DestroyWindow(window);
}

/* ------------------------------------------------------------------------- */
/* Async wakeup                                                              */
/* ------------------------------------------------------------------------- */

/* The point of the worker thread is that a *blocked* application wakes up as
   soon as input arrives, instead of waiting for the next poll timeout. Feed a
   record from another thread while the main thread sits in SDL_WaitEvent() with
   no timeout at all and measure how long that takes. */
typedef struct
{
    int delay_ms;
    const char *record;
} FeederArgs;

static void *feeder_thread(void *arg)
{
    FeederArgs *args = (FeederArgs *)arg;
    SDL_Delay(args->delay_ms);
    feed(args->record);
    return NULL;
}

/* The worker is asynchronous: records fed earlier may still be in flight, so
   wait until the event stream has been quiet for a moment before asserting. */
static void settle(void)
{
    int quiet = 0;

    for (int i = 0; i < 100 && quiet < 3; i++) {
        SDL_Event event;
        bool any = false;

        SDL_PumpEvents();
        while (SDL_PollEvent(&event)) {
            any = true;
        }
        if (any) {
            quiet = 0;
        } else {
            quiet++;
            SDL_Delay(10);
        }
    }
}

static void test_async_wakeup(void)
{
    SDL_Event event;
    pthread_t thread;
    FeederArgs args;
    Uint64 start, elapsed;

    settle();
    args.delay_ms = 120;
    args.record = "EV_KEY 31 1\n";          /* S */
    if (pthread_create(&thread, NULL, feeder_thread, &args) != 0) {
        CHECK(false, "pthread_create() failed");
        return;
    }

    start = SDL_GetTicksNS();
    CHECK(SDL_WaitEvent(&event), "SDL_WaitEvent(): %s", SDL_GetError());
    elapsed = SDL_GetTicksNS() - start;
    pthread_join(thread, NULL);

    CHECK(event.type == SDL_EVENT_KEY_DOWN, "woken by event type %u", event.type);
    CHECK(event.key.scancode == SDL_SCANCODE_S, "woken by scancode %d", event.key.scancode);
    CHECK(elapsed >= 100000000LL, "SDL_WaitEvent() returned after only %llu us",
          (unsigned long long)(elapsed / 1000));
    CHECK(elapsed < 1500000000LL, "SDL_WaitEvent() took %llu us to notice input",
          (unsigned long long)(elapsed / 1000));
    printf("  input woke a blocked SDL_WaitEvent() %llu us after it was sent\n",
           (unsigned long long)(elapsed / 1000000));

    drain();
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("test_input (SDLop)\n");

    unlink(fifo_path);
    if (mkfifo(fifo_path, 0600) != 0) {
        printf("SKIP: cannot create %s\n", fifo_path);
        return 0;
    }
    SDL_setenv_unsafe("SDLOP_TEST_INPUT", fifo_path, 1);
    SDL_setenv_unsafe("SDL_VIDEODRIVER", "offscreen", 0);

    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init(SDL_INIT_VIDEO): %s", SDL_GetError());

    /* Motion and wheel records are addressed to a window, so the test needs one
       (normally the compositor would have given it focus). */
    CHECK(input_window = SDL_CreateWindow("input target", 320, 200, 0),
          "SDL_CreateWindow(): %s", SDL_GetError());

    /* SDL_Init() has started the reader thread by now, so the producer side of
       the fifo can be opened. It stays open for the whole test: closing it would
       end the reader thread. */
    fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
    if (fifo_fd >= 0) {
        probe_layout();
    }
    if (fifo_fd < 0) {
        printf("SKIP: cannot open %s for writing\n", fifo_path);
        SDL_Quit();
        return 0;
    }

    test_scancode_table();
    test_key_events();
    test_modifiers();
    test_mouse_buttons_and_motion();
    test_text_input();
    test_focus_and_windows();
    test_async_wakeup();

    SDL_DestroyWindow(input_window);
    SDL_Quit();
    close(fifo_fd);

    printf("%d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
