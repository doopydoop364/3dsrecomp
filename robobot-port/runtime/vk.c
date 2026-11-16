/* Vulkan backend for PICA200 rasterisation (see vkr.h). Selected with RB_RENDERER=vulkan.
 *
 * Surface cache: every colour or depth buffer the PICA renders into is a GPU image keyed by guest
 * address, size and format. Guest memory is the truth at native resolution:
 *  - a surface the GPU has drawn into is "dirty" and is written back (downloaded) only when something
 *    else needs those bytes: a GX transfer, copy or partial fill, a texture read, or the display;
 *  - a clean surface is re-checked against guest memory (hash) on its first use after each guest
 *    submission, and re-uploaded if the memory changed;
 *  - only one surface may cover a byte range: an overlapping new surface replaces the old one.
 * Textures are decoded on the CPU (pica.c's decoder) and cached by address, size, format and content.
 * RB_VK_VALIDATE=1 enables the Khronos validation layer; RB_VK_DEVICE=n picks a GPU. */
#include "host.h"
#include "vkr.h"
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

int vkr_on;
static const uint32_t vert_spv[] =
#include "pica_vert.inc"
;
static const uint32_t frag_spv[] =
#include "pica_frag.inc"
;

#define CHK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) fatal("vulkan: %s failed (%d)", #x, r_); } while (0)

static VkInstance inst;
static VkPhysicalDevice pdev;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkCommandPool cpool;
static VkCommandBuffer cmd;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties memprops;
static VkDescriptorSetLayout dsl;
static VkPipelineLayout playout;
static VkShaderModule vertmod, fragmod;
static VkSampler sampler;
static PFN_vkCmdPushDescriptorSetKHR push_desc;
static VkFormat d24fmt;
static int recording, in_render;
static uint64_t t_wait, t_fin, t_tex, t_surf;
static uint64_t vns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint64_t ring_gen, gen = 1, nsubmits, nsyncs, ndownloads, nuploads, ntexuploads;

/* ---- memory ---- */
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits >> i) & 1 && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    fatal("vulkan: no suitable memory type");
}
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize size, used; } Ring;
static Ring ring, rbk;
/* cached: VK_MEMORY_PROPERTY_HOST_CACHED_BIT for buffers the CPU reads (uncached reads are very slow) */
static void mkbuf(Ring *r, VkDeviceSize size, VkBufferUsageFlags use, VkMemoryPropertyFlags cached) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = use };
    CHK(vkCreateBuffer(dev, &bi, 0, &r->buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, r->buf, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | cached) };
    CHK(vkAllocateMemory(dev, &ai, 0, &r->mem));
    CHK(vkBindBufferMemory(dev, r->buf, r->mem, 0));
    CHK(vkMapMemory(dev, r->mem, 0, size, 0, (void **)&r->map));
    r->size = size; r->used = 0;
}

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkImageAspectFlags aspect; } Img;
static Img grave[1024]; static int ngrave;     /* destroyed after the GPU is idle */

/* ---- command recording ---- */
static void begin_cmd(void) {
    if (recording) return;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHK(vkBeginCommandBuffer(cmd, &bi));
    recording = 1;
}
static void barrier(void) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, 0, 0, 0);
}
static void end_render(void) {
    if (!in_render) return;
    vkCmdEndRendering(cmd);
    in_render = 0;
    barrier();
}

typedef struct Surf Surf;
typedef struct { Surf *s; VkDeviceSize off, soff; } Pending;
static Pending pend[64]; static int npend;
static void finish_downloads(void);

/* Submit everything recorded and wait for the GPU. */
static void submit_wait(void) {
    if (!recording) return;
    end_render();
    CHK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    CHK(vkQueueSubmit(queue, 1, &si, fence));
    uint64_t t0 = vns();
    CHK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    t_wait += vns() - t0;
    CHK(vkResetFences(dev, 1, &fence));
    CHK(vkResetCommandPool(dev, cpool, 0));
    recording = 0; nsubmits++;
    ring.used = 0; rbk.used = 0; ring_gen++;
    t0 = vns(); finish_downloads(); t_fin += vns() - t0;
    for (int i = 0; i < ngrave; i++) { vkDestroyImageView(dev, grave[i].view, 0); vkDestroyImage(dev, grave[i].img, 0); vkFreeMemory(dev, grave[i].mem, 0); }
    ngrave = 0;
}
static VkDeviceSize ring_alloc(Ring *r, VkDeviceSize n) {
    VkDeviceSize off = (r->used + 255) & ~(VkDeviceSize)255;
    if (off + n > r->size) {
        if (n > r->size) fatal("vulkan: %llu bytes do not fit the staging ring", (unsigned long long)n);
        submit_wait(); begin_cmd(); off = 0;
    }
    r->used = off + n;
    return off;
}

