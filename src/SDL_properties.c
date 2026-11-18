/*
  SDLop - properties subsystem (see <SDL3/SDL_properties.h>).

  Simple linked-list backing store; property IDs are unique nonzero
  Uint32 counter values.

  Thread safety: a global rwlock guards the table of sets; each set has a
  refcount (one reference held by the table, one per active operation) so a
  set being destroyed stays alive until its last user lets go. Set contents
  are guarded by the per-set rwlock; SDL_LockProperties takes that lock in
  write mode and is recursive on the owning thread. SDL3 coordinates with
  the same rwlock-per-set design.

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
    pthread_rwlock_t lock;      /* guards this set's prop list; non-recursive, like SDL3 */
    void *lock_owner;           /* (pthread_t) of the lock holder, atomic; NULL when unlocked */
    SDL_AtomicInt refs;         /* table link + one per active user */
    bool dead;                  /* SDL_DestroyProperties was called */
} SDLop_PropertySet;

static SDLop_PropertySet *property_sets;
static pthread_rwlock_t property_sets_lock;
static pthread_once_t property_sets_lock_once = PTHREAD_ONCE_INIT;
static pthread_rwlockattr_t property_lock_attr; /* shared by table + set locks */
static SDL_AtomicInt next_property_id;

/* glibc's default rwlock prefers readers, which lets a read-heavy workload
   starve SDL_CreateProperties/SDL_DestroyProperties indefinitely; prefer
   writers instead (writers are rare). */
static void property_lock_attr_init_once(void)
{
    pthread_rwlockattr_init(&property_lock_attr);
    pthread_rwlockattr_setkind_np(&property_lock_attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&property_sets_lock, &property_lock_attr);
}

static void property_lock_attr_ensure(void)
{
    pthread_once(&property_sets_lock_once, property_lock_attr_init_once);
}

static void clear_property(SDLop_Property *prop)
{
    free(prop->name);
    if (prop->type == SDL_PROPERTY_TYPE_STRING) {
        free(prop->value.string);
    }
    free(prop);
}

static void property_set_free(SDLop_PropertySet *set)
{
    SDLop_Property *prop = set->props;
    while (prop) {
        SDLop_Property *next = prop->next;
        clear_property(prop);
        prop = next;
    }
    pthread_rwlock_destroy(&set->lock);
    free(set);
}

static void property_set_unref(SDLop_PropertySet *set)
{
    if (set && SDL_AtomicDecRef(&set->refs)) {
        /* Only dead sets can reach zero references (live sets always have
           the table's reference), and only after SDL_DestroyProperties.
           Unlink here, now that no user can still hold a pointer. */
        property_lock_attr_ensure();
        pthread_rwlock_wrlock(&property_sets_lock);
        for (SDLop_PropertySet **link = &property_sets; *link; link = &(*link)->next) {
            if (*link == set) {
                *link = set->next;
                break;
            }
        }
        pthread_rwlock_unlock(&property_sets_lock);
        property_set_free(set);
    }
}

/* Take a reference, but fail if the count is already zero: a set whose
   count hit zero is being freed (its table unlink is pending behind the
   global write lock), and must not be resurrected. */
static bool property_set_try_ref(SDLop_PropertySet *set)
{
    int old = SDL_GetAtomicInt(&set->refs);
    while (old > 0) {
        if (SDL_CompareAndSwapAtomicInt(&set->refs, old, old + 1)) {
            return true;
        }
        old = SDL_GetAtomicInt(&set->refs);
    }
    return false;
}

/* Find a set and take a reference on it; the reference keeps the struct
   alive even if SDL_DestroyProperties marks it dead meanwhile. Dead sets
   stay in the table until their last reference is dropped, so that a
   matching SDL_UnlockProperties can still find (and unlock) a set that was
   destroyed while locked; every other operation rejects dead sets. */
static SDLop_PropertySet *property_set_ref_ex(SDL_PropertiesID id, bool allow_dead)
{
    SDLop_PropertySet *result = NULL;
    if (id != 0) {
        property_lock_attr_ensure();
        pthread_rwlock_rdlock(&property_sets_lock);
        for (SDLop_PropertySet *set = property_sets; set; set = set->next) {
            if (set->id == id && (allow_dead || !set->dead) && property_set_try_ref(set)) {
                result = set;
                break;
            }
        }
        pthread_rwlock_unlock(&property_sets_lock);
    }
    return result;
}

