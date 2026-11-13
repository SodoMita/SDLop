/*
  SDLop - KMS/DRM Vulkan WSI (VK_KHR_surface + VK_KHR_display).

  A port of SDL3's kmsdrmvulkan.c merged into SDLop's Vulkan style: no
  vulkan headers or link-time dependency; entry points are fetched from the
  loader that SDL_vulkan.c has dlopen'd (SDL_Vulkan_GetVkGetInstanceProcAddr),
  exactly like sdlop_wayland_vulkan.c does for VK_KHR_wayland_surface.

  KMS/DRM is a fullscreen, x-less display system, so the surface is a
  VK_KHR_display plane surface on the display owned by the KMS CRTC. SDL3's
  code creates a custom display mode when the requested window size has no
  predefined mode; so does this port. One display per DRM device is
  supported (SDLop's KMSDRM driver drives a single connector/CRTC).

  Two deliberate deviations from SDL3, both required for lavapipe (the
  software Vulkan driver):
    - SDLop has no SDL display list; the Vulkan display chosen is the
      first enumerated VK display (index 0), matching the single display
      this device drives.
    - SDL3 only accepts physical devices of type integrated/discrete GPU;
      lavapipe reports VK_PHYSICAL_DEVICE_TYPE_CPU (4) and would be
      rejected. This port accepts any device that
      vkEnumeratePhysicalDevices returns and reports its deviceType in the
      log instead.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include "sdlop_kmsdrm.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

/* ------------------------------------------------------------------ */
/* Minimal Vulkan type definitions (no vulkan.h)                       */
/* ------------------------------------------------------------------ */

#define VK_SUCCESS                        0
#define VK_MAX_EXTENSION_NAME_SIZE        256
#define VK_API_VERSION_1_0                (1u << 22)
#define VK_STRUCTURE_TYPE_DISPLAY_MODE_CREATE_INFO_KHR  1000002000
#define VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR 1000002001
#define VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR  0x00000001
#define VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR  0x00000001
#define VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU 1
#define VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   2
#define VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    3
#define VK_PHYSICAL_DEVICE_TYPE_CPU            4
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2      1000059000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT 1000247000
#define VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME           "VK_EXT_physical_device_drm"

typedef int32_t  VkResult_;
typedef uint32_t VkFlags_;
typedef void    *VkInstance_;
typedef void    *VkPhysicalDevice_;
typedef void    *VkDisplayKHR_;
typedef void    *VkDisplayModeKHR_;
typedef uint64_t VkSurfaceKHR_;
typedef uint32_t VkBool32_;

