/*
  SDLop - event queue.

  Fixed-capacity ring of SDL_Event structures owned by the main thread: no
  allocation and no locking on the hot path. Input arrives through the
  lock-free raw ring / window system fd and is translated in
  SDL_PumpEvents().

  SDL3 semantics on top of that:
    - SDL_PushEvent() is thread-safe: other threads (timer callbacks, worker
      threads) append to a small locked side queue and wake the main thread
      through the eventfd; the main thread merges it in on the next pump /
      poll / peep, preserving order per producer.
    - the event filter and watchers run on the pushing thread, before the
      event is queued (SDL3 order: filter first, then watchers);
    - SDL_SetEventEnabled() drops internally generated events of disabled
      types and flushes pending ones;
    - SDL_PollEvent() pumps when the queue is empty, SDL_PeepEvents() never
      pumps;
    - SDL_RunOnMainThread() callbacks run inside SDL_PumpEvents().

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include <poll.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Temporary memory (SDL3): pointers carried by events (clipboard mime  */
/* type arrays) live on a per-thread list, move into the queue entry   */
/* when the event is pushed, move to the reader's list when it is      */
/* polled, and are freed at the next SDL_PumpEvents().                  */
/* ------------------------------------------------------------------ */

typedef struct TempMem
{
    void *memory;
    struct TempMem *next;
} TempMem;

static _Thread_local TempMem *thread_temp;

void *SDLOP_AllocateTemporaryMemory(size_t size)
{
    void *mem = SDL_malloc(size);
    if (!mem) {
        return NULL;
    }
    TempMem *e = (TempMem *)SDL_malloc(sizeof(*e));
    if (!e) {
        return mem; /* leak rather than crash */
    }
    e->memory = mem;
    e->next = thread_temp;
    thread_temp = e;
    return mem;
}

static void free_temp_list(TempMem *list)
{
    while (list) {
        TempMem *e = list;
        list = e->next;
        SDL_free(e->memory);
        SDL_free(e);
    }
}

void SDLOP_FreeTemporaryMemory(void)
{
    TempMem *list = thread_temp;
    thread_temp = NULL;
    free_temp_list(list);
}

/* unlink the pushing thread's entry for the event's pointer, if any */
static TempMem *claim_event_memory(const SDL_Event *event)
{
    const void *ptr = NULL;
    if (event->type == SDL_EVENT_CLIPBOARD_UPDATE) {
        ptr = event->clipboard.mime_types;
    }
    if (!ptr) {
        return NULL;
    }
    for (TempMem **link = &thread_temp; *link; link = &(*link)->next) {
        if ((*link)->memory == ptr) {
            TempMem *e = *link;
            *link = e->next;
            e->next = NULL;
            return e;
        }
    }
    return NULL;
}

static void give_memory_to_thread(TempMem *list)
{
    while (list) {
        TempMem *e = list;
        list = e->next;
        e->next = thread_temp;
        thread_temp = e;
    }
}

/* ------------------------------------------------------------------ */
/* Queue storage                                                       */
/* ------------------------------------------------------------------ */

/* Enough room for high-rate input accumulated between application frames.
 * Still bounded; input state translation continues when full, while input
 * notifications that cannot fit are dropped. */
#define SDLOP_EVENT_QUEUE_SIZE 8192 /* power of two */
#define SDLOP_TEXT_SIZE 64

typedef struct SDLOP_EventSlot
{
    SDL_Event event;
    char textbuf[SDLOP_TEXT_SIZE]; /* backing store for SDL_EVENT_TEXT_INPUT */
    TempMem *memory;               /* temporary memory owned by this event */
} SDLOP_EventSlot;

static SDLOP_EventSlot event_queue[SDLOP_EVENT_QUEUE_SIZE];
static Uint32 queue_head; /* next write index */
static Uint32 queue_tail; /* next read index */

static inline Uint32 queue_count(void)
{
    return queue_head - queue_tail;
}

Uint32 SDLOP_EventQueueFree(void)
{
    return SDLOP_EVENT_QUEUE_SIZE - queue_count();
}

