/*
  SDLop -- SDL_properties.h.

  A property bag is a small array of name/value pairs. Bags are looked up by ID in
  one global table; individual bags are used from the thread that owns them (the
  same contract SDL3 documents for window properties).
*/

#include "../sdlop_internal.h"

#include <string.h>
#include <pthread.h>

typedef struct SDLOP_Property
{
    char *name;
    SDL_PropertyType type;
    SDL_CleanupPropertyCallback cleanup;   /* pointer properties only */
    void *cleanup_userdata;
    union
    {
        Sint64 number;
        float floating;
        bool boolean;
        char *string;
        void *pointer;
    } value;
} SDLOP_Property;

typedef struct SDLOP_PropertyBag
{
    SDL_PropertiesID id;
    int count;
    int capacity;
    bool is_global;
    pthread_mutex_t lock;                  /* taken by SDL_LockProperties() */
    SDLOP_Property *props;
} SDLOP_PropertyBag;

/* free a property's payload, running the cleanup callback for pointer values */
static void sdlop_dispose_property(SDLOP_Property *prop)
{
    if (!prop) {
        return;
    }
    if (prop->type == SDL_PROPERTY_TYPE_STRING) {
        SDLOP_Free(prop->value.string);
    } else if (prop->type == SDL_PROPERTY_TYPE_POINTER && prop->cleanup) {
        prop->cleanup(prop->cleanup_userdata, prop->value.pointer);
    }
    prop->cleanup = NULL;
    prop->cleanup_userdata = NULL;
    prop->value.pointer = NULL;
}

static pthread_mutex_t sdlop_props_lock = PTHREAD_MUTEX_INITIALIZER;
static SDLOP_PropertyBag **sdlop_bags;
static int sdlop_num_bags;
static int sdlop_num_bags_capacity;
static SDL_PropertiesID sdlop_next_props_id = 1;
static SDLOP_PropertyBag *sdlop_global_bag;

static SDLOP_PropertyBag *sdlop_bag_for(SDL_PropertiesID id)
{
    int i;
    for (i = 0; i < sdlop_num_bags; i++) {
        if (sdlop_bags[i]->id == id) {
            return sdlop_bags[i];
        }
    }
    return NULL;
}

static SDLOP_Property *sdlop_prop_for(SDLOP_PropertyBag *bag, const char *name, bool create)
{
    int i;
    if (!bag || !name || !name[0]) {
        return NULL;
    }
    for (i = 0; i < bag->count; i++) {
        if (strcmp(bag->props[i].name, name) == 0) {
            return &bag->props[i];
        }
    }
    if (!create) {
        return NULL;
    }
    if (bag->count == bag->capacity) {
        int newcap = bag->capacity ? bag->capacity * 2 : 4;
        SDLOP_Property *newprops = (SDLOP_Property *)SDLOP_Realloc(bag->props, (size_t)newcap * sizeof(*newprops));
        if (!newprops) {
            SDLOP_OutOfMemory();
            return NULL;
        }
        bag->props = newprops;
        bag->capacity = newcap;
    }
    memset(&bag->props[bag->count], 0, sizeof(bag->props[bag->count]));
    bag->props[bag->count].name = SDL_strdup(name);
    if (!bag->props[bag->count].name) {
        SDLOP_OutOfMemory();
        return NULL;
    }
    bag->props[bag->count].type = SDL_PROPERTY_TYPE_INVALID;
    return &bag->props[bag->count++];
}

static SDLOP_PropertyBag *sdlop_create_bag(bool is_global)
{
    SDLOP_PropertyBag *bag = (SDLOP_PropertyBag *)SDLOP_Calloc(1, sizeof(*bag));
    if (!bag) {
        SDLOP_OutOfMemory();
        return 0;
    }
    pthread_mutex_init(&bag->lock, NULL);
    pthread_mutex_lock(&sdlop_props_lock);
    if (sdlop_num_bags == sdlop_num_bags_capacity) {
        int newcap = sdlop_num_bags_capacity ? sdlop_num_bags_capacity * 2 : 8;
        SDLOP_PropertyBag **newbags = (SDLOP_PropertyBag **)SDLOP_Realloc(sdlop_bags, (size_t)newcap * sizeof(*newbags));
        if (!newbags) {
            pthread_mutex_unlock(&sdlop_props_lock);
            SDLOP_Free(bag);
            SDLOP_OutOfMemory();
            return 0;
        }
        sdlop_bags = newbags;
        sdlop_num_bags_capacity = newcap;
    }
    bag->id = sdlop_next_props_id++;
    bag->is_global = is_global;
    sdlop_bags[sdlop_num_bags++] = bag;
    pthread_mutex_unlock(&sdlop_props_lock);
    return bag;
}

SDL_PropertiesID SDL_CreateProperties(void)
{
    SDLOP_PropertyBag *bag = sdlop_create_bag(false);
    return bag ? bag->id : 0;
}

