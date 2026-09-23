/*
  SDLop - event queue.

  Fixed-capacity ring of SDL_Event structures. No allocation on the hot
  path, single-threaded (main thread) by contract - input arrives through
  the lock-free raw ring / window system fd and is translated in
  SDL_PumpEvents().

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include <poll.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Queue storage                                                       */
/* ------------------------------------------------------------------ */

#define SDLOP_EVENT_QUEUE_SIZE 128 /* power of two */
#define SDLOP_TEXT_SIZE 64

typedef struct SDLOP_EventSlot
{
    SDL_Event event;
    char textbuf[SDLOP_TEXT_SIZE]; /* backing store for SDL_EVENT_TEXT_INPUT */
} SDLOP_EventSlot;

static SDLOP_EventSlot event_queue[SDLOP_EVENT_QUEUE_SIZE];
static Uint32 queue_head; /* next write index */
static Uint32 queue_tail; /* next read index */

static inline Uint32 queue_count(void)
{
    return queue_head - queue_tail;
}

bool SDLOP_PushEventInternal(SDL_Event *event)
{
    if (queue_count() >= SDLOP_EVENT_QUEUE_SIZE) {
        return SDL_SetError("Event queue is full (%d events)", SDLOP_EVENT_QUEUE_SIZE);
    }
    SDLOP_EventSlot *slot = &event_queue[queue_head & (SDLOP_EVENT_QUEUE_SIZE - 1)];
    slot->event = *event;
    if (event->type == SDL_EVENT_TEXT_INPUT && event->text.text) {
        /* copy text into slot-owned storage (SDL3 semantics: pointer is
         * transient and owned by SDL) */
        size_t len = strlen(event->text.text);
        if (len >= SDLOP_TEXT_SIZE) {
            len = SDLOP_TEXT_SIZE - 1;
        }
        memcpy(slot->textbuf, event->text.text, len);
        slot->textbuf[len] = '\0';
        slot->event.text.text = slot->textbuf;
    }
    queue_head++;
    return true;
}

void SDLOP_SendWindowEvent(SDL_Window *window, SDL_EventType type, Sint32 data1, Sint32 data2)
{
    SDL_Event event;
    SDL_zero(event);
    event.window.type = type;
    event.window.timestamp = SDL_GetTicksNS();
    event.window.windowID = window ? window->id : 0;
    event.window.data1 = data1;
    event.window.data2 = data2;
    SDLOP_PushEventInternal(&event);
}