static inline SDLOP_EventSlot *slot_at(Uint32 i)
{
    return &event_queue[i & (SDLOP_EVENT_QUEUE_SIZE - 1)];
}

/* copy an event into a slot, taking a private copy of TEXT_INPUT text
 * (SDL3: the text pointer is owned by SDL and valid until the next poll) */
static void store_event(SDLOP_EventSlot *slot, const SDL_Event *event)
{
    slot->event = *event;
    if (event->type == SDL_EVENT_TEXT_INPUT && event->text.text && event->text.text != slot->textbuf) {
        size_t len = strlen(event->text.text);
        if (len >= SDLOP_TEXT_SIZE) {
            len = SDLOP_TEXT_SIZE - 1;
        }
        memmove(slot->textbuf, event->text.text, len);
        slot->textbuf[len] = '\0';
        slot->event.text.text = slot->textbuf;
    }
}

static bool queue_add_with_memory(const SDL_Event *event, TempMem *memory)
{
    if (queue_count() >= SDLOP_EVENT_QUEUE_SIZE) {
        give_memory_to_thread(memory); /* freed at the next pump */
        return SDL_SetError("Event queue is full (%d events)", SDLOP_EVENT_QUEUE_SIZE);
    }
    SDLOP_EventSlot *slot = slot_at(queue_head);
    store_event(slot, event);
    slot->memory = memory;
    queue_head++;
    return true;
}

static bool queue_add(const SDL_Event *event)
{
    return queue_add_with_memory(event, claim_event_memory(event));
}

/* an event leaves the queue: its memory goes to the reading thread */
static void release_slot(SDLOP_EventSlot *slot)
{
    give_memory_to_thread(slot->memory);
    slot->memory = NULL;
}

/* remove entry i (absolute index) keeping order */
static void queue_cut(Uint32 i)
{
    for (Uint32 j = i; j + 1 != queue_head; j++) {
        SDLOP_EventSlot *dst = slot_at(j);
        SDLOP_EventSlot *src = slot_at(j + 1);
        *dst = *src;
        if (dst->event.type == SDL_EVENT_TEXT_INPUT && src->event.text.text == src->textbuf) {
            dst->event.text.text = dst->textbuf;
        }
    }
    queue_head--;
}

/* ------------------------------------------------------------------ */
/* Cross-thread side queue                                             */
/* ------------------------------------------------------------------ */

#define SDLOP_SIDE_QUEUE_MAX 4096

typedef struct SideEvent
{
    SDL_Event event;
    char text[SDLOP_TEXT_SIZE];
    TempMem *memory;
} SideEvent;

static pthread_mutex_t side_lock = PTHREAD_MUTEX_INITIALIZER;
static SideEvent *side_queue;
static int side_count, side_capacity;
static _Atomic int side_pending;

static void wake_main_thread(void)
{
    if (sdlop.wake_fd >= 0) {
        uint64_t one = 1;
        (void)!write(sdlop.wake_fd, &one, sizeof(one));
    }
}

static bool side_push(const SDL_Event *event)
{
    pthread_mutex_lock(&side_lock);
    if (side_count >= SDLOP_SIDE_QUEUE_MAX) {
        pthread_mutex_unlock(&side_lock);
        return SDL_SetError("Event queue is full");
    }
    if (side_count == side_capacity) {
        int cap = side_capacity ? side_capacity * 2 : 64;
        SideEvent *q = (SideEvent *)realloc(side_queue, sizeof(SideEvent) * (size_t)cap);
        if (!q) {
            pthread_mutex_unlock(&side_lock);
            return SDL_OutOfMemory();
        }
        side_queue = q;
        side_capacity = cap;
    }
    SideEvent *se = &side_queue[side_count++];
    se->event = *event;
    se->memory = claim_event_memory(event);
    if (event->type == SDL_EVENT_TEXT_INPUT && event->text.text) {
        SDL_strlcpy(se->text, event->text.text, sizeof(se->text));
        se->event.text.text = se->text;
    }
    side_pending = side_count;
    pthread_mutex_unlock(&side_lock);
    wake_main_thread();
    return true;
}

