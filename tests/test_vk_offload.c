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
    SDL_unsetenv_unsafe("SDLOP_VULKAN_OFFLOAD");
    SDL_unsetenv_unsafe("SDLOP_VK_OFFLOAD");
    SDL_unsetenv_unsafe("SDLOP_OFFLOAD");
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_AUTO, "unset -> AUTO");

    SDL_setenv_unsafe("SDLOP_OFFLOAD", "0", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_NEVER, "SDLOP_OFFLOAD=0 -> NEVER");
    SDL_setenv_unsafe("SDLOP_OFFLOAD", "force", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_FORCE, "SDLOP_OFFLOAD=force -> FORCE");

    SDL_setenv_unsafe("SDLOP_VK_OFFLOAD", "off", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_NEVER, "SDLOP_VK_OFFLOAD overrides SDLOP_OFFLOAD");

    SDL_setenv_unsafe("SDLOP_VULKAN_OFFLOAD", "1", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_FORCE, "SDLOP_VULKAN_OFFLOAD overrides SDLOP_VK_OFFLOAD");

    SDL_setenv_unsafe("SDLOP_VULKAN_OFFLOAD", "no", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_NEVER, "no -> NEVER");
    SDL_setenv_unsafe("SDLOP_VULKAN_OFFLOAD", "auto", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_AUTO, "auto -> AUTO");

    SDL_unsetenv_unsafe("SDLOP_VULKAN_OFFLOAD");
    SDL_unsetenv_unsafe("SDLOP_VK_OFFLOAD");
    SDL_unsetenv_unsafe("SDLOP_OFFLOAD");

    /* the same options from code (SDL_SetHint) */
    CHECK(SDL_SetHint(SDLOP_HINT_VULKAN_OFFLOAD, "1"), "set hint: %s", SDL_GetError());
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_FORCE, "hint SDLOP_VULKAN_OFFLOAD=1 -> FORCE");
    SDL_ResetHint(SDLOP_HINT_VULKAN_OFFLOAD);
    SDL_SetHint(SDLOP_HINT_OFFLOAD, "off");
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_NEVER, "hint SDLOP_OFFLOAD=off -> NEVER");
    SDL_SetHint(SDLOP_HINT_VK_OFFLOAD, "force");
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_FORCE, "hint SDLOP_VK_OFFLOAD wins over hint SDLOP_OFFLOAD");
    /* SDL precedence: environment over a normal hint, override hint over both */
    SDL_setenv_unsafe("SDLOP_VK_OFFLOAD", "0", 1);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_NEVER, "environment wins over a normal hint");
    SDL_SetHintWithPriority(SDLOP_HINT_VK_OFFLOAD, "1", SDL_HINT_OVERRIDE);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_FORCE, "override hint wins over the environment");
    SDL_unsetenv_unsafe("SDLOP_VK_OFFLOAD");
    SDL_ResetHint(SDLOP_HINT_VK_OFFLOAD);
    SDL_ResetHint(SDLOP_HINT_OFFLOAD);
    CHECK(SDLOP_VulkanOffload_GetPolicy() == SDLOP_VK_OFFLOAD_AUTO, "reset hints -> AUTO");
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
    SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv_unsafe("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    SDL_setenv_unsafe("SDLOP_VULKAN_OFFLOAD", "0", 1);

    if (SDL_Init(SDL_INIT_VIDEO)) {
        Uint32 ext_count = 99;
        char const *const *exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
        CHECK(exts == NULL && ext_count == 0,
              "dummy GetInstanceExtensions returns NULL when SDLOP_VULKAN_OFFLOAD=0");

        SDL_Window *w = SDL_CreateWindow("dummy vk optout", 64, 64, SDL_WINDOW_VULKAN);
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

    SDL_unsetenv_unsafe("SDLOP_VULKAN_OFFLOAD");

    /* the policy chosen with SDL_SetHint alone must act exactly like the
     * environment variable (offload extensions where the loader has
     * VK_EXT_headless_surface, nothing otherwise) */
    if (SDL_Init(SDL_INIT_VIDEO)) {
        Uint32 n_env = 99, n = 99;
        SDL_setenv_unsafe("SDLOP_VULKAN_OFFLOAD", "1", 1);
        char const *const *exts = SDL_Vulkan_GetInstanceExtensions(&n_env);
        SDL_unsetenv_unsafe("SDLOP_VULKAN_OFFLOAD");
        SDL_SetHint(SDLOP_HINT_VULKAN_OFFLOAD, "1");
        exts = SDL_Vulkan_GetInstanceExtensions(&n);
        CHECK(n == n_env && (n == 0 || n == 2), "SDL_SetHint(SDLOP_VULKAN_OFFLOAD, 1): %u extensions, environment: %u", n, n_env);
        if (n_env == 0) {
            printf("test_vk_offload: hint end-to-end compared with the environment only (no VK_EXT_headless_surface)\n");
        }
        SDL_SetHint(SDLOP_HINT_VULKAN_OFFLOAD, "0");
        n = 99;
        exts = SDL_Vulkan_GetInstanceExtensions(&n);
        CHECK(exts == NULL && n == 0, "SDL_SetHint(SDLOP_VULKAN_OFFLOAD, 0): no extensions (got %u)", n);
        SDL_ResetHint(SDLOP_HINT_VULKAN_OFFLOAD);
        SDL_Vulkan_UnloadLibrary();
        SDL_Quit();
    }
    SDL_unsetenv_unsafe("SDL_VIDEODRIVER");
}

/* ------------------------------------------------------------------ */
/* In-process mock Vulkan driver testing all 4 offload audit fixes     */
/* ------------------------------------------------------------------ */

typedef void (*MockVkVoidFn)(void);
typedef MockVkVoidFn (*MockPFN_gipa)(void *inst, const char *name);
typedef MockVkVoidFn (*MockPFN_gdpa)(void *dev, const char *name);

typedef struct MockVkExtProps {
    char extensionName[256];
    uint32_t specVersion;
} MockVkExtProps;

typedef struct MockVkPhysProps {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    uint8_t limits[504];
    uint8_t sparseProperties[20];
} MockVkPhysProps;

typedef struct MockVkQFProps {
    uint32_t queueFlags;
    uint32_t queueCount;
    uint32_t timestampValidBits;
    struct { uint32_t width, height, depth; } minImageTransferGranularity;
} MockVkQFProps;

typedef struct MockVkMemProps {
    uint32_t memoryTypeCount;
    struct { uint32_t propertyFlags, heapIndex; } memoryTypes[32];
    uint32_t memoryHeapCount;
    struct { uint64_t size; uint32_t flags; } memoryHeaps[16];
} MockVkMemProps;

typedef struct MockVkSurfCaps {
    uint32_t minImageCount, maxImageCount;
    struct { uint32_t width, height; } currentExtent, minImageExtent, maxImageExtent;
    uint32_t maxImageArrayLayers;
    uint32_t supportedTransforms, currentTransform, supportedCompositeAlpha, supportedUsageFlags;
} MockVkSurfCaps;

typedef struct MockVkDevQueueCI {
    uint32_t sType;
    const void *pNext;
    uint32_t flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float *pQueuePriorities;
} MockVkDevQueueCI;

typedef struct MockVkDevQueueInfo2 {
    uint32_t sType;
    const void *pNext;
    uint32_t flags;
    uint32_t queueFamilyIndex;
    uint32_t queueIndex;
} MockVkDevQueueInfo2;

typedef struct MockVkDevCI {
    uint32_t sType;
    const void *pNext;
    uint32_t flags;
    uint32_t queueCreateInfoCount;
    const MockVkDevQueueCI *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} MockVkDevCI;

typedef struct MockVkSwapchainCI {
    uint32_t sType;
    const void *pNext;
    uint32_t flags;
    uint64_t surface;
    uint32_t minImageCount;
    uint32_t imageFormat;
    uint32_t imageColorSpace;
    struct { uint32_t width, height; } imageExtent;
    uint32_t imageArrayLayers;
    uint32_t imageUsage;
    uint32_t imageSharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    uint32_t preTransform, compositeAlpha, presentMode;
    uint32_t clipped;
    uint64_t oldSwapchain;
} MockVkSwapchainCI;

typedef struct MockVkPresentInfo {
    uint32_t sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const uint64_t *pWaitSemaphores;
    uint32_t swapchainCount;
    const uint64_t *pSwapchains;
    const uint32_t *pImageIndices;
    int *pResults;
} MockVkPresentInfo;

typedef struct MockVkSubmitInfo {
    uint32_t sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const uint64_t *pWaitSemaphores;
    const uint32_t *pWaitDstStageMask;
    uint32_t commandBufferCount;
    void *const *pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const uint64_t *pSignalSemaphores;
} MockVkSubmitInfo;

typedef struct MockVkImgBarrier {
    uint32_t sType;
    const void *pNext;
    uint32_t srcAccessMask, dstAccessMask;
    uint32_t oldLayout, newLayout;
    uint32_t srcQueueFamilyIndex, dstQueueFamilyIndex;
    uint64_t image;
    struct { uint32_t aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount; } subresourceRange;
} MockVkImgBarrier;

typedef struct MockVkCmd {
    uint32_t qf;
    int has_copy;
    int has_rel_qfot;
    int has_acq_qfot;
} MockVkCmd;

static struct {
    int null_inst_bad_lookup;
    int probe_created;
    int probe_destroyed;
    int present_qf_has_transfer;
    uint32_t supported_usage;
    uint32_t mem_flags;
    int expose_invalidate;
    int invalidate_calls;
    int host_barrier_seen;
    int multi_sc_suboptimal_second;
    int create_sc_calls;
    uint32_t last_sc_usage;
    int copy_seen;
    int copy_before_present;
    int present_seen;
    int qfot_rel_seen;
    int qfot_acq_seen;
    int double_consume;
    int wrong_qf;
    uint64_t last_present_sem;
    uint32_t last_present_wait_cnt;
    uint64_t sem_sig[64];
    uint64_t sem_wait[64];
    uint32_t pool_qf[64];
    MockVkCmd *pool_cmds[64][4];
    uint64_t next_h;
    uint8_t staging[64 * 48 * 4];
} mvk;

static int mvk_inst_obj, mvk_gpu_obj, mvk_dev_obj, mvk_q_pres_obj, mvk_q_xfer_obj, mvk_q_untracked_obj;

static int mvk_create_inst(const void *ci, const void *a, void **out) {
    (void)ci; (void)a; mvk.probe_created++; *out = &mvk_inst_obj; return 0;
}
static void mvk_destroy_inst(void *i, const void *a) {
    (void)i; (void)a; mvk.probe_destroyed++;
}
static int mvk_enum_exts(const char *l, uint32_t *c, MockVkExtProps *p) {
    (void)l;
    static const char *names[2] = { "VK_KHR_surface", "VK_EXT_headless_surface" };
    if (!p) { *c = 2; return 0; }
    for (uint32_t i = 0; i < 2 && i < *c; i++) {
        snprintf(p[i].extensionName, sizeof(p[i].extensionName), "%s", names[i]);
        p[i].specVersion = 1;
    }
    *c = (*c < 2) ? *c : 2;
    return 0;
}
static int mvk_enum_gpus(void *i, uint32_t *c, void **g) {
    (void)i;
    if (!g) { *c = 1; return 0; }
    if (*c >= 1) { g[0] = &mvk_gpu_obj; *c = 1; }
    return 0;
}
static void mvk_gpu_props(void *g, MockVkPhysProps *p) {
    (void)g;
    memset(p, 0, sizeof(*p));
    p->apiVersion = (1u << 22);
    p->deviceType = 2; /* DISCRETE_GPU */
    snprintf(p->deviceName, sizeof(p->deviceName), "Mock GPU");
}
static void mvk_qf_props(void *g, uint32_t *c, MockVkQFProps *p) {
    (void)g;
    if (mvk.present_qf_has_transfer) {
        if (!p) { *c = 1; return; }
        p[0].queueFlags = 1u | 4u; /* GRAPHICS | TRANSFER */
        p[0].queueCount = 1;
        *c = 1;
    } else {
        if (!p) { *c = 3; return; }
        p[0].queueFlags = 0; p[0].queueCount = 1;
        p[1].queueFlags = 0; p[1].queueCount = 1; /* present-only */
        p[2].queueFlags = 4u; p[2].queueCount = 1; /* TRANSFER */
        *c = 3;
    }
}
static void mvk_mem_props(void *g, MockVkMemProps *m) {
    (void)g;
    memset(m, 0, sizeof(*m));
    m->memoryTypeCount = 1;
    m->memoryTypes[0].propertyFlags = mvk.mem_flags ? mvk.mem_flags : (2u | 4u);
    m->memoryHeapCount = 1;
    m->memoryHeaps[0].size = 1024 * 1024;
}
static int mvk_create_headless_surf(void *i, const void *ci, const void *a, uint64_t *s) {
    (void)i; (void)ci; (void)a; *s = 0x1110u + mvk.next_h++; return 0;
}
static void mvk_destroy_surf(void *i, uint64_t s, const void *a) { (void)i;(void)s;(void)a; }
static int mvk_surf_caps(void *g, uint64_t s, MockVkSurfCaps *caps) {
    (void)g; (void)s;
    memset(caps, 0, sizeof(*caps));
    caps->minImageCount = 2; caps->maxImageCount = 4;
    caps->currentExtent.width = 64; caps->currentExtent.height = 48;
    caps->supportedUsageFlags = mvk.supported_usage;
    return 0;
}
static int mvk_create_dev(void *g, const MockVkDevCI *ci, const void *a, void **d) {
    (void)g; (void)ci; (void)a; *d = &mvk_dev_obj; return 0;
}
static void mvk_destroy_dev(void *d, const void *a) { (void)d;(void)a; }
static void mvk_get_q(void *d, uint32_t qf, uint32_t qi, void **q) {
    (void)d; (void)qi;
    if (mvk.present_qf_has_transfer) *q = &mvk_q_pres_obj;
    else *q = (qf == 1) ? &mvk_q_pres_obj : &mvk_q_xfer_obj;
}
static void mvk_get_q2(void *d, const MockVkDevQueueInfo2 *qi2, void **q) {
    mvk_get_q(d, qi2->queueFamilyIndex, qi2->queueIndex, q);
}
static int mvk_create_sc(void *d, const MockVkSwapchainCI *ci, const void *a, uint64_t *sc) {
    (void)d; (void)a;
    mvk.create_sc_calls++;
    mvk.last_sc_usage = ci->imageUsage;
    if ((ci->imageUsage & 1u) && !(mvk.supported_usage & 1u)) {
        return -3;
    }
    *sc = 0x2220u + mvk.next_h++;
    return 0;
}
static void mvk_destroy_sc(void *d, uint64_t sc, const void *a) { (void)d;(void)sc;(void)a; }
static int mvk_get_sc_imgs(void *d, uint64_t sc, uint32_t *c, uint64_t *imgs) {
    (void)d; (void)sc;
    if (!imgs) { *c = 2; return 0; }
    if (*c >= 1) imgs[0] = 0x3001u;
    if (*c >= 2) imgs[1] = 0x3002u;
    *c = (*c < 2) ? *c : 2;
    return 0;
}
static int mvk_create_buf(void *d, const void *ci, const void *a, uint64_t *b) {
    (void)d;(void)ci;(void)a; *b = mvk.next_h++; return 0;
}
static void mvk_destroy_buf(void *d, uint64_t b, const void *a) { (void)d;(void)b;(void)a; }
static void mvk_buf_mem_req(void *d, uint64_t b, uint64_t *req) {
    (void)d;(void)b; req[0] = 64 * 48 * 4; req[1] = 16; ((uint32_t *)&req[2])[0] = 1;
}
static int mvk_alloc_mem(void *d, const void *ai, const void *a, uint64_t *m) {
    (void)d;(void)ai;(void)a; *m = mvk.next_h++; return 0;
}
static void mvk_free_mem(void *d, uint64_t m, const void *a) { (void)d;(void)m;(void)a; }
static int mvk_bind_buf(void *d, uint64_t b, uint64_t m, uint64_t o) { (void)d;(void)b;(void)m;(void)o; return 0; }
static int mvk_map_mem(void *d, uint64_t m, uint64_t o, uint64_t sz, uint32_t f, void **pp) {
    (void)d;(void)m;(void)o;(void)sz;(void)f; *pp = mvk.staging; return 0;
}
static void mvk_unmap_mem(void *d, uint64_t m) { (void)d;(void)m; }
static int mvk_invalidate(void *d, uint32_t cnt, const void *ranges) {
    (void)d; (void)cnt; (void)ranges;
    mvk.invalidate_calls++;
    return 0;
}
static int mvk_create_cp(void *d, const void *ci, const void *a, uint64_t *cp) {
    (void)d; (void)a;
    uint64_t h = mvk.next_h++;
    const uint32_t *u = (const uint32_t *)ci;
    /* VkCommandPoolCreateInfo: sType(0), pad, pNext(ptr), flags, queueFamilyIndex */
    struct { uint32_t sType; const void *pNext; uint32_t flags, qf; } tmp;
    memcpy(&tmp, u, sizeof(tmp));
    if (h < 64) mvk.pool_qf[h] = tmp.qf;
    *cp = h;
    return 0;
}
static void mvk_destroy_cp(void *d, uint64_t cp, const void *a) {
    (void)d; (void)a;
    if (cp < 64) {
        for (int i = 0; i < 4; i++) { free(mvk.pool_cmds[cp][i]); mvk.pool_cmds[cp][i] = NULL; }
    }
}
static int mvk_alloc_cb(void *d, const void *ai, void **cbs) {
    (void)d;
    struct { uint32_t sType; const void *pNext; uint64_t pool; uint32_t level, count; } tmp;
    memcpy(&tmp, ai, sizeof(tmp));
    uint32_t qf = (tmp.pool < 64) ? mvk.pool_qf[tmp.pool] : 0;
    for (uint32_t i = 0; i < tmp.count; i++) {
        MockVkCmd *mc = (MockVkCmd *)calloc(1, sizeof(*mc));
        mc->qf = qf;
        if (tmp.pool < 64 && i < 4) mvk.pool_cmds[tmp.pool][i] = mc;
        cbs[i] = mc;
    }
    return 0;
}
static int mvk_reset_cb(void *cb, uint32_t f) {
    (void)f; MockVkCmd *mc = (MockVkCmd *)cb; uint32_t qf = mc->qf; memset(mc, 0, sizeof(*mc)); mc->qf = qf; return 0;
}
static int mvk_begin_cb(void *cb, const void *bi) { (void)cb;(void)bi; return 0; }
static int mvk_end_cb(void *cb) { (void)cb; return 0; }
static void mvk_barrier(void *cb, uint32_t s, uint32_t d, uint32_t df,
                        uint32_t mc, const void *mp, uint32_t bc, const void *bp,
                        uint32_t ic, const MockVkImgBarrier *ip) {
    (void)s;(void)df;(void)bc;(void)bp;
    if (mc > 0 && mp && (d & 0x00004000u)) {
        mvk.host_barrier_seen++;
    }
    MockVkCmd *cmd = (MockVkCmd *)cb;
    for (uint32_t i = 0; i < ic; i++) {
        if (ip[i].srcQueueFamilyIndex != ~0u && ip[i].dstQueueFamilyIndex != ~0u &&
            ip[i].srcQueueFamilyIndex != ip[i].dstQueueFamilyIndex) {
            if (cmd->qf == ip[i].srcQueueFamilyIndex) cmd->has_rel_qfot = 1;
            if (cmd->qf == ip[i].dstQueueFamilyIndex) cmd->has_acq_qfot = 1;
        }
    }
}
static void mvk_copy_img_to_buf(void *cb, uint64_t img, uint32_t lay, uint64_t buf, uint32_t rc, const void *r) {
    (void)img;(void)lay;(void)buf;(void)rc;(void)r;
    ((MockVkCmd *)cb)->has_copy = 1;
    for (size_t i = 0; i < 64u * 48u; i++) {
        mvk.staging[i * 4 + 0] = 102; mvk.staging[i * 4 + 1] = 204;
        mvk.staging[i * 4 + 2] = 51;  mvk.staging[i * 4 + 3] = 255;
    }
}
static int mvk_create_fence(void *d, const void *ci, const void *a, uint64_t *f) { (void)d;(void)ci;(void)a; *f = mvk.next_h++; return 0; }
static void mvk_destroy_fence(void *d, uint64_t f, const void *a) { (void)d;(void)f;(void)a; }
static int mvk_reset_fences(void *d, uint32_t c, const uint64_t *f) { (void)d;(void)c;(void)f; return 0; }
static int mvk_wait_fences(void *d, uint32_t c, const uint64_t *f, uint32_t all, uint64_t t) { (void)d;(void)c;(void)f;(void)all;(void)t; return 0; }
static int mvk_create_sem(void *d, const void *ci, const void *a, uint64_t *s) { (void)d;(void)ci;(void)a; *s = mvk.next_h++; return 0; }
static void mvk_destroy_sem(void *d, uint64_t s, const void *a) { (void)d;(void)s;(void)a; }
static int mvk_submit(void *q, uint32_t count, const MockVkSubmitInfo *sis, uint64_t f) {
    (void)f;
    uint32_t q_qf = (q == &mvk_q_pres_obj) ? (mvk.present_qf_has_transfer ? 0u : 1u) : 2u;
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t w = 0; w < sis[i].waitSemaphoreCount; w++) {
            uint64_t h = sis[i].pWaitSemaphores[w];
            if (h < 64) {
                if (mvk.sem_wait[h] >= mvk.sem_sig[h]) mvk.double_consume++;
                mvk.sem_wait[h]++;
            }
        }
        for (uint32_t s = 0; s < sis[i].signalSemaphoreCount; s++) {
            uint64_t h = sis[i].pSignalSemaphores[s];
            if (h < 64) mvk.sem_sig[h]++;
        }
        for (uint32_t c = 0; c < sis[i].commandBufferCount; c++) {
            MockVkCmd *mc = (MockVkCmd *)sis[i].pCommandBuffers[c];
            if (mc->qf != q_qf) mvk.wrong_qf++;
            if (mc->has_rel_qfot) mvk.qfot_rel_seen++;
            if (mc->has_acq_qfot) mvk.qfot_acq_seen++;
            if (mc->has_copy) {
                mvk.copy_seen++;
                if (mvk.copy_seen == mvk.present_seen + 1) mvk.copy_before_present++;
            }
        }
    }
    return 0;
}
static int mvk_present(void *q, const MockVkPresentInfo *pi) {
    (void)q;
    mvk.present_seen++;
    if (pi) {
        mvk.last_present_wait_cnt = pi->waitSemaphoreCount;
        mvk.last_present_sem = pi->waitSemaphoreCount ? pi->pWaitSemaphores[0] : 0;
        for (uint32_t w = 0; w < pi->waitSemaphoreCount; w++) {
            uint64_t h = pi->pWaitSemaphores[w];
            if (h < 64) {
                if (mvk.sem_wait[h] >= mvk.sem_sig[h]) mvk.double_consume++;
                mvk.sem_wait[h]++;
            }
        }
        if (pi->pResults) {
            for (uint32_t i = 0; i < pi->swapchainCount; i++) {
                pi->pResults[i] = (mvk.multi_sc_suboptimal_second && i == 1)
                    ? 1000001003 /* VK_SUBOPTIMAL_KHR */
                    : 0;
            }
        }
        if (mvk.multi_sc_suboptimal_second && pi->swapchainCount > 1) {
            return 1000001003;
        }
    }
    return 0;
}

