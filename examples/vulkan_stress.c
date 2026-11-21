/*
  SDLop example: heavier Vulkan stress scene.

  Unlike vulkan_clear, this example exercises a realistic frame graph:

    1. compute pass updates an instanced particle buffer on the GPU;
    2. graphics pass draws thousands of blended, procedurally shaded quads
       into an offscreen attachment;
    3. post pass samples that attachment through a 3x3 blur, adds a plasma
       background and writes to the swapchain;
    4. the final image is copied to a host buffer for a deterministic
       checksum (useful when comparing native WSI vs shm/dumb offload).

  All Vulkan entry points are resolved through SDL_Vulkan_GetVkGetInstanceProcAddr
  so SDLop can intercept them when offload is active. That makes the same binary
  work with native WSI, Weston wl_shm, X11 shm, KMS dumb buffers and fbdev.

  Offload follows SDLop's auto policy by default: native WSI is kept where it
  works, and the headless/shm path is used when present is CPU-only or unsafe
  (for example NVIDIA 470 KMS). --offload and --native force either choice.

  Usage:
    vulkan_stress [--offload|--native] [--frames N] [--duration SECONDS]
                  [--size WxH] [--particles N]

  Defaults: 60 frames, 320x180, 256 particles (a deliberately modest preset;
  use --frames 300 --size 1280x720 --particles 4096 for the full stress run).
*/

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shaders/stress_particles.comp.inc"
#include "shaders/stress_particles.vert.inc"
#include "shaders/stress_particles.frag.inc"
#include "shaders/stress_post.vert.inc"
#include "shaders/stress_post.frag.inc"

/* Every Vulkan call goes through SDL dispatch so the offload wrapper can
 * intercept swapchain creation, acquisition and present. */
