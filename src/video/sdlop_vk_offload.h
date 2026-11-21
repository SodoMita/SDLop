/*
  SDLop - Platform-agnostic headless GPU Vulkan offload.

  Enables hardware-accelerated Vulkan rendering via VK_EXT_headless_surface
  on platforms without native Vulkan WSI (e.g. dummy or framebuffer-only
  drivers), on Wayland compositors without hardware buffer sharing (wl_shm
  only), or on KMS/DRM setups where VK_KHR_display has no usable display.

  When the application resolves Vulkan entry points through
  SDL_Vulkan_GetVkGetInstanceProcAddr(), hardware GPUs are prioritized over
  software devices (lavapipe). If an ICD cannot create a real headless
  swapchain, swapchain images and acquisition are emulated with plain GPU
  images; vkQueuePresentKHR then reads them back and blits them into the
  active driver's window framebuffer (CreateWindowFramebuffer /
  UpdateWindowFramebuffer).

  Controlled by SDLOP_VULKAN_OFFLOAD (alias SDLOP_VK_OFFLOAD), falling back
  to SDLOP_OFFLOAD:
    0 / off / false / no    - disable offload
    1 / on / true / force   - force offload when VK_EXT_headless_surface exists
    unset / auto            - automatic when output lacks hw WSI + hw GPU exists

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef sdlop_vk_offload_h_
#define sdlop_vk_offload_h_

#include "internal/sdlop_internal.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum SDLOP_VulkanOffloadPolicy
{
    SDLOP_VK_OFFLOAD_AUTO = 0,
    SDLOP_VK_OFFLOAD_NEVER = 1,
    SDLOP_VK_OFFLOAD_FORCE = 2,
} SDLOP_VulkanOffloadPolicy;

/* Read SDLOP_VULKAN_OFFLOAD / SDLOP_VK_OFFLOAD / SDLOP_OFFLOAD env var. */
SDLOP_VulkanOffloadPolicy SDLOP_VulkanOffload_GetPolicy(void);

/* Decide whether Vulkan offload should be active for the current video driver
 * using `real_gipa` (from libvulkan). */
bool SDLOP_VulkanOffload_ShouldUseOffload(SDL_FunctionPointer real_gipa);

/* True once offload has been selected for the current Vulkan session. */
bool SDLOP_VulkanOffload_IsActive(void);

/* Returns {"VK_KHR_surface", "VK_EXT_headless_surface"} with *count = 2. */
char const *const *SDLOP_VulkanOffload_GetInstanceExtensions(Uint32 *count);

/* Create a VK_EXT_headless_surface VkSurfaceKHR for `window` and track it. */
bool SDLOP_VulkanOffload_CreateSurface(SDL_FunctionPointer real_gipa,
                                       SDL_Window *window,
                                       void *instance,
                                       const void *allocator,
                                       Uint64 *surface);

/* Untrack a surface when destroyed. */
void SDLOP_VulkanOffload_NotifyDestroySurface(Uint64 surface);

/* Untrack a window when destroyed so swapchain present won't touch it. */
void SDLOP_VulkanOffload_NotifyWindowDestroyed(SDL_Window *window);

/* Wrap vkGetInstanceProcAddr so hardware GPUs are prioritized and swapchain
 * presents can blit into the driver's software framebuffer. */
SDL_FunctionPointer SDLOP_VulkanOffload_WrapGetInstanceProcAddr(SDL_FunctionPointer real_gipa);

/* Release all tracked offload Vulkan state and reset probe cache. */
void SDLOP_VulkanOffload_Shutdown(void);

#endif /* sdlop_vk_offload_h_ */
