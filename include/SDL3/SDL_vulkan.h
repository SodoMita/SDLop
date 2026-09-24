/*
  SDLop -- a lean reimplementation of the SDL3 API: windowing, input, timing.

  GENERATED FILE -- do not edit.  Produced by tools/sdlop.py from the upstream SDL3
  header SDL_vulkan.h (3.2.10), which is zlib licensed, Copyright (C) 1997-2025 Sam Lantinga
  and SDL contributors.  Declarations are copied verbatim so that source and binary
  compatibility with SDL3 are exact; the items SDLop does not implement were
  removed (the full list is in tools/dropped.txt).

  In this header: Vulkan surface creation, including upstream's VkInstance/VkSurfaceKHR typedefs.
*/

#ifndef SDL_vulkan_h_
#define SDL_vulkan_h_

#include <SDL3/SDL_begin_code.h>
#include <SDL3/SDL_video.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef VULKAN_H_

#define NO_SDL_VULKAN_TYPEDEFS

#endif

#ifndef NO_SDL_VULKAN_TYPEDEFS

#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;

#if defined(__LP64__) || defined(_WIN64) || defined(__x86_64__) || defined(_M_X64) || defined(__ia64) || defined (_M_IA64) || defined(__aarch64__) || defined(__powerpc64__)

#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef struct object##_T *object;

#else

#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef uint64_t object;

#endif

VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSurfaceKHR)
struct VkAllocationCallbacks;

#undef VK_DEFINE_HANDLE

#undef VK_DEFINE_NON_DISPATCHABLE_HANDLE

#endif

extern SDL_DECLSPEC bool SDLCALL SDL_Vulkan_LoadLibrary(const char *path);

extern SDL_DECLSPEC SDL_FunctionPointer SDLCALL SDL_Vulkan_GetVkGetInstanceProcAddr(void);

extern SDL_DECLSPEC void SDLCALL SDL_Vulkan_UnloadLibrary(void);

extern SDL_DECLSPEC char const * const * SDLCALL SDL_Vulkan_GetInstanceExtensions(Uint32 *count);

extern SDL_DECLSPEC bool SDLCALL SDL_Vulkan_CreateSurface(SDL_Window *window,
 VkInstance instance,
 const struct VkAllocationCallbacks *allocator,
 VkSurfaceKHR *surface);

extern SDL_DECLSPEC void SDLCALL SDL_Vulkan_DestroySurface(VkInstance instance,
 VkSurfaceKHR surface,
 const struct VkAllocationCallbacks *allocator);

extern SDL_DECLSPEC bool SDLCALL SDL_Vulkan_GetPresentationSupport(VkInstance instance,
 VkPhysicalDevice physicalDevice,
 Uint32 queueFamilyIndex);

#include <SDL3/SDL_close_code.h>

#ifdef __cplusplus
}
#endif

#endif /* SDL_vulkan_h_ */
