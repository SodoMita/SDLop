/*
  SDLop - Vulkan loader management + WSI dispatch + headless GPU offload.

  The loader (libvulkan.so.1) is dlopen'd, like SDL3 does. Surface
  creation is delegated to the video driver, with a platform-agnostic
  VK_EXT_headless_surface offload fallback for drivers or configurations
  without hardware WSI.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "internal/sdlop_internal.h"
#ifndef __EMSCRIPTEN__
#include "video/sdlop_vk_offload.h"
#endif
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
#ifndef __EMSCRIPTEN__
    if (vk_get_instance_proc_addr) {
        return SDLOP_VulkanOffload_WrapGetInstanceProcAddr(vk_get_instance_proc_addr);
    }
#endif
    return vk_get_instance_proc_addr;
}

void SDL_Vulkan_UnloadLibrary(void)
{
#ifndef __EMSCRIPTEN__
    SDLOP_VulkanOffload_Shutdown();
#endif
    if (vulkan_handle) {
        dlclose(vulkan_handle);
        vulkan_handle = NULL;
        vk_get_instance_proc_addr = NULL;
    }
}

void SDLOP_Vulkan_NotifyWindowDestroyed(SDL_Window *window)
{
#ifndef __EMSCRIPTEN__
    SDLOP_VulkanOffload_NotifyWindowDestroyed(window);
#else
    (void)window;
#endif
}

void SDLOP_Vulkan_Quit(void)
{
#ifndef __EMSCRIPTEN__
    SDLOP_VulkanOffload_Shutdown();
#endif
}

/* Default for drivers that don't override it (wayland keeps this for
 * compatibility with apps built against the previous hardcoded list). */
static const char *const sdlop_vulkan_extensions[] = {
    "VK_KHR_surface",
    "VK_KHR_wayland_surface",
    NULL
};

char const *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count)
{
#ifndef __EMSCRIPTEN__
    if (!vulkan_handle) {
        if (!SDL_Vulkan_LoadLibrary(NULL)) {
            /* Keep going for drivers with a static native list; clear only if needed below. */
        }
    }
    if (vk_get_instance_proc_addr &&
        SDLOP_VulkanOffload_ShouldUseOffload(vk_get_instance_proc_addr)) {
        return SDLOP_VulkanOffload_GetInstanceExtensions(count);
    }
#endif
    if (sdlop.video && sdlop.video->Vulkan_GetInstanceExtensions) {
        return sdlop.video->Vulkan_GetInstanceExtensions(sdlop.video, count);
    }
    if (sdlop.video && !sdlop.video->Vulkan_CreateSurface) {
        if (count) {
            *count = 0;
        }
        SDL_SetError("Video driver '%s' does not support Vulkan (and no hardware GPU with VK_EXT_headless_surface is available for offload)",
                     sdlop.video->name);
        return NULL;
    }
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
    if (!instance) {
        return SDL_SetError("Invalid VkInstance");
    }
    if (!surface) {
        return SDL_SetError("NULL surface pointer");
    }
    if (!vulkan_handle && !SDL_Vulkan_LoadLibrary(NULL)) {
        return false;
    }

#ifndef __EMSCRIPTEN__
    if (SDLOP_VulkanOffload_IsActive() ||
        ((!sdlop.video || !sdlop.video->Vulkan_CreateSurface) &&
         SDLOP_VulkanOffload_ShouldUseOffload(vk_get_instance_proc_addr))) {
        return SDLOP_VulkanOffload_CreateSurface(vk_get_instance_proc_addr,
                                                 window, instance, allocator, (Uint64 *)surface);
    }
#endif

    if (sdlop.video && sdlop.video->Vulkan_CreateSurface) {
        if (sdlop.video->Vulkan_CreateSurface(sdlop.video, window, instance, allocator, (Uint64 *)surface)) {
            return true;
        }
#ifndef __EMSCRIPTEN__
        if (SDLOP_VulkanOffload_GetPolicy() != SDLOP_VK_OFFLOAD_NEVER &&
            vk_get_instance_proc_addr) {
            typedef SDL_FunctionPointer (*PFN_gipa)(VkInstance, const char *);
            if (((PFN_gipa)vk_get_instance_proc_addr)(instance, "vkCreateHeadlessSurfaceEXT") != NULL) {
                return SDLOP_VulkanOffload_CreateSurface(vk_get_instance_proc_addr,
                                                         window, instance, allocator, (Uint64 *)surface);
            }
        }
#endif
        return false;
    }

    return SDL_SetError("Video driver '%s' does not support Vulkan",
                        sdlop.video ? sdlop.video->name : "none");
}

void SDL_Vulkan_DestroySurface(VkInstance instance, VkSurfaceKHR surface, const struct VkAllocationCallbacks *allocator)
{
    if (!instance || !vulkan_handle) {
        return;
    }
#ifndef __EMSCRIPTEN__
    SDLOP_VulkanOffload_NotifyDestroySurface((Uint64)surface);
#endif
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
    return vk_get_instance_proc_addr != NULL;
}
