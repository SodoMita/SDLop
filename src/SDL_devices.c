/*
  SDLop - input device registry: mice, keyboards and touch devices
  (SDL_GetMice / SDL_GetKeyboards / SDL_GetTouchDevices and friends).

  Same contract as stock SDL3's SDL_AddMouse / SDL_AddKeyboard /
  SDL_AddTouch: drivers register devices, the registry owns the names and
  sends the *_ADDED / *_REMOVED events. Touch devices a driver never
  announced are registered on their first finger-down, so the touch API
  works with every backend.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

typedef struct DeviceEntry
{
    Uint32 id;
    char *name;
} DeviceEntry;

typedef struct DeviceList
{
    DeviceEntry *items;
    int count;
} DeviceList;

static DeviceList mice, keyboards;

static int list_index(const DeviceList *list, Uint32 id)
{
    for (int i = 0; i < list->count; ++i) {
        if (list->items[i].id == id) {
            return i;
        }
    }
    return -1;
}

static bool list_add(DeviceList *list, Uint32 id, const char *name)
{
    if (id == 0 || list_index(list, id) >= 0) {
        return false;
    }
    DeviceEntry *items = (DeviceEntry *)SDL_realloc(list->items, (size_t)(list->count + 1) * sizeof(*items));
    if (!items) {
        return false;
    }
    list->items = items;
    items[list->count].id = id;
    items[list->count].name = SDL_strdup(name);
    list->count++;
    return true;
}

static bool list_remove(DeviceList *list, Uint32 id)
{
    int i = list_index(list, id);
    if (i < 0) {
        return false;
    }
    SDL_free(list->items[i].name);
    if (i != list->count - 1) {
        SDL_memmove(&list->items[i], &list->items[i + 1], (size_t)(list->count - i - 1) * sizeof(list->items[0]));
    }
    list->count--;
    return true;
}

static void list_clear(DeviceList *list)
{
    for (int i = 0; i < list->count; ++i) {
        SDL_free(list->items[i].name);
    }
    SDL_free(list->items);
    list->items = NULL;
    list->count = 0;
}

static Uint32 *list_ids(const DeviceList *list, int *count)
{
    Uint32 *ids = (Uint32 *)SDL_malloc((size_t)(list->count + 1) * sizeof(*ids));
    if (!ids) {
        if (count) {
            *count = 0;
        }
        return NULL;
    }
    for (int i = 0; i < list->count; ++i) {
        ids[i] = list->items[i].id;
    }
    ids[list->count] = 0;
    if (count) {
        *count = list->count;
    }
    return ids;
}

static void push_device_event(Uint32 type, Uint32 which)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = type;
    event.common.timestamp = SDL_GetTicksNS();
    if (type == SDL_EVENT_KEYBOARD_ADDED || type == SDL_EVENT_KEYBOARD_REMOVED) {
        event.kdevice.which = which;
    } else {
        event.mdevice.which = which;
    }
    SDLOP_PushEventInternal(&event);
}

/* ------------------------------------------------------------------ */
/* mice                                                                */
/* ------------------------------------------------------------------ */

void SDLOP_AddMouse(SDL_MouseID id, const char *name)
{
    if (list_add(&mice, id, name ? name : "Mouse")) {
        push_device_event(SDL_EVENT_MOUSE_ADDED, id);
    }
}

void SDLOP_RemoveMouse(SDL_MouseID id, bool send_event)
{
    if (list_remove(&mice, id) && send_event) {
        push_device_event(SDL_EVENT_MOUSE_REMOVED, id);
    }
}

bool SDL_HasMouse(void)
{
    return mice.count > 0;
}

SDL_MouseID *SDL_GetMice(int *count)
{
    return (SDL_MouseID *)list_ids(&mice, count);
}

