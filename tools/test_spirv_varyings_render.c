// Render test for runtime/nvn/vulkan/spirv_varyings.c, driven by tools/test_spirv_varyings_render.py: builds one
// graphics pipeline from the given SPIR-V stages (vertex, optional tessellation control + evaluation, optional
// geometry, fragment), draws one triangle into a small offscreen R32G32B32A32_UINT image, then renumbers the last
// pre-rasterization stage + fragment pair with spirv_compact_varyings, draws again and compares the two images word for
// word. With --fs-alt it also draws with a fragment shader that reads two varyings swapped, which must give another
// image (so the comparison would see a wrong renumbering). With --validation, VK_LAYER_KHRONOS_validation is on and any
// warning or error it reports fails the test (it checks the stages' interfaces: Locations, Components, types).
// Nothing is shown on a display.
//
// Usage: test_spirv_varyings_render --list
//        test_spirv_varyings_render --device N [--validation] --vs a.spv [--tcs b.spv --tes c.spv] [--gs d.spv]
//                                   --fs e.spv [--fs-alt f.spv]
// --list prints "device <i> <api> <driver id> <name> | <driver info>" per Vulkan 1.3 device and "validation yes|no";
// it exits 77 when there's no Vulkan device. A run prints "ok ..." and exits 0, or "FAIL ..." and exits 1.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "../runtime/nvn/vulkan/spirv_varyings.h"

enum { W = 32, H = 32 };
#define VAL_LAYER "VK_LAYER_KHRONOS_validation"

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static int val_msgs;

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d\n", #x, r_); exit(1); } } while (0)

static VKAPI_ATTR VkBool32 VKAPI_CALL on_msg(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                             const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)user;
    // (general messages are the loader's: ICDs of other machines it can't load and the like)
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT &&
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT))) {
        val_msgs++;
        printf("validation: %s\n", d->pMessage);
    }
    return VK_FALSE;
}

static bool layer_present(void) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties *l = calloc(n ? n : 1, sizeof *l);
    vkEnumerateInstanceLayerProperties(&n, l);
    bool found = false;
    for (uint32_t i = 0; i < n; i++) found |= !strcmp(l[i].layerName, VAL_LAYER);
    free(l);
    return found;
}

static uint32_t *load(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL can't open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)sz);
    if (fread(w, 1, (size_t)sz, f) != (size_t)sz) { printf("FAIL can't read %s\n", path); exit(1); }
    fclose(f);
    *n = (size_t)sz / 4;
    return w;
}

static VkShaderModule module(const uint32_t *w, size_t n) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n * 4, .pCode = w};
    VkShaderModule m;
    CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
    return m;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    printf("FAIL no memory type\n");
    exit(1);
}

static VkImage img;
static VkImageView view;
static VkBuffer buf;
static VkDeviceMemory buf_mem;
static void *buf_map;
static VkCommandPool pool;

static void make_targets(void) {
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R32G32B32A32_UINT, .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    CHECK(vkCreateImage(dev, &ici, NULL, &img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory im;
    CHECK(vkAllocateMemory(dev, &ai, NULL, &im));
    CHECK(vkBindImageMemory(dev, img, im, 0));
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ici.format,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(dev, &vci, NULL, &view));
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 16,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(dev, &ai, NULL, &buf_mem));
    CHECK(vkBindBufferMemory(dev, buf, buf_mem, 0));
    CHECK(vkMapMemory(dev, buf_mem, 0, VK_WHOLE_SIZE, 0, &buf_map));
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam};
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
}

typedef struct { const uint32_t *w; size_t n; VkShaderStageFlagBits stage; } Stage;

