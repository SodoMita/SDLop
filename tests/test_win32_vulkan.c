/*
  SDLop test: the Vulkan path on Windows, through SDL_Vulkan_*.

  This corrects a recorded assumption in this project. Vulkan was listed as
  unverifiable here because /dev/dri is absent; that is true of hardware Vulkan
  but not of Vulkan. Wine ships WineVulkan, which forwards to the host loader,
  which finds Mesa's lavapipe:

      llvmpipe (LLVM 19.1.7, 256 bits), Vulkan 1.4.305

  So the Win32 surface path can be exercised for real: create an instance with
  the extensions SDL asks for (VK_KHR_surface + VK_KHR_win32_surface), enumerate
  a device, create a VkSurfaceKHR, and ask the Win32 presentation query.

  A note for whoever debugs this next: when a hand-built Vulkan struct is the
  wrong size or shape, Wine asserts deep inside winevulkan (loader_thunks.c) or
  faults, rather than returning an error. Three faults while writing this probe
  were all mine -- a truncated VkPhysicalDeviceProperties (it is ~824 bytes, not
  36), calling vkGetInstanceProcAddr(NULL, ...) for a global command, and
  passing a NULL instance to SDL_Vulkan_CreateSurface. None was a WineVulkan or
  SDLop defect. Do not read an assert in loader.c as an SDLop bug without
  checking the struct first.

  Skips cleanly when no Vulkan implementation is reachable, since a machine
  without one is a valid configuration rather than a broken SDLop.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int skipped = 0;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            failures++;                                          \
        }                                                        \
    } while (0)

/* SDLop deliberately ships no Vulkan headers, so declare the few types and
 * entry points used here. SDL_vulkan.h provides VkInstance and VkSurfaceKHR
 * only; the scalars below are the real Vulkan spellings, not invented ones. */
typedef Sint32 VkResult;
typedef Uint32 VkStructureType;
typedef Uint32 VkInstanceCreateFlags;
typedef Uint32 VkBool32;
#define VK_SUCCESS ((VkResult)0)
#define VK_STRUCTURE_TYPE_APPLICATION_INFO ((VkStructureType)0)
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO ((VkStructureType)1)
#define VK_API_VERSION_1_0 ((Uint32)(1u << 22))
typedef struct VkPhysicalDevice_T *VkPhysicalDeviceLocal;
typedef void *(*PFN_gipa_local)(VkInstance instance, const char *name);
typedef VkResult (*PFN_create_instance_local)(const void *create_info, const void *allocator, VkInstance *instance);
typedef VkResult (*PFN_enumerate_phys_local)(VkInstance instance, Uint32 *count, VkPhysicalDeviceLocal *devices);
typedef void (*PFN_destroy_instance_local)(VkInstance instance, const void *allocator);
/* Two arguments only: (physicalDevice, queueFamilyIndex). Passing an instance
 * first, as I first wrote it, makes the driver validate the wrong handle and
 * report an unrelated presentation-support error. */
typedef VkBool32 (*PFN_presentation_support_local)(VkPhysicalDeviceLocal device, Uint32 queue_family);

/* VkApplicationInfo + VkInstanceCreateInfo, laid out explicitly. */
struct app_info {
    VkStructureType sType;
    const void *pNext;
    const char *pApplicationName;
    Uint32 applicationVersion;
    const char *pEngineName;
    Uint32 engineVersion;
    Uint32 apiVersion;
};

struct instance_create_info {
    VkStructureType sType;
    const void *pNext;
    VkInstanceCreateFlags flags;
    const struct app_info *pApplicationInfo;
    Uint32 enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    Uint32 enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
};