static void mkimg(Img *im, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags use, VkImageAspectFlags aspect) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
        .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    CHK(vkCreateImage(dev, &ci, 0, &im->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im->img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CHK(vkAllocateMemory(dev, &ai, 0, &im->mem));
    CHK(vkBindImageMemory(dev, im->img, im->mem, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = f, .subresourceRange = { aspect, 0, 1, 0, 1 } };
    CHK(vkCreateImageView(dev, &vi, 0, &im->view));
    im->aspect = aspect;
    begin_cmd(); end_render();
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im->img, .subresourceRange = { aspect, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}
static void bury(Img *im) {
    if (ngrave == 1024) submit_wait();
    grave[ngrave++] = *im; memset(im, 0, sizeof *im);
}

/* ---- guest pixel formats ---- */
static const uint32_t xl[8] = { 0, 1, 4, 5, 16, 17, 20, 21 }, yl[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
static const uint32_t cbpp[8] = { 4, 3, 2, 2, 2, 4, 4, 4 }, dbpp[4] = { 2, 3, 3, 4 };
/* Byte offset of pixel (x, y) in a tiled PICA buffer; y counts up from the bottom row. */
static uint32_t tile_off(uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint32_t bpp) {
    uint32_t yy = h - 1 - y;
    return (xl[x & 7] + yl[yy & 7] + (x & ~7u) * 8) * bpp + (yy & ~7u) * w * bpp;
}
static uint8_t x4(uint32_t v) { return (uint8_t)((v << 4) | v); }
static uint8_t x5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static void color_read(const uint8_t *p, uint32_t fmt, uint8_t c[4]) {
    uint32_t v = (uint32_t)(p[0] | p[1] << 8);
    switch (fmt) {
    case 0: c[0] = p[3]; c[1] = p[2]; c[2] = p[1]; c[3] = p[0]; return;
    case 1: c[0] = p[2]; c[1] = p[1]; c[2] = p[0]; c[3] = 255; return;
    case 2: c[0] = x5(v >> 11); c[1] = x5((v >> 6) & 31); c[2] = x5((v >> 1) & 31); c[3] = (v & 1) ? 255 : 0; return;
    case 3: c[0] = x5(v >> 11); c[1] = (uint8_t)((((v >> 5) & 63) << 2) | (((v >> 5) & 63) >> 4)); c[2] = x5(v & 31); c[3] = 255; return;
    default: c[0] = x4(v >> 12); c[1] = x4((v >> 8) & 15); c[2] = x4((v >> 4) & 15); c[3] = x4(v & 15); return;
    }
}
static void color_write(uint8_t *p, uint32_t fmt, const uint8_t c[4]) {
    uint32_t v;
    switch (fmt) {
    case 0: p[0] = c[3]; p[1] = c[2]; p[2] = c[1]; p[3] = c[0]; return;
    case 1: p[0] = c[2]; p[1] = c[1]; p[2] = c[0]; return;
    case 2: v = (uint32_t)(c[0] >> 3) << 11 | (uint32_t)(c[1] >> 3) << 6 | (uint32_t)(c[2] >> 3) << 1 | (uint32_t)(c[3] >> 7); break;
    case 3: v = (uint32_t)(c[0] >> 3) << 11 | (uint32_t)(c[1] >> 2) << 5 | (uint32_t)(c[2] >> 3); break;
    default: v = (uint32_t)(c[0] >> 4) << 12 | (uint32_t)(c[1] >> 4) << 8 | (uint32_t)(c[2] >> 4) << 4 | (uint32_t)(c[3] >> 4); break;
    }
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static uint64_t hash_mem(const uint8_t *p, size_t n) {
    uint64_t h[4] = { 0x9E3779B97F4A7C15ull ^ n, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull };
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int k = 0; k < 4; k++) { uint64_t w; memcpy(&w, p + i + 8 * k, 8); h[k] = (h[k] ^ w) * 0xff51afd7ed558ccdull; h[k] ^= h[k] >> 31; }
    for (; i < n; i++) h[0] = (h[0] ^ p[i]) * 0x100000001b3ull;
    return (h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7)) * 0x9E3779B97F4A7C15ull;
}

/* ---- surfaces ---- */
struct Surf { int used, depth; uint32_t addr, size, w, h, fmt; Img im; int dirty; uint64_t hash, checked, last; };
#define NSURF 64
static Surf surf[NSURF];
static int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return a < b + bn && b < a + an; }

static void queue_download(Surf *s) {
    begin_cmd(); end_render();
    VkDeviceSize n = (VkDeviceSize)s->w * s->h;
    Pending *p = &pend[npend];
    if (s->depth && s->fmt != 0) {
        p->off = ring_alloc(&rbk, n * 4); p->soff = ring_alloc(&rbk, n);
        VkBufferImageCopy c[2] = { { .bufferOffset = p->off, .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageExtent = { s->w, s->h, 1 } },
                                   { .bufferOffset = p->soff, .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 }, .imageExtent = { s->w, s->h, 1 } } };
        vkCmdCopyImageToBuffer(cmd, s->im.img, VK_IMAGE_LAYOUT_GENERAL, rbk.buf, 2, c);
    } else {
        p->off = ring_alloc(&rbk, n * 4);
        VkBufferImageCopy c = { .bufferOffset = p->off, .imageSubresource = { s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { s->w, s->h, 1 } };
        vkCmdCopyImageToBuffer(cmd, s->im.img, VK_IMAGE_LAYOUT_GENERAL, rbk.buf, 1, &c);
    }
    p->s = s; npend++;
    ndownloads++;
    if (npend == 64) submit_wait();
}
static void finish_downloads(void) {
    for (int i = 0; i < npend; i++) {
        Surf *s = pend[i].s;
        uint8_t *mem = (uint8_t *)(uintptr_t)s->addr;
        const uint8_t *src = rbk.map + pend[i].off, *st = rbk.map + pend[i].soff;
        for (uint32_t y = 0; y < s->h; y++)
            for (uint32_t x = 0; x < s->w; x++) {
                uint32_t k = y * s->w + x;
                if (!s->depth) { color_write(mem + tile_off(s->w, s->h, x, y, cbpp[s->fmt]), s->fmt, src + 4 * k); continue; }
                uint8_t *p = mem + tile_off(s->w, s->h, x, y, dbpp[s->fmt]);
                if (s->fmt == 0) { memcpy(p, src + 2 * k, 2); continue; }
                uint32_t d;
                if (d24fmt == VK_FORMAT_D24_UNORM_S8_UINT) { memcpy(&d, src + 4 * k, 4); d &= 0xffffff; }
                else { float f; memcpy(&f, src + 4 * k, 4); d = (uint32_t)(f * 16777215.0f + 0.5f); }
                p[0] = (uint8_t)d; p[1] = (uint8_t)(d >> 8); p[2] = (uint8_t)(d >> 16);
                if (s->fmt == 3) p[3] = st[k];
            }
        s->dirty = 0; s->hash = hash_mem(mem, s->size); s->checked = gen;
    }
    npend = 0;
}
static void upload_surface(Surf *s) {
    begin_cmd(); end_render();
    const uint8_t *mem = (const uint8_t *)(uintptr_t)s->addr;
    VkDeviceSize n = (VkDeviceSize)s->w * s->h;
    VkDeviceSize off = ring_alloc(&ring, n * 4), soff = 0;
    uint8_t *dst = ring.map + off, *sdst = 0;
    if (s->depth && s->fmt != 0) { soff = ring_alloc(&ring, n); sdst = ring.map + soff; }
    for (uint32_t y = 0; y < s->h; y++)
        for (uint32_t x = 0; x < s->w; x++) {
            uint32_t k = y * s->w + x;
            if (!s->depth) { color_read(mem + tile_off(s->w, s->h, x, y, cbpp[s->fmt]), s->fmt, dst + 4 * k); continue; }
            const uint8_t *p = mem + tile_off(s->w, s->h, x, y, dbpp[s->fmt]);
            if (s->fmt == 0) { memcpy(dst + 2 * k, p, 2); continue; }
            uint32_t d = (uint32_t)(p[0] | p[1] << 8 | p[2] << 16);
            if (d24fmt == VK_FORMAT_D24_UNORM_S8_UINT) memcpy(dst + 4 * k, &d, 4);
            else { float f = (float)d / 16777215.0f; memcpy(dst + 4 * k, &f, 4); }
            sdst[k] = s->fmt == 3 ? p[3] : 0;
        }
    if (s->depth && s->fmt != 0) {
        VkBufferImageCopy c[2] = { { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageExtent = { s->w, s->h, 1 } },
                                   { .bufferOffset = soff, .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 }, .imageExtent = { s->w, s->h, 1 } } };
        vkCmdCopyBufferToImage(cmd, ring.buf, s->im.img, VK_IMAGE_LAYOUT_GENERAL, 2, c);
    } else {
        VkBufferImageCopy c = { .bufferOffset = off, .imageSubresource = { s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { s->w, s->h, 1 } };
        vkCmdCopyBufferToImage(cmd, ring.buf, s->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    barrier();
    nuploads++;
}
static void drop_surface(Surf *s) { bury(&s->im); s->used = 0; }

/* Write back every dirty surface overlapping [addr, addr+size). will_write: the caller is about to change
 * those bytes, so clean surfaces there must be re-checked before their next use. */
void vkr_sync_range(uint32_t addr, uint32_t size, int will_write) {
    if (!vkr_on) return;
    int any = 0;
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &surf[i];
        if (!s->used || !overlaps(addr, size, s->addr, s->size)) continue;
        if (s->dirty) { queue_download(s); any = 1; }
        if (will_write) s->checked = 0;
    }
    if (any) { submit_wait(); nsyncs++; }
}
void vkr_sync_all(void) { if (vkr_on) vkr_sync_range(0, 0xffffffffu, 0); }

static Surf *get_surface(uint32_t addr, uint32_t w, uint32_t h, uint32_t fmt, int depth) {
    uint32_t size = w * h * (depth ? dbpp[fmt & 3] : cbpp[fmt & 7]);
    Surf *free_slot = 0;
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &surf[i];
        if (s->used && s->addr == addr && s->w == w && s->h == h && s->fmt == fmt && s->depth == depth) { s->last = gen; return s; }
    }
    vkr_sync_range(addr, size, 0);                                 /* write back what the new surface replaces */
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &surf[i];
        if (s->used && overlaps(addr, size, s->addr, s->size)) drop_surface(s);
        if (!s->used && !free_slot) free_slot = s;
    }
    if (!free_slot) {                                              /* evict the least recently used */
        Surf *lru = &surf[0];
        for (int i = 1; i < NSURF; i++) if (surf[i].last < lru->last) lru = &surf[i];
        if (lru->dirty) { queue_download(lru); submit_wait(); }
        drop_surface(lru); free_slot = lru;
    }
    Surf *s = free_slot;
    memset(s, 0, sizeof *s);
    s->used = 1; s->depth = depth; s->addr = addr; s->size = size; s->w = w; s->h = h; s->fmt = fmt; s->last = gen;
    if (depth) {
        VkFormat f = fmt == 0 ? VK_FORMAT_D16_UNORM : d24fmt;
        VkImageAspectFlags a = fmt == 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        mkimg(&s->im, w, h, f, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, a);
    } else mkimg(&s->im, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    s->hash = ~0ull;                                               /* forces the first upload */
    LOG(2, "vulkan: new %s surface %08x %ux%u format %u", depth ? "depth" : "colour", addr, w, h, fmt);
    return s;
}
static void validate(Surf *s) {
    if (!s || s->dirty || s->checked == gen) return;
    uint64_t h = hash_mem((const uint8_t *)(uintptr_t)s->addr, s->size);
    if (h != s->hash) upload_surface(s);
    s->hash = h; s->checked = gen;
}

int vkr_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t ctl) {
    if (!vkr_on) return 0;
    uint32_t fw = (ctl & 0x100) ? 3 : (ctl & 0x200) ? 4 : 2;
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &surf[i];
        if (!s->used || s->addr != start || s->addr + s->size != end) continue;
        uint32_t bpp = s->depth ? dbpp[s->fmt] : cbpp[s->fmt];
        uint8_t pat[24];
        for (int k = 0; k < 24; k++) pat[k] = (uint8_t)(value >> (8 * (k % fw)));
        for (uint32_t k = bpp; k + bpp <= 24; k += bpp) if (memcmp(pat, pat + k, bpp)) return 0;   /* not one colour per pixel */
        begin_cmd(); end_render();
        VkImageSubresourceRange r = { s->im.aspect, 0, 1, 0, 1 };
        if (!s->depth) {
            uint8_t c[4]; color_read(pat, s->fmt, c);
            VkClearColorValue cv = { .float32 = { c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f } };
            vkCmdClearColorImage(cmd, s->im.img, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
        } else {
            uint32_t d = s->fmt == 0 ? (uint32_t)(pat[0] | pat[1] << 8) : (uint32_t)(pat[0] | pat[1] << 8 | pat[2] << 16);
            VkClearDepthStencilValue dv = { (float)d / (s->fmt == 0 ? 65535.0f : 16777215.0f), s->fmt == 3 ? pat[3] : 0 };
            vkCmdClearDepthStencilImage(cmd, s->im.img, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &r);
        }
        barrier();
        s->dirty = 1;
        return 1;
    }
    return 0;
}

/* ---- textures ---- */
typedef struct { int used; uint32_t addr, size, w, h, fmt; Img im; uint64_t hash, checked, last; } Tex;
#define NTEX 768
static Tex texs[NTEX];
static Img dummy;
static Tex *get_texture(const VkrTex *t, int unit, void (*decode)(int, uint8_t *)) {
    vkr_sync_range(t->addr, t->size, 0);                          /* render-to-texture: the bytes must be current */
    Tex *e = 0, *lru = &texs[0];
    for (int i = 0; i < NTEX; i++) {
        Tex *x = &texs[i];
        if (x->used && x->addr == t->addr && x->w == t->w && x->h == t->h && x->fmt == t->fmt) { e = x; break; }
        if (!x->used) { if (lru->used) lru = x; }
        else if (lru->used && x->last < lru->last) lru = x;
    }
    if (e && e->checked == gen) { e->last = gen; return e; }
    uint64_t h = hash_mem((const uint8_t *)(uintptr_t)t->addr, t->size);
    if (e && e->hash == h) { e->checked = gen; e->last = gen; return e; }
    if (!e) {
        e = lru;
        if (e->used) bury(&e->im);
        memset(e, 0, sizeof *e);
        e->used = 1; e->addr = t->addr; e->w = t->w; e->h = t->h; e->fmt = t->fmt; e->size = t->size;
        mkimg(&e->im, t->w, t->h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    VkDeviceSize n = (VkDeviceSize)t->w * t->h * 4;
    begin_cmd(); end_render();
    VkDeviceSize off = ring_alloc(&ring, n);
    decode(unit, ring.map + off);
    VkBufferImageCopy c = { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { t->w, t->h, 1 } };
    vkCmdCopyBufferToImage(cmd, ring.buf, e->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    barrier();
    e->hash = h; e->checked = gen; e->last = gen;
    ntexuploads++;
    return e;
}

/* ---- pipelines ---- */
typedef struct { uint8_t blend, logic, sc, dc, ec, sa, da, ea, lop, wmask, dkind, color; } PKey;
typedef struct { PKey k; VkPipeline p; } PEntry;
static PEntry pipes[512]; static int npipes;
static VkPipeline get_pipeline(const PKey *k) {
    for (int i = 0; i < npipes; i++) if (!memcmp(&pipes[i].k, k, sizeof *k)) return pipes[i].p;
    if (npipes == 512) fatal("vulkan: too many pipeline variants");
    static const VkBlendFactor bf[16] = { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ONE };
    static const VkBlendOp bo[8] = { VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
        VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD };
    static const VkLogicOp lo[16] = { VK_LOGIC_OP_CLEAR, VK_LOGIC_OP_AND, VK_LOGIC_OP_AND_REVERSE, VK_LOGIC_OP_COPY, VK_LOGIC_OP_SET,
        VK_LOGIC_OP_COPY_INVERTED, VK_LOGIC_OP_NO_OP, VK_LOGIC_OP_INVERT, VK_LOGIC_OP_NAND, VK_LOGIC_OP_OR, VK_LOGIC_OP_NOR, VK_LOGIC_OP_XOR,
        VK_LOGIC_OP_EQUIVALENT, VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_OR_REVERSE, VK_LOGIC_OP_OR_INVERTED };
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertmod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragmod, .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, 112, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[9];
    for (int i = 0; i < 6; i++) va[i] = (VkVertexInputAttributeDescription){ (uint32_t)i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)(16 * i) };
    va[6] = (VkVertexInputAttributeDescription){ 6, 0, VK_FORMAT_R32_SFLOAT, 96 };
    va[7] = (VkVertexInputAttributeDescription){ 7, 0, VK_FORMAT_R32G32_SINT, 100 };
    va[8] = (VkVertexInputAttributeDescription){ 8, 0, VK_FORMAT_R32_SFLOAT, 108 };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb, .vertexAttributeDescriptionCount = 9, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba = { .blendEnable = k->blend && !k->logic, .srcColorBlendFactor = bf[k->sc], .dstColorBlendFactor = bf[k->dc],
        .colorBlendOp = bo[k->ec], .srcAlphaBlendFactor = bf[k->sa], .dstAlphaBlendFactor = bf[k->da], .alphaBlendOp = bo[k->ea], .colorWriteMask = k->wmask };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = k->logic, .logicOp = lo[k->lop], .attachmentCount = k->color ? 1 : 0, .pAttachments = &ba };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn };
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM, df = k->dkind == 1 ? VK_FORMAT_D16_UNORM : k->dkind == 2 ? d24fmt : VK_FORMAT_UNDEFINED;
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->color ? 1 : 0,
        .pColorAttachmentFormats = &cf, .depthAttachmentFormat = df, .stencilAttachmentFormat = k->dkind == 2 ? d24fmt : VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = playout };
    VkPipeline p;
    CHK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, 0, &p));
    pipes[npipes].k = *k; pipes[npipes].p = p; npipes++;
    return p;
}

