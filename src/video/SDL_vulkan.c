/*
  SDLop -- Vulkan surface creation.

  Same API as SDL3's SDL_vulkan.h:

      SDL_Vulkan_LoadLibrary / SDL_Vulkan_UnloadLibrary
      SDL_Vulkan_GetVkGetInstanceProcAddr
      SDL_Vulkan_GetInstanceExtensions
      SDL_Vulkan_CreateSurface / SDL_Vulkan_DestroySurface
      SDL_Vulkan_GetPresentationSupport

  The Vulkan loader is dlopen()ed on demand (there is no link-time dependency on
  libvulkan), exactly like SDL3 does, and the surface is created through the
  window's Wayland surface with vkCreateWaylandSurfaceKHR.

  Nothing here needs the Vulkan headers: SDL_vulkan.h defines VkInstance,
  VkPhysicalDevice, VkSurfaceKHR and struct VkAllocationCallbacks itself, and the
  entry points are looked up by name, so the build works on machines that only
  have the runtime (or nothing at all).
*/

#include "sdlop_internal.h"

#include <SDL3/SDL_vulkan.h>

#ifdef SDLOP_HAVE_WAYLAND
#include <wayland-client.h>
#endif

#include <dlfcn.h>

/* ---------------------------------------------------------------- Vulkan ABI */

#define VK_SUCCESS 0
#define VK_INCOMPLETE 5

#define VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR 1000008000
#define VK_KHR_SURFACE_EXTENSION_NAME "VK_KHR_surface"
#define VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME "VK_KHR_wayland_surface"

typedef void *(*PFN_vkGetInstanceProcAddr)(VkInstance instance, const char *name);
typedef int (*PFN_vkCreateWaylandSurfaceKHR)(VkInstance instance,
                                            const void *create_info,
                                            const struct VkAllocationCallbacks *allocator,
                                            VkSurfaceKHR *surface);
typedef void (*PFN_vkDestroySurfaceKHR)(VkInstance instance, VkSurfaceKHR surface,
                                        const struct VkAllocationCallbacks *allocator);
typedef bool (*PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)(VkPhysicalDevice physical_device,
                                                                     Uint32 queue_family_index,
                                                                     struct wl_display *display);

/* struct VkWaylandSurfaceCreateInfoKHR */
typedef struct
{
    Uint32 sType;
    const void *pNext;
    Uint32 flags;
    struct wl_display *display;
    struct wl_surface *surface;
} SDLOP_VkWaylandSurfaceCreateInfoKHR;

/* ------------------------------------------------------------- loader state */

static void *sdlop_vulkan_handle;
static char sdlop_vulkan_path[256];
static int sdlop_vulkan_refcount;
static PFN_vkGetInstanceProcAddr sdlop_vk_get_instance_proc_addr;

#define SDLOP_VULKAN_DEFAULT "libvulkan.so.1"

static const char *sdlop_vulkan_result_string(int result)
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

/* SDLop presents through Wayland; anything else has no WSI functions,
   which is what SDL3 reports as "Vulkan is not supported by the <driver> driver".*/
static bool sdlop_vulkan_supported(void)
{
#ifdef SDLOP_HAVE_WAYLAND
    const char *driver = SDL_GetCurrentVideoDriver();
    return driver && SDL_strcmp(driver, "wayland") == 0;
#else
    return false;
#endif
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
#ifdef SDLOP_HAVE_WAYLAND
    static const char *const extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    };
#endif

    if (!sdlop_vulkan_supported()) {
        SDL_SetError("Vulkan is not supported by the %s video driver",
                     SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");
        return NULL;
    }
#ifdef SDLOP_HAVE_WAYLAND
    if (count) {
        *count = SDL_arraysize(extensions);
    }
    return extensions;
#else
    if (count) {
        *count = 0;
    }
    return NULL;
#endif
}

bool SDL_Vulkan_CreateSurface(SDL_Window *window,
                              VkInstance instance,
                              const struct VkAllocationCallbacks *allocator,
                              VkSurfaceKHR *surface)
{
    int result;

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
#ifdef SDLOP_HAVE_WAYLAND
    {
        PFN_vkCreateWaylandSurfaceKHR create_surface;
        SDLOP_VkWaylandSurfaceCreateInfoKHR create_info;
        struct wl_surface *wl_surface = window->driver.wayland.wl_surface;
        struct wl_display *wl_display = (struct wl_display *)sdlop_wl_display_handle();

        if (!wl_surface || !wl_display) {
            return SDL_SetError("The window has no Wayland surface");
        }
        create_surface = (PFN_vkCreateWaylandSurfaceKHR)sdlop_vk_get_instance_proc_addr(
            instance, "vkCreateWaylandSurfaceKHR");
        if (!create_surface) {
            return SDL_SetError(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME
                                " extension is not enabled in the Vulkan instance");
        }
        SDL_zero(create_info);
        create_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        create_info.display = wl_display;
        create_info.surface = wl_surface;
        result = create_surface(instance, &create_info, allocator, surface);
    }
#else
    result = -7;                       /* VK_ERROR_EXTENSION_NOT_PRESENT */
#endif
    if (result != VK_SUCCESS) {
        return SDL_SetError("vkCreateWaylandSurfaceKHR failed: %s",
                            sdlop_vulkan_result_string(result));
    }
    return true;
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
    PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR get_support;

    if (!instance) {
        return SDL_InvalidParamError("instance");
    }
    if (!physicalDevice) {
        return SDL_InvalidParamError("physicalDevice");
    }
    if (!sdlop_vk_get_instance_proc_addr) {
        return SDL_SetError("Vulkan is not loaded");
    }
#ifdef SDLOP_HAVE_WAYLAND
    get_support = (PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)
        sdlop_vk_get_instance_proc_addr(instance,
                                        "vkGetPhysicalDeviceWaylandPresentationSupportKHR");
    if (!get_support) {
        return SDL_SetError(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME
                            " extension is not enabled in the Vulkan instance");
    }
    return get_support(physicalDevice, queueFamilyIndex,
                       (struct wl_display *)sdlop_wl_display_handle());
#else
    return SDL_SetError("Vulkan is not supported by this video driver");
#endif
}