SDL_PropertiesID SDL_GetGlobalProperties(void)
{
    SDLOP_PropertyBag *bag;
    pthread_mutex_lock(&sdlop_props_lock);
    bag = sdlop_global_bag;
    pthread_mutex_unlock(&sdlop_props_lock);
    if (!bag) {
        bag = sdlop_create_bag(true);
        if (!bag) {
            return 0;
        }
        pthread_mutex_lock(&sdlop_props_lock);
        if (!sdlop_global_bag) {
            sdlop_global_bag = bag;
        } else {
            /* another thread won the race; drop ours */
            bag = sdlop_global_bag;
        }
        pthread_mutex_unlock(&sdlop_props_lock);
    }
    return bag->id;
}

bool SDL_HasProperty(SDL_PropertiesID props, const char *name)
{
    return sdlop_prop_for(sdlop_bag_for(props), name, false) != NULL;
}

SDL_PropertyType SDL_GetPropertyType(SDL_PropertiesID props, const char *name)
{
    SDLOP_Property *prop = sdlop_prop_for(sdlop_bag_for(props), name, false);
    return prop ? prop->type : SDL_PROPERTY_TYPE_INVALID;
}

static bool sdlop_set_prop(SDL_PropertiesID props, const char *name, SDL_PropertyType type)
{
    SDLOP_PropertyBag *bag = sdlop_bag_for(props);
    SDLOP_Property *prop;
    if (!bag || !name || !name[0]) {
        return SDL_InvalidParamError(name ? "properties" : "name");
    }
    prop = sdlop_prop_for(bag, name, true);
    if (!prop) {
        return false;
    }
    sdlop_dispose_property(prop);
    prop->type = type;
    return true;
}

bool SDL_SetStringProperty(SDL_PropertiesID props, const char *name, const char *value)
{
    SDLOP_Property *prop;
    if (!sdlop_set_prop(props, name, SDL_PROPERTY_TYPE_STRING)) {
        return false;
    }
    prop = sdlop_prop_for(sdlop_bag_for(props), name, false);
    prop->value.string = value ? SDL_strdup(value) : NULL;
    return value ? (prop->value.string != NULL) : true;
}

bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value)
{
    SDLOP_Property *prop;
    if (!sdlop_set_prop(props, name, SDL_PROPERTY_TYPE_NUMBER)) {
        return false;
    }
    prop = sdlop_prop_for(sdlop_bag_for(props), name, false);
    prop->value.number = value;
    return true;
}

bool SDL_SetFloatProperty(SDL_PropertiesID props, const char *name, float value)
{
    SDLOP_Property *prop;
    if (!sdlop_set_prop(props, name, SDL_PROPERTY_TYPE_FLOAT)) {
        return false;
    }
    prop = sdlop_prop_for(sdlop_bag_for(props), name, false);
    prop->value.floating = value;
    return true;
}

bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value)
{
    SDLOP_Property *prop;
    if (!sdlop_set_prop(props, name, SDL_PROPERTY_TYPE_BOOLEAN)) {
        return false;
    }
    prop = sdlop_prop_for(sdlop_bag_for(props), name, false);
    prop->value.boolean = value;
    return true;
}

const char *SDL_GetStringProperty(SDL_PropertiesID props, const char *name, const char *default_value)
{
    SDLOP_Property *prop = sdlop_prop_for(sdlop_bag_for(props), name, false);
    if (prop && prop->type == SDL_PROPERTY_TYPE_STRING) {
        return prop->value.string;
    }
    return default_value;
}

float SDL_GetFloatProperty(SDL_PropertiesID props, const char *name, float default_value)
{
    SDLOP_Property *prop = sdlop_prop_for(sdlop_bag_for(props), name, false);
    if (prop && prop->type == SDL_PROPERTY_TYPE_FLOAT) {
        return prop->value.floating;
    }
    return default_value;
}

bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value)
{
    SDLOP_Property *prop = sdlop_prop_for(sdlop_bag_for(props), name, false);
    if (prop && prop->type == SDL_PROPERTY_TYPE_BOOLEAN) {
        return prop->value.boolean;
    }
    return default_value;
}

Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 default_value)
{
    SDLOP_Property *prop = sdlop_prop_for(sdlop_bag_for(props), name, false);
    if (prop && prop->type == SDL_PROPERTY_TYPE_NUMBER) {
        return prop->value.number;
    }
    return default_value;
}

bool SDL_SetPointerPropertyWithCleanup(SDL_PropertiesID props, const char *name, void *value,
                                       SDL_CleanupPropertyCallback cleanup, void *userdata)
{
    SDLOP_Property *prop;
    if (!sdlop_set_prop(props, name, SDL_PROPERTY_TYPE_POINTER)) {
        return false;
    }
    prop = sdlop_prop_for(sdlop_bag_for(props), name, false);
    prop->value.pointer = value;
    prop->cleanup = cleanup;
    prop->cleanup_userdata = userdata;
    return true;
}

