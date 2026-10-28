// Vulkan renderer + presenter (built with -DMVP_VULKAN: macOS through MoltenVK, Linux / Windows natively).
//
// Renderer (GpuBackend): the same design as the OpenGL backend in gs_gpu.cpp, so the two match pixel for pixel:
// dual-source blending for the GS blend formula, a copy of the target for the cases fixed-function blending can't
// express, the destination alpha test through the stencil buffer. Pipelines are made on first use, one per
// combination of fixed-function state (topology, blend, colour mask, depth test, stencil use).
//
// Presenter: game picture -> [FXAA] -> resample -> sharpen/colour/CRT -> swapchain, then the UI (shaders: vk/*).
// One command buffer per frame in flight records everything (renderer flushes, texture uploads, the presenter's
// passes); read-backs submit it early and wait. Images stay in VK_IMAGE_LAYOUT_GENERAL, with full barriers
// between passes: simple and correct, and these passes are few per frame.
#ifdef MVP_VULKAN
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_vulkan.h>
#ifdef __APPLE__
#include <SDL_metal.h>
#define VK_USE_PLATFORM_METAL_EXT
#endif
#include <cstdio>
#include <string>
#define VK_NO_PROTOTYPES                 // the loader is opened at run time: no Vulkan driver = no Vulkan, not a failed start
#include <vulkan/vulkan.h>
#ifndef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME      // older headers (before 1.3.216)
#define VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME "VK_KHR_portability_enumeration"
#define VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR 0x00000001u
#endif
// every Vulkan function used, as a pointer loaded through vkGetInstanceProcAddr
#define VK_GLOBAL_FUNCS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define VK_INSTANCE_FUNCS(X) \
    X(vkAcquireNextImageKHR) X(vkAllocateCommandBuffers) X(vkAllocateDescriptorSets) X(vkAllocateMemory) \
    X(vkBeginCommandBuffer) X(vkBindBufferMemory) X(vkBindImageMemory) X(vkCmdBeginRenderPass) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindPipeline) X(vkCmdBlitImage) X(vkCmdClearAttachments) X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdDraw) \
    X(vkCmdEndRenderPass) X(vkCmdPipelineBarrier) X(vkCmdPushConstants) X(vkCmdSetScissor) X(vkCmdSetViewport) \
    X(vkCreateBuffer) X(vkCreateCommandPool) X(vkCreateDescriptorPool) X(vkCreateDescriptorSetLayout) \
    X(vkCreateDevice) X(vkCreateFence) X(vkCreateFramebuffer) X(vkCreateGraphicsPipelines) X(vkCreateImage) \
    X(vkCreateImageView) X(vkCreatePipelineLayout) X(vkCreateRenderPass) X(vkCreateSampler) X(vkCreateSemaphore) \
    X(vkCreateShaderModule) X(vkCreateSwapchainKHR) X(vkDestroyBuffer) X(vkDestroyFramebuffer) X(vkDestroyImage) \
    X(vkDestroyImageView) X(vkDestroySampler) X(vkDestroySemaphore) X(vkDestroySwapchainKHR) X(vkDeviceWaitIdle) \
    X(vkEndCommandBuffer) X(vkEnumerateDeviceExtensionProperties) X(vkEnumeratePhysicalDevices) X(vkFreeMemory) \
    X(vkGetBufferMemoryRequirements) X(vkGetDeviceQueue) X(vkGetImageMemoryRequirements) \
    X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetSwapchainImagesKHR) \
    X(vkMapMemory) X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkResetCommandBuffer) X(vkResetDescriptorPool) \
    X(vkResetFences) X(vkUpdateDescriptorSets) X(vkWaitForFences)
#define VK_DECL(f) static PFN_##f f = nullptr;
VK_GLOBAL_FUNCS(VK_DECL)
VK_INSTANCE_FUNCS(VK_DECL)
#ifdef __APPLE__
static PFN_vkCreateMetalSurfaceEXT vkCreateMetalSurfaceEXT = nullptr;
#endif
static PFN_vkGetInstanceProcAddr s_gipa = nullptr;
static bool vk_load_loader() {
    if (s_gipa) return true;
    if (!(s_gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr())) {
        bool ok = SDL_Vulkan_LoadLibrary(nullptr) == 0;
#ifdef __APPLE__
        if (!ok) {                                            // the app bundle's Frameworks, then Homebrew
            std::string fw; if (char* b = SDL_GetBasePath()) { fw = std::string(b) + "../Frameworks/libMoltenVK.dylib"; SDL_free(b); }
            for (const std::string& p : {fw, std::string("/opt/homebrew/lib/libvulkan.1.dylib"), std::string("/usr/local/lib/libvulkan.1.dylib"),
                                          std::string("/opt/homebrew/lib/libMoltenVK.dylib"), std::string("/usr/local/lib/libMoltenVK.dylib")})
                if (!p.empty() && SDL_Vulkan_LoadLibrary(p.c_str()) == 0) { ok = true; break; }
        }
#endif
        if (!ok) { fprintf(stderr, "[vulkan] no Vulkan loader (%s)\n", SDL_GetError()); return false; }
        s_gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    }
    if (!s_gipa) return false;
#define VK_LOAD_G(f) f = (PFN_##f)s_gipa(nullptr, #f);
    VK_GLOBAL_FUNCS(VK_LOAD_G)
    return vkCreateInstance && vkEnumerateInstanceExtensionProperties;
}
static bool vk_load_instance(VkInstance inst) {
    bool ok = true;
#define VK_LOAD_I(f) if (!(f = (PFN_##f)s_gipa(inst, #f))) { fprintf(stderr, "[vulkan] missing %s\n", #f); ok = false; }
    VK_INSTANCE_FUNCS(VK_LOAD_I)
#ifdef __APPLE__
    vkCreateMetalSurfaceEXT = (PFN_vkCreateMetalSurfaceEXT)s_gipa(inst, "vkCreateMetalSurfaceEXT");
    if (!vkCreateMetalSurfaceEXT) ok = false;
#endif
    return ok;
}
#include "gs_gpu.h"
#include "present.h"
#include "ee_runtime.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include <algorithm>
#include <string>
#include <chrono>

static const uint32_t kGsVert[] =
#include "vk/gs.vert.inc"
;
static const uint32_t kGsFrag[] =
#include "vk/gs.frag.inc"
;
static const uint32_t kQuadVert[] =
#include "vk/quad.vert.inc"
;
static const uint32_t kCopyFrag[] =
#include "vk/copy.frag.inc"
;
static const uint32_t kDepthFrag[] =
#include "vk/depth.frag.inc"
;
static const uint32_t kDateFrag[] =
#include "vk/date.frag.inc"
;
static const uint32_t kDownFrag[] =
#include "vk/down.frag.inc"
;
static const uint32_t kFxaaFrag[] =
#include "vk/fxaa.frag.inc"
;
static const uint32_t kResampleFrag[] =
#include "vk/resample.frag.inc"
;
static const uint32_t kFinalFrag[] =
#include "vk/final.frag.inc"
;
static const uint32_t kUiVert[] =
#include "vk/ui.vert.inc"
;
static const uint32_t kUiFrag[] =
#include "vk/ui.frag.inc"
;

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "[vulkan] %s failed (%d)\n", #x, (int)r_); return false; } } while (0)

