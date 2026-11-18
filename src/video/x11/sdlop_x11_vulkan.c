/*
  SDLop - X11 Vulkan WSI (VK_KHR_xlib_surface).
  The driver does not link libvulkan; entry points come from the loader
  that SDL_vulkan.c has dlopen'd.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"

#define VK_SUCCESS_ 0
#define VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR_ 1000004000

typedef struct VkXlibSurfaceCreateInfoKHR_
{
    uint32_t sType; /* VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR */
    const void *pNext;
    uint32_t flags;
    SDLOP_Display *dpy;
    unsigned long window; /* X11 Window */
} VkXlibSurfaceCreateInfoKHR_;

typedef int32_t VkResult_;
typedef void *VkInstance_;

typedef VkResult_ (*PFN_vkCreateXlibSurfaceKHR_)(VkInstance_, const VkXlibSurfaceCreateInfoKHR_ *, const void *, uint64_t *);
typedef void *(*PFN_vkGetInstanceProcAddr_)(VkInstance_, const char *);

static const char *const x11_vulkan_extensions[] = {
    "VK_KHR_surface",
    "VK_KHR_xlib_surface",
    NULL
};

const char *const *SDLOP_X11_Vulkan_GetInstanceExtensions(SDLop_VideoDevice *device, Uint32 *count)
{
    (void)device;
    if (count) {
        *count = 2;
    }
    return x11_vulkan_extensions;
}

bool SDLOP_X11_Vulkan_CreateSurface(SDLop_VideoDevice *device, SDL_Window *window, void *instance, const void *allocator, Uint64 *surface)
{
    SDLOP_Display *dpy = SDLOP_X11_GetDisplay(device);
    SDLOP_Window xwindow = SDLOP_X11_GetWindowHandle(window);
    if (!dpy || xwindow == SDLOP_None) {
        return SDL_SetError("Window has no X11 handle");
    }

    SDL_FunctionPointer gipa_fp = SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa_fp) {
        return SDL_SetError("Vulkan loader not loaded");
    }
    PFN_vkGetInstanceProcAddr_ gipa = (PFN_vkGetInstanceProcAddr_)gipa_fp;
    PFN_vkCreateXlibSurfaceKHR_ create_surface =
        (PFN_vkCreateXlibSurfaceKHR_)gipa((VkInstance_)instance, "vkCreateXlibSurfaceKHR");
    if (!create_surface) {
        return SDL_SetError("vkCreateXlibSurfaceKHR not available (missing VK_KHR_xlib_surface?)");
    }

    VkXlibSurfaceCreateInfoKHR_ ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR_;
    ci.dpy = dpy;
    ci.window = (unsigned long)xwindow;

    VkResult_ res = create_surface((VkInstance_)instance, &ci, allocator, surface);
    if (res != VK_SUCCESS_) {
        return SDL_SetError("vkCreateXlibSurfaceKHR failed: %d", (int)res);
    }
    return true;
}