void SDLOP_SendQuitEvent(void)
{
    SDL_Event event;
    SDL_zero(event);
    event.quit.type = SDL_EVENT_QUIT;
    event.quit.timestamp = SDL_GetTicksNS();
    SDLOP_PushEventInternal(&event);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void SDL_PumpEvents(void)
{
    /* 1. drain raw input ring -> SDL events */
    SDLOP_RawInputPump();

    /* 2. drain window system (non-blocking) */
    if (sdlop.video) {
        sdlop.video->PumpEvents(sdlop.video, -1);
    }
}

bool SDL_HasEvent(Uint32 type)
{
    return SDL_HasEvents(type, type);
}

bool SDL_HasEvents(Uint32 minType, Uint32 maxType)
{
    for (Uint32 i = queue_tail; i != queue_head; i++) {
        Uint32 type = event_queue[i & (SDLOP_EVENT_QUEUE_SIZE - 1)].event.type;
        if (type >= minType && type <= maxType) {
            return true;
        }
    }
    return false;
}

void SDL_FlushEvent(Uint32 type)
{
    SDL_FlushEvents(type, type);
}

void SDL_FlushEvents(Uint32 minType, Uint32 maxType)
{
    /* compact queue, dropping matching events */
    Uint32 keep = queue_tail;
    for (Uint32 i = queue_tail; i != queue_head; i++) {
        SDLOP_EventSlot *src = &event_queue[i & (SDLOP_EVENT_QUEUE_SIZE - 1)];
        if (src->event.type >= minType && src->event.type <= maxType) {
            continue; /* drop */
        }
        if (keep != i) {
            event_queue[keep & (SDLOP_EVENT_QUEUE_SIZE - 1)] = *src;
        }
        keep++;
    }
    queue_head = keep;
}

bool SDL_PollEvent(SDL_Event *event)
{
    if (queue_tail == queue_head) {
        return false;
    }
    if (event) {
        *event = event_queue[queue_tail & (SDLOP_EVENT_QUEUE_SIZE - 1)].event;
    }
    queue_tail++;
    return true;
}

bool SDL_WaitEventTimeout(SDL_Event *event, Sint32 timeoutMS)
{
    const Uint64 deadline = (timeoutMS < 0) ? 0 : SDL_GetTicks() + (Uint64)timeoutMS;

    for (;;) {
        SDL_PumpEvents();
        if (SDL_PollEvent(event)) {
            return true;
        }
        if (timeoutMS >= 0 && SDL_GetTicks() >= deadline) {
            return false;
        }

        /* Wait for activity on the raw-input wake fd and/or the window
         * system fd. Small caps keep key-repeat timing accurate. */
        int wait_ms = 10;
        if (timeoutMS >= 0) {
            Sint64 left = (Sint64)(deadline - SDL_GetTicks());
            if (left <= 0) {
                continue; /* re-checks queue, then returns false */
            }
            if (left < wait_ms) {
                wait_ms = (int)left;
            }
        }

        int video_fd = sdlop.video ? sdlop.video->GetEventFD(sdlop.video) : -1;
        struct pollfd pfds[2];
        int nfds = 0;
        if (sdlop.wake_fd >= 0) {
            pfds[nfds].fd = sdlop.wake_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }
        if (video_fd >= 0) {
            pfds[nfds].fd = video_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }
        if (nfds > 0) {
            poll(pfds, (nfds_t)nfds, wait_ms);
            if (sdlop.wake_fd >= 0 && (pfds[0].revents & POLLIN)) {
                uint64_t val;
                (void)!read(sdlop.wake_fd, &val, sizeof(val)); /* reset */
            }
        } else {
            SDL_Delay((Uint32)wait_ms);
        }
    }
}

bool SDL_WaitEvent(SDL_Event *event)
{
    return SDL_WaitEventTimeout(event, -1);
}

bool SDL_PushEvent(SDL_Event *event)
{
    if (!event) {
        return SDL_SetError("SDL_PushEvent() with NULL event");
    }
    if (!event->common.timestamp) {
        event->common.timestamp = SDL_GetTicksNS();
    }
    return SDLOP_PushEventInternal(event);
}

Uint32 SDL_RegisterEvents(int numevents)
{
    if (numevents <= 0) {
        return 0;
    }
    if (sdlop.next_user_event == 0) {
        sdlop.next_user_event = SDL_EVENT_USER;
    }
    if ((Uint64)sdlop.next_user_event + (Uint64)numevents > SDL_EVENT_LAST) {
        return SDL_SetError("No user events remaining"), 0;
    }
    Uint32 base = sdlop.next_user_event;
    sdlop.next_user_event += (Uint32)numevents;
    return base;
}

bool SDL_QuitRequested(void)
{
    SDL_PumpEvents();
    return SDL_HasEvent(SDL_EVENT_QUIT);
}

SDL_Window *SDL_GetWindowFromEvent(const SDL_Event *event)
{
    if (!event) {
        return NULL;
    }
    SDL_WindowID id = 0;
    switch (event->type) {
    case SDL_EVENT_WINDOW_SHOWN ... SDL_EVENT_WINDOW_LAST:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        id = event->window.windowID; /* windowID is at the same offset in all of these */
        break;
    default:
        return NULL;
    }
    return id ? SDL_GetWindowFromID(id) : NULL;
}
