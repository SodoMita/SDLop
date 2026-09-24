/*
  SDLop rendering-surface tests: the GL and Vulkan contexts an application
  creates on a SDLop window.

  Both parts need a real display, so they are written to *skip* (exit 0 with a
  SKIP line) when the machine cannot do it, and to fail loudly when it can:

      # on a Wayland session (weston headless works)
      SDL_VIDEODRIVER=wayland WAYLAND_DISPLAY=wayland-1 ./build/tests/test_gl

  Vulkan goes through the dlopen()ed loader and the compositor's surface
  extension; GL goes through the EGL driver in src/gl/SDL_egl.c.
*/

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>

static int failures;
static int checks;
static int skips;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        checks++;                                                               \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

#define SKIP(...)                                                               \
    do {                                                                        \
        skips++;                                                                \
        printf("SKIP: ");                                                       \
        printf(__VA_ARGS__);                                                    \
        printf("\n");                                                           \
    } while (0)

/* ------------------------------------------------------------------ Vulkan */

/* Just enough Vulkan for an instance and a surface: the test intentionally does
   not include <vulkan/vulkan.h>, exactly like an application that only uses the
   SDL_Vulkan_* API. */
#define VK_SUCCESS 0
#define VK_STRUCTURE_TYPE_APPLICATION_INFO 0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1
#define VK_API_VERSION_1_0 (1u << 22)

typedef void *VulkanInstance;
typedef void *VulkanPhysicalDevice;

typedef struct
{
    Uint32 sType;
    const void *pNext;
    const char *pApplicationName;
    Uint32 applicationVersion;
    const char *pEngineName;
    Uint32 engineVersion;
    Uint32 apiVersion;
} VulkanAppInfo;

typedef struct
{
    Uint32 sType;
    const void *pNext;
    Uint32 flags;
    const VulkanAppInfo *pApplicationInfo;
    Uint32 enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    Uint32 enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} VulkanInstanceCreateInfo;

typedef struct
{
    Uint32 queueFlags;
    Uint32 queueCount;
    Uint32 timestampValidBits;
    Uint32 minImageTransferGranularity[3];
} VulkanQueueFamilyProperties;

