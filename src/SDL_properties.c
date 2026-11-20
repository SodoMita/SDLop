/*
  SDLop - properties subsystem (see <SDL3/SDL_properties.h>).

  Linked-list backing store (property groups are small); IDs are unique
  nonzero Uint32 counter values. Thread-safe like SDL3: one recursive lock
  guards the registry and every group, and SDL_LockProperties() takes it
  so a caller can make several calls atomically.

  Conversions follow SDL3: GetString formats numbers / floats / booleans,
  GetNumber / GetFloat / GetBoolean parse strings, pointers only match
  pointers. Setting a NULL string / pointer clears the property. Pointer
  properties with a cleanup callback are not copied by SDL_CopyProperties.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include <math.h>
#include <stdlib.h>

typedef struct SDLop_Property
{
    struct SDLop_Property *next;
    char *name;
    SDL_PropertyType type;
    union
    {
        void *pointer;
        char *string;
        Sint64 number;
        float float_value;
        bool boolean;
    } value;
    char *string_storage; /* GetStringProperty() formatting of scalars */
    SDL_CleanupPropertyCallback cleanup;
    void *userdata;
} SDLop_Property;

typedef struct SDLop_PropertySet
{
    SDL_PropertiesID id;
    struct SDLop_PropertySet *next;
    SDLop_Property *props;
} SDLop_PropertySet;

static SDLop_PropertySet *property_sets;
static SDL_PropertiesID next_property_id;
static SDL_PropertiesID global_properties;

static pthread_mutex_t prop_lock;
static pthread_once_t prop_once = PTHREAD_ONCE_INIT;

static void init_lock(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&prop_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void lock(void)
{
    pthread_once(&prop_once, init_lock);
    pthread_mutex_lock(&prop_lock);
}

static void unlock(void)
{
    pthread_mutex_unlock(&prop_lock);
}

static SDLop_PropertySet *property_set_by_id(SDL_PropertiesID id)
{
    if (id == 0) {
        return NULL;
    }
    for (SDLop_PropertySet *set = property_sets; set; set = set->next) {
        if (set->id == id) {
            return set;
        }
    }
    return NULL;
}

/* frees the property; runs the cleanup callback (with the lock held, like
 * SDL3 - callbacks must not call back into the same group from another
 * thread) */
static void free_property(SDLop_Property *prop, bool cleanup)
{
    if (prop->type == SDL_PROPERTY_TYPE_STRING) {
        free(prop->value.string);
    } else if (prop->type == SDL_PROPERTY_TYPE_POINTER && prop->cleanup && cleanup) {
        prop->cleanup(prop->userdata, prop->value.pointer);
    }
    free(prop->string_storage);
    free(prop->name);
    free(prop);
}

static SDLop_Property **find_property(SDLop_PropertySet *set, const char *name)
{
    SDLop_Property **link = &set->props;
    while (*link) {
        if (strcmp((*link)->name, name) == 0) {
            return link;
        }
        link = &(*link)->next;
    }
    return NULL;
}

static SDLop_Property *find_any(SDL_PropertiesID props, const char *name)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set || !name || !*name) {
        return NULL;
    }
    SDLop_Property **link = find_property(set, name);
    return link ? *link : NULL;
}

SDL_PropertiesID SDL_CreateProperties(void)
{
    SDLop_PropertySet *set = (SDLop_PropertySet *)calloc(1, sizeof(*set));
    if (!set) {
        SDL_OutOfMemory();
        return 0;
    }
    lock();
    set->id = ++next_property_id;
    if (set->id == 0) {
        set->id = ++next_property_id; /* skip the reserved 0 */
    }
    set->next = property_sets;
    property_sets = set;
    unlock();
    return set->id;
}

SDL_PropertiesID SDL_GetGlobalProperties(void)
{
    lock();
    if (!global_properties || !property_set_by_id(global_properties)) {
        global_properties = SDL_CreateProperties();
    }
    SDL_PropertiesID id = global_properties;
    unlock();
    return id;
}

static void clear_all(SDLop_PropertySet *set)
{
    SDLop_Property *prop = set->props;
    set->props = NULL;
    while (prop) {
        SDLop_Property *next = prop->next;
        free_property(prop, true);
        prop = next;
    }
}

void SDL_DestroyProperties(SDL_PropertiesID props)
{
    if (!props) {
        return;
    }
    lock();
    for (SDLop_PropertySet **link = &property_sets; *link; link = &(*link)->next) {
        SDLop_PropertySet *set = *link;
        if (set->id == props) {
            *link = set->next;
            clear_all(set);
            free(set);
            if (props == global_properties) {
                global_properties = 0;
            }
            break;
        }
    }
    unlock();
}

/* SDL_Quit(): the global group goes away, like SDL3's SDL_QuitProperties */
void SDLOP_QuitProperties(void)
{
    if (global_properties) {
        SDL_DestroyProperties(global_properties);
    }
}

bool SDL_LockProperties(SDL_PropertiesID props)
{
    lock();
    if (!property_set_by_id(props)) {
        unlock();
        return SDL_InvalidParamError("props");
    }
    return true; /* recursive lock held until SDL_UnlockProperties() */
}