int main(void)
{
    SDL_Window *window;
    PFN_gipa_local gipa;
    PFN_create_instance_local create_instance;
    PFN_enumerate_phys_local enumerate_physical;
    PFN_destroy_instance_local destroy_instance;
    struct app_info app;
    struct instance_create_info create_info;
    const char *const *extensions;
    VkInstance instance = NULL;
    VkSurfaceKHR surface = 0;
    Uint32 extension_count = 0;
    Uint32 device_count = 0;
    int have_win32_surface_extension = 0;
    Uint32 i;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("sdlop win32 vulkan", 320, 200, SDL_WINDOW_VULKAN);
    if (!window) {
        /* A driver with no Vulkan support is a valid configuration. */
        printf("skipped: no Vulkan window: %s\n", SDL_GetError());
        SDL_Quit();
        return 77;
    }
    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        printf("skipped: no Vulkan implementation reachable: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 77;
    }

    gipa = (PFN_gipa_local)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa) {
        CHECK(0, "SDL_Vulkan_GetVkGetInstanceProcAddr returned NULL after a "
                 "successful LoadLibrary");
        SDL_Vulkan_UnloadLibrary();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* --- the extensions SDL must ask for on Windows ------------------- */
    extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    CHECK(extensions != NULL && extension_count > 0,
          "SDL_Vulkan_GetInstanceExtensions returned nothing");
    for (i = 0; extensions && i < extension_count; i++) {
        if (SDL_strcmp(extensions[i], "VK_KHR_surface") == 0) {
            /* always required */
        }
        if (SDL_strcmp(extensions[i], "VK_KHR_win32_surface") == 0) {
            have_win32_surface_extension = 1;
        }
    }
    CHECK(have_win32_surface_extension,
          "VK_KHR_win32_surface is not in the requested extension list, so a "
          "surface could not be created on Windows");

    /* --- instance ----------------------------------------------------- */
    /* vkCreateInstance is a *global* command: it must be looked up with a NULL
     * instance. Asking with an instance handle returns NULL. */
    create_instance = (PFN_create_instance_local)gipa(NULL, "vkCreateInstance");
    if (!create_instance) {
        printf("skipped: no vkCreateInstance entry point\n");
        SDL_Vulkan_UnloadLibrary();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 77;
    }

    SDL_zero(app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "sdlop test";
    app.apiVersion = VK_API_VERSION_1_0;

    SDL_zero(create_info);
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = (const struct app_info *)&app;
    create_info.enabledExtensionCount = extension_count;
    create_info.ppEnabledExtensionNames = extensions;
    create_info.enabledLayerCount = 0;

    {
        const VkResult result = create_instance(&create_info, NULL, &instance);
        /* -9 is VK_ERROR_INCOMPATIBLE_DRIVER. The loader is installed but no
         * ICD is registered (a plain GitHub runner has vulkan-loader and no
         * driver), so there is no driver to be incompatible *with*: that is a
         * missing dependency, not a defect in this library, and the same
         * absence makes the Linux job skip. Every other VkResult is still a
         * real failure: the extensions came from SDL_Vulkan_GetInstanceExtensions,
         * so a driver that rejects them is the bug this test exists to catch. */
        /* -9 is VK_ERROR_INCOMPATIBLE_DRIVER; Win7's loader with no ICD
         * returns -3 (VK_ERROR_INITIALIZATION_FAILED) instead. Both mean no
         * usable driver here, not a defect in SDL's extension list. */
        if (result == (VkResult)-9 || result == (VkResult)-3) {
            printf("skipped: no Vulkan driver installed (VkResult %d)\n", (int)result);
            SDL_Vulkan_UnloadLibrary();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 77;
        }
        if (result != VK_SUCCESS) {
            /* An implementation that cannot make an instance with the
             * extensions SDL asked for is a real failure, not a skip: the
             * library loaded, so this was reachable. */
            CHECK(0, "vkCreateInstance with SDL's extensions failed: %d", (int)result);
            SDL_Vulkan_UnloadLibrary();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }
    CHECK(instance != NULL, "vkCreateInstance returned success with a NULL instance");

    /* --- a device to talk about --------------------------------------- */
    enumerate_physical = (PFN_enumerate_phys_local)gipa(instance, "vkEnumeratePhysicalDevices");
    if (enumerate_physical) {
        enumerate_physical(instance, &device_count, NULL);
        printf("test_win32_vulkan: %u physical device(s)\n", device_count);
        CHECK(device_count > 0, "an instance was created but no physical device was "
                                "enumerated, so nothing could ever be rendered");
    }

    /* --- the Win32 surface, which is the point of the driver ----------- */
    {
        const bool created = SDL_Vulkan_CreateSurface(window, instance, NULL, &surface);
        printf("test_win32_vulkan: SDL_Vulkan_CreateSurface -> %d (%s)\n",
               created ? 1 : 0, created ? "ok" : SDL_GetError());
        CHECK(created, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        CHECK(surface != 0, "SDL_Vulkan_CreateSurface succeeded but produced a NULL "
                            "VkSurfaceKHR");
    }

    /* --- Win32 presentation support ----------------------------------- */
    if (device_count > 0 && enumerate_physical) {
        VkPhysicalDeviceLocal devices[8];
        Uint32 count = device_count < 8 ? device_count : 8;
        memset(devices, 0, sizeof(devices));
        enumerate_physical(instance, &count, devices);
        if (count > 0) {
            PFN_presentation_support_local presentation =
                (PFN_presentation_support_local)gipa(instance, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
            if (presentation) {
                const VkBool32 supported = presentation(devices[0], 0);
                printf("test_win32_vulkan: Win32 presentation support (queue 0) = %s\n",
                       supported ? "yes" : "no");
            } else {
                printf("test_win32_vulkan: vkGetPhysicalDeviceWin32PresentationSupportKHR "
                       "not present; queue family 0 correctness not checked\n");
            }
        }
    }

    /* --- clean release ------------------------------------------------- */
    if (surface) {
        SDL_Vulkan_DestroySurface(instance, surface, NULL);
    }
    destroy_instance = (PFN_destroy_instance_local)gipa(instance, "vkDestroyInstance");
    if (destroy_instance) {
        destroy_instance(instance, NULL);
    }
    SDL_Vulkan_UnloadLibrary();
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures) {
        fprintf(stderr, "test_win32_vulkan: %d failure(s)\n", failures);
        return 1;
    }
    if (skipped) {
        printf("test_win32_vulkan: nothing verified\n");
        return 77;
    }
    printf("test_win32_vulkan: instance, device enumeration and a Win32 surface\n");
    return 0;
}
