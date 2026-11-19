/*
  SDLop - Platform-agnostic headless GPU Vulkan offload.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_vk_offload.h"

#ifdef SDLop_VIDEO_WAYLAND
#include "wayland/sdlop_wayland_internal.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ */
/* Minimal Vulkan definitions (no vulkan.h required)                   */
/* ------------------------------------------------------------------ */

#define VK_SUCCESS 0
#define VK_TRUE 1u
#define VK_FALSE 0u
#define VK_MAX_EXTENSION_NAME_SIZE 256
#define VK_API_VERSION_1_0 (1u << 22)
#define VK_QUEUE_FAMILY_IGNORED (~0u)

#define VK_PHYSICAL_DEVICE_TYPE_OTHER 0
#define VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU 1
#define VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU 2
#define VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU 3
#define VK_PHYSICAL_DEVICE_TYPE_CPU 4

#define VK_FORMAT_R8G8B8A8_UNORM 37
#define VK_FORMAT_R8G8B8A8_SRGB 43
#define VK_FORMAT_B8G8R8A8_UNORM 44
#define VK_FORMAT_B8G8R8A8_SRGB 50
#define VK_COLOR_SPACE_SRGB_NONLINEAR_KHR 0

#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT 0x00000001u
#define VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL 6u
#define VK_IMAGE_LAYOUT_PRESENT_SRC_KHR 1000001002u

#define VK_IMAGE_ASPECT_COLOR_BIT 0x00000001u
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT 0x00000002u
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT 0x00000002u
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 0x00000004u

#define VK_ACCESS_TRANSFER_READ_BIT 0x00000800u
#define VK_ACCESS_MEMORY_READ_BIT 0x00008000u
#define VK_PIPELINE_STAGE_TRANSFER_BIT 0x00001000u
#define VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT 0x00002000u

#define VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT 0x00000002u
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY 0u
#define VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT 0x00000001u
#define VK_SHARING_MODE_EXCLUSIVE 0u

#define VK_STRUCTURE_TYPE_APPLICATION_INFO 0u
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1u
#define VK_STRUCTURE_TYPE_SUBMIT_INFO 4u
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO 5u
#define VK_STRUCTURE_TYPE_FENCE_CREATE_INFO 8u
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO 12u
#define VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO 39u
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO 40u
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO 42u
#define VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER 45u
#define VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT 1000256000u

typedef int32_t VkResult_;
typedef uint32_t VkFlags_;
typedef uint32_t VkBool32_;
typedef uint64_t VkDeviceSize_;
typedef void *VkInstance_;
typedef void *VkPhysicalDevice_;
typedef void *VkDevice_;
typedef void *VkQueue_;
typedef void *VkCommandBuffer_;
typedef uint64_t VkSurfaceKHR_;
typedef uint64_t VkSwapchainKHR_;
typedef uint64_t VkImage_;
typedef uint64_t VkBuffer_;
typedef uint64_t VkDeviceMemory_;
typedef uint64_t VkCommandPool_;
typedef uint64_t VkFence_;
typedef uint64_t VkSemaphore_;

typedef struct VkExtent2D_ {
    uint32_t width;
    uint32_t height;
} VkExtent2D_;

typedef struct VkExtent3D_ {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} VkExtent3D_;

typedef struct VkOffset3D_ {
    int32_t x;
    int32_t y;
    int32_t z;
} VkOffset3D_;