void SDL_UnlockProperties(SDL_PropertiesID props)
{
    (void)props;
    unlock();
}

bool SDL_HasProperty(SDL_PropertiesID props, const char *name)
{
    lock();
    bool r = find_any(props, name) != NULL;
    unlock();
    return r;
}

bool SDL_ClearProperty(SDL_PropertiesID props, const char *name)
{
    lock();
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set) {
        unlock();
        return SDL_InvalidParamError("props");
    }
    if (!name || !*name) {
        unlock();
        return SDL_InvalidParamError("name");
    }
    SDLop_Property **link = find_property(set, name);
    if (link) {
        SDLop_Property *prop = *link;
        *link = prop->next;
        free_property(prop, true);
    }
    unlock();
    return true; /* nothing to clear: success, like SDL3 */
}

SDL_PropertyType SDL_GetPropertyType(SDL_PropertiesID props, const char *name)
{
    lock();
    SDLop_Property *prop = find_any(props, name);
    SDL_PropertyType type = prop ? prop->type : SDL_PROPERTY_TYPE_INVALID;
    unlock();
    return type;
}

bool SDL_EnumerateProperties(SDL_PropertiesID props, SDL_EnumeratePropertiesCallback callback, void *userdata)
{
    if (!callback) {
        return SDL_InvalidParamError("callback");
    }
    lock();
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set) {
        unlock();
        return SDL_InvalidParamError("props");
    }
    for (SDLop_Property *prop = set->props; prop;) {
        SDLop_Property *next = prop->next; /* callback may clear this one */
        callback(userdata, props, prop->name);
        prop = next;
    }
    unlock();
    return true;
}

/* Install a new property, replacing any with the same name. On failure
 * the pointer cleanup callback is still invoked (SDL3 contract). */
static bool set_property(SDL_PropertiesID props, const char *name, SDLop_Property *prop)
{
    lock();
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set) {
        unlock();
        free_property(prop, true);
        return SDL_InvalidParamError("props");
    }
    if (!name || !*name) {
        unlock();
        free_property(prop, true);
        return SDL_InvalidParamError("name");
    }
    prop->name = strdup(name);
    if (!prop->name) {
        unlock();
        free_property(prop, true);
        return SDL_OutOfMemory();
    }
    SDLop_Property **link = find_property(set, name);
    if (link) {
        SDLop_Property *old = *link;
        *link = old->next;
        free_property(old, true);
    }
    prop->next = set->props;
    set->props = prop;
    unlock();
    return true;
}

static SDLop_Property *new_property(SDL_PropertyType type)
{
    SDLop_Property *prop = (SDLop_Property *)calloc(1, sizeof(*prop));
    if (prop) {
        prop->type = type;
    }
    return prop;
}

bool SDL_SetPointerPropertyWithCleanup(SDL_PropertiesID props, const char *name, void *value,
                                       SDL_CleanupPropertyCallback cleanup, void *userdata)
{
    if (!value) {
        if (cleanup) {
            cleanup(userdata, value);
        }
        return SDL_ClearProperty(props, name);
    }
    SDLop_Property *prop = new_property(SDL_PROPERTY_TYPE_POINTER);
    if (!prop) {
        if (cleanup) {
            cleanup(userdata, value);
        }
        return SDL_OutOfMemory();
    }
    prop->value.pointer = value;
    prop->cleanup = cleanup;
    prop->userdata = userdata;
    return set_property(props, name, prop);
}

bool SDL_SetPointerProperty(SDL_PropertiesID props, const char *name, void *value)
{
    return SDL_SetPointerPropertyWithCleanup(props, name, value, NULL, NULL);
}

bool SDL_SetStringProperty(SDL_PropertiesID props, const char *name, const char *value)
{
    if (!value) {
        return SDL_ClearProperty(props, name);
    }
    SDLop_Property *prop = new_property(SDL_PROPERTY_TYPE_STRING);
    if (!prop) {
        return SDL_OutOfMemory();
    }
    prop->value.string = strdup(value);
    if (!prop->value.string) {
        free(prop);
        return SDL_OutOfMemory();
    }
    return set_property(props, name, prop);
}

bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value)
{
    SDLop_Property *prop = new_property(SDL_PROPERTY_TYPE_NUMBER);
    if (!prop) {
        return SDL_OutOfMemory();
    }
    prop->value.number = value;
    return set_property(props, name, prop);
}

bool SDL_SetFloatProperty(SDL_PropertiesID props, const char *name, float value)
{
    SDLop_Property *prop = new_property(SDL_PROPERTY_TYPE_FLOAT);
    if (!prop) {
        return SDL_OutOfMemory();
    }
    prop->value.float_value = value;
    return set_property(props, name, prop);
}

bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value)
{
    SDLop_Property *prop = new_property(SDL_PROPERTY_TYPE_BOOLEAN);
    if (!prop) {
        return SDL_OutOfMemory();
    }
    prop->value.boolean = value;
    return set_property(props, name, prop);
}