static SDLop_PropertySet *property_set_ref(SDL_PropertiesID id)
{
    return property_set_ref_ex(id, false);
}

/* caller holds set->lock (read or write) */
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
    property_lock_attr_ensure();
    if (pthread_rwlock_init(&set->lock, &property_lock_attr) != 0) {
        free(set);
        SDL_OutOfMemory();
        return 0;
    }
    do {
        set->id = (SDL_PropertiesID)(SDL_AddAtomicInt(&next_property_id, 1) + 1);
    } while (set->id == 0); /* skip the reserved 0 */
    SDL_SetAtomicInt(&set->refs, 1); /* the table's reference */
    property_lock_attr_ensure();
    pthread_rwlock_wrlock(&property_sets_lock);
    set->next = property_sets;
    property_sets = set;
    pthread_rwlock_unlock(&property_sets_lock);
    return set->id;
}

void SDL_DestroyProperties(SDL_PropertiesID props)
{
    bool marked = false;
    SDLop_PropertySet *set = NULL;
    property_lock_attr_ensure();
    pthread_rwlock_wrlock(&property_sets_lock);
    for (SDLop_PropertySet *it = property_sets; it; it = it->next) {
        if (it->id == props && !it->dead) {
            it->dead = true; /* rejects new users; stays findable for Unlock */
            set = it;
            marked = true;
            break;
        }
    }
    pthread_rwlock_unlock(&property_sets_lock);
    /* Drop the table's reference; the last remaining user frees the set. */
    if (marked) {
        property_set_unref(set);
    }
}

bool SDL_LockProperties(SDL_PropertiesID props)
{
    SDLop_PropertySet *set = property_set_ref(props);
    if (!set) {
        return SDL_SetError("Invalid properties ID");
    }
    pthread_rwlock_wrlock(&set->lock);
    /* pthread_t is an unsigned long on SDLop's targets: it fits in a
       pointer-sized atomic. */
    SDL_SetAtomicPointer(&set->lock_owner, (void *)pthread_self());
    /* keep the reference: it is released by the matching
       SDL_UnlockProperties, which accepts dead sets for exactly this
       reason (a set destroyed while locked must still be unlockable) */
    return true;
}

void SDL_UnlockProperties(SDL_PropertiesID props)
{
    /* Dead sets are accepted here: a set destroyed while locked must still
       be unlockable, or the lock (and the session reference) would leak. */
    SDLop_PropertySet *set = property_set_ref_ex(props, true);
    if (!set) {
        return;
    }
    /* Only the lock holder may unlock; without that check an unmatched
       unlock (e.g. after a failed Lock on a just-destroyed set) would
       release another thread's lock and unbalance the references. */
    if (SDL_GetAtomicPointer(&set->lock_owner) == (void *)pthread_self()) {
        SDL_SetAtomicPointer(&set->lock_owner, NULL);
        pthread_rwlock_unlock(&set->lock);
        property_set_unref(set); /* the lock session's reference */
    }
    property_set_unref(set); /* this call's reference */
}

bool SDL_HasProperty(SDL_PropertiesID props, const char *name)
{
    SDLop_PropertySet *set = property_set_ref(props);
    if (!set || !name) {
        property_set_unref(set);
        return false;
    }
    pthread_rwlock_rdlock(&set->lock);
    bool found = find_property(set, name) != NULL;
    pthread_rwlock_unlock(&set->lock);
    property_set_unref(set);
    return found;
}

