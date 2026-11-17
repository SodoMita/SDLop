/*
  SDLop - properties subsystem (see <SDL3/SDL_properties.h>).

  Simple linked-list backing store; property IDs are unique nonzero
  Uint32 counter values. Thread-safe via a global table lock and a
  per-set rwlock, in the spirit of SDL3's implementation.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "SDL3/SDL_properties.h"
#include <math.h>
#include <strings.h>
#include <stdlib.h>
#include <pthread.h>

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
} SDLop_Property;

typedef struct SDLop_PropertySet
{
    SDL_PropertiesID id;
    struct SDLop_PropertySet *next;
    SDLop_Property *props;
    pthread_rwlock_t lock;      /* guards this set's prop list */
    int lock_recursion;         /* LockProperties may nest on one thread */
    pthread_t lock_owner;
} SDLop_PropertySet;

static SDLop_PropertySet *property_sets;
static pthread_rwlock_t property_sets_lock = PTHREAD_RWLOCK_INITIALIZER;
static SDL_AtomicInt next_property_id;

static SDLop_PropertySet *property_set_by_id(SDL_PropertiesID id)
{
    if (id == 0) {
        return NULL;
    }
    SDLop_PropertySet *result = NULL;
    pthread_rwlock_rdlock(&property_sets_lock);
    for (SDLop_PropertySet *set = property_sets; set; set = set->next) {
        if (set->id == id) {
            result = set;
            break;
        }
    }
    pthread_rwlock_unlock(&property_sets_lock);
    return result;
}

static void clear_property(SDLop_Property *prop)
{
    free(prop->name);
    if (prop->type == SDL_PROPERTY_TYPE_STRING) {
        free(prop->value.string);
    }
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

SDL_PropertiesID SDL_CreateProperties(void)
{
    SDLop_PropertySet *set = (SDLop_PropertySet *)calloc(1, sizeof(*set));
    if (!set) {
        SDL_OutOfMemory();
        return 0;
    }
    if (pthread_rwlock_init(&set->lock, NULL) != 0) {
        free(set);
        SDL_OutOfMemory();
        return 0;
    }
    do {
        set->id = (SDL_PropertiesID)(SDL_AddAtomicInt(&next_property_id, 1) + 1);
    } while (set->id == 0); /* skip the reserved 0 */
    pthread_rwlock_wrlock(&property_sets_lock);
    set->next = property_sets;
    property_sets = set;
    pthread_rwlock_unlock(&property_sets_lock);
    return set->id;
}

void SDL_ClearProperties(SDL_PropertiesID props)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set) {
        return;
    }
    pthread_rwlock_wrlock(&set->lock);
    SDLop_Property *prop = set->props;
    while (prop) {
        SDLop_Property *next = prop->next;
        clear_property(prop);
        prop = next;
    }
    set->props = NULL;
    pthread_rwlock_unlock(&set->lock);
}

void SDL_DestroyProperties(SDL_PropertiesID props)
{
    SDLop_PropertySet *set = NULL;
    pthread_rwlock_wrlock(&property_sets_lock);
    for (SDLop_PropertySet **link = &property_sets; *link; link = &(*link)->next) {
        if ((*link)->id == props) {
            set = *link;
            *link = set->next;
            break;
        }
    }
    pthread_rwlock_unlock(&property_sets_lock);
    if (!set) {
        return;
    }
    /* Free the props directly; the set is already unlinked so
       SDL_ClearProperties() could no longer find it. */
    pthread_rwlock_wrlock(&set->lock);
    SDLop_Property *prop = set->props;
    while (prop) {
        SDLop_Property *next = prop->next;
        clear_property(prop);
        prop = next;
    }
    set->props = NULL;
    pthread_rwlock_unlock(&set->lock);
    pthread_rwlock_destroy(&set->lock);
    free(set);
}

