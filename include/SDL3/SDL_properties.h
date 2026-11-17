/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Properties (subset of <SDL3/SDL_properties.h>, identical names/values/signatures).

  Not implemented (link against real SDL3 if you need them):
    SDL_GetGlobalProperties, SDL_SetPointerPropertyWithCleanup,
    SDL_CleanupPropertyCallback.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_properties_h_
#define SDL_properties_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>

typedef Uint32 SDL_PropertiesID;

typedef enum SDL_PropertyType
{
    SDL_PROPERTY_TYPE_INVALID,
    SDL_PROPERTY_TYPE_POINTER,
    SDL_PROPERTY_TYPE_STRING,
    SDL_PROPERTY_TYPE_NUMBER,
    SDL_PROPERTY_TYPE_FLOAT,
    SDL_PROPERTY_TYPE_BOOLEAN
} SDL_PropertyType;

extern SDL_PropertiesID SDL_CreateProperties(void);
extern void SDL_DestroyProperties(SDL_PropertiesID props);

extern bool SDL_CopyProperties(SDL_PropertiesID source, SDL_PropertiesID destination);

extern bool SDL_LockProperties(SDL_PropertiesID props);
extern void SDL_UnlockProperties(SDL_PropertiesID props);

extern bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value);
extern Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 default_value);
extern bool SDL_SetFloatProperty(SDL_PropertiesID props, const char *name, float value);
extern float SDL_GetFloatProperty(SDL_PropertiesID props, const char *name, float default_value);
extern bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value);
extern bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value);
extern bool SDL_SetStringProperty(SDL_PropertiesID props, const char *name, const char *value);
extern const char *SDL_GetStringProperty(SDL_PropertiesID props, const char *name, const char *default_value);
extern bool SDL_SetPointerProperty(SDL_PropertiesID props, const char *name, void *value);
extern void *SDL_GetPointerProperty(SDL_PropertiesID props, const char *name, void *default_value);

extern bool SDL_HasProperty(SDL_PropertiesID props, const char *name);
extern SDL_PropertyType SDL_GetPropertyType(SDL_PropertiesID props, const char *name);
extern bool SDL_ClearProperty(SDL_PropertiesID props, const char *name);
extern void SDL_ClearProperties(SDL_PropertiesID props);

/** Callback for SDL_EnumerateProperties(); props is the group being enumerated. */
typedef void (*SDL_EnumeratePropertiesCallback)(void *userdata, SDL_PropertiesID props, const char *name);

extern bool SDL_EnumerateProperties(SDL_PropertiesID props, SDL_EnumeratePropertiesCallback callback, void *userdata);

#endif /* SDL_properties_h_ */