/* main thread: move side-queued events into the ring */
static void merge_side_queue(void)
{
    if (!side_pending) {
        return;
    }
    pthread_mutex_lock(&side_lock);
    int i = 0;
    for (; i < side_count; i++) {
        if (queue_count() >= SDLOP_EVENT_QUEUE_SIZE) {
            break; /* keep the rest for later */
        }
        queue_add_with_memory(&side_queue[i].event, side_queue[i].memory);
    }
    if (i > 0) {
        memmove(side_queue, side_queue + i, sizeof(SideEvent) * (size_t)(side_count - i));
        side_count -= i;
        for (int k = 0; k < side_count; k++) {
            if (side_queue[k].event.type == SDL_EVENT_TEXT_INPUT && side_queue[k].event.text.text) {
                side_queue[k].event.text.text = side_queue[k].text;
            }
        }
    }
    side_pending = side_count;
    pthread_mutex_unlock(&side_lock);
}

/* ------------------------------------------------------------------ */
/* Filter / watchers                                                   */
/* ------------------------------------------------------------------ */

typedef struct EventWatcher
{
    SDL_EventFilter callback;
    void *userdata;
    bool removed;
} EventWatcher;

static pthread_mutex_t watch_lock;
static pthread_once_t watch_once = PTHREAD_ONCE_INIT;
static EventWatcher event_filter;
static EventWatcher *watchers;
static int num_watchers;
static int dispatch_depth;     /* removal while dispatching is deferred */
static bool watchers_dirty;