const char *SDL_GetMouseNameForID(SDL_MouseID instance_id)
{
    switch (instance_id) {
    case 0 /* SDL_GLOBAL_MOUSE_ID */:
        return "Mouse";
    case SDL_TOUCH_MOUSEID: {
        /* can't tell which touch device it was: use the first one */
        const char *name = NULL;
        SDL_TouchID *devices = SDL_GetTouchDevices(NULL);
        if (devices) {
            if (devices[0]) {
                name = SDL_GetTouchDeviceName(devices[0]);
            }
            SDL_free(devices);
        }
        return name ? name : "Touch";
    }
    case SDL_PEN_MOUSEID:
        return "Pen";
    default:
        break;
    }
    int i = list_index(&mice, instance_id);
    if (i < 0) {
        SDL_SetError("Mouse %" SDL_PRIu32 " not found", instance_id);
        return NULL;
    }
    return mice.items[i].name;
}

/* ------------------------------------------------------------------ */
/* keyboards                                                           */
/* ------------------------------------------------------------------ */

void SDLOP_AddKeyboard(SDL_KeyboardID id, const char *name)
{
    if (list_add(&keyboards, id, name ? name : "Keyboard")) {
        push_device_event(SDL_EVENT_KEYBOARD_ADDED, id);
    }
}

void SDLOP_RemoveKeyboard(SDL_KeyboardID id, bool send_event)
{
    if (list_remove(&keyboards, id) && send_event) {
        push_device_event(SDL_EVENT_KEYBOARD_REMOVED, id);
    }
}

bool SDL_HasKeyboard(void)
{
    return keyboards.count > 0;
}

SDL_KeyboardID *SDL_GetKeyboards(int *count)
{
    return (SDL_KeyboardID *)list_ids(&keyboards, count);
}

const char *SDL_GetKeyboardNameForID(SDL_KeyboardID instance_id)
{
    if (instance_id == 0 /* SDL_GLOBAL_KEYBOARD_ID */) {
        return "Keyboard";
    }
    int i = list_index(&keyboards, instance_id);
    if (i < 0) {
        SDL_SetError("Keyboard %" SDL_PRIu32 " not found", instance_id);
        return NULL;
    }
    return keyboards.items[i].name;
}

/* ------------------------------------------------------------------ */
/* touch devices + finger state                                        */
/* ------------------------------------------------------------------ */

typedef struct TouchDevice
{
    SDL_TouchID id;
    SDL_TouchDeviceType type;
    char *name;
    SDL_Finger *fingers;
    int num_fingers;
    int max_fingers;
} TouchDevice;

static TouchDevice *touch_devices;
static int num_touch_devices;

static TouchDevice *find_touch(SDL_TouchID id)
{
    for (int i = 0; i < num_touch_devices; ++i) {
        if (touch_devices[i].id == id) {
            return &touch_devices[i];
        }
    }
    return NULL;
}

void SDLOP_AddTouch(SDL_TouchID id, SDL_TouchDeviceType type, const char *name)
{
    if (id == 0 || find_touch(id)) {
        return;
    }
    TouchDevice *devs = (TouchDevice *)SDL_realloc(touch_devices, (size_t)(num_touch_devices + 1) * sizeof(*devs));
    if (!devs) {
        return;
    }
    touch_devices = devs;
    TouchDevice *t = &devs[num_touch_devices++];
    SDL_zerop(t);
    t->id = id;
    t->type = type;
    t->name = SDL_strdup(name ? name : "Touch");
}

void SDLOP_RemoveTouch(SDL_TouchID id)
{
    for (int i = 0; i < num_touch_devices; ++i) {
        if (touch_devices[i].id == id) {
            SDL_free(touch_devices[i].name);
            SDL_free(touch_devices[i].fingers);
            if (i != num_touch_devices - 1) {
                SDL_memmove(&touch_devices[i], &touch_devices[i + 1],
                            (size_t)(num_touch_devices - i - 1) * sizeof(touch_devices[0]));
            }
            num_touch_devices--;
            return;
        }
    }
}

