/*
  SDLop - Wayland Vulkan WSI (VK_KHR_wayland_surface).
  The driver does not link libvulkan; entry points come from the loader
  that SDL_vulkan.c has dlopen'd.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_wayland_internal.h"

#define VK_SUCCESS 0
#define VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR 1000006000

typedef struct VkWaylandSurfaceCreateInfoKHR_
{
    uint32_t sType; /* VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR */
    const void *pNext;
    uint32_t flags;
    struct wl_display *display;
    struct wl_surface *surface;
} VkWaylandSurfaceCreateInfoKHR_;

typedef int32_t VkResult_;
typedef void *VkInstance_;

typedef VkResult_ (*PFN_vkCreateWaylandSurfaceKHR_)(VkInstance_, const VkWaylandSurfaceCreateInfoKHR_ *, const void *, uint64_t *);
typedef void *(*PFN_vkGetInstanceProcAddr_)(VkInstance_, const char *);

bool SDLOP_Wayland_Vulkan_CreateSurface(SDLop_VideoDevice *device, SDL_Window *window, void *instance, const void *allocator, Uint64 *surface)
{
    (void)device;
    struct wl_display *wl = SDLOP_Wayland_GetDisplay();
    struct wl_surface *wls = SDLOP_Wayland_GetWindowSurfaceHandle(window);
    if (!wl || !wls) {
        return SDL_SetError("Window has no Wayland surface");
    }

    SDL_FunctionPointer gipa_fp = SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa_fp) {
        return SDL_SetError("Vulkan loader not loaded");
    }
    PFN_vkGetInstanceProcAddr_ gipa = (PFN_vkGetInstanceProcAddr_)gipa_fp;
    PFN_vkCreateWaylandSurfaceKHR_ create_surface =
        (PFN_vkCreateWaylandSurfaceKHR_)gipa((VkInstance_)instance, "vkCreateWaylandSurfaceKHR");
    if (!create_surface) {
        return SDL_SetError("vkCreateWaylandSurfaceKHR not available (missing VK_KHR_wayland_surface?)");
    }

    VkWaylandSurfaceCreateInfoKHR_ ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    ci.display = wl;
    ci.surface = wls;

    VkResult_ res = create_surface((VkInstance_)instance, &ci, allocator, surface);
    if (res != VK_SUCCESS) {
        return SDL_SetError("vkCreateWaylandSurfaceKHR failed: %d", (int)res);
    }
    return true;
}