bool SDL_ClearProperty(SDL_PropertiesID props, const char *name)
{
    SDLop_PropertySet *set = property_set_ref(props);
    if (!set) {
        return SDL_SetError("Invalid properties ID");
    }
    if (!name) {
        property_set_unref(set);
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
    property_set_unref(set);
    return true;
}

SDL_PropertyType SDL_GetPropertyType(SDL_PropertiesID props, const char *name)
{
    SDLop_PropertySet *set = property_set_ref(props);
    if (!set || !name) {
        property_set_unref(set);
        return SDL_PROPERTY_TYPE_INVALID;
    }
    pthread_rwlock_rdlock(&set->lock);
    SDLop_Property **link = find_property(set, name);
    SDL_PropertyType type = link ? (*link)->type : SDL_PROPERTY_TYPE_INVALID;
    pthread_rwlock_unlock(&set->lock);
    property_set_unref(set);
    return type;
}

bool SDL_EnumerateProperties(SDL_PropertiesID props, SDL_EnumeratePropertiesCallback callback, void *userdata)
{
    SDLop_PropertySet *set = property_set_ref(props);
    if (!set) {
        return SDL_SetError("Invalid properties ID");
    }
    if (!callback) {
        property_set_unref(set);
        return SDL_SetError("Invalid callback");
    }
    /* Like SDL3, the callback runs with the set locked and must not modify
       this property set. */
    pthread_rwlock_rdlock(&set->lock);
    for (SDLop_Property *prop = set->props; prop; prop = prop->next) {
        callback(userdata, props, prop->name);
    }
    pthread_rwlock_unlock(&set->lock);
    property_set_unref(set);
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

/* Set a value of an exact type, overwriting any clashing name in place.
   Caller holds set->lock in write mode. */
static bool set_property_locked(SDLop_PropertySet *set, const char *name, SDL_PropertyType type,
                                const void *value)
{
    SDLop_Property **link = find_property(set, name);
    SDLop_Property *prop = link ? *link : NULL;
    if (prop && prop->type == SDL_PROPERTY_TYPE_STRING) {
        free(prop->value.string);
        prop->value.string = NULL;
    }
    if (!prop) {
        prop = (SDLop_Property *)calloc(1, sizeof(*prop));
        if (!prop) {
            return SDL_OutOfMemory();
        }
        prop->name = strdup(name);
        if (!prop->name) {
            free(prop);
            return SDL_OutOfMemory();
        }
        prop->next = set->props;
        set->props = prop;
    }
    prop->type = type;
    switch (type) {
    case SDL_PROPERTY_TYPE_POINTER:
        prop->value.pointer = *(void *const *)value;
        break;
    case SDL_PROPERTY_TYPE_STRING: {
        const char *str = *(const char *const *)value;
        prop->value.string = strdup(str ? str : "");
        if (!prop->value.string) {
            return SDL_OutOfMemory();
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
        return SDL_SetError("Invalid property type");
    }
    return true;
}

static bool set_property(SDL_PropertiesID props, const char *name, SDL_PropertyType type,
                         const void *value)
{
    SDLop_PropertySet *set = property_set_ref(props);
    if (!set) {
        return SDL_SetError("Invalid properties ID");
    }
    if (!name || !*name) {
        property_set_unref(set);
        return SDL_SetError("Invalid property name");
    }
    pthread_rwlock_wrlock(&set->lock);
    bool ok = set_property_locked(set, name, type, value);
    pthread_rwlock_unlock(&set->lock);
    property_set_unref(set);
    return ok;
}

/* Look up a property of an exact type. Caller must hold set->lock; the
   returned pointer borrows the set's storage and is only valid while the
   set stays locked (SDL3 has the same borrowing rule). */
static SDLop_Property *get_property_locked(SDLop_PropertySet *set, const char *name, SDL_PropertyType type)
{
    SDLop_Property **link = find_property(set, name);
    return (link && (*link)->type == type) ? *link : NULL;
}

bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value)
{
    return set_property(props, name, SDL_PROPERTY_TYPE_NUMBER, &value);
}

Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 default_value)
{
    SDLop_PropertySet *set = property_set_ref(props);
    if (!set || !name) {
        property_set_unref(set);
        return default_value;
    }
    pthread_rwlock_rdlock(&set->lock);
    SDLop_Property **link = find_property(set, name);
    Sint64 result = link ? property_as_number(*link, default_value) : default_value;
    pthread_rwlock_unlock(&set->lock);
    property_set_unref(set);
    return result;
}

bool SDL_SetFloatProperty(SDL_PropertiesID props, const char *name, float value)
{
    return set_property(props, name, SDL_PROPERTY_TYPE_FLOAT, &value);
}

float SDL_GetFloatProperty(SDL_PropertiesID props, const char *name, float default_value)
{
    SDLop_PropertySet *set = property_set_ref(props);
    if (!set || !name) {
        property_set_unref(set);
        return default_value;
    }
    pthread_rwlock_rdlock(&set->lock);
    SDLop_Property **link = find_property(set, name);
    float result = link ? property_as_float(*link, default_value) : default_value;
    pthread_rwlock_unlock(&set->lock);
    property_set_unref(set);
    return result;
}

bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value)
{
    return set_property(props, name, SDL_PROPERTY_TYPE_BOOLEAN, &value);
}

bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value)
{
    SDLop_PropertySet *set = property_set_ref(props);
    if (!set || !name) {
        property_set_unref(set);
        return default_value;
    }
    pthread_rwlock_rdlock(&set->lock);
    SDLop_Property **link = find_property(set, name);
    bool result = link ? property_as_boolean(*link, default_value) : default_value;
    pthread_rwlock_unlock(&set->lock);
    property_set_unref(set);
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
    SDLop_PropertySet *set = property_set_ref(props);
    if (!set || !name) {
        property_set_unref(set);
        return default_value;
    }
    pthread_rwlock_rdlock(&set->lock);
    SDLop_Property *prop = get_property_locked(set, name, SDL_PROPERTY_TYPE_STRING);
    const char *result = prop ? prop->value.string : default_value;
    pthread_rwlock_unlock(&set->lock);
    property_set_unref(set);
    return result;
}