void *SDL_GetPointerProperty(SDL_PropertiesID props, const char *name, void *default_value)
{
    lock();
    SDLop_Property *prop = find_any(props, name);
    void *v = (prop && prop->type == SDL_PROPERTY_TYPE_POINTER) ? prop->value.pointer : default_value;
    unlock();
    return v;
}

const char *SDL_GetStringProperty(SDL_PropertiesID props, const char *name, const char *default_value)
{
    const char *value = default_value;
    lock();
    SDLop_Property *prop = find_any(props, name);
    if (prop) {
        switch (prop->type) {
        case SDL_PROPERTY_TYPE_STRING:
            value = prop->value.string;
            break;
        case SDL_PROPERTY_TYPE_NUMBER:
            if (!prop->string_storage) {
                SDL_asprintf(&prop->string_storage, "%" SDL_PRIs64, prop->value.number);
            }
            if (prop->string_storage) {
                value = prop->string_storage;
            }
            break;
        case SDL_PROPERTY_TYPE_FLOAT:
            if (!prop->string_storage) {
                SDL_asprintf(&prop->string_storage, "%f", (double)prop->value.float_value);
            }
            if (prop->string_storage) {
                value = prop->string_storage;
            }
            break;
        case SDL_PROPERTY_TYPE_BOOLEAN:
            value = prop->value.boolean ? "true" : "false";
            break;
        default:
            break;
        }
    }
    unlock();
    return value;
}

Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 default_value)
{
    Sint64 value = default_value;
    lock();
    SDLop_Property *prop = find_any(props, name);
    if (prop) {
        switch (prop->type) {
        case SDL_PROPERTY_TYPE_STRING:
            value = (Sint64)strtoll(prop->value.string, NULL, 0);
            break;
        case SDL_PROPERTY_TYPE_NUMBER:
            value = prop->value.number;
            break;
        case SDL_PROPERTY_TYPE_FLOAT:
            value = (Sint64)llround((double)prop->value.float_value);
            break;
        case SDL_PROPERTY_TYPE_BOOLEAN:
            value = prop->value.boolean ? 1 : 0;
            break;
        default:
            break;
        }
    }
    unlock();
    return value;
}

float SDL_GetFloatProperty(SDL_PropertiesID props, const char *name, float default_value)
{
    float value = default_value;
    lock();
    SDLop_Property *prop = find_any(props, name);
    if (prop) {
        switch (prop->type) {
        case SDL_PROPERTY_TYPE_STRING:
            value = (float)atof(prop->value.string);
            break;
        case SDL_PROPERTY_TYPE_NUMBER:
            value = (float)prop->value.number;
            break;
        case SDL_PROPERTY_TYPE_FLOAT:
            value = prop->value.float_value;
            break;
        case SDL_PROPERTY_TYPE_BOOLEAN:
            value = prop->value.boolean ? 1.0f : 0.0f;
            break;
        default:
            break;
        }
    }
    unlock();
    return value;
}

bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value)
{
    bool value = default_value;
    lock();
    SDLop_Property *prop = find_any(props, name);
    if (prop) {
        switch (prop->type) {
        case SDL_PROPERTY_TYPE_STRING:
            value = SDLOP_GetStringBoolean(prop->value.string, default_value);
            break;
        case SDL_PROPERTY_TYPE_NUMBER:
            value = prop->value.number != 0;
            break;
        case SDL_PROPERTY_TYPE_FLOAT:
            value = prop->value.float_value != 0.0f;
            break;
        case SDL_PROPERTY_TYPE_BOOLEAN:
            value = prop->value.boolean;
            break;
        default:
            break;
        }
    }
    unlock();
    return value;
}

bool SDL_CopyProperties(SDL_PropertiesID source, SDL_PropertiesID destination)
{
    lock();
    SDLop_PropertySet *src = property_set_by_id(source);
    SDLop_PropertySet *dst = property_set_by_id(destination);
    if (!src) {
        unlock();
        return SDL_InvalidParamError("src");
    }
    if (!dst) {
        unlock();
        return SDL_InvalidParamError("dst");
    }
    bool ok = true;
    for (SDLop_Property *prop = src->props; prop; prop = prop->next) {
        switch (prop->type) {
        case SDL_PROPERTY_TYPE_POINTER:
            if (!prop->cleanup) { /* SDL3: owned pointers are not copied */
                ok &= SDL_SetPointerProperty(destination, prop->name, prop->value.pointer);
            }
            break;
        case SDL_PROPERTY_TYPE_STRING:
            ok &= SDL_SetStringProperty(destination, prop->name, prop->value.string);
            break;
        case SDL_PROPERTY_TYPE_NUMBER:
            ok &= SDL_SetNumberProperty(destination, prop->name, prop->value.number);
            break;
        case SDL_PROPERTY_TYPE_FLOAT:
            ok &= SDL_SetFloatProperty(destination, prop->name, prop->value.float_value);
            break;
        case SDL_PROPERTY_TYPE_BOOLEAN:
            ok &= SDL_SetBooleanProperty(destination, prop->name, prop->value.boolean);
            break;
        default:
            break;
        }
    }
    unlock();
    return ok;
}