bool SDL_LockProperties(SDL_PropertiesID props)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set) {
        return SDL_SetError("Invalid properties ID");
    }
    /* Allow recursive locking from the thread that already holds the lock. */
    if (set->lock_recursion > 0 && pthread_equal(set->lock_owner, pthread_self())) {
        set->lock_recursion++;
        return true;
    }
    pthread_rwlock_wrlock(&set->lock);
    set->lock_owner = pthread_self();
    set->lock_recursion = 1;
    return true;
}

void SDL_UnlockProperties(SDL_PropertiesID props)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set) {
        return;
    }
    if (--set->lock_recursion == 0) {
        pthread_rwlock_unlock(&set->lock);
    }
}

bool SDL_HasProperty(SDL_PropertiesID props, const char *name)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set || !name) {
        return false;
    }
    pthread_rwlock_rdlock(&set->lock);
    bool found = find_property(set, name) != NULL;
    pthread_rwlock_unlock(&set->lock);
    return found;
}

bool SDL_ClearProperty(SDL_PropertiesID props, const char *name)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set) {
        return SDL_SetError("Invalid properties ID");
    }
    if (!name) {
        return SDL_SetError("Invalid property name");
    }
    pthread_rwlock_wrlock(&set->lock);
    SDLop_Property **link = find_property(set, name);
    if (link) {
        SDLop_Property *prop = *link;
        *link = prop->next;
        clear_property(prop);
    }
    /* nothing to clear is success, like SDL3 */
    pthread_rwlock_unlock(&set->lock);
    return true;
}

SDL_PropertyType SDL_GetPropertyType(SDL_PropertiesID props, const char *name)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set || !name) {
        return SDL_PROPERTY_TYPE_INVALID;
    }
    pthread_rwlock_rdlock(&set->lock);
    SDLop_Property **link = find_property(set, name);
    SDL_PropertyType type = link ? (*link)->type : SDL_PROPERTY_TYPE_INVALID;
    pthread_rwlock_unlock(&set->lock);
    return type;
}

bool SDL_EnumerateProperties(SDL_PropertiesID props, SDL_EnumeratePropertiesCallback callback, void *userdata)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set) {
        return SDL_SetError("Invalid properties ID");
    }
    if (!callback) {
        return SDL_SetError("Invalid callback");
    }
    pthread_rwlock_rdlock(&set->lock);
    for (SDLop_Property *prop = set->props; prop; prop = prop->next) {
        callback(userdata, props, prop->name);
    }
    pthread_rwlock_unlock(&set->lock);
    return true;
}

/* SDL3-compatible scalar getters convert across scalar property types
 * (STRING parses as number/bool, NUMBER<->FLOAT<->BOOLEAN, POINTER only
 * matches POINTER). Missing/invalid values return the supplied default
 * silently, like SDL3. */
static Sint64 property_as_number(const SDLop_Property *prop, Sint64 default_value)
{
    switch (prop->type) {
    case SDL_PROPERTY_TYPE_STRING: {
        char *end = NULL;
        long long v = strtoll(prop->value.string ? prop->value.string : "", &end, 0);
        return end ? (Sint64)v : default_value;
    }
    case SDL_PROPERTY_TYPE_NUMBER:
        return prop->value.number;
    case SDL_PROPERTY_TYPE_FLOAT:
        return (Sint64)llround((double)prop->value.float_value);
    case SDL_PROPERTY_TYPE_BOOLEAN:
        return prop->value.boolean ? 1 : 0;
    default:
        return default_value;
    }
}

static float property_as_float(const SDLop_Property *prop, float default_value)
{
    switch (prop->type) {
    case SDL_PROPERTY_TYPE_STRING:
        return prop->value.string ? strtof(prop->value.string, NULL) : default_value;
    case SDL_PROPERTY_TYPE_NUMBER:
        return (float)prop->value.number;
    case SDL_PROPERTY_TYPE_FLOAT:
        return prop->value.float_value;
    case SDL_PROPERTY_TYPE_BOOLEAN:
        return prop->value.boolean ? 1.0f : 0.0f;
    default:
        return default_value;
    }
}