typedef struct VkExtensionProperties_
{
    char     extensionName[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
} VkExtensionProperties_;

typedef struct VkExtent2D_
{
    uint32_t width, height;
} VkExtent2D_;

typedef struct VkOffset2D_
{
    int32_t x, y;
} VkOffset2D_;

typedef struct VkDisplayModeParametersKHR_
{
    VkExtent2D_ visibleRegion;
    uint32_t    refreshRate; /* thousandths of Hz */
} VkDisplayModeParametersKHR_;

typedef struct VkDisplayModeCreateInfoKHR_
{
    uint32_t                   sType;            /* VK_STRUCTURE_TYPE_DISPLAY_MODE_CREATE_INFO_KHR */
    const void                *pNext;
    VkFlags_                   flags;
    VkDisplayModeParametersKHR_ parameters;
} VkDisplayModeCreateInfoKHR_;

typedef struct VkDisplayModePropertiesKHR_
{
    VkDisplayModeKHR_          displayMode;
    VkDisplayModeParametersKHR_ parameters;
} VkDisplayModePropertiesKHR_;

typedef struct VkDisplayPropertiesKHR_
{
    VkDisplayKHR_ display;
    const char   *displayName;
    VkExtent2D_   physicalDimensions;
    VkExtent2D_   physicalResolution;
    VkFlags_      supportedTransforms;
    VkBool32_     planeReorderPossible;
    VkBool32_     persistentContent;
} VkDisplayPropertiesKHR_;

typedef struct VkDisplayPlanePropertiesKHR_
{
    VkDisplayKHR_ currentDisplay;
    uint32_t      currentStackIndex;
} VkDisplayPlanePropertiesKHR_;

typedef struct VkDisplayPlaneCapabilitiesKHR_
{
    VkFlags_    supportedAlpha;
    VkOffset2D_ minSrcPosition;
    VkOffset2D_ maxSrcPosition;
    VkExtent2D_ minSrcExtent;
    VkExtent2D_ maxSrcExtent;
    VkOffset2D_ minDstPosition;
    VkOffset2D_ maxDstPosition;
    VkExtent2D_ minDstExtent;
    VkExtent2D_ maxDstExtent;
} VkDisplayPlaneCapabilitiesKHR_;

typedef struct VkDisplaySurfaceCreateInfoKHR_
{
    uint32_t    sType;          /* VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR */
    const void *pNext;
    VkFlags_    flags;
    VkDisplayModeKHR_ displayMode;
    uint32_t    planeIndex;
    uint32_t    planeStackIndex;
    uint32_t    transform;      /* VkSurfaceTransformFlagBitsKHR */
    float       globalAlpha;
    uint32_t    alphaMode;      /* VkDisplayPlaneAlphaFlagBitsKHR */
    VkExtent2D_ imageExtent;
} VkDisplaySurfaceCreateInfoKHR_;

/* vkGetPhysicalDeviceProperties writes the full driver-defined struct; the
 * layout prefix up to deviceType is fixed by the spec:
 *   apiVersion@0, driverVersion@4, vendorID@8, deviceID@12, deviceType@16
 * so read those out of a generous opaque buffer instead of copying the
 * whole (version-dependent) layout. */
typedef union KMSDRM_VkPhysicalDeviceProperties_
{
    uint64_t      alignment;
    unsigned char bytes[4096];
} KMSDRM_VkPhysicalDeviceProperties_;

/* VK_EXT_physical_device_drm: match a Vulkan device to the DRM device the
 * backend picked (major/minor of the primary node) */
typedef struct VkPhysicalDeviceDrmPropertiesEXT_
{
    uint32_t sType;
    const void *pNext;
    uint64_t drmDeviceID;
    int32_t  drmPrimaryMajor, drmPrimaryMinor, drmRenderMajor, drmRenderMinor;
} VkPhysicalDeviceDrmPropertiesEXT_;

typedef struct VkPhysicalDeviceProperties2_
{
    uint32_t sType;
    void *pNext;
    KMSDRM_VkPhysicalDeviceProperties_ properties;
} VkPhysicalDeviceProperties2_;

typedef void *(*PFN_vkGetInstanceProcAddr_)(VkInstance_ instance, const char *pName);
typedef VkResult_ (*PFN_vkEnumerateInstanceExtensionProperties_)(const char *pLayerName,
                                                                 uint32_t *pPropertyCount,
                                                                 VkExtensionProperties_ *pProperties);
typedef VkResult_ (*PFN_vkEnumeratePhysicalDevices_)(VkInstance_ instance,
                                                     uint32_t *pPhysicalDeviceCount,
                                                     VkPhysicalDevice_ *pPhysicalDevices);
typedef void (*PFN_vkGetPhysicalDeviceProperties_)(VkPhysicalDevice_ physicalDevice,
                                                   KMSDRM_VkPhysicalDeviceProperties_ *pProperties);
typedef VkResult_ (*PFN_vkGetPhysicalDeviceDisplayPropertiesKHR_)(VkPhysicalDevice_ physicalDevice,
                                                                  uint32_t *pPropertyCount,
                                                                  VkDisplayPropertiesKHR_ *pProperties);
typedef VkResult_ (*PFN_vkGetDisplayModePropertiesKHR_)(VkPhysicalDevice_ physicalDevice,
                                                        VkDisplayKHR_ display,
                                                        uint32_t *pPropertyCount,
                                                        VkDisplayModePropertiesKHR_ *pProperties);
typedef VkResult_ (*PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR_)(VkPhysicalDevice_ physicalDevice,
                                                                       uint32_t *pPropertyCount,
                                                                       VkDisplayPlanePropertiesKHR_ *pProperties);
typedef VkResult_ (*PFN_vkGetDisplayPlaneSupportedDisplaysKHR_)(VkPhysicalDevice_ physicalDevice,
                                                                uint32_t planeIndex,
                                                                uint32_t *pDisplayCount,
                                                                VkDisplayKHR_ *pDisplays);
typedef VkResult_ (*PFN_vkGetDisplayPlaneCapabilitiesKHR_)(VkPhysicalDevice_ physicalDevice,
                                                           VkDisplayModeKHR_ mode,
                                                           uint32_t planeIndex,
                                                           VkDisplayPlaneCapabilitiesKHR_ *pCapabilities);
typedef VkResult_ (*PFN_vkCreateDisplayModeKHR_)(VkPhysicalDevice_ physicalDevice,
                                                 VkDisplayKHR_ display,
                                                 const VkDisplayModeCreateInfoKHR_ *pCreateInfo,
                                                 const void *pAllocator,
                                                 VkDisplayModeKHR_ *pMode);
typedef VkResult_ (*PFN_vkCreateDisplayPlaneSurfaceKHR_)(VkInstance_ instance,
                                                         const VkDisplaySurfaceCreateInfoKHR_ *pCreateInfo,
                                                         const void *pAllocator,
                                                         VkSurfaceKHR_ *pSurface);
typedef VkResult_ (*PFN_vkEnumerateDeviceExtensionProperties_)(VkPhysicalDevice_ physicalDevice,
                                                               const char *pLayerName,
                                                               uint32_t *pPropertyCount,
                                                               VkExtensionProperties_ *pProperties);
typedef void (*PFN_vkGetPhysicalDeviceProperties2_)(VkPhysicalDevice_ physicalDevice,
                                                    VkPhysicalDeviceProperties2_ *pProperties);

/* ------------------------------------------------------------------ */
/* Instance extensions advertised to apps                              */
/* ------------------------------------------------------------------ */

static const char *const kmsdrm_vulkan_extensions[] = {
    "VK_KHR_surface",
    "VK_KHR_display",
    NULL
};

const char *const *SDLOP_KMSDRM_Vulkan_GetInstanceExtensions(SDLop_VideoDevice *device, Uint32 *count)
{
    (void)device;
    if (count) {
        *count = 2;
    }
    return kmsdrm_vulkan_extensions;
}

static PFN_vkGetInstanceProcAddr_ kmsdrm_vk_gipa(void)
{
    return (PFN_vkGetInstanceProcAddr_)SDL_Vulkan_GetVkGetInstanceProcAddr();
}

/* Check that `name` was reported by vkEnumerateInstanceExtensionProperties
 * (i.e. the driver implements it). When in doubt, return true and let the
 * CreateDisplayPlaneSurface call fail naturally. */
static bool kmsdrm_vulkan_extension_available(VkInstance_ instance, const char *name)
{
    PFN_vkGetInstanceProcAddr_ gipa = kmsdrm_vk_gipa();
    if (!gipa) {
        return true;
    }
    PFN_vkEnumerateInstanceExtensionProperties_ vkEIExt =
        (PFN_vkEnumerateInstanceExtensionProperties_)gipa(instance, "vkEnumerateInstanceExtensionProperties");
    if (!vkEIExt) {
        return true;
    }
    uint32_t count = 0;
    if (vkEIExt(NULL, &count, NULL) != VK_SUCCESS || count == 0) {
        return true;
    }
    VkExtensionProperties_ *props = (VkExtensionProperties_ *)calloc(count, sizeof(*props));
    if (!props) {
        return true;
    }
    bool found = false;
    if (vkEIExt(NULL, &count, props) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(props[i].extensionName, name) == 0) {
                found = true;
                break;
            }
        }
    } else {
        found = true; /* can't tell; don't second-guess */
    }
    free(props);
    return found;
}

