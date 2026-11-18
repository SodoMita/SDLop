/*
  SDLop test: headless GPU Vulkan offload policy, env overrides
  (SDLOP_VULKAN_OFFLOAD / SDLOP_VK_OFFLOAD / SDLOP_OFFLOAD), opt-out on
  software-only drivers (dummy), and live offload probe when Vulkan exists.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include "video/sdlop_vk_offload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                           \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
            fprintf(stderr, __VA_ARGS__);                          \
            fprintf(stderr, "\n");                                 \
            failures++;                                            \
        }                                                          \
    } while (0)

static void test_policy(void)
{
    unsetenv("SDLOP_VULKAN_OFFLOAD");
    unsetenv("SDLOP_VK_OFFLOAD");
    unsetenv("SDLOP_OFFLOAD");
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_AUTO, "unset -> AUTO");

    setenv("SDLOP_OFFLOAD", "0", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_NEVER, "SDLOP_OFFLOAD=0 -> NEVER");
    setenv("SDLOP_OFFLOAD", "force", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_FORCE, "SDLOP_OFFLOAD=force -> FORCE");

    setenv("SDLOP_VK_OFFLOAD", "off", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_NEVER, "SDLOP_VK_OFFLOAD overrides SDLOP_OFFLOAD");

    setenv("SDLOP_VULKAN_OFFLOAD", "1", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_FORCE, "SDLOP_VULKAN_OFFLOAD overrides SDLOP_VK_OFFLOAD");

    setenv("SDLOP_VULKAN_OFFLOAD", "no", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_NEVER, "no -> NEVER");
    setenv("SDLOP_VULKAN_OFFLOAD", "auto", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_AUTO, "auto -> AUTO");

    unsetenv("SDLOP_VULKAN_OFFLOAD");
    unsetenv("SDLOP_VK_OFFLOAD");
    unsetenv("SDLOP_OFFLOAD");
}

static void test_extensions_layout(void)
{
    Uint32 count = 0;
    char const *const *exts = SDLOP_VulkanOffload_GetInstanceExtensions(&count);
    CHECK(count == 2, "offload extension count == 2 (got %u)", count);
    CHECK(exts && exts[0] && strcmp(exts[0], "VK_KHR_surface") == 0, "ext[0] == VK_KHR_surface");
    CHECK(exts && exts[1] && strcmp(exts[1], "VK_EXT_headless_surface") == 0, "ext[1] == VK_EXT_headless_surface");
    CHECK(exts && exts[2] == NULL, "ext list NULL-terminated");
}

static void test_dummy_opt_out(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDLOP_VULKAN_OFFLOAD", "0", 1);

    if (SDL_Init(SDL_INIT_VIDEO)) {
        Uint32 ext_count = 99;
        char const *const *exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
        CHECK(exts == NULL && ext_count == 0,
              "dummy GetInstanceExtensions returns NULL when SDLOP_VULKAN_OFFLOAD=0");

        SDL_Window *w = SDL_CreateWindow("dummy vk optout", 64, 64, SDL_WINDOW_VULKAN);
        CHECK(w != NULL, "dummy window created");
        if (w) {
            VkSurfaceKHR surf = 0;
            /* Dummy non-null instance check only when libvulkan exists; otherwise LoadLibrary fails cleanly. */
            CHECK(!SDL_Vulkan_CreateSurface(w, (VkInstance)(uintptr_t)0x1, NULL, &surf),
                  "dummy CreateSurface fails when SDLOP_VULKAN_OFFLOAD=0");
            SDL_DestroyWindow(w);
        }
        SDL_Vulkan_UnloadLibrary();
        SDL_Quit();
    }

    unsetenv("SDLOP_VULKAN_OFFLOAD");
    unsetenv("SDL_VIDEODRIVER");
}

int main(void)
{
    test_policy();
    test_extensions_layout();
    test_dummy_opt_out();

    if (failures) {
        printf("test_vk_offload: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_vk_offload: PASS\n");
    return 0;
}