/* ---- drawing ---- */
static Surf *rt_c, *rt_d;
static void ensure_render(Surf *c, Surf *d, uint32_t w, uint32_t h) {
    if (in_render && rt_c == c && rt_d == d) return;
    end_render();
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = c ? c->im.view : 0,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingAttachmentInfo da = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = d ? d->im.view : 0,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { w, h } }, .layerCount = 1,
        .colorAttachmentCount = c ? 1 : 0, .pColorAttachments = &ca, .pDepthAttachment = d ? &da : 0,
        .pStencilAttachment = (d && d->fmt != 0) ? &da : 0 };
    vkCmdBeginRendering(cmd, &ri);
    in_render = 1; rt_c = c; rt_d = d;
}

typedef struct {
    uint32_t R[0x300]; float L[48][4]; float gamb[4], misc[4]; uint32_t flags[4]; uint32_t texinfo[3][4];
} Ubo;
static VkDeviceSize lut_off; static uint64_t lut_ver = ~0ull, lut_ring_gen = ~0ull;

void vkr_draw(const VkrDraw *d, const void *tris, int ntris, void (*decode)(int, uint8_t *)) {
    if (!ntris) return;
    begin_cmd();
    const uint32_t *R = d->regs;
    Tex *tx[3] = { 0, 0, 0 };
    { uint64_t t0 = vns(); for (int u = 0; u < 3; u++) if (d->tex[u].on) tx[u] = get_texture(&d->tex[u], u, decode); t_tex += vns() - t0; }
    Surf *sc = d->caddr ? get_surface(d->caddr, d->width, d->height, d->cfmt, 0) : 0;
    Surf *sd = d->daddr ? get_surface(d->daddr, d->width, d->height, d->dfmt, 1) : 0;
    if (sc && sd && overlaps(sc->addr, sc->size, sd->addr, sd->size)) sd = 0;
    validate(sc); validate(sd);
    if (sc) sc->last = gen;
    if (sd) sd->last = gen;

    /* staging space for this draw: vertices, uniforms and (if changed) the lighting LUT, all in one go */
    VkDeviceSize vbytes = (VkDeviceSize)ntris * 3 * 112, need = vbytes + sizeof(Ubo) + 24 * 256 * 4 + 1024;
    if (((ring.used + 255) & ~(VkDeviceSize)255) + need > ring.size) { submit_wait(); begin_cmd(); }
    VkDeviceSize voff = ring_alloc(&ring, vbytes), uoff = ring_alloc(&ring, sizeof(Ubo));
    memcpy(ring.map + voff, tris, vbytes);
    if (d->lut_version != lut_ver || lut_ring_gen != ring_gen) {
        lut_off = ring_alloc(&ring, 24 * 256 * 4); memcpy(ring.map + lut_off, d->lut, 24 * 256 * 4);
        lut_ver = d->lut_version; lut_ring_gen = ring_gen;
    }
    Ubo *u = (Ubo *)(ring.map + uoff);
    memcpy(u->R, R, sizeof u->R); memcpy(u->L, d->lights, sizeof u->L); memcpy(u->gamb, d->gamb, sizeof u->gamb);
    u->misc[0] = d->zscale; u->misc[1] = d->zoff; u->misc[2] = u->misc[3] = 0;
    u->flags[0] = d->flat; u->flags[1] = d->flat_color; u->flags[2] = d->nlights;
    u->flags[3] = (tx[0] ? 1 : 0) | (tx[1] ? 2 : 0) | (tx[2] ? 4 : 0);
    for (int k = 0; k < 3; k++) { u->texinfo[k][0] = d->tex[k].w; u->texinfo[k][1] = d->tex[k].h; u->texinfo[k][2] = d->tex[k].wrap; u->texinfo[k][3] = d->tex[k].border; }

    ensure_render(sc, sd, d->width, d->height);

    uint32_t om = R[0x107], allow_c = R[0x113] & 15, allow_d = R[0x115] & 3, bl = R[0x101];
    PKey k = { 0 };
    k.color = sc != 0;
    k.dkind = sd ? (sd->fmt == 0 ? 1 : 2) : 0;
    k.logic = !(R[0x100] & 0x100) && !d->flat;
    k.blend = (R[0x100] & 0x100) && !d->flat;
    if (k.blend) { k.sc = (bl >> 16) & 15; k.dc = (bl >> 20) & 15; k.ec = bl & 7; k.sa = (bl >> 24) & 15; k.da = (bl >> 28) & 15; k.ea = (bl >> 8) & 7; }
    k.lop = R[0x102] & 15;
    k.wmask = allow_c ? (om >> 8) & 15 : 0;
    if (sc && (sc->fmt == 1 || sc->fmt == 3)) k.wmask &= 7;      /* no alpha in memory: keep the image's alpha at 1 */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, get_pipeline(&k));

    VkDescriptorBufferInfo ub = { ring.buf, uoff, sizeof(Ubo) }, lb = { ring.buf, lut_off, 24 * 256 * 4 };
    VkDescriptorImageInfo ii[3];
    for (int t = 0; t < 3; t++) ii[t] = (VkDescriptorImageInfo){ sampler, tx[t] ? tx[t]->im.view : dummy.view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[5] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &ub },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &lb } };
    for (int t = 0; t < 3; t++)
        w[2 + t] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)(2 + t), .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[t] };
    push_desc(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 5, w);
    float size[2] = { (float)d->width, (float)d->height };
    vkCmdPushConstants(cmd, playout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof size, size);

    VkViewport vp = { 0, 0, (float)d->width, (float)d->height, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sr = { { 0, 0 }, { d->width, d->height } };
    if ((R[0x065] & 3) == 3) {
        int32_t x1 = (int32_t)(R[0x066] & 0x3ff), y1 = (int32_t)((R[0x066] >> 16) & 0x3ff), x2 = (int32_t)(R[0x067] & 0x3ff) + 1, y2 = (int32_t)((R[0x067] >> 16) & 0x3ff) + 1;
        if (x2 > (int32_t)d->width) x2 = (int32_t)d->width;
        if (y2 > (int32_t)d->height) y2 = (int32_t)d->height;
        sr.offset.x = x1; sr.offset.y = y1;
        sr.extent.width = x2 > x1 ? (uint32_t)(x2 - x1) : 0; sr.extent.height = y2 > y1 ? (uint32_t)(y2 - y1) : 0;
    }
    vkCmdSetScissor(cmd, 0, 1, &sr);
    uint32_t kk = R[0x103];
    float bc[4] = { (kk & 255) / 255.0f, ((kk >> 8) & 255) / 255.0f, ((kk >> 16) & 255) / 255.0f, (kk >> 24) / 255.0f };
    vkCmdSetBlendConstants(cmd, bc);
    static const VkCompareOp cop[8] = { VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
        VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL };
    uint32_t dtest = om & 1, dfunc = (om >> 4) & 7, dwrite = (om >> 12) & 1;
    vkCmdSetDepthTestEnable(cmd, sd != 0);
    vkCmdSetDepthWriteEnable(cmd, sd && allow_d && dwrite);
    vkCmdSetDepthCompareOp(cmd, dtest ? cop[dfunc] : VK_COMPARE_OP_ALWAYS);
    uint32_t st = R[0x105], sa = R[0x106];
    int stencil = sd && sd->fmt == 3 && (st & 1);
    vkCmdSetStencilTestEnable(cmd, stencil);
    vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, (VkStencilOp)(sa & 7), (VkStencilOp)((sa >> 8) & 7), (VkStencilOp)((sa >> 4) & 7), cop[(st >> 4) & 7]);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, (st >> 24) & 0xff);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, allow_d ? (st >> 8) & 0xff : 0);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, (st >> 16) & 0xff);

    vkCmdBindVertexBuffers(cmd, 0, 1, &ring.buf, &voff);
    vkCmdDraw(cmd, (uint32_t)ntris * 3, 1, 0, 0);
    if (sc && k.wmask) sc->dirty = 1;
    if (sd && allow_d) sd->dirty = 1;
}

