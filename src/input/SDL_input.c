/*
  SDLop -- the input core: raw evdev records in, SDL events out.

  The shape of the design (this is the "asyncinput" model):

      input thread(s)                     main thread
      -----------------------------       -----------------------------------
      read() /dev/input/event*      -->   SDLOP_PumpRawInput()
      (or an X11/XWayland event)          -> translate record -> SDL event
      push 64-byte record into            -> SDLOP_PushEvent()
      the lock-free ring                  -> wake SDL_WaitEvent()

  The worker never allocates, never takes a lock and never touches SDL state, so
  it cannot be delayed by the application; the ring is a plain SPSC ring with an
  atomic head/tail pair, padded to a cache line so producer and consumer do not
  share one. Kernel timestamps are carried through, so an application can measure
  input-to-photon latency with SDL_GetTicksNS() - event.key.timestamp.

  Translation itself is on the main thread, which is where SDL's state lives
  (keystate[], modstate, window focus) and where the cost is a few branches per
  record. A full 64-event read costs a handful of microseconds.
*/

#include "../sdlop_internal.h"
#include "sdlop_evdev.h"
#include "sdlop_keynames.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>

#ifdef SDLOP_HAVE_LINUX_INPUT
#include <linux/input.h>
#else
/* Minimal evdev constants so the translation switch compiles everywhere. */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define SYN_REPORT 0
#define SYN_DROPPED 3
#define REL_X 0x00
#define REL_Y 0x01
#define REL_WHEEL 0x08
#define REL_HWHEEL 0x06
#define REL_WHEEL_HI_RES 0x0b
#define REL_HWHEEL_HI_RES 0x0c
#define ABS_X 0x00
#define ABS_Y 0x01
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define BTN_SIDE 0x113
#define BTN_EXTRA 0x114
#define BTN_FORWARD 0x115
#define BTN_BACK 0x116
#define BTN_TASK 0x117
#define KEY_ESC 1
#define KEY_MAX 0x2ff
#endif

/* ------------------------------------------------------------------------- */
/* evdev keycode -> SDL scancode                                             */
/*                                                                            */
/* The table is data, so it is generated: tools/gen_evdev.py turns upstream    */
/* SDL3's src/events/scancodes_linux.h into src/generated/sdlop_evdev.h. Using  */
/* upstream's table is what makes the scancodes of every key (keypad, media     */
/* keys, international keys) identical to SDL3's.                              */
/* ------------------------------------------------------------------------- */