#define VK_PTR(name) static PFN_##name p_##name
VK_PTR(vkCreateInstance);
VK_PTR(vkDestroyInstance);
VK_PTR(vkEnumeratePhysicalDevices);
VK_PTR(vkGetPhysicalDeviceProperties);
VK_PTR(vkGetPhysicalDeviceQueueFamilyProperties);
VK_PTR(vkGetPhysicalDeviceMemoryProperties);
VK_PTR(vkGetPhysicalDeviceSurfaceSupportKHR);
VK_PTR(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
VK_PTR(vkGetPhysicalDeviceSurfaceFormatsKHR);
VK_PTR(vkCreateDevice);
VK_PTR(vkGetDeviceProcAddr);
VK_PTR(vkGetDeviceQueue);
VK_PTR(vkCreateSwapchainKHR);
VK_PTR(vkGetSwapchainImagesKHR);
VK_PTR(vkAcquireNextImageKHR);
VK_PTR(vkDestroySwapchainKHR);
VK_PTR(vkCreateImageView);
VK_PTR(vkDestroyImageView);
VK_PTR(vkCreateRenderPass);
VK_PTR(vkDestroyRenderPass);
VK_PTR(vkCreateFramebuffer);
VK_PTR(vkDestroyFramebuffer);
VK_PTR(vkCreateCommandPool);
VK_PTR(vkAllocateCommandBuffers);
VK_PTR(vkDestroyCommandPool);
VK_PTR(vkCreateBuffer);
VK_PTR(vkGetBufferMemoryRequirements);
VK_PTR(vkAllocateMemory);
VK_PTR(vkBindBufferMemory);
VK_PTR(vkFreeMemory);
VK_PTR(vkDestroyBuffer);
VK_PTR(vkMapMemory);
VK_PTR(vkUnmapMemory);
VK_PTR(vkCreateImage);
VK_PTR(vkGetImageMemoryRequirements);
VK_PTR(vkBindImageMemory);
VK_PTR(vkDestroyImage);
VK_PTR(vkCreateSampler);
VK_PTR(vkDestroySampler);
VK_PTR(vkCreateShaderModule);
VK_PTR(vkDestroyShaderModule);
VK_PTR(vkCreateDescriptorSetLayout);
VK_PTR(vkDestroyDescriptorSetLayout);
VK_PTR(vkCreateDescriptorPool);
VK_PTR(vkDestroyDescriptorPool);
VK_PTR(vkAllocateDescriptorSets);
VK_PTR(vkUpdateDescriptorSets);
VK_PTR(vkCreatePipelineLayout);
VK_PTR(vkDestroyPipelineLayout);
VK_PTR(vkCreateGraphicsPipelines);
VK_PTR(vkCreateComputePipelines);
VK_PTR(vkDestroyPipeline);
VK_PTR(vkCreateSemaphore);
VK_PTR(vkDestroySemaphore);
VK_PTR(vkCreateFence);
VK_PTR(vkDestroyFence);
VK_PTR(vkWaitForFences);
VK_PTR(vkResetFences);
VK_PTR(vkResetCommandBuffer);
VK_PTR(vkBeginCommandBuffer);
VK_PTR(vkEndCommandBuffer);
VK_PTR(vkQueueSubmit);
VK_PTR(vkQueuePresentKHR);
VK_PTR(vkCmdBeginRenderPass);
VK_PTR(vkCmdEndRenderPass);
VK_PTR(vkCmdBindPipeline);
VK_PTR(vkCmdBindVertexBuffers);
VK_PTR(vkCmdDraw);
VK_PTR(vkCmdDispatch);
VK_PTR(vkCmdPushConstants);
VK_PTR(vkCmdBindDescriptorSets);
VK_PTR(vkCmdPipelineBarrier);
VK_PTR(vkCmdCopyImageToBuffer);
VK_PTR(vkDeviceWaitIdle);

#define vkCreateInstance p_vkCreateInstance
#define vkDestroyInstance p_vkDestroyInstance
#define vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices
#define vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties
#define vkGetPhysicalDeviceQueueFamilyProperties p_vkGetPhysicalDeviceQueueFamilyProperties
#define vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties
#define vkGetPhysicalDeviceSurfaceSupportKHR p_vkGetPhysicalDeviceSurfaceSupportKHR
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
#define vkGetPhysicalDeviceSurfaceFormatsKHR p_vkGetPhysicalDeviceSurfaceFormatsKHR
#define vkCreateDevice p_vkCreateDevice
#define vkGetDeviceProcAddr p_vkGetDeviceProcAddr
#define vkGetDeviceQueue p_vkGetDeviceQueue
#define vkCreateSwapchainKHR p_vkCreateSwapchainKHR
#define vkGetSwapchainImagesKHR p_vkGetSwapchainImagesKHR
#define vkAcquireNextImageKHR p_vkAcquireNextImageKHR
#define vkDestroySwapchainKHR p_vkDestroySwapchainKHR
#define vkCreateImageView p_vkCreateImageView
#define vkDestroyImageView p_vkDestroyImageView
#define vkCreateRenderPass p_vkCreateRenderPass
#define vkDestroyRenderPass p_vkDestroyRenderPass
#define vkCreateFramebuffer p_vkCreateFramebuffer
#define vkDestroyFramebuffer p_vkDestroyFramebuffer
#define vkCreateCommandPool p_vkCreateCommandPool
#define vkAllocateCommandBuffers p_vkAllocateCommandBuffers
#define vkDestroyCommandPool p_vkDestroyCommandPool
#define vkCreateBuffer p_vkCreateBuffer
#define vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements
#define vkAllocateMemory p_vkAllocateMemory
#define vkBindBufferMemory p_vkBindBufferMemory
#define vkFreeMemory p_vkFreeMemory
#define vkDestroyBuffer p_vkDestroyBuffer
#define vkMapMemory p_vkMapMemory
#define vkUnmapMemory p_vkUnmapMemory
#define vkCreateImage p_vkCreateImage
#define vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements
#define vkBindImageMemory p_vkBindImageMemory
#define vkDestroyImage p_vkDestroyImage
#define vkCreateSampler p_vkCreateSampler
#define vkDestroySampler p_vkDestroySampler
#define vkCreateShaderModule p_vkCreateShaderModule
#define vkDestroyShaderModule p_vkDestroyShaderModule
#define vkCreateDescriptorSetLayout p_vkCreateDescriptorSetLayout
#define vkDestroyDescriptorSetLayout p_vkDestroyDescriptorSetLayout
#define vkCreateDescriptorPool p_vkCreateDescriptorPool
#define vkDestroyDescriptorPool p_vkDestroyDescriptorPool
#define vkAllocateDescriptorSets p_vkAllocateDescriptorSets
#define vkUpdateDescriptorSets p_vkUpdateDescriptorSets
#define vkCreatePipelineLayout p_vkCreatePipelineLayout
#define vkDestroyPipelineLayout p_vkDestroyPipelineLayout
#define vkCreateGraphicsPipelines p_vkCreateGraphicsPipelines
#define vkCreateComputePipelines p_vkCreateComputePipelines
#define vkDestroyPipeline p_vkDestroyPipeline
#define vkCreateSemaphore p_vkCreateSemaphore
#define vkDestroySemaphore p_vkDestroySemaphore
#define vkCreateFence p_vkCreateFence
#define vkDestroyFence p_vkDestroyFence
#define vkWaitForFences p_vkWaitForFences
#define vkResetFences p_vkResetFences
#define vkResetCommandBuffer p_vkResetCommandBuffer
#define vkBeginCommandBuffer p_vkBeginCommandBuffer
#define vkEndCommandBuffer p_vkEndCommandBuffer
#define vkQueueSubmit p_vkQueueSubmit
#define vkQueuePresentKHR p_vkQueuePresentKHR
#define vkCmdBeginRenderPass p_vkCmdBeginRenderPass
#define vkCmdEndRenderPass p_vkCmdEndRenderPass
#define vkCmdBindPipeline p_vkCmdBindPipeline
#define vkCmdBindVertexBuffers p_vkCmdBindVertexBuffers
#define vkCmdDraw p_vkCmdDraw
#define vkCmdDispatch p_vkCmdDispatch
#define vkCmdPushConstants p_vkCmdPushConstants
#define vkCmdBindDescriptorSets p_vkCmdBindDescriptorSets
#define vkCmdPipelineBarrier p_vkCmdPipelineBarrier
#define vkCmdCopyImageToBuffer p_vkCmdCopyImageToBuffer
#define vkDeviceWaitIdle p_vkDeviceWaitIdle

#define VK_CHECK(x)                                                        \
    do {                                                                   \
        VkResult err_ = (x);                                               \
        if (err_ != VK_SUCCESS) {                                          \
            fprintf(stderr, "Vulkan error %d at %s:%d\n", err_, __FILE__, __LINE__); \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

#define LOAD_I(n)                                                          \
    do {                                                                   \
        p_##n = (PFN_##n)gipa(instance, #n);                                \
        if (!p_##n) {                                                      \
            fprintf(stderr, "missing instance proc %s\n", #n);             \
            return 1;                                                      \
        }                                                                  \
    } while (0)

#define LOAD_D(n)                                                          \
    do {                                                                   \
        p_##n = (PFN_##n)gdpa(device, #n);                                 \
        if (!p_##n) {                                                      \
            fprintf(stderr, "missing device proc %s\n", #n);               \
            return 1;                                                      \
        }                                                                  \
    } while (0)

typedef struct Particle {
    float pos[2];
    float vel[2];
    float color[3];
    float size;
} Particle;

typedef struct ScenePC {
    float resolution[2];
    float time;
    float pad;
} ScenePC;

typedef struct ComputePC {
    float dt;
    float time;
    uint32_t count;
    float pad;
} ComputePC;

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--offload|--native] [--frames N] [--duration SECONDS]\n"
            "          [--size WxH] [--particles N]\n"
            "  --offload       force SDLop headless/offload path\n"
            "  --native        force the video driver's native Vulkan WSI instead\n"
            "  --frames N      stop after N frames (default 60)\n"
            "  --duration S    run for S seconds instead of a fixed frame count\n"
            "  --size WxH      window size (default 320x180)\n"
            "  --particles N   instanced quads driven by the compute pass (default 256)\n",
            argv0);
}

static uint32_t find_memory_type(VkPhysicalDeviceMemoryProperties *props,
                                 uint32_t type_bits, VkMemoryPropertyFlags wanted)
{
    for (uint32_t i = 0; i < props->memoryTypeCount && i < 32; i++) {
        if ((type_bits & (1u << i)) &&
            (props->memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return UINT32_MAX;
}

static VkShaderModule make_shader_module(VkDevice device,
                                         const uint32_t *code, size_t words)
{
    VkShaderModuleCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = words * sizeof(*code),
        .pCode = code,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &sci, NULL, &module));
    return module;
}

static void buffer_barrier(VkCommandBuffer cmd, VkBuffer buffer,
                           VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                           VkAccessFlags src_access, VkAccessFlags dst_access)
{
    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0,
                         0, NULL, 1, &barrier, 0, NULL);
}

static void image_barrier(VkCommandBuffer cmd, VkImage image,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0,
                         0, NULL, 0, NULL, 1, &barrier);
}

static uint32_t frame_checksum(const uint8_t *pixels, int w, int h, int pitch)
{
    uint32_t hash = 2166136261u;
    for (int y = 0; y < h; y += 8) {
        for (int x = 0; x < w; x += 8) {
            uint32_t value = 0;
            memcpy(&value, pixels + (size_t)y * (size_t)pitch + (size_t)x * 4, 4);
            hash = (hash ^ value) * 16777619u;
        }
    }
    return hash ? hash : 1u;
}

int main(int argc, char *argv[])
{
    int frames_target = 60;
    double duration_sec = 0.0;
    int win_w = 320, win_h = 180;
    uint32_t particle_count = 256;
    const char *offload_hint = NULL; /* NULL: leave SDLop's auto policy alone */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(a, "--offload")) {
            offload_hint = "force";
        } else if (!strcmp(a, "--native")) {
            offload_hint = "0";
        } else if (!strcmp(a, "--frames") && i + 1 < argc) {
            frames_target = atoi(argv[++i]);
        } else if (!strcmp(a, "--duration") && i + 1 < argc) {
            duration_sec = atof(argv[++i]);
        } else if (!strcmp(a, "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2) {
                usage(argv[0]);
                return 1;
            }
        } else if (!strcmp(a, "--particles") && i + 1 < argc) {
            particle_count = (uint32_t)atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (frames_target < 1 || win_w < 64 || win_h < 64 ||
        particle_count < 1 || particle_count > 262144) {
        usage(argv[0]);
        return 1;
    }
    if (duration_sec > 0.0) {
        frames_target = INT32_MAX;
    }

    /* An explicit CLI choice wins over the environment. With no flag, leave
     * the hint untouched so SDLop's capability-based auto policy decides. */
    if (offload_hint &&
        !SDL_SetHintWithPriority("SDLOP_VK_OFFLOAD", offload_hint, SDL_HINT_OVERRIDE)) {
        fprintf(stderr, "SDL_SetHintWithPriority failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        fprintf(stderr, "SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa) {
        fprintf(stderr, "SDL_Vulkan_GetVkGetInstanceProcAddr returned NULL\n");
        return 1;
    }
    p_vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!p_vkCreateInstance) {
        fprintf(stderr, "missing vkCreateInstance\n");
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("SDLop - Vulkan stress",
                                          win_w, win_h, SDL_WINDOW_VULKAN);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    Uint32 ext_count = 0;
    char const *const *exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    bool offload = false;
    for (Uint32 i = 0; i < ext_count; i++) {
        if (exts && exts[i] && strstr(exts[i], "headless")) {
            offload = true;
        }
    }

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "sdlop_vulkan_stress",
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = exts,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ici, NULL, &instance));

    LOAD_I(vkDestroyInstance);
    LOAD_I(vkEnumeratePhysicalDevices);
    LOAD_I(vkGetPhysicalDeviceProperties);
    LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_I(vkGetPhysicalDeviceMemoryProperties);
    LOAD_I(vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD_I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_I(vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_I(vkCreateDevice);
    LOAD_I(vkGetDeviceProcAddr);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, instance, NULL, &surface)) {
        fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return 1;
    }

    uint32_t gpu_count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &gpu_count, NULL));
    if (!gpu_count) {
        fprintf(stderr, "no Vulkan physical devices\n");
        return 1;
    }
    VkPhysicalDevice *gpus = (VkPhysicalDevice *)alloca(gpu_count * sizeof(*gpus));
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &gpu_count, gpus));

    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    uint32_t qf = UINT32_MAX;
    for (uint32_t i = 0; i < gpu_count && gpu == VK_NULL_HANDLE; i++) {
        uint32_t qfc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qfc, NULL);
        VkQueueFamilyProperties *qfp = (VkQueueFamilyProperties *)alloca(qfc * sizeof(*qfp));
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qfc, qfp);
        for (uint32_t j = 0; j < qfc; j++) {
            VkBool32 present_ok = VK_FALSE;
            VkQueueFlags needed = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((qfp[j].queueFlags & needed) == needed &&
                vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], j, surface, &present_ok) == VK_SUCCESS &&
                present_ok) {
                gpu = gpus[i];
                qf = j;
                break;
            }
        }
    }
    if (gpu == VK_NULL_HANDLE) {
        fprintf(stderr, "no graphics+compute+present queue family\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("Vulkan device: %s (queue family %u)\n", props.deviceName, qf);
    printf("Vulkan offload: %s\n", offload ? "yes (VK_EXT_headless_surface)" : "no (native WSI)");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio,
    };
    const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &dqci,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = dev_exts,
    };
    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(gpu, &dci, NULL, &device));

    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
    if (!gdpa) {
        fprintf(stderr, "no vkGetDeviceProcAddr\n");
        return 1;
    }
    LOAD_D(vkGetDeviceQueue);
    LOAD_D(vkCreateSwapchainKHR);
    LOAD_D(vkGetSwapchainImagesKHR);
    LOAD_D(vkAcquireNextImageKHR);
    LOAD_D(vkDestroySwapchainKHR);
    LOAD_D(vkCreateImageView);
    LOAD_D(vkDestroyImageView);
    LOAD_D(vkCreateRenderPass);
    LOAD_D(vkDestroyRenderPass);
    LOAD_D(vkCreateFramebuffer);
    LOAD_D(vkDestroyFramebuffer);
    LOAD_D(vkCreateCommandPool);
    LOAD_D(vkAllocateCommandBuffers);
    LOAD_D(vkDestroyCommandPool);
    LOAD_D(vkCreateBuffer);
    LOAD_D(vkGetBufferMemoryRequirements);
    LOAD_D(vkAllocateMemory);
    LOAD_D(vkBindBufferMemory);
    LOAD_D(vkFreeMemory);
    LOAD_D(vkDestroyBuffer);
    LOAD_D(vkMapMemory);
    LOAD_D(vkUnmapMemory);
    LOAD_D(vkCreateImage);
    LOAD_D(vkGetImageMemoryRequirements);
    LOAD_D(vkBindImageMemory);
    LOAD_D(vkDestroyImage);
    LOAD_D(vkCreateSampler);
    LOAD_D(vkDestroySampler);
    LOAD_D(vkCreateShaderModule);
    LOAD_D(vkDestroyShaderModule);
    LOAD_D(vkCreateDescriptorSetLayout);
    LOAD_D(vkDestroyDescriptorSetLayout);
    LOAD_D(vkCreateDescriptorPool);
    LOAD_D(vkDestroyDescriptorPool);
    LOAD_D(vkAllocateDescriptorSets);
    LOAD_D(vkUpdateDescriptorSets);
    LOAD_D(vkCreatePipelineLayout);
    LOAD_D(vkDestroyPipelineLayout);
    LOAD_D(vkCreateGraphicsPipelines);
    LOAD_D(vkCreateComputePipelines);
    LOAD_D(vkDestroyPipeline);
    LOAD_D(vkCreateSemaphore);
    LOAD_D(vkDestroySemaphore);
    LOAD_D(vkCreateFence);
    LOAD_D(vkDestroyFence);
    LOAD_D(vkWaitForFences);
    LOAD_D(vkResetFences);
    LOAD_D(vkResetCommandBuffer);
    LOAD_D(vkBeginCommandBuffer);
    LOAD_D(vkEndCommandBuffer);
    LOAD_D(vkQueueSubmit);
    LOAD_D(vkQueuePresentKHR);
    LOAD_D(vkCmdBeginRenderPass);
    LOAD_D(vkCmdEndRenderPass);
    LOAD_D(vkCmdBindPipeline);
    LOAD_D(vkCmdBindVertexBuffers);
    LOAD_D(vkCmdDraw);
    LOAD_D(vkCmdDispatch);
    LOAD_D(vkCmdPushConstants);
    LOAD_D(vkCmdBindDescriptorSets);
    LOAD_D(vkCmdPipelineBarrier);
    LOAD_D(vkCmdCopyImageToBuffer);
    LOAD_D(vkDeviceWaitIdle);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, qf, 0, &queue);

    VkPhysicalDeviceMemoryProperties memprops;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memprops);

    /* ---- swapchain ---- */
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
    uint32_t fmt_count = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fmt_count, NULL));
    VkSurfaceFormatKHR *fmts = (VkSurfaceFormatKHR *)alloca(fmt_count * sizeof(*fmts));
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fmt_count, fmts));
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < fmt_count; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            fmt = fmts[i];
            break;
        }
    }

    SDL_GetWindowSize(window, &win_w, &win_h);
    VkExtent2D extent = { (uint32_t)win_w, (uint32_t)win_h };
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
        win_w = (int)extent.width;
        win_h = (int)extent.height;
    }
    printf("Vulkan surface format: 0x%x  extent: %ux%u  particles: %u\n",
           fmt.format, extent.width, extent.height, particle_count);

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSwapchainKHR(device, &sci, NULL, &swapchain));
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, NULL));
    VkImage *swap_images = (VkImage *)alloca(image_count * sizeof(*swap_images));
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, swap_images));
    VkImageView *swap_views = (VkImageView *)alloca(image_count * sizeof(*swap_views));
    VkFramebuffer *post_fbs = (VkFramebuffer *)alloca(image_count * sizeof(*post_fbs));

    /* ---- render passes ---- */
    VkAttachmentDescription scene_att = {
        .format = fmt.format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkAttachmentReference scene_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription scene_sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &scene_ref,
    };
    VkRenderPassCreateInfo scene_rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &scene_att,
        .subpassCount = 1, .pSubpasses = &scene_sub,
    };
    VkRenderPass scene_rp = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(device, &scene_rpci, NULL, &scene_rp));

    VkAttachmentDescription post_att = scene_att;
    post_att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference post_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription post_sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &post_ref,
    };
    VkRenderPassCreateInfo post_rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &post_att,
        .subpassCount = 1, .pSubpasses = &post_sub,
    };
    VkRenderPass post_rp = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(device, &post_rpci, NULL, &post_rp));

    /* ---- offscreen scene target ---- */
    VkImageCreateInfo scene_image_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt.format,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage scene_image = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImage(device, &scene_image_ci, NULL, &scene_image));
    VkMemoryRequirements scene_req;
    vkGetImageMemoryRequirements(device, scene_image, &scene_req);
    uint32_t scene_mem_type = find_memory_type(&memprops, scene_req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (scene_mem_type == UINT32_MAX) {
        scene_mem_type = find_memory_type(&memprops, scene_req.memoryTypeBits, 0);
    }
    VkDeviceMemory scene_mem = VK_NULL_HANDLE;
    VkMemoryAllocateInfo scene_mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = scene_req.size, .memoryTypeIndex = scene_mem_type,
    };
    VK_CHECK(vkAllocateMemory(device, &scene_mai, NULL, &scene_mem));
    VK_CHECK(vkBindImageMemory(device, scene_image, scene_mem, 0));

    VkImageViewCreateInfo scene_view_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = scene_image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt.format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageView scene_view = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(device, &scene_view_ci, NULL, &scene_view));

    VkFramebufferCreateInfo scene_fb_ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = scene_rp, .attachmentCount = 1, .pAttachments = &scene_view,
        .width = extent.width, .height = extent.height, .layers = 1,
    };
    VkFramebuffer scene_fb = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFramebuffer(device, &scene_fb_ci, NULL, &scene_fb));

    for (uint32_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo ivci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = swap_images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt.format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VK_CHECK(vkCreateImageView(device, &ivci, NULL, &swap_views[i]));
        VkFramebufferCreateInfo fbci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = post_rp, .attachmentCount = 1, .pAttachments = &swap_views[i],
            .width = extent.width, .height = extent.height, .layers = 1,
        };
        VK_CHECK(vkCreateFramebuffer(device, &fbci, NULL, &post_fbs[i]));
    }

    /* ---- buffers ---- */
    VkDeviceMemory particle_mem = VK_NULL_HANDLE;
    VkBuffer particle_buf = VK_NULL_HANDLE;
    size_t particle_bytes = (size_t)particle_count * sizeof(Particle);
    VkBufferCreateInfo particle_bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = particle_bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
    };
    VK_CHECK(vkCreateBuffer(device, &particle_bci, NULL, &particle_buf));
    VkMemoryRequirements particle_req;
    vkGetBufferMemoryRequirements(device, particle_buf, &particle_req);
    uint32_t particle_mem_type = find_memory_type(&memprops, particle_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (particle_mem_type == UINT32_MAX) {
        fprintf(stderr, "no host-visible coherent memory for particle buffer\n");
        return 1;
    }
    VkMemoryAllocateInfo particle_mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = particle_req.size, .memoryTypeIndex = particle_mem_type,
    };
    VK_CHECK(vkAllocateMemory(device, &particle_mai, NULL, &particle_mem));
    VK_CHECK(vkBindBufferMemory(device, particle_buf, particle_mem, 0));
    Particle *particles = NULL;
    VK_CHECK(vkMapMemory(device, particle_mem, 0, particle_bytes, 0, (void **)&particles));
    for (uint32_t i = 0; i < particle_count; i++) {
        float a = (float)i * 2.39996323f;
        float r = 0.12f + 0.85f * sqrtf((float)i / (float)particle_count);
        particles[i].pos[0] = cosf(a) * r;
        particles[i].pos[1] = sinf(a) * r;
        particles[i].vel[0] = -sinf(a) * 0.25f;
        particles[i].vel[1] = cosf(a) * 0.25f;
        float hue = (float)i / (float)particle_count;
        particles[i].color[0] = 0.55f + 0.45f * sinf(hue * 6.283f);
        particles[i].color[1] = 0.55f + 0.45f * sinf(hue * 6.283f + 2.1f);
        particles[i].color[2] = 0.55f + 0.45f * sinf(hue * 6.283f + 4.2f);
        particles[i].size = 0.018f + 0.045f * ((float)((i * 37u) % 101u) / 101.0f);
    }
    vkUnmapMemory(device, particle_mem);

    static const float corners[] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,
    };
    VkBuffer corner_buf = VK_NULL_HANDLE;
    VkDeviceMemory corner_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo corner_bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(corners), .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
    };
    VK_CHECK(vkCreateBuffer(device, &corner_bci, NULL, &corner_buf));
    VkMemoryRequirements corner_req;
    vkGetBufferMemoryRequirements(device, corner_buf, &corner_req);
    uint32_t corner_mem_type = find_memory_type(&memprops, corner_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo corner_mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = corner_req.size, .memoryTypeIndex = corner_mem_type,
    };
    VK_CHECK(vkAllocateMemory(device, &corner_mai, NULL, &corner_mem));
    VK_CHECK(vkBindBufferMemory(device, corner_buf, corner_mem, 0));
    float *corner_map = NULL;
    VK_CHECK(vkMapMemory(device, corner_mem, 0, sizeof(corners), 0, (void **)&corner_map));
    memcpy(corner_map, corners, sizeof(corners));
    vkUnmapMemory(device, corner_mem);

    size_t rb_size = (size_t)extent.width * (size_t)extent.height * 4u;
    VkBuffer rb_buf = VK_NULL_HANDLE;
    VkDeviceMemory rb_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo rb_bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = rb_size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VK_CHECK(vkCreateBuffer(device, &rb_bci, NULL, &rb_buf));
    VkMemoryRequirements rb_req;
    vkGetBufferMemoryRequirements(device, rb_buf, &rb_req);
    uint32_t rb_mem_type = find_memory_type(&memprops, rb_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo rb_mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = rb_req.size, .memoryTypeIndex = rb_mem_type,
    };
    VK_CHECK(vkAllocateMemory(device, &rb_mai, NULL, &rb_mem));
    VK_CHECK(vkBindBufferMemory(device, rb_buf, rb_mem, 0));
    uint8_t *rb_map = NULL;
    VK_CHECK(vkMapMemory(device, rb_mem, 0, rb_size, 0, (void **)&rb_map));

    /* ---- descriptors ---- */
    VkDescriptorSetLayoutBinding compute_binding = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    VkDescriptorSetLayoutCreateInfo compute_dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &compute_binding,
    };
    VkDescriptorSetLayout compute_dsl = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &compute_dslci, NULL, &compute_dsl));

    VkDescriptorSetLayoutBinding post_binding = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo post_dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &post_binding,
    };
    VkDescriptorSetLayout post_dsl = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &post_dslci, NULL, &post_dsl));

    VkDescriptorPoolSize pool_sizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 2, .pPoolSizes = pool_sizes,
    };
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &dpool));

    VkDescriptorSet compute_set = VK_NULL_HANDLE;
    VkDescriptorSet post_set = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool, .descriptorSetCount = 2,
        .pSetLayouts = (VkDescriptorSetLayout[]){ compute_dsl, post_dsl },
    };
    VK_CHECK(vkAllocateDescriptorSets(device, &dsai, sets));
    compute_set = sets[0];
    post_set = sets[1];

    VkDescriptorBufferInfo particle_dbi = {
        .buffer = particle_buf, .offset = 0, .range = VK_WHOLE_SIZE,
    };
    VkWriteDescriptorSet compute_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = compute_set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &particle_dbi,
    };
    vkUpdateDescriptorSets(device, 1, &compute_write, 0, NULL);

    VkSamplerCreateInfo sampler_ci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxAnisotropy = 1.0f,
        .minLod = 0.0f, .maxLod = 0.0f,
    };
    VkSampler sampler = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(device, &sampler_ci, NULL, &sampler));

    VkDescriptorImageInfo scene_dii = {
        .sampler = sampler, .imageView = scene_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet post_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = post_set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &scene_dii,
    };
    vkUpdateDescriptorSets(device, 1, &post_write, 0, NULL);

    /* ---- shader modules and pipelines ---- */
    VkShaderModule particle_comp = make_shader_module(device, stress_particles_comp_spv,
        sizeof(stress_particles_comp_spv) / sizeof(stress_particles_comp_spv[0]));
    VkShaderModule particle_vert = make_shader_module(device, stress_particles_vert_spv,
        sizeof(stress_particles_vert_spv) / sizeof(stress_particles_vert_spv[0]));
    VkShaderModule particle_frag = make_shader_module(device, stress_particles_frag_spv,
        sizeof(stress_particles_frag_spv) / sizeof(stress_particles_frag_spv[0]));
    VkShaderModule post_vert = make_shader_module(device, stress_post_vert_spv,
        sizeof(stress_post_vert_spv) / sizeof(stress_post_vert_spv[0]));
    VkShaderModule post_frag = make_shader_module(device, stress_post_frag_spv,
        sizeof(stress_post_frag_spv) / sizeof(stress_post_frag_spv[0]));

    VkPushConstantRange compute_pc_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(ComputePC),
    };
    VkPipelineLayoutCreateInfo compute_plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &compute_dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &compute_pc_range,
    };
    VkPipelineLayout compute_layout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &compute_plci, NULL, &compute_layout));

    VkComputePipelineCreateInfo compute_pci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = particle_comp, .pName = "main",
        },
        .layout = compute_layout,
    };
    VkPipeline compute_pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute_pci, NULL, &compute_pipeline));

    VkPushConstantRange scene_pc_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0, .size = sizeof(ScenePC),
    };
    VkPipelineLayoutCreateInfo particle_plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &scene_pc_range,
    };
    VkPipelineLayout particle_layout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &particle_plci, NULL, &particle_layout));

    VkPushConstantRange post_pc_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(ScenePC),
    };
    VkPipelineLayoutCreateInfo post_plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &post_dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &post_pc_range,
    };
    VkPipelineLayout post_layout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &post_plci, NULL, &post_layout));

    VkViewport viewport = { 0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f };
    VkRect2D scissor = { { 0, 0 }, { extent.width, extent.height } };
    VkPipelineViewportStateCreateInfo vpci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo msaa = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkVertexInputBindingDescription bindings[2] = {
        { 0, sizeof(float) * 2, VK_VERTEX_INPUT_RATE_VERTEX },
        { 1, sizeof(Particle), VK_VERTEX_INPUT_RATE_INSTANCE },
    };
    VkVertexInputAttributeDescription attrs[4] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
        { 1, 1, VK_FORMAT_R32G32_SFLOAT, offsetof(Particle, pos) },
        { 2, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Particle, color) },
        { 3, 1, VK_FORMAT_R32_SFLOAT, offsetof(Particle, size) },
    };
    VkPipelineVertexInputStateCreateInfo particle_visci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = bindings,
        .vertexAttributeDescriptionCount = 4, .pVertexAttributeDescriptions = attrs,
    };
    VkPipelineInputAssemblyStateCreateInfo input_ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineColorBlendAttachmentState add_blend = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo particle_cbsci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &add_blend,
    };
    VkPipelineShaderStageCreateInfo particle_stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = particle_vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = particle_frag, .pName = "main" },
    };
    VkGraphicsPipelineCreateInfo particle_gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = particle_stages,
        .pVertexInputState = &particle_visci,
        .pInputAssemblyState = &input_ia,
        .pViewportState = &vpci,
        .pRasterizationState = &raster,
        .pMultisampleState = &msaa,
        .pColorBlendState = &particle_cbsci,
        .layout = particle_layout, .renderPass = scene_rp, .subpass = 0,
    };
    VkPipeline particle_pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &particle_gpci,
                                       NULL, &particle_pipeline));

    VkPipelineVertexInputStateCreateInfo empty_visci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineColorBlendAttachmentState opaque_blend = {
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo post_cbsci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &opaque_blend,
    };
    VkPipelineShaderStageCreateInfo post_stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = post_vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = post_frag, .pName = "main" },
    };
    VkGraphicsPipelineCreateInfo post_gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = post_stages,
        .pVertexInputState = &empty_visci,
        .pInputAssemblyState = &input_ia,
        .pViewportState = &vpci,
        .pRasterizationState = &raster,
        .pMultisampleState = &msaa,
        .pColorBlendState = &post_cbsci,
        .layout = post_layout, .renderPass = post_rp, .subpass = 0,
    };
    VkPipeline post_pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &post_gpci,
                                       NULL, &post_pipeline));

    /* ---- command buffers and sync ---- */
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qf,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));

    VkSemaphoreCreateInfo semci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquire_sem = VK_NULL_HANDLE, render_sem = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSemaphore(device, &semci, NULL, &acquire_sem));
    VK_CHECK(vkCreateSemaphore(device, &semci, NULL, &render_sem));
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fci, NULL, &fence));

    /* ---- frame loop ---- */
    int frames = 0;
    uint32_t first_checksum = 0, last_checksum = 0;
    float sim_time = 0.0f;
    const float dt = 1.0f / 60.0f;
    Uint64 start_ns = SDL_GetTicksNS();
    Uint64 deadline_ns = duration_sec > 0.0
        ? start_ns + (Uint64)(duration_sec * 1e9) : 0;
    bool done = false;

    while (!done && frames < frames_target) {
        if (deadline_ns && SDL_GetTicksNS() >= deadline_ns) {
            break;
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(device, 1, &fence));
        VK_CHECK(vkResetCommandBuffer(cmd, 0));

        uint32_t index = 0;
        VkResult acq = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                             acquire_sem, VK_NULL_HANDLE, &index);
        if (acq == VK_SUBOPTIMAL_KHR || acq == VK_ERROR_OUT_OF_DATE_KHR) {
            continue;
        }
        VK_CHECK(acq);

        sim_time += dt;
        ScenePC scene_pc = { { (float)extent.width, (float)extent.height }, sim_time, 0.0f };
        ComputePC compute_pc = { dt, sim_time, particle_count, 0.0f };

        VkCommandBufferBeginInfo cbi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        VK_CHECK(vkBeginCommandBuffer(cmd, &cbi));

        /* Host initialization -> compute shader. */
        buffer_barrier(cmd, particle_buf,
                       VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_HOST_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout,
                                0, 1, &compute_set, 0, NULL);
        vkCmdPushConstants(cmd, compute_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(compute_pc), &compute_pc);
        vkCmdDispatch(cmd, (particle_count + 63u) / 64u, 1, 1);

        /* Compute writes -> vertex shader reads. */
        buffer_barrier(cmd, particle_buf,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);

        VkClearValue scene_clear = { .color = { { 0.005f, 0.008f, 0.015f, 1.0f } } };
        VkRenderPassBeginInfo scene_rbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = scene_rp, .framebuffer = scene_fb,
            .renderArea = { { 0, 0 }, extent },
            .clearValueCount = 1, .pClearValues = &scene_clear,
        };
        vkCmdBeginRenderPass(cmd, &scene_rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particle_pipeline);
        vkCmdPushConstants(cmd, particle_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(scene_pc), &scene_pc);
        VkDeviceSize offsets[2] = { 0, 0 };
        vkCmdBindVertexBuffers(cmd, 0, 2,
                               (VkBuffer[]){ corner_buf, particle_buf }, offsets);
        vkCmdDraw(cmd, 6, particle_count, 0, 0);
        vkCmdEndRenderPass(cmd);

        VkClearValue post_clear = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } };
        VkRenderPassBeginInfo post_rbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = post_rp, .framebuffer = post_fbs[index],
            .renderArea = { { 0, 0 }, extent },
            .clearValueCount = 1, .pClearValues = &post_clear,
        };
        vkCmdBeginRenderPass(cmd, &post_rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout,
                                0, 1, &post_set, 0, NULL);
        vkCmdPushConstants(cmd, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(scene_pc), &scene_pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);

        image_barrier(cmd, swap_images[index],
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {
            .bufferOffset = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { extent.width, extent.height, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, swap_images[index],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_buf, 1, &copy);
        image_barrier(cmd, swap_images[index],
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_TRANSFER_READ_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        VK_CHECK(vkEndCommandBuffer(cmd));

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &acquire_sem,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1, .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &render_sem,
        };
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));

        VkPresentInfoKHR present = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &render_sem,
            .swapchainCount = 1, .pSwapchains = &swapchain,
            .pImageIndices = &index,
        };
        VkResult pres = vkQueuePresentKHR(queue, &present);
        if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "present failed: %d\n", pres);
            break;
        }

        if (frames == 0 || frames == frames_target - 1 ||
            (deadline_ns && SDL_GetTicksNS() >= deadline_ns)) {
            VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
            uint32_t checksum = frame_checksum(rb_map, extent.width, extent.height,
                                               (int)extent.width * 4);
            if (frames == 0) {
                first_checksum = checksum;
            }
            last_checksum = checksum;
        }
        frames++;
    }
    VK_CHECK(vkDeviceWaitIdle(device));
    Uint64 elapsed_ns = SDL_GetTicksNS() - start_ns;
    double elapsed = (double)elapsed_ns / 1e9;

    printf("presented %d frames in %.3f s => %.1f fps\n",
           frames, elapsed, elapsed > 0.0 ? frames / elapsed : 0.0);
    printf("readback checksum: first=0x%08x last=0x%08x animated=%s\n",
           first_checksum, last_checksum,
           first_checksum && last_checksum && first_checksum != last_checksum ? "yes" : "no");

    vkUnmapMemory(device, rb_mem);
    vkDestroySemaphore(device, acquire_sem, NULL);
    vkDestroySemaphore(device, render_sem, NULL);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyPipeline(device, compute_pipeline, NULL);
    vkDestroyPipeline(device, particle_pipeline, NULL);
    vkDestroyPipeline(device, post_pipeline, NULL);
    vkDestroyPipelineLayout(device, compute_layout, NULL);
    vkDestroyPipelineLayout(device, particle_layout, NULL);
    vkDestroyPipelineLayout(device, post_layout, NULL);
    vkDestroyShaderModule(device, particle_comp, NULL);
    vkDestroyShaderModule(device, particle_vert, NULL);
    vkDestroyShaderModule(device, particle_frag, NULL);
    vkDestroyShaderModule(device, post_vert, NULL);
    vkDestroyShaderModule(device, post_frag, NULL);
    vkDestroyDescriptorPool(device, dpool, NULL);
    vkDestroyDescriptorSetLayout(device, compute_dsl, NULL);
    vkDestroyDescriptorSetLayout(device, post_dsl, NULL);
    vkDestroySampler(device, sampler, NULL);
    vkFreeMemory(device, rb_mem, NULL);
    vkDestroyBuffer(device, rb_buf, NULL);
    vkFreeMemory(device, corner_mem, NULL);
    vkDestroyBuffer(device, corner_buf, NULL);
    vkFreeMemory(device, particle_mem, NULL);
    vkDestroyBuffer(device, particle_buf, NULL);
    vkDestroyFramebuffer(device, scene_fb, NULL);
    for (uint32_t i = 0; i < image_count; i++) {
        vkDestroyFramebuffer(device, post_fbs[i], NULL);
        vkDestroyImageView(device, swap_views[i], NULL);
    }
    vkDestroyImageView(device, scene_view, NULL);
    vkFreeMemory(device, scene_mem, NULL);
    vkDestroyImage(device, scene_image, NULL);
    vkDestroyRenderPass(device, scene_rp, NULL);
    vkDestroyRenderPass(device, post_rp, NULL);
    vkDestroySwapchainKHR(device, swapchain, NULL);
    SDL_Vulkan_DestroySurface(instance, surface, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    SDL_Vulkan_UnloadLibrary();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