static bool property_as_boolean(const SDLop_Property *prop, bool default_value)
{
    switch (prop->type) {
    case SDL_PROPERTY_TYPE_STRING: {
        const char *str = prop->value.string;
        if (!str) {
            return default_value;
        }
        if (!strcasecmp(str, "true") || !strcasecmp(str, "yes") || !strcasecmp(str, "on") || !strcmp(str, "1")) {
            return true;
        }
        if (!strcasecmp(str, "false") || !strcasecmp(str, "no") || !strcasecmp(str, "off") || !strcmp(str, "0")) {
            return false;
        }
        return default_value;
    }
    case SDL_PROPERTY_TYPE_NUMBER:
        return prop->value.number != 0;
    case SDL_PROPERTY_TYPE_FLOAT:
        return prop->value.float_value != 0.0f;
    case SDL_PROPERTY_TYPE_BOOLEAN:
        return prop->value.boolean;
    default:
        return default_value;
    }
}

static SDLop_Property *find_any(const SDLop_PropertySet *set, const char *name)
{
    SDLop_Property **link = find_property((SDLop_PropertySet *)set, name);
    return link ? *link : NULL;
}

/* Set a value of an exact type; overwrites any clashing name in place.
   Takes the set's write lock for the whole read-modify-write. */
static bool set_property(SDL_PropertiesID props, const char *name, SDL_PropertyType type,
                         const void *value)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set) {
        return SDL_SetError("Invalid properties ID");
    }
    if (!name || !*name) {
        return SDL_SetError("Invalid property name");
    }
    pthread_rwlock_wrlock(&set->lock);
    SDLop_Property **link = find_property(set, name);
    SDLop_Property *prop = link ? *link : NULL;
    if (prop && prop->type == SDL_PROPERTY_TYPE_STRING) {
        free(prop->value.string);
        prop->value.string = NULL;
    }
    if (!prop) {
        prop = (SDLop_Property *)calloc(1, sizeof(*prop));
        if (!prop) {
            pthread_rwlock_unlock(&set->lock);
            return SDL_OutOfMemory();
        }
        prop->name = strdup(name);
        if (!prop->name) {
            free(prop);
            pthread_rwlock_unlock(&set->lock);
            return SDL_OutOfMemory();
        }
        prop->next = set->props;
        set->props = prop;
    }
    prop->type = type;
    bool ok = true;
    switch (type) {
    case SDL_PROPERTY_TYPE_POINTER:
        prop->value.pointer = *(void *const *)value;
        break;
    case SDL_PROPERTY_TYPE_STRING: {
        const char *str = *(const char *const *)value;
        prop->value.string = strdup(str ? str : "");
        if (!prop->value.string) {
            SDL_OutOfMemory();
            ok = false;
        }
        break;
    }
    case SDL_PROPERTY_TYPE_NUMBER:
        prop->value.number = *(const Sint64 *)value;
        break;
    case SDL_PROPERTY_TYPE_FLOAT:
        prop->value.float_value = *(const float *)value;
        break;
    case SDL_PROPERTY_TYPE_BOOLEAN:
        prop->value.boolean = *(const bool *)value;
        break;
    default:
        SDL_SetError("Invalid property type");
        ok = false;
        break;
    }
    pthread_rwlock_unlock(&set->lock);
    return ok;
}

static SDLop_Property *get_property(SDL_PropertiesID props, const char *name, SDL_PropertyType type)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set || !name) {
        return NULL;
    }
    pthread_rwlock_rdlock(&set->lock);
    SDLop_Property **link = find_property(set, name);
    SDLop_Property *prop = (link && (*link)->type == type) ? *link : NULL;
    pthread_rwlock_unlock(&set->lock);
    return prop;
}

bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value)
{
    return set_property(props, name, SDL_PROPERTY_TYPE_NUMBER, &value);
}

Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 default_value)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set || !name) {
        return default_value;
    }
    pthread_rwlock_rdlock(&set->lock);
    SDLop_Property *prop = find_any(set, name);
    Sint64 result = prop ? property_as_number(prop, default_value) : default_value;
    pthread_rwlock_unlock(&set->lock);
    return result;
}

bool SDL_SetFloatProperty(SDL_PropertiesID props, const char *name, float value)
{
    return set_property(props, name, SDL_PROPERTY_TYPE_FLOAT, &value);
}

float SDL_GetFloatProperty(SDL_PropertiesID props, const char *name, float default_value)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set || !name) {
        return default_value;
    }
    pthread_rwlock_rdlock(&set->lock);
    SDLop_Property *prop = find_any(set, name);
    float result = prop ? property_as_float(prop, default_value) : default_value;
    pthread_rwlock_unlock(&set->lock);
    return result;
}

bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value)
{
    return set_property(props, name, SDL_PROPERTY_TYPE_BOOLEAN, &value);
}

bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set || !name) {
        return default_value;
    }
    pthread_rwlock_rdlock(&set->lock);
    SDLop_Property *prop = find_any(set, name);
    bool result = prop ? property_as_boolean(prop, default_value) : default_value;
    pthread_rwlock_unlock(&set->lock);
    return result;
}

bool SDL_SetStringProperty(SDL_PropertiesID props, const char *name, const char *value)
{
    if (!value) {
        return SDL_ClearProperty(props, name); /* SDL3: NULL deletes the property */
    }
    return set_property(props, name, SDL_PROPERTY_TYPE_STRING, &value);
}

const char *SDL_GetStringProperty(SDL_PropertiesID props, const char *name, const char *default_value)
{
    SDLop_Property *prop = get_property(props, name, SDL_PROPERTY_TYPE_STRING);
    return prop ? prop->value.string : default_value;
}

bool SDL_SetPointerProperty(SDL_PropertiesID props, const char *name, void *value)
{
    return set_property(props, name, SDL_PROPERTY_TYPE_POINTER, &value);
}

void *SDL_GetPointerProperty(SDL_PropertiesID props, const char *name, void *default_value)
{
    SDLop_Property *prop = get_property(props, name, SDL_PROPERTY_TYPE_POINTER);
    return prop ? prop->value.pointer : default_value;
}

bool SDL_CopyProperties(SDL_PropertiesID source, SDL_PropertiesID destination)
{
    SDLop_PropertySet *src = property_set_by_id(source);
    SDLop_PropertySet *dst = property_set_by_id(destination);
    if (!src) {
        return SDL_SetError("Invalid source properties ID");
    }
    if (!dst) {
        return SDL_SetError("Invalid destination properties ID");
    }
    pthread_rwlock_rdlock(&src->lock);
    for (SDLop_Property *prop = src->props; prop; prop = prop->next) {
        switch (prop->type) {
        case SDL_PROPERTY_TYPE_POINTER:
            SDL_SetPointerProperty(destination, prop->name, prop->value.pointer);
            break;
        case SDL_PROPERTY_TYPE_STRING:
            SDL_SetStringProperty(destination, prop->name, prop->value.string);
            break;
        case SDL_PROPERTY_TYPE_NUMBER:
            SDL_SetNumberProperty(destination, prop->name, prop->value.number);
            break;
        case SDL_PROPERTY_TYPE_FLOAT:
            SDL_SetFloatProperty(destination, prop->name, prop->value.float_value);
            break;
        case SDL_PROPERTY_TYPE_BOOLEAN:
            SDL_SetBooleanProperty(destination, prop->name, prop->value.boolean);
            break;
        default:
            break;
        }
    }
    pthread_rwlock_unlock(&src->lock);
    return true;
}