SDL_Scancode SDLOP_ScancodeFromEvdevKeycode(Uint32 evdev_code)
{
    if (evdev_code < SDLOP_EVDEV_KEYCODE_COUNT) {
        return sdlop_scancode_from_evdev[evdev_code];
    }
    return SDL_SCANCODE_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
/* Lock-free SPSC ring of raw records                                        */
/* ------------------------------------------------------------------------- */

typedef struct SDLOP_RawRing
{
    _Atomic Uint32 head;                 /* written by the input thread */
    _Atomic Uint32 tail;                 /* written by the main thread */
    _Atomic Uint64 overruns;
    char pad[48];
    SDLOP_RawInputRecord records[SDLOP_RAW_QUEUE_SIZE];
} SDLOP_RawRing;

static SDLOP_RawRing *sdlop_ring;
static bool sdlop_async_input_active;
static bool sdlop_test_input_active;
static int sdlop_test_input_fd = -1;
static char *sdlop_test_input_path;
static Uint32 sdlop_mouse_device_id = 1;

bool SDLOP_AsyncInputActive(void)
{
    return sdlop_async_input_active || sdlop_test_input_active;
}

bool SDLOP_PushRawInput(const SDLOP_RawInputRecord *record)
{
    Uint32 head = atomic_load_explicit(&sdlop_ring->head, memory_order_relaxed);
    Uint32 next = (head + 1) % SDLOP_RAW_QUEUE_SIZE;

    if (next == atomic_load_explicit(&sdlop_ring->tail, memory_order_acquire)) {
        atomic_fetch_add_explicit(&sdlop_ring->overruns, 1, memory_order_relaxed);
        return false;                /* ring full: prefer dropping to blocking */
    }
    sdlop_ring->records[head] = *record;
    atomic_store_explicit(&sdlop_ring->head, next, memory_order_release);
    return true;
}

int SDLOP_DrainRawInput(SDLOP_RawInputRecord *out, int max_records)
{
    int count = 0;
    Uint32 tail = atomic_load_explicit(&sdlop_ring->tail, memory_order_relaxed);

    while (count < max_records) {
        Uint32 head = atomic_load_explicit(&sdlop_ring->head, memory_order_acquire);
        if (tail == head) {
            break;
        }
        out[count++] = sdlop_ring->records[tail];
        tail = (tail + 1) % SDLOP_RAW_QUEUE_SIZE;
    }
    atomic_store_explicit(&sdlop_ring->tail, tail, memory_order_release);
    return count;
}

Uint64 SDLOP_RawInputOverruns(void)
{
    return atomic_load_explicit(&sdlop_ring->overruns, memory_order_relaxed);
}

void SDLOP_MaybeInitRawInput(void)
{
    if (!sdlop_ring) {
        sdlop_ring = (SDLOP_RawRing *)calloc(1, sizeof(SDLOP_RawRing));
    }
}

/* ------------------------------------------------------------------------- */
/* Text input synthesis                                                      */
/* ------------------------------------------------------------------------- */

/* SDL_Keycode -> UTF-8, for the text-input path. SDLop derives text
   from the keycode (like SDL does when no IME is involved).

   The text a queued event points at has to outlive the push (the application may
   read it much later), so it is copied into a small ring of fixed buffers - the
   same trick SDL3 uses for its text/drop event strings. 16 pending text events
   are enough for any sane key repeat rate. */
#define SDLOP_TEXT_POOL_SLOTS 16
static char sdlop_text_pool[SDLOP_TEXT_POOL_SLOTS][SDL_TEXTINPUTEVENT_TEXT_SIZE];
static unsigned int sdlop_text_pool_next;

static char *sdlop_text_storage(const char *utf8)
{
    char *slot = sdlop_text_pool[sdlop_text_pool_next];
    sdlop_text_pool_next = (sdlop_text_pool_next + 1) % SDLOP_TEXT_POOL_SLOTS;
    SDL_strlcpy(slot, utf8, SDL_TEXTINPUTEVENT_TEXT_SIZE);
    return slot;
}
/* Push SDL_EVENT_TEXT_INPUT with `utf8` (which must outlive the push only until
   this call returns: it is copied into the text ring). */
static void sdlop_push_text(const char *utf8, Uint64 timestamp)
{
    SDL_Event event;

    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.timestamp = timestamp;
    event.text.windowID = 0;
    event.text.text = sdlop_text_storage(utf8);
    SDLOP_PushEvent(&event);
}

void SDLOP_HandleTextInputForKey(SDL_Scancode scancode, SDL_Keycode key, Uint32 evdev_code,
                                 bool down, Uint64 timestamp)
{
    char utf8[32];
    const SDLOP_KeyLayout *layout = SDLOP_GetKeyLayout();
    int len = 0;

    (void)scancode;
    if (!down) {
        return;
    }
    if (!SDL_TextInputActive(NULL)) {
        return;
    }
    /* SDL3 sends no text while Ctrl or Alt is held: Ctrl+C is a shortcut, not the
       letter "c". */
    if (SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) {
        return;
    }

    /* The compositor's keymap knows what the key really types (dead keys and
       non-US layouts included), so ask it first. */
    if (layout && layout->text_from_evdev && evdev_code) {
        len = layout->text_from_evdev(evdev_code, utf8, sizeof(utf8));
        if (len > 0 && ((unsigned char)utf8[0] < 0x20 || (unsigned char)utf8[0] == 0x7F)) {
            len = 0;                   /* Enter, Tab, Backspace: no text */
        }
    }
    if (len > 0) {
        sdlop_push_text(utf8, timestamp);
        return;
    }
    if (layout && layout->text_from_evdev) {
        /* The layout is the authority on what a key types: when it says "nothing"
           (a modifier, a dead key, a key outside the layout) then inventing text
           from the SDL keycode would produce nonsense like the Arabic letter that
           0xFEE1 encodes. This is also what SDL3 does - text comes from the
           keymap or not at all. */
        return;
    }

    /* No layout at all: derive the text from the keycode, which is what SDL3 does
       when xkbcommon is not available. */

    /* Only characters produce text: modifier keys, function keys and the other
       extended keycodes (everything at or above SDLK_SCANCODE_MASK) must not, and
       neither must control characters or keys outside Unicode. */
    if (key < 0x20 || key == SDLK_DELETE || key >= SDLK_SCANCODE_MASK || key > 0x10FFFF) {
        return;
    }
    if (key <= 0x7F) {
        utf8[len++] = (char)key;
    } else if (key <= 0x7FF) {
        utf8[len++] = (char)(0xC0 | (key >> 6));
        utf8[len++] = (char)(0x80 | (key & 0x3F));
    } else if (key <= 0xFFFF) {
        utf8[len++] = (char)(0xE0 | (key >> 12));
        utf8[len++] = (char)(0x80 | ((key >> 6) & 0x3F));
        utf8[len++] = (char)(0x80 | (key & 0x3F));
    } else {
        utf8[len++] = (char)(0xF0 | (key >> 18));
        utf8[len++] = (char)(0x80 | ((key >> 12) & 0x3F));
        utf8[len++] = (char)(0x80 | ((key >> 6) & 0x3F));
        utf8[len++] = (char)(0x80 | (key & 0x3F));
    }
    utf8[len] = '\0';
    sdlop_push_text(utf8, timestamp);
}

/* ------------------------------------------------------------------------- */
/* Raw record -> SDL events                                                  */
/* ------------------------------------------------------------------------- */

static Uint8 sdlop_mouse_button_for_evdev(Uint16 code)
{
    switch (code) {
        case BTN_LEFT: return SDL_BUTTON_LEFT;
        case BTN_RIGHT: return SDL_BUTTON_RIGHT;
        case BTN_MIDDLE: return SDL_BUTTON_MIDDLE;
        case BTN_SIDE: return SDL_BUTTON_X1;
        case BTN_EXTRA: case BTN_FORWARD: case BTN_BACK: case BTN_TASK: return SDL_BUTTON_X2;
        default: return 0;
    }
}

/* Which window the raw records belong to: the one with mouse focus, else the one
   with keyboard focus, else the only window there is. Without this a headless
   or single-window application would never see pointer motion. */
SDL_Window *SDLOP_InputTargetWindow(void)
{
    SDL_Window *window = SDLOP_GetMouseFocusWindow();
    int num_windows = 0;
    SDL_Window *only = NULL;

    if (window) {
        return window;
    }
    window = SDLOP_GetKeyboardFocusWindow();
    if (window) {
        return window;
    }
    {
        SDL_Window **windows = SDL_GetWindows(&num_windows);
        if (windows && num_windows == 1) {
            only = windows[0];
        }
        SDL_free(windows);
    }
    return only;
}

static void sdlop_translate_record(const SDLOP_RawInputRecord *record)
{
    Uint64 timestamp = record->timestamp_ns;
    SDL_Window *window = SDLOP_InputTargetWindow();

    switch (record->type) {
        case EV_KEY: {
            Uint8 button = sdlop_mouse_button_for_evdev(record->code);
            if (button) {
                float x = -1.0f, y = -1.0f;
                if (window) {
                    SDL_GetMouseState(&x, &y);
                }
                SDLOP_SendMouseButton(record->device_id ? record->device_id : sdlop_mouse_device_id,
                                      button, record->value != 0, x, y, timestamp);
                return;
            }
            {
                SDL_Scancode scancode = SDLOP_ScancodeFromEvdevKeycode(record->code);
                const SDLOP_KeyLayout *layout = SDLOP_GetKeyLayout();
                if (scancode == SDL_SCANCODE_UNKNOWN) {
                    return;
                }
                /* Keep a platform layout's state (shift, caps lock, dead keys) in
                   step with the keys we are actually seeing. */
                if (layout && layout->update_key) {
                    layout->update_key(record->code, record->value != 0);
                }
                SDLOP_SendKeyEvent(scancode, SDLK_UNKNOWN, SDL_KMOD_NONE,
                                   record->value != 0, record->value == 2, timestamp, record->code);
            }
            return;
        }

        case EV_REL:
            if (!window) {
                return;
            }
            switch (record->code) {
                case REL_X:
                case REL_Y: {
                    float dx = 0.0f, dy = 0.0f;
                    static int pending_x, pending_y;
                    if (record->code == REL_X) {
                        pending_x += record->value;
                    } else {
                        pending_y += record->value;
                    }
                    dx = (float)pending_x;
                    dy = (float)pending_y;
                    pending_x = pending_y = 0;
                    if (dx != 0.0f || dy != 0.0f) {
                        SDLOP_SendMouseMotionRelative(window->id, dx * window->display_scale,
                                                      dy * window->display_scale, timestamp);
                    }
                    return;
                }
                case REL_WHEEL:
                    SDLOP_SendMouseWheel(window->id, 0.0f, (float)record->value,
                                         SDL_MOUSEWHEEL_NORMAL, timestamp);
                    return;
                case REL_HWHEEL:
                    SDLOP_SendMouseWheel(window->id, (float)record->value, 0.0f,
                                         SDL_MOUSEWHEEL_NORMAL, timestamp);
                    return;
                default:
                    return;
            }

        case EV_ABS:
            if (!window) {
                return;
            }
            switch (record->code) {
                case ABS_X:
                case ABS_Y: {
                    static int abs_x, abs_y;
                    if (record->code == ABS_X) {
                        abs_x = record->value;
                    } else {
                        abs_y = record->value;
                    }
                    SDLOP_SendMouseMotionAbsolute(window->id, (float)abs_x, (float)abs_y, timestamp);
                    return;
                }
                default:
                    return;
            }

        default:
            return;                    /* EV_SYN/EV_MSC carry no SDL event */
    }
}

/* Translate everything the input sources produced. Runs on the main thread
   inside SDL_PumpEvents(); backends may also call it right after they filled the
   ring to keep the queue from overflowing. */
void SDLOP_PumpRawInput(void)
{
    SDLOP_RawInputRecord records[64];
    int num, i;

    if (!sdlop_ring) {
        return;
    }
    for (;;) {
        num = SDLOP_DrainRawInput(records, (int)(sizeof(records) / sizeof(records[0])));
        if (num <= 0) {
            return;
        }
        for (i = 0; i < num; i++) {
            sdlop_translate_record(&records[i]);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Device bookkeeping                                                        */
/* ------------------------------------------------------------------------- */

void SDLOP_OnDeviceAdded(Uint32 device_id, SDLOP_InputDeviceClass cls, const char *name)
{
    SDL_Event event;
    Uint64 timestamp = SDL_GetTicksNS();

    if (cls == SDLOP_DEVICE_MOUSE) {
        sdlop_mouse_device_id = device_id;
    }
    memset(&event, 0, sizeof(event));
    event.type = (cls == SDLOP_DEVICE_MOUSE) ? SDL_EVENT_MOUSE_ADDED
                                             : SDL_EVENT_KEYBOARD_ADDED;
    event.common.timestamp = timestamp;
    if (cls == SDLOP_DEVICE_MOUSE) {
        event.mdevice.which = device_id;
        event.mdevice.timestamp = timestamp;
    } else {
        event.kdevice.which = device_id;
        event.kdevice.timestamp = timestamp;
    }
    SDLOP_PushEvent(&event);
    if (name) {
        SDLOP_LogDebug("sdlop: input device %u (%s) added", device_id, name);
    }
}

void SDLOP_OnDeviceRemoved(Uint32 device_id)
{
    SDL_Event event;
    Uint64 timestamp = SDL_GetTicksNS();

    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_KEYBOARD_REMOVED;
    event.common.timestamp = timestamp;
    SDLOP_PushEvent(&event);
    (void)device_id;
}

/* ------------------------------------------------------------------------- */
/* Test input: SDLOP_TEST_INPUT=<fifo>                                       */
/*                                                                            */
/* CI machines and containers have no /dev/input, so the test suite feeds      */
/* records through a FIFO with the same text format evtest prints. It is the   */
/* same path a real device takes, minus the kernel.                            */
/* ------------------------------------------------------------------------- */

static void *sdlop_test_input_thread(void *arg)
{
    (void)arg;
    for (;;) {
        char line[256];
        size_t len = 0;
        ssize_t rc;

        /* read one line from the fifo (blocking) */
        while (len < sizeof(line) - 1) {
            rc = read(sdlop_test_input_fd, line + len, 1);
            if (rc == 0) {
                SDL_Delay(1);                  /* no writer connected yet */
                continue;
            }
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN) {
                    SDL_Delay(1);
                    continue;
                }
                return NULL;
            }
            if (line[len] == '\n') {
                break;
            }
            len++;
        }
        line[len] = '\0';

        {
            char kind[32];
            long long ts = 0;
            long device = 0;
            unsigned code = 0;
            long value = 0;
            SDLOP_RawInputRecord record;

            if (line[0] == '#' || len == 0) {
                continue;
            }
            /* format: <type> <code> <value> [device=<n>] [ts=<ns>] */
            if (SDL_sscanf(line, "%31s %u %ld %ld %lld", kind, &code, &value, &device, &ts) < 3) {
                continue;
            }
            memset(&record, 0, sizeof(record));
            if (ts > 0) {
                record.timestamp_ns = (Uint64)ts;
            } else {
                record.timestamp_ns = SDL_GetTicksNS();
            }
            record.device_id = (Uint32)(device > 0 ? device : 1);
            record.from_evdev = false;

            if (SDL_strcasecmp(kind, "EV_KEY") == 0 || SDL_strcasecmp(kind, "KEY") == 0) {
                record.type = EV_KEY;
                record.value = (Sint32)value;
                record.device_class = sdlop_mouse_button_for_evdev((Uint16)code)
                                          ? SDLOP_DEVICE_MOUSE : SDLOP_DEVICE_KEYBOARD;
            } else if (SDL_strcasecmp(kind, "EV_REL") == 0 || SDL_strcasecmp(kind, "REL") == 0) {
                record.type = EV_REL;
                record.value = (Sint32)value;
                record.device_class = SDLOP_DEVICE_MOUSE;
            } else if (SDL_strcasecmp(kind, "EV_ABS") == 0 || SDL_strcasecmp(kind, "ABS") == 0) {
                record.type = EV_ABS;
                record.value = (Sint32)value;
                record.device_class = SDLOP_DEVICE_MOUSE;
            } else {
                continue;
            }
            record.code = (Uint16)code;
            SDLOP_PushRawInput(&record);
            SDLOP_SignalEvents();
        }
    }
}

bool SDLOP_InitTestInput(const char *path)
{
    pthread_t thread;

    SDLOP_MaybeInitRawInput();
    /* Opened read-write on purpose: a FIFO that only has a reader reports EOF as
       soon as the writer goes away, and this thread must survive that (the test
       opens and closes its writer several times). */
    sdlop_test_input_fd = open(path, O_RDWR | O_NONBLOCK);
    if (sdlop_test_input_fd < 0) {
        return SDL_SetError("Couldn't open test input fifo '%s': %s", path, strerror(errno));
    }
    sdlop_test_input_path = SDL_strdup(path);
    sdlop_test_input_active = true;
    sdlop_async_input_active = true;
    if (pthread_create(&thread, NULL, sdlop_test_input_thread, NULL) != 0) {
        sdlop_test_input_active = false;
        close(sdlop_test_input_fd);
        sdlop_test_input_fd = -1;
        return SDL_SetError("Couldn't start test input thread");
    }
    pthread_detach(thread);
    SDLOP_LogInfo("sdlop: reading test input from %s", path);
    return true;
}

void SDLOP_QuitTestInput(void)
{
    if (sdlop_test_input_fd >= 0) {
        close(sdlop_test_input_fd);
        sdlop_test_input_fd = -1;
    }
    sdlop_test_input_active = false;
    SDLOP_Free(sdlop_test_input_path);
    sdlop_test_input_path = NULL;
}
