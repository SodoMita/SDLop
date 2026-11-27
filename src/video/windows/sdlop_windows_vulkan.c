/*
  SDLop - Vulkan on Win32 (VK_KHR_win32_surface).

  SDL_Vulkan_CreateSurface hands us the VkInstance the application created.
  We need vkCreateWin32SurfaceKHR from that instance, so the loader
  (vulkan-1.dll) is loaded directly rather than depending on a linked
  Vulkan library, exactly like the other SDLop backends do.

  Nothing here requires the Vulkan SDK to *build*: the two Win32 types and
  the one struct below come straight from the Vulkan specification.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"

#ifdef _WIN32

#include "sdlop_windows.h"

/* --- the bits of vulkan.h we need (VK_KHR_win32_surface) --- */

#define VK_SUCCESS 0
#define VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR 1000009000
#define VK_KHR_WIN32_SURFACE_EXTENSION_NAME "VK_KHR_win32_surface"

typedef struct VkWin32SurfaceCreateInfoKHR
{
    int sType;
    const void *pNext;
    unsigned int flags;
    HINSTANCE hinstance;
    HWND hwnd;
} VkWin32SurfaceCreateInfoKHR;

typedef int (*PFN_vkCreateWin32SurfaceKHR)(void *instance, const VkWin32SurfaceCreateInfoKHR *create_info,
                                           const void *allocator, VkSurfaceKHR *surface);

typedef void *VkInstance_;
typedef void *(*PFN_vkGetInstanceProcAddr_)(VkInstance_, const char *);
typedef uint32_t (*PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR_)(void *, uint32_t);

/* SDL_vulkan.h already defines VkSurfaceKHR (a pointer on 64-bit builds, a
 * 64-bit integer elsewhere), so the surface type comes from there. */

static const char *const win32_vulkan_extensions[] = {
    "VK_KHR_surface",
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    NULL
};

const char *const *SDLOP_Win32VulkanGetInstanceExtensions(SDLop_VideoDevice *device, Uint32 *count)
{
    (void)device;
    if (count) {
        *count = (Uint32)(SDL_arraysize(win32_vulkan_extensions) - 1);
    }
    return win32_vulkan_extensions;
}

bool SDLOP_Win32VulkanCreateSurface(SDLop_VideoDevice *device, SDL_Window *window,
                                   void *instance, const void *allocator, Uint64 *surface)
{
    (void)device;
    if (!instance) {
        return SDL_SetError("Vulkan instance is NULL");
    }
    if (!window || !window->driverdata) {
        return SDL_SetError("No window to create a Vulkan surface for");
    }
    /* Resolve the entry point through the instance the application created,
     * the way the Vulkan loader intends: vkGetInstanceProcAddr comes from
     * SDL_Vulkan_GetVkGetInstanceProcAddr(), so a loader the application
     * supplied via SDL_Vulkan_LoadLibrary(path) is the one that answers, and
     * instance-level layers stay in the dispatch chain. LoadLibraryW(
     * "vulkan-1.dll") went straight to the system loader instead, which
     * ignores both -- an application that loaded its own Vulkan got a surface
     * from a different loader than the instance came from. */
    PFN_vkGetInstanceProcAddr_ gipa = (PFN_vkGetInstanceProcAddr_)SDL_Vulkan_GetVkGetInstanceProcAddr();
    PFN_vkCreateWin32SurfaceKHR create = NULL;
    if (gipa) {
        create = (PFN_vkCreateWin32SurfaceKHR)gipa((VkInstance_)instance, "vkCreateWin32SurfaceKHR");
    }
    if (!create) {
        return SDL_SetError("vkCreateWin32SurfaceKHR is missing from the Vulkan loader");
    }

    SDLop_Win32WindowData *data = (SDLop_Win32WindowData *)window->driverdata;
    if (!data->hwnd) {
        return SDL_SetError("Window has no HWND");
    }

    VkWin32SurfaceCreateInfoKHR info;
    SDL_zero(info);
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.pNext = NULL;
    info.flags = 0;
    info.hinstance = GetModuleHandleW(NULL);
    info.hwnd = data->hwnd;

    VkSurfaceKHR created = (VkSurfaceKHR)0;
    const int rc = create(instance, &info, allocator, &created);
    if (rc != VK_SUCCESS) {
        return SDL_SetError("vkCreateWin32SurfaceKHR() failed: %d", rc);
    }
    if (surface) {
        *surface = (Uint64)created;
    }
    return true;
}

/* SDL_Vulkan_GetPresentationSupport(): ask the driver whether a queue family
 * can present to a Win32 surface. Without this the call reported "no
 * presentation support" for every queue, so applications gave up on Vulkan
 * even on hardware that presents to Win32 perfectly well. */
bool SDLOP_Win32VulkanGetPresentationSupport(SDLop_VideoDevice *device, void *instance,
                                             void *physical_device, Uint32 queue_family)
{
    (void)device;
    PFN_vkGetInstanceProcAddr_ gipa = (PFN_vkGetInstanceProcAddr_)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa) {
        return SDL_SetError("Vulkan loader not loaded");
    }
    PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR_ query =
        (PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR_)gipa((VkInstance_)instance,
                                                                  "vkGetPhysicalDeviceWin32PresentationSupportKHR");
    if (!query) {
        return SDL_SetError(VK_KHR_WIN32_SURFACE_EXTENSION_NAME " is not enabled in the Vulkan instance");
    }
    return query(physical_device, queue_family) != 0;
}

#endif /* _WIN32 */