static MockVkVoidFn mvk_gdpa(void *d, const char *n) {
    (void)d;
    if (!n) return NULL;
    if (strcmp(n, "vkDestroyDevice") == 0) return (MockVkVoidFn)mvk_destroy_dev;
    if (strcmp(n, "vkGetDeviceQueue") == 0) return (MockVkVoidFn)mvk_get_q;
    if (strcmp(n, "vkGetDeviceQueue2") == 0) return (MockVkVoidFn)mvk_get_q2;
    if (strcmp(n, "vkCreateSwapchainKHR") == 0) return (MockVkVoidFn)mvk_create_sc;
    if (strcmp(n, "vkDestroySwapchainKHR") == 0) return (MockVkVoidFn)mvk_destroy_sc;
    if (strcmp(n, "vkGetSwapchainImagesKHR") == 0) return (MockVkVoidFn)mvk_get_sc_imgs;
    if (strcmp(n, "vkCreateBuffer") == 0) return (MockVkVoidFn)mvk_create_buf;
    if (strcmp(n, "vkDestroyBuffer") == 0) return (MockVkVoidFn)mvk_destroy_buf;
    if (strcmp(n, "vkGetBufferMemoryRequirements") == 0) return (MockVkVoidFn)mvk_buf_mem_req;
    if (strcmp(n, "vkAllocateMemory") == 0) return (MockVkVoidFn)mvk_alloc_mem;
    if (strcmp(n, "vkFreeMemory") == 0) return (MockVkVoidFn)mvk_free_mem;
    if (strcmp(n, "vkBindBufferMemory") == 0) return (MockVkVoidFn)mvk_bind_buf;
    if (strcmp(n, "vkMapMemory") == 0) return (MockVkVoidFn)mvk_map_mem;
    if (strcmp(n, "vkUnmapMemory") == 0) return (MockVkVoidFn)mvk_unmap_mem;
    if (strcmp(n, "vkInvalidateMappedMemoryRanges") == 0) {
        return mvk.expose_invalidate ? (MockVkVoidFn)mvk_invalidate : NULL;
    }
    if (strcmp(n, "vkCreateCommandPool") == 0) return (MockVkVoidFn)mvk_create_cp;
    if (strcmp(n, "vkDestroyCommandPool") == 0) return (MockVkVoidFn)mvk_destroy_cp;
    if (strcmp(n, "vkAllocateCommandBuffers") == 0) return (MockVkVoidFn)mvk_alloc_cb;
    if (strcmp(n, "vkResetCommandBuffer") == 0) return (MockVkVoidFn)mvk_reset_cb;
    if (strcmp(n, "vkBeginCommandBuffer") == 0) return (MockVkVoidFn)mvk_begin_cb;
    if (strcmp(n, "vkEndCommandBuffer") == 0) return (MockVkVoidFn)mvk_end_cb;
    if (strcmp(n, "vkCmdPipelineBarrier") == 0) return (MockVkVoidFn)mvk_barrier;
    if (strcmp(n, "vkCmdCopyImageToBuffer") == 0) return (MockVkVoidFn)mvk_copy_img_to_buf;
    if (strcmp(n, "vkCreateFence") == 0) return (MockVkVoidFn)mvk_create_fence;
    if (strcmp(n, "vkDestroyFence") == 0) return (MockVkVoidFn)mvk_destroy_fence;
    if (strcmp(n, "vkResetFences") == 0) return (MockVkVoidFn)mvk_reset_fences;
    if (strcmp(n, "vkWaitForFences") == 0) return (MockVkVoidFn)mvk_wait_fences;
    if (strcmp(n, "vkCreateSemaphore") == 0) return (MockVkVoidFn)mvk_create_sem;
    if (strcmp(n, "vkDestroySemaphore") == 0) return (MockVkVoidFn)mvk_destroy_sem;
    if (strcmp(n, "vkQueueSubmit") == 0) return (MockVkVoidFn)mvk_submit;
    if (strcmp(n, "vkQueuePresentKHR") == 0) return (MockVkVoidFn)mvk_present;
    return NULL;
}

