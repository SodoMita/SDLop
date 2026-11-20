/*
  SDLop live test: end-to-end Vulkan GPU offload on a real Vulkan loader + ICD
  (e.g. Mesa lavapipe or hardware Vulkan driver supporting VK_EXT_headless_surface).

  Uses SDL_Vulkan_GetVkGetInstanceProcAddr() + vkGetDeviceProcAddr() (the
  conformant SDL3 Vulkan dispatch pattern) so the offload wrapper handles
  VK_EXT_headless_surface, swapchain TRANSFER_SRC injection, and vkQueuePresentKHR
  readback into the dummy window's XRGB8888 framebuffer.

  Skips cleanly (exit 0) if libvulkan.so.1 or VK_EXT_headless_surface is absent.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <alloca.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                                           \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
            fprintf(stderr, __VA_ARGS__);                          \
            fprintf(stderr, "\n");                                 \
            failures++;                                            \
            goto cleanup;                                          \
        }                                                          \
    } while (0)

#ifdef VK_EXT_debug_utils
typedef struct ValidationState {
    int error_count;
} ValidationState;

static VKAPI_ATTR VkBool32 VKAPI_CALL validation_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user_data)
{
    (void)types;
    ValidationState *st = (ValidationState *)user_data;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (st) {
            st->error_count++;
        }
        fprintf(stderr, "VULKAN VALIDATION ERROR: %s\n",
                (data && data->pMessage) ? data->pMessage : "(null)");
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        fprintf(stderr, "VULKAN VALIDATION WARNING: %s\n",
                (data && data->pMessage) ? data->pMessage : "(null)");
    }
    return VK_FALSE;
}
#endif

static void test_auto_probe_no_crash(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDLOP_VULKAN_OFFLOAD", "auto", 1);
    if (SDL_Init(SDL_INIT_VIDEO)) {
        if (SDL_Vulkan_LoadLibrary(NULL)) {
            Uint32 c = 0;
            (void)SDL_Vulkan_GetInstanceExtensions(&c);
            SDL_Vulkan_UnloadLibrary();
        }
        SDL_Quit();
    }
}

int main(void)
{
    int failures = 0;
    int skipped = 0;

    SDL_Window *w = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFramebuffer *fbs = NULL;
    VkImageView *views = NULL;
    uint32_t icount = 0;
    VkSemaphore sem_a = VK_NULL_HANDLE;
    VkSemaphore sem_b = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

#ifdef VK_EXT_debug_utils
    ValidationState vstate = { 0 };
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT p_destroy_debug = NULL;
#endif

    PFN_vkGetInstanceProcAddr gipa = NULL;
    PFN_vkGetDeviceProcAddr gdpa = NULL;
    PFN_vkDestroyInstance p_destroy_instance = NULL;
    PFN_vkDestroyDevice p_destroy_device = NULL;
    PFN_vkDeviceWaitIdle p_device_wait_idle = NULL;
    PFN_vkDestroySwapchainKHR p_destroy_swapchain = NULL;
    PFN_vkDestroyRenderPass p_destroy_render_pass = NULL;
    PFN_vkDestroyCommandPool p_destroy_command_pool = NULL;
    PFN_vkDestroyFramebuffer p_destroy_framebuffer = NULL;
    PFN_vkDestroyImageView p_destroy_image_view = NULL;
    PFN_vkDestroySemaphore p_destroy_semaphore = NULL;
    PFN_vkDestroyFence p_destroy_fence = NULL;

    const char *strict_env = getenv("SDLOP_TEST_STRICT");
    int strict = (strict_env && strcmp(strict_env, "1") == 0);

    test_auto_probe_no_crash();

    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    setenv("SDLOP_VULKAN_OFFLOAD", "force", 1);

    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL_Init: %s", SDL_GetError());
    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        printf("test_vk_offload_live: SKIP (no Vulkan loader: %s)\n", SDL_GetError());
        skipped = 1;
        goto cleanup;
    }

    gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    CHECK(gipa != NULL, "SDL_Vulkan_GetVkGetInstanceProcAddr returned NULL");

    Uint32 ext_count = 0;
    char const *const *exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!exts || ext_count < 2 || strcmp(exts[1], "VK_EXT_headless_surface") != 0) {
        printf("test_vk_offload_live: SKIP (VK_EXT_headless_surface unavailable)\n");
        skipped = 1;
        goto cleanup;
    }

    PFN_vkCreateInstance p_create_instance =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    CHECK(p_create_instance != NULL, "vkCreateInstance lookup");

    int has_val_layer = 0;
    int has_debug_utils = 0;
    int has_val_features = 0;

    PFN_vkEnumerateInstanceLayerProperties p_enum_layers =
        (PFN_vkEnumerateInstanceLayerProperties)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    PFN_vkEnumerateInstanceExtensionProperties p_enum_inst_exts =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");

    if (p_enum_layers) {
        uint32_t lc = 0;
        if (p_enum_layers(&lc, NULL) == VK_SUCCESS && lc > 0) {
            VkLayerProperties *lps = (VkLayerProperties *)alloca(lc * sizeof(*lps));
            if (p_enum_layers(&lc, lps) == VK_SUCCESS) {
                for (uint32_t i = 0; i < lc; i++) {
                    if (strcmp(lps[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                        has_val_layer = 1;
                        break;
                    }
                }
            }
        }
    }
    if (p_enum_inst_exts) {
        uint32_t ec = 0;
        if (p_enum_inst_exts(NULL, &ec, NULL) == VK_SUCCESS && ec > 0) {
            VkExtensionProperties *eps = (VkExtensionProperties *)alloca(ec * sizeof(*eps));
            if (p_enum_inst_exts(NULL, &ec, eps) == VK_SUCCESS) {
                for (uint32_t i = 0; i < ec; i++) {
                    if (strcmp(eps[i].extensionName, "VK_EXT_debug_utils") == 0) {
                        has_debug_utils = 1;
                    }
                }
            }
        }
        if (has_val_layer) {
            uint32_t lec = 0;
            if (p_enum_inst_exts("VK_LAYER_KHRONOS_validation", &lec, NULL) == VK_SUCCESS && lec > 0) {
                VkExtensionProperties *leps = (VkExtensionProperties *)alloca(lec * sizeof(*leps));
                if (p_enum_inst_exts("VK_LAYER_KHRONOS_validation", &lec, leps) == VK_SUCCESS) {
                    for (uint32_t i = 0; i < lec; i++) {
                        if (strcmp(leps[i].extensionName, "VK_EXT_debug_utils") == 0) {
                            has_debug_utils = 1;
                        }
                        if (strcmp(leps[i].extensionName, "VK_EXT_validation_features") == 0) {
                            has_val_features = 1;
                        }
                    }
                }
            }
        }
    }

    const char *inst_exts[16];
    uint32_t total_inst_exts = 0;
    for (Uint32 i = 0; i < ext_count && total_inst_exts < 14; i++) {
        inst_exts[total_inst_exts++] = exts[i];
    }
    int enable_validation = (has_val_layer && has_debug_utils);
    if (strict) {
        CHECK(enable_validation && has_val_features,
              "SDLOP_TEST_STRICT=1 requires VK_LAYER_KHRONOS_validation (%d), VK_EXT_debug_utils (%d), VK_EXT_validation_features (%d)",
              has_val_layer, has_debug_utils, has_val_features);
    }
    const char *val_layers[1] = { "VK_LAYER_KHRONOS_validation" };
    if (enable_validation) {
        inst_exts[total_inst_exts++] = "VK_EXT_debug_utils";
        if (has_val_features) {
            inst_exts[total_inst_exts++] = "VK_EXT_validation_features";
        }
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "sdlop_test_vk_offload_live",
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = total_inst_exts,
        .ppEnabledExtensionNames = inst_exts,
        .enabledLayerCount = enable_validation ? 1u : 0u,
        .ppEnabledLayerNames = enable_validation ? val_layers : NULL,
    };
#ifdef VK_EXT_validation_features
    VkValidationFeatureEnableEXT enables[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    VkValidationFeaturesEXT val_features = {
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount = 1,
        .pEnabledValidationFeatures = enables,
    };
    if (enable_validation && has_val_features) {
        val_features.pNext = ici.pNext;
        ici.pNext = &val_features;
    }
#else
    if (strict) {
        CHECK(0, "SDLOP_TEST_STRICT=1 requires VK_EXT_validation_features at compile time");
    }
#endif
#ifdef VK_EXT_debug_utils
    VkDebugUtilsMessengerCreateInfoEXT dmci = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = validation_callback,
        .pUserData = &vstate,
    };
    if (enable_validation) {
        dmci.pNext = ici.pNext;
        ici.pNext = &dmci;
    }
#else
    if (strict) {
        CHECK(0, "SDLOP_TEST_STRICT=1 requires VK_EXT_debug_utils at compile time");
    }
#endif
    VkResult ires = p_create_instance(&ici, NULL, &instance);
    if (ires == VK_ERROR_EXTENSION_NOT_PRESENT || ires == VK_ERROR_INCOMPATIBLE_DRIVER) {
        printf("test_vk_offload_live: SKIP (vkCreateInstance=%d)\n", ires);
        skipped = 1;
        goto cleanup;
    }
    CHECK(ires == VK_SUCCESS && instance != VK_NULL_HANDLE, "vkCreateInstance: %d", ires);

#ifdef VK_EXT_debug_utils
    if (enable_validation) {
        PFN_vkCreateDebugUtilsMessengerEXT p_create_debug =
            (PFN_vkCreateDebugUtilsMessengerEXT)gipa(instance, "vkCreateDebugUtilsMessengerEXT");
        p_destroy_debug =
            (PFN_vkDestroyDebugUtilsMessengerEXT)gipa(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (p_create_debug) {
            CHECK(p_create_debug(instance, &dmci, NULL, &debug_messenger) == VK_SUCCESS,
                  "vkCreateDebugUtilsMessengerEXT");
            printf("validation: enabled (syncval=%s)\n", has_val_features ? "yes" : "no");
        } else if (strict) {
            CHECK(0, "SDLOP_TEST_STRICT=1 requires vkCreateDebugUtilsMessengerEXT");
        }
    } else {
        printf("validation: not installed (VK_LAYER_KHRONOS_validation=%d, debug_utils=%d)\n",
               has_val_layer, has_debug_utils);
    }
#endif

    p_destroy_instance = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices p_enum_gpus =
        (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties p_gpu_props =
        (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties p_qf_props =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR p_surf_support =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR p_surf_caps =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR p_surf_fmts =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    PFN_vkCreateDevice p_create_device =
        (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
    CHECK(p_destroy_instance && p_enum_gpus && p_gpu_props && p_qf_props &&
          p_surf_support && p_surf_caps && p_surf_fmts && p_create_device && gdpa,
          "instance proc lookups");

    w = SDL_CreateWindow("vk-offload-live", 64, 48, SDL_WINDOW_VULKAN);
    CHECK(w != NULL, "SDL_CreateWindow: %s", SDL_GetError());
    CHECK(SDL_Vulkan_CreateSurface(w, instance, NULL, &surface) && surface != VK_NULL_HANDLE,
          "SDL_Vulkan_CreateSurface: %s", SDL_GetError());

    uint32_t gpu_count = 0;
    CHECK(p_enum_gpus(instance, &gpu_count, NULL) == VK_SUCCESS && gpu_count > 0, "no Vulkan GPUs");
    VkPhysicalDevice *gpus = (VkPhysicalDevice *)alloca(gpu_count * sizeof(*gpus));
    CHECK(p_enum_gpus(instance, &gpu_count, gpus) == VK_SUCCESS, "enum GPUs");

    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    uint32_t qf = 0;
    for (uint32_t i = 0; i < gpu_count && gpu == VK_NULL_HANDLE; i++) {
        uint32_t qfc = 0;
        p_qf_props(gpus[i], &qfc, NULL);
        VkQueueFamilyProperties *qfps = (VkQueueFamilyProperties *)alloca(qfc * sizeof(*qfps));
        p_qf_props(gpus[i], &qfc, qfps);
        for (uint32_t q = 0; q < qfc; q++) {
            VkBool32 sup = VK_FALSE;
            if ((qfps[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                p_surf_support(gpus[i], q, surface, &sup) == VK_SUCCESS && sup) {
                gpu = gpus[i];
                qf = q;
                break;
            }
        }
    }
    CHECK(gpu != VK_NULL_HANDLE, "no graphics+present queue family");

    VkPhysicalDeviceProperties props;
    p_gpu_props(gpu, &props);
    printf("device: %s (type %u)\n", props.deviceName, props.deviceType);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qf,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &dqci,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = dev_exts,
    };
    VkResult dres = p_create_device(gpu, &dci, NULL, &device);
    CHECK(dres == VK_SUCCESS && device != VK_NULL_HANDLE, "vkCreateDevice: %d", dres);

    p_destroy_device = (PFN_vkDestroyDevice)gdpa(device, "vkDestroyDevice");
    p_device_wait_idle = (PFN_vkDeviceWaitIdle)gdpa(device, "vkDeviceWaitIdle");
    PFN_vkGetDeviceQueue p_get_queue = (PFN_vkGetDeviceQueue)gdpa(device, "vkGetDeviceQueue");
    PFN_vkCreateSwapchainKHR p_create_swapchain =
        (PFN_vkCreateSwapchainKHR)gdpa(device, "vkCreateSwapchainKHR");
    p_destroy_swapchain = (PFN_vkDestroySwapchainKHR)gdpa(device, "vkDestroySwapchainKHR");
    PFN_vkGetSwapchainImagesKHR p_get_sc_images =
        (PFN_vkGetSwapchainImagesKHR)gdpa(device, "vkGetSwapchainImagesKHR");
    PFN_vkAcquireNextImageKHR p_acquire = (PFN_vkAcquireNextImageKHR)gdpa(device, "vkAcquireNextImageKHR");
    PFN_vkQueuePresentKHR p_present = (PFN_vkQueuePresentKHR)gdpa(device, "vkQueuePresentKHR");
    PFN_vkCreateRenderPass p_create_rp = (PFN_vkCreateRenderPass)gdpa(device, "vkCreateRenderPass");
    p_destroy_render_pass = (PFN_vkDestroyRenderPass)gdpa(device, "vkDestroyRenderPass");
    PFN_vkCreateCommandPool p_create_cp = (PFN_vkCreateCommandPool)gdpa(device, "vkCreateCommandPool");
    p_destroy_command_pool = (PFN_vkDestroyCommandPool)gdpa(device, "vkDestroyCommandPool");
    PFN_vkAllocateCommandBuffers p_alloc_cb =
        (PFN_vkAllocateCommandBuffers)gdpa(device, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer p_begin_cb = (PFN_vkBeginCommandBuffer)gdpa(device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer p_end_cb = (PFN_vkEndCommandBuffer)gdpa(device, "vkEndCommandBuffer");
    PFN_vkCmdBeginRenderPass p_begin_rp = (PFN_vkCmdBeginRenderPass)gdpa(device, "vkCmdBeginRenderPass");
    PFN_vkCmdEndRenderPass p_end_rp = (PFN_vkCmdEndRenderPass)gdpa(device, "vkCmdEndRenderPass");
    PFN_vkCreateImageView p_create_iv = (PFN_vkCreateImageView)gdpa(device, "vkCreateImageView");
    p_destroy_image_view = (PFN_vkDestroyImageView)gdpa(device, "vkDestroyImageView");
    PFN_vkCreateFramebuffer p_create_fb = (PFN_vkCreateFramebuffer)gdpa(device, "vkCreateFramebuffer");
    p_destroy_framebuffer = (PFN_vkDestroyFramebuffer)gdpa(device, "vkDestroyFramebuffer");
    PFN_vkCreateSemaphore p_create_sem = (PFN_vkCreateSemaphore)gdpa(device, "vkCreateSemaphore");
    p_destroy_semaphore = (PFN_vkDestroySemaphore)gdpa(device, "vkDestroySemaphore");
    PFN_vkCreateFence p_create_fence = (PFN_vkCreateFence)gdpa(device, "vkCreateFence");
    p_destroy_fence = (PFN_vkDestroyFence)gdpa(device, "vkDestroyFence");
    PFN_vkResetFences p_reset_fences = (PFN_vkResetFences)gdpa(device, "vkResetFences");
    PFN_vkWaitForFences p_wait_fences = (PFN_vkWaitForFences)gdpa(device, "vkWaitForFences");
    PFN_vkQueueSubmit p_queue_submit = (PFN_vkQueueSubmit)gdpa(device, "vkQueueSubmit");
    CHECK(p_destroy_device && p_device_wait_idle && p_get_queue && p_create_swapchain &&
          p_destroy_swapchain && p_get_sc_images && p_acquire && p_present && p_create_rp &&
          p_destroy_render_pass && p_create_cp && p_destroy_command_pool && p_alloc_cb &&
          p_begin_cb && p_end_cb && p_begin_rp && p_end_rp && p_create_iv && p_destroy_image_view &&
          p_create_fb && p_destroy_framebuffer && p_create_sem && p_destroy_semaphore &&
          p_create_fence && p_destroy_fence && p_reset_fences && p_wait_fences && p_queue_submit,
          "device proc lookups");

    VkQueue queue = VK_NULL_HANDLE;
    p_get_queue(device, qf, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    CHECK(p_surf_caps(gpu, surface, &caps) == VK_SUCCESS, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
        printf("test_vk_offload_live: SKIP (headless surface lacks VK_IMAGE_USAGE_TRANSFER_SRC_BIT)\n");
        skipped = 1;
        goto cleanup;
    }

    uint32_t fc = 0;
    CHECK(p_surf_fmts(gpu, surface, &fc, NULL) == VK_SUCCESS && fc > 0, "surface formats count");
    VkSurfaceFormatKHR *fmts = (VkSurfaceFormatKHR *)alloca(fc * sizeof(*fmts));
    CHECK(p_surf_fmts(gpu, surface, &fc, fmts) == VK_SUCCESS, "surface formats");
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < fc; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            fmt = fmts[i];
            break;
        }
    }

    VkExtent2D extent = { 64, 48 };
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    }
    uint32_t nimg = caps.minImageCount + 1;
    if (caps.maxImageCount && nimg > caps.maxImageCount) {
        nimg = caps.maxImageCount;
    }

    /* Deliberately pass only COLOR_ATTACHMENT_BIT; offload wrapper adds TRANSFER_SRC_BIT */
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = nimg,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    CHECK(p_create_swapchain(device, &sci, NULL, &swapchain) == VK_SUCCESS, "vkCreateSwapchainKHR");

    VkAttachmentDescription att = {
        .format = fmt.format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
    };
    VkSubpassDependency deps[2] = {
        {
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        },
        {
            .srcSubpass = 0,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = 0,
        },
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &sub,
        .dependencyCount = 2,
        .pDependencies = deps,
    };
    CHECK(p_create_rp(device, &rpci, NULL, &render_pass) == VK_SUCCESS, "render pass");

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qf,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    CHECK(p_create_cp(device, &cpci, NULL, &pool) == VK_SUCCESS, "command pool");
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    CHECK(p_alloc_cb(device, &cai, &cmd) == VK_SUCCESS, "command buffer");

    VkSemaphoreCreateInfo semci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK(p_create_sem(device, &semci, NULL, &sem_a) == VK_SUCCESS, "sem_a");
    CHECK(p_create_sem(device, &semci, NULL, &sem_b) == VK_SUCCESS, "sem_b");
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    CHECK(p_create_fence(device, &fci, NULL, &fence) == VK_SUCCESS, "fence");

    CHECK(p_get_sc_images(device, swapchain, &icount, NULL) == VK_SUCCESS && icount > 0, "sc image count");
    VkImage *imgs = (VkImage *)alloca(icount * sizeof(*imgs));
    CHECK(p_get_sc_images(device, swapchain, &icount, imgs) == VK_SUCCESS, "sc images");

    views = (VkImageView *)calloc(icount, sizeof(*views));
    fbs = (VkFramebuffer *)calloc(icount, sizeof(*fbs));
    CHECK(views != NULL && fbs != NULL, "oom");

    for (uint32_t i = 0; i < icount; i++) {
        VkImageViewCreateInfo ivci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = imgs[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmt.format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        CHECK(p_create_iv(device, &ivci, NULL, &views[i]) == VK_SUCCESS, "image view %u", i);

        VkFramebufferCreateInfo fbci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = render_pass,
            .attachmentCount = 1,
            .pAttachments = &views[i],
            .width = extent.width,
            .height = extent.height,
            .layers = 1,
        };
        CHECK(p_create_fb(device, &fbci, NULL, &fbs[i]) == VK_SUCCESS, "framebuffer %u", i);
    }

    for (int frame = 0; frame < 2; frame++) {
        CHECK(p_wait_fences(device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS, "wait fence");
        CHECK(p_reset_fences(device, 1, &fence) == VK_SUCCESS, "reset fence");

        uint32_t idx = 0;
        CHECK(p_acquire(device, swapchain, UINT64_MAX, sem_a, VK_NULL_HANDLE, &idx) == VK_SUCCESS, "acquire");
        CHECK(idx < icount, "acquired index %u < %u", idx, icount);

        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK(p_begin_cb(cmd, &bi) == VK_SUCCESS, "begin cmd");
        VkClearValue clear = { .color.float32 = { 0.2f, 0.8f, 0.4f, 1.0f } };
        VkRenderPassBeginInfo rbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = render_pass,
            .framebuffer = fbs[idx],
            .renderArea = { { 0, 0 }, extent },
            .clearValueCount = 1,
            .pClearValues = &clear,
        };
        p_begin_rp(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        p_end_rp(cmd);
        CHECK(p_end_cb(cmd) == VK_SUCCESS, "end cmd");

        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sem_a,
            .pWaitDstStageMask = &stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &sem_b,
        };
        CHECK(p_queue_submit(queue, 1, &si, fence) == VK_SUCCESS, "submit");

        /* Deliberately present immediately after submit without CPU fence wait so
         * the offload wrapper must synchronize on sem_b rather than assuming idle. */
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sem_b,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &idx,
        };
        VkResult pres = p_present(queue, &pi);
        CHECK(pres == VK_SUCCESS || pres == VK_SUBOPTIMAL_KHR, "vkQueuePresentKHR: %d", pres);
    }
    p_device_wait_idle(device);

    SDL_Surface *win_fb = SDL_GetWindowSurface(w);
    CHECK(win_fb != NULL && win_fb->pixels != NULL, "window framebuffer populated");
    CHECK(win_fb->w == (int)extent.width && win_fb->h == (int)extent.height,
          "fb size %dx%d == %ux%u", win_fb->w, win_fb->h, extent.width, extent.height);

    int mismatches = 0;
    for (int y = 0; y < win_fb->h; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)win_fb->pixels + (size_t)y * (size_t)win_fb->pitch);
        for (int x = 0; x < win_fb->w; x++) {
            uint32_t px = row[x];
            int r = (int)((px >> 16) & 0xFFu);
            int g = (int)((px >> 8) & 0xFFu);
            int b = (int)(px & 0xFFu);
            if (abs(r - 51) > 4 || abs(g - 204) > 4 || abs(b - 102) > 4) {
                mismatches++;
            }
        }
    }
    printf("framebuffer %dx%d mismatches=%d\n", win_fb->w, win_fb->h, mismatches);
    CHECK(mismatches == 0, "expected clear (0.2,0.8,0.4) across framebuffer");

