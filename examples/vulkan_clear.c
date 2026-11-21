/*
  SDLop example: Vulkan clear-color rendering through a real swapchain.

  By default this uses the platform's native Vulkan WSI. On platforms where
  presentation is CPU-only (no dmabuf/PRIME scanout), pass --offload to route
  it through SDLop's VK_EXT_headless_surface offload wrapper: rendering stays
  on the Vulkan device and vkQueuePresentKHR reads the image back into the
  window's XRGB8888 shm/dumb buffer.

  Some drivers (e.g. NVIDIA 470) do not support swapchains on headless
  surfaces. On such systems use a driver that does (for example, set
  VK_ICD_FILENAMES to Mesa lavapipe) to exercise the CPU-only present path.

  Usage: vulkan_clear [--offload] [frames]
         (a frame count enables the benchmark timing output)
*/

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* All Vulkan calls go through SDL's dispatch so the offload wrapper can
 * intercept swapchain/present when --offload is used. */
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
VK_PTR(vkCreateRenderPass);
VK_PTR(vkCreateImageView);
VK_PTR(vkCreateFramebuffer);
VK_PTR(vkCreateCommandPool);
VK_PTR(vkAllocateCommandBuffers);
VK_PTR(vkCreateBuffer);
VK_PTR(vkGetBufferMemoryRequirements);
VK_PTR(vkAllocateMemory);
VK_PTR(vkBindBufferMemory);
VK_PTR(vkCreateSemaphore);
VK_PTR(vkCreateFence);
VK_PTR(vkAcquireNextImageKHR);
VK_PTR(vkWaitForFences);
VK_PTR(vkResetFences);
VK_PTR(vkResetCommandBuffer);
VK_PTR(vkBeginCommandBuffer);
VK_PTR(vkCmdBeginRenderPass);
VK_PTR(vkCmdEndRenderPass);
VK_PTR(vkCmdPipelineBarrier);
VK_PTR(vkCmdCopyImageToBuffer);
VK_PTR(vkEndCommandBuffer);
VK_PTR(vkQueueSubmit);
VK_PTR(vkQueuePresentKHR);
VK_PTR(vkMapMemory);
VK_PTR(vkUnmapMemory);
VK_PTR(vkDeviceWaitIdle);
VK_PTR(vkDestroySemaphore);
VK_PTR(vkDestroyFence);
VK_PTR(vkFreeMemory);
VK_PTR(vkDestroyBuffer);
VK_PTR(vkDestroyCommandPool);
VK_PTR(vkDestroyFramebuffer);
VK_PTR(vkDestroyImageView);
VK_PTR(vkDestroyRenderPass);
VK_PTR(vkDestroySwapchainKHR);
VK_PTR(vkDestroyDevice);

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
#define vkCreateRenderPass p_vkCreateRenderPass
#define vkCreateImageView p_vkCreateImageView
#define vkCreateFramebuffer p_vkCreateFramebuffer
#define vkCreateCommandPool p_vkCreateCommandPool
#define vkAllocateCommandBuffers p_vkAllocateCommandBuffers
#define vkCreateBuffer p_vkCreateBuffer
#define vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements
#define vkAllocateMemory p_vkAllocateMemory
#define vkBindBufferMemory p_vkBindBufferMemory
#define vkCreateSemaphore p_vkCreateSemaphore
#define vkCreateFence p_vkCreateFence
#define vkAcquireNextImageKHR p_vkAcquireNextImageKHR
#define vkWaitForFences p_vkWaitForFences
#define vkResetFences p_vkResetFences
#define vkResetCommandBuffer p_vkResetCommandBuffer
#define vkBeginCommandBuffer p_vkBeginCommandBuffer
#define vkCmdBeginRenderPass p_vkCmdBeginRenderPass
#define vkCmdEndRenderPass p_vkCmdEndRenderPass
#define vkCmdPipelineBarrier p_vkCmdPipelineBarrier
#define vkCmdCopyImageToBuffer p_vkCmdCopyImageToBuffer
#define vkEndCommandBuffer p_vkEndCommandBuffer
#define vkQueueSubmit p_vkQueueSubmit
#define vkQueuePresentKHR p_vkQueuePresentKHR
#define vkMapMemory p_vkMapMemory
#define vkUnmapMemory p_vkUnmapMemory
#define vkDeviceWaitIdle p_vkDeviceWaitIdle
#define vkDestroySemaphore p_vkDestroySemaphore
#define vkDestroyFence p_vkDestroyFence
#define vkFreeMemory p_vkFreeMemory
#define vkDestroyBuffer p_vkDestroyBuffer
#define vkDestroyCommandPool p_vkDestroyCommandPool
#define vkDestroyFramebuffer p_vkDestroyFramebuffer
#define vkDestroyImageView p_vkDestroyImageView
#define vkDestroyRenderPass p_vkDestroyRenderPass
#define vkDestroySwapchainKHR p_vkDestroySwapchainKHR
#define vkDestroyDevice p_vkDestroyDevice

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
        p_##n = (PFN_##n)gipa(instance, #n);                                   \
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