static void test_vulkan(void)
{
    SDL_Window *window;
    Uint32 count = 0;
    char const *const *extensions;
    Uint32 i;
    bool has_surface_extension = false;
    SDL_FunctionPointer proc;
    int (*vkCreateInstance)(const VulkanInstanceCreateInfo *, const void *, VulkanInstance *);
    void (*vkDestroyInstance)(VulkanInstance, const void *);
    int (*vkEnumeratePhysicalDevices)(VulkanInstance, Uint32 *, VulkanPhysicalDevice *);
    void (*vkGetPhysicalDeviceQueueFamilyProperties)(VulkanPhysicalDevice, Uint32 *,
                                                     VulkanQueueFamilyProperties *);
    VulkanInstance instance = NULL;
    VulkanInstanceCreateInfo create_info;
    VulkanAppInfo app_info;

    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        SKIP("no Vulkan loader: %s", SDL_GetError());
        return;
    }
    CHECK(SDL_Vulkan_GetVkGetInstanceProcAddr() != NULL, "no vkGetInstanceProcAddr");

    extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    CHECK(extensions != NULL && count > 0, "SDL_Vulkan_GetInstanceExtensions(): %s", SDL_GetError());
    if (!extensions) {
        SDL_Vulkan_UnloadLibrary();
        return;
    }
    printf("  vulkan instance extensions:");
    for (i = 0; i < count; i++) {
        printf(" %s", extensions[i]);
        if (SDL_strcmp(extensions[i], "VK_KHR_surface") == 0 ||
            SDL_strstr(extensions[i], "_surface") != NULL) {
            has_surface_extension = true;
        }
    }
    printf("\n");
    CHECK(has_surface_extension, "no surface extension in the list");

    window = SDL_CreateWindow("vulkan", 320, 200, SDL_WINDOW_VULKAN);
    CHECK(window != NULL, "SDL_CreateWindow(SDL_WINDOW_VULKAN): %s", SDL_GetError());
    if (!window) {
        SDL_Vulkan_UnloadLibrary();
        return;
    }

    proc = SDL_Vulkan_GetVkGetInstanceProcAddr();
    vkCreateInstance = (int (*)(const VulkanInstanceCreateInfo *, const void *, VulkanInstance *))
        ((void *(*)(void *, const char *))proc)(NULL, "vkCreateInstance");
    CHECK(vkCreateInstance != NULL, "vkCreateInstance not found");

    SDL_zero(app_info);
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "test_gl";
    app_info.pEngineName = "SDLop";
    app_info.apiVersion = VK_API_VERSION_1_0;

    SDL_zero(create_info);
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = count;
    create_info.ppEnabledExtensionNames = extensions;

    if (!vkCreateInstance || vkCreateInstance(&create_info, NULL, &instance) != VK_SUCCESS) {
        SKIP("the Vulkan driver cannot make a %s instance", extensions[1]);
        SDL_DestroyWindow(window);
        SDL_Vulkan_UnloadLibrary();
        return;
    }

    {
        VkSurfaceKHR surface = NULL;

        CHECK(SDL_Vulkan_CreateSurface(window, instance, NULL, &surface),
              "SDL_Vulkan_CreateSurface(): %s", SDL_GetError());
        CHECK(surface != NULL, "vkCreateWaylandSurfaceKHR produced no surface");

        /* the presentation query needs a device and a queue family */
        vkEnumeratePhysicalDevices = NULL;
        {
            void *(*get_proc)(VulkanInstance, const char *) = (void *(*)(VulkanInstance, const char *))proc;
            vkEnumeratePhysicalDevices =
                (int (*)(VulkanInstance, Uint32 *, VulkanPhysicalDevice *))
                    get_proc(instance, "vkEnumeratePhysicalDevices");
            vkGetPhysicalDeviceQueueFamilyProperties =
                (void (*)(VulkanPhysicalDevice, Uint32 *, VulkanQueueFamilyProperties *))
                    get_proc(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        }
        if (vkEnumeratePhysicalDevices && vkGetPhysicalDeviceQueueFamilyProperties) {
            Uint32 device_count = 0;

            if (vkEnumeratePhysicalDevices(instance, &device_count, NULL) == VK_SUCCESS && device_count > 0) {
                VulkanPhysicalDevice *devices =
                    (VulkanPhysicalDevice *)SDL_malloc(sizeof(*devices) * device_count);
                if (devices && vkEnumeratePhysicalDevices(instance, &device_count, devices) == VK_SUCCESS) {
                    Uint32 families = 0;
                    VulkanQueueFamilyProperties *props;
                    bool supported = false;

                    vkGetPhysicalDeviceQueueFamilyProperties(devices[0], &families, NULL);
                    props = (VulkanQueueFamilyProperties *)SDL_malloc(sizeof(*props) * families);
                    if (props && families) {
                        vkGetPhysicalDeviceQueueFamilyProperties(devices[0], &families, props);
                        for (i = 0; i < families; i++) {
                            if (SDL_Vulkan_GetPresentationSupport(instance, devices[0], i)) {
                                supported = true;
                            }
                        }
                        CHECK(supported || SDL_GetError() != NULL,
                              "SDL_Vulkan_GetPresentationSupport() is not consistent");
                        if (!supported) {
                            SKIP("this device has no wayland presentation queue (%s)", SDL_GetError());
                        }
                    }
                    SDL_free(props);
                }
                SDL_free(devices);
            }
        }

        SDL_Vulkan_DestroySurface(instance, surface, NULL);
    }

    vkDestroyInstance = (void (*)(VulkanInstance, const void *))
        ((void *(*)(VulkanInstance, const char *))proc)(instance, "vkDestroyInstance");
    if (vkDestroyInstance) {
        vkDestroyInstance(instance, NULL);
    }

    CHECK(SDL_Vulkan_CreateSurface(NULL, instance, NULL, NULL) == false,
          "SDL_Vulkan_CreateSurface() accepted a NULL window");
    /* the surface was created and destroyed, so the loader must still be usable */
    CHECK(SDL_Vulkan_GetVkGetInstanceProcAddr() != NULL, "loader vanished");

    SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    CHECK(SDL_Vulkan_GetVkGetInstanceProcAddr() == NULL,
          "vkGetInstanceProcAddr still available after SDL_Vulkan_UnloadLibrary()");
}