/* ------------------------------------------------------------------ */
/* Surface creation (SDL3's KMSDRM_Vulkan_CreateSurface)               */
/* ------------------------------------------------------------------ */

/* Does this Vulkan device drive the DRM device at (drm_major, drm_minor)?
 * Returns false when VK_EXT_physical_device_drm (or the loader entry points
 * needed to query it) is unavailable. */
static bool kmsdrmvk_device_matches_drm(VkInstance_ instance, VkPhysicalDevice_ gpu,
                                        uint32_t drm_major, uint32_t drm_minor)
{
    PFN_vkGetInstanceProcAddr_ gipa = kmsdrm_vk_gipa();
    if (!gipa) {
        return false;
    }
    PFN_vkEnumerateDeviceExtensionProperties_ pEnumDevExt =
        (PFN_vkEnumerateDeviceExtensionProperties_)gipa(instance, "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceProperties2_ pGetProps2 =
        (PFN_vkGetPhysicalDeviceProperties2_)gipa(instance, "vkGetPhysicalDeviceProperties2");
    if (!pEnumDevExt || !pGetProps2) {
        return false;
    }
    uint32_t ext_count = 0;
    if (pEnumDevExt(gpu, NULL, &ext_count, NULL) != VK_SUCCESS || ext_count == 0) {
        return false;
    }
    VkExtensionProperties_ *exts = (VkExtensionProperties_ *)calloc(ext_count, sizeof(*exts));
    if (!exts) {
        return false;
    }
    if (pEnumDevExt(gpu, NULL, &ext_count, exts) != VK_SUCCESS) {
        free(exts);
        return false;
    }
    bool has_drm_ext = false;
    for (uint32_t i = 0; i < ext_count; i++) {
        if (strcmp(exts[i].extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0) {
            has_drm_ext = true;
            break;
        }
    }
    free(exts);
    if (!has_drm_ext) {
        return false;
    }
    VkPhysicalDeviceDrmPropertiesEXT_ drm_props;
    memset(&drm_props, 0, sizeof(drm_props));
    drm_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    drm_props.drmPrimaryMajor = -1;
    drm_props.drmPrimaryMinor = -1;
    VkPhysicalDeviceProperties2_ props2;
    memset(&props2, 0, sizeof(props2));
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &drm_props;
    pGetProps2(gpu, &props2);
    return drm_props.drmPrimaryMajor == (int32_t)drm_major &&
           drm_props.drmPrimaryMinor == (int32_t)drm_minor;
}

bool SDLOP_KMSDRM_Vulkan_CreateSurface(SDLop_VideoDevice *device, SDL_Window *window,
                                       void *instance, const void *allocator, Uint64 *surface)
{
    (void)device;
    if (!window || !instance || !surface) {
        SDL_SetError("Invalid arguments for Vulkan surface creation");
        return false;
    }

    PFN_vkGetInstanceProcAddr_ gipa = kmsdrm_vk_gipa();
    if (!gipa) {
        SDL_SetError("Vulkan loader is not loaded");
        return false;
    }

    /* hard requirements: the instance must have VK_KHR_display enabled */
    PFN_vkCreateDisplayPlaneSurfaceKHR_ vkCreateDisplayPlaneSurfaceKHR =
        (PFN_vkCreateDisplayPlaneSurfaceKHR_)gipa((VkInstance_)instance, "vkCreateDisplayPlaneSurfaceKHR");
    if (!vkCreateDisplayPlaneSurfaceKHR) {
        SDL_SetError("VK_KHR_display extension is not enabled in the Vulkan instance "
                     "(SDL_Vulkan_GetInstanceExtensions() advertises it; enable it on the instance)");
        return false;
    }
    if (!kmsdrm_vulkan_extension_available((VkInstance_)instance, "VK_KHR_display")
        || !kmsdrm_vulkan_extension_available((VkInstance_)instance, "VK_KHR_surface")) {
        SDL_SetError("Installed Vulkan driver does not support VK_KHR_display/VK_KHR_surface");
        return false;
    }

    PFN_vkEnumeratePhysicalDevices_ vkEnumeratePhysicalDevices =
        (PFN_vkEnumeratePhysicalDevices_)gipa((VkInstance_)instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties_ vkGetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties_)gipa((VkInstance_)instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceDisplayPropertiesKHR_ vkGetPhysicalDeviceDisplayPropertiesKHR =
        (PFN_vkGetPhysicalDeviceDisplayPropertiesKHR_)gipa((VkInstance_)instance, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    PFN_vkGetDisplayModePropertiesKHR_ vkGetDisplayModePropertiesKHR =
        (PFN_vkGetDisplayModePropertiesKHR_)gipa((VkInstance_)instance, "vkGetDisplayModePropertiesKHR");
    PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR_ vkGetPhysicalDeviceDisplayPlanePropertiesKHR =
        (PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR_)gipa((VkInstance_)instance, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR");
    PFN_vkGetDisplayPlaneSupportedDisplaysKHR_ vkGetDisplayPlaneSupportedDisplaysKHR =
        (PFN_vkGetDisplayPlaneSupportedDisplaysKHR_)gipa((VkInstance_)instance, "vkGetDisplayPlaneSupportedDisplaysKHR");
    PFN_vkGetDisplayPlaneCapabilitiesKHR_ vkGetDisplayPlaneCapabilitiesKHR =
        (PFN_vkGetDisplayPlaneCapabilitiesKHR_)gipa((VkInstance_)instance, "vkGetDisplayPlaneCapabilitiesKHR");
    PFN_vkCreateDisplayModeKHR_ vkCreateDisplayModeKHR =
        (PFN_vkCreateDisplayModeKHR_)gipa((VkInstance_)instance, "vkCreateDisplayModeKHR");

    if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties
        || !vkGetPhysicalDeviceDisplayPropertiesKHR || !vkGetDisplayModePropertiesKHR
        || !vkGetPhysicalDeviceDisplayPlanePropertiesKHR || !vkGetDisplayPlaneSupportedDisplaysKHR
        || !vkGetDisplayPlaneCapabilitiesKHR || !vkCreateDisplayModeKHR) {
        SDL_SetError("Vulkan driver is missing VK_KHR_display entry points");
        return false;
    }

    VkPhysicalDevice_ *physical_devices = NULL;
    VkDisplayPropertiesKHR_ *display_props = NULL;
    VkDisplayModePropertiesKHR_ *mode_props = NULL;
    VkDisplayPlanePropertiesKHR_ *plane_props = NULL;
    bool ok = false;

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices((VkInstance_)instance, &gpu_count, NULL);
    if (gpu_count == 0) {
        SDL_SetError("Vulkan can't find any physical devices");
        goto clean;
    }
    physical_devices = (VkPhysicalDevice_ *)calloc(gpu_count, sizeof(VkPhysicalDevice_));
    if (!physical_devices) {
        SDL_OutOfMemory();
        goto clean;
    }
    vkEnumeratePhysicalDevices((VkInstance_)instance, &gpu_count, physical_devices);

    /* DRM device the backend drives — used to prefer the matching GPU */
    uint32_t drm_major = 0, drm_minor = 0;
    bool drm_fd_ok = false;
    {
        KMSDRM_DeviceData *vid = KMSDRM_GetDevice();
        struct stat st;
        if (vid && vid->drm_fd >= 0 && fstat(vid->drm_fd, &st) == 0) {
            drm_major = (uint32_t)major(st.st_rdev);
            drm_minor = (uint32_t)minor(st.st_rdev);
            drm_fd_ok = true;
        }
    }

    /* Iterate the candidates and pick the first device with at least one
     * KMS display and a plane compatible with it. A device whose DRM node
     * matches the backend's (VK_EXT_physical_device_drm) wins over plain
     * enumeration order. Devices without KMS displays (e.g. lavapipe) are
     * skipped like any other unusable candidate instead of aborting. */
    VkPhysicalDevice_ gpu = NULL;
    uint32_t gpu_index = UINT32_MAX;
    uint32_t gpu_api_version = 0, gpu_device_type = 0;
    int gpu_score = -1;
    for (uint32_t i = 0; i < gpu_count; i++) {
        KMSDRM_VkPhysicalDeviceProperties_ props;
        memset(&props, 0, sizeof(props));
        vkGetPhysicalDeviceProperties(physical_devices[i], &props);
        uint32_t api_version = 0, device_type = 0;
        memcpy(&api_version, props.bytes + 0, sizeof(api_version));
        memcpy(&device_type, props.bytes + 16, sizeof(device_type));
        /* accept every device kind (SDL3 only takes 1/2 and rejects lavapipe's CPU device) */
        if (api_version < VK_API_VERSION_1_0
            || (device_type != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                && device_type != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                && device_type != VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU
                && device_type != VK_PHYSICAL_DEVICE_TYPE_CPU)) {
            continue;
        }

        uint32_t dcount = 0;
        vkGetPhysicalDeviceDisplayPropertiesKHR(physical_devices[i], &dcount, NULL);
        if (dcount == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                        "SDLop KMS/Vulkan: physical device %u has no KMS displays, skipping", i);
            continue;
        }
        VkDisplayPropertiesKHR_ *cand_displays =
            (VkDisplayPropertiesKHR_ *)calloc(dcount, sizeof(*cand_displays));
        if (!cand_displays) {
            SDL_OutOfMemory();
            goto clean;
        }
        vkGetPhysicalDeviceDisplayPropertiesKHR(physical_devices[i], &dcount, cand_displays);
        VkDisplayKHR_ display0 = cand_displays[0].display;
        free(cand_displays);

        /* a plane that can drive display0 (bound to it, or unused) */
        uint32_t pcount = 0;
        vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical_devices[i], &pcount, NULL);
        bool plane_ok = false;
        if (pcount > 0) {
            VkDisplayPlanePropertiesKHR_ *pprops =
                (VkDisplayPlanePropertiesKHR_ *)calloc(pcount, sizeof(*pprops));
            if (!pprops) {
                SDL_OutOfMemory();
                goto clean;
            }
            vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical_devices[i], &pcount, pprops);
            for (uint32_t p = 0; p < pcount && !plane_ok; p++) {
                if (pprops[p].currentDisplay != display0 && pprops[p].currentDisplay != NULL) {
                    continue;
                }
                uint32_t supported_count = 0;
                vkGetDisplayPlaneSupportedDisplaysKHR(physical_devices[i], p, &supported_count, NULL);
                if (supported_count == 0) {
                    continue;
                }
                VkDisplayKHR_ *supported =
                    (VkDisplayKHR_ *)calloc(supported_count, sizeof(*supported));
                if (!supported) {
                    SDL_OutOfMemory();
                    free(pprops);
                    goto clean;
                }
                vkGetDisplayPlaneSupportedDisplaysKHR(physical_devices[i], p, &supported_count, supported);
                for (uint32_t s2 = 0; s2 < supported_count; s2++) {
                    if (supported[s2] == display0) {
                        plane_ok = true;
                        break;
                    }
                }
                free(supported);
            }
            free(pprops);
        }
        if (!plane_ok) {
            SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                        "SDLop KMS/Vulkan: physical device %u has no plane compatible with its display, skipping",
                        i);
            continue;
        }

        int score = 10;
        if (drm_fd_ok &&
            kmsdrmvk_device_matches_drm((VkInstance_)instance, physical_devices[i], drm_major, drm_minor)) {
            score += 100;
        }
        if (score > gpu_score) {
            gpu_score = score;
            gpu = physical_devices[i];
            gpu_index = i;
            gpu_api_version = api_version;
            gpu_device_type = device_type;
        }
    }
    if (!gpu) {
        SDL_SetError("Vulkan can't find a physical device with a KMS display and a compatible plane");
        goto clean;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                "SDLop KMS/Vulkan: using physical device %u (deviceType=%u apiVersion=%u.%u.%u%s)",
                gpu_index, gpu_device_type,
                (gpu_api_version >> 22) & 0x3ff,
                (gpu_api_version >> 12) & 0x3ff,
                gpu_api_version & 0xfff,
                gpu_score >= 110 ? ", DRM node matches" : "");

    uint32_t display_count = 0;
    vkGetPhysicalDeviceDisplayPropertiesKHR(gpu, &display_count, NULL);
    if (display_count == 0) {
        SDL_SetError("Vulkan can't find any displays on this device");
        goto clean;
    }
    display_props = (VkDisplayPropertiesKHR_ *)calloc(display_count, sizeof(*display_props));
    if (!display_props) {
        SDL_OutOfMemory();
        goto clean;
    }
    vkGetPhysicalDeviceDisplayPropertiesKHR(gpu, &display_count, display_props);

    /* SDLop drives a single display per DRM device: the Vulkan display list
     * is also single-entry in practice. Use index 0. */
    VkDisplayKHR_ display = display_props[0].display;
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "SDLop KMS/Vulkan: using display '%s'",
                display_props[0].displayName ? display_props[0].displayName : "?");

    uint32_t mode_count = 0;
    vkGetDisplayModePropertiesKHR(gpu, display, &mode_count, NULL);
    if (mode_count == 0) {
        SDL_SetError("Vulkan can't find any video modes for the display");
        goto clean;
    }
    mode_props = (VkDisplayModePropertiesKHR_ *)calloc(mode_count, sizeof(*mode_props));
    if (!mode_props) {
        SDL_OutOfMemory();
        goto clean;
    }
    vkGetDisplayModePropertiesKHR(gpu, display, &mode_count, mode_props);

    /* exact match for the window size wins; otherwise create a custom mode
     * (never pick a larger one: the scanout can't exceed the window) */
    VkDisplayModeKHR_ display_mode = NULL;
    for (uint32_t i = 0; i < mode_count; i++) {
        if (mode_props[i].parameters.visibleRegion.width == (uint32_t)window->w
            && mode_props[i].parameters.visibleRegion.height == (uint32_t)window->h) {
            display_mode = mode_props[i].displayMode;
            break;
        }
    }

    if (!display_mode) {
        KMSDRM_DeviceData *vid = KMSDRM_GetDevice();
        uint32_t refresh_mhz = 60000; /* fallback: 60 Hz */
        if (vid && vid->display.mode.clock != 0) {
            int mhz = KMSDRM_ModeRefreshMHz(&vid->display.mode);
            if (mhz > 0) {
                refresh_mhz = (uint32_t)mhz;
            }
        }

        VkDisplayModeCreateInfoKHR_ mode_info;
        memset(&mode_info, 0, sizeof(mode_info));
        mode_info.sType = VK_STRUCTURE_TYPE_DISPLAY_MODE_CREATE_INFO_KHR;
        mode_info.parameters.visibleRegion.width = (uint32_t)window->w;
        mode_info.parameters.visibleRegion.height = (uint32_t)window->h;
        mode_info.parameters.refreshRate = refresh_mhz;
        VkDisplayModeKHR_ new_mode = NULL;
        VkResult_ res = vkCreateDisplayModeKHR(gpu, display, &mode_info, NULL, &new_mode);
        if (res != VK_SUCCESS || !new_mode) {
            SDL_SetError("Vulkan could not find a predefined mode for %dx%d and could not create one (%d)",
                         window->w, window->h, (int)res);
            goto clean;
        }
        display_mode = new_mode;
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO,
                    "SDLop KMS/Vulkan: created %dx%d @%umHz display mode",
                    window->w, window->h, refresh_mhz);
    }

    uint32_t plane_count = 0;
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(gpu, &plane_count, NULL);
    if (plane_count == 0) {
        SDL_SetError("Vulkan can't find any display planes");
        goto clean;
    }
    plane_props = (VkDisplayPlanePropertiesKHR_ *)calloc(plane_count, sizeof(*plane_props));
    if (!plane_props) {
        SDL_OutOfMemory();
        goto clean;
    }
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(gpu, &plane_count, plane_props);

    /* prefer a plane bound to this display (or unused) that supports
     * per-pixel alpha */
    const uint32_t wanted_alpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    uint32_t plane = UINT32_MAX;
    for (uint32_t i = 0; i < plane_count; i++) {
        if (plane_props[i].currentDisplay != display && plane_props[i].currentDisplay != NULL) {
            continue;
        }

        uint32_t supported_count = 0;
        vkGetDisplayPlaneSupportedDisplaysKHR(gpu, i, &supported_count, NULL);
        if (supported_count == 0) {
            continue;
        }
        VkDisplayKHR_ *supported = (VkDisplayKHR_ *)calloc(supported_count, sizeof(*supported));
        if (!supported) {
            SDL_OutOfMemory();
            goto clean;
        }
        vkGetDisplayPlaneSupportedDisplaysKHR(gpu, i, &supported_count, supported);

        bool display_supported = false;
        for (uint32_t j = 0; j < supported_count; j++) {
            if (supported[j] == display) {
                display_supported = true;
                break;
            }
        }
        free(supported);
        if (!display_supported) {
            continue;
        }

        VkDisplayPlaneCapabilitiesKHR_ caps;
        memset(&caps, 0, sizeof(caps));
        if (vkGetDisplayPlaneCapabilitiesKHR(gpu, display_mode, i, &caps) != VK_SUCCESS) {
            continue;
        }
        /* supportedAlpha is a bitmask of VK_DISPLAY_PLANE_ALPHA_*_BIT_KHR;
         * a plane carrying opaque plus other modes must match too */
        if ((caps.supportedAlpha & wanted_alpha) != 0) {
            plane = i;
            break;
        }
    }

    if (plane == UINT32_MAX) {
        SDL_SetError("Vulkan couldn't find an appropriate display plane");
        goto clean;
    }

    VkDisplaySurfaceCreateInfoKHR_ surface_info;
    memset(&surface_info, 0, sizeof(surface_info));
    surface_info.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
    surface_info.displayMode = display_mode;
    surface_info.planeIndex = plane;
    surface_info.planeStackIndex = 0;
    surface_info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    surface_info.globalAlpha = 1.0f;
    surface_info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    surface_info.imageExtent.width = (uint32_t)window->w;
    surface_info.imageExtent.height = (uint32_t)window->h;

    VkSurfaceKHR_ vk_surface = 0;
    VkResult_ res = vkCreateDisplayPlaneSurfaceKHR((VkInstance_)instance, &surface_info,
                                                   allocator, &vk_surface);
    if (res != VK_SUCCESS) {
        SDL_SetError("vkCreateDisplayPlaneSurfaceKHR failed: %d", (int)res);
        goto clean;
    }

    *surface = (Uint64)vk_surface;
    ok = true;

clean:
    free(plane_props);
    free(mode_props);
    free(display_props);
    free(physical_devices);
    return ok;
}
