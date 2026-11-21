/*
  SDLop - Vulkan present offload, Windows build.

  On Linux SDLop can present Vulkan through VK_EXT_headless_surface and blit
  into the driver's software scanout, which keeps the fbdev and KMS/DRM
  drivers usable when the ICD has no WSI for them. On Windows every ICD ships
  VK_KHR_win32_surface, so offload is never selected: these entry points are
  the "offload is not active" answers, which is what the callers already
  handle (it is the same state as SDLOP_VULKAN_OFFLOAD=never on Linux).

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_vk_offload.h"

#ifdef _WIN32

SDLOP_VulkanOffloadPolicy SDLOP_VulkanOffload_GetPolicy(void)
{
    /* read through the hint layer so SDL_SetHint works, like every other knob */
    static const char *const names[] = {
        SDLOP_HINT_VULKAN_OFFLOAD, SDLOP_HINT_VK_OFFLOAD, SDLOP_HINT_OFFLOAD, NULL
    };
    const char *env = SDLOP_HintFirst(names);
    if (env && *env && SDL_strcasecmp(env, "auto") != 0 && SDL_strcasecmp(env, "never") != 0 &&
        SDL_strcmp(env, "0") != 0 && SDL_strcasecmp(env, "off") != 0 &&
        SDL_strcasecmp(env, "false") != 0 && SDL_strcasecmp(env, "no") != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                    "Vulkan present offload is a Linux feature; ignoring SDLOP_VULKAN_OFFLOAD=%s", env);
    }
    return SDLOP_VK_OFFLOAD_NEVER;
}

bool SDLOP_VulkanOffload_ShouldUseOffload(SDL_FunctionPointer real_gipa)
{
    (void)real_gipa;
    return false;
}

bool SDLOP_VulkanOffload_IsActive(void)
{
    return false;
}

char const *const *SDLOP_VulkanOffload_GetInstanceExtensions(Uint32 *count)
{
    if (count) {
        *count = 0;
    }
    return NULL;
}

bool SDLOP_VulkanOffload_CreateSurface(SDL_FunctionPointer real_gipa, SDL_Window *window,
                                       void *instance, const void *allocator, Uint64 *surface)
{
    (void)real_gipa; (void)window; (void)instance; (void)allocator;
    if (surface) {
        *surface = 0;
    }
    return SDL_SetError("Vulkan present offload is not available on Windows");
}

void SDLOP_VulkanOffload_NotifyDestroySurface(Uint64 surface)
{
    (void)surface;
}

void SDLOP_VulkanOffload_NotifyWindowDestroyed(SDL_Window *window)
{
    (void)window;
}

SDL_FunctionPointer SDLOP_VulkanOffload_WrapGetInstanceProcAddr(SDL_FunctionPointer real_gipa)
{
    return real_gipa; /* nothing to wrap: no offload swapchain present path */
}

void SDLOP_VulkanOffload_Shutdown(void)
{
}

#endif /* _WIN32 */