// Draws one triangle (a patch of 3 with tessellation) with the stages and copies the image into out (W x H uvec4).
static void render(const Stage *st, int nst, uint32_t *out) {
    VkPipelineShaderStageCreateInfo ss[5];
    bool tess = false;
    for (int i = 0; i < nst; i++) {
        ss[i] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = st[i].stage, .module = module(st[i].w, st[i].n), .pName = "main"};
        tess |= st[i].stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    }
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(dev, &lci, NULL, &layout));
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = tess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineTessellationStateCreateInfo ts = {.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
                                                .patchControlPoints = 3};
    VkViewport vp = {0, 0, W, H, 0, 1};
    VkRect2D sc = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &cba};
    VkFormat fmt = VK_FORMAT_R32G32B32A32_UINT;
    VkPipelineRenderingCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                         .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci,
                                        .stageCount = (uint32_t)nst, .pStages = ss, .pVertexInputState = &vi,
                                        .pInputAssemblyState = &ia, .pTessellationState = tess ? &ts : NULL,
                                        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                        .pColorBlendState = &cb, .layout = layout};
    VkPipeline pipe;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe));

    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
                              .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL,
                         0, NULL, 1, &b);
    VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                     .clearValue.color.uint32 = {0, 0, 0, 0}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = sc, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &att};
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
    VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    memcpy(out, buf_map, W * H * 16);
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    for (int i = 0; i < nst; i++) vkDestroyShaderModule(dev, ss[i].module, NULL);
}

static void make_instance(bool validation) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "test_spirv_varyings_render",
                             .apiVersion = VK_API_VERSION_1_3};
    const char *layer = VAL_LAYER, *ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT mci = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = on_msg};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (validation) {
        ci.pNext = &mci;  // (also covers instance creation)
        ci.enabledLayerCount = 1, ci.ppEnabledLayerNames = &layer;
        ci.enabledExtensionCount = 1, ci.ppEnabledExtensionNames = &ext;
    }
    VkResult r = vkCreateInstance(&ci, NULL, &inst);
    if (r != VK_SUCCESS) {
        printf("no Vulkan instance (%d)\n", r);
        exit(77);
    }
    if (validation) {
        PFN_vkCreateDebugUtilsMessengerEXT f = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerEXT m;
        CHECK(f(inst, &mci, NULL, &m));
    }
}

// The Vulkan 1.3 physical devices.
static uint32_t devices(VkPhysicalDevice *out, uint32_t max) {
    uint32_t n = 0, k = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice *all = calloc(n ? n : 1, sizeof *all);
    vkEnumeratePhysicalDevices(inst, &n, all);
    for (uint32_t i = 0; i < n && k < max; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(all[i], &p);
        if (p.apiVersion >= VK_API_VERSION_1_3) out[k++] = all[i];
    }
    free(all);
    return k;
}

