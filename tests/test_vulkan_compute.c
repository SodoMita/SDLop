/*
  SDLop test: Vulkan compute pipeline on lavapipe.

  No window or swapchain - exercises the GPU API directly:
    - device + queue creation (SDL_Vulkan_LoadLibrary loader)
    - storage buffer + descriptor set
    - compute pipeline from embedded SPIR-V
    - dispatch + fence + host read-back, verifying every written value

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>

#include "shaders/compute_fill.inc"

#define N 256

static int failures = 0;

#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);         \
            fprintf(stderr, "\n");                \
            failures++;                           \
        }                                         \
    } while (0)

int main(void)
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        printf("test_vulkan_compute: SKIP (loader: %s)\n", SDL_GetError());
        return 0;
    }

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS) {
        printf("test_vulkan_compute: SKIP (no Vulkan ICD)\n");
        return 0;
    }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, NULL);
    if (n == 0) {
        printf("test_vulkan_compute: SKIP (no physical devices)\n");
        goto cleanup;
    }
    VkPhysicalDevice phys[8];
    vkEnumeratePhysicalDevices(instance, &n, phys);
    if (n > 8) {
        n = 8;
    }

    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t qfamily = 0;
    bool found = false;
    for (uint32_t i = 0; i < n && !found; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys[i], &props);
        printf("device %u: %s\n", i, props.deviceName);
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qn, NULL);
        VkQueueFamilyProperties qps[16];
        if (qn > 16) {
            qn = 16;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qn, qps);
        for (uint32_t q = 0; q < qn; q++) {
            if (qps[q].queueCount > 0 && (qps[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                float prio = 1.0f;
                VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                    .queueFamilyIndex = q, .queueCount = 1, .pQueuePriorities = &prio };
                VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                    .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
                if (vkCreateDevice(phys[i], &dci, NULL, &device) == VK_SUCCESS) {
                    vkGetDeviceQueue(device, q, 0, &queue);
                    chosen = phys[i];
                    qfamily = q;
                    found = true;
                    break;
                }
            }
        }
    }
    CHECK(found, "no device with a compute queue");
    if (!found) {
        goto cleanup;
    }

    /* host-visible storage buffer */
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = N * sizeof(uint32_t), .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    CHECK(vkCreateBuffer(device, &bci, NULL, &buf) == VK_SUCCESS, "create buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buf, &req);
    VkPhysicalDeviceMemoryProperties mprops;
    vkGetPhysicalDeviceMemoryProperties(chosen, &mprops);
    uint32_t mtype = UINT32_MAX;
    for (uint32_t i = 0; i < mprops.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mtype = i;
            break;
        }
    }
    CHECK(mtype != UINT32_MAX, "no host-visible memory type");
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = mtype };
    CHECK(vkAllocateMemory(device, &mai, NULL, &mem) == VK_SUCCESS, "alloc");
    CHECK(vkBindBufferMemory(device, buf, mem, 0) == VK_SUCCESS, "bind");

    /* zero it so a stale/no-op dispatch would be detected */
    void *map = NULL;
    CHECK(vkMapMemory(device, mem, 0, N * sizeof(uint32_t), 0, &map) == VK_SUCCESS, "map");
    memset(map, 0, N * sizeof(uint32_t));
    VkMappedMemoryRange mfr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = mem,
        .offset = 0, .size = N * sizeof(uint32_t) };
    vkFlushMappedMemoryRanges(device, 1, &mfr);

    /* descriptor set with the storage buffer */
    VkDescriptorSetLayoutBinding binding = { .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    CHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl) == VK_SUCCESS, "dsl");
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl };
    CHECK(vkCreatePipelineLayout(device, &plci, NULL, &playout) == VK_SUCCESS, "pipeline layout");
    VkDescriptorPoolSize psize = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &psize };
    CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &dpool) == VK_SUCCESS, "dpool");
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    CHECK(vkAllocateDescriptorSets(device, &dsai, &dset) == VK_SUCCESS, "dset");
    VkDescriptorBufferInfo dbi = { .buffer = buf, .offset = 0, .range = N * sizeof(uint32_t) };
    VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset,
        .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &dbi };
    vkUpdateDescriptorSets(device, 1, &wds, 0, NULL);

    /* compute pipeline */
    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(compute_fill_spv), .pCode = compute_fill_spv };
    VkShaderModule sm = VK_NULL_HANDLE;
    CHECK(vkCreateShaderModule(device, &smci, NULL, &sm) == VK_SUCCESS, "shader module");
    VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
        .layout = playout };
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline) == VK_SUCCESS,
          "compute pipeline");
    if (sm) {
        vkDestroyShaderModule(device, sm, NULL);
    }

    /* record + dispatch */
    VkCommandPoolCreateInfo cpci2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfamily };
    CHECK(vkCreateCommandPool(device, &cpci2, NULL, &cpool) == VK_SUCCESS, "cpool");
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd) == VK_SUCCESS, "cmd");
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &cbbi);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &dset, 0, NULL);
    vkCmdDispatch(cmd, N / 64, 1, 1);
    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS, "end cmd");

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK(vkCreateFence(device, &fci, NULL, &fence) == VK_SUCCESS, "fence");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
        .pCommandBuffers = &cmd };
    CHECK(vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS, "submit");
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS, "fence wait");

    /* verify */
    uint32_t *out = (uint32_t *)map;
    bool ok = true;
    for (uint32_t i = 0; i < N; i++) {
        if (out[i] != i * 3u + 1u) {
            CHECK(false, "buf[%u] = %u (expected %u)", i, out[i], i * 3u + 1u);
            ok = false;
            if (i > 4) {
                break;
            }
        }
    }
    if (ok) {
        printf("compute: all %u outputs correct\n", N);
    }
    vkUnmapMemory(device, mem);

cleanup:
    if (device) {
        vkDeviceWaitIdle(device);
    }
    if (fence) {
        vkDestroyFence(device, fence, NULL);
    }
    if (cmd) {
        vkFreeCommandBuffers(device, cpool, 1, &cmd);
    }
    if (cpool) {
        vkDestroyCommandPool(device, cpool, NULL);
    }
    if (pipeline) {
        vkDestroyPipeline(device, pipeline, NULL);
    }
    if (playout) {
        vkDestroyPipelineLayout(device, playout, NULL);
    }
    if (dpool) {
        vkDestroyDescriptorPool(device, dpool, NULL);
    }
    if (mem) {
        vkFreeMemory(device, mem, NULL);
    }
    if (buf) {
        vkDestroyBuffer(device, buf, NULL);
    }
    if (device) {
        vkDestroyDevice(device, NULL);
    }
    if (instance) {
        vkDestroyInstance(instance, NULL);
    }
    SDL_Vulkan_UnloadLibrary();

    if (failures) {
        printf("test_vulkan_compute: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("test_vulkan_compute: PASS\n");
    return 0;
}