bool SDL_SetPointerProperty(SDL_PropertiesID props, const char *name, void *value)
{
    return SDL_SetPointerPropertyWithCleanup(props, name, value, NULL, NULL);
}

void *SDL_GetPointerProperty(SDL_PropertiesID props, const char *name, void *default_value)
{
    SDLOP_Property *prop = sdlop_prop_for(sdlop_bag_for(props), name, false);
    if (prop && prop->type == SDL_PROPERTY_TYPE_POINTER) {
        return prop->value.pointer;
    }
    return default_value;
}

bool SDL_ClearProperty(SDL_PropertiesID props, const char *name)
{
    SDLOP_PropertyBag *bag = sdlop_bag_for(props);
    int i;
    if (!bag || !name) {
        return SDL_InvalidParamError(name ? "properties" : "name");
    }
    for (i = 0; i < bag->count; i++) {
        if (strcmp(bag->props[i].name, name) == 0) {
            sdlop_dispose_property(&bag->props[i]);
            SDLOP_Free(bag->props[i].name);
            memmove(&bag->props[i], &bag->props[i + 1],
                    (size_t)(bag->count - i - 1) * sizeof(bag->props[0]));
            bag->count--;
            return true;
        }
    }
    return true;
}

bool SDL_EnumerateProperties(SDL_PropertiesID props, SDL_EnumeratePropertiesCallback callback, void *userdata)
{
    SDLOP_PropertyBag *bag = sdlop_bag_for(props);
    int i;
    if (!bag || !callback) {
        return SDL_InvalidParamError(callback ? "properties" : "callback");
    }
    for (i = 0; i < bag->count; i++) {
        callback(userdata, props, bag->props[i].name);
    }
    return true;
}

bool SDL_CopyProperties(SDL_PropertiesID src, SDL_PropertiesID dst)
{
    SDLOP_PropertyBag *srcbag = sdlop_bag_for(src);
    int i;
    if (!srcbag || !sdlop_bag_for(dst)) {
        return SDL_InvalidParamError("properties");
    }
    for (i = 0; i < srcbag->count; i++) {
        SDLOP_Property *p = &srcbag->props[i];
        switch (p->type) {
            case SDL_PROPERTY_TYPE_STRING:
                if (!SDL_SetStringProperty(dst, p->name, p->value.string)) return false;
                break;
            case SDL_PROPERTY_TYPE_NUMBER:
                if (!SDL_SetNumberProperty(dst, p->name, p->value.number)) return false;
                break;
            case SDL_PROPERTY_TYPE_FLOAT:
                if (!SDL_SetFloatProperty(dst, p->name, p->value.floating)) return false;
                break;
            case SDL_PROPERTY_TYPE_BOOLEAN:
                if (!SDL_SetBooleanProperty(dst, p->name, p->value.boolean)) return false;
                break;
            case SDL_PROPERTY_TYPE_POINTER:
                if (!SDL_SetPointerPropertyWithCleanup(dst, p->name, p->value.pointer,
                                                       p->cleanup, p->cleanup_userdata)) return false;
                break;
            default:
                break;
        }
    }
    return true;
}

void SDL_DestroyProperties(SDL_PropertiesID props)
{
    SDLOP_PropertyBag *bag = sdlop_bag_for(props);
    int i;
    if (!bag || bag->is_global) {
        return;
    }
    for (i = 0; i < bag->count; i++) {
        sdlop_dispose_property(&bag->props[i]);
        SDLOP_Free(bag->props[i].name);
    }
    SDLOP_Free(bag->props);
    pthread_mutex_destroy(&bag->lock);
    pthread_mutex_lock(&sdlop_props_lock);
    for (i = 0; i < sdlop_num_bags; i++) {
        if (sdlop_bags[i] == bag) {
            memmove(&sdlop_bags[i], &sdlop_bags[i + 1],
                    (size_t)(sdlop_num_bags - i - 1) * sizeof(sdlop_bags[0]));
            sdlop_num_bags--;
            break;
        }
    }
    pthread_mutex_unlock(&sdlop_props_lock);
    SDLOP_Free(bag);
}

bool SDL_LockProperties(SDL_PropertiesID props)
{
    SDLOP_PropertyBag *bag = sdlop_bag_for(props);
    if (!bag) {
        return SDL_InvalidParamError("props");
    }
    /* SDL3 makes this lock recursive (apps take it from a callback they
       registered for the same bag); a recursive mutex gives the same contract. */
    pthread_mutex_lock(&bag->lock);
    return true;
}

void SDL_UnlockProperties(SDL_PropertiesID props)
{
    SDLOP_PropertyBag *bag = sdlop_bag_for(props);
    if (bag) {
        pthread_mutex_unlock(&bag->lock);
    }
}
