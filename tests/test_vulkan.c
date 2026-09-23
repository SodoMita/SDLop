/*
  SDLop test: Vulkan WSI via SDL_Vulkan_* (lavapipe on headless systems).
  Creates instance + device + swapchain, presents one cleared frame and
  verifies the color by read-back. Skips cleanly when no Wayland
  compositor or Vulkan loader is available.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <alloca.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);         \
            fprintf(stderr, "\n");                \
            failures++;                           \
            goto cleanup;                         \
        }                                         \
    } while (0)

int main(void)
{
    int failures = 0;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer rb_buf = VK_NULL_HANDLE;
    VkDeviceMemory rb_mem = VK_NULL_HANDLE;
    VkSemaphore sem_a = VK_NULL_HANDLE, sem_b = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    SDL_Window *w = NULL;

    if (!getenv("WAYLAND_DISPLAY")) {
        printf("test_vulkan: SKIP (WAYLAND_DISPLAY not set)\n");
        return 0;
    }
    setenv("SDLOP_DISABLE_RAW_INPUT", "1", 1);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* loader + extensions */
    CHECK(SDL_Vulkan_LoadLibrary(NULL), "load vulkan loader: %s", SDL_GetError());
    CHECK(SDL_Vulkan_GetVkGetInstanceProcAddr() != NULL, "vkGetInstanceProcAddr");
    Uint32 ext_count = 0;
    char const *const *exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    CHECK(ext_count == 2 && exts && exts[0], "instance extensions (%u)", ext_count);

    w = SDL_CreateWindow("vk test", 320, 240, SDL_WINDOW_VULKAN);
    CHECK(w != NULL, "create window: %s", SDL_GetError());
    CHECK(SDL_Vulkan_CreateSurface(w, instance, NULL, &surface) == false,
          "CreateSurface must fail before instance exists");

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = exts,
    };
    CHECK(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS, "vkCreateInstance");
    CHECK(SDL_Vulkan_CreateSurface(w, instance, NULL, &surface), "SDL_Vulkan_CreateSurface: %s", SDL_GetError());

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, NULL);
    CHECK(gpu_count > 0, "no physical devices");
    VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(instance, &gpu_count, &gpu);

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qf_count, NULL);
    VkQueueFamilyProperties *qfp = alloca(qf_count * sizeof(*qfp));
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qf_count, qfp);
    uint32_t qf = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; i++) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &ok);
        if ((qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) {
            qf = i;
            break;
        }
    }
    CHECK(qf != UINT32_MAX, "no graphics+present queue");
    CHECK(SDL_Vulkan_GetPresentationSupport(instance, qf), "presentation support");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
    CHECK(vkCreateDevice(gpu, &dci, NULL, &device) == VK_SUCCESS, "vkCreateDevice");
    VkQueue queue;
    vkGetDeviceQueue(device, qf, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);
    uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fc, NULL);
    VkSurfaceFormatKHR *fmts = alloca(fc * sizeof(*fmts));
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fc, fmts);
    VkSurfaceFormatKHR fmt = fmts[0];

    VkExtent2D extent = { 320, 240 };
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    }
    uint32_t nimg = caps.minImageCount + 1;
    if (caps.maxImageCount && nimg > caps.maxImageCount) {
        nimg = caps.maxImageCount;
    }
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = nimg,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    CHECK(vkCreateSwapchainKHR(device, &sci, NULL, &swapchain) == VK_SUCCESS, "swapchain");
    VkImage img;
    uint32_t idx = 0;

    VkAttachmentDescription att = { .format = fmt.format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub };
    CHECK(vkCreateRenderPass(device, &rpci, NULL, &render_pass) == VK_SUCCESS, "render pass");

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    CHECK(vkCreateCommandPool(device, &cpci, NULL, &pool) == VK_SUCCESS, "command pool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    CHECK(vkAllocateCommandBuffers(device, &cai, &cmd) == VK_SUCCESS, "command buffer");

    size_t rb_size = (size_t)extent.width * extent.height * 4;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = rb_size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    CHECK(vkCreateBuffer(device, &bci, NULL, &rb_buf) == VK_SUCCESS, "readback buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, rb_buf, &req);
    VkPhysicalDeviceMemoryProperties memp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memp);
    uint32_t mt = 0;
    for (uint32_t i = 0; i < memp.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) && (memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mt = i;
            break;
        }
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = mt };
    CHECK(vkAllocateMemory(device, &mai, NULL, &rb_mem) == VK_SUCCESS, "readback memory");
    vkBindBufferMemory(device, rb_buf, rb_mem, 0);

    VkSemaphoreCreateInfo semci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCreateSemaphore(device, &semci, NULL, &sem_a);
    vkCreateSemaphore(device, &semci, NULL, &sem_b);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    vkCreateFence(device, &fci, NULL, &fence);

    /* acquire, clear to magenta, read back, present */
    CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, sem_a, VK_NULL_HANDLE, &idx) == VK_SUCCESS, "acquire");
    uint32_t icount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &icount, NULL);
    VkImage *imgs = alloca(icount * sizeof(*imgs));
    vkGetSwapchainImagesKHR(device, swapchain, &icount, imgs);
    img = imgs[idx];

    VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt.format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CHECK(vkCreateImageView(device, &ivci, NULL, &view) == VK_SUCCESS, "image view");
    VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = render_pass,
        .attachmentCount = 1, .pAttachments = &view, .width = extent.width, .height = extent.height, .layers = 1 };
    CHECK(vkCreateFramebuffer(device, &fbci, NULL, &fb) == VK_SUCCESS, "framebuffer");

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);
    VkClearValue clear = { .color.float32 = { 1.0f, 0.0f, 1.0f, 1.0f } }; /* magenta */
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = render_pass,
        .framebuffer = fb, .renderArea = { { 0, 0 }, extent }, .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cmd);
    VkImageMemoryBarrier to_tr = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_tr);
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { extent.width, extent.height, 1 } };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_buf, 1, &region);
    vkEndCommandBuffer(cmd);

    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &fence);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_a,
        .pWaitDstStageMask = &stage, .commandBufferCount = 1, .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &sem_b };
    CHECK(vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS, "submit");
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

    void *map = NULL;
    vkMapMemory(device, rb_mem, 0, 4, 0, &map);
    Uint8 *p = (Uint8 *)map;
    Uint8 cb = p[0], cg = p[1], cr = p[2];
    vkUnmapMemory(device, rb_mem);
    printf("readback pixel: R=%u G=%u B=%u\n", cr, cg, cb);
    CHECK(cr > 250 && cb > 250 && cg < 8, "expected magenta (got R=%u G=%u B=%u)", cr, cg, cb);

    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &sem_b, .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &idx };
    VkResult pres = vkQueuePresentKHR(queue, &pi);
    CHECK(pres == VK_SUCCESS || pres == VK_SUBOPTIMAL_KHR, "present (%d)", pres);
    vkDeviceWaitIdle(device);

