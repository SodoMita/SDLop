/*
  SDLop - Vulkan loader management + WSI dispatch.

  The loader (libvulkan.so.1) is dlopen'd, like SDL3 does. Surface
  creation is delegated to the video driver (VK_KHR_wayland_surface).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#include <dlfcn.h>

static void *vulkan_handle;
static SDL_FunctionPointer vk_get_instance_proc_addr;

bool SDL_Vulkan_LoadLibrary(const char *path)
{
    if (vulkan_handle) {
        return true;
    }
    const char *names[] = { path, "libvulkan.so.1", "libvulkan.so" };
    for (size_t i = 0; i < SDL_arraysize(names); i++) {
        if (!names[i]) {
            continue;
        }
        vulkan_handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (vulkan_handle) {
            break;
        }
    }
    if (!vulkan_handle) {
        return SDL_SetError("Could not load Vulkan loader (libvulkan.so.1): %s", dlerror());
    }
    vk_get_instance_proc_addr = (SDL_FunctionPointer)dlsym(vulkan_handle, "vkGetInstanceProcAddr");
    if (!vk_get_instance_proc_addr) {
        dlclose(vulkan_handle);
        vulkan_handle = NULL;
        return SDL_SetError("Vulkan loader has no vkGetInstanceProcAddr");
    }
    return true;
}

SDL_FunctionPointer SDL_Vulkan_GetVkGetInstanceProcAddr(void)
{
    return vk_get_instance_proc_addr;
}

void SDL_Vulkan_UnloadLibrary(void)
{
    if (vulkan_handle) {
        dlclose(vulkan_handle);
        vulkan_handle = NULL;
        vk_get_instance_proc_addr = NULL;
    }
}

static const char *const sdlop_vulkan_extensions[] = {
    "VK_KHR_surface",
    "VK_KHR_wayland_surface",
    NULL
};

char const *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count)
{
    if (count) {
        *count = 2;
    }
    return sdlop_vulkan_extensions;
}

bool SDL_Vulkan_CreateSurface(SDL_Window *window, VkInstance instance, const struct VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    if (!window) {
        return SDL_SetError("Invalid window");
    }
    if (!surface) {
        return SDL_SetError("NULL surface pointer");
    }
    if (!vulkan_handle && !SDL_Vulkan_LoadLibrary(NULL)) {
        return false;
    }
    if (!sdlop.video || !sdlop.video->Vulkan_CreateSurface) {
        return SDL_SetError("Video driver does not support Vulkan");
    }
    return sdlop.video->Vulkan_CreateSurface(sdlop.video, window, instance, allocator, (Uint64 *)surface);
}

void SDL_Vulkan_DestroySurface(VkInstance instance, VkSurfaceKHR surface, const struct VkAllocationCallbacks *allocator)
{
    if (!instance || !vulkan_handle) {
        return;
    }
    typedef void (*PFN_vkDestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const struct VkAllocationCallbacks *);
    PFN_vkDestroySurfaceKHR destroy =
        (PFN_vkDestroySurfaceKHR)((SDL_FunctionPointer (*)(VkInstance, const char *))vk_get_instance_proc_addr)(instance, "vkDestroySurfaceKHR");
    if (destroy) {
        destroy(instance, surface, allocator);
    }
}

bool SDL_Vulkan_GetPresentationSupport(VkInstance instance, Uint32 queueFamilyIndex)
{
    if (!instance) {
        SDL_SetError("Invalid VkInstance");
        return false;
    }
    if (!vulkan_handle && !SDL_Vulkan_LoadLibrary(NULL)) {
        return false;
    }
    (void)queueFamilyIndex;
    /* Requires a physical device in real use; SDL3 callers pass one via the
     * instance proc - keep semantics: presence of the WSI entry point and a
     * live wayland display is enough for our single-GPU lean case. */
    return vk_get_instance_proc_addr != NULL;
}