#ifdef VK_EXT_debug_utils
    CHECK(vstate.error_count == 0, "expected 0 Vulkan validation errors, got %d", vstate.error_count);
#endif

cleanup:
    if (device && p_device_wait_idle) {
        p_device_wait_idle(device);
    }
    if (fence && p_destroy_fence) {
        p_destroy_fence(device, fence, NULL);
    }
    if (sem_a && p_destroy_semaphore) {
        p_destroy_semaphore(device, sem_a, NULL);
    }
    if (sem_b && p_destroy_semaphore) {
        p_destroy_semaphore(device, sem_b, NULL);
    }
    if (fbs && p_destroy_framebuffer) {
        for (uint32_t i = 0; i < icount; i++) {
            if (fbs[i]) {
                p_destroy_framebuffer(device, fbs[i], NULL);
            }
        }
    }
    if (views && p_destroy_image_view) {
        for (uint32_t i = 0; i < icount; i++) {
            if (views[i]) {
                p_destroy_image_view(device, views[i], NULL);
            }
        }
    }
    free(fbs);
    free(views);
    if (pool && p_destroy_command_pool) {
        p_destroy_command_pool(device, pool, NULL);
    }
    if (render_pass && p_destroy_render_pass) {
        p_destroy_render_pass(device, render_pass, NULL);
    }
    if (swapchain && p_destroy_swapchain) {
        p_destroy_swapchain(device, swapchain, NULL);
    }
    if (device && p_destroy_device) {
        p_destroy_device(device, NULL);
    }
    if (surface && instance) {
        SDL_Vulkan_DestroySurface(instance, surface, NULL);
    }
#ifdef VK_EXT_debug_utils
    if (debug_messenger && instance && p_destroy_debug) {
        p_destroy_debug(instance, debug_messenger, NULL);
    }
#endif
    if (instance && p_destroy_instance) {
        p_destroy_instance(instance, NULL);
    }
    SDL_Vulkan_UnloadLibrary();
    if (w) {
        SDL_DestroyWindow(w);
    }
    SDL_Quit();

    if (skipped) {
        if (strict) {
            fprintf(stderr, "FAIL: test_vk_offload_live skipped under SDLOP_TEST_STRICT=1\n");
            return 1;
        }
        return 0;
    }
#ifdef VK_EXT_debug_utils
    if (vstate.error_count > 0) {
        failures++;
    }
#endif
    if (failures) {
        printf("test_vk_offload_live: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_vk_offload_live: PASS\n");
    return 0;
}