static void init_watch_lock(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&watch_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void watch_lock_acquire(void)
{
    pthread_once(&watch_once, init_watch_lock);
    pthread_mutex_lock(&watch_lock);
}

static void compact_watchers(void)
{
    int n = 0;
    for (int i = 0; i < num_watchers; i++) {
        if (!watchers[i].removed) {
            watchers[n++] = watchers[i];
        }
    }
    num_watchers = n;
    watchers_dirty = false;
}

/* returns false when the filter dropped the event */
static bool dispatch_watchers(SDL_Event *event)
{
    if (!event_filter.callback && !num_watchers) {
        return true; /* lock-free fast path: nothing registered */
    }
    bool keep = true;
    watch_lock_acquire();
    if (event_filter.callback && !event_filter.callback(event_filter.userdata, event)) {
        keep = false;
    } else if (num_watchers) {
        dispatch_depth++;
        for (int i = 0; i < num_watchers; i++) {
            if (!watchers[i].removed) {
                watchers[i].callback(watchers[i].userdata, event);
            }
        }
        if (--dispatch_depth == 0 && watchers_dirty) {
            compact_watchers();
        }
    }
    pthread_mutex_unlock(&watch_lock);
    return keep;
}

void SDL_SetEventFilter(SDL_EventFilter filter, void *userdata)
{
    watch_lock_acquire();
    event_filter.callback = filter;
    event_filter.userdata = userdata;
    pthread_mutex_unlock(&watch_lock);
    if (filter) {
        /* SDL3: cut currently queued events the new filter rejects */
        SDL_FilterEvents(filter, userdata);
    }
}

bool SDL_GetEventFilter(SDL_EventFilter *filter, void **userdata)
{
    watch_lock_acquire();
    SDL_EventFilter f = event_filter.callback;
    void *u = event_filter.userdata;
    pthread_mutex_unlock(&watch_lock);
    if (filter) {
        *filter = f;
    }
    if (userdata) {
        *userdata = u;
    }
    return f != NULL;
}

bool SDL_AddEventWatch(SDL_EventFilter filter, void *userdata)
{
    if (!filter) {
        return SDL_InvalidParamError("filter");
    }
    watch_lock_acquire();
    EventWatcher *w = (EventWatcher *)SDL_realloc(watchers, sizeof(EventWatcher) * (size_t)(num_watchers + 1));
    if (!w) {
        pthread_mutex_unlock(&watch_lock);
        return false;
    }
    watchers = w;
    watchers[num_watchers].callback = filter;
    watchers[num_watchers].userdata = userdata;
    watchers[num_watchers].removed = false;
    num_watchers++;
    pthread_mutex_unlock(&watch_lock);
    return true;
}

void SDL_RemoveEventWatch(SDL_EventFilter filter, void *userdata)
{
    watch_lock_acquire();
    for (int i = 0; i < num_watchers; i++) {
        if (!watchers[i].removed && watchers[i].callback == filter && watchers[i].userdata == userdata) {
            watchers[i].removed = true;
            watchers_dirty = true;
            break;
        }
    }
    if (dispatch_depth == 0 && watchers_dirty) {
        compact_watchers();
    }
    pthread_mutex_unlock(&watch_lock);
}

void SDL_FilterEvents(SDL_EventFilter filter, void *userdata)
{
    if (!filter) {
        return;
    }
    merge_side_queue();
    Uint32 keep = queue_tail;
    for (Uint32 i = queue_tail; i != queue_head; i++) {
        SDLOP_EventSlot *src = slot_at(i);
        if (!filter(userdata, &src->event)) {
            release_slot(src);
            continue;
        }
        if (keep != i) {
            SDLOP_EventSlot *dst = slot_at(keep);
            *dst = *src;
            if (dst->event.type == SDL_EVENT_TEXT_INPUT && src->event.text.text == src->textbuf) {
                dst->event.text.text = dst->textbuf;
            }
        }
        keep++;
    }
    queue_head = keep;
}

/* ------------------------------------------------------------------ */
/* Enabled / disabled event types (SDL3: 256 blocks of 256 bits)       */
/* ------------------------------------------------------------------ */

typedef struct
{
    Uint32 bits[8];
} DisabledBlock;

static DisabledBlock *disabled_events[256];

bool SDL_EventEnabled(Uint32 type)
{
    Uint8 hi = (Uint8)((type >> 8) & 0xff);
    Uint8 lo = (Uint8)(type & 0xff);
    if (disabled_events[hi] && (disabled_events[hi]->bits[lo / 32] & (1u << (lo & 31)))) {
        return false;
    }
    return true;
}

void SDL_SetEventEnabled(Uint32 type, bool enabled)
{
    Uint8 hi = (Uint8)((type >> 8) & 0xff);
    Uint8 lo = (Uint8)(type & 0xff);
    bool current = SDL_EventEnabled(type);
    if (enabled == current) {
        return;
    }
    if (enabled) {
        disabled_events[hi]->bits[lo / 32] &= ~(1u << (lo & 31));
    } else {
        if (!disabled_events[hi]) {
            disabled_events[hi] = (DisabledBlock *)calloc(1, sizeof(DisabledBlock));
        }
        if (disabled_events[hi]) {
            disabled_events[hi]->bits[lo / 32] |= (1u << (lo & 31));
            SDL_FlushEvent(type);
        }
    }
    if (type == SDL_EVENT_TEXT_INPUT || type == SDL_EVENT_TEXT_EDITING) {
        /* nothing else to toggle: SDLop has no IME to switch off */
    }
}

/* ------------------------------------------------------------------ */
/* Push paths                                                          */
/* ------------------------------------------------------------------ */

static bool push_any_thread(SDL_Event *event, Uint32 reserve_slots, bool drop_input)
{
    if (!dispatch_watchers(event)) {
        SDL_ClearError();
        return false;
    }
    if (SDLOP_IsMainThread()) {
        /* Input state has already been updated by its caller. Preserve
         * watchers/filters, but never turn queue pressure into a state freeze
         * or a queue-full error for input notifications. */
        if (drop_input || (reserve_slots && SDLOP_EventQueueFree() < reserve_slots)) {
            return false;
        }
        return queue_add(event);
    }
    return side_push(event);
}

/* events generated by SDLop itself (input/window drivers, main thread) */
bool SDLOP_PushEventInternal(SDL_Event *event)
{
    if (!SDL_EventEnabled(event->type)) {
        return false;
    }
    return push_any_thread(event, 0, false);
}

/* Input state is updated before this call. A compound key translation may
 * suppress its whole notification sequence when there is insufficient room. */
bool SDLOP_PushInputEvent(SDL_Event *event)
{
    if (!SDL_EventEnabled(event->type)) {
        return false;
    }
    return push_any_thread(event, 1, sdlop.suppress_input_events);
}

bool SDL_PushEvent(SDL_Event *event)
{
    if (!event) {
        return SDL_InvalidParamError("event");
    }
    if (!event->common.timestamp) {
        event->common.timestamp = SDL_GetTicksNS();
    }
    return push_any_thread(event, 0, false);
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
    if (window && type == SDL_EVENT_WINDOW_MOVED && SDLOP_WINDOW_IS_POPUP(window) && window->parent) {
        /* popup positions are reported relative to the parent */
        event.window.data1 -= window->parent->x;
        event.window.data2 -= window->parent->y;
    }
    SDLOP_PushEventInternal(&event);

    /* a window move may change which display the window lives on */
    if (window && type == SDL_EVENT_WINDOW_MOVED) {
        SDLOP_UpdateWindowDisplay(window);
        SDLOP_OnWindowMoved(window);
    } else if (window && type == SDL_EVENT_WINDOW_RESIZED) {
        SDLOP_GL_NotifyWindowResized(window);
    }
    /* SDL_OnWindowFocusGained/Lost: grabs only hold while focused */
    if (window && (type == SDL_EVENT_WINDOW_FOCUS_GAINED || type == SDL_EVENT_WINDOW_FOCUS_LOST) &&
        (window->flags & (SDL_WINDOW_MOUSE_GRABBED | SDL_WINDOW_KEYBOARD_GRABBED)) && !window->is_destroying) {
        SDLOP_UpdateWindowGrab(window);
    }
}

void SDLOP_SendDisplayEvent(SDL_DisplayID displayID, SDL_EventType type, Sint32 data1, Sint32 data2)
{
    SDL_Event event;
    SDL_zero(event);
    event.display.type = type;
    event.display.timestamp = SDL_GetTicksNS();
    event.display.displayID = displayID;
    event.display.data1 = data1;
    event.display.data2 = data2;
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

void SDLOP_SendSimpleEvent(SDL_EventType type)
{
    SDL_Event event;
    SDL_zero(event);
    event.common.type = type;
    event.common.timestamp = SDL_GetTicksNS();
    SDLOP_PushEventInternal(&event);
}

/* ------------------------------------------------------------------ */
/* Main-thread callbacks (SDL_RunOnMainThread)                         */
/* ------------------------------------------------------------------ */

typedef struct MainCallback
{
    SDL_MainThreadCallback callback;
    void *userdata;
    bool wait;
    int state; /* 0 pending, 1 done, 2 canceled */
    pthread_cond_t done;
    struct MainCallback *next;
} MainCallback;

static pthread_mutex_t main_cb_lock = PTHREAD_MUTEX_INITIALIZER;
static MainCallback *main_cb_head, *main_cb_tail;
static _Atomic int main_cb_pending;

bool SDL_RunOnMainThread(SDL_MainThreadCallback callback, void *userdata, bool wait_complete)
{
    if (!callback) {
        return SDL_InvalidParamError("callback");
    }
    if (SDLOP_IsMainThread() || !SDL_WasInit(SDL_INIT_EVENTS)) {
        callback(userdata);
        return true;
    }
    MainCallback *cb = (MainCallback *)SDL_calloc(1, sizeof(*cb));
    if (!cb) {
        return false;
    }
    cb->callback = callback;
    cb->userdata = userdata;
    cb->wait = wait_complete;
    if (wait_complete) {
        pthread_cond_init(&cb->done, NULL);
    }
    pthread_mutex_lock(&main_cb_lock);
    if (main_cb_tail) {
        main_cb_tail->next = cb;
    } else {
        main_cb_head = cb;
    }
    main_cb_tail = cb;
    main_cb_pending = 1;
    pthread_mutex_unlock(&main_cb_lock);
    wake_main_thread();

    if (!wait_complete) {
        return true;
    }
    pthread_mutex_lock(&main_cb_lock);
    while (cb->state == 0) {
        pthread_cond_wait(&cb->done, &main_cb_lock);
    }
    int state = cb->state;
    pthread_mutex_unlock(&main_cb_lock);
    pthread_cond_destroy(&cb->done);
    SDL_free(cb);
    return state == 1 ? true : SDL_SetError("Callback canceled");
}

static void run_main_callbacks(bool cancel)
{
    if (!main_cb_pending) {
        return;
    }
    pthread_mutex_lock(&main_cb_lock);
    MainCallback *list = main_cb_head;
    main_cb_head = main_cb_tail = NULL;
    main_cb_pending = 0;
    pthread_mutex_unlock(&main_cb_lock);

    while (list) {
        MainCallback *cb = list;
        list = cb->next;
        if (!cancel) {
            cb->callback(cb->userdata);
        }
        if (cb->wait) {
            pthread_mutex_lock(&main_cb_lock);
            cb->state = cancel ? 2 : 1;
            pthread_cond_broadcast(&cb->done);
            pthread_mutex_unlock(&main_cb_lock); /* waiter frees it */
        } else {
            SDL_free(cb);
        }
    }
}

/* SDL_Quit(): reset queue state, cancel waiting callbacks */
void SDLOP_QuitEvents(void)
{
    run_main_callbacks(true);
    for (Uint32 i = queue_tail; i != queue_head; i++) {
        release_slot(slot_at(i));
    }
    queue_head = queue_tail = 0;
    pthread_mutex_lock(&side_lock);
    for (int i = 0; i < side_count; i++) {
        free_temp_list(side_queue[i].memory);
    }
    free(side_queue);
    side_queue = NULL;
    side_count = side_capacity = 0;
    side_pending = 0;
    pthread_mutex_unlock(&side_lock);
    watch_lock_acquire();
    free(watchers);
    watchers = NULL;
    num_watchers = 0;
    watchers_dirty = false;
    event_filter.callback = NULL;
    event_filter.userdata = NULL;
    pthread_mutex_unlock(&watch_lock);
    for (int i = 0; i < 256; i++) {
        free(disabled_events[i]);
        disabled_events[i] = NULL;
    }
    SDLOP_FreeTemporaryMemory();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void SDL_PumpEvents(void)
{
    if (!SDLOP_IsMainThread()) {
        return; /* SDL3: only the main thread pumps */
    }
    /* Event watches can call public event APIs. Nested backend/raw pumping
     * would reorder the outer event and can invalidate an in-progress input
     * recovery snapshot. Only the outermost pump advances producers. */
    static bool pumping;
    if (pumping) {
        return;
    }
    pumping = true;
    SDLOP_FreeTemporaryMemory(); /* memory of previously polled events */
    run_main_callbacks(false);

    /* 1. drain raw input ring -> SDL events */
    SDLOP_RawInputPump();

    /* 2. drain window system (non-blocking) */
    if (sdlop.video) {
        sdlop.video->PumpEvents(sdlop.video, -1);
        SDLOP_UpdateCursorAnimation();
    }

    /* 3. events pushed by other threads */
    merge_side_queue();
    pumping = false;
}

int SDL_PeepEvents(SDL_Event *events, int numevents, SDL_EventAction action, Uint32 minType, Uint32 maxType)
{
    if (action == SDL_ADDEVENT) {
        if (!events) {
            return SDL_InvalidParamError("events"), -1;
        }
        int used = 0;
        for (int i = 0; i < numevents; i++) {
            SDL_Event *e = &events[i];
            if (!e->common.timestamp) {
                e->common.timestamp = SDL_GetTicksNS();
            }
            bool ok = SDLOP_IsMainThread() ? queue_add(e) : side_push(e);
            if (!ok) {
                break;
            }
            used++;
        }
        return used;
    }

    merge_side_queue();
    int used = 0;
    for (Uint32 i = queue_tail; i != queue_head && (!events || used < numevents);) {
        SDLOP_EventSlot *slot = slot_at(i);
        Uint32 type = slot->event.type;
        if (type >= minType && type <= maxType) {
            if (events) {
                events[used] = slot->event;
                if (action == SDL_GETEVENT) {
                    release_slot(slot);
                    if (i == queue_tail) {
                        queue_tail++;
                        i++;
                    } else {
                        /* SDL3 returns pointers that stay valid until the next
                         * pump; a mid-queue cut moves slots, so give text a
                         * stable home in a small rotating buffer */
                        static char textcopies[8][SDLOP_TEXT_SIZE];
                        static unsigned textidx;
                        if (type == SDL_EVENT_TEXT_INPUT && events[used].text.text) {
                            char *dst = textcopies[textidx++ & 7];
                            SDL_strlcpy(dst, events[used].text.text, SDLOP_TEXT_SIZE);
                            events[used].text.text = dst;
                        }
                        queue_cut(i);
                    }
                    used++;
                    continue;
                }
            }
            used++;
        }
        i++;
    }
    return used;
}

bool SDL_HasEvent(Uint32 type)
{
    return SDL_HasEvents(type, type);
}

bool SDL_HasEvents(Uint32 minType, Uint32 maxType)
{
    merge_side_queue();
    for (Uint32 i = queue_tail; i != queue_head; i++) {
        Uint32 type = slot_at(i)->event.type;
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
    merge_side_queue();
    /* compact queue, dropping matching events */
    Uint32 keep = queue_tail;
    for (Uint32 i = queue_tail; i != queue_head; i++) {
        SDLOP_EventSlot *src = slot_at(i);
        if (src->event.type >= minType && src->event.type <= maxType) {
            release_slot(src); /* drop */
            continue;
        }
        if (keep != i) {
            SDLOP_EventSlot *dst = slot_at(keep);
            *dst = *src;
            if (dst->event.type == SDL_EVENT_TEXT_INPUT && src->event.text.text == src->textbuf) {
                dst->event.text.text = dst->textbuf;
            }
        }
        keep++;
    }
    queue_head = keep;
}

static bool poll_queue(SDL_Event *event)
{
    if (queue_tail == queue_head) {
        merge_side_queue();
        if (queue_tail == queue_head) {
            return false;
        }
    }
    if (event) {
        SDLOP_EventSlot *slot = slot_at(queue_tail);
        *event = slot->event;
        release_slot(slot);
        queue_tail++;
    }
    return true;
}

bool SDL_PollEvent(SDL_Event *event)
{
    if (poll_queue(event)) {
        return true;
    }
    /* SDL3 pumps on every poll; pumping once the queue has drained is
     * equivalent for `while (SDL_PollEvent(&e))` loops and keeps the
     * per-event cost of a full queue at a plain ring read */
    SDL_PumpEvents();
    return poll_queue(event);
}

bool SDL_WaitEventTimeout(SDL_Event *event, Sint32 timeoutMS)
{
    const Uint64 deadline = (timeoutMS < 0) ? 0 : SDL_GetTicks() + (Uint64)timeoutMS;

    for (;;) {
        SDL_PumpEvents();
        if (poll_queue(event)) {
            return true;
        }
        if (timeoutMS >= 0 && SDL_GetTicks() >= deadline) {
            return false;
        }

        /* Wait for activity on the raw-input / cross-thread wake fd and/or
         * the window system fd. Small caps keep key-repeat timing accurate. */
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

Uint32 SDL_RegisterEvents(int numevents)
{
    if (numevents <= 0) {
        return 0;
    }
    if (sdlop.next_user_event == 0) {
        sdlop.next_user_event = SDL_EVENT_USER;
    }
    if ((Uint64)sdlop.next_user_event + (Uint64)numevents > SDL_EVENT_LAST + 1ull) {
        return SDL_SetError("No user events remaining"), 0;
    }
    Uint32 base = sdlop.next_user_event;
    sdlop.next_user_event += (Uint32)numevents;
    return base;
}
