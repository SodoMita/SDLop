/*
  SDLop - properties subsystem (see <SDL3/SDL_properties.h>).

  Simple linked-list backing store; property IDs are unique nonzero
  Uint32 counter values. Main-thread owned; threading helpers are
  API-compatible no-ops.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include "SDL3/SDL_properties.h"
#include <math.h>
#include <strings.h>
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
} SDLop_Property;

typedef struct SDLop_PropertySet
{
    SDL_PropertiesID id;
    struct SDLop_PropertySet *next;
    SDLop_Property *props;
} SDLop_PropertySet;

static SDLop_PropertySet *property_sets;
static SDL_PropertiesID next_property_id;

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
    set->id = ++next_property_id;
    if (set->id == 0) {
        set->id = ++next_property_id; /* skip the reserved 0 */
    }
    set->next = property_sets;
    property_sets = set;
    return set->id;
}

void SDL_ClearProperties(SDL_PropertiesID props)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set) {
        return;
    }
    SDLop_Property *prop = set->props;
    while (prop) {
        SDLop_Property *next = prop->next;
        clear_property(prop);
        prop = next;
    }
    set->props = NULL;
}

void SDL_DestroyProperties(SDL_PropertiesID props)
{
    for (SDLop_PropertySet **link = &property_sets; *link; link = &(*link)->next) {
        SDLop_PropertySet *set = *link;
        if (set->id == props) {
            SDL_ClearProperties(props);
            *link = set->next;
            free(set);
            return;
        }
    }
}

bool SDL_LockProperties(SDL_PropertiesID props)
{
    (void)props;
    return true; /* main-thread owned */
}

void SDL_UnlockProperties(SDL_PropertiesID props)
{
    (void)props;
}

bool SDL_HasProperty(SDL_PropertiesID props, const char *name)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    return set && name && find_property(set, name) != NULL;
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
    SDLop_Property **link = find_property(set, name);
    if (!link) {
        return true; /* nothing to clear: success, like SDL3 */
    }
    SDLop_Property *prop = *link;
    *link = prop->next;
    clear_property(prop);
    return true;
}

SDL_PropertyType SDL_GetPropertyType(SDL_PropertiesID props, const char *name)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set || !name) {
        return SDL_PROPERTY_TYPE_INVALID;
    }
    SDLop_Property **link = find_property(set, name);
    return link ? (*link)->type : SDL_PROPERTY_TYPE_INVALID;
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
    for (SDLop_Property *prop = set->props; prop; prop = prop->next) {
        callback(userdata, props, prop->name);
    }
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

/* Set a value of an exact type; overwrites any clashing name. */
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
    SDL_ClearProperty(props, name);
    SDLop_Property *prop = (SDLop_Property *)calloc(1, sizeof(*prop));
    if (!prop) {
        return SDL_OutOfMemory();
    }
    prop->name = strdup(name);
    if (!prop->name) {
        free(prop);
        return SDL_OutOfMemory();
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
            free(prop->name);
            free(prop);
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
        free(prop->name);
        free(prop);
        return SDL_SetError("Invalid property type");
    }
    prop->next = set->props;
    set->props = prop;
    return true;
}

static SDLop_Property *get_property(SDL_PropertiesID props, const char *name, SDL_PropertyType type)
{
    SDLop_PropertySet *set = property_set_by_id(props);
    if (!set || !name) {
        return NULL;
    }
    SDLop_Property **link = find_property(set, name);
    if (!link || (*link)->type != type) {
        return NULL;
    }
    return *link;
}

bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value)
{
    return set_property(props, name, SDL_PROPERTY_TYPE_NUMBER, &value);
}

Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 default_value)
{
    SDLop_Property *prop = find_any(property_set_by_id(props), name);
    return prop ? property_as_number(prop, default_value) : default_value;
}

bool SDL_SetFloatProperty(SDL_PropertiesID props, const char *name, float value)
{
    return set_property(props, name, SDL_PROPERTY_TYPE_FLOAT, &value);
}

float SDL_GetFloatProperty(SDL_PropertiesID props, const char *name, float default_value)
{
    SDLop_Property *prop = find_any(property_set_by_id(props), name);
    return prop ? property_as_float(prop, default_value) : default_value;
}

bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value)
{
    return set_property(props, name, SDL_PROPERTY_TYPE_BOOLEAN, &value);
}

bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value)
{
    SDLop_Property *prop = find_any(property_set_by_id(props), name);
    return prop ? property_as_boolean(prop, default_value) : default_value;
}

bool SDL_SetStringProperty(SDL_PropertiesID props, const char *name, const char *value)
{
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
    return true;
}