typedef struct VkExtensionProperties_ {
    char extensionName[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
} VkExtensionProperties_;

typedef union VkPhysicalDevicePropertiesBuf_ {
    uint64_t alignment;
    unsigned char bytes[4096];
} VkPhysicalDevicePropertiesBuf_;

typedef struct VkApplicationInfo_ {
    uint32_t sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} VkApplicationInfo_;

typedef struct VkInstanceCreateInfo_ {
    uint32_t sType;
    const void *pNext;
    VkFlags_ flags;
    const VkApplicationInfo_ *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo_;

typedef struct VkDeviceQueueCreateInfo_ {
    uint32_t sType;
    const void *pNext;
    VkFlags_ flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float *pQueuePriorities;
} VkDeviceQueueCreateInfo_;

typedef struct VkDeviceCreateInfo_ {
    uint32_t sType;
    const void *pNext;
    VkFlags_ flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo_ *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} VkDeviceCreateInfo_;

typedef struct VkHeadlessSurfaceCreateInfoEXT_ {
    uint32_t sType;
    const void *pNext;
    VkFlags_ flags;
} VkHeadlessSurfaceCreateInfoEXT_;

typedef struct VkSurfaceCapabilitiesKHR_ {
    uint32_t minImageCount;
    uint32_t maxImageCount;
    VkExtent2D_ currentExtent;
    VkExtent2D_ minImageExtent;
    VkExtent2D_ maxImageExtent;
    uint32_t maxImageArrayLayers;
    VkFlags_ supportedTransforms;
    uint32_t currentTransform;
    VkFlags_ supportedCompositeAlpha;
    VkFlags_ supportedUsageFlags;
} VkSurfaceCapabilitiesKHR_;

typedef struct VkSurfaceFormatKHR_ {
    uint32_t format;
    uint32_t colorSpace;
} VkSurfaceFormatKHR_;

typedef struct VkSwapchainCreateInfoKHR_ {
    uint32_t sType;
    const void *pNext;
    VkFlags_ flags;
    VkSurfaceKHR_ surface;
    uint32_t minImageCount;
    uint32_t imageFormat;
    uint32_t imageColorSpace;
    VkExtent2D_ imageExtent;
    uint32_t imageArrayLayers;
    VkFlags_ imageUsage;
    uint32_t imageSharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    uint32_t preTransform;
    uint32_t compositeAlpha;
    uint32_t presentMode;
    VkBool32_ clipped;
    VkSwapchainKHR_ oldSwapchain;
} VkSwapchainCreateInfoKHR_;

typedef struct VkPresentInfoKHR_ {
    uint32_t sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore_ *pWaitSemaphores;
    uint32_t swapchainCount;
    const VkSwapchainKHR_ *pSwapchains;
    const uint32_t *pImageIndices;
    VkResult_ *pResults;
} VkPresentInfoKHR_;

typedef struct VkPhysicalDeviceMemoryProperties_ {
    uint32_t memoryTypeCount;
    struct {
        VkFlags_ propertyFlags;
        uint32_t heapIndex;
    } memoryTypes[32];
    uint32_t memoryHeapCount;
    struct {
        VkDeviceSize_ size;
        VkFlags_ flags;
    } memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties_;

typedef struct VkMemoryRequirements_ {
    VkDeviceSize_ size;
    VkDeviceSize_ alignment;
    uint32_t memoryTypeBits;
} VkMemoryRequirements_;

typedef struct VkBufferCreateInfo_ {
    uint32_t sType;
    const void *pNext;
    VkFlags_ flags;
    VkDeviceSize_ size;
    VkFlags_ usage;
    uint32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
} VkBufferCreateInfo_;

typedef struct VkMemoryAllocateInfo_ {
    uint32_t sType;
    const void *pNext;
    VkDeviceSize_ allocationSize;
    uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo_;

typedef struct VkCommandPoolCreateInfo_ {
    uint32_t sType;
    const void *pNext;
    VkFlags_ flags;
    uint32_t queueFamilyIndex;
} VkCommandPoolCreateInfo_;

typedef struct VkCommandBufferAllocateInfo_ {
    uint32_t sType;
    const void *pNext;
    VkCommandPool_ commandPool;
    uint32_t level;
    uint32_t commandBufferCount;
} VkCommandBufferAllocateInfo_;

typedef struct VkCommandBufferBeginInfo_ {
    uint32_t sType;
    const void *pNext;
    VkFlags_ flags;
    const void *pInheritanceInfo;
} VkCommandBufferBeginInfo_;

typedef struct VkImageSubresourceRange_ {
    VkFlags_ aspectMask;
    uint32_t baseMipLevel;
    uint32_t levelCount;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
} VkImageSubresourceRange_;

typedef struct VkImageMemoryBarrier_ {
    uint32_t sType;
    const void *pNext;
    VkFlags_ srcAccessMask;
    VkFlags_ dstAccessMask;
    uint32_t oldLayout;
    uint32_t newLayout;
    uint32_t srcQueueFamilyIndex;
    uint32_t dstQueueFamilyIndex;
    VkImage_ image;
    VkImageSubresourceRange_ subresourceRange;
} VkImageMemoryBarrier_;

typedef struct VkImageSubresourceLayers_ {
    VkFlags_ aspectMask;
    uint32_t mipLevel;
    uint32_t baseArrayLayer;
    uint32_t layerCount;
} VkImageSubresourceLayers_;

typedef struct VkBufferImageCopy_ {
    VkDeviceSize_ bufferOffset;
    uint32_t bufferRowLength;
    uint32_t bufferImageHeight;
    VkImageSubresourceLayers_ imageSubresource;
    VkOffset3D_ imageOffset;
    VkExtent3D_ imageExtent;
} VkBufferImageCopy_;

typedef struct VkSubmitInfo_ {
    uint32_t sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const VkSemaphore_ *pWaitSemaphores;
    const VkFlags_ *pWaitDstStageMask;
    uint32_t commandBufferCount;
    const VkCommandBuffer_ *pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const VkSemaphore_ *pSignalSemaphores;
} VkSubmitInfo_;

typedef struct VkFenceCreateInfo_ {
    uint32_t sType;
    const void *pNext;
    VkFlags_ flags;
} VkFenceCreateInfo_;

typedef struct VkDisplayPropertiesKHR_ {
    void *display;
    const char *displayName;
    VkExtent2D_ physicalDimensions;
    VkExtent2D_ physicalResolution;
    VkFlags_ supportedTransforms;
    VkBool32_ planeReorderPossible;
    VkBool32_ persistentContent;
} VkDisplayPropertiesKHR_;

typedef void (*PFN_vkVoid_)(void);
#define VK_CAST(T, fn) ((T)(uintptr_t)(fn))
#define VK_VOID(fn) ((PFN_vkVoid_)(uintptr_t)(fn))
typedef PFN_vkVoid_ (*PFN_vkGetInstanceProcAddr_)(VkInstance_, const char *);
typedef PFN_vkVoid_ (*PFN_vkGetDeviceProcAddr_)(VkDevice_, const char *);
typedef VkResult_ (*PFN_vkEnumerateInstanceExtensionProperties_)(const char *, uint32_t *, VkExtensionProperties_ *);
typedef VkResult_ (*PFN_vkCreateInstance_)(const VkInstanceCreateInfo_ *, const void *, VkInstance_ *);
typedef void (*PFN_vkDestroyInstance_)(VkInstance_, const void *);
typedef VkResult_ (*PFN_vkEnumeratePhysicalDevices_)(VkInstance_, uint32_t *, VkPhysicalDevice_ *);
typedef void (*PFN_vkGetPhysicalDeviceProperties_)(VkPhysicalDevice_, VkPhysicalDevicePropertiesBuf_ *);
typedef void (*PFN_vkGetPhysicalDeviceMemoryProperties_)(VkPhysicalDevice_, VkPhysicalDeviceMemoryProperties_ *);
typedef VkResult_ (*PFN_vkGetPhysicalDeviceDisplayPropertiesKHR_)(VkPhysicalDevice_, uint32_t *, VkDisplayPropertiesKHR_ *);
typedef VkResult_ (*PFN_vkGetPhysicalDeviceSurfaceSupportKHR_)(VkPhysicalDevice_, uint32_t, VkSurfaceKHR_, VkBool32_ *);
typedef VkResult_ (*PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR_)(VkPhysicalDevice_, VkSurfaceKHR_, VkSurfaceCapabilitiesKHR_ *);
typedef VkResult_ (*PFN_vkGetPhysicalDeviceSurfaceFormatsKHR_)(VkPhysicalDevice_, VkSurfaceKHR_, uint32_t *, VkSurfaceFormatKHR_ *);
typedef VkResult_ (*PFN_vkCreateHeadlessSurfaceEXT_)(VkInstance_, const VkHeadlessSurfaceCreateInfoEXT_ *, const void *, VkSurfaceKHR_ *);
typedef VkResult_ (*PFN_vkCreateDevice_)(VkPhysicalDevice_, const VkDeviceCreateInfo_ *, const void *, VkDevice_ *);
typedef void (*PFN_vkDestroyDevice_)(VkDevice_, const void *);
typedef VkResult_ (*PFN_vkCreateSwapchainKHR_)(VkDevice_, const VkSwapchainCreateInfoKHR_ *, const void *, VkSwapchainKHR_ *);
typedef void (*PFN_vkDestroySwapchainKHR_)(VkDevice_, VkSwapchainKHR_, const void *);
typedef VkResult_ (*PFN_vkGetSwapchainImagesKHR_)(VkDevice_, VkSwapchainKHR_, uint32_t *, VkImage_ *);
typedef VkResult_ (*PFN_vkQueuePresentKHR_)(VkQueue_, const VkPresentInfoKHR_ *);
typedef VkResult_ (*PFN_vkCreateBuffer_)(VkDevice_, const VkBufferCreateInfo_ *, const void *, VkBuffer_ *);
typedef void (*PFN_vkDestroyBuffer_)(VkDevice_, VkBuffer_, const void *);
typedef void (*PFN_vkGetBufferMemoryRequirements_)(VkDevice_, VkBuffer_, VkMemoryRequirements_ *);
typedef VkResult_ (*PFN_vkAllocateMemory_)(VkDevice_, const VkMemoryAllocateInfo_ *, const void *, VkDeviceMemory_ *);
typedef void (*PFN_vkFreeMemory_)(VkDevice_, VkDeviceMemory_, const void *);
typedef VkResult_ (*PFN_vkBindBufferMemory_)(VkDevice_, VkBuffer_, VkDeviceMemory_, VkDeviceSize_);
typedef VkResult_ (*PFN_vkMapMemory_)(VkDevice_, VkDeviceMemory_, VkDeviceSize_, VkDeviceSize_, VkFlags_, void **);
typedef void (*PFN_vkUnmapMemory_)(VkDevice_, VkDeviceMemory_);
typedef VkResult_ (*PFN_vkCreateCommandPool_)(VkDevice_, const VkCommandPoolCreateInfo_ *, const void *, VkCommandPool_ *);
typedef void (*PFN_vkDestroyCommandPool_)(VkDevice_, VkCommandPool_, const void *);
typedef VkResult_ (*PFN_vkAllocateCommandBuffers_)(VkDevice_, const VkCommandBufferAllocateInfo_ *, VkCommandBuffer_ *);
typedef VkResult_ (*PFN_vkResetCommandBuffer_)(VkCommandBuffer_, VkFlags_);
typedef VkResult_ (*PFN_vkBeginCommandBuffer_)(VkCommandBuffer_, const VkCommandBufferBeginInfo_ *);
typedef VkResult_ (*PFN_vkEndCommandBuffer_)(VkCommandBuffer_);
typedef void (*PFN_vkCmdPipelineBarrier_)(VkCommandBuffer_, VkFlags_, VkFlags_, VkFlags_, uint32_t, const void *, uint32_t, const void *, uint32_t, const VkImageMemoryBarrier_ *);
typedef void (*PFN_vkCmdCopyImageToBuffer_)(VkCommandBuffer_, VkImage_, uint32_t, VkBuffer_, uint32_t, const VkBufferImageCopy_ *);
typedef VkResult_ (*PFN_vkCreateFence_)(VkDevice_, const VkFenceCreateInfo_ *, const void *, VkFence_ *);
typedef void (*PFN_vkDestroyFence_)(VkDevice_, VkFence_, const void *);
typedef VkResult_ (*PFN_vkResetFences_)(VkDevice_, uint32_t, const VkFence_ *);
typedef VkResult_ (*PFN_vkWaitForFences_)(VkDevice_, uint32_t, const VkFence_ *, VkBool32_, uint64_t);
typedef VkResult_ (*PFN_vkQueueSubmit_)(VkQueue_, uint32_t, const VkSubmitInfo_ *, VkFence_);

/* ------------------------------------------------------------------ */
/* Tracking tables & state                                             */
/* ------------------------------------------------------------------ */

typedef struct TrackedSurface
{
    VkSurfaceKHR_ surface;
    VkInstance_ instance;
    SDL_Window *window;
    struct TrackedSurface *next;
} TrackedSurface;

typedef struct TrackedPhysicalDevice
{
    VkPhysicalDevice_ gpu;
    VkInstance_ instance;
    struct TrackedPhysicalDevice *next;
} TrackedPhysicalDevice;

typedef struct TrackedDevice
{
    VkDevice_ device;
    VkInstance_ instance;
    VkPhysicalDevice_ gpu;
    uint32_t queue_family;

    PFN_vkGetDeviceProcAddr_ real_gdpa;
    PFN_vkDestroyDevice_ real_destroy_device;
    PFN_vkCreateSwapchainKHR_ real_create_swapchain;
    PFN_vkDestroySwapchainKHR_ real_destroy_swapchain;
    PFN_vkGetSwapchainImagesKHR_ real_get_swapchain_images;
    PFN_vkQueuePresentKHR_ real_queue_present;

    PFN_vkCreateBuffer_ p_create_buffer;
    PFN_vkDestroyBuffer_ p_destroy_buffer;
    PFN_vkGetBufferMemoryRequirements_ p_get_buf_mem_req;
    PFN_vkAllocateMemory_ p_alloc_mem;
    PFN_vkFreeMemory_ p_free_mem;
    PFN_vkBindBufferMemory_ p_bind_buf_mem;
    PFN_vkMapMemory_ p_map_mem;
    PFN_vkUnmapMemory_ p_unmap_mem;
    PFN_vkCreateCommandPool_ p_create_cmd_pool;
    PFN_vkDestroyCommandPool_ p_destroy_cmd_pool;
    PFN_vkAllocateCommandBuffers_ p_alloc_cmd_bufs;
    PFN_vkResetCommandBuffer_ p_reset_cmd_buf;
    PFN_vkBeginCommandBuffer_ p_begin_cmd_buf;
    PFN_vkEndCommandBuffer_ p_end_cmd_buf;
    PFN_vkCmdPipelineBarrier_ p_cmd_barrier;
    PFN_vkCmdCopyImageToBuffer_ p_cmd_copy_img_to_buf;
    PFN_vkCreateFence_ p_create_fence;
    PFN_vkDestroyFence_ p_destroy_fence;
    PFN_vkResetFences_ p_reset_fences;
    PFN_vkWaitForFences_ p_wait_fences;
    PFN_vkQueueSubmit_ p_queue_submit;

    struct TrackedDevice *next;
} TrackedDevice;

typedef struct TrackedSwapchain
{
    VkSwapchainKHR_ swapchain;
    TrackedDevice *dev;
    SDL_Window *window;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    VkImage_ *images;
    uint32_t image_count;

    /* Lazy readback staging state */
    bool staging_init;
    bool staging_failed;
    VkBuffer_ staging_buf;
    VkDeviceMemory_ staging_mem;
    VkDeviceSize_ staging_size;
    void *staging_mapped;
    VkCommandPool_ cmd_pool;
    VkCommandBuffer_ cmd_buf;
    VkFence_ fence;

    struct TrackedSwapchain *next;
} TrackedSwapchain;

static struct
{
    PFN_vkGetInstanceProcAddr_ real_gipa;
    VkInstance_ instance;
    bool active;
    bool probed;
    SDLOP_VulkanOffloadPolicy cached_policy;
    const SDLop_VideoDevice *cached_video;
    bool cached_decision;

    TrackedSurface *surfaces;
    TrackedPhysicalDevice *gpus;
    TrackedDevice *devices;
    TrackedSwapchain *swapchains;
} vk_offload;

static void track_gpu_instance(VkPhysicalDevice_ gpu, VkInstance_ instance)
{
    if (!gpu || !instance) {
        return;
    }
    for (TrackedPhysicalDevice *g = vk_offload.gpus; g; g = g->next) {
        if (g->gpu == gpu) {
            g->instance = instance;
            return;
        }
    }
    TrackedPhysicalDevice *g = (TrackedPhysicalDevice *)calloc(1, sizeof(*g));
    if (g) {
        g->gpu = gpu;
        g->instance = instance;
        g->next = vk_offload.gpus;
        vk_offload.gpus = g;
    }
}

static VkInstance_ resolve_instance_for_gpu(VkPhysicalDevice_ gpu)
{
    for (TrackedPhysicalDevice *g = vk_offload.gpus; g; g = g->next) {
        if (g->gpu == gpu && g->instance) {
            return g->instance;
        }
    }
    return vk_offload.instance;
}

static void untrack_gpus_for_instance(VkInstance_ instance)
{
    TrackedPhysicalDevice **pp = &vk_offload.gpus;
    while (*pp) {
        TrackedPhysicalDevice *g = *pp;
        if (g->instance == instance) {
            *pp = g->next;
            free(g);
        } else {
            pp = &g->next;
        }
    }
}

static const char *const vk_offload_extensions[] = {
    "VK_KHR_surface",
    "VK_EXT_headless_surface",
    NULL
};

/* ------------------------------------------------------------------ */
/* Policy & extension helpers                                          */
/* ------------------------------------------------------------------ */

SDLOP_VulkanOffloadPolicy SDLOP_VulkanOffload_GetPolicy(void)
{
    const char *v = getenv("SDLOP_VULKAN_OFFLOAD");
    if (!v || !*v) {
        v = getenv("SDLOP_VK_OFFLOAD");
    }
    if (!v || !*v) {
        v = getenv("SDLOP_OFFLOAD");
    }
    if (!v || !*v || strcasecmp(v, "auto") == 0) {
        return SDLOP_VK_OFFLOAD_AUTO;
    }
    if (strcmp(v, "0") == 0 || strcasecmp(v, "off") == 0 ||
        strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0) {
        return SDLOP_VK_OFFLOAD_NEVER;
    }
    if (strcmp(v, "1") == 0 || strcasecmp(v, "on") == 0 ||
        strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 ||
        strcasecmp(v, "force") == 0) {
        return SDLOP_VK_OFFLOAD_FORCE;
    }
    return SDLOP_VK_OFFLOAD_AUTO;
}

static bool has_instance_extension(PFN_vkGetInstanceProcAddr_ gipa,
                                   const char *name)
{
    if (!gipa || !name) {
        return false;
    }
    PFN_vkEnumerateInstanceExtensionProperties_ p_enum =
        VK_CAST(PFN_vkEnumerateInstanceExtensionProperties_, gipa(NULL, "vkEnumerateInstanceExtensionProperties"));
    if (!p_enum) {
        return false;
    }
    uint32_t count = 0;
    if (p_enum(NULL, &count, NULL) != VK_SUCCESS || count == 0) {
        return false;
    }
    VkExtensionProperties_ *props = (VkExtensionProperties_ *)calloc(count, sizeof(*props));
    if (!props) {
        return false;
    }
    bool found = false;
    if (p_enum(NULL, &count, props) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(props[i].extensionName, name) == 0) {
                found = true;
                break;
            }
        }
    }
    free(props);
    return found;
}

static bool is_hardware_device_type(uint32_t device_type)
{
    return device_type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
           device_type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
           device_type == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU;
}

static uint32_t query_gpu_device_type(PFN_vkGetInstanceProcAddr_ gipa,
                                      VkInstance_ instance,
                                      VkPhysicalDevice_ gpu)
{
    PFN_vkGetPhysicalDeviceProperties_ p_props =
        VK_CAST(PFN_vkGetPhysicalDeviceProperties_, gipa(instance, "vkGetPhysicalDeviceProperties"));
    if (!p_props) {
        return VK_PHYSICAL_DEVICE_TYPE_OTHER;
    }
    VkPhysicalDevicePropertiesBuf_ buf;
    memset(&buf, 0, sizeof(buf));
    p_props(gpu, &buf);
    uint32_t api_version = 0, device_type = 0;
    memcpy(&api_version, buf.bytes + 0, sizeof(api_version));
    memcpy(&device_type, buf.bytes + 16, sizeof(device_type));
    if (api_version < VK_API_VERSION_1_0) {
        return VK_PHYSICAL_DEVICE_TYPE_OTHER;
    }
    return device_type;
}

static void probe_vulkan_devices(PFN_vkGetInstanceProcAddr_ gipa,
                                 bool check_kms_displays,
                                 bool *has_hw_gpu_out,
                                 bool *has_kms_display_out)
{
    *has_hw_gpu_out = false;
    *has_kms_display_out = false;

    PFN_vkCreateInstance_ p_create = VK_CAST(PFN_vkCreateInstance_, gipa(NULL, "vkCreateInstance"));
    PFN_vkDestroyInstance_ p_destroy = VK_CAST(PFN_vkDestroyInstance_, gipa(NULL, "vkDestroyInstance"));
    if (!p_create || !p_destroy) {
        return;
    }

    const char *exts[2] = { "VK_KHR_surface", "VK_KHR_display" };
    uint32_t ext_count = 0;
    if (check_kms_displays &&
        has_instance_extension(gipa, "VK_KHR_surface") &&
        has_instance_extension(gipa, "VK_KHR_display")) {
        ext_count = 2;
    }

    VkApplicationInfo_ app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "SDLop-probe";
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo_ ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = ext_count;
    ici.ppEnabledExtensionNames = ext_count ? exts : NULL;

    VkInstance_ inst = NULL;
    if (p_create(&ici, NULL, &inst) != VK_SUCCESS || !inst) {
        if (ext_count > 0) {
            ici.enabledExtensionCount = 0;
            ici.ppEnabledExtensionNames = NULL;
            if (p_create(&ici, NULL, &inst) != VK_SUCCESS || !inst) {
                return;
            }
            ext_count = 0;
        } else {
            return;
        }
    }

    PFN_vkEnumeratePhysicalDevices_ p_enum =
        VK_CAST(PFN_vkEnumeratePhysicalDevices_, gipa(inst, "vkEnumeratePhysicalDevices"));
    PFN_vkGetPhysicalDeviceDisplayPropertiesKHR_ p_disp = ext_count ?
        VK_CAST(PFN_vkGetPhysicalDeviceDisplayPropertiesKHR_, gipa(inst, "vkGetPhysicalDeviceDisplayPropertiesKHR")) : NULL;

    if (p_enum) {
        uint32_t count = 0;
        if (p_enum(inst, &count, NULL) == VK_SUCCESS && count > 0) {
            VkPhysicalDevice_ *gpus = (VkPhysicalDevice_ *)calloc(count, sizeof(*gpus));
            if (gpus && p_enum(inst, &count, gpus) == VK_SUCCESS) {
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t dtype = query_gpu_device_type(gipa, inst, gpus[i]);
                    if (is_hardware_device_type(dtype)) {
                        *has_hw_gpu_out = true;
                    }
                    if (p_disp) {
                        uint32_t dcount = 0;
                        if (p_disp(gpus[i], &dcount, NULL) == VK_SUCCESS && dcount > 0) {
                            *has_kms_display_out = true;
                        }
                    }
                }
            }
            free(gpus);
        }
    }

    p_destroy(inst, NULL);
}

bool SDLOP_VulkanOffload_ShouldUseOffload(SDL_FunctionPointer real_gipa)
{
    if (real_gipa) {
        vk_offload.real_gipa = (PFN_vkGetInstanceProcAddr_)real_gipa;
    }
    PFN_vkGetInstanceProcAddr_ gipa = vk_offload.real_gipa;
    SDLOP_VulkanOffloadPolicy policy = SDLOP_VulkanOffload_GetPolicy();

    if (vk_offload.probed &&
        vk_offload.cached_policy == policy &&
        vk_offload.cached_video == sdlop.video) {
        vk_offload.active = vk_offload.cached_decision;
        return vk_offload.cached_decision;
    }

    vk_offload.probed = true;
    vk_offload.cached_policy = policy;
    vk_offload.cached_video = sdlop.video;
    vk_offload.cached_decision = false;

    if (!gipa || policy == SDLOP_VK_OFFLOAD_NEVER) {
        vk_offload.active = false;
        return false;
    }

    if (!has_instance_extension(gipa, "VK_KHR_surface") ||
        !has_instance_extension(gipa, "VK_EXT_headless_surface")) {
        vk_offload.active = false;
        return false;
    }

    if (policy == SDLOP_VK_OFFLOAD_FORCE) {
        vk_offload.cached_decision = true;
        vk_offload.active = true;
        return true;
    }

    /* SDLOP_VK_OFFLOAD_AUTO */
    const char *drv = sdlop.video ? sdlop.video->name : "";
    bool is_kmsdrm = (strcmp(drv, "kmsdrm") == 0);
    bool is_wayland = (strcmp(drv, "wayland") == 0);

    if (sdlop.video && sdlop.video->Vulkan_CreateSurface && !is_kmsdrm && !is_wayland) {
        /* Custom/future driver with native Vulkan WSI: prefer native WSI. */
        vk_offload.active = false;
        return false;
    }

    bool has_hw_gpu = false;
    bool has_kms_display = false;
    probe_vulkan_devices(gipa, is_kmsdrm, &has_hw_gpu, &has_kms_display);

    bool decision = false;
    if (!sdlop.video || !sdlop.video->Vulkan_CreateSurface) {
        /* Driver without native Vulkan WSI (e.g. dummy, framebuffer-only drivers). */
        decision = has_hw_gpu;
    } else if (is_wayland) {
#ifdef SDLop_VIDEO_WAYLAND
        decision = has_hw_gpu && !SDLOP_Wayland_CompositorHasHardwareBuffers();
#else
        decision = has_hw_gpu;
#endif
    } else if (is_kmsdrm) {
        decision = has_hw_gpu && !has_kms_display;
    }

    vk_offload.cached_decision = decision;
    vk_offload.active = decision;
    return decision;
}

bool SDLOP_VulkanOffload_IsActive(void)
{
    return vk_offload.active;
}

char const *const *SDLOP_VulkanOffload_GetInstanceExtensions(Uint32 *count)
{
    if (count) {
        *count = 2;
    }
    return vk_offload_extensions;
}

/* ------------------------------------------------------------------ */
/* Tracking helpers                                                    */
/* ------------------------------------------------------------------ */

static TrackedSurface *find_surface(VkSurfaceKHR_ s)
{
    for (TrackedSurface *ts = vk_offload.surfaces; ts; ts = ts->next) {
        if (ts->surface == s) {
            return ts;
        }
    }
    return NULL;
}

static TrackedDevice *find_device(VkDevice_ d)
{
    for (TrackedDevice *td = vk_offload.devices; td; td = td->next) {
        if (td->device == d) {
            return td;
        }
    }
    return NULL;
}

static TrackedSwapchain *find_swapchain(VkSwapchainKHR_ sc)
{
    for (TrackedSwapchain *ts = vk_offload.swapchains; ts; ts = ts->next) {
        if (ts->swapchain == sc) {
            return ts;
        }
    }
    return NULL;
}

static void destroy_swapchain_staging(TrackedSwapchain *ts)
{
    if (!ts || !ts->dev) {
        return;
    }
    TrackedDevice *dev = ts->dev;
    if (ts->staging_mapped && dev->p_unmap_mem && ts->staging_mem) {
        dev->p_unmap_mem(dev->device, ts->staging_mem);
        ts->staging_mapped = NULL;
    }
    if (ts->fence && dev->p_destroy_fence) {
        dev->p_destroy_fence(dev->device, ts->fence, NULL);
        ts->fence = 0;
    }
    if (ts->cmd_pool && dev->p_destroy_cmd_pool) {
        dev->p_destroy_cmd_pool(dev->device, ts->cmd_pool, NULL);
        ts->cmd_pool = 0;
        ts->cmd_buf = NULL;
    }
    if (ts->staging_buf && dev->p_destroy_buffer) {
        dev->p_destroy_buffer(dev->device, ts->staging_buf, NULL);
        ts->staging_buf = 0;
    }
    if (ts->staging_mem && dev->p_free_mem) {
        dev->p_free_mem(dev->device, ts->staging_mem, NULL);
        ts->staging_mem = 0;
    }
    ts->staging_size = 0;
    ts->staging_init = false;
}

static void free_tracked_swapchain(TrackedSwapchain *ts)
{
    if (!ts) {
        return;
    }
    destroy_swapchain_staging(ts);
    free(ts->images);
    free(ts);
}

bool SDLOP_VulkanOffload_CreateSurface(SDL_FunctionPointer real_gipa,
                                       SDL_Window *window,
                                       void *instance,
                                       const void *allocator,
                                       Uint64 *surface)
{
    if (!window || !instance || !surface) {
        return SDL_SetError("Invalid arguments for Vulkan headless surface");
    }
    if (real_gipa) {
        vk_offload.real_gipa = (PFN_vkGetInstanceProcAddr_)real_gipa;
    }
    PFN_vkGetInstanceProcAddr_ gipa = vk_offload.real_gipa;
    if (!gipa) {
        return SDL_SetError("Vulkan loader not loaded");
    }

    PFN_vkCreateHeadlessSurfaceEXT_ p_create =
        VK_CAST(PFN_vkCreateHeadlessSurfaceEXT_, gipa((VkInstance_)instance, "vkCreateHeadlessSurfaceEXT"));
    if (!p_create) {
        return SDL_SetError("vkCreateHeadlessSurfaceEXT not available (missing VK_EXT_headless_surface?)");
    }

    VkHeadlessSurfaceCreateInfoEXT_ ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;

    VkSurfaceKHR_ s = 0;
    VkResult_ res = p_create((VkInstance_)instance, &ci, allocator, &s);
    if (res != VK_SUCCESS || !s) {
        return SDL_SetError("vkCreateHeadlessSurfaceEXT failed: %d", (int)res);
    }

    TrackedSurface *ts = (TrackedSurface *)calloc(1, sizeof(*ts));
    if (ts) {
        ts->surface = s;
        ts->instance = (VkInstance_)instance;
        ts->window = window;
        ts->next = vk_offload.surfaces;
        vk_offload.surfaces = ts;
    }

    vk_offload.active = true;
    vk_offload.instance = (VkInstance_)instance;
    *surface = (Uint64)s;
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "SDLop: Vulkan offload active on %s via VK_EXT_headless_surface",
                sdlop.video ? sdlop.video->name : "?");
    return true;
}