cleanup:
    if (device) {
        vkDeviceWaitIdle(device);
    }
    if (fence) {
        vkDestroyFence(device, fence, NULL);
    }
    if (sem_a) {
        vkDestroySemaphore(device, sem_a, NULL);
    }
    if (sem_b) {
        vkDestroySemaphore(device, sem_b, NULL);
    }
    if (fb) {
        vkDestroyFramebuffer(device, fb, NULL);
    }
    if (view) {
        vkDestroyImageView(device, view, NULL);
    }
    if (rb_mem) {
        vkFreeMemory(device, rb_mem, NULL);
    }
    if (rb_buf) {
        vkDestroyBuffer(device, rb_buf, NULL);
    }
    if (pool) {
        vkDestroyCommandPool(device, pool, NULL);
    }
    if (render_pass) {
        vkDestroyRenderPass(device, render_pass, NULL);
    }
    if (swapchain) {
        vkDestroySwapchainKHR(device, swapchain, NULL);
    }
    if (device) {
        vkDestroyDevice(device, NULL);
    }
    if (surface && instance) {
        SDL_Vulkan_DestroySurface(instance, surface, NULL);
    }
    if (instance) {
        vkDestroyInstance(instance, NULL);
    }
    SDL_Vulkan_UnloadLibrary();
    if (w) {
        SDL_DestroyWindow(w);
    }
    SDL_Quit();

    if (failures) {
        printf("test_vulkan: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_vulkan: PASS\n");
    return 0;
}