int main(int argc, char **argv) {
    const char *path[5] = {0}, *fs_alt = NULL;  // vs tcs tes gs fs
    int devi = -1;
    bool list = false, validation = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *n = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--list")) list = true;
        else if (!strcmp(a, "--validation")) validation = true;
        else if (n && !strcmp(a, "--device")) devi = atoi(n), i++;
        else if (n && !strcmp(a, "--vs")) path[0] = n, i++;
        else if (n && !strcmp(a, "--tcs")) path[1] = n, i++;
        else if (n && !strcmp(a, "--tes")) path[2] = n, i++;
        else if (n && !strcmp(a, "--gs")) path[3] = n, i++;
        else if (n && !strcmp(a, "--fs")) path[4] = n, i++;
        else if (n && !strcmp(a, "--fs-alt")) fs_alt = n, i++;
        else { fprintf(stderr, "bad argument %s (see the top of tools/test_spirv_varyings_render.c)\n", a); return 2; }
    }
    if (list) {
        make_instance(false);
        VkPhysicalDevice d[16];
        uint32_t n = devices(d, 16);
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
            vkGetPhysicalDeviceProperties2(d[i], &p);
            printf("device %u %u.%u %d %s | %s %s\n", i, VK_API_VERSION_MAJOR(p.properties.apiVersion),
                   VK_API_VERSION_MINOR(p.properties.apiVersion), drv.driverID, p.properties.deviceName, drv.driverName,
                   drv.driverInfo);
        }
        printf("validation %s\n", layer_present() ? "yes" : "no");
        return n ? 0 : 77;
    }
    if (!path[0] || !path[4] || !path[1] != !path[2] || devi < 0) { fprintf(stderr, "need --device, --vs and --fs\n"); return 2; }
    if (validation && !layer_present()) { printf("FAIL %s is not installed\n", VAL_LAYER); return 1; }
    make_instance(validation);
    VkPhysicalDevice d[16];
    if ((uint32_t)devi >= devices(d, 16)) { printf("no device %d\n", devi); return 77; }
    phys = d[devi];

    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(phys, &have);
    VkPhysicalDeviceVulkan13Features have13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 have2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have13};
    vkGetPhysicalDeviceFeatures2(phys, &have2);
    VkPhysicalDeviceFeatures want = {.shaderFloat64 = VK_TRUE, .sampleRateShading = VK_TRUE,
                                     .geometryShader = path[3] != NULL, .tessellationShader = path[1] != NULL};
    if (!have.shaderFloat64 || !have.sampleRateShading || (want.geometryShader && !have.geometryShader) ||
        (want.tessellationShader && !have.tessellationShader) || !have13.dynamicRendering) {
        printf("skip: the device lacks shaderFloat64, sampleRateShading, dynamicRendering or the stage\n");
        return 77;
    }
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    nq = nq > 16 ? 16 : nq;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    for (qfam = 0; qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT); qfam++) {}
    if (qfam == nq) { printf("skip: no graphics queue\n"); return 77; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .pEnabledFeatures = &want};
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    make_targets();

    static const VkShaderStageFlagBits bits[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                                  VK_SHADER_STAGE_FRAGMENT_BIT};
    Stage st[5];
    int nst = 0, last = -1;
    for (int i = 0; i < 5; i++)
        if (path[i]) {
            st[nst] = (Stage){.stage = bits[i]};
            st[nst].w = load(path[i], &st[nst].n);
            if (i < 4) last = nst;
            nst++;
        }
    static uint32_t off[W * H * 4], on[W * H * 4], alt[W * H * 4];
    render(st, nst, off);

    // the renumbered pair: copies of the last pre-rasterization stage and the fragment shader
    Stage rn[5];
    memcpy(rn, st, sizeof st);
    uint32_t *pre = malloc(st[last].n * 4), *fs = malloc(st[nst - 1].n * 4);
    memcpy(pre, st[last].w, st[last].n * 4);
    memcpy(fs, st[nst - 1].w, st[nst - 1].n * 4);
    char info[256];
    int r = spirv_compact_varyings(pre, st[last].n, fs, st[nst - 1].n, 16, info, sizeof info);
    if (r != 1) { printf("FAIL spirv_compact_varyings = %d: %s\n", r, info); return 1; }
    rn[last].w = pre, rn[nst - 1].w = fs;
    render(rn, nst, on);

    int covered = 0, diff = 0, alt_diff = -1;
    for (int i = 0; i < W * H; i++) {
        covered += off[i * 4 + 3] != 0;
        diff += memcmp(off + i * 4, on + i * 4, 16) != 0;
    }
    if (fs_alt) {
        Stage sa[5];
        memcpy(sa, st, sizeof st);
        sa[nst - 1].w = load(fs_alt, &sa[nst - 1].n);
        render(sa, nst, alt);
        alt_diff = 0;
        for (int i = 0; i < W * H; i++) alt_diff += memcmp(off + i * 4, alt + i * 4, 16) != 0;
    }
    vkDeviceWaitIdle(dev);
    bool ok = covered > 0 && diff == 0 && alt_diff != 0 && val_msgs == 0;
    printf("%s %d of %d pixels drawn, %d differ with the renumbering", ok ? "ok" : "FAIL", covered, W * H, diff);
    if (alt_diff >= 0) printf(", %d with two varyings swapped", alt_diff);
    if (validation) printf(", %d validation messages", val_msgs);
    printf("; %s\n", info);
    return ok ? 0 : 1;
}