int main(int argc, char *argv[])
{
    int bench_frames = 0;
    bool force_offload = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--offload") == 0) {
            force_offload = true;
        } else if (argv[i][0] != '-') {
            bench_frames = atoi(argv[i]);
        } else {
            fprintf(stderr, "usage: vulkan_clear [--offload] [frames]\n");
            return 1;
        }
    }
    const int total_frames = bench_frames ? bench_frames : 200;

    if (force_offload) {
        SDL_SetHint("SDLOP_VK_OFFLOAD", "force");
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

    SDL_Window *window = SDL_CreateWindow("SDLop - Vulkan clear", 640, 480, SDL_WINDOW_VULKAN);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    Uint32 ext_count = 0;
    char const *const *exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    printf("Vulkan instance extensions:");
    for (Uint32 i = 0; i < ext_count; i++) {
        printf(" %s", exts[i]);
    }
    printf("\n");

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "sdlop_vulkan_clear",
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = exts,
    };
    VkInstance instance;
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

    VkSurfaceKHR surface;
    if (!SDL_Vulkan_CreateSurface(window, instance, NULL, &surface)) {
        fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return 1;
    }

    uint32_t gpu_count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &gpu_count, NULL));
    if (gpu_count == 0) {
        fprintf(stderr, "no Vulkan physical devices\n");
        return 1;
    }
    VkPhysicalDevice *gpus = (VkPhysicalDevice *)alloca(gpu_count * sizeof(*gpus));
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &gpu_count, gpus));

    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    uint32_t qf = UINT32_MAX;
    for (uint32_t i = 0; i < gpu_count && gpu == VK_NULL_HANDLE; i++) {
        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qf_count, NULL);
        VkQueueFamilyProperties *qfp = (VkQueueFamilyProperties *)alloca(qf_count * sizeof(*qfp));
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qf_count, qfp);
        for (uint32_t j = 0; j < qf_count; j++) {
            VkBool32 present_ok = VK_FALSE;
            if ((qfp[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], j, surface, &present_ok) == VK_SUCCESS &&
                present_ok) {
                gpu = gpus[i];
                qf = j;
                break;
            }
        }
    }
    if (gpu == VK_NULL_HANDLE) {
        fprintf(stderr, "no graphics+present queue family\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("Vulkan device: %s (queue family %u)\n", props.deviceName, qf);
    printf("Vulkan offload: %s\n",
           strstr((const char *)exts[1] ? exts[1] : "", "headless") ? "yes (VK_EXT_headless_surface)" : "no (native WSI)");

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
    VkDevice device;
    VK_CHECK(vkCreateDevice(gpu, &dci, NULL, &device));

    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
    if (!gdpa) {
        fprintf(stderr, "no vkGetDeviceProcAddr\n");
        return 1;
    }
    LOAD_D(vkGetDeviceQueue);
    LOAD_D(vkCreateSwapchainKHR);
    LOAD_D(vkGetSwapchainImagesKHR);
    LOAD_D(vkCreateRenderPass);
    LOAD_D(vkCreateImageView);
    LOAD_D(vkCreateFramebuffer);
    LOAD_D(vkCreateCommandPool);
    LOAD_D(vkAllocateCommandBuffers);
    LOAD_D(vkCreateBuffer);
    LOAD_D(vkGetBufferMemoryRequirements);
    LOAD_D(vkAllocateMemory);
    LOAD_D(vkBindBufferMemory);
    LOAD_D(vkCreateSemaphore);
    LOAD_D(vkCreateFence);
    LOAD_D(vkAcquireNextImageKHR);
    LOAD_D(vkWaitForFences);
    LOAD_D(vkResetFences);
    LOAD_D(vkResetCommandBuffer);
    LOAD_D(vkBeginCommandBuffer);
    LOAD_D(vkCmdBeginRenderPass);
    LOAD_D(vkCmdEndRenderPass);
    LOAD_D(vkCmdPipelineBarrier);
    LOAD_D(vkCmdCopyImageToBuffer);
    LOAD_D(vkEndCommandBuffer);
    LOAD_D(vkQueueSubmit);
    LOAD_D(vkQueuePresentKHR);
    LOAD_D(vkMapMemory);
    LOAD_D(vkUnmapMemory);
    LOAD_D(vkDeviceWaitIdle);
    LOAD_D(vkDestroySemaphore);
    LOAD_D(vkDestroyFence);
    LOAD_D(vkFreeMemory);
    LOAD_D(vkDestroyBuffer);
    LOAD_D(vkDestroyCommandPool);
    LOAD_D(vkDestroyFramebuffer);
    LOAD_D(vkDestroyImageView);
    LOAD_D(vkDestroyRenderPass);
    LOAD_D(vkDestroySwapchainKHR);
    LOAD_D(vkDestroyDevice);

    VkQueue queue;
    vkGetDeviceQueue(device, qf, 0, &queue);

    /* ---- swapchain ---- */
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fmt_count, NULL);
    VkSurfaceFormatKHR *fmts = (VkSurfaceFormatKHR *)alloca(fmt_count * sizeof(*fmts));
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fmt_count, fmts);
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < fmt_count; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            fmt = fmts[i];
            break;
        }
    }
    printf("Vulkan surface format: 0x%x  extent: %ux%u\n",
           fmt.format, caps.currentExtent.width, caps.currentExtent.height);

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    VkExtent2D extent = { (uint32_t)w, (uint32_t)h };
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
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
    VkSwapchainKHR swapchain;
    VK_CHECK(vkCreateSwapchainKHR(device, &sci, NULL, &swapchain));
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, NULL));
    VkImage *images = (VkImage *)alloca(image_count * sizeof(*images));
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, images));

    /* ---- render pass ---- */
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
        .colorAttachmentCount = 1, .pColorAttachments = &ref,
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub,
    };
    VkRenderPass render_pass;
    VK_CHECK(vkCreateRenderPass(device, &rpci, NULL, &render_pass));

    VkImageView *views = (VkImageView *)alloca(image_count * sizeof(*views));
    VkFramebuffer *fbs = (VkFramebuffer *)alloca(image_count * sizeof(*fbs));
    for (uint32_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo ivci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt.format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VK_CHECK(vkCreateImageView(device, &ivci, NULL, &views[i]));
        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = render_pass, .attachmentCount = 1, .pAttachments = &views[i],
            .width = extent.width, .height = extent.height, .layers = 1,
        };
        VK_CHECK(vkCreateFramebuffer(device, &fci, NULL, &fbs[i]));
    }

    /* ---- commands + readback ---- */
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qf, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    VkCommandPool pool;
    VK_CHECK(vkCreateCommandPool(device, &cpci, NULL, &pool));
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VK_CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));

    size_t rb_size = (size_t)extent.width * extent.height * 4;
    VkBuffer rb_buf;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = rb_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VK_CHECK(vkCreateBuffer(device, &bci, NULL, &rb_buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, rb_buf, &req);
    VkPhysicalDeviceMemoryProperties memp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memp);
    uint32_t mem_type = 0;
    for (uint32_t i = 0; i < memp.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mem_type = i;
            break;
        }
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = mem_type,
    };
    VkDeviceMemory rb_mem;
    VK_CHECK(vkAllocateMemory(device, &mai, NULL, &rb_mem));
    VK_CHECK(vkBindBufferMemory(device, rb_buf, rb_mem, 0));

    VkSemaphore sem_a, sem_b;
    VkSemaphoreCreateInfo semci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK_CHECK(vkCreateSemaphore(device, &semci, NULL, &sem_a));
    VK_CHECK(vkCreateSemaphore(device, &semci, NULL, &sem_b));
    VkFence fence;
    VkFenceCreateInfo fci2 = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    VK_CHECK(vkCreateFence(device, &fci2, NULL, &fence));

    /* ---- frame loop ---- */
    int frames = 0;
    Uint64 t0 = SDL_GetTicksNS();
    bool done = false;
    Uint32 last_r = 0, last_g = 0, last_b = 0;

    while (!done && frames < total_frames) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        uint32_t idx = 0;
        VkResult acq = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, sem_a, VK_NULL_HANDLE, &idx);
        if (acq == VK_SUBOPTIMAL_KHR || acq == VK_ERROR_OUT_OF_DATE_KHR) {
            continue;
        }
        VK_CHECK(acq);

        float t = (float)frames / (float)total_frames;
        VkClearValue clear = { .color.float32 = {
            0.5f + 0.5f * sinf(6.28f * t),
            0.5f + 0.5f * sinf(6.28f * t + 2.1f),
            0.5f + 0.5f * sinf(6.28f * t + 4.2f), 1.0f,
        } };

        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(device, 1, &fence));
        VK_CHECK(vkResetCommandBuffer(cmd, 0));

        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        VkRenderPassBeginInfo rbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = render_pass, .framebuffer = fbs[idx],
            .renderArea = { { 0, 0 }, extent },
            .clearValueCount = 1, .pClearValues = &clear,
        };
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cmd);

        VkImageMemoryBarrier to_transfer = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[idx],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                             1, &to_transfer);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { extent.width, extent.height, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               rb_buf, 1, &region);
        VkImageMemoryBarrier to_present = to_transfer;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        to_present.dstAccessMask = 0;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL,
                             1, &to_present);
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_a,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1, .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &sem_b,
        };
        VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));

        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_b,
            .swapchainCount = 1, .pSwapchains = &swapchain,
            .pImageIndices = &idx,
        };
        VkResult pres = vkQueuePresentKHR(queue, &pi);
        if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "present failed: %d\n", pres);
            break;
        }

        if (frames == total_frames - 1) {
            VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
            void *map = NULL;
            VK_CHECK(vkMapMemory(device, rb_mem, 0, 4, 0, &map));
            Uint8 *p = (Uint8 *)map;
            last_b = p[0];
            last_g = p[1];
            last_r = p[2];
            vkUnmapMemory(device, rb_mem);
        }
        frames++;
        if (!bench_frames) {
            SDL_Delay(8);
        }
    }
    VK_CHECK(vkDeviceWaitIdle(device));
    Uint64 dt = SDL_GetTicksNS() - t0;

    printf("presented %d frames in %.3f s => %.1f fps\n",
           frames, dt / 1e9, frames / (dt / 1e9));
    printf("readback center color (last frame): R=%u G=%u B=%u\n", last_r, last_g, last_b);

    float t = (float)(total_frames - 1) / (float)total_frames;
    Uint8 er = (Uint8)((0.5f + 0.5f * sinf(6.28f * t)) * 255);
    Uint8 eg = (Uint8)((0.5f + 0.5f * sinf(6.28f * t + 2.1f)) * 255);
    Uint8 eb = (Uint8)((0.5f + 0.5f * sinf(6.28f * t + 4.2f)) * 255);
    bool ok = abs((int)last_r - er) <= 2 && abs((int)last_g - eg) <= 2 &&
              abs((int)last_b - eb) <= 2;
    printf("expected:                            R=%u G=%u B=%u  => %s\n",
           er, eg, eb, ok ? "MATCH" : "MISMATCH");

    vkDestroySemaphore(device, sem_a, NULL);
    vkDestroySemaphore(device, sem_b, NULL);
    vkDestroyFence(device, fence, NULL);
    vkFreeMemory(device, rb_mem, NULL);
    vkDestroyBuffer(device, rb_buf, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    for (uint32_t i = 0; i < image_count; i++) {
        vkDestroyFramebuffer(device, fbs[i], NULL);
        vkDestroyImageView(device, views[i], NULL);
    }
    vkDestroyRenderPass(device, render_pass, NULL);
    vkDestroySwapchainKHR(device, swapchain, NULL);
    vkDestroyDevice(device, NULL);
    SDL_Vulkan_DestroySurface(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
}
