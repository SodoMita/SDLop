/*
  SDLop - X11 Vulkan WSI (VK_KHR_xcb_surface).

  The driver talks xcb, so the surface is created on SDLop's own
  connection; SDL3 does the same whenever libX11-xcb is available. Entry
  points come from the loader SDL_vulkan.c has dlopen-ed - libvulkan is
  never linked.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_x11_internal.h"

#define VK_SUCCESS 0
#define VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR 1000005000

typedef struct VkXcbSurfaceCreateInfoKHR_
{
    uint32_t sType; /* VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR */
    const void *pNext;
    uint32_t flags;
    xcb_connection_t *connection;
    xcb_window_t window;
} VkXcbSurfaceCreateInfoKHR_;

typedef int32_t VkResult_;
typedef void *VkInstance_;
typedef VkResult_ (*PFN_vkCreateXcbSurfaceKHR_)(VkInstance_, const VkXcbSurfaceCreateInfoKHR_ *, const void *, uint64_t *);
typedef void *(*PFN_vkGetInstanceProcAddr_)(VkInstance_, const char *);

bool SDLOP_X11_Vulkan_CreateSurface(SDLop_VideoDevice *device, SDL_Window *window, void *instance,
                                    const void *allocator, Uint64 *surface)
{
    (void)device;
    xcb_connection_t *conn = SDLOP_X11_GetConnection();
    xcb_window_t xid = SDLOP_X11_GetWindowXID(window);
    if (!conn || xid == XCB_WINDOW_NONE) {
        return SDL_SetError("Window has no X11 window");
    }
    SDL_FunctionPointer gipa_fp = SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa_fp) {
        return SDL_SetError("Vulkan loader not loaded");
    }
    PFN_vkGetInstanceProcAddr_ gipa = (PFN_vkGetInstanceProcAddr_)gipa_fp;
    PFN_vkCreateXcbSurfaceKHR_ create_surface =
        (PFN_vkCreateXcbSurfaceKHR_)gipa((VkInstance_)instance, "vkCreateXcbSurfaceKHR");
    if (!create_surface) {
        return SDL_SetError("vkCreateXcbSurfaceKHR not available (missing VK_KHR_xcb_surface?)");
    }
    VkXcbSurfaceCreateInfoKHR_ ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    ci.connection = conn;
    ci.window = xid;
    VkResult_ res = create_surface((VkInstance_)instance, &ci, allocator, surface);
    if (res != VK_SUCCESS) {
        return SDL_SetError("vkCreateXcbSurfaceKHR failed: %d", (int)res);
    }
    return true;
}

const char *const *SDLOP_X11_Vulkan_GetInstanceExtensions(SDLop_VideoDevice *device, Uint32 *count)
{
    (void)device;
    static const char *const extensions[] = { "VK_KHR_surface", "VK_KHR_xcb_surface" };
    if (count) {
        *count = (Uint32)SDL_arraysize(extensions);
    }
    return extensions;
}
