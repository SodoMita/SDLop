/*
  SDLop -- a lean reimplementation of the SDL3 API: windowing, input, timing.

  GENERATED FILE -- do not edit.  Produced by tools/sdlop.py from the upstream SDL3
  header SDL_properties.h (3.2.10), which is zlib licensed, Copyright (C) 1997-2025 Sam Lantinga
  and SDL contributors.  Declarations are copied verbatim so that source and binary
  compatibility with SDL3 are exact; the items SDLop does not implement were
  removed (the full list is in tools/dropped.txt).

  In this header: Property bags (used by SDL_CreateWindowWithProperties and SDL_GetWindowProperties).
*/

#ifndef SDL_properties_h_
#define SDL_properties_h_

#include <SDL3/SDL_begin_code.h>

#ifdef __cplusplus
extern "C" {
#endif

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

extern SDL_DECLSPEC SDL_PropertiesID SDLCALL SDL_GetGlobalProperties(void);

extern SDL_DECLSPEC SDL_PropertiesID SDLCALL SDL_CreateProperties(void);

extern SDL_DECLSPEC bool SDLCALL SDL_CopyProperties(SDL_PropertiesID src, SDL_PropertiesID dst);

extern SDL_DECLSPEC bool SDLCALL SDL_LockProperties(SDL_PropertiesID props);

extern SDL_DECLSPEC void SDLCALL SDL_UnlockProperties(SDL_PropertiesID props);

typedef void (SDLCALL *SDL_CleanupPropertyCallback)(void *userdata, void *value);

extern SDL_DECLSPEC bool SDLCALL SDL_SetPointerPropertyWithCleanup(SDL_PropertiesID props, const char *name, void *value, SDL_CleanupPropertyCallback cleanup, void *userdata);

extern SDL_DECLSPEC bool SDLCALL SDL_SetPointerProperty(SDL_PropertiesID props, const char *name, void *value);

extern SDL_DECLSPEC bool SDLCALL SDL_SetStringProperty(SDL_PropertiesID props, const char *name, const char *value);

extern SDL_DECLSPEC bool SDLCALL SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value);

extern SDL_DECLSPEC bool SDLCALL SDL_SetFloatProperty(SDL_PropertiesID props, const char *name, float value);

extern SDL_DECLSPEC bool SDLCALL SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value);

extern SDL_DECLSPEC bool SDLCALL SDL_HasProperty(SDL_PropertiesID props, const char *name);

extern SDL_DECLSPEC SDL_PropertyType SDLCALL SDL_GetPropertyType(SDL_PropertiesID props, const char *name);

extern SDL_DECLSPEC void * SDLCALL SDL_GetPointerProperty(SDL_PropertiesID props, const char *name, void *default_value);

extern SDL_DECLSPEC const char * SDLCALL SDL_GetStringProperty(SDL_PropertiesID props, const char *name, const char *default_value);

extern SDL_DECLSPEC Sint64 SDLCALL SDL_GetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 default_value);

extern SDL_DECLSPEC float SDLCALL SDL_GetFloatProperty(SDL_PropertiesID props, const char *name, float default_value);

extern SDL_DECLSPEC bool SDLCALL SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value);

extern SDL_DECLSPEC bool SDLCALL SDL_ClearProperty(SDL_PropertiesID props, const char *name);

typedef void (SDLCALL *SDL_EnumeratePropertiesCallback)(void *userdata, SDL_PropertiesID props, const char *name);

extern SDL_DECLSPEC bool SDLCALL SDL_EnumerateProperties(SDL_PropertiesID props, SDL_EnumeratePropertiesCallback callback, void *userdata);

extern SDL_DECLSPEC void SDLCALL SDL_DestroyProperties(SDL_PropertiesID props);

#include <SDL3/SDL_close_code.h>

#ifdef __cplusplus
}
#endif

#endif /* SDL_properties_h_ */