static MockVkVoidFn mvk_gipa(void *inst, const char *n) {
    if (!n) return NULL;
    if (!inst) {
        if (strcmp(n, "vkGetInstanceProcAddr") == 0) return (MockVkVoidFn)mvk_gipa;
        if (strcmp(n, "vkCreateInstance") == 0) return (MockVkVoidFn)mvk_create_inst;
        if (strcmp(n, "vkEnumerateInstanceExtensionProperties") == 0) return (MockVkVoidFn)mvk_enum_exts;
        if (strcmp(n, "vkEnumerateInstanceLayerProperties") == 0 ||
            strcmp(n, "vkEnumerateInstanceVersion") == 0) return NULL;
        mvk.null_inst_bad_lookup++;
        return NULL;
    }
    if (strcmp(n, "vkGetInstanceProcAddr") == 0) return (MockVkVoidFn)mvk_gipa;
    if (strcmp(n, "vkDestroyInstance") == 0) return (MockVkVoidFn)mvk_destroy_inst;
    if (strcmp(n, "vkEnumeratePhysicalDevices") == 0) return (MockVkVoidFn)mvk_enum_gpus;
    if (strcmp(n, "vkGetPhysicalDeviceProperties") == 0) return (MockVkVoidFn)mvk_gpu_props;
    if (strcmp(n, "vkGetPhysicalDeviceQueueFamilyProperties") == 0) return (MockVkVoidFn)mvk_qf_props;
    if (strcmp(n, "vkGetPhysicalDeviceMemoryProperties") == 0) return (MockVkVoidFn)mvk_mem_props;
    if (strcmp(n, "vkCreateHeadlessSurfaceEXT") == 0) return (MockVkVoidFn)mvk_create_headless_surf;
    if (strcmp(n, "vkDestroySurfaceKHR") == 0) return (MockVkVoidFn)mvk_destroy_surf;
    if (strcmp(n, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) return (MockVkVoidFn)mvk_surf_caps;
    if (strcmp(n, "vkCreateDevice") == 0) return (MockVkVoidFn)mvk_create_dev;
    if (strcmp(n, "vkGetDeviceProcAddr") == 0) return (MockVkVoidFn)mvk_gdpa;
    return mvk_gdpa(NULL, n);
}

static void run_mock_vk_case(const char *label, const char *policy,
                             int present_qf_has_transfer, uint32_t supported_usage,
                             int use_get_queue2, uint32_t sharing_mode, int expect_sc_fail)
{
    memset(&mvk, 0, sizeof(mvk));
    mvk.present_qf_has_transfer = present_qf_has_transfer;
    mvk.supported_usage = supported_usage;
    mvk.next_h = 1;

    SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv_unsafe("SDLOP_DISABLE_RAW_INPUT", "1", 1);
    SDL_setenv_unsafe("SDLOP_VULKAN_OFFLOAD", policy, 1);
    CHECK(SDL_Init(SDL_INIT_VIDEO), "%s: SDL_Init", label);

    MockPFN_gipa gipa = (MockPFN_gipa)SDLOP_VulkanOffload_WrapGetInstanceProcAddr(
        (SDL_FunctionPointer)mvk_gipa);
    CHECK(gipa != NULL, "%s: wrap gipa", label);
    CHECK(SDLOP_VulkanOffload_ShouldUseOffload((SDL_FunctionPointer)mvk_gipa),
          "%s: ShouldUseOffload", label);
    CHECK(mvk.null_inst_bad_lookup == 0,
          "%s: real gipa(NULL, non-global) called %d times", label, mvk.null_inst_bad_lookup);
    if (strcmp(policy, "auto") == 0) {
        CHECK(mvk.probe_created == 1 && mvk.probe_destroyed == 1,
              "%s: AUTO probe created=%d destroyed=%d", label, mvk.probe_created, mvk.probe_destroyed);
    }
    CHECK(gipa(NULL, "vkDestroyInstance") == NULL,
          "%s: wrapped gipa(NULL, vkDestroyInstance) must return NULL", label);

    typedef int (*PFN_ci)(const void *, const void *, void **);
    typedef void (*PFN_di)(void *, const void *);
    typedef int (*PFN_epd)(void *, uint32_t *, void **);
    typedef int (*PFN_caps)(void *, uint64_t, MockVkSurfCaps *);
    typedef int (*PFN_cd)(void *, const MockVkDevCI *, const void *, void **);
    typedef void (*PFN_dd)(void *, const void *);
    typedef void (*PFN_gq)(void *, uint32_t, uint32_t, void **);
    typedef void (*PFN_gq2)(void *, const MockVkDevQueueInfo2 *, void **);
    typedef int (*PFN_csc)(void *, const MockVkSwapchainCI *, const void *, uint64_t *);
    typedef void (*PFN_dsc)(void *, uint64_t, const void *);
    typedef int (*PFN_pres)(void *, const MockVkPresentInfo *);

    void *inst = NULL;
    CHECK(((PFN_ci)gipa(NULL, "vkCreateInstance"))(NULL, NULL, &inst) == 0 && inst, "%s: ci", label);
    PFN_di p_di = (PFN_di)gipa(inst, "vkDestroyInstance");
    PFN_epd p_epd = (PFN_epd)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_caps p_caps = (PFN_caps)gipa(inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_cd p_cd = (PFN_cd)gipa(inst, "vkCreateDevice");
    MockPFN_gdpa gdpa = (MockPFN_gdpa)gipa(inst, "vkGetDeviceProcAddr");

    SDL_Window *w = SDL_CreateWindow(label, 64, 48, 0);
    CHECK(w != NULL, "%s: win", label);
    Uint64 surf = 0;
    CHECK(SDLOP_VulkanOffload_CreateSurface((SDL_FunctionPointer)mvk_gipa, w, inst, NULL, &surf),
          "%s: CreateSurface", label);

    void *gpu = NULL;
    uint32_t gc = 1;
    CHECK(p_epd(inst, &gc, &gpu) == 0 && gpu, "%s: gpu", label);

    MockVkSurfCaps caps;
    CHECK(p_caps(gpu, surf, &caps) == 0, "%s: caps", label);
    CHECK(caps.supportedUsageFlags == supported_usage,
          "%s: caps usage=0x%x != 0x%x", label, caps.supportedUsageFlags, supported_usage);

    float prio = 1.0f;
    MockVkDevQueueCI qcis[2];
    uint32_t qci_cnt = 0;
    uint32_t pres_qf = 0;
    if (present_qf_has_transfer) {
        qcis[0] = (MockVkDevQueueCI){ 2, NULL, 0, 0, 1, &prio };
        qci_cnt = 1; pres_qf = 0;
    } else {
        qcis[0] = (MockVkDevQueueCI){ 2, NULL, 0, 1, 1, &prio };
        qcis[1] = (MockVkDevQueueCI){ 2, NULL, 0, 2, 1, &prio };
        qci_cnt = 2; pres_qf = 1;
    }
    MockVkDevCI dci = { 3, NULL, 0, qci_cnt, qcis, 0, NULL, 0, NULL, NULL };
    void *dev = NULL;
    CHECK(p_cd(gpu, &dci, NULL, &dev) == 0 && dev, "%s: dev", label);

    PFN_dd p_dd = (PFN_dd)gdpa(dev, "vkDestroyDevice");
    PFN_gq p_gq = (PFN_gq)gdpa(dev, "vkGetDeviceQueue");
    PFN_gq2 p_gq2 = (PFN_gq2)gdpa(dev, "vkGetDeviceQueue2");
    PFN_csc p_csc = (PFN_csc)gdpa(dev, "vkCreateSwapchainKHR");
    PFN_dsc p_dsc = (PFN_dsc)gdpa(dev, "vkDestroySwapchainKHR");
    PFN_pres p_pres = (PFN_pres)gdpa(dev, "vkQueuePresentKHR");

    void *pres_q = NULL;
    if (use_get_queue2) {
        MockVkDevQueueInfo2 qi2 = { 1000145003u, NULL, 0, pres_qf, 0 };
        p_gq2(dev, &qi2, &pres_q);
    } else {
        p_gq(dev, pres_qf, 0, &pres_q);
    }

    uint32_t conc_qfs[2] = { 1, 2 };
    MockVkSwapchainCI sci;
    memset(&sci, 0, sizeof(sci));
    sci.surface = surf;
    sci.minImageCount = 2;
    sci.imageFormat = 44; /* B8G8R8A8_UNORM */
    sci.imageExtent.width = 64; sci.imageExtent.height = 48;
    sci.imageArrayLayers = 1;
    sci.imageUsage = 0x10u; /* COLOR_ATTACHMENT_BIT */
    sci.imageSharingMode = sharing_mode;
    sci.queueFamilyIndexCount = (sharing_mode == 1u) ? 2u : 0u;
    sci.pQueueFamilyIndices = (sharing_mode == 1u) ? conc_qfs : NULL;

    uint64_t sc = 0;
    int sc_res = p_csc(dev, &sci, NULL, &sc);
    if (expect_sc_fail) {
        CHECK(sc_res != 0 && sc == 0 && mvk.create_sc_calls == 0,
              "%s: rejected unsupported TRANSFER_SRC_BIT without retry (res=%d calls=%d)",
              label, sc_res, mvk.create_sc_calls);
        goto cleanup;
    }
    CHECK(sc_res == 0 && sc != 0 && (mvk.last_sc_usage & 1u) != 0, "%s: create sc", label);

    uint64_t app_sem = mvk.next_h++;
    for (int f = 0; f < 2; f++) {
        mvk.sem_sig[app_sem]++;
        uint32_t idx = 0;
        MockVkPresentInfo pi = { 1000001001u, NULL, 1, &app_sem, 1, &sc, &idx, NULL };
        CHECK(p_pres(pres_q, &pi) == 0, "%s: present %d", label, f);
    }
    CHECK(mvk.copy_seen == 2 && mvk.copy_before_present == 2,
          "%s: copy=%d before_present=%d", label, mvk.copy_seen, mvk.copy_before_present);
    CHECK(mvk.double_consume == 0 && mvk.wrong_qf == 0,
          "%s: double_consume=%d wrong_qf=%d", label, mvk.double_consume, mvk.wrong_qf);
    CHECK(mvk.last_present_wait_cnt == 1 && mvk.last_present_sem != app_sem,
          "%s: present waits on internal semaphore", label);
    if (!present_qf_has_transfer && sharing_mode == 0u) {
        CHECK(mvk.qfot_rel_seen == 4 && mvk.qfot_acq_seen == 4,
              "%s: QFOT rel=%d acq=%d", label, mvk.qfot_rel_seen, mvk.qfot_acq_seen);
    } else {
        CHECK(mvk.qfot_rel_seen == 0 && mvk.qfot_acq_seen == 0,
              "%s: no QFOT needed (rel=%d acq=%d)", label, mvk.qfot_rel_seen, mvk.qfot_acq_seen);
    }
    SDL_Surface *fb = SDL_GetWindowSurface(w);
    CHECK(fb && fb->pixels, "%s: window fb", label);
    if (fb && fb->pixels) {
        uint32_t px = *(const uint32_t *)fb->pixels;
        CHECK(((px >> 16) & 0xFFu) == 51 && ((px >> 8) & 0xFFu) == 204 && (px & 0xFFu) == 102,
              "%s: fb pixel mismatch 0x%08x", label, px);
    }
    p_dsc(dev, sc, NULL);
cleanup:
    p_dd(dev, NULL);
    SDLOP_VulkanOffload_NotifyDestroySurface(surf);
    p_di(inst, NULL);
    SDLOP_VulkanOffload_Shutdown();
    SDL_DestroyWindow(w);
    SDL_Quit();
    SDL_unsetenv_unsafe("SDLOP_VULKAN_OFFLOAD");
    SDL_unsetenv_unsafe("SDL_VIDEODRIVER");
}

static void run_mock_vk_edge_cases(void)
{
    typedef int (*PFN_ci)(const void *, const void *, void **);
    typedef void (*PFN_di)(void *, const void *);
    typedef int (*PFN_epd)(void *, uint32_t *, void **);
    typedef int (*PFN_cd)(void *, const MockVkDevCI *, const void *, void **);
    typedef void (*PFN_dd)(void *, const void *);
    typedef void (*PFN_gq)(void *, uint32_t, uint32_t, void **);
    typedef int (*PFN_csc)(void *, const MockVkSwapchainCI *, const void *, uint64_t *);
    typedef void (*PFN_dsc)(void *, uint64_t, const void *);
    typedef int (*PFN_pres)(void *, const MockVkPresentInfo *);

    /* Case A & B: Non-coherent HOST_VISIBLE memory (with and without invalidate fn) */
    for (int has_inv = 0; has_inv <= 1; has_inv++) {
        memset(&mvk, 0, sizeof(mvk));
        mvk.present_qf_has_transfer = 1;
        mvk.supported_usage = 0x11u;
        mvk.mem_flags = 2u; /* HOST_VISIBLE only, no HOST_COHERENT */
        mvk.expose_invalidate = has_inv;
        mvk.next_h = 1;

        SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
        SDL_setenv_unsafe("SDLOP_DISABLE_RAW_INPUT", "1", 1);
        SDL_setenv_unsafe("SDLOP_VULKAN_OFFLOAD", "force", 1);
        CHECK(SDL_Init(SDL_INIT_VIDEO), "noncoherent(%d): SDL_Init", has_inv);

        MockPFN_gipa gipa = (MockPFN_gipa)SDLOP_VulkanOffload_WrapGetInstanceProcAddr(
            (SDL_FunctionPointer)mvk_gipa);
        CHECK(SDLOP_VulkanOffload_ShouldUseOffload((SDL_FunctionPointer)mvk_gipa), "nc offload");
        void *inst = NULL;
        CHECK(((PFN_ci)gipa(NULL, "vkCreateInstance"))(NULL, NULL, &inst) == 0, "nc ci");
        PFN_di p_di = (PFN_di)gipa(inst, "vkDestroyInstance");
        PFN_epd p_epd = (PFN_epd)gipa(inst, "vkEnumeratePhysicalDevices");
        PFN_cd p_cd = (PFN_cd)gipa(inst, "vkCreateDevice");
        MockPFN_gdpa gdpa = (MockPFN_gdpa)gipa(inst, "vkGetDeviceProcAddr");

        SDL_Window *w = SDL_CreateWindow("nc", 64, 48, 0);
        Uint64 surf = 0;
        CHECK(SDLOP_VulkanOffload_CreateSurface((SDL_FunctionPointer)mvk_gipa, w, inst, NULL, &surf), "nc surf");
        void *gpu = NULL; uint32_t gc = 1;
        CHECK(p_epd(inst, &gc, &gpu) == 0, "nc gpu");
        float prio = 1.0f;
        MockVkDevQueueCI qci = { 2, NULL, 0, 0, 1, &prio };
        MockVkDevCI dci = { 3, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL };
        void *dev = NULL;
        CHECK(p_cd(gpu, &dci, NULL, &dev) == 0, "nc dev");

        PFN_dd p_dd = (PFN_dd)gdpa(dev, "vkDestroyDevice");
        PFN_gq p_gq = (PFN_gq)gdpa(dev, "vkGetDeviceQueue");
        PFN_csc p_csc = (PFN_csc)gdpa(dev, "vkCreateSwapchainKHR");
        PFN_dsc p_dsc = (PFN_dsc)gdpa(dev, "vkDestroySwapchainKHR");
        PFN_pres p_pres = (PFN_pres)gdpa(dev, "vkQueuePresentKHR");
        void *pres_q = NULL;
        p_gq(dev, 0, 0, &pres_q);

        MockVkSwapchainCI sci;
        memset(&sci, 0, sizeof(sci));
        sci.surface = surf; sci.minImageCount = 2; sci.imageFormat = 44;
        sci.imageExtent.width = 64; sci.imageExtent.height = 48;
        sci.imageArrayLayers = 1; sci.imageUsage = 0x10u;

        uint64_t sc = 0;
        CHECK(p_csc(dev, &sci, NULL, &sc) == 0, "nc sc");
        uint64_t app_sem = mvk.next_h++;
        mvk.sem_sig[app_sem]++;
        uint32_t idx = 0;
        MockVkPresentInfo pi = { 1000001001u, NULL, 1, &app_sem, 1, &sc, &idx, NULL };
        CHECK(p_pres(pres_q, &pi) == 0, "nc present");

        if (has_inv) {
            CHECK(mvk.invalidate_calls == 1 && mvk.host_barrier_seen == 1 && mvk.copy_seen == 1,
                  "noncoherent(1): inv=%d barrier=%d copy=%d",
                  mvk.invalidate_calls, mvk.host_barrier_seen, mvk.copy_seen);
            SDL_Surface *fb = SDL_GetWindowSurface(w);
            CHECK(fb && fb->pixels, "nc fb");
            if (fb && fb->pixels) {
                uint32_t px = *(const uint32_t *)fb->pixels;
                CHECK(((px >> 16) & 0xFFu) == 51 && ((px >> 8) & 0xFFu) == 204 && (px & 0xFFu) == 102,
                      "nc fb pixel mismatch 0x%08x", px);
            }
        } else {
            CHECK(mvk.copy_seen == 0 && mvk.invalidate_calls == 0 &&
                  mvk.last_present_sem == app_sem && mvk.double_consume == 0,
                  "noncoherent(0): refused staging (copy=%d inv=%d sem=%llu/%llu)",
                  mvk.copy_seen, mvk.invalidate_calls,
                  (unsigned long long)mvk.last_present_sem, (unsigned long long)app_sem);
        }

        p_dsc(dev, sc, NULL);
        p_dd(dev, NULL);
        SDLOP_VulkanOffload_NotifyDestroySurface(surf);
        p_di(inst, NULL);
        SDLOP_VulkanOffload_Shutdown();
        SDL_DestroyWindow(w);
        SDL_Quit();
        SDL_unsetenv_unsafe("SDLOP_VULKAN_OFFLOAD");
        SDL_unsetenv_unsafe("SDL_VIDEODRIVER");
    }

    /* Case C & D: Untracked present queue refused + multi-swapchain present chaining & pResults */
    {
        memset(&mvk, 0, sizeof(mvk));
        mvk.present_qf_has_transfer = 1;
        mvk.supported_usage = 0x11u;
        mvk.mem_flags = 2u | 4u;
        mvk.multi_sc_suboptimal_second = 1;
        mvk.next_h = 1;

        SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
        SDL_setenv_unsafe("SDLOP_DISABLE_RAW_INPUT", "1", 1);
        SDL_setenv_unsafe("SDLOP_VULKAN_OFFLOAD", "force", 1);
        CHECK(SDL_Init(SDL_INIT_VIDEO), "multi/untracked: SDL_Init");

        MockPFN_gipa gipa = (MockPFN_gipa)SDLOP_VulkanOffload_WrapGetInstanceProcAddr(
            (SDL_FunctionPointer)mvk_gipa);
        CHECK(SDLOP_VulkanOffload_ShouldUseOffload((SDL_FunctionPointer)mvk_gipa), "multi offload");
        void *inst = NULL;
        CHECK(((PFN_ci)gipa(NULL, "vkCreateInstance"))(NULL, NULL, &inst) == 0, "m ci");
        PFN_di p_di = (PFN_di)gipa(inst, "vkDestroyInstance");
        PFN_epd p_epd = (PFN_epd)gipa(inst, "vkEnumeratePhysicalDevices");
        PFN_cd p_cd = (PFN_cd)gipa(inst, "vkCreateDevice");
        MockPFN_gdpa gdpa = (MockPFN_gdpa)gipa(inst, "vkGetDeviceProcAddr");

        SDL_Window *w1 = SDL_CreateWindow("m1", 64, 48, 0);
        SDL_Window *w2 = SDL_CreateWindow("m2", 64, 48, 0);
        Uint64 surf1 = 0, surf2 = 0;
        CHECK(SDLOP_VulkanOffload_CreateSurface((SDL_FunctionPointer)mvk_gipa, w1, inst, NULL, &surf1), "m s1");
        CHECK(SDLOP_VulkanOffload_CreateSurface((SDL_FunctionPointer)mvk_gipa, w2, inst, NULL, &surf2), "m s2");

        void *gpu = NULL; uint32_t gc = 1;
        CHECK(p_epd(inst, &gc, &gpu) == 0, "m gpu");
        float prio = 1.0f;
        MockVkDevQueueCI qci = { 2, NULL, 0, 0, 1, &prio };
        MockVkDevCI dci = { 3, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL };
        void *dev = NULL;
        CHECK(p_cd(gpu, &dci, NULL, &dev) == 0, "m dev");

        PFN_dd p_dd = (PFN_dd)gdpa(dev, "vkDestroyDevice");
        PFN_gq p_gq = (PFN_gq)gdpa(dev, "vkGetDeviceQueue");
        PFN_csc p_csc = (PFN_csc)gdpa(dev, "vkCreateSwapchainKHR");
        PFN_dsc p_dsc = (PFN_dsc)gdpa(dev, "vkDestroySwapchainKHR");
        PFN_pres p_pres = (PFN_pres)gdpa(dev, "vkQueuePresentKHR");
        void *pres_q = NULL;
        p_gq(dev, 0, 0, &pres_q);

        MockVkSwapchainCI sci;
        memset(&sci, 0, sizeof(sci));
        sci.surface = surf1; sci.minImageCount = 2; sci.imageFormat = 44;
        sci.imageExtent.width = 64; sci.imageExtent.height = 48;
        sci.imageArrayLayers = 1; sci.imageUsage = 0x10u;
        uint64_t sc1 = 0, sc2 = 0;
        CHECK(p_csc(dev, &sci, NULL, &sc1) == 0, "m sc1");
        sci.surface = surf2;
        CHECK(p_csc(dev, &sci, NULL, &sc2) == 0, "m sc2");

        /* Case C: untracked present queue must be refused cleanly */
        uint64_t app_sem1 = mvk.next_h++;
        mvk.sem_sig[app_sem1]++;
        uint32_t idx0 = 0;
        MockVkPresentInfo pi_untracked = { 1000001001u, NULL, 1, &app_sem1, 1, &sc1, &idx0, NULL };
        CHECK(p_pres(&mvk_q_untracked_obj, &pi_untracked) == 0, "untracked pres");
        CHECK(mvk.copy_seen == 0 && mvk.last_present_sem == app_sem1 && mvk.double_consume == 0,
              "untracked queue refused readback (copy=%d sem=%llu/%llu)",
              mvk.copy_seen, (unsigned long long)mvk.last_present_sem, (unsigned long long)app_sem1);

        /* Case D: multi-swapchain present chains both readbacks & preserves pResults */
        uint64_t app_sem2 = mvk.next_h++;
        mvk.sem_sig[app_sem2]++;
        uint64_t scs[2] = { sc1, sc2 };
        uint32_t idxs[2] = { 0, 1 };
        int results[2] = { -99, -99 };
        MockVkPresentInfo pi_multi = { 1000001001u, NULL, 1, &app_sem2, 2, scs, idxs, results };
        int pres_res = p_pres(pres_q, &pi_multi);
        CHECK(pres_res == 1000001003, "multi pres res=%d", pres_res);
        CHECK(results[0] == 0 && results[1] == 1000001003,
              "multi pResults=[%d, %d]", results[0], results[1]);
        CHECK(mvk.copy_seen == 2 && mvk.double_consume == 0 && mvk.wrong_qf == 0 &&
              mvk.last_present_wait_cnt == 1 && mvk.last_present_sem != app_sem2,
              "multi chain copy=%d dc=%d wqf=%d", mvk.copy_seen, mvk.double_consume, mvk.wrong_qf);

        SDL_Surface *fb1 = SDL_GetWindowSurface(w1);
        SDL_Surface *fb2 = SDL_GetWindowSurface(w2);
        CHECK(fb1 && fb1->pixels && fb2 && fb2->pixels, "multi fbs");

        p_dsc(dev, sc1, NULL);
        p_dsc(dev, sc2, NULL);
        p_dd(dev, NULL);
        SDLOP_VulkanOffload_NotifyDestroySurface(surf1);
        SDLOP_VulkanOffload_NotifyDestroySurface(surf2);
        p_di(inst, NULL);
        SDLOP_VulkanOffload_Shutdown();
        SDL_DestroyWindow(w1);
        SDL_DestroyWindow(w2);
        SDL_Quit();
        SDL_unsetenv_unsafe("SDLOP_VULKAN_OFFLOAD");
        SDL_unsetenv_unsafe("SDL_VIDEODRIVER");
    }
}

/* with a loader that has VK_EXT_headless_surface (the mock), SDL_SetHint
 * alone switches the offload decision */
static void run_mock_vk_hint_case(void)
{
    memset(&mvk, 0, sizeof(mvk));
    mvk.present_qf_has_transfer = 1;
    mvk.supported_usage = 0x11u;
    mvk.next_h = 1;
    SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
    SDL_unsetenv_unsafe("SDLOP_VULKAN_OFFLOAD");
    SDL_unsetenv_unsafe("SDLOP_VK_OFFLOAD");
    SDL_unsetenv_unsafe("SDLOP_OFFLOAD");
    CHECK(SDL_Init(SDL_INIT_VIDEO), "hint case: SDL_Init");
    SDL_SetHint(SDLOP_HINT_VULKAN_OFFLOAD, "1");
    CHECK(SDLOP_VulkanOffload_ShouldUseOffload((SDL_FunctionPointer)mvk_gipa),
          "hint SDLOP_VULKAN_OFFLOAD=1 enables offload");
    Uint32 n = 0;
    CHECK(SDLOP_VulkanOffload_GetInstanceExtensions(&n) && n == 2, "hint case: offload extensions (%u)", n);
    SDL_SetHint(SDLOP_HINT_VULKAN_OFFLOAD, "0");
    CHECK(!SDLOP_VulkanOffload_ShouldUseOffload((SDL_FunctionPointer)mvk_gipa),
          "hint SDLOP_VULKAN_OFFLOAD=0 disables offload");
    SDL_SetHint(SDLOP_HINT_OFFLOAD, "force");
    SDL_ResetHint(SDLOP_HINT_VULKAN_OFFLOAD);
    CHECK(SDLOP_VulkanOffload_ShouldUseOffload((SDL_FunctionPointer)mvk_gipa),
          "hint SDLOP_OFFLOAD=force enables offload");
    SDL_ResetHint(SDLOP_HINT_OFFLOAD);
    SDLOP_VulkanOffload_Shutdown();
    SDL_Quit();
    SDL_unsetenv_unsafe("SDL_VIDEODRIVER");
}

static void test_mock_vulkan_offload(void)
{
    run_mock_vk_hint_case();
    run_mock_vk_case("auto_probe_single_qf", "auto", 1, 0x11u, 0, 0u, 0);
    run_mock_vk_case("qfot_get_queue2_qf1_qf2", "force", 0, 0x11u, 1, 0u, 0);
    run_mock_vk_case("concurrent_skip_qfot", "force", 0, 0x11u, 1, 1u, 0);
    run_mock_vk_case("reject_no_transfer_src", "force", 1, 0x10u, 0, 0u, 1);
    run_mock_vk_edge_cases();
}

int main(void)
{
    test_policy();
    test_extensions_layout();
    test_dummy_opt_out();
    test_mock_vulkan_offload();

    if (failures) {
        printf("test_vk_offload: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_vk_offload: PASS\n");
    return 0;
}
