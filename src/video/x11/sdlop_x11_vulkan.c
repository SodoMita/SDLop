/*
  SDLop - Xlib Vulkan WSI (VK_KHR_xlib_surface).

  Vulkan handles and entry points are resolved through SDLop's runtime-loaded
  Vulkan loader. This module does not link to libvulkan or libX11.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"

#include <stdint.h>
#include <string.h>

#define VK_SUCCESS_ 0
#define VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR_ 1000004000

typedef int32_t SDLOP_VkResult;
typedef void *SDLOP_VkInstance;
typedef uint64_t SDLOP_VkSurfaceKHR;
typedef uint32_t SDLOP_VkFlags;

typedef struct SDLOP_VkXlibSurfaceCreateInfoKHR
{
    uint32_t sType;
    const void *pNext;
    SDLOP_VkFlags flags;
    Display *dpy;
    Window window;
} SDLOP_VkXlibSurfaceCreateInfoKHR;

typedef SDLOP_VkResult (*PFN_vkCreateXlibSurfaceKHR_)(
    SDLOP_VkInstance, const SDLOP_VkXlibSurfaceCreateInfoKHR *,
    const void *, SDLOP_VkSurfaceKHR *);
typedef SDL_FunctionPointer (*PFN_vkGetInstanceProcAddr_)(
    SDLOP_VkInstance, const char *);

static const char *const x11_vulkan_extensions[] = {
    "VK_KHR_surface",
    "VK_KHR_xlib_surface",
    NULL
};

const char *const *SDLOP_X11_Vulkan_GetInstanceExtensions(
    SDLop_VideoDevice *device, Uint32 *count)
{
    (void)device;
    if (count) {
        *count = (Uint32)(SDL_arraysize(x11_vulkan_extensions) - 1);
    }
    return x11_vulkan_extensions;
}

bool SDLOP_X11_Vulkan_CreateSurface(SDLop_VideoDevice *device,
                                   SDL_Window *window, void *instance,
                                   const void *allocator, Uint64 *surface)
{
    (void)device;
    if (!window || !instance || !surface) {
        return SDL_SetError("Invalid X11 Vulkan surface arguments");
    }
    Window native_window = SDLOP_X11_GetWindowHandle(window);
    Display *display = SDLOP_X11_GetDisplay();
    if (!display || native_window == None) {
        return SDL_SetError("Window has no X11 native handle");
    }

    SDL_FunctionPointer gipa_fp = SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa_fp) {
        return SDL_SetError("Vulkan loader not loaded");
    }
    PFN_vkGetInstanceProcAddr_ gipa = (PFN_vkGetInstanceProcAddr_)gipa_fp;
    PFN_vkCreateXlibSurfaceKHR_ create_surface =
        (PFN_vkCreateXlibSurfaceKHR_)gipa((SDLOP_VkInstance)instance,
                                          "vkCreateXlibSurfaceKHR");
    if (!create_surface) {
        return SDL_SetError("vkCreateXlibSurfaceKHR is unavailable (enable VK_KHR_xlib_surface)");
    }

    SDLOP_VkXlibSurfaceCreateInfoKHR create_info;
    memset(&create_info, 0, sizeof(create_info));
    create_info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR_;
    create_info.dpy = display;
    create_info.window = native_window;

    SDLOP_VkResult result = create_surface((SDLOP_VkInstance)instance,
                                           &create_info, allocator,
                                           (SDLOP_VkSurfaceKHR *)surface);
    if (result != VK_SUCCESS_) {
        return SDL_SetError("vkCreateXlibSurfaceKHR failed: %d", result);
    }
    return true;
}