// ================================================================ shared device / per-frame state
struct Img {
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE, dview = VK_NULL_HANDLE;
    int w = 0, h = 0, levels = 1; VkFormat fmt = VK_FORMAT_UNDEFINED;
};
struct Chunk { VkBuffer buf; VkDeviceMemory mem; u8* map; size_t size, used; };
struct Slot {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> pools; size_t pool_i = 0;
    std::vector<Chunk> chunks; size_t chunk_i = 0;
    std::vector<std::function<void()>> trash;           // destroyed once this slot's work has finished
};
static struct Vk {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties mprops{};
    bool aniso_ok = false;
    bool dual = true;                                   // dual-source blending
    bool portability = false;                           // MoltenVK (Vulkan on Metal): GPU blits are slow there
    Slot slots[2]; int cur = 0; bool recording = false;
    VkBuffer rb = VK_NULL_HANDLE; VkDeviceMemory rb_mem = VK_NULL_HANDLE; u8* rb_map = nullptr; size_t rb_size = 0;   // read-backs
    VkSampler s_near = VK_NULL_HANDLE, s_lin = VK_NULL_HANDLE, s_mip = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_misc = VK_NULL_HANDLE;
    VkPipelineLayout pl_misc = VK_NULL_HANDLE;
    Img dummy;                                          // 1x1 texture for unused bindings
    VkBuffer dummy_buf = VK_NULL_HANDLE; VkDeviceMemory dummy_mem = VK_NULL_HANDLE;
} V;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < V.mprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (V.mprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static bool make_buffer(size_t size, VkBufferUsageFlags use, VkBuffer& b, VkDeviceMemory& m, u8** map) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = use; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(V.dev, &bi, nullptr, &b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(V.dev, b, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(V.dev, &ai, nullptr, &m));
    VKCHECK(vkBindBufferMemory(V.dev, b, m, 0));
    if (map) VKCHECK(vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, (void**)map));
    return true;
}
static VkCommandBuffer cmd() {
    Slot& s = V.slots[V.cur];
    if (!V.recording) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s.cb, &bi);
        V.recording = true;
    }
    return s.cb;
}
// everything before -> everything after (outside render passes)
static void barrier() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
static void layout(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, uint32_t levels = VK_REMAINING_MIP_LEVELS) {
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    ib.oldLayout = from; ib.newLayout = to;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img; ib.subresourceRange = {aspect, 0, levels, 0, 1};
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
}
// per-frame upload memory (vertex data, texture staging): host-visible chunks, reused when the slot comes round again
static u8* arena(size_t size, size_t align, VkBuffer& buf, VkDeviceSize& off) {
    Slot& s = V.slots[V.cur];
    for (;;) {
        if (s.chunk_i < s.chunks.size()) {
            Chunk& c = s.chunks[s.chunk_i];
            size_t o = (c.used + align - 1) / align * align;
            if (o + size <= c.size) { c.used = o + size; buf = c.buf; off = o; return c.map + o; }
            s.chunk_i++; continue;
        }
        Chunk c{}; c.size = std::max<size_t>(size + align, 32u << 20);
        if (!make_buffer(c.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, c.buf, c.mem, &c.map)) return nullptr;
        s.chunks.push_back(c);
        s.chunk_i = s.chunks.size() - 1;
    }
}
static VkDescriptorPool new_pool() {
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 8192; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
    VkDescriptorPool p = VK_NULL_HANDLE;
    vkCreateDescriptorPool(V.dev, &pi, nullptr, &p);
    return p;
}
static VkDescriptorSet alloc_set(VkDescriptorSetLayout l) {
    Slot& s = V.slots[V.cur];
    for (;;) {
        if (s.pool_i >= s.pools.size()) s.pools.push_back(new_pool());
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.pools[s.pool_i]; ai.descriptorSetCount = 1; ai.pSetLayouts = &l;
        VkDescriptorSet set;
        if (vkAllocateDescriptorSets(V.dev, &ai, &set) == VK_SUCCESS) return set;
        s.pool_i++;
    }
}
static void write_img(VkDescriptorSet set, uint32_t binding, VkImageView view, VkSampler smp) {
    VkDescriptorImageInfo ii{smp, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(V.dev, 1, &w, 0, nullptr);
}
static void write_buf(VkDescriptorSet set, uint32_t binding, VkBuffer b, VkDeviceSize off, VkDeviceSize range) {
    VkDescriptorBufferInfo bi{b, off, range};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(V.dev, 1, &w, 0, nullptr);
}
// a misc set: binding 0 storage buffer (or the dummy), 1 and 2 textures (or the dummy)
static VkDescriptorSet misc_set(VkImageView t1, VkSampler s1, VkImageView t2 = VK_NULL_HANDLE, VkSampler s2 = VK_NULL_HANDLE,
                                VkBuffer b = VK_NULL_HANDLE, VkDeviceSize off = 0, VkDeviceSize range = 0) {
    VkDescriptorSet set = alloc_set(V.dsl_misc);
    write_buf(set, 0, b ? b : V.dummy_buf, b ? off : 0, b ? range : 256);
    write_img(set, 1, t1 ? t1 : V.dummy.view, s1 ? s1 : V.s_near);
    write_img(set, 2, t2 ? t2 : V.dummy.view, s2 ? s2 : V.s_near);
    return set;
}
static void trash(std::function<void()> f) { V.slots[V.cur].trash.push_back(std::move(f)); }
static void free_img(Img& i) {
    if (!i.img) return;
    Img c = i; i = Img();
    trash([c] { if (c.dview) vkDestroyImageView(V.dev, c.dview, nullptr); vkDestroyImageView(V.dev, c.view, nullptr);
                vkDestroyImage(V.dev, c.img, nullptr); vkFreeMemory(V.dev, c.mem, nullptr); });
}
static bool make_img(Img& o, int w, int h, VkFormat fmt, VkImageUsageFlags use, int levels = 1) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {(uint32_t)w, (uint32_t)h, 1};
    ci.mipLevels = levels; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = use; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(V.dev, &ci, nullptr, &o.img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(V.dev, o.img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size; ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkAllocateMemory(V.dev, &ai, nullptr, &o.mem));
    VKCHECK(vkBindImageMemory(V.dev, o.img, o.mem, 0));
    const bool ds = fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = o.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = {ds ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)levels, 0, 1};
    VKCHECK(vkCreateImageView(V.dev, &vi, nullptr, &o.view));
    if (ds) { vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; VKCHECK(vkCreateImageView(V.dev, &vi, nullptr, &o.dview)); }
    o.w = w; o.h = h; o.levels = levels; o.fmt = fmt;
    layout(o.img, ds ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    return true;
}
// copy pixels into level 0 (bpp bytes per pixel), then build the mip chain if the image has one
// Texture uploads are queued and recorded together (one barrier pair for all of them) before the next flush,
// read-back or frame: two full barriers per texture were most of the upload cost under MoltenVK.
struct PendingCopy { VkBuffer b; VkImage img; VkBufferImageCopy r; };
static std::vector<PendingCopy> s_pending;
static void flush_uploads() {
    if (s_pending.empty()) return;
    barrier();
    for (size_t i = 0; i < s_pending.size(); ) {           // consecutive regions of one image in one call
        size_t j = i + 1;
        while (j < s_pending.size() && s_pending[j].img == s_pending[i].img && s_pending[j].b == s_pending[i].b) j++;
        std::vector<VkBufferImageCopy> regs; regs.reserve(j - i);
        for (size_t k = i; k < j; k++) regs.push_back(s_pending[k].r);
        vkCmdCopyBufferToImage(cmd(), s_pending[i].b, s_pending[i].img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regs.size(), regs.data());
        i = j;
    }
    s_pending.clear();
    barrier();
}
// queue level 0 of a texture (no mip chain)
static void upload_queued(Img& o, const void* px) {
    if (!px) return;
    const size_t n = (size_t)o.w * o.h * 4;
    VkBuffer b; VkDeviceSize off;
    u8* p = arena(n, 16, b, off);
    if (!p) return;
    std::memcpy(p, px, n);
    VkBufferImageCopy r{}; r.bufferOffset = off; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.imageExtent = {(uint32_t)o.w, (uint32_t)o.h, 1};
    s_pending.push_back({b, o.img, r});
}
static void gen_mips(Img& o);
static void upload(Img& o, const void* px, int bpp) {
    flush_uploads();
    if (!px) return;
    const size_t n = (size_t)o.w * o.h * bpp;
    VkBuffer b; VkDeviceSize off;
    u8* p = arena(n, 16, b, off);
    if (!p) return;
    std::memcpy(p, px, n);
    barrier();
    VkBufferImageCopy r{}; r.bufferOffset = off; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.imageExtent = {(uint32_t)o.w, (uint32_t)o.h, 1};
    vkCmdCopyBufferToImage(cmd(), b, o.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    gen_mips(o);
}
static void gen_mips(Img& o) {
    int w = o.w, h = o.h;
    for (int l = 1; l < o.levels; l++) {
        barrier();
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l - 1, 0, 1}; bl.srcOffsets[1] = {w, h, 1};
        w = std::max(1, w / 2); h = std::max(1, h / 2);
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, 1}; bl.dstOffsets[1] = {w, h, 1};
        vkCmdBlitImage(cmd(), o.img, VK_IMAGE_LAYOUT_GENERAL, o.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    }
    barrier();
}
// level 0 from px and every smaller level averaged on the CPU (2x2 box), all copied in one command: building the
// chain with GPU blits costs a render pass per level under MoltenVK, and the game uploads thousands of textures a second
static void upload_mips_cpu(Img& o, const u32* px) {
    flush_uploads();
    size_t total = 0; int w = o.w, h = o.h;
    for (int l = 0; l < o.levels; l++) { total += (size_t)w * h * 4; w = std::max(1, w / 2); h = std::max(1, h / 2); }
    VkBuffer b; VkDeviceSize off;
    u8* out = arena(total, 16, b, off);
    if (!out) return;
    static std::vector<u8> tmp;                    // built in ordinary memory (upload memory may be uncached), then copied
    if (tmp.size() < total) tmp.resize(total);
    u8* p = tmp.data();
    std::vector<VkBufferImageCopy> regs(o.levels);
    w = o.w; h = o.h;
    std::memcpy(p, px, (size_t)w * h * 4);
    const u8* src = p; size_t at = 0;
    for (int l = 0; l < o.levels; l++) {
        VkBufferImageCopy& r = regs[l];
        r = VkBufferImageCopy{}; r.bufferOffset = off + at;   // (offsets within tmp = offsets within the upload) r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, 1}; r.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
        if (l + 1 == o.levels) break;
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        u8* dst = p + at + (size_t)w * h * 4;
        // 2x2 average of four RGBA8 pixels at once in 32-bit words (per byte: (a+b)/2 twice, rounding down)
        auto avg = [](u32 a, u32 b) { return ((a & 0xfefefefeu) >> 1) + ((b & 0xfefefefeu) >> 1) + (a & b & 0x01010101u); };
        const u32* s32 = (const u32*)src; u32* d32 = (u32*)dst;
        const int sx = w > 1 ? 1 : 0;
        for (int y = 0; y < nh; y++) {
            const u32* r0 = s32 + (size_t)std::min(2 * y, h - 1) * w; const u32* r1 = s32 + (size_t)std::min(2 * y + 1, h - 1) * w;
            u32* d = d32 + (size_t)y * nw;
            for (int x = 0; x < nw; x++) { const int x0 = 2 * x; d[x] = avg(avg(r0[x0], r0[x0 + sx]), avg(r1[x0], r1[x0 + sx])); }
        }
        at += (size_t)w * h * 4; src = dst; w = nw; h = nh;
    }
    std::memcpy(out, p, total);
    barrier();
    vkCmdCopyBufferToImage(cmd(), b, o.img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regs.size(), regs.data());
    barrier();
}
static int mip_levels(int w, int h) { int l = 1; while ((w | h) > 1) { w >>= 1; h >>= 1; l++; } return l; }
static void reset_slot(Slot& s) {
    for (auto& f : s.trash) f();
    s.trash.clear();
    for (VkDescriptorPool p : s.pools) vkResetDescriptorPool(V.dev, p, 0);
    s.pool_i = 0;
    for (Chunk& c : s.chunks) c.used = 0;
    s.chunk_i = 0;
}
// submit what has been recorded and wait for it (read-backs); recording continues in the same slot
static double s_t_sync = 0, s_t_acq = 0, s_t_fence = 0; static int s_n_sync = 0;
static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void submit_wait() {
    const double t0 = now_ms();
    struct T { double t0; ~T() { s_t_sync += now_ms() - t0; s_n_sync++; } } tt{t0};
    Slot& s = V.slots[V.cur];
    flush_uploads();
    cmd();
    vkEndCommandBuffer(s.cb); V.recording = false;
    vkResetFences(V.dev, 1, &s.fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &s.cb;
    vkQueueSubmit(V.queue, 1, &si, s.fence);
    vkWaitForFences(V.dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
    vkResetCommandBuffer(s.cb, 0);
    reset_slot(s);
}
static u8* readback(size_t n) {
    if (V.rb_size < n) {
        if (V.rb) { vkDestroyBuffer(V.dev, V.rb, nullptr); vkFreeMemory(V.dev, V.rb_mem, nullptr); }
        V.rb_size = n + (4u << 20);
        if (!make_buffer(V.rb_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, V.rb, V.rb_mem, &V.rb_map)) { V.rb_size = 0; return nullptr; }
    }
    return V.rb_map;
}
static VkShaderModule shader(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes; ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(V.dev, &ci, nullptr, &m) != VK_SUCCESS) fprintf(stderr, "[vulkan] shader module failed\n");
    return m;
}
#define SHADER(a) shader(a, sizeof a)

// pipeline description: the fixed-function state that varies
struct PipeDesc {
    VkShaderModule vs, fs; VkPipelineLayout layout; VkRenderPass rp;
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    int blend = 0;                 // 0 none, 1 alpha (UI), 2 dual-source add, 3 dual reverse-subtract, 4 dual subtract, 5 zero rgb
    VkColorComponentFlags mask = 0xf;
    bool ztest = false; VkCompareOp zfunc = VK_COMPARE_OP_ALWAYS; bool zwrite = false;
    int stencil = 0;               // 0 off, 1 always replace 1, 2 equal 1 keep, 3 equal 1 zero on pass
    int colors = 1;
};
static VkPipeline make_pipe(const PipeDesc& d) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = d.vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = d.fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = d.topo;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.ztest; ds.depthWriteEnable = d.zwrite; ds.depthCompareOp = d.zfunc;
    if (d.stencil) {
        ds.stencilTestEnable = VK_TRUE;
        VkStencilOpState s{};
        s.compareMask = 0xff; s.writeMask = 0xff; s.reference = 1;
        s.failOp = VK_STENCIL_OP_KEEP; s.depthFailOp = VK_STENCIL_OP_KEEP;
        if (d.stencil == 1) { s.compareOp = VK_COMPARE_OP_ALWAYS; s.passOp = VK_STENCIL_OP_REPLACE; }
        else { s.compareOp = VK_COMPARE_OP_EQUAL; s.passOp = d.stencil == 3 ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_KEEP; }
        ds.front = ds.back = s;
    }
    VkPipelineColorBlendAttachmentState ba[2]{};
    for (int i = 0; i < d.colors; i++) ba[i].colorWriteMask = d.mask;
    if (d.blend == 1) {
        ba[0].blendEnable = VK_TRUE;
        ba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; ba[0].colorBlendOp = VK_BLEND_OP_ADD;
        ba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; ba[0].alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (d.blend >= 2) {
        ba[0].blendEnable = VK_TRUE;
        if (d.blend == 5) { ba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ZERO; ba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO; ba[0].colorBlendOp = VK_BLEND_OP_ADD; }
        else {
            ba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE; ba[0].dstColorBlendFactor = VK_BLEND_FACTOR_SRC1_COLOR;
            ba[0].colorBlendOp = d.blend == 3 ? VK_BLEND_OP_REVERSE_SUBTRACT : d.blend == 4 ? VK_BLEND_OP_SUBTRACT : VK_BLEND_OP_ADD;
        }
        ba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba[0].alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = d.colors; cb.pAttachments = ba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds; pi.pColorBlendState = &cb; pi.pDynamicState = &dy;
    pi.layout = d.layout; pi.renderPass = d.rp; pi.subpass = 0;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS) fprintf(stderr, "[vulkan] pipeline failed\n");
    return p;
}
static VkRenderPass make_rp(const VkFormat* fmts, int ncol, VkFormat depth, bool clear, VkImageLayout final_layout) {
    VkAttachmentDescription at[3]{};
    VkAttachmentReference cr[2], dr;
    for (int i = 0; i < ncol; i++) {
        at[i].format = fmts[i]; at[i].samples = VK_SAMPLE_COUNT_1_BIT;
        at[i].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; at[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[i].initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL; at[i].finalLayout = final_layout;
        cr[i] = {(uint32_t)i, final_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL};
    }
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = ncol; sp.pColorAttachments = cr;
    int n = ncol;
    if (depth != VK_FORMAT_UNDEFINED) {
        at[n].format = depth; at[n].samples = VK_SAMPLE_COUNT_1_BIT;
        at[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[n].initialLayout = VK_IMAGE_LAYOUT_GENERAL; at[n].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        dr = {(uint32_t)n, VK_IMAGE_LAYOUT_GENERAL};
        sp.pDepthStencilAttachment = &dr; n++;
    }
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    for (auto& d : dep) {
        d.srcStageMask = d.dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        d.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; d.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = n; ci.pAttachments = at; ci.subpassCount = 1; ci.pSubpasses = &sp; ci.dependencyCount = 2; ci.pDependencies = dep;
    VkRenderPass rp = VK_NULL_HANDLE;
    vkCreateRenderPass(V.dev, &ci, nullptr, &rp);
    return rp;
}
static VkFramebuffer make_fb(VkRenderPass rp, const VkImageView* views, int n, int w, int h) {
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = rp; ci.attachmentCount = n; ci.pAttachments = views; ci.width = w; ci.height = h; ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    vkCreateFramebuffer(V.dev, &ci, nullptr, &fb);
    return fb;
}
static void begin_rp(VkRenderPass rp, VkFramebuffer fb, int w, int h, bool clear = false) {
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    VkClearValue cv[2]; cv[0].color = {{0, 0, 0, 1}}; cv[1].color = {{0, 0, 0, 1}};
    bi.renderPass = rp; bi.framebuffer = fb; bi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    bi.clearValueCount = clear ? 2 : 0; bi.pClearValues = cv;
    vkCmdBeginRenderPass(cmd(), &bi, VK_SUBPASS_CONTENTS_INLINE);
}
static void viewport(int x, int y, int w, int h) {
    VkViewport v{(float)x, (float)y, (float)w, (float)h, 0, 1};
    vkCmdSetViewport(cmd(), 0, 1, &v);
}
static void scissor(int x, int y, int w, int h) {
    VkRect2D r{{std::max(x, 0), std::max(y, 0)}, {(uint32_t)std::max(w, 0), (uint32_t)std::max(h, 0)}};
    vkCmdSetScissor(cmd(), 0, 1, &r);
}
struct QuadPC { float rect[4], uv[4]; float p0[4]; float p1[4]; float p2[4]; };
static void quad_draw(const QuadPC& q) {
    vkCmdPushConstants(cmd(), V.pl_misc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof q, &q);
    vkCmdDraw(cmd(), 4, 1, 0, 0);
}
static QuadPC full_quad() { QuadPC q{}; q.rect[0] = -1; q.rect[1] = -1; q.rect[2] = 1; q.rect[3] = 1; q.uv[2] = 1; q.uv[3] = 1; return q; }

// ================================================================ renderer
struct GsPC { u32 first, f0, f1, ts, rx, ry, fog; };   // vk/common.glsl
static_assert(sizeof(GsPC) == 28, "push constants (vk/common.glsl)");

struct VkBackend : GpuBackend {
    VkRenderPass rp_target = VK_NULL_HANDLE, rp_down = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_gs0 = VK_NULL_HANDLE, dsl_tex = VK_NULL_HANDLE;
    VkPipelineLayout pl_gs = VK_NULL_HANDLE;
    VkShaderModule m_gsv, m_gsf, m_quad, m_copy, m_depth, m_date, m_down;
    VkPipeline p_copy = VK_NULL_HANDLE, p_depth = VK_NULL_HANDLE, p_date = VK_NULL_HANDLE, p_down = VK_NULL_HANDLE;
    std::unordered_map<u32, VkPipeline> pipes;
    VkSampler s_tex = VK_NULL_HANDLE;
    Img col, dst, depth, ncol, nz;
    VkFramebuffer fb = VK_NULL_HANDLE, fb_down = VK_NULL_HANDLE;
    std::vector<Img> texs; std::vector<unsigned> free_ids, free_later;
    std::unordered_map<unsigned, std::vector<u32>> cpu_px;
    // freed texture images are kept for reuse (the game replaces textures thousands of times a minute; a new
    // VkImage + allocation each time is slow, above all on MoltenVK)
    std::unordered_map<u64, std::vector<Img>> pool; size_t pooled = 0;
    static u64 pkey(int w, int h, int l) { return (u64)w | (u64)h << 20 | (u64)l << 40; }
    int w = 0, h = 0, nw = 0, nh = 0, scale = 1;
    float aniso = 0;

    const char* name() const override { return "Vulkan"; }
    bool dual_source() const override { return V.dual; }
    bool init() {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 2; li.pBindings = b;
        VKCHECK(vkCreateDescriptorSetLayout(V.dev, &li, nullptr, &dsl_gs0));
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        li.bindingCount = 1;
        VKCHECK(vkCreateDescriptorSetLayout(V.dev, &li, nullptr, &dsl_tex));
        VkDescriptorSetLayout sets[2] = {dsl_gs0, dsl_tex};
        VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GsPC)};
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 2; pi.pSetLayouts = sets; pi.pushConstantRangeCount = 1; pi.pPushConstantRanges = &pr;
        VKCHECK(vkCreatePipelineLayout(V.dev, &pi, nullptr, &pl_gs));
        const VkFormat c8 = VK_FORMAT_R8G8B8A8_UNORM, cdown[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT};
        rp_target = make_rp(&c8, 1, VK_FORMAT_D32_SFLOAT_S8_UINT, false, VK_IMAGE_LAYOUT_GENERAL);
        rp_down = make_rp(cdown, 2, VK_FORMAT_UNDEFINED, false, VK_IMAGE_LAYOUT_GENERAL);
        m_gsv = SHADER(kGsVert); m_gsf = SHADER(kGsFrag); m_quad = SHADER(kQuadVert); m_copy = SHADER(kCopyFrag);
        m_depth = SHADER(kDepthFrag); m_date = SHADER(kDateFrag); m_down = SHADER(kDownFrag);
        if (!rp_target || !rp_down || !m_gsv || !m_gsf || !m_quad || !m_copy || !m_depth || !m_date || !m_down) return false;
        PipeDesc d; d.vs = m_quad; d.layout = V.pl_misc; d.rp = rp_target;
        d.fs = m_copy; p_copy = make_pipe(d);
        d.fs = m_depth; d.mask = 0; d.ztest = true; d.zwrite = true; d.zfunc = VK_COMPARE_OP_ALWAYS; p_depth = make_pipe(d);
        d.fs = m_date; d.ztest = false; d.zwrite = false; d.stencil = 1; p_date = make_pipe(d);
        PipeDesc dd; dd.vs = m_quad; dd.fs = m_down; dd.layout = V.pl_misc; dd.rp = rp_down; dd.colors = 2; p_down = make_pipe(dd);
        if (!p_copy || !p_depth || !p_date || !p_down) return false;
        set_aniso(0);
        texs.push_back(Img());                                // id 0 = none
        return true;
    }
    // GS pipeline for: topology, blend (0 none, 2..5), colour mask, z func (0 off, 1..4), z write, stencil (0, 2, 3)
    VkPipeline gs_pipe(int topo, int blend, u32 mask, int zf, bool zw, int stencil) {
        const u32 key = (u32)topo | (u32)blend << 2 | mask << 5 | (u32)zf << 9 | (u32)zw << 12 | (u32)stencil << 13;
        auto it = pipes.find(key);
        if (it != pipes.end()) return it->second;
        static const VkPrimitiveTopology tp[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        static const VkCompareOp zo[5] = {VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
        PipeDesc d; d.vs = m_gsv; d.fs = m_gsf; d.layout = pl_gs; d.rp = rp_target; d.topo = tp[topo];
        d.blend = blend; d.mask = mask; d.ztest = zf != 0; d.zfunc = zo[zf]; d.zwrite = zw; d.stencil = stencil;
        VkPipeline p = make_pipe(d);
        pipes[key] = p;
        return p;
    }
    void set_target(int tw, int th, int wn, int hn, int sc) override {
        w = tw; h = th; nw = wn; nh = hn; scale = sc;
        free_img(col); free_img(dst); free_img(depth); free_img(ncol); free_img(nz);
        if (fb) { VkFramebuffer f = fb; trash([f] { vkDestroyFramebuffer(V.dev, f, nullptr); }); fb = VK_NULL_HANDLE; }
        if (fb_down) { VkFramebuffer f = fb_down; trash([f] { vkDestroyFramebuffer(V.dev, f, nullptr); }); fb_down = VK_NULL_HANDLE; }
        const VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        make_img(col, w, h, VK_FORMAT_R8G8B8A8_UNORM, cu);
        make_img(dst, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        make_img(depth, w, h, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        VkImageView v[2] = {col.view, depth.view};
        fb = make_fb(rp_target, v, 2, w, h);
        VkClearColorValue cc{}; VkClearDepthStencilValue dc{0.0f, 0};
        VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, dr{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd(), col.img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &cr);
        vkCmdClearColorImage(cmd(), dst.img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &cr);
        vkCmdClearDepthStencilImage(cmd(), depth.img, VK_IMAGE_LAYOUT_GENERAL, &dc, 1, &dr);
        barrier();
    }
    void upload_target(const u32* px, int wn, int hn) override {
        Img t; make_img(t, wn, hn, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        upload(t, px, 4);
        VkDescriptorSet set = misc_set(t.view, V.s_near);
        begin_rp(rp_target, fb, w, h);
        viewport(0, 0, std::min(wn * scale, w), std::min(hn * scale, h)); scissor(0, 0, w, h);
        vkCmdBindPipeline(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, p_copy);
        vkCmdBindDescriptorSets(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, V.pl_misc, 0, 1, &set, 0, nullptr);
        quad_draw(full_quad());
        vkCmdEndRenderPass(cmd());
        barrier();
        free_img(t);
    }
    void write_native(const u32* c, const float* d) override {
        upload_target(c, nw, nh);
        if (!d) return;
        Img t; make_img(t, nw, nh, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        upload(t, d, 4);
        VkDescriptorSet set = misc_set(t.view, V.s_near);
        begin_rp(rp_target, fb, w, h);
        viewport(0, 0, w, h); scissor(0, 0, w, h);
        vkCmdBindPipeline(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, p_depth);
        vkCmdBindDescriptorSets(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, V.pl_misc, 0, 1, &set, 0, nullptr);
        quad_draw(full_quad());
        vkCmdEndRenderPass(cmd());
        barrier();
        free_img(t);
    }
    bool read_native(std::vector<u32>& c, std::vector<float>* d) override {
        const size_t n = (size_t)nw * nh;
        u8* rb = readback(n * 8);
        if (!rb) return false;
        VkImage src = col.img;
        if (scale > 1 || d) {        // nearest down-sample (the GL blit's sample positions); depth as R32F colour
            if (!ncol.img) {
                make_img(ncol, nw, nh, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                make_img(nz, nw, nh, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                VkImageView v[2] = {ncol.view, nz.view};
                fb_down = make_fb(rp_down, v, 2, nw, nh);
            }
            barrier();
            VkDescriptorSet set = misc_set(col.view, V.s_near, depth.dview, V.s_near);
            begin_rp(rp_down, fb_down, nw, nh);
            viewport(0, 0, nw, nh); scissor(0, 0, nw, nh);
            vkCmdBindPipeline(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, p_down);
            vkCmdBindDescriptorSets(cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, V.pl_misc, 0, 1, &set, 0, nullptr);
            QuadPC q = full_quad(); int sc = scale; std::memcpy(&q.p0[0], &sc, 4);
            quad_draw(q);
            vkCmdEndRenderPass(cmd());
            src = ncol.img;
        }
        barrier();
        VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.imageExtent = {(uint32_t)nw, (uint32_t)nh, 1};
        vkCmdCopyImageToBuffer(cmd(), src, VK_IMAGE_LAYOUT_GENERAL, V.rb, 1, &r);
        if (d) { r.bufferOffset = n * 4; vkCmdCopyImageToBuffer(cmd(), nz.img, VK_IMAGE_LAYOUT_GENERAL, V.rb, 1, &r); }
        submit_wait();
        c.resize(n); std::memcpy(c.data(), rb, n * 4);
        if (d) { d->resize(n); std::memcpy(d->data(), rb + n * 4, n * 4); }
        return true;
    }
    bool read(int x0, int y0, int rw, int rh, std::vector<u32>& out) override {
        const size_t n = (size_t)rw * rh;
        u8* rb = readback(n * 4);
        if (!rb) return false;
        barrier();
        VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageOffset = {x0, y0, 0}; r.imageExtent = {(uint32_t)rw, (uint32_t)rh, 1};
        vkCmdCopyImageToBuffer(cmd(), col.img, VK_IMAGE_LAYOUT_GENERAL, V.rb, 1, &r);
        submit_wait();
        out.resize(n); std::memcpy(out.data(), rb, n * 4);
        return true;
    }
    bool pooled_img(Img& t, int tw, int th, int lv) {
        auto& pv = pool[pkey(tw, th, lv)];
        if (!pv.empty()) { t = pv.back(); pv.pop_back(); pooled--; return true; }
        return make_img(t, tw, th, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, lv);
    }
    void recycle(const Img& t) {
        if (!t.img) return;
        trash([this, t]() {                              // once the GPU is done with it: keep for reuse, or destroy
            if (pooled < 2048) { pool[pkey(t.w, t.h, t.levels)].push_back(t); pooled++; }
            else { vkDestroyImageView(V.dev, t.view, nullptr); vkDestroyImage(V.dev, t.img, nullptr); vkFreeMemory(V.dev, t.mem, nullptr); }
        });
    }
    // textures sampled with anisotropic filtering in this flush get a mip chain first (once)
    void prepare_mips(const GpuBatch* batches, size_t nb) {
        if (aniso <= 1.0f) return;
        for (size_t i = 0; i < nb; i++) {
            const GpuState& st = batches[i].st;
            if (!(st.kind == 2 && st.tme && st.tex && st.bil && st.wms < 2 && st.wmt < 2) || st.tex >= texs.size()) continue;
            Img& src = texs[st.tex];
            if (!src.img || src.levels > 1 || src.w < 32 || src.h < 32) continue;   // (small ones: level 0 only)
            auto cp = cpu_px.find(st.tex);
            static const bool gpu_mips = std::getenv("MVP_VK_GPU_MIPS") != nullptr;
            if (V.portability && !gpu_mips && cp == cpu_px.end()) continue;   // (made before filtering was on: level 0 only)
            Img m;
            if (!pooled_img(m, src.w, src.h, mip_levels(src.w, src.h))) continue;
            if (cp != cpu_px.end()) { upload_mips_cpu(m, cp->second.data()); cpu_px.erase(cp); }
            else {
                barrier();
                VkImageCopy c{};
                c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.extent = {(uint32_t)src.w, (uint32_t)src.h, 1};
                vkCmdCopyImage(cmd(), src.img, VK_IMAGE_LAYOUT_GENERAL, m.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                gen_mips(m);
            }
            recycle(src);
            src = m;
        }
    }
    unsigned tex_create(const u32* px, int tw, int th) override {
        Img t;
        const int lv = 1;           // mip chains are made when anisotropic filtering first samples a texture (prepare_mips)
        if (!pooled_img(t, tw, th, lv)) return 0;
        upload_queued(t, px);
        unsigned tid;
        if (!free_ids.empty()) { tid = free_ids.back(); free_ids.pop_back(); texs[tid] = t; }
        else { tid = (unsigned)texs.size(); texs.push_back(t); }
        // MoltenVK: keep the pixels of textures that may need a mip chain, which is then averaged on the CPU
        static const bool gpu_mips = std::getenv("MVP_VK_GPU_MIPS") != nullptr;
        if (V.portability && !gpu_mips && aniso > 1.0f && tw >= 32 && th >= 32 && px) cpu_px[tid].assign(px, px + (size_t)tw * th);
        return tid;
    }
    void tex_free(unsigned tid) override { if (tid && tid < texs.size()) free_later.push_back(tid); }
    void end_flush() override {
        for (unsigned tid : free_later) {
            recycle(texs[tid]); texs[tid] = Img(); free_ids.push_back(tid); cpu_px.erase(tid);
        }
        free_later.clear();
    }
    void set_aniso(float level) override {
        // MoltenVK (Vulkan on a Mac, where Metal is the default): no anisotropic filtering. Building mip chains is
        // slow there and its anisotropic sampling showed the 3D scene black in the home-run derby
        if (V.portability) level = 0;
        if (level == aniso && s_tex) return;
        aniso = level;
        if (s_tex) { VkSampler s = s_tex; trash([s] { vkDestroySampler(V.dev, s, nullptr); }); }
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.maxLod = 16.0f;
        if (level > 1.0f && V.aniso_ok) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = std::min(level, V.props.limits.maxSamplerAnisotropy); }
        vkCreateSampler(V.dev, &si, nullptr, &s_tex);
    }
    uintptr_t target_handle() override { return (uintptr_t)col.img; }

    void execute(const float* verts, size_t nverts, const GpuBatch* batches, size_t nb) override {
        flush_uploads();
        prepare_mips(batches, nb);
        const size_t bytes = nverts * GPU_VTX_FLOATS * sizeof(float);
        VkBuffer vb; VkDeviceSize voff;
        u8* p = arena(bytes, std::max<size_t>(V.props.limits.minStorageBufferOffsetAlignment, 16), vb, voff);
        if (!p) return;
        std::memcpy(p, verts, bytes);
        VkDescriptorSet set0 = alloc_set(dsl_gs0);
        write_buf(set0, 0, vb, voff, std::max<size_t>(bytes, 4));
        write_img(set0, 1, dst.view, V.s_near);
        std::unordered_map<u32, VkDescriptorSet> tex_sets;
        VkDescriptorSet date_set = VK_NULL_HANDLE;
        VkCommandBuffer cb = cmd();
        bool in_pass = false;
        VkPipeline bound = VK_NULL_HANDLE;
        VkDescriptorSet bound_tex = VK_NULL_HANDLE;
        int sc[4] = {-1, -1, -1, -1};                      // the scissor last set in this pass
        auto open = [&] {
            if (in_pass) return;
            sc[0] = -1;
            barrier();
            begin_rp(rp_target, fb, w, h);
            viewport(0, 0, w, h);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_gs, 0, 1, &set0, 0, nullptr);
            in_pass = true; bound = VK_NULL_HANDLE; bound_tex = VK_NULL_HANDLE;
        };
        auto close = [&] { if (in_pass) { vkCmdEndRenderPass(cb); in_pass = false; } };
        static const u32 kPer[3] = {1, 2, 3};
        for (size_t bi = 0; bi < nb; bi++) {
            const GpuBatch& b = batches[bi];
            const GpuState& st = b.st;
            const bool bbox_ok = b.bx1 > b.bx0 && b.by1 > b.by0;
            if ((b.copy || b.date) && bbox_ok) {          // copy of the target under the primitive(s)
                close(); barrier();
                VkImageCopy c{};
                c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.srcOffset = c.dstOffset = {b.bx0, b.by0, 0};
                c.extent = {(uint32_t)(b.bx1 - b.bx0), (uint32_t)(b.by1 - b.by0), 1};
                vkCmdCopyImage(cb, col.img, VK_IMAGE_LAYOUT_GENERAL, dst.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                g_gpu_stat_copies++;
            }
            int x0 = st.sx0 * scale, y0 = st.sy0 * scale;
            int x1 = std::min((st.sx1 + 1) * scale, w), y1 = std::min((st.sy1 + 1) * scale, h);
            if (x1 <= x0 || y1 <= y0) continue;
            if (b.date && !bbox_ok) continue;
            open();
            // the GS state, packed as vk/common.glsl unpacks it
            const int hx = std::max<int>(st.hsx, 1), hy = std::max<int>(st.hsy, 1);
            int rx0, rx1, ry0, ry1;
            if (st.wms == 2) { rx0 = st.minu * hx; rx1 = st.maxu * hx + hx - 1; } else { rx0 = st.minu; rx1 = st.maxu; }
            if (st.wmt == 2) { ry0 = st.minv * hy; ry1 = st.maxv * hy + hy - 1; } else { ry0 = st.minv; ry1 = st.maxv; }
            const u32 hw = (aniso > 1.0f && st.kind == 2 && st.tme && st.tex && st.bil && st.wms < 2 && st.wmt < 2) ? 1 : 0;
            const u32 date_u = (st.test & (1 << 14)) ? ((st.test >> 15) & 1) + 1 : 0;
            // blend: as setup_blend() in the GL backend
            int blend = 0; u32 bmode;
            if (!st.abe) bmode = 0;
            else if (b.copy == 2) bmode = 2;
            else {
                bmode = 1;
                u32 A = st.alpha & 3, B = (st.alpha >> 2) & 3, D = (st.alpha >> 6) & 3;
                bool neg_s = (B == 0 && A != 0 && D != 0), neg_d = (B == 1 && A != 1 && D != 1);
                blend = neg_s && neg_d ? 5 : (neg_s && !neg_d) ? 3 : (!neg_s && neg_d) ? 4 : 2;
            }
            auto f0_of = [&](u32 apass, u32 date) {
                return (u32)st.tme | (u32)st.fst << 1 | (u32)(st.iip || st.kind == 3 ? 1 : 0) << 2 | (u32)st.fge << 3 | (u32)(st.tfx & 3) << 4 |
                       (u32)st.tcc << 6 | (u32)st.bil << 7 | hw << 8 | (u32)(st.wms & 3) << 9 | (u32)(st.wmt & 3) << 11 | apass << 13 |
                       (u32)st.fba << 14 | (st.alpha & 3) << 15 | ((st.alpha >> 2) & 3) << 17 | ((st.alpha >> 4) & 3) << 19 |
                       ((st.alpha >> 6) & 3) << 21 | bmode << 23 | date << 25 | (u32)st.colclamp << 27 | kPer[b.mode] << 28;
            };
            const bool z16 = st.zpsm == 0x32 || st.zpsm == 0x3a;
            const u32 zcode = z16 ? 2 : st.zpsm == 0x31 ? 1 : 0;
            auto f1_of = [&](int atst, u32 aref) {
                return (u32)(atst + 1) | aref << 4 | ((st.alpha >> 8) & 0xff) << 12 | (u32)std::min(hx, 31) << 20 | (u32)std::min(hy, 31) << 25 | zcode << 30;
            };
            GsPC u;
            u.first = b.first;
            u.ts = (u32)(std::max<int>(st.tex_w, 1) * hx) | (u32)(std::max<int>(st.tex_h, 1) * hy) << 16;
            u.rx = (u32)(rx0 & 0xffff) | (u32)(rx1 & 0xffff) << 16; u.ry = (u32)(ry0 & 0xffff) | (u32)(ry1 & 0xffff) << 16;
            u.fog = st.fogcol & 0xffffff;
            u32 date_now = date_u;
            u.f0 = f0_of(0, date_now); u.f1 = f1_of(-1, 0);
            // texture set
            const u32 tid = st.tme && st.tex && st.tex < texs.size() && texs[st.tex].img ? st.tex : 0;
            VkDescriptorSet ts;
            auto f = tex_sets.find(tid);
            if (f != tex_sets.end()) ts = f->second;
            else { ts = alloc_set(dsl_tex); write_img(ts, 0, tid ? texs[tid].view : V.dummy.view, s_tex); tex_sets[tid] = ts; }
            if (b.date) {
                // stencil = 1 where the destination alpha passes the test (from the copy taken above)
                VkClearAttachment ca{VK_IMAGE_ASPECT_STENCIL_BIT, 0, {}};
                ca.clearValue.depthStencil = {0.0f, 0};
                VkClearRect cr{{{b.bx0, b.by0}, {(uint32_t)(b.bx1 - b.bx0), (uint32_t)(b.by1 - b.by0)}}, 0, 1};
                vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
                if (!date_set) date_set = misc_set(dst.view, V.s_near);
                scissor(b.bx0, b.by0, b.bx1 - b.bx0, b.by1 - b.by0);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_date);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pl_misc, 0, 1, &date_set, 0, nullptr);
                QuadPC q = full_quad(); int datm = (int)((st.test >> 15) & 1); std::memcpy(&q.p0[0], &datm, 4);
                quad_draw(q);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_gs, 0, 1, &set0, 0, nullptr);
                bound = p_date; bound_tex = VK_NULL_HANDLE;
                date_now = 0;
            }
            if (ts != bound_tex) { vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_gs, 1, 1, &ts, 0, nullptr); bound_tex = ts; }
            if (b.date || x0 != sc[0] || y0 != sc[1] || x1 != sc[2] || y1 != sc[3]) {
                scissor(x0, y0, x1 - x0, y1 - y0); sc[0] = x0; sc[1] = y0; sc[2] = x1; sc[3] = y1;
            }
            const bool zte = (st.test >> 16) & 1;
            const int zf = zte ? (int)((st.test >> 17) & 3) + 1 : 0;
            const bool zw = zte && !st.zmsk;
            const u32 m = st.fbmsk;
            auto cmask = [&](bool alpha) {
                return ((m & 0xff) != 0xff ? 1u : 0u) | ((m & 0xff00) != 0xff00 ? 2u : 0u) | ((m & 0xff0000) != 0xff0000 ? 4u : 0u) |
                       (alpha && (m & 0xff000000u) != 0xff000000u ? 8u : 0u);
            };
            auto draw = [&](u32 mask, bool zwrite, bool writes_alpha) {
                const int sten = b.date ? (b.date == 2 && writes_alpha ? 3 : 2) : 0;
                VkPipeline pp = gs_pipe(b.mode, blend, mask, zf, zwrite, sten);
                if (pp != bound) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp); bound = pp; }
                vkCmdPushConstants(cb, pl_gs, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof u, &u);
                vkCmdDraw(cb, b.count, 1, b.first, 0);
            };
            const bool ate = (st.test & 1) != 0;
            const u32 atst = (st.test >> 1) & 7, afail = (st.test >> 12) & 3;
            const u32 aref = (st.test >> 4) & 0xff;
            if (!ate || atst == 1) { u.f0 = f0_of(0, date_now); u.f1 = f1_of(-1, 0); draw(cmask(true), zw, true); continue; }
            if (atst != 0) { u.f0 = f0_of(1, date_now); u.f1 = f1_of((int)atst, aref); draw(cmask(true), zw, true); }
            if (afail != 0) {                                   // pixels that fail: partial writes
                u.f0 = f0_of(0, date_now); u.f1 = f1_of((int)atst, aref);
                if (afail == 1) draw(cmask(true), false, true);
                else if (afail == 2) draw(0, zw, false);
                else draw(cmask(false), false, false);
            }
        }
        close();
        barrier();
    }
};
static VkBackend* s_vkb = nullptr;
static bool s_vk_ready = false;
GpuBackend* gpu_backend_vulkan() {
    if (s_vkb) return s_vkb;
    if (!s_vk_ready) return nullptr;            // the Vulkan presenter isn't running
    VkBackend* b = new VkBackend();
    if (!b->init()) { delete b; return nullptr; }
    return s_vkb = b;
}

// ================================================================ presenter
class VkPresenter : public Presenter {
public:
    SDL_Window* win = nullptr;
#ifdef __APPLE__
    SDL_MetalView mview = nullptr;
#endif
    VkSwapchainKHR swap = VK_NULL_HANDLE;
    VkFormat swap_fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D ext{0, 0};
    std::vector<VkImage> simg; std::vector<VkImageView> sview; std::vector<VkFramebuffer> sfb; std::vector<VkSemaphore> sdone;
    VkRenderPass rp_off = VK_NULL_HANDLE, rp_swap = VK_NULL_HANDLE;
    VkPipeline p_fxaa = VK_NULL_HANDLE, p_resample = VK_NULL_HANDLE, p_final = VK_NULL_HANDLE, p_ui = VK_NULL_HANDLE;
    VkShaderModule m_quad, m_fxaa, m_resample, m_final, m_uiv, m_uif;
    Img game, fb1i, fb2i, font, ui_img[kUiImages];
    VkFramebuffer fb1 = VK_NULL_HANDLE, fb2 = VK_NULL_HANDLE;
    bool have_game = false, vsync = false, want_recreate = false, can_capture = false;
    std::vector<u32> captured; int cap_w = 0, cap_h = 0;
    std::vector<float> uiv;

    const char* name() const override { return "Vulkan"; }
    bool init(SDL_Window* w) {
        win = w;
        if (!vk_load_loader()) return false;
        // instance
        std::vector<const char*> exts;
        unsigned n = 0;
#ifdef __APPLE__
        exts = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME};
#else
        SDL_Vulkan_GetInstanceExtensions(win, &n, nullptr);
        exts.resize(n);
        SDL_Vulkan_GetInstanceExtensions(win, &n, exts.data());
#endif
        uint32_t ne = 0; vkEnumerateInstanceExtensionProperties(nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ie(ne); vkEnumerateInstanceExtensionProperties(nullptr, &ne, ie.data());
        bool portability = false;
        for (auto& e : ie) if (!std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) portability = true;
        if (portability) exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "MVP Baseball 2005"; app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ii.pApplicationInfo = &app; ii.enabledExtensionCount = (uint32_t)exts.size(); ii.ppEnabledExtensionNames = exts.data();
        if (portability) ii.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        VKCHECK(vkCreateInstance(&ii, nullptr, &V.inst));
        if (!vk_load_instance(V.inst)) return false;
        // surface
#ifdef __APPLE__
        mview = SDL_Metal_CreateView(win);
        if (!mview) return false;
        VkMetalSurfaceCreateInfoEXT si{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
        si.pLayer = (const CAMetalLayer*)SDL_Metal_GetLayer(mview);
        VKCHECK(vkCreateMetalSurfaceEXT(V.inst, &si, nullptr, &V.surface));
#else
        if (!SDL_Vulkan_CreateSurface(win, V.inst, &V.surface)) { fprintf(stderr, "[vulkan] surface: %s\n", SDL_GetError()); return false; }
#endif
        // device: the first with graphics + present, dual-source blending and depth32/stencil8
        uint32_t np = 0; vkEnumeratePhysicalDevices(V.inst, &np, nullptr);
        std::vector<VkPhysicalDevice> pds(np); vkEnumeratePhysicalDevices(V.inst, &np, pds.data());
        for (VkPhysicalDevice pd : pds) {
            VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(pd, &f);
            VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
            // (no dual-source blending: blended primitives use the target copy instead - slower, same result)
            VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) { rlog("vulkan", "%s: no D32_S8 depth buffer", pp.deviceName); continue; }
            uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
            std::vector<VkQueueFamilyProperties> qf(nq); vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
            for (uint32_t q = 0; q < nq; q++) {
                VkBool32 pres = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, V.surface, &pres);
                if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) { V.phys = pd; V.qfam = q; break; }
            }
            if (V.phys) { V.aniso_ok = f.samplerAnisotropy; V.dual = f.dualSrcBlend; break; }
            rlog("vulkan", "%s: no queue that can present to the window", pp.deviceName);
        }
        if (!V.phys) { fprintf(stderr, "[vulkan] no suitable GPU\n"); return false; }
        vkGetPhysicalDeviceProperties(V.phys, &V.props);
        vkGetPhysicalDeviceMemoryProperties(V.phys, &V.mprops);
        if (V.props.limits.maxPushConstantsSize < sizeof(GsPC)) { fprintf(stderr, "[vulkan] push constants too small\n"); return false; }
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = V.qfam; qi.queueCount = 1; qi.pQueuePriorities = &prio;
        VkPhysicalDeviceFeatures en{}; en.dualSrcBlend = V.dual; en.samplerAnisotropy = V.aniso_ok;
        if (!V.dual) rlog("vulkan", "%s has no dual-source blending: blending through target copies", V.props.deviceName);
        std::vector<const char*> dext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        uint32_t nde = 0; vkEnumerateDeviceExtensionProperties(V.phys, nullptr, &nde, nullptr);
        std::vector<VkExtensionProperties> de(nde); vkEnumerateDeviceExtensionProperties(V.phys, nullptr, &nde, de.data());
        for (auto& e : de) if (!std::strcmp(e.extensionName, "VK_KHR_portability_subset")) { dext.push_back("VK_KHR_portability_subset"); V.portability = true; }
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.pEnabledFeatures = &en;
        di.enabledExtensionCount = (uint32_t)dext.size(); di.ppEnabledExtensionNames = dext.data();
        VKCHECK(vkCreateDevice(V.phys, &di, nullptr, &V.dev));
        vkGetDeviceQueue(V.dev, V.qfam, 0, &V.queue);
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; ci.queueFamilyIndex = V.qfam;
        VKCHECK(vkCreateCommandPool(V.dev, &ci, nullptr, &V.cpool));
        for (Slot& s : V.slots) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = V.cpool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
            VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, &s.cb));
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VKCHECK(vkCreateFence(V.dev, &fi, nullptr, &s.fence));
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VKCHECK(vkCreateSemaphore(V.dev, &sci, nullptr, &s.acquired));
        }
        // samplers, the misc layout, dummies
        VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sm.addressModeU = sm.addressModeV = sm.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sm.magFilter = sm.minFilter = VK_FILTER_NEAREST; sm.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        VKCHECK(vkCreateSampler(V.dev, &sm, nullptr, &V.s_near));
        sm.magFilter = sm.minFilter = VK_FILTER_LINEAR;
        VKCHECK(vkCreateSampler(V.dev, &sm, nullptr, &V.s_lin));
        sm.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; sm.maxLod = 16.0f;
        VKCHECK(vkCreateSampler(V.dev, &sm, nullptr, &V.s_mip));
        VkDescriptorSetLayoutBinding b[3]{};
        for (int i = 0; i < 3; i++) { b[i].binding = i; b[i].descriptorCount = 1; }
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].descriptorType = b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].stageFlags = b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 3; li.pBindings = b;
        VKCHECK(vkCreateDescriptorSetLayout(V.dev, &li, nullptr, &V.dsl_misc));
        VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(QuadPC)};
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 1; pi.pSetLayouts = &V.dsl_misc; pi.pushConstantRangeCount = 1; pi.pPushConstantRanges = &pr;
        VKCHECK(vkCreatePipelineLayout(V.dev, &pi, nullptr, &V.pl_misc));
        u8* dm = nullptr;
        if (!make_buffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, V.dummy_buf, V.dummy_mem, &dm)) return false;
        std::memset(dm, 0, 256);
        u32 zero = 0;
        if (!make_img(V.dummy, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
        upload(V.dummy, &zero, 4);
        // presenter passes
        m_quad = SHADER(kQuadVert); m_fxaa = SHADER(kFxaaFrag); m_resample = SHADER(kResampleFrag); m_final = SHADER(kFinalFrag);
        m_uiv = SHADER(kUiVert); m_uif = SHADER(kUiFrag);
        if (!make_swapchain()) return false;
        const VkFormat c8 = VK_FORMAT_R8G8B8A8_UNORM;
        rp_off = make_rp(&c8, 1, VK_FORMAT_UNDEFINED, false, VK_IMAGE_LAYOUT_GENERAL);
        PipeDesc d; d.vs = m_quad; d.layout = V.pl_misc; d.rp = rp_off;
        d.fs = m_fxaa; p_fxaa = make_pipe(d);
        d.fs = m_resample; p_resample = make_pipe(d);
        if (!make_swap_pipes() || !p_fxaa || !p_resample) return false;
        const UiAtlas& a = ui_atlas();                       // drawn at 56 px: small text samples the mips
        if (!make_img(font, a.w, a.h, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mip_levels(a.w, a.h))) return false;
        upload(font, a.px.data(), 1);
        s_vk_ready = true;
        rlog("vulkan", "presenter: %s (Vulkan %u.%u)", V.props.deviceName, VK_VERSION_MAJOR(V.props.apiVersion), VK_VERSION_MINOR(V.props.apiVersion));
        return true;
    }
    bool make_swap_pipes() {
        if (rp_swap) return true;
        rp_swap = make_rp(&swap_fmt, 1, VK_FORMAT_UNDEFINED, true, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        PipeDesc d; d.vs = m_quad; d.layout = V.pl_misc; d.rp = rp_swap;
        d.fs = m_final; p_final = make_pipe(d);
        d.vs = m_uiv; d.fs = m_uif; d.blend = 1; d.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; p_ui = make_pipe(d);
        return p_final && p_ui;
    }
    void drawable_size(int& w, int& h) override {
#ifdef __APPLE__
        SDL_Metal_GetDrawableSize(win, &w, &h);
#else
        SDL_Vulkan_GetDrawableSize(win, &w, &h);
#endif
    }
    void destroy_swapchain() {
        for (VkFramebuffer f : sfb) vkDestroyFramebuffer(V.dev, f, nullptr);
        for (VkImageView v : sview) vkDestroyImageView(V.dev, v, nullptr);
        for (VkSemaphore s : sdone) vkDestroySemaphore(V.dev, s, nullptr);
        sfb.clear(); sview.clear(); simg.clear(); sdone.clear();
    }
    bool make_swapchain() {
        VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.phys, V.surface, &caps);
        uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(V.phys, V.surface, &nf, nullptr);
        std::vector<VkSurfaceFormatKHR> fs(nf); vkGetPhysicalDeviceSurfaceFormatsKHR(V.phys, V.surface, &nf, fs.data());
        VkSurfaceFormatKHR fmt = fs.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fs[0];
        for (auto& f : fs) if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { fmt = f; break; }
        if (rp_swap && fmt.format != swap_fmt) fmt.format = swap_fmt;      // keep the pipelines' format
        swap_fmt = fmt.format;
        int dw, dh; drawable_size(dw, dh);
        ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{(uint32_t)dw, (uint32_t)dh};
        ext.width = std::max(1u, std::min(std::max(ext.width, caps.minImageExtent.width), caps.maxImageExtent.width));
        ext.height = std::max(1u, std::min(std::max(ext.height, caps.minImageExtent.height), caps.maxImageExtent.height));
        uint32_t npm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(V.phys, V.surface, &npm, nullptr);
        std::vector<VkPresentModeKHR> pms(npm); vkGetPhysicalDeviceSurfacePresentModesKHR(V.phys, V.surface, &npm, pms.data());
        VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
        if (!vsync) for (VkPresentModeKHR m : pms) { if (m == VK_PRESENT_MODE_MAILBOX_KHR) { pm = m; break; } if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) pm = m; }
        VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        si.surface = V.surface; si.minImageCount = std::max(caps.minImageCount, std::min(3u, caps.maxImageCount ? caps.maxImageCount : 3u));
        si.imageFormat = fmt.format; si.imageColorSpace = fmt.colorSpace; si.imageExtent = ext; si.imageArrayLayers = 1;
        can_capture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        si.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (can_capture ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
        si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; si.preTransform = caps.currentTransform;
        si.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; si.presentMode = pm; si.clipped = VK_TRUE; si.oldSwapchain = swap;
        {
            std::string ml; for (VkPresentModeKHR m : pms) ml += std::to_string((int)m) + " ";
            rlog("vulkan", "swapchain %ux%u, present mode %d (available: %s), vsync %d", ext.width, ext.height, (int)pm, ml.c_str(), (int)vsync);
        }
        VkSwapchainKHR ns = VK_NULL_HANDLE;
        VKCHECK(vkCreateSwapchainKHR(V.dev, &si, nullptr, &ns));
        if (swap) { destroy_swapchain(); vkDestroySwapchainKHR(V.dev, swap, nullptr); }
        swap = ns;
        uint32_t ni = 0; vkGetSwapchainImagesKHR(V.dev, swap, &ni, nullptr);
        simg.resize(ni); vkGetSwapchainImagesKHR(V.dev, swap, &ni, simg.data());
        if (!make_swap_pipes()) return false;
        for (VkImage im : simg) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = im; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = swap_fmt; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView v; VKCHECK(vkCreateImageView(V.dev, &vi, nullptr, &v));
            sview.push_back(v);
            sfb.push_back(make_fb(rp_swap, &v, 1, ext.width, ext.height));
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore s; VKCHECK(vkCreateSemaphore(V.dev, &sci, nullptr, &s));
            sdone.push_back(s);
        }
        return true;
    }
    void set_vsync(bool on) override { if (on != vsync) { vsync = on; want_recreate = true; } }
    void set_ui_image(int id, const u32* px, int w, int h) override {
        if (id < 0 || id >= kUiImages) return;
        free_img(ui_img[id]);
        if (!w) return;
        make_img(ui_img[id], w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        upload(ui_img[id], px, 4);
    }
    void game_img(int w, int h) {
        if (game.img && game.w == w && game.h == h) return;
        free_img(game);
        make_img(game, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    void set_game(const u32* px, int w, int h) override { game_img(w, h); upload(game, px, 4); have_game = true; }
    bool set_game_gpu(uintptr_t handle, int x0, int y0, int w, int h) override {
        if (!handle || w <= 0 || h <= 0) return false;
        game_img(w, h);
        barrier();
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.srcOffset = {x0, y0, 0}; c.extent = {(uint32_t)w, (uint32_t)h, 1};
        vkCmdCopyImage(cmd(), (VkImage)handle, VK_IMAGE_LAYOUT_GENERAL, game.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        barrier();
        have_game = true;
        return true;
    }
    void offscreen(Img& i, VkFramebuffer& fb, int w, int h) {
        if (i.img && i.w == w && i.h == h) return;
        free_img(i);
        if (fb) { VkFramebuffer f = fb; trash([f] { vkDestroyFramebuffer(V.dev, f, nullptr); }); }
        make_img(i, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        fb = make_fb(rp_off, &i.view, 1, w, h);
    }
    // finish this frame's command buffer; the next slot becomes current once its previous frame has finished
    void next_slot() {
        V.cur ^= 1;
        Slot& s = V.slots[V.cur];
        const double tf = now_ms();
        vkWaitForFences(V.dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
        s_t_fence += now_ms() - tf;
        static int frames = 0; static double t_last = now_ms();
        if (++frames == 300) {
            static const bool prof = std::getenv("MVP_VK_PROF") != nullptr;
            if (prof) rlog("vulkan", "300 frames in %.0f ms: acquire %.0f, frame fence %.0f, %d read-back waits %.0f ms",
                           now_ms() - t_last, s_t_acq, s_t_fence, s_n_sync, s_t_sync);
            frames = 0; t_last = now_ms(); s_t_acq = s_t_fence = s_t_sync = 0; s_n_sync = 0;
        }
        vkResetCommandBuffer(s.cb, 0);
        reset_slot(s);
    }
    void frame(int dw, int dh, const DstRect& dst, const PostParams& pp, const UiBatch& ui) override {
        if (dw <= 0 || dh <= 0) return;
        if (want_recreate || (int)ext.width != dw || (int)ext.height != dh) {
            vkDeviceWaitIdle(V.dev);
            want_recreate = false;
            if (!make_swapchain()) return;
        }
        Slot& s = V.slots[V.cur];
        uint32_t idx = 0;
        const double ta = now_ms();
        VkResult ar = vkAcquireNextImageKHR(V.dev, swap, UINT64_MAX, s.acquired, VK_NULL_HANDLE, &idx);
        s_t_acq += now_ms() - ta;
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) { want_recreate = true; return; }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return;
        flush_uploads();
        VkCommandBuffer cb = cmd();
        const bool pic = have_game && game.img && dst.w > 0 && dst.h > 0;
        Img* src = &game; int sw = game.w, sh = game.h;
        if (pic) {
            barrier();
            if (pp.aa) {                                     // 1. FXAA at the picture's own resolution
                offscreen(fb1i, fb1, sw, sh);
                VkDescriptorSet set = misc_set(src->view, V.s_lin);
                begin_rp(rp_off, fb1, sw, sh); viewport(0, 0, sw, sh); scissor(0, 0, sw, sh);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_fxaa);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pl_misc, 0, 1, &set, 0, nullptr);
                QuadPC q = full_quad(); q.p0[0] = 1.0f / sw; q.p0[1] = 1.0f / sh; q.p0[2] = pp.aa >= 2 ? 16.0f : 8.0f; q.p0[3] = pp.aa >= 2 ? 1.0f / 256 : 1.0f / 128;
                quad_draw(q);
                vkCmdEndRenderPass(cb);
                barrier();
                src = &fb1i;
            }
            offscreen(fb2i, fb2, dst.w, dst.h);              // 2. resample to the output size
            VkDescriptorSet set = misc_set(src->view, pp.scale_filter == 0 ? V.s_near : V.s_lin);
            begin_rp(rp_off, fb2, dst.w, dst.h); viewport(0, 0, dst.w, dst.h); scissor(0, 0, dst.w, dst.h);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_resample);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pl_misc, 0, 1, &set, 0, nullptr);
            int mode = pp.scale_filter;
            if (mode == 3 && sw <= dst.w && sh <= dst.h) mode = 2;
            QuadPC q = full_quad(); q.p0[0] = (float)sw; q.p0[1] = (float)sh; q.p0[2] = (float)dst.w; q.p0[3] = (float)dst.h; std::memcpy(&q.p1[0], &mode, 4);
            quad_draw(q);
            vkCmdEndRenderPass(cb);
        }
        barrier();
        // UI vertices for the whole frame
        VkBuffer ub = VK_NULL_HANDLE; VkDeviceSize uoff = 0; size_t ubytes = 0;
        if (!ui.q.empty()) {
            build_ui(ui);
            ubytes = uiv.size() * sizeof(float);
            u8* p = arena(ubytes, std::max<size_t>(V.props.limits.minStorageBufferOffsetAlignment, 16), ub, uoff);
            if (p) std::memcpy(p, uiv.data(), ubytes); else ub = VK_NULL_HANDLE;
        }
        // 3. the window: UI under the picture, picture (sharpen + colour + CRT), UI
        begin_rp(rp_swap, sfb[idx], ext.width, ext.height, true);
        viewport(0, 0, ext.width, ext.height); scissor(0, 0, ext.width, ext.height);
        if (ui.under) draw_ui(ui, ub, uoff, ubytes, 0, ui.under);
        if (pic) {
            VkDescriptorSet set = misc_set(fb2i.view, V.s_lin);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_final);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pl_misc, 0, 1, &set, 0, nullptr);
            QuadPC q{};
            q.rect[0] = (float)dst.x / dw * 2 - 1; q.rect[2] = (float)(dst.x + dst.w) / dw * 2 - 1;
            q.rect[1] = (float)dst.y / dh * 2 - 1; q.rect[3] = (float)(dst.y + dst.h) / dh * 2 - 1;
            q.uv[2] = 1; q.uv[3] = 1;
            q.p0[0] = (float)dst.w; q.p0[1] = (float)dst.h; q.p0[2] = pp.sharpen; q.p0[3] = pp.gamma;
            q.p1[0] = pp.brightness; q.p1[1] = pp.contrast; q.p1[2] = pp.saturation;
            int ip[2] = {pp.crt, pp.native_lines}; std::memcpy(&q.p2[0], ip, 8);
            quad_draw(q);
        }
        draw_ui(ui, ub, uoff, ubytes, ui.under, ui.q.size());
        vkCmdEndRenderPass(cb);
        bool cap = capture && can_capture;
        capture = false;
        u8* rb = nullptr;
        if (cap && (rb = readback((size_t)ext.width * ext.height * 4))) {
            layout(simg[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1);
            VkBufferImageCopy r{}; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.imageExtent = {ext.width, ext.height, 1};
            vkCmdCopyImageToBuffer(cb, simg[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.rb, 1, &r);
            layout(simg[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 1);
        }
        vkEndCommandBuffer(cb); V.recording = false;
        vkResetFences(V.dev, 1, &s.fence);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s.acquired; si.pWaitDstStageMask = &ws;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sdone[idx];
        vkQueueSubmit(V.queue, 1, &si, s.fence);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &sdone[idx]; pi.swapchainCount = 1; pi.pSwapchains = &swap; pi.pImageIndices = &idx;
        VkResult pr = vkQueuePresentKHR(V.queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) want_recreate = true;
        if (rb) {
            vkWaitForFences(V.dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
            cap_w = ext.width; cap_h = ext.height;
            captured.resize((size_t)cap_w * cap_h);
            const u32* p = (const u32*)rb;
            const bool bgra = swap_fmt == VK_FORMAT_B8G8R8A8_UNORM || swap_fmt == VK_FORMAT_B8G8R8A8_SRGB;
            for (size_t i = 0; i < captured.size(); i++) { u32 c = p[i]; captured[i] = bgra ? (c & 0xff00ff00u) | ((c >> 16) & 0xff) | ((c & 0xff) << 16) : c; }
        }
        next_slot();
    }
    void build_ui(const UiBatch& ui) {
        uiv.clear();
        uiv.reserve(ui.q.size() * 6 * 15);
        for (const UiQuad& q : ui.q) {
            float r = ((q.color >> 16) & 0xff) / 255.0f, g = ((q.color >> 8) & 0xff) / 255.0f, b = (q.color & 0xff) / 255.0f, a = (q.color >> 24) / 255.0f;
            float hw = q.w * 0.5f, hh = q.h * 0.5f;
            const float cx[6] = {0, 1, 0, 1, 1, 0}, cy[6] = {0, 0, 1, 0, 1, 1};
            for (int i = 0; i < 6; i++) {
                float px = q.x + cx[i] * q.w, py = q.y + cy[i] * q.h;
                float u = q.u0 + (q.u1 - q.u0) * cx[i], v = q.v0 + (q.v1 - q.v0) * cy[i];
                float lx = (cx[i] - 0.5f) * q.w, ly = (cy[i] - 0.5f) * q.h;
                float v15[15] = {px, py, u, v, r, g, b, a, lx, ly, hw, hh, q.radius, q.soft, (float)q.kind};
                uiv.insert(uiv.end(), v15, v15 + 15);
            }
        }
    }
    void draw_ui(const UiBatch& ui, VkBuffer ub, VkDeviceSize uoff, size_t ubytes, size_t first, size_t last) {
        if (last <= first || !ub) return;
        VkCommandBuffer cb = cmd();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_ui);
        QuadPC q{}; q.p0[0] = (float)ext.width; q.p0[1] = (float)ext.height;
        vkCmdPushConstants(cb, V.pl_misc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof q, &q);
        size_t i = first;
        while (i < last) {                    // one draw per run of quads using the same image
            int img = ui.q[i].kind == UI_IMAGE ? (int)ui.q[i].soft : -1;
            size_t j = i + 1;
            while (j < last && (ui.q[j].kind == UI_IMAGE ? (int)ui.q[j].soft : -1) == img) j++;
            const bool ok = img < 0 || (img < kUiImages && ui_img[img].img);
            if (ok) {
                VkDescriptorSet set = misc_set(font.view, V.s_mip, img >= 0 ? ui_img[img].view : VK_NULL_HANDLE, V.s_lin, ub, uoff, ubytes);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pl_misc, 0, 1, &set, 0, nullptr);
                vkCmdDraw(cb, (uint32_t)((j - i) * 6), 1, (uint32_t)(i * 6), 0);
            }
            i = j;
        }
    }
    bool read_window(std::vector<u32>& px, int& w, int& h) override {
        if (captured.empty()) return false;
        px = captured; w = cap_w; h = cap_h; return true;
    }
};

Presenter* presenter_vk_create(SDL_Window* win) {
    VkPresenter* p = new VkPresenter();
    if (!p->init(win)) { fprintf(stderr, "[vulkan] not available - using OpenGL\n"); return nullptr; }   // (leaks the half-made device)
    return p;
}
#endif