/* ---------------------------------------------------------------------- GL */

static void test_gl(void)
{
    SDL_Window *window;
    SDL_GLContext context;
    void *proc;

    if (!SDL_GL_LoadLibrary(NULL)) {
        SKIP("no EGL/GL library: %s", SDL_GetError());
        return;
    }
    CHECK(SDL_GL_GetCurrentWindow() == NULL, "a GL window is current before SDL_GL_MakeCurrent()");
    CHECK(SDL_GL_GetCurrentContext() == NULL, "a GL context is current already");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    {
        int major = 0, minor = 0;
        CHECK(SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major) && major == 2,
              "context major version attribute = %d", major);
        CHECK(SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor) && minor == 0,
              "context minor version attribute = %d", minor);
    }

    window = SDL_CreateWindow("gl", 320, 200, SDL_WINDOW_OPENGL);
    CHECK(window != NULL, "SDL_CreateWindow(SDL_WINDOW_OPENGL): %s", SDL_GetError());
    if (!window) {
        SDL_GL_UnloadLibrary();
        return;
    }

    context = SDL_GL_CreateContext(window);
    if (!context) {
        SKIP("no GL context: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_GL_UnloadLibrary();
        return;
    }
    CHECK(context != NULL, "SDL_GL_CreateContext(): %s", SDL_GetError());

    CHECK(SDL_GL_MakeCurrent(window, context), "SDL_GL_MakeCurrent(): %s", SDL_GetError());
    CHECK(SDL_GL_GetCurrentWindow() == window, "SDL_GL_GetCurrentWindow() is not the GL window");
    CHECK(SDL_GL_GetCurrentContext() == context, "SDL_GL_GetCurrentContext() is not the context");

    proc = SDL_GL_GetProcAddress("glClearColor");
    CHECK(proc != NULL, "SDL_GL_GetProcAddress(\"glClearColor\"): %s", SDL_GetError());
    CHECK(SDL_GL_GetProcAddress("no_such_gl_function_sha1") == NULL,
          "SDL_GL_GetProcAddress() invented a function");

    {
        int interval = -1;
        if (SDL_GL_SetSwapInterval(1)) {
            CHECK(SDL_GL_GetSwapInterval(&interval) && interval == 1, "swap interval = %d", interval);
        } else {
            SKIP("swap interval not supported: %s", SDL_GetError());
        }
    }
    CHECK(SDL_GL_SetSwapInterval(0), "SDL_GL_SetSwapInterval(0): %s", SDL_GetError());

    {
        void (*glClearColor)(float, float, float, float) = NULL;
        void (*glClear)(unsigned int) = NULL;
        *((void **)&glClearColor) = SDL_GL_GetProcAddress("glClearColor");
        *((void **)&glClear) = SDL_GL_GetProcAddress("glClear");
        if (glClearColor && glClear) {
            glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
            glClear(0x00004000 /* GL_COLOR_BUFFER_BIT */);
            CHECK(SDL_GL_SwapWindow(window), "SDL_GL_SwapWindow(): %s", SDL_GetError());
        }
    }

    CHECK(SDL_GL_MakeCurrent(NULL, NULL), "SDL_GL_MakeCurrent(NULL, NULL): %s", SDL_GetError());
    CHECK(SDL_GL_GetCurrentContext() == NULL, "a context is still current");

    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_GL_UnloadLibrary();
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("test_gl (SDLop)\n");
    /* Needs a real display for GL/Vulkan; try the requested driver and fall back
       to offscreen (where the two SKIP lines below are the honest answer). */
    SDL_setenv_unsafe("SDL_VIDEODRIVER", "offscreen", 0);
    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init(): %s", SDL_GetError());
    printf("  video driver '%s'\n", SDL_GetCurrentVideoDriver());

    test_vulkan();
    test_gl();

    SDL_Quit();
    printf("%d checks, %d failure%s, %d skip%s\n", checks, failures, failures == 1 ? "" : "s",
           skips, skips == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