void SDLOP_VulkanOffload_NotifyDestroySurface(Uint64 surface)
{
    TrackedSurface **pp = &vk_offload.surfaces;
    while (*pp) {
        TrackedSurface *ts = *pp;
        if (ts->surface == (VkSurfaceKHR_)surface) {
            *pp = ts->next;
            free(ts);
            return;
        }
        pp = &ts->next;
    }
}

void SDLOP_VulkanOffload_NotifyWindowDestroyed(SDL_Window *window)
{
    if (!window) {
        return;
    }
    for (TrackedSurface *ts = vk_offload.surfaces; ts; ts = ts->next) {
        if (ts->window == window) {
            ts->window = NULL;
        }
    }
    for (TrackedSwapchain *sc = vk_offload.swapchains; sc; sc = sc->next) {
        if (sc->window == window) {
            sc->window = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Staging readback into software framebuffer                          */
/* ------------------------------------------------------------------ */

static int find_host_mem_type(TrackedDevice *dev, uint32_t type_bits)
{
    VkInstance_ inst = dev->instance ? dev->instance : resolve_instance_for_gpu(dev->gpu);
    if (!vk_offload.real_gipa || !dev->gpu || !inst) {
        return -1;
    }
    PFN_vkGetPhysicalDeviceMemoryProperties_ p_memprops =
        VK_CAST(PFN_vkGetPhysicalDeviceMemoryProperties_, vk_offload.real_gipa(inst, "vkGetPhysicalDeviceMemoryProperties"));
    if (!p_memprops) {
        return -1;
    }
    VkPhysicalDeviceMemoryProperties_ mp;
    memset(&mp, 0, sizeof(mp));
    p_memprops(dev->gpu, &mp);

    const uint32_t wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount && i < 32; i++) {
        if ((type_bits & (1u << i)) && ((mp.memoryTypes[i].propertyFlags & wanted) == wanted)) {
            return (int)i;
        }
    }
    for (uint32_t i = 0; i < mp.memoryTypeCount && i < 32; i++) {
        if ((type_bits & (1u << i)) &&
            ((mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)) {
            return (int)i;
        }
    }
    return -1;
}

static bool ensure_swapchain_staging(TrackedSwapchain *sc)
{
    if (sc->staging_init) {
        return !sc->staging_failed;
    }
    if (sc->staging_failed || !sc->dev || sc->width == 0 || sc->height == 0) {
        return false;
    }

    TrackedDevice *d = sc->dev;
    if (!d->p_create_buffer || !d->p_get_buf_mem_req || !d->p_alloc_mem ||
        !d->p_bind_buf_mem || !d->p_map_mem || !d->p_create_cmd_pool ||
        !d->p_alloc_cmd_bufs || !d->p_reset_cmd_buf || !d->p_begin_cmd_buf ||
        !d->p_end_cmd_buf || !d->p_cmd_barrier || !d->p_cmd_copy_img_to_buf ||
        !d->p_create_fence || !d->p_reset_fences || !d->p_wait_fences ||
        !d->p_queue_submit) {
        sc->staging_failed = true;
        return false;
    }

    VkDeviceSize_ bytes = (VkDeviceSize_)sc->width * (VkDeviceSize_)sc->height * 4u;

    VkBufferCreateInfo_ bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (d->p_create_buffer(d->device, &bci, NULL, &sc->staging_buf) != VK_SUCCESS || !sc->staging_buf) {
        sc->staging_failed = true;
        return false;
    }

    VkMemoryRequirements_ req;
    memset(&req, 0, sizeof(req));
    d->p_get_buf_mem_req(d->device, sc->staging_buf, &req);

    int mtype = find_host_mem_type(d, req.memoryTypeBits);
    if (mtype < 0) {
        destroy_swapchain_staging(sc);
        sc->staging_failed = true;
        return false;
    }

    VkMemoryAllocateInfo_ mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)mtype;

    if (d->p_alloc_mem(d->device, &mai, NULL, &sc->staging_mem) != VK_SUCCESS || !sc->staging_mem) {
        destroy_swapchain_staging(sc);
        sc->staging_failed = true;
        return false;
    }
    if (d->p_bind_buf_mem(d->device, sc->staging_buf, sc->staging_mem, 0) != VK_SUCCESS) {
        destroy_swapchain_staging(sc);
        sc->staging_failed = true;
        return false;
    }
    if (d->p_map_mem(d->device, sc->staging_mem, 0, bytes, 0, &sc->staging_mapped) != VK_SUCCESS ||
        !sc->staging_mapped) {
        destroy_swapchain_staging(sc);
        sc->staging_failed = true;
        return false;
    }

    VkCommandPoolCreateInfo_ cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = d->queue_family;

    if (d->p_create_cmd_pool(d->device, &cpci, NULL, &sc->cmd_pool) != VK_SUCCESS || !sc->cmd_pool) {
        destroy_swapchain_staging(sc);
        sc->staging_failed = true;
        return false;
    }

    VkCommandBufferAllocateInfo_ cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = sc->cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    if (d->p_alloc_cmd_bufs(d->device, &cbai, &sc->cmd_buf) != VK_SUCCESS || !sc->cmd_buf) {
        destroy_swapchain_staging(sc);
        sc->staging_failed = true;
        return false;
    }

    VkFenceCreateInfo_ fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (d->p_create_fence(d->device, &fci, NULL, &sc->fence) != VK_SUCCESS || !sc->fence) {
        destroy_swapchain_staging(sc);
        sc->staging_failed = true;
        return false;
    }

    sc->staging_size = bytes;
    sc->staging_init = true;
    return true;
}

static void copy_vk_pixels_to_xrgb8888(const uint8_t *src,
                                       uint32_t src_w, uint32_t src_h,
                                       uint32_t format,
                                       void *dst_pixels, int dst_pitch,
                                       int dst_w, int dst_h)
{
    int copy_w = (int)src_w < dst_w ? (int)src_w : dst_w;
    int copy_h = (int)src_h < dst_h ? (int)src_h : dst_h;
    bool is_rgba = (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB);
    size_t src_stride = (size_t)src_w * 4u;
    size_t row_bytes = (size_t)copy_w * 4u;
    uint8_t *dst_base = (uint8_t *)dst_pixels;

    for (int y = 0; y < copy_h; y++) {
        const uint8_t *src_row = src + (size_t)y * src_stride;
        uint8_t *dst_row = dst_base + (size_t)y * (size_t)dst_pitch;
        if (!is_rgba) {
            memcpy(dst_row, src_row, row_bytes);
        } else {
            uint32_t *dst32 = (uint32_t *)dst_row;
            for (int x = 0; x < copy_w; x++) {
                uint32_t r = src_row[x * 4 + 0];
                uint32_t g = src_row[x * 4 + 1];
                uint32_t b = src_row[x * 4 + 2];
                dst32[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
}

static void blit_swapchain_to_window_fb(TrackedSwapchain *sc, uint32_t image_index, VkQueue_ queue)
{
    if (!sc || !sc->window || !sc->images || image_index >= sc->image_count) {
        return;
    }
    if (!sdlop.video ||
        !sdlop.video->CreateWindowFramebuffer ||
        !sdlop.video->UpdateWindowFramebuffer) {
        return;
    }
    if (!ensure_swapchain_staging(sc)) {
        return;
    }

    TrackedDevice *d = sc->dev;
    VkImage_ img = sc->images[image_index];

    if (d->p_reset_fences(d->device, 1, &sc->fence) != VK_SUCCESS ||
        d->p_reset_cmd_buf(sc->cmd_buf, 0) != VK_SUCCESS) {
        return;
    }

    VkCommandBufferBeginInfo_ cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (d->p_begin_cmd_buf(sc->cmd_buf, &cbbi) != VK_SUCCESS) {
        return;
    }

    VkImageMemoryBarrier_ to_src;
    memset(&to_src, 0, sizeof(to_src));
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = img;
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.layerCount = 1;

    d->p_cmd_barrier(sc->cmd_buf,
                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     0, 0, NULL, 0, NULL, 1, &to_src);

    VkBufferImageCopy_ region;
    memset(&region, 0, sizeof(region));
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = sc->width;
    region.imageExtent.height = sc->height;
    region.imageExtent.depth = 1;

    d->p_cmd_copy_img_to_buf(sc->cmd_buf, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             sc->staging_buf, 1, &region);

    VkImageMemoryBarrier_ to_present = to_src;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    d->p_cmd_barrier(sc->cmd_buf,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                     0, 0, NULL, 0, NULL, 1, &to_present);

    if (d->p_end_cmd_buf(sc->cmd_buf) != VK_SUCCESS) {
        return;
    }

    VkSubmitInfo_ si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sc->cmd_buf;

    if (d->p_queue_submit(queue, 1, &si, sc->fence) != VK_SUCCESS) {
        return;
    }
    if (d->p_wait_fences(d->device, 1, &sc->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return;
    }

    SDL_Window *win = sc->window;
    int ww = win->w > 0 ? win->w : (int)sc->width;
    int wh = win->h > 0 ? win->h : (int)sc->height;
    if (win->surface && (win->surface->w != ww || win->surface->h != wh)) {
        if (sdlop.video->DestroyWindowFramebuffer) {
            sdlop.video->DestroyWindowFramebuffer(sdlop.video, win);
        } else {
            SDL_DestroySurface(win->surface);
            win->surface = NULL;
        }
    }
    if (!win->surface) {
        if (!sdlop.video->CreateWindowFramebuffer(sdlop.video, win, &win->surface)) {
            return;
        }
    }
    if (!win->surface || !win->surface->pixels) {
        return;
    }

    copy_vk_pixels_to_xrgb8888((const uint8_t *)sc->staging_mapped,
                               sc->width, sc->height, sc->format,
                               win->surface->pixels, win->surface->pitch,
                               win->surface->w, win->surface->h);

    SDL_Rect rect = { 0, 0, win->surface->w, win->surface->h };
    sdlop.video->UpdateWindowFramebuffer(sdlop.video, win, &rect, 1);
    SDLOP_SendWindowEvent(win, SDL_EVENT_WINDOW_EXPOSED, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Wrapped Vulkan entry points                                         */
/* ------------------------------------------------------------------ */

static VkResult_ wrap_vkEnumeratePhysicalDevices(VkInstance_ instance,
                                                 uint32_t *pPhysicalDeviceCount,
                                                 VkPhysicalDevice_ *pPhysicalDevices)
{
    if (!vk_offload.real_gipa) {
        return -1;
    }
    if (instance) {
        vk_offload.instance = instance;
    }
    PFN_vkEnumeratePhysicalDevices_ p_real =
        VK_CAST(PFN_vkEnumeratePhysicalDevices_, vk_offload.real_gipa(instance, "vkEnumeratePhysicalDevices"));
    if (!p_real) {
        return -1;
    }
    VkResult_ res = p_real(instance, pPhysicalDeviceCount, pPhysicalDevices);
    if (res != VK_SUCCESS || !pPhysicalDevices || !pPhysicalDeviceCount || *pPhysicalDeviceCount == 0) {
        return res;
    }
    for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++) {
        track_gpu_instance(pPhysicalDevices[i], instance);
    }
    if (*pPhysicalDeviceCount <= 1) {
        return res;
    }

    /* Stable-partition hardware GPUs ahead of software (CPU / lavapipe) devices
     * so callers picking physicalDevices[0] automatically get the hardware GPU. */
    uint32_t n = *pPhysicalDeviceCount;
    VkPhysicalDevice_ *sorted = (VkPhysicalDevice_ *)calloc(n, sizeof(*sorted));
    if (!sorted) {
        return res;
    }
    uint32_t out = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (is_hardware_device_type(query_gpu_device_type(vk_offload.real_gipa, instance, pPhysicalDevices[i]))) {
            sorted[out++] = pPhysicalDevices[i];
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        if (!is_hardware_device_type(query_gpu_device_type(vk_offload.real_gipa, instance, pPhysicalDevices[i]))) {
            sorted[out++] = pPhysicalDevices[i];
        }
    }
    memcpy(pPhysicalDevices, sorted, n * sizeof(*sorted));
    free(sorted);
    return res;
}

static VkResult_ wrap_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice_ gpu,
                                                             uint32_t queueFamilyIndex,
                                                             VkSurfaceKHR_ surface,
                                                             VkBool32_ *pSupported)
{
    VkInstance_ inst = resolve_instance_for_gpu(gpu);
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR_ p_real = (vk_offload.real_gipa && inst) ?
        VK_CAST(PFN_vkGetPhysicalDeviceSurfaceSupportKHR_, vk_offload.real_gipa(inst, "vkGetPhysicalDeviceSurfaceSupportKHR")) : NULL;
    if (p_real) {
        VkResult_ res = p_real(gpu, queueFamilyIndex, surface, pSupported);
        if (res == VK_SUCCESS) {
            return res;
        }
    }
    if (find_surface(surface) && pSupported) {
        *pSupported = VK_TRUE;
        return VK_SUCCESS;
    }
    return -1;
}

static VkResult_ wrap_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice_ gpu,
                                                                  VkSurfaceKHR_ surface,
                                                                  VkSurfaceCapabilitiesKHR_ *pCapabilities)
{
    VkInstance_ inst = resolve_instance_for_gpu(gpu);
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR_ p_real = (vk_offload.real_gipa && inst) ?
        VK_CAST(PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR_, vk_offload.real_gipa(inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) : NULL;
    if (!p_real) {
        return -1;
    }
    VkResult_ res = p_real(gpu, surface, pCapabilities);
    if (res == VK_SUCCESS && pCapabilities) {
        TrackedSurface *ts = find_surface(surface);
        if (ts) {
            pCapabilities->supportedUsageFlags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (ts->window &&
                (pCapabilities->currentExtent.width == 0xFFFFFFFFu ||
                 pCapabilities->currentExtent.height == 0xFFFFFFFFu)) {
                pCapabilities->currentExtent.width = ts->window->w > 0 ? (uint32_t)ts->window->w : 640u;
                pCapabilities->currentExtent.height = ts->window->h > 0 ? (uint32_t)ts->window->h : 480u;
            }
        }
    }
    return res;
}

static VkResult_ wrap_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice_ gpu,
                                                             VkSurfaceKHR_ surface,
                                                             uint32_t *pSurfaceFormatCount,
                                                             VkSurfaceFormatKHR_ *pSurfaceFormats)
{
    VkInstance_ inst = resolve_instance_for_gpu(gpu);
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR_ p_real = (vk_offload.real_gipa && inst) ?
        VK_CAST(PFN_vkGetPhysicalDeviceSurfaceFormatsKHR_, vk_offload.real_gipa(inst, "vkGetPhysicalDeviceSurfaceFormatsKHR")) : NULL;
    if (!p_real) {
        return -1;
    }
    VkResult_ res = p_real(gpu, surface, pSurfaceFormatCount, pSurfaceFormats);
    if (res == VK_SUCCESS && pSurfaceFormatCount && *pSurfaceFormatCount == 0 && find_surface(surface)) {
        if (!pSurfaceFormats) {
            *pSurfaceFormatCount = 1;
        } else {
            pSurfaceFormats[0].format = VK_FORMAT_B8G8R8A8_UNORM;
            pSurfaceFormats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            *pSurfaceFormatCount = 1;
        }
    }
    return res;
}

static VkResult_ wrap_vkCreateSwapchainKHR(VkDevice_ device,
                                            const VkSwapchainCreateInfoKHR_ *pCreateInfo,
                                            const void *pAllocator,
                                            VkSwapchainKHR_ *pSwapchain)
{
    TrackedDevice *td = find_device(device);
    if (!td || !td->real_create_swapchain || !pCreateInfo || !pSwapchain) {
        return -1;
    }

    TrackedSurface *ts = find_surface(pCreateInfo->surface);
    VkSwapchainCreateInfoKHR_ ci = *pCreateInfo;
    if (ts) {
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    VkResult_ res = td->real_create_swapchain(device, &ci, pAllocator, pSwapchain);
    if (res != VK_SUCCESS && ts) {
        /* Retry without forcing TRANSFER_SRC_BIT if driver rejected it. */
        res = td->real_create_swapchain(device, pCreateInfo, pAllocator, pSwapchain);
    }
    if (res == VK_SUCCESS && ts && *pSwapchain) {
        TrackedSwapchain *sc = (TrackedSwapchain *)calloc(1, sizeof(*sc));
        if (sc) {
            sc->swapchain = *pSwapchain;
            sc->dev = td;
            sc->window = ts->window;
            sc->width = pCreateInfo->imageExtent.width;
            sc->height = pCreateInfo->imageExtent.height;
            sc->format = pCreateInfo->imageFormat;
            if (td->real_get_swapchain_images) {
                uint32_t ic = 0;
                if (td->real_get_swapchain_images(device, *pSwapchain, &ic, NULL) == VK_SUCCESS && ic > 0) {
                    sc->images = (VkImage_ *)calloc(ic, sizeof(VkImage_));
                    if (sc->images &&
                        td->real_get_swapchain_images(device, *pSwapchain, &ic, sc->images) == VK_SUCCESS) {
                        sc->image_count = ic;
                    }
                }
            }
            sc->next = vk_offload.swapchains;
            vk_offload.swapchains = sc;
        }
    }
    return res;
}

static VkResult_ wrap_vkGetSwapchainImagesKHR(VkDevice_ device,
                                               VkSwapchainKHR_ swapchain,
                                               uint32_t *pSwapchainImageCount,
                                               VkImage_ *pSwapchainImages)
{
    TrackedDevice *td = find_device(device);
    if (!td || !td->real_get_swapchain_images) {
        return -1;
    }
    VkResult_ res = td->real_get_swapchain_images(device, swapchain, pSwapchainImageCount, pSwapchainImages);
    if (res == VK_SUCCESS && pSwapchainImages && pSwapchainImageCount && *pSwapchainImageCount > 0) {
        TrackedSwapchain *sc = find_swapchain(swapchain);
        if (sc) {
            free(sc->images);
            sc->images = (VkImage_ *)calloc(*pSwapchainImageCount, sizeof(VkImage_));
            if (sc->images) {
                memcpy(sc->images, pSwapchainImages, *pSwapchainImageCount * sizeof(VkImage_));
                sc->image_count = *pSwapchainImageCount;
            }
        }
    }
    return res;
}

static void wrap_vkDestroySwapchainKHR(VkDevice_ device,
                                       VkSwapchainKHR_ swapchain,
                                       const void *pAllocator)
{
    TrackedDevice *td = find_device(device);
    TrackedSwapchain **pp = &vk_offload.swapchains;
    while (*pp) {
        TrackedSwapchain *sc = *pp;
        if (sc->swapchain == swapchain) {
            *pp = sc->next;
            free_tracked_swapchain(sc);
            break;
        }
        pp = &sc->next;
    }
    if (td && td->real_destroy_swapchain) {
        td->real_destroy_swapchain(device, swapchain, pAllocator);
    }
}

static VkResult_ wrap_vkQueuePresentKHR(VkQueue_ queue, const VkPresentInfoKHR_ *pPresentInfo)
{
    PFN_vkQueuePresentKHR_ p_present = NULL;
    if (pPresentInfo && pPresentInfo->swapchainCount > 0 && pPresentInfo->pSwapchains) {
        TrackedSwapchain *sc0 = find_swapchain(pPresentInfo->pSwapchains[0]);
        if (sc0 && sc0->dev) {
            p_present = sc0->dev->real_queue_present;
        }
    }
    if (!p_present && vk_offload.devices) {
        p_present = vk_offload.devices->real_queue_present;
    }
    if (!p_present) {
        return -1;
    }

    VkResult_ res = p_present(queue, pPresentInfo);
    if (res == VK_SUCCESS && pPresentInfo && pPresentInfo->pSwapchains && pPresentInfo->pImageIndices) {
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            TrackedSwapchain *sc = find_swapchain(pPresentInfo->pSwapchains[i]);
            if (sc && sc->window) {
                blit_swapchain_to_window_fb(sc, pPresentInfo->pImageIndices[i], queue);
            }
        }
    }
    return res;
}

static void wrap_vkDestroyDevice(VkDevice_ device, const void *pAllocator)
{
    TrackedSwapchain **spp = &vk_offload.swapchains;
    while (*spp) {
        TrackedSwapchain *sc = *spp;
        if (sc->dev && sc->dev->device == device) {
            *spp = sc->next;
            free_tracked_swapchain(sc);
        } else {
            spp = &sc->next;
        }
    }

    TrackedDevice *td = NULL;
    TrackedDevice **dpp = &vk_offload.devices;
    while (*dpp) {
        if ((*dpp)->device == device) {
            td = *dpp;
            *dpp = td->next;
            break;
        }
        dpp = &(*dpp)->next;
    }

    if (td) {
        PFN_vkDestroyDevice_ real_destroy = td->real_destroy_device;
        free(td);
        if (real_destroy) {
            real_destroy(device, pAllocator);
        }
    }
}

static PFN_vkVoid_ wrap_vkGetDeviceProcAddr(VkDevice_ device, const char *pName)
{
    if (!pName) {
        return NULL;
    }
    TrackedDevice *td = find_device(device);
    if (!td || !td->real_gdpa) {
        VkInstance_ inst = (td && td->instance) ? td->instance : vk_offload.instance;
        return (vk_offload.real_gipa && inst) ? vk_offload.real_gipa(inst, pName) : NULL;
    }
    if (vk_offload.active) {
        if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
            return VK_VOID(wrap_vkCreateSwapchainKHR);
        }
        if (strcmp(pName, "vkDestroySwapchainKHR") == 0) {
            return VK_VOID(wrap_vkDestroySwapchainKHR);
        }
        if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) {
            return VK_VOID(wrap_vkGetSwapchainImagesKHR);
        }
        if (strcmp(pName, "vkQueuePresentKHR") == 0) {
            return VK_VOID(wrap_vkQueuePresentKHR);
        }
        if (strcmp(pName, "vkDestroyDevice") == 0) {
            return VK_VOID(wrap_vkDestroyDevice);
        }
    }
    return td->real_gdpa(device, pName);
}

static VkResult_ wrap_vkCreateInstance(const void *pCreateInfo,
                                       const void *pAllocator,
                                       VkInstance_ *pInstance)
{
    if (!vk_offload.real_gipa) {
        return -1;
    }
    PFN_vkCreateInstance_ p_real =
        VK_CAST(PFN_vkCreateInstance_, vk_offload.real_gipa(NULL, "vkCreateInstance"));
    if (!p_real) {
        return -1;
    }
    VkResult_ res = p_real(pCreateInfo, pAllocator, pInstance);
    if (res == VK_SUCCESS && pInstance && *pInstance) {
        vk_offload.instance = *pInstance;
    }
    return res;
}

static void wrap_vkDestroyInstance(VkInstance_ instance, const void *pAllocator)
{
    if (vk_offload.real_gipa && instance) {
        PFN_vkDestroyInstance_ p_real =
            VK_CAST(PFN_vkDestroyInstance_, vk_offload.real_gipa(instance, "vkDestroyInstance"));
        if (p_real) {
            p_real(instance, pAllocator);
        }
    }
    untrack_gpus_for_instance(instance);
    if (vk_offload.instance == instance) {
        vk_offload.instance = NULL;
    }
}

static VkResult_ wrap_vkCreateDevice(VkPhysicalDevice_ physicalDevice,
                                     const VkDeviceCreateInfo_ *pCreateInfo,
                                     const void *pAllocator,
                                     VkDevice_ *pDevice)
{
    VkInstance_ inst = resolve_instance_for_gpu(physicalDevice);
    if (!vk_offload.real_gipa || !inst) {
        return -1;
    }
    PFN_vkCreateDevice_ p_real =
        VK_CAST(PFN_vkCreateDevice_, vk_offload.real_gipa(inst, "vkCreateDevice"));
    if (!p_real) {
        return -1;
    }
    VkResult_ res = p_real(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS || !pDevice || !*pDevice) {
        return res;
    }

    TrackedDevice *td = (TrackedDevice *)calloc(1, sizeof(*td));
    if (!td) {
        return res;
    }
    td->device = *pDevice;
    td->instance = inst;
    td->gpu = physicalDevice;
    td->queue_family = (pCreateInfo && pCreateInfo->queueCreateInfoCount > 0 && pCreateInfo->pQueueCreateInfos)
        ? pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex : 0u;

    td->real_gdpa = VK_CAST(PFN_vkGetDeviceProcAddr_, vk_offload.real_gipa(inst, "vkGetDeviceProcAddr"));
    PFN_vkGetDeviceProcAddr_ gdpa = td->real_gdpa;
    if (gdpa) {
        td->real_destroy_device = VK_CAST(PFN_vkDestroyDevice_, gdpa(*pDevice, "vkDestroyDevice"));
        td->real_create_swapchain = VK_CAST(PFN_vkCreateSwapchainKHR_, gdpa(*pDevice, "vkCreateSwapchainKHR"));
        td->real_destroy_swapchain = VK_CAST(PFN_vkDestroySwapchainKHR_, gdpa(*pDevice, "vkDestroySwapchainKHR"));
        td->real_get_swapchain_images = VK_CAST(PFN_vkGetSwapchainImagesKHR_, gdpa(*pDevice, "vkGetSwapchainImagesKHR"));
        td->real_queue_present = VK_CAST(PFN_vkQueuePresentKHR_, gdpa(*pDevice, "vkQueuePresentKHR"));

        td->p_create_buffer = VK_CAST(PFN_vkCreateBuffer_, gdpa(*pDevice, "vkCreateBuffer"));
        td->p_destroy_buffer = VK_CAST(PFN_vkDestroyBuffer_, gdpa(*pDevice, "vkDestroyBuffer"));
        td->p_get_buf_mem_req = VK_CAST(PFN_vkGetBufferMemoryRequirements_, gdpa(*pDevice, "vkGetBufferMemoryRequirements"));
        td->p_alloc_mem = VK_CAST(PFN_vkAllocateMemory_, gdpa(*pDevice, "vkAllocateMemory"));
        td->p_free_mem = VK_CAST(PFN_vkFreeMemory_, gdpa(*pDevice, "vkFreeMemory"));
        td->p_bind_buf_mem = VK_CAST(PFN_vkBindBufferMemory_, gdpa(*pDevice, "vkBindBufferMemory"));
        td->p_map_mem = VK_CAST(PFN_vkMapMemory_, gdpa(*pDevice, "vkMapMemory"));
        td->p_unmap_mem = VK_CAST(PFN_vkUnmapMemory_, gdpa(*pDevice, "vkUnmapMemory"));
        td->p_create_cmd_pool = VK_CAST(PFN_vkCreateCommandPool_, gdpa(*pDevice, "vkCreateCommandPool"));
        td->p_destroy_cmd_pool = VK_CAST(PFN_vkDestroyCommandPool_, gdpa(*pDevice, "vkDestroyCommandPool"));
        td->p_alloc_cmd_bufs = VK_CAST(PFN_vkAllocateCommandBuffers_, gdpa(*pDevice, "vkAllocateCommandBuffers"));
        td->p_reset_cmd_buf = VK_CAST(PFN_vkResetCommandBuffer_, gdpa(*pDevice, "vkResetCommandBuffer"));
        td->p_begin_cmd_buf = VK_CAST(PFN_vkBeginCommandBuffer_, gdpa(*pDevice, "vkBeginCommandBuffer"));
        td->p_end_cmd_buf = VK_CAST(PFN_vkEndCommandBuffer_, gdpa(*pDevice, "vkEndCommandBuffer"));
        td->p_cmd_barrier = VK_CAST(PFN_vkCmdPipelineBarrier_, gdpa(*pDevice, "vkCmdPipelineBarrier"));
        td->p_cmd_copy_img_to_buf = VK_CAST(PFN_vkCmdCopyImageToBuffer_, gdpa(*pDevice, "vkCmdCopyImageToBuffer"));
        td->p_create_fence = VK_CAST(PFN_vkCreateFence_, gdpa(*pDevice, "vkCreateFence"));
        td->p_destroy_fence = VK_CAST(PFN_vkDestroyFence_, gdpa(*pDevice, "vkDestroyFence"));
        td->p_reset_fences = VK_CAST(PFN_vkResetFences_, gdpa(*pDevice, "vkResetFences"));
        td->p_wait_fences = VK_CAST(PFN_vkWaitForFences_, gdpa(*pDevice, "vkWaitForFences"));
        td->p_queue_submit = VK_CAST(PFN_vkQueueSubmit_, gdpa(*pDevice, "vkQueueSubmit"));
    }

    td->next = vk_offload.devices;
    vk_offload.devices = td;
    return res;
}

static PFN_vkVoid_ wrapped_vkGetInstanceProcAddr(VkInstance_ instance, const char *pName)
{
    if (!vk_offload.real_gipa || !pName) {
        return NULL;
    }
    if (instance) {
        vk_offload.instance = instance;
    }
    if (vk_offload.active) {
        if (!instance && strcmp(pName, "vkCreateInstance") == 0) {
            return VK_VOID(wrap_vkCreateInstance);
        }
        if (strcmp(pName, "vkDestroyInstance") == 0) {
            return VK_VOID(wrap_vkDestroyInstance);
        }
        if (strcmp(pName, "vkEnumeratePhysicalDevices") == 0) {
            return VK_VOID(wrap_vkEnumeratePhysicalDevices);
        }
        if (strcmp(pName, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0) {
            return VK_VOID(wrap_vkGetPhysicalDeviceSurfaceSupportKHR);
        }
        if (strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) {
            return VK_VOID(wrap_vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
        }
        if (strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0) {
            return VK_VOID(wrap_vkGetPhysicalDeviceSurfaceFormatsKHR);
        }
        if (strcmp(pName, "vkCreateDevice") == 0) {
            return VK_VOID(wrap_vkCreateDevice);
        }
        if (strcmp(pName, "vkDestroyDevice") == 0) {
            return VK_VOID(wrap_vkDestroyDevice);
        }
        if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
            return VK_VOID(wrap_vkGetDeviceProcAddr);
        }
        if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
            return VK_VOID(wrap_vkCreateSwapchainKHR);
        }
        if (strcmp(pName, "vkDestroySwapchainKHR") == 0) {
            return VK_VOID(wrap_vkDestroySwapchainKHR);
        }
        if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) {
            return VK_VOID(wrap_vkGetSwapchainImagesKHR);
        }
        if (strcmp(pName, "vkQueuePresentKHR") == 0) {
            return VK_VOID(wrap_vkQueuePresentKHR);
        }
    }
    return vk_offload.real_gipa(instance, pName);
}

SDL_FunctionPointer SDLOP_VulkanOffload_WrapGetInstanceProcAddr(SDL_FunctionPointer real_gipa)
{
    vk_offload.real_gipa = VK_CAST(PFN_vkGetInstanceProcAddr_, real_gipa);
    if (!real_gipa) {
        return NULL;
    }
    return VK_CAST(SDL_FunctionPointer, wrapped_vkGetInstanceProcAddr);
}

void SDLOP_VulkanOffload_Shutdown(void)
{
    while (vk_offload.swapchains) {
        TrackedSwapchain *sc = vk_offload.swapchains;
        vk_offload.swapchains = sc->next;
        free_tracked_swapchain(sc);
    }
    while (vk_offload.devices) {
        TrackedDevice *td = vk_offload.devices;
        vk_offload.devices = td->next;
        free(td);
    }
    while (vk_offload.gpus) {
        TrackedPhysicalDevice *g = vk_offload.gpus;
        vk_offload.gpus = g->next;
        free(g);
    }
    while (vk_offload.surfaces) {
        TrackedSurface *ts = vk_offload.surfaces;
        vk_offload.surfaces = ts->next;
        free(ts);
    }
    vk_offload.instance = NULL;
    vk_offload.active = false;
    vk_offload.probed = false;
    vk_offload.cached_policy = SDLOP_VK_OFFLOAD_AUTO;
    vk_offload.cached_video = NULL;
    vk_offload.cached_decision = false;
}