/* finger bookkeeping, called by SDLOP_SendTouch before the event goes out */
void SDLOP_TrackTouch(Uint32 type, SDL_TouchID touchID, SDL_FingerID fingerID, float x, float y, float pressure)
{
    if (touchID == SDL_MOUSE_TOUCHID || touchID == SDL_PEN_TOUCHID) {
        return;
    }
    TouchDevice *t = find_touch(touchID);
    if (!t) {
        if (type != SDL_EVENT_FINGER_DOWN) {
            return;
        }
        SDLOP_AddTouch(touchID, SDL_TOUCH_DEVICE_DIRECT, "Touch");
        t = find_touch(touchID);
        if (!t) {
            return;
        }
    }
    int idx = -1;
    for (int i = 0; i < t->num_fingers; ++i) {
        if (t->fingers[i].id == fingerID) {
            idx = i;
            break;
        }
    }
    if (type == SDL_EVENT_FINGER_DOWN || (type == SDL_EVENT_FINGER_MOTION && idx < 0)) {
        if (idx < 0) {
            if (t->num_fingers == t->max_fingers) {
                int n = t->max_fingers ? t->max_fingers * 2 : 4;
                SDL_Finger *f = (SDL_Finger *)SDL_realloc(t->fingers, (size_t)n * sizeof(*f));
                if (!f) {
                    return;
                }
                t->fingers = f;
                t->max_fingers = n;
            }
            idx = t->num_fingers++;
            t->fingers[idx].id = fingerID;
        }
        t->fingers[idx].x = x;
        t->fingers[idx].y = y;
        t->fingers[idx].pressure = pressure;
    } else if (type == SDL_EVENT_FINGER_MOTION) {
        t->fingers[idx].x = x;
        t->fingers[idx].y = y;
        t->fingers[idx].pressure = pressure;
    } else if (idx >= 0) { /* UP / CANCELED */
        if (idx != t->num_fingers - 1) {
            SDL_memmove(&t->fingers[idx], &t->fingers[idx + 1], (size_t)(t->num_fingers - idx - 1) * sizeof(t->fingers[0]));
        }
        t->num_fingers--;
    }
}

static TouchDevice *get_touch(SDL_TouchID id)
{
    TouchDevice *t = find_touch(id);
    if (!t && id != SDL_MOUSE_TOUCHID && id != SDL_PEN_TOUCHID) {
        SDL_SetError("Unknown touch device id %d, cannot reset", (int)id);
    }
    return t;
}

SDL_TouchID *SDL_GetTouchDevices(int *count)
{
    if (count) {
        *count = 0;
    }
    SDL_TouchID *ids = (SDL_TouchID *)SDL_malloc((size_t)(num_touch_devices + 1) * sizeof(*ids));
    if (!ids) {
        return NULL;
    }
    for (int i = 0; i < num_touch_devices; ++i) {
        ids[i] = touch_devices[i].id;
    }
    ids[num_touch_devices] = 0;
    if (count) {
        *count = num_touch_devices;
    }
    return ids;
}

const char *SDL_GetTouchDeviceName(SDL_TouchID touchID)
{
    TouchDevice *t = get_touch(touchID);
    return t ? t->name : NULL;
}

SDL_TouchDeviceType SDL_GetTouchDeviceType(SDL_TouchID touchID)
{
    TouchDevice *t = get_touch(touchID);
    return t ? t->type : SDL_TOUCH_DEVICE_INVALID;
}

SDL_Finger **SDL_GetTouchFingers(SDL_TouchID touchID, int *count)
{
    if (count) {
        *count = 0;
    }
    TouchDevice *t = get_touch(touchID);
    if (!t) {
        return NULL;
    }
    /* one allocation: NULL-terminated pointer array followed by the data */
    SDL_Finger **fingers = (SDL_Finger **)SDL_malloc((size_t)(t->num_fingers + 1) * sizeof(*fingers) +
                                                     (size_t)t->num_fingers * sizeof(**fingers));
    if (!fingers) {
        return NULL;
    }
    SDL_Finger *data = (SDL_Finger *)(fingers + (t->num_fingers + 1));
    for (int i = 0; i < t->num_fingers; ++i) {
        data[i] = t->fingers[i];
        fingers[i] = &data[i];
    }
    fingers[t->num_fingers] = NULL;
    if (count) {
        *count = t->num_fingers;
    }
    return fingers;
}

void SDLOP_DevicesQuit(void)
{
    list_clear(&mice);
    list_clear(&keyboards);
    while (num_touch_devices) {
        SDLOP_RemoveTouch(touch_devices[0].id);
    }
    SDL_free(touch_devices);
    touch_devices = NULL;
}
