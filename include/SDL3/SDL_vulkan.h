/*
  SDLop - a lean, fast, SDL3-compatible windowing + input library.
  Vulkan support (API compatible with <SDL3/SDL_vulkan.h>).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_vulkan_h_
#define SDL_vulkan_h_

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_video.h>

/* Avoid including vulkan.h; don't define VkInstance if it's already included */
#ifdef VULKAN_H_
#define NO_SDL_VULKAN_TYPEDEFS
#endif
#ifndef NO_SDL_VULKAN_TYPEDEFS
#define VK_DEFINE_HANDLE(object) typedef struct object##_T *object;

#if defined(__LP64__) || defined(_WIN64) || defined(__x86_64__) || defined(_M_X64) || \
    defined(__ia64) || defined(_M_IA64) || defined(__aarch64__) || defined(__powerpc64__)
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef struct object##_T *object;
#else
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef uint64_t object;
#endif

VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSurfaceKHR)
struct VkAllocationCallbacks;

/* Make sure to undef to avoid issues in case of later vulkan include */
#undef VK_DEFINE_HANDLE
#undef VK_DEFINE_NON_DISPATCHABLE_HANDLE

#endif /* !NO_SDL_VULKAN_TYPEDEFS */

/**
 * Dynamically load the Vulkan loader library (libvulkan.so.1).
 *
 * \param path platform dependent path or NULL for the default name.
 * \returns true on success or false on failure.
 */
extern bool SDL_Vulkan_LoadLibrary(const char *path);

/**
 * Get the address of the vkGetInstanceProcAddr function.
 */
extern SDL_FunctionPointer SDL_Vulkan_GetVkGetInstanceProcAddr(void);

/**
 * Unload the Vulkan loader library previously loaded by
 * SDL_Vulkan_LoadLibrary().
 */
extern void SDL_Vulkan_UnloadLibrary(void);

/**
 * Get the Vulkan instance extensions needed for window creation.
 *
 * \param count a pointer to an unsigned int filled with the number of
 *              extensions (may be NULL).
 * \returns a NULL-terminated array of extension name strings.
 */
extern char const *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count);

/**
 * Create a Vulkan rendering surface for a window.
 *
 * The Vulkan loader must have been loaded (SDL_Vulkan_LoadLibrary or an
 * earlier call to this function does it implicitly).
 */
extern bool SDL_Vulkan_CreateSurface(SDL_Window *window,
                                     VkInstance instance,
                                     const struct VkAllocationCallbacks *allocator,
                                     VkSurfaceKHR *surface);

/**
 * Destroy a Vulkan rendering surface created by SDL_Vulkan_CreateSurface().
 */
extern void SDL_Vulkan_DestroySurface(VkInstance instance,
                                      VkSurfaceKHR surface,
                                      const struct VkAllocationCallbacks *allocator);

/**
 * Query support for presentation via a given Vulkan queue family.
 */
extern bool SDL_Vulkan_GetPresentationSupport(VkInstance instance,
                                              Uint32 queueFamilyIndex);

#endif /* SDL_vulkan_h_ */