void vkr_new_gen(void) { gen++; }
void vkr_frame(void) {
    if (!vkr_on) return;
    submit_wait();
    static uint64_t frames;
    if (++frames % 300 == 0)
        LOG(1, "vulkan: GPU wait %.2f s, write-back %.2f s, textures %.2f s", t_wait / 1e9, t_fin / 1e9, t_tex / 1e9);
        LOG(1, "vulkan: %llu submissions, %llu sync points, %llu surface downloads, %llu surface uploads, %llu texture uploads, %d pipelines",
            (unsigned long long)nsubmits, (unsigned long long)nsyncs, (unsigned long long)ndownloads, (unsigned long long)nuploads,
            (unsigned long long)ntexuploads, npipes);
}

/* ---- setup ---- */
int vkr_init(void) {
    const char *sel = getenv("RB_RENDERER");
    if (sel && strcmp(sel, "vulkan")) return 0;                     /* default: Vulkan when available */
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "3dsrecomp", .apiVersion = VK_API_VERSION_1_3 };
    const char *layer = "VK_LAYER_KHRONOS_validation";
    int validate_on = getenv("RB_VK_VALIDATE") != 0;
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .enabledLayerCount = validate_on ? 1 : 0, .ppEnabledLayerNames = &layer };
    if (vkCreateInstance(&ici, 0, &inst) != VK_SUCCESS) { LOG(1, "vulkan: no instance, using the software renderer"); return 0; }
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, 0);
    VkPhysicalDevice devs[16]; if (n > 16) n = 16; vkEnumeratePhysicalDevices(inst, &n, devs);
    int pick = -1;
    const char *want = getenv("RB_VK_DEVICE");
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        if (want) { if ((int)i == atoi(want)) pick = (int)i; continue; }
        if (pick < 0 || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pick = (int)i;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
    }
    if (pick < 0) { LOG(1, "vulkan: no Vulkan 1.3 device, using the software renderer"); return 0; }
    pdev = devs[pick];
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pdev, &props);
    vkGetPhysicalDeviceMemoryProperties(pdev, &memprops);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, 0);
    VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qp);
    qfam = ~0u;
    for (uint32_t i = 0; i < nq; i++) if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    if (qfam == ~0u) { LOG(1, "vulkan: no graphics queue"); return 0; }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pdev, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    d24fmt = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE, .shaderDemoteToHelperInvocation = VK_TRUE };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13, .features = { .logicOp = VK_TRUE } };
    const char *ext = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &ext };
    if (vkCreateDevice(pdev, &dci, 0, &dev) != VK_SUCCESS) { LOG(1, "vulkan: device creation failed, using the software renderer"); return 0; }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    push_desc = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
    if (!push_desc) fatal("vulkan: vkCmdPushDescriptorSetKHR missing");

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam };
    CHK(vkCreateCommandPool(dev, &cpi, 0, &cpool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    CHK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHK(vkCreateFence(dev, &fci, 0, &fence));
    mkbuf(&ring, 128u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0);
    mkbuf(&rbk, 64u << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    VkDescriptorSetLayoutBinding b[5] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0 },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0 },
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0 },
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0 },
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0 } };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 5, .pBindings = b };
    CHK(vkCreateDescriptorSetLayout(dev, &dli, 0, &dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    CHK(vkCreatePipelineLayout(dev, &pli, 0, &playout));
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vert_spv, .pCode = vert_spv };
    CHK(vkCreateShaderModule(dev, &smi, 0, &vertmod));
    smi.codeSize = sizeof frag_spv; smi.pCode = frag_spv;
    CHK(vkCreateShaderModule(dev, &smi, 0, &fragmod));
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    CHK(vkCreateSampler(dev, &sci, 0, &sampler));
    mkimg(&dummy, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    submit_wait();
    vkr_on = 1;
    LOG(1, "vulkan: rendering on %s (depth-stencil format %s)", props.deviceName, d24fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32FS8");
    return 1;
}
