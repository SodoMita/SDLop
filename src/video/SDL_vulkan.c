/*
  SDLop -- Vulkan surface creation.

  Same API as SDL3's SDL_vulkan.h:

      SDL_Vulkan_LoadLibrary / SDL_Vulkan_UnloadLibrary
      SDL_Vulkan_GetVkGetInstanceProcAddr
      SDL_Vulkan_GetInstanceExtensions
      SDL_Vulkan_CreateSurface / SDL_Vulkan_DestroySurface
      SDL_Vulkan_GetPresentationSupport

  The Vulkan loader is dlopen()ed on demand (there is no link-time dependency on
  libvulkan), exactly like SDL3 does, and everything platform specific is asked of
  the video driver: it knows its surface extension, its vkCreate*SurfaceKHR and
  its native handles, so this file is the same on Wayland and on X11.

  Nothing here needs the Vulkan headers: SDL_vulkan.h defines VkInstance,
  VkPhysicalDevice, VkSurfaceKHR and struct VkAllocationCallbacks itself, and the
  entry points are looked up by name, so the build works on machines that only
  have the runtime (or nothing at all).
*/

#include "sdlop_internal.h"

#include <SDL3/SDL_vulkan.h>

#include <dlfcn.h>

/* ---------------------------------------------------------------- Vulkan ABI */

/* Only the pieces every backend needs live here: the created-surface and
   presentation-support functions are per platform, so each backend declares its
   own (see SDL_wayland.c and SDL_x11.c). No Vulkan headers are required - the
   loader is dlopen()ed and its entry points are looked up by name, so the build
   works on a machine that has no Vulkan SDK at all. */

#define VK_SUCCESS 0

typedef void *(*PFN_vkGetInstanceProcAddr)(VkInstance instance, const char *name);
typedef void (*PFN_vkDestroySurfaceKHR)(VkInstance instance, VkSurfaceKHR surface,
                                        const struct VkAllocationCallbacks *allocator);

/* ------------------------------------------------------------- loader state */

static void *sdlop_vulkan_handle;
static char sdlop_vulkan_path[256];
static int sdlop_vulkan_refcount;
static PFN_vkGetInstanceProcAddr sdlop_vk_get_instance_proc_addr;

#define SDLOP_VULKAN_DEFAULT "libvulkan.so.1"

static const char *sdlop_vulkan_result_string_impl(int result)
{
    switch (result) {
    case -1: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case -2: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case -3: return "VK_ERROR_INITIALIZATION_FAILED";
    case -4: return "VK_ERROR_DEVICE_LOST";
    case -5: return "VK_ERROR_MEMORY_MAP_FAILED";
    case -6: return "VK_ERROR_LAYER_NOT_PRESENT";
    case -7: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case -8: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case -9: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case -11: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case -12: return "VK_ERROR_SURFACE_LOST_KHR";
    case -1000000000: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: break;
    }
    return "unknown error";
}

const char *SDLOP_VulkanResultString(int result)
{
    return sdlop_vulkan_result_string_impl(result);
}

/* A driver supports Vulkan when it can create a surface and name the instance
   extensions it needs - the same three hooks the public API below is built on.
   Offscreen has none of them, which is exactly SDL3's "Vulkan is not supported by
   the <driver> driver". */
static bool sdlop_vulkan_supported(void)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();

    return driver && driver->create_vulkan_surface && driver->get_vulkan_instance_extensions &&
           driver->vulkan_presentation_support;
}



bool SDLOP_VulkanLoad(const char *path)
{
    SDL_FunctionPointer proc;

    if (!sdlop_vulkan_supported()) {
        return SDL_SetError("Vulkan is not supported by the %s video driver",
                            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");
    }
    if (sdlop_vulkan_refcount > 0) {
        if (path && SDL_strcmp(path, sdlop_vulkan_path) != 0) {
            return SDL_SetError("Vulkan loader library already loaded");
        }
        sdlop_vulkan_refcount++;
        return true;
    }
    if (!path) {
        path = SDL_GetHint(SDL_HINT_VULKAN_LIBRARY);
    }
    if (!path || !*path) {
        path = SDLOP_VULKAN_DEFAULT;
    }
    sdlop_vulkan_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!sdlop_vulkan_handle) {
        const char *reason = dlerror();
        return SDL_SetError("Couldn't load the Vulkan loader '%s': %s", path,
                            reason ? reason : "unknown error");
    }
    proc = (SDL_FunctionPointer)dlsym(sdlop_vulkan_handle, "vkGetInstanceProcAddr");
    if (!proc) {
        dlclose(sdlop_vulkan_handle);
        sdlop_vulkan_handle = NULL;
        return SDL_SetError("Couldn't find vkGetInstanceProcAddr in '%s'", path);
    }
    sdlop_vk_get_instance_proc_addr = (PFN_vkGetInstanceProcAddr)proc;
    SDL_strlcpy(sdlop_vulkan_path, path, sizeof(sdlop_vulkan_path));
    sdlop_vulkan_refcount = 1;
    return true;
}