bool SDL_SetPointerProperty(SDL_PropertiesID props, const char *name, void *value)
{
    return set_property(props, name, SDL_PROPERTY_TYPE_POINTER, &value);
}

void *SDL_GetPointerProperty(SDL_PropertiesID props, const char *name, void *default_value)
{
    SDLop_PropertySet *set = property_set_ref(props);
    if (!set || !name) {
        property_set_unref(set);
        return default_value;
    }
    pthread_rwlock_rdlock(&set->lock);
    SDLop_Property *prop = get_property_locked(set, name, SDL_PROPERTY_TYPE_POINTER);
    void *result = prop ? prop->value.pointer : default_value;
    pthread_rwlock_unlock(&set->lock);
    property_set_unref(set);
    return result;
}

bool SDL_CopyProperties(SDL_PropertiesID source, SDL_PropertiesID destination)
{
    if (source == destination) {
        SDLop_PropertySet *set = property_set_ref(source);
        if (!set) {
            return SDL_SetError("Invalid properties ID");
        }
        property_set_unref(set);
        return true; /* copying a set onto itself is the identity */
    }

    SDLop_PropertySet *src = property_set_ref(source);
    SDLop_PropertySet *dst = property_set_ref(destination);
    if (!src) {
        property_set_unref(dst);
        return SDL_SetError("Invalid source properties ID");
    }
    if (!dst) {
        property_set_unref(src);
        return SDL_SetError("Invalid destination properties ID");
    }

    /* Lock both in address order so concurrent A->B and B->A copies can't
       deadlock. Write-locking the source too keeps the order trivially
       correct; copies are rare. */
    SDLop_PropertySet *lo = (src < dst) ? src : dst;
    SDLop_PropertySet *hi = (src < dst) ? dst : src;
    pthread_rwlock_wrlock(&lo->lock);
    pthread_rwlock_wrlock(&hi->lock);
    for (SDLop_Property *prop = src->props; prop; prop = prop->next) {
        switch (prop->type) {
        case SDL_PROPERTY_TYPE_POINTER:
        case SDL_PROPERTY_TYPE_STRING:
        case SDL_PROPERTY_TYPE_NUMBER:
        case SDL_PROPERTY_TYPE_FLOAT:
        case SDL_PROPERTY_TYPE_BOOLEAN:
            (void)set_property_locked(dst, prop->name, prop->type, &prop->value);
            break;
        default:
            break;
        }
    }
    pthread_rwlock_unlock(&hi->lock);
    pthread_rwlock_unlock(&lo->lock);
    property_set_unref(dst);
    property_set_unref(src);
    return true;
}