void SDLOP_VulkanUnload(void)
{
    if (sdlop_vulkan_refcount > 0 && --sdlop_vulkan_refcount == 0) {
        if (sdlop_vulkan_handle) {
            dlclose(sdlop_vulkan_handle);
        }
        sdlop_vulkan_handle = NULL;
        sdlop_vk_get_instance_proc_addr = NULL;
        sdlop_vulkan_path[0] = '\0';
    }
}

/* The entry point a backend needs to do its platform's work (the loader's own
   vkGetInstanceProcAddr, with the instance already loaded). */
SDL_FunctionPointer SDLOP_VulkanGetInstanceProc(VkInstance instance, const char *name)
{
    if (!sdlop_vk_get_instance_proc_addr) {
        return NULL;
    }
    return (SDL_FunctionPointer)sdlop_vk_get_instance_proc_addr(instance, name);
}

void SDLOP_VulkanCleanup(void)
{
    while (sdlop_vulkan_refcount > 0) {
        SDLOP_VulkanUnload();
    }
}

/* --------------------------------------------------------------- public API */

bool SDL_Vulkan_LoadLibrary(const char *path)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        return SDL_SetError("Video subsystem has not been initialized");
    }
    return SDLOP_VulkanLoad(path);
}

SDL_FunctionPointer SDL_Vulkan_GetVkGetInstanceProcAddr(void)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_SetError("Video subsystem has not been initialized");
        return NULL;
    }
    if (!sdlop_vulkan_refcount) {
        SDL_SetError("No Vulkan loader has been loaded");
        return NULL;
    }
    return (SDL_FunctionPointer)sdlop_vk_get_instance_proc_addr;
}

void SDL_Vulkan_UnloadLibrary(void)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_SetError("Video subsystem has not been initialized");
        return;
    }
    SDLOP_VulkanUnload();
}

char const *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();

    /* The names are the backend's: VK_KHR_surface plus whatever its platform
       needs (VK_KHR_wayland_surface, VK_KHR_xlib_surface, ...). */
    if (!sdlop_vulkan_supported()) {
        SDL_SetError("Vulkan is not supported by the %s video driver",
                     SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");
        return NULL;
    }
    return driver->get_vulkan_instance_extensions(count);
}

bool SDL_Vulkan_CreateSurface(SDL_Window *window,
                              VkInstance instance,
                              const struct VkAllocationCallbacks *allocator,
                              VkSurfaceKHR *surface)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();

    if (!window) {
        return SDL_InvalidParamError("window");
    }
    if (!instance) {
        return SDL_InvalidParamError("instance");
    }
    if (!surface) {
        return SDL_InvalidParamError("surface");
    }
    if (!(window->flags & SDL_WINDOW_VULKAN)) {
        return SDL_SetError("The specified window isn't a Vulkan window");
    }
    if (!sdlop_vulkan_handle) {
        return SDL_SetError("Vulkan is not loaded");
    }
    if (!sdlop_vulkan_supported()) {
        return SDL_SetError("Vulkan is not supported by the %s video driver",
                            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");
    }
    return driver->create_vulkan_surface(window, instance, allocator, surface);
}

void SDL_Vulkan_DestroySurface(VkInstance instance,
                               VkSurfaceKHR surface,
                               const struct VkAllocationCallbacks *allocator)
{
    PFN_vkDestroySurfaceKHR destroy_surface;

    if (!instance || !surface || !sdlop_vk_get_instance_proc_addr) {
        return;
    }
    destroy_surface = (PFN_vkDestroySurfaceKHR)sdlop_vk_get_instance_proc_addr(
        instance, "vkDestroySurfaceKHR");
    if (destroy_surface) {
        destroy_surface(instance, surface, allocator);
    }
}

bool SDL_Vulkan_GetPresentationSupport(VkInstance instance,
                                       VkPhysicalDevice physicalDevice,
                                       Uint32 queueFamilyIndex)
{
    const SDLOP_VideoDriver *driver = SDLOP_GetVideoDriver();

    if (!instance) {
        return SDL_InvalidParamError("instance");
    }
    if (!physicalDevice) {
        return SDL_InvalidParamError("physicalDevice");
    }
    if (!sdlop_vk_get_instance_proc_addr) {
        return SDL_SetError("Vulkan is not loaded");
    }
    if (!sdlop_vulkan_supported()) {
        return SDL_SetError("Vulkan is not supported by this video driver");
    }
    /* Whether this queue family can present to the platform's display is the
       backend's question: it owns the protocol/visual the query needs. */
    return driver->vulkan_presentation_support(instance, physicalDevice, queueFamilyIndex);
}
