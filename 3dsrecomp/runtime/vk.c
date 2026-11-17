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
static const uint32_t pvert_spv[] =
#include "present_vert.inc"
;
static const uint32_t pfrag_spv[] =
#include "present_frag.inc"
;

#define CHK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) fatal("vulkan: %s failed (%d)", #x, r_); } while (0)

static VkInstance inst;
static VkPhysicalDevice pdev;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
/* Two frame contexts: with Vulkan presentation, a frame's work is submitted without waiting and the CPU records the
 * next frame into the other context (its own command buffer, fence, half of the staging ring and destroy list).
 * Every synchronous wait (submit_wait) still waits for all earlier work, so correctness does not depend on this. */
typedef struct Img Img;
typedef struct { VkCommandPool pool; VkCommandBuffer cb, cb2; VkFence fen; VkSemaphore acq; int pending; struct ImgList *grave; } Ctx;   /* cb2: present pass */
static Ctx ctxs[2]; static int cur_ctx;
#define cpool (ctxs[cur_ctx].pool)
#define cmd (ctxs[cur_ctx].cb)
#define fence (ctxs[cur_ctx].fen)
static VkPhysicalDeviceMemoryProperties memprops;
static VkDescriptorSetLayout dsl;
static VkPipelineLayout playout;
static VkShaderModule vertmod, fragmod;
static VkSampler sampler;
static PFN_vkCmdPushDescriptorSetKHR push_desc;
static VkFormat d24fmt;
static int recording, in_render;
static const char *const *inst_exts; static uint32_t ninst_exts;
static int (*mk_surface)(void *instance, uint64_t *surface);
static VkSurfaceKHR vsurf;
static int presenting, init_done;

static uint64_t t_wait, t_fin, t_tex, t_pipe, nskipped;   /* t_pipe: time building pipeline variants */
static VkPipelineCache pcache;
uint64_t gpu_frame(void);
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

struct Img { VkImage img; VkDeviceMemory mem; VkImageView view; VkImageAspectFlags aspect; };
struct ImgList { Img img[1024]; int n; };
static struct ImgList graves[2];                   /* per context: destroyed once that context's GPU work is done */
static void destroy_list(struct ImgList *g) {
    for (int i = 0; i < g->n; i++) { vkDestroyImageView(dev, g->img[i].view, 0); vkDestroyImage(dev, g->img[i].img, 0); vkFreeMemory(dev, g->img[i].mem, 0); }
    g->n = 0;
}

/* ---- command recording ---- */
static void barrier(void);
static void begin_cmd(void) {
    if (recording) return;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHK(vkBeginCommandBuffer(cmd, &bi));
    recording = 1;
    barrier();                         /* order after the other frame context's work, which may still be running */
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
typedef struct { Surf *s; VkDeviceSize off, soff, hoff; int native, hi; } Pending;
static Pending pend[64]; static int npend;
static void finish_downloads(void);
static uint64_t nstale;
static void invalidate_textures(uint32_t addr, uint32_t size);

/* Submit everything recorded and wait for the GPU. */
static void submit_wait(void) {
    if (!recording) return;
    end_render();
    CHK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    CHK(vkQueueSubmit(queue, 1, &si, fence));
    uint64_t t0 = vns();
    CHK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));      /* queue order: the other context is done too */
    t_wait += vns() - t0;
    CHK(vkResetFences(dev, 1, &fence));
    CHK(vkResetCommandPool(dev, cpool, 0));
    Ctx *o = &ctxs[cur_ctx ^ 1];
    if (o->pending) { CHK(vkResetFences(dev, 1, &o->fen)); o->pending = 0; }
    recording = 0; nsubmits++;
    ring.used = 0; rbk.used = 0; ring_gen++;
    t0 = vns(); finish_downloads(); t_fin += vns() - t0;
    destroy_list(&graves[0]); destroy_list(&graves[1]);
}
/* Submit the current context without waiting (signal/wait semaphores for presentation) and switch to the other one,
 * waiting only for the work submitted two frames ago. */
static void submit_async(VkSemaphore wait_sem, VkSemaphore signal_sem) {
    end_render();
    CHK(vkEndCommandBuffer(cmd));
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = wait_sem ? 1 : 0, .pWaitSemaphores = &wait_sem,
        .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &cmd, .signalSemaphoreCount = signal_sem ? 1 : 0, .pSignalSemaphores = &signal_sem };
    CHK(vkQueueSubmit(queue, 1, &si, fence));
    ctxs[cur_ctx].pending = 1; nsubmits++;
    cur_ctx ^= 1;
    if (ctxs[cur_ctx].pending) {
        uint64_t t0 = vns();
        CHK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        t_wait += vns() - t0;
        CHK(vkResetFences(dev, 1, &fence));
        ctxs[cur_ctx].pending = 0;
    }
    CHK(vkResetCommandPool(dev, cpool, 0));
    destroy_list(&graves[cur_ctx]);
    recording = 0; ring.used = 0; ring_gen++;
}
static VkDeviceSize ring_alloc(Ring *r, VkDeviceSize n) {
    VkDeviceSize half = r == &ring ? r->size / 2 : r->size, base = r == &ring ? half * (VkDeviceSize)cur_ctx : 0;
    VkDeviceSize off = (r->used + 255) & ~(VkDeviceSize)255;      /* the staging ring is split between the two contexts */
    if (off + n > half) {
        if (n > half) fatal("vulkan: %llu bytes do not fit the staging ring", (unsigned long long)n);
        submit_wait(); begin_cmd(); off = 0;
        base = r == &ring ? half * (VkDeviceSize)cur_ctx : 0;
    }
    r->used = off + n;
    return base + off;
}

/* layers > 0: a 2D-array image and view with that many layers (textures); 0: a plain 2D image (surfaces). */
static void mkimg_full(Img *im, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, VkFormat f, VkImageUsageFlags use, VkImageAspectFlags aspect) {
    uint32_t nl = layers ? layers : 1;
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
        .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = nl, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    CHK(vkCreateImage(dev, &ci, 0, &im->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im->img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CHK(vkAllocateMemory(dev, &ai, 0, &im->mem));
    CHK(vkBindImageMemory(dev, im->img, im->mem, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = layers ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        .format = f, .subresourceRange = { aspect, 0, levels, 0, nl } };
    CHK(vkCreateImageView(dev, &vi, 0, &im->view));
    im->aspect = aspect;
    begin_cmd(); end_render();
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im->img, .subresourceRange = { aspect, 0, levels, 0, nl } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}
static void mkimg(Img *im, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags use, VkImageAspectFlags aspect) {
    mkimg_full(im, w, h, 1, 0, f, use, aspect);
}
static void bury(Img *im) {
    struct ImgList *g = &graves[cur_ctx];
    if (g->n == 1024) submit_wait();
    g->img[g->n++] = *im; memset(im, 0, sizeof *im);
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
/* Surfaces are rendered at `scale` times the native size (video.scale); guest memory always holds the native
 * image: write-backs average each scale x scale block (colour) or take its centre sample (depth, stencil). */
static uint32_t scale = 1;
struct Surf { int used, depth; uint32_t addr, size, w, h, fmt; Img im; int dirty; uint64_t hash, checked, last;
              int display; uint8_t *hires; uint64_t hires_gen; Img small; };   /* small: native-size copy target (scale > 1) */   /* display: last download kept at full resolution */
#define NSURF 64
static Surf surf[NSURF];
static int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return a < b + bn && b < a + an; }

static void queue_download(Surf *s) {
    begin_cmd(); end_render();
    uint32_t W = s->w * scale, H = s->h * scale;
    VkDeviceSize n = (VkDeviceSize)W * H;
    Pending *p = &pend[npend];
    memset(p, 0, sizeof *p);
    if (s->depth && s->fmt != 0) {
        p->off = ring_alloc(&rbk, n * 4); p->soff = ring_alloc(&rbk, n);
        VkBufferImageCopy c[2] = { { .bufferOffset = p->off, .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } },
                                   { .bufferOffset = p->soff, .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } } };
        vkCmdCopyImageToBuffer(cmd, s->im.img, VK_IMAGE_LAYOUT_GENERAL, rbk.buf, 2, c);
    } else if (!s->depth && scale > 1) {                          /* colour: average on the GPU, read back native size */
        if (!s->small.img) mkimg(&s->small, s->w, s->h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit b = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { (int32_t)W, (int32_t)H, 1 } },
                          .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffsets = { { 0, 0, 0 }, { (int32_t)s->w, (int32_t)s->h, 1 } } };
        vkCmdBlitImage(cmd, s->im.img, VK_IMAGE_LAYOUT_GENERAL, s->small.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
        barrier();
        p->off = ring_alloc(&rbk, (VkDeviceSize)s->w * s->h * 4); p->native = 1;
        VkBufferImageCopy c = { .bufferOffset = p->off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { s->w, s->h, 1 } };
        vkCmdCopyImageToBuffer(cmd, s->small.img, VK_IMAGE_LAYOUT_GENERAL, rbk.buf, 1, &c);
        if (s->display) {                                          /* the window shows this one at full resolution */
            p->hoff = ring_alloc(&rbk, n * 4); p->hi = 1;
            VkBufferImageCopy h = { .bufferOffset = p->hoff, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
            vkCmdCopyImageToBuffer(cmd, s->im.img, VK_IMAGE_LAYOUT_GENERAL, rbk.buf, 1, &h);
        }
    } else {
        p->off = ring_alloc(&rbk, n * 4);
        VkBufferImageCopy c = { .bufferOffset = p->off, .imageSubresource = { s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
        vkCmdCopyImageToBuffer(cmd, s->im.img, VK_IMAGE_LAYOUT_GENERAL, rbk.buf, 1, &c);
        p->hoff = p->off; p->hi = 1;
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
        uint32_t S = pend[i].native ? 1 : scale, W = s->w * S, c = S / 2;
        for (uint32_t y = 0; y < s->h; y++)
            for (uint32_t x = 0; x < s->w; x++) {
                if (!s->depth) {
                    uint8_t px[4];
                    if (S == 1) memcpy(px, src + 4 * (y * W + x), 4);
                    else {
                        uint32_t acc[4] = { 0, 0, 0, 0 };
                        for (uint32_t j = 0; j < S; j++)
                            for (uint32_t i2 = 0; i2 < S; i2++) { const uint8_t *q = src + 4 * ((y * S + j) * W + x * S + i2); for (int k = 0; k < 4; k++) acc[k] += q[k]; }
                        for (int k = 0; k < 4; k++) px[k] = (uint8_t)((acc[k] + S * S / 2) / (S * S));
                    }
                    color_write(mem + tile_off(s->w, s->h, x, y, cbpp[s->fmt]), s->fmt, px);
                    continue;
                }
                uint32_t k = (y * S + c) * W + x * S + c;
                uint8_t *p = mem + tile_off(s->w, s->h, x, y, dbpp[s->fmt]);
                if (s->fmt == 0) { memcpy(p, src + 2 * k, 2); continue; }
                uint32_t d;
                if (d24fmt == VK_FORMAT_D24_UNORM_S8_UINT) { memcpy(&d, src + 4 * k, 4); d &= 0xffffff; }
                else { float f; memcpy(&f, src + 4 * k, 4); d = (uint32_t)(f * 16777215.0f + 0.5f); }
                p[0] = (uint8_t)d; p[1] = (uint8_t)(d >> 8); p[2] = (uint8_t)(d >> 16);
                if (s->fmt == 3) p[3] = st[k];
            }
        if (s->display && !s->depth && pend[i].hi) {               /* keep the full-resolution image for the window */
            size_t n = (size_t)s->w * scale * s->h * scale * 4;
            if (!s->hires) s->hires = malloc(n);
            if (s->hires) { memcpy(s->hires, rbk.map + pend[i].hoff, n); s->hires_gen++; }
        }
        s->dirty = 0; s->hash = hash_mem(mem, s->size); s->checked = gen;
        invalidate_textures(s->addr, s->size);
    }
    npend = 0;
}
static void upload_surface(Surf *s) {
    begin_cmd(); end_render();
    const uint8_t *mem = (const uint8_t *)(uintptr_t)s->addr;
    uint32_t S = scale, W = s->w * S, H = s->h * S;
    VkDeviceSize n = (VkDeviceSize)W * H;
    VkDeviceSize off = ring_alloc(&ring, n * 4), soff = 0;
    uint8_t *dst = ring.map + off, *sdst = 0;
    if (s->depth && s->fmt != 0) { soff = ring_alloc(&ring, n); sdst = ring.map + soff; }
    for (uint32_t y = 0; y < s->h; y++)
        for (uint32_t x = 0; x < s->w; x++) {
            uint8_t v[4] = { 0, 0, 0, 0 }, sv = 0; int vb = 4;
            if (!s->depth) color_read(mem + tile_off(s->w, s->h, x, y, cbpp[s->fmt]), s->fmt, v);
            else {
                const uint8_t *p = mem + tile_off(s->w, s->h, x, y, dbpp[s->fmt]);
                if (s->fmt == 0) { memcpy(v, p, 2); vb = 2; }
                else {
                    uint32_t d = (uint32_t)(p[0] | p[1] << 8 | p[2] << 16);
                    if (d24fmt == VK_FORMAT_D24_UNORM_S8_UINT) memcpy(v, &d, 4);
                    else { float f = (float)d / 16777215.0f; memcpy(v, &f, 4); }
                    sv = s->fmt == 3 ? p[3] : 0;
                }
            }
            for (uint32_t j = 0; j < S; j++)
                for (uint32_t i2 = 0; i2 < S; i2++) {
                    uint32_t k = (y * S + j) * W + x * S + i2;
                    memcpy(dst + (size_t)vb * k, v, (size_t)vb);
                    if (sdst) sdst[k] = sv;
                }
        }
    if (s->depth && s->fmt != 0) {
        VkBufferImageCopy c[2] = { { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } },
                                   { .bufferOffset = soff, .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } } };
        vkCmdCopyBufferToImage(cmd, ring.buf, s->im.img, VK_IMAGE_LAYOUT_GENERAL, 2, c);
    } else {
        VkBufferImageCopy c = { .bufferOffset = off, .imageSubresource = { s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
        vkCmdCopyBufferToImage(cmd, ring.buf, s->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    barrier();
    nuploads++;
}
static void drop_surface(Surf *s) { bury(&s->im); if (s->small.img) bury(&s->small); free(s->hires); s->hires = 0; s->used = 0; }

/* Guest bytes in [addr, addr+size) changed behind the texture cache: re-check overlapping textures on next use. */

/* Write back every dirty surface overlapping [addr, addr+size). will_write: the caller is about to change
 * those bytes, so clean surfaces there must be re-checked before their next use. */
int vkr_why;                                              /* caller's reason for the next sync (profiler) */
static uint64_t sync_by[8], dl_by[8];
void vkr_sync_counts(uint64_t waits[8], uint64_t downloads[8]) { memcpy(waits, sync_by, sizeof sync_by); memcpy(downloads, dl_by, sizeof dl_by); }
void vkr_sync_range(uint32_t addr, uint32_t size, int will_write) {
    int why = vkr_why & 7; vkr_why = 0;
    if (!vkr_on) return;
    int any = 0;
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &surf[i];
        if (!s->used || !overlaps(addr, size, s->addr, s->size)) continue;
        if (s->dirty) { queue_download(s); any = 1; dl_by[why]++;
            static int ls = -1; if (ls < 0) ls = getenv("RB_LOG_SYNC") != 0;
            if (ls) fprintf(stderr, "sync: frame %llu cause %d range %08x+%x surface %08x %ux%u fmt %u%s\n", (unsigned long long)gpu_frame(), why,
                            addr, size, s->addr, s->w, s->h, s->fmt, s->depth ? " depth" : ""); }
        if (will_write) s->checked = 0;
    }
    if (any) { submit_wait(); nsyncs++; sync_by[why]++; }
    if (will_write) invalidate_textures(addr, size);
}
void vkr_sync_all(void) { if (vkr_on) vkr_sync_range(0, 0xffffffffu, 0); }

static Surf *get_surface(uint32_t addr, uint32_t w, uint32_t h, uint32_t fmt, int depth) {
    uint32_t size = w * h * (depth ? dbpp[fmt & 3] : cbpp[fmt & 7]);
    Surf *free_slot = 0;
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &surf[i];
        if (s->used && s->addr == addr && s->w == w && s->h == h && s->fmt == fmt && s->depth == depth) { s->last = gen; return s; }
    }
    vkr_why = 6; vkr_sync_range(addr, size, 0);                    /* write back what the new surface replaces */
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
        mkimg(&s->im, w * scale, h * scale, f, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, a);
    } else mkimg(&s->im, w * scale, h * scale, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
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
typedef struct { int used; uint32_t addr, size, w, h, fmt, levels, faces, face_addr[6]; Img im; uint64_t hash, checked, last; } Tex;
#define NTEX 768
static Tex texs[NTEX];
static Img dummy;
static void invalidate_textures(uint32_t addr, uint32_t size) {
    for (int i = 0; i < NTEX; i++)
        if (texs[i].used && overlaps(addr, size, texs[i].addr, texs[i].size)) { if (texs[i].checked == gen) nstale++; texs[i].checked = 0; }
}
static Tex *get_texture(const VkrTex *t, int unit, void (*decode)(int, int, int, uint8_t *)) {
    for (uint32_t f = 0; f < t->faces; f++) { vkr_why = 7; vkr_sync_range(t->face_addr[f], t->size, 0); }   /* render-to-texture: the bytes must be current */
    Tex *e = 0, *lru = &texs[0];
    for (int i = 0; i < NTEX; i++) {
        Tex *x = &texs[i];
        if (x->used && x->addr == t->addr && x->w == t->w && x->h == t->h && x->fmt == t->fmt && x->levels == t->levels && x->faces == t->faces &&
            !memcmp(x->face_addr, t->face_addr, sizeof x->face_addr)) { e = x; break; }
        if (!x->used) { if (lru->used) lru = x; }
        else if (lru->used && x->last < lru->last) lru = x;
    }
    if (e && e->checked == gen) { e->last = gen; return e; }
    uint64_t h = 0;
    for (uint32_t f = 0; f < t->faces; f++) h = h * 31 + hash_mem((const uint8_t *)(uintptr_t)t->face_addr[f], t->size);
    if (e && e->hash == h) { e->checked = gen; e->last = gen; return e; }
    if (!e) {
        e = lru;
        if (e->used) bury(&e->im);
        memset(e, 0, sizeof *e);
        e->used = 1; e->addr = t->addr; e->w = t->w; e->h = t->h; e->fmt = t->fmt; e->size = t->size; e->levels = t->levels; e->faces = t->faces; memcpy(e->face_addr, t->face_addr, sizeof e->face_addr);
        mkimg_full(&e->im, t->w, t->h, t->levels, t->faces, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    begin_cmd(); end_render();
    for (uint32_t f = 0; f < t->faces; f++) for (uint32_t l = 0; l < t->levels; l++) {
        uint32_t w = t->w >> l, h = t->h >> l;
        VkDeviceSize off = ring_alloc(&ring, (VkDeviceSize)w * h * 4);
        decode(unit, (int)f, (int)l, ring.map + off);
        VkBufferImageCopy c = { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1 }, .imageExtent = { w, h, 1 } };
        vkCmdCopyBufferToImage(cmd, ring.buf, e->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    barrier();
    e->hash = h; e->checked = gen; e->last = gen;
    ntexuploads++;
    return e;
}

/* ---- samplers: the game's filters and mip range (texture_filter = "console"), or forced settings ---- */
static int filter_mode = -1;                       /* 0 nearest (reference), 1 console, 2 force linear */
static float max_aniso = 1;                        /* video.anisotropy (1 = off), clamped to the device limit */
typedef struct { uint8_t mag, min, mip, as, at, minl, maxl; int16_t bias; } SKey;
static struct { SKey k; VkSampler s; } samplers[256]; static int nsamplers;
static VkSampler get_sampler(const VkrTex *t, int *hw) {
    SKey k = { 0 };
    k.mag = (t->param >> 1) & 1; k.min = (t->param >> 2) & 1; k.mip = (t->param >> 24) & 1;
    if (filter_mode == 2) k.mag = k.min = k.mip = 1;
    k.minl = (uint8_t)((t->lod >> 24) & 15); k.maxl = (uint8_t)(t->levels - 1);
    if (k.minl > k.maxl) k.minl = k.maxl;
    k.bias = (int16_t)((int32_t)(t->lod << 19) >> 19);             /* signed 1.4.8 fixed point, bits 0-12 */
    *hw = filter_mode != 0 && (k.mag || k.min || t->levels > 1);
    static const VkSamplerAddressMode am[8] = { VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT };
    k.as = (uint8_t)(t->wrap & 7); k.at = (uint8_t)((t->wrap >> 8) & 7);   /* border modes are resolved in the shader */
    for (int i = 0; i < nsamplers; i++) if (!memcmp(&samplers[i].k, &k, sizeof k)) return samplers[i].s;
    if (nsamplers == 256) return sampler;
    VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = k.mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = k.min ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, .mipmapMode = k.mip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = am[k.as], .addressModeV = am[k.at], .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .minLod = (float)k.minl, .maxLod = (float)k.maxl, .mipLodBias = (float)k.bias / 256.0f,
        .anisotropyEnable = max_aniso > 1 && k.min, .maxAnisotropy = max_aniso };   /* Azahar leaves the bias at 0 */
    CHK(vkCreateSampler(dev, &ci, 0, &samplers[nsamplers].s));
    samplers[nsamplers].k = k;
    return samplers[nsamplers++].s;
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
    uint64_t tp = vns();
    CHK(vkCreateGraphicsPipelines(dev, pcache, 1, &pi, 0, &p));
    tp = vns() - tp; t_pipe += tp;
    LOG(2, "vulkan: new pipeline variant %d built in %.1f ms (frame %llu)", npipes + 1, tp / 1e6, (unsigned long long)gpu_frame());
    pipes[npipes].k = *k; pipes[npipes].p = p; npipes++;
    return p;
}

/* ---- drawing ---- */
static Surf *rt_c, *rt_d;
static VkImageView rt_cv, rt_dv;      /* a surface slot can be recreated with a new image: compare the images too */
static uint32_t rt_w, rt_h;
static void ensure_render(Surf *c, Surf *d, uint32_t w, uint32_t h) {
    if (in_render && rt_c == c && rt_d == d && rt_cv == (c ? c->im.view : 0) && rt_dv == (d ? d->im.view : 0) && rt_w == w && rt_h == h) return;
    end_render();
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = c ? c->im.view : 0,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingAttachmentInfo da = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = d ? d->im.view : 0,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { w * scale, h * scale } }, .layerCount = 1,
        .colorAttachmentCount = c ? 1 : 0, .pColorAttachments = &ca, .pDepthAttachment = d ? &da : 0,
        .pStencilAttachment = (d && d->fmt != 0) ? &da : 0 };
    vkCmdBeginRendering(cmd, &ri);
    in_render = 1; rt_c = c; rt_d = d; rt_cv = c ? c->im.view : 0; rt_dv = d ? d->im.view : 0; rt_w = w; rt_h = h;
}

#define LUT_BYTES ((24 * 256 + 128) * 4)   /* lighting LUTs, then the fog LUT */
typedef struct {
    uint32_t R[0x300]; float L[48][4]; float gamb[4], misc[4]; uint32_t flags[4]; uint32_t texinfo[3][4];
} Ubo;
static VkDeviceSize lut_off; static uint64_t lut_ver = ~0ull, lut_ring_gen = ~0ull;

void vkr_draw(const VkrDraw *d, const void *tris, int ntris, void (*decode)(int, int, int, uint8_t *)) {
    if (!ntris) return;
    begin_cmd();
    const uint32_t *R = d->regs;
    Tex *tx[3] = { 0, 0, 0 };
    { uint64_t t0 = vns(); for (int u = 0; u < 3; u++) if (d->tex[u].on) tx[u] = get_texture(&d->tex[u], u, decode); t_tex += vns() - t0; }
    uint32_t daddr = d->daddr;
    if (d->caddr && daddr && overlaps(d->caddr, d->width * d->height * cbpp[d->cfmt & 7], daddr, d->width * d->height * dbpp[d->dfmt & 3]))
        daddr = 0;                                                 /* depth aimed at the colour buffer: draw without depth */
    Surf *sc = d->caddr ? get_surface(d->caddr, d->width, d->height, d->cfmt, 0) : 0;
    Surf *sd = daddr ? get_surface(daddr, d->width, d->height, d->dfmt, 1) : 0;
    validate(sc); validate(sd);
    if (sc) sc->last = gen;
    if (sd) sd->last = gen;

    /* staging space for this draw: vertices, uniforms and (if changed) the lighting LUT, all in one go */
    VkDeviceSize vbytes = (VkDeviceSize)ntris * 3 * 112, need = vbytes + sizeof(Ubo) + LUT_BYTES + 1024;
    if (((ring.used + 255) & ~(VkDeviceSize)255) + need > ring.size / 2) { submit_wait(); begin_cmd(); }
    VkDeviceSize voff = ring_alloc(&ring, vbytes), uoff = ring_alloc(&ring, sizeof(Ubo));
    memcpy(ring.map + voff, tris, vbytes);
    if (d->lut_version != lut_ver || lut_ring_gen != ring_gen) {
        lut_off = ring_alloc(&ring, LUT_BYTES); memcpy(ring.map + lut_off, d->lut, 24 * 256 * 4); memcpy(ring.map + lut_off + 24 * 256 * 4, d->fog_lut, 128 * 4);
        lut_ver = d->lut_version; lut_ring_gen = ring_gen;
    }
    Ubo *u = (Ubo *)(ring.map + uoff);
    memcpy(u->R, R, sizeof u->R); memcpy(u->L, d->lights, sizeof u->L); memcpy(u->gamb, d->gamb, sizeof u->gamb);
    u->misc[0] = d->zscale; u->misc[1] = d->zoff; u->misc[2] = (float)scale; u->misc[3] = 0;
    u->flags[0] = d->flat; u->flags[1] = d->flat_color; u->flags[2] = d->nlights;
    u->flags[3] = (tx[0] ? 1 : 0) | (tx[1] ? 2 : 0) | (tx[2] ? 4 : 0);
    VkSampler smp[3] = { sampler, sampler, sampler };
    for (int k = 0; k < 3; k++) {
        int hw = 0;
        if (tx[k]) smp[k] = get_sampler(&d->tex[k], &hw);
        u->texinfo[k][0] = d->tex[k].w; u->texinfo[k][1] = d->tex[k].h; u->texinfo[k][2] = d->tex[k].wrap | (uint32_t)hw << 16 | (d->tex[k].faces == 6 ? 1u << 17 : 0); u->texinfo[k][3] = d->tex[k].border;
    }

    begin_cmd();                       /* validating a surface (e.g. one that overlaps a texture) may have submitted */
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

    VkDescriptorBufferInfo ub = { ring.buf, uoff, sizeof(Ubo) }, lb = { ring.buf, lut_off, LUT_BYTES };
    VkDescriptorImageInfo ii[3];
    for (int t = 0; t < 3; t++) ii[t] = (VkDescriptorImageInfo){ smp[t], tx[t] ? tx[t]->im.view : dummy.view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[5] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &ub },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &lb } };
    for (int t = 0; t < 3; t++)
        w[2 + t] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)(2 + t), .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[t] };
    push_desc(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 5, w);
    float size[2] = { (float)d->width, (float)d->height };
    vkCmdPushConstants(cmd, playout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof size, size);

    VkViewport vp = { 0, 0, (float)(d->width * scale), (float)(d->height * scale), 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sr = { { 0, 0 }, { d->width, d->height } };
    if ((R[0x065] & 3) == 3) {
        int32_t x1 = (int32_t)(R[0x066] & 0x3ff), y1 = (int32_t)((R[0x066] >> 16) & 0x3ff), x2 = (int32_t)(R[0x067] & 0x3ff) + 1, y2 = (int32_t)((R[0x067] >> 16) & 0x3ff) + 1;
        if (x2 > (int32_t)d->width) x2 = (int32_t)d->width;
        if (y2 > (int32_t)d->height) y2 = (int32_t)d->height;
        sr.offset.x = x1; sr.offset.y = y1;
        sr.extent.width = x2 > x1 ? (uint32_t)(x2 - x1) : 0; sr.extent.height = y2 > y1 ? (uint32_t)(y2 - y1) : 0;
    }
    sr.offset.x *= (int32_t)scale; sr.offset.y *= (int32_t)scale; sr.extent.width *= scale; sr.extent.height *= scale;
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
/* Running totals for the profiler: ns waiting for the GPU, writing surfaces back, preparing textures; texture uploads. */
void vkr_counters(uint64_t out[4]) { out[0] = t_wait; out[1] = t_fin; out[2] = t_tex; out[3] = ntexuploads; }
void vkr_frame(void) {
    if (!vkr_on) return;
    if (!presenting) submit_wait();                               /* with presentation, vkr_present submits the frame */
    static uint64_t frames;
    if (++frames % 300 == 0) {
        LOG(1, "vulkan: GPU wait %.2f s, write-back %.2f s, textures %.2f s, pipeline builds %.2f s, stale texture catches %llu", t_wait / 1e9, t_fin / 1e9, t_tex / 1e9, t_pipe / 1e9, (unsigned long long)nstale);
        LOG(1, "vulkan: %llu submissions, %llu sync points, %llu surface downloads, %llu surface uploads, %llu texture uploads, %d pipelines, %llu frames not presented",
            (unsigned long long)nsubmits, (unsigned long long)nsyncs, (unsigned long long)ndownloads, (unsigned long long)nuploads,
            (unsigned long long)ntexuploads, npipes, (unsigned long long)nskipped);
    }
}

/* ---- full-resolution display: a display transfer that copies a rendered surface to a screen buffer is
 * remembered, so the window can show that surface at the render scale instead of the native copy ---- */
typedef struct { uint32_t in, out, w, h; int flip; } Link;
static Link links[8]; static int nlinks;
static void disp_snapshot(Surf *s, uint32_t out, int flip, uint32_t y0, uint32_t rows);
static void disp_invalidate(uint32_t out);
int vkr_display_link(uint32_t in, uint32_t out, uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh, uint32_t flags) {
    if (!vkr_on || (scale == 1 && !presenting)) return 0;
    if (presenting) disp_invalidate(out);
    int plain = !((flags >> 24) & 3) && !((flags >> 1) & 1);          /* no downscaling, tiled input */
    for (int i = 0; i < nlinks; i++) if (links[i].out == out) { links[i] = links[--nlinks]; break; }
    /* The source may be a whole rendered surface, or tile-row-aligned rows inside one (some games render both screens
     * into one buffer and transfer each part). Memory row r holds PICA y = h-1-r. */
    Surf *src = 0; uint32_t row0 = 0, rows = oh ? oh : ih;
    if (plain)
        for (int i = 0; i < NSURF && !src; i++) {
            Surf *s = &surf[i];
            if (!s->used || s->depth || s->w != iw || in < s->addr || in >= s->addr + s->size) continue;
            uint32_t rowb = s->w * cbpp[s->fmt], off = in - s->addr;
            if (off % (rowb * 8) || off / rowb + rows > s->h) continue;
            src = s; row0 = off / rowb;
        }
    if (presenting) {                                              /* why a transfer still goes through the CPU (log once each) */
        static uint32_t seen[16][3]; static int nseen;
        int known = 0; for (int i = 0; i < nseen; i++) if (seen[i][0] == in && seen[i][1] == out && seen[i][2] == flags) known = 1;
        if (!known && nseen < 16 && !src && gen > 1000) {
            seen[nseen][0] = in; seen[nseen][1] = out; seen[nseen][2] = flags; nseen++;
            LOG(1, "vulkan: display transfer %08x -> %08x (%ux%u -> %ux%u, flags %08x) goes through the CPU: %s", in, out, iw, ih, ow, oh, flags,
                !plain ? "scaled or linear input" : "source is not a rendered surface");
        }
    }
    if (!src) return 0;
    uint32_t y0 = src->h - row0 - rows;                            /* PICA y of the lowest row taken */
    if (presenting) { disp_snapshot(src, out, (int)(flags & 1), y0, rows); return !getenv("RB_DISPLAY_COPY"); }   /* the window samples
                                                                          the GPU copy: the CPU copy is skipped */
    if (row0 == 0 && rows == src->h) {                             /* full-resolution readback path (SDL window) */
        src->display = 1;
        if (nlinks == 8) links[0] = links[--nlinks];
        links[nlinks++] = (Link){ in, out, iw, ih, (int)(flags & 1) };
    }
    return 0;
}
const uint8_t *vkr_hires(uint32_t out, uint32_t want_h, uint32_t *w, uint32_t *h, int *flip, uint32_t *sc) {
    if (!vkr_on || scale == 1) return 0;
    for (int i = 0; i < nlinks; i++) {
        if (links[i].out != out || links[i].h != want_h || links[i].w != 240) continue;
        for (int k = 0; k < NSURF; k++) {
            Surf *s = &surf[k];
            if (s->used && !s->depth && s->addr == links[i].in && s->w == links[i].w && s->h == links[i].h && s->hires && !s->dirty) {
                *w = s->w * scale; *h = s->h * scale; *flip = links[i].flip; *sc = scale;
                return s->hires;
            }
        }
    }
    return 0;
}

/* ---- window presentation through Vulkan (video.present = "vulkan") ----
 * A plain display transfer of a cached surface copies it, at the render scale and in command order, into a
 * per-output display image: a snapshot, so later rendering into the same target cannot show early. vkr_present
 * draws each screen from its snapshot (or from the native image the CPU composed, when there is none), then the
 * overlay (menu, FPS) with alpha, into the swapchain. Present never waits for the monitor (MAILBOX or IMMEDIATE). */
#define NDISP 8                                            /* screens x double buffers x stereo eyes */
typedef struct { uint32_t out, w, h; int flip, valid; uint64_t last; Img im; } Disp;
static Disp disp[NDISP]; static uint64_t disp_clock;
static void disp_invalidate(uint32_t out) { for (int i = 0; i < NDISP; i++) if (disp[i].out == out) disp[i].valid = 0; }
static void disp_snapshot(Surf *s, uint32_t out, int flip, uint32_t y0, uint32_t rows) {
    Disp *d = 0;
    for (int i = 0; i < NDISP && !d; i++) if (disp[i].out == out) d = &disp[i];
    for (int i = 0; i < NDISP && !d; i++) if (!disp[i].im.img) d = &disp[i];
    if (!d) { d = &disp[0]; for (int i = 1; i < NDISP; i++) if (disp[i].last < d->last) d = &disp[i]; }   /* least recently used */
    d->last = ++disp_clock;
    uint32_t W = s->w * scale, H = rows * scale;                  /* rows y0..y0+rows of the surface (PICA y) */
    if (d->im.img && (d->w != W || d->h != H)) bury(&d->im);
    if (!d->im.img) mkimg(&d->im, W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    begin_cmd(); end_render(); barrier();
    VkImageCopy c = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffset = { 0, (int32_t)(y0 * scale), 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { W, H, 1 } };
    vkCmdCopyImage(cmd, s->im.img, VK_IMAGE_LAYOUT_GENERAL, d->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    barrier();
    d->out = out; d->w = W; d->h = H; d->flip = flip; d->valid = 1;
    { static int ls = -1; if (ls < 0) ls = getenv("RB_LOG_SNAP") != 0;
      if (ls) fprintf(stderr, "snap: frame %llu surface %08x -> out %08x rows %u..%u flip %d slot %d\n", (unsigned long long)gpu_frame(), s->addr, out, y0, y0 + rows, flip, (int)(d - disp)); }
}

static VkSwapchainKHR swap;
static VkImage simg[8]; static VkImageView sview[8]; static uint32_t nsimg;
static VkExtent2D sext; static VkFormat sfmt;
static VkSemaphore rel_sem[8];
static VkFence acq_fence; static int pend_idx = -1;   /* acquired image not yet free */
static VkPipeline ppipe[2]; static VkPipelineLayout pplayout; static VkDescriptorSetLayout pdsl;
static VkSampler lin_sampler;
static Img nat[2], ovl; static uint32_t ovw, ovh; static int need_recreate;

static void destroy_swapchain(void) {
    vkDeviceWaitIdle(dev);
    if (pend_idx >= 0) { vkWaitForFences(dev, 1, &acq_fence, VK_TRUE, 100000000ull); vkResetFences(dev, 1, &acq_fence); pend_idx = -1; }
    for (uint32_t i = 0; i < nsimg; i++) { vkDestroyImageView(dev, sview[i], 0); vkDestroySemaphore(dev, rel_sem[i], 0); }
    nsimg = 0;
}
static int create_swapchain(uint32_t w, uint32_t h) {
    VkSwapchainKHR old = swap;
    destroy_swapchain();
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdev, vsurf, &caps);
    VkExtent2D e = caps.currentExtent.width != 0xffffffffu ? caps.currentExtent : (VkExtent2D){ w, h };
    if (!e.width || !e.height) return 0;                          /* minimised */
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, vsurf, &nf, 0);
    VkSurfaceFormatKHR fm[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, vsurf, &nf, fm);
    VkSurfaceFormatKHR f = fm[0];
    for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) { f = fm[i]; break; }
    uint32_t npm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(pdev, vsurf, &npm, 0);
    VkPresentModeKHR pm[16]; if (npm > 16) npm = 16; vkGetPhysicalDeviceSurfacePresentModesKHR(pdev, vsurf, &npm, pm);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < npm; i++) if (pm[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = pm[i];
    for (uint32_t i = 0; i < npm; i++) if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = pm[i];
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vsurf, .minImageCount = count,
        .imageFormat = f.format, .imageColorSpace = f.colorSpace, .imageExtent = e, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = mode, .clipped = VK_TRUE, .oldSwapchain = old };
    if (vkCreateSwapchainKHR(dev, &ci, 0, &swap) != VK_SUCCESS) { swap = 0; return 0; }
    if (old) vkDestroySwapchainKHR(dev, old, 0);
    nsimg = 8; vkGetSwapchainImagesKHR(dev, swap, &nsimg, simg);
    for (uint32_t i = 0; i < nsimg; i++) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = simg[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = f.format, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        CHK(vkCreateImageView(dev, &vi, 0, &sview[i]));
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        CHK(vkCreateSemaphore(dev, &si, 0, &rel_sem[i]));
    }
    if (sfmt != f.format) {                                        /* pipelines are built for the swapchain format */
        for (int i = 0; i < 2; i++) if (ppipe[i]) { vkDestroyPipeline(dev, ppipe[i], 0); ppipe[i] = 0; }
        sfmt = f.format;
    }
    sext = e;
    static const char *mn[4] = { "immediate", "mailbox", "fifo", "fifo-relaxed" };
    LOG(1, "vulkan: presenting %ux%u, %u images, %s", e.width, e.height, nsimg, mode < 4 ? mn[mode] : "?");
    return 1;
}
static void present_pipelines(void) {
    if (!pdsl) {
        VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0 };
        VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 1, .pBindings = &b };
        CHK(vkCreateDescriptorSetLayout(dev, &dli, 0, &pdsl));
        VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
        VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &pdsl,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
        CHK(vkCreatePipelineLayout(dev, &pli, 0, &pplayout));
        VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
        CHK(vkCreateSampler(dev, &sci, 0, &lin_sampler));
    }
    if (ppipe[0]) return;
    VkShaderModule vm, fm;
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof pvert_spv, .pCode = pvert_spv };
    CHK(vkCreateShaderModule(dev, &smi, 0, &vm));
    smi.codeSize = sizeof pfrag_spv; smi.pCode = pfrag_spv;
    CHK(vkCreateShaderModule(dev, &smi, 0, &fm));
    for (int blend = 0; blend < 2; blend++) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" } };
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
        VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState ba = { .blendEnable = blend, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 15 };
        VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &sfmt };
        VkGraphicsPipelineCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
            .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pplayout };
        CHK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, 0, &ppipe[blend]));
    }
    vkDestroyShaderModule(dev, vm, 0); vkDestroyShaderModule(dev, fm, 0);
}
static void upload_rgba(Img *im, uint32_t *cw, uint32_t *ch, uint32_t w, uint32_t h, const uint8_t *src, size_t stride) {
    if (im->img && (*cw != w || *ch != h)) bury(im);
    if (!im->img) { mkimg(im, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT); *cw = w; *ch = h; }
    VkDeviceSize off = ring_alloc(&ring, (VkDeviceSize)w * h * 4);
    for (uint32_t y = 0; y < h; y++) memcpy(ring.map + off + (size_t)y * w * 4, src + (size_t)y * stride, (size_t)w * 4);
    VkBufferImageCopy c = { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
    vkCmdCopyBufferToImage(cmd, ring.buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

int vkr_presenting(void) { return presenting; }
int vkr_init_present(const char *const *exts, uint32_t n, int (*mksurf)(void *instance, uint64_t *surface)) {
    inst_exts = exts; ninst_exts = n; mk_surface = mksurf;
    vkr_init();
    return presenting;
}
void vkr_present(const VkrScreen scr[2], uint32_t win_w, uint32_t win_h, const uint8_t *overlay, uint32_t ow, uint32_t oh, int overlay_dirty, int fxaa) {
    if (!presenting) return;
    if (!swap || need_recreate || sext.width != win_w || sext.height != win_h) {
        need_recreate = 0;
        if (!create_swapchain(win_w, win_h)) return;
    }
    present_pipelines();
    /* The GPU queue never waits for the compositor: a window image is requested without blocking and drawn into only
     * once it is free (CPU-side fence check). Until then the frame's rendering is submitted on its own and the window
     * keeps showing the previous frame. */
    if (!acq_fence) { VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; CHK(vkCreateFence(dev, &fi, 0, &acq_fence)); }
    VkResult r = VK_SUCCESS;
    if (pend_idx < 0) {
        uint32_t i2;
        r = vkAcquireNextImageKHR(dev, swap, 0, VK_NULL_HANDLE, acq_fence, &i2);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { pend_idx = (int)i2; if (r == VK_SUBOPTIMAL_KHR) need_recreate = 1; }
        else if (r != VK_NOT_READY && r != VK_TIMEOUT) { if (r != VK_ERROR_OUT_OF_DATE_KHR) LOG(1, "vulkan: acquire failed (%d)", r); need_recreate = 1; }
    }
    if (pend_idx < 0 || vkGetFenceStatus(dev, acq_fence) != VK_SUCCESS) {
        nskipped++;
        begin_cmd(); submit_async(VK_NULL_HANDLE, VK_NULL_HANDLE);     /* rendering only; present a later frame */
        return;
    }
    CHK(vkResetFences(dev, 1, &acq_fence));
    uint32_t idx = (uint32_t)pend_idx; pend_idx = -1;
    VkSemaphore acq_sem = VK_NULL_HANDLE;
    if (recording) {                                              /* rendering first, then the present pass */
        end_render();
        CHK(vkEndCommandBuffer(cmd));
        VkSubmitInfo gs = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        CHK(vkQueueSubmit(queue, 1, &gs, VK_NULL_HANDLE));
        recording = 0; nsubmits++;
        VkCommandBuffer t = ctxs[cur_ctx].cb; ctxs[cur_ctx].cb = ctxs[cur_ctx].cb2; ctxs[cur_ctx].cb2 = t;
    }
    begin_cmd(); end_render();
    static uint32_t nw[2], nh[2];
    for (int k = 0; k < 2; k++) if (scr[k].native) upload_rgba(&nat[k], &nw[k], &nh[k], scr[k].nw, 240, scr[k].native, scr[k].stride);
    if (overlay && (overlay_dirty || !ovl.img || ovw != ow || ovh != oh)) upload_rgba(&ovl, &ovw, &ovh, ow, oh, overlay, (size_t)ow * 4);
    barrier();
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = simg[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, 0, 0, 0, 1, &b);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = sview[idx], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0, 0, 0, 1 } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, sext }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = { 0, 0, (float)sext.width, (float)sext.height, 0, 1 }; VkRect2D sc = { { 0, 0 }, sext };
    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
    float W = (float)sext.width, H = (float)sext.height;
    for (int k = 0; k < 3; k++) {
        VkImageView view = 0; float pc[8] = { 0 };
        if (k < 2) {
            if (scr[k].rect[2] <= 0) continue;
            Disp *d = 0;
            for (int i = 0; i < NDISP; i++) if (disp[i].valid && disp[i].out == scr[k].fb && scr[k].fb) d = &disp[i];
            if (d && d->h == scr[k].nw * scale && d->w == 240 * scale) { view = d->im.view; pc[4] = 1; pc[5] = (float)d->flip; pc[6] = fxaa ? 1.0f : 0.0f; }
            else if (scr[k].native) { view = nat[k].view; pc[4] = 0; }
            else continue;
            pc[0] = scr[k].rect[0] / W * 2 - 1; pc[1] = scr[k].rect[1] / H * 2 - 1;
            pc[2] = (scr[k].rect[0] + scr[k].rect[2]) / W * 2 - 1; pc[3] = (scr[k].rect[1] + scr[k].rect[3]) / H * 2 - 1;
        } else {
            if (!overlay || !ovl.img) continue;
            view = ovl.view; pc[0] = -1; pc[1] = -1; pc[2] = 1; pc[3] = 1; pc[4] = 2;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ppipe[k == 2]);
        VkDescriptorImageInfo ii = { lin_sampler, view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
        push_desc(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pplayout, 0, 1, &w);
        vkCmdPushConstants(cmd, pplayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
    static long long dump_at = -2; static long long nframe; static VkDeviceSize dump_off; int dumping = 0;
    if (dump_at == -2) { const char *e = getenv("RB_PRESENT_DUMP"); dump_at = e ? atoll(e) : -1; }
    if (++nframe == dump_at) {                                     /* test: copy the presented image out (raw BGRA/RGBA) */
        VkImageMemoryBarrier t = b; t.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; t.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        t.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; t.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &t);
        dump_off = ring_alloc(&rbk, (VkDeviceSize)sext.width * sext.height * 4);
        VkBufferImageCopy c = { .bufferOffset = dump_off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { sext.width, sext.height, 1 } };
        vkCmdCopyImageToBuffer(cmd, simg[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbk.buf, 1, &c);
        t.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; t.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        t.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; t.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, 0, 0, 0, 1, &t);
        dumping = 1;
    }
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &b);
    submit_async(acq_sem, rel_sem[idx]);                          /* the CPU goes on with the next frame */
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &rel_sem[idx],
        .swapchainCount = 1, .pSwapchains = &swap, .pImageIndices = &idx };
    r = vkQueuePresentKHR(queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) need_recreate = 1;
    if (dumping) {                                                /* test only: wait for this frame and save it */
        CHK(vkQueueWaitIdle(queue));
        FILE *f = fopen("build/present_dump.raw", "wb");
        if (f) { fwrite(rbk.map + dump_off, 4, (size_t)sext.width * sext.height, f); fclose(f); }
        LOG(1, "vulkan: presented frame written to build/present_dump.raw (%ux%u, format %d)", sext.width, sext.height, sfmt);
    }
}

/* ---- setup ---- */
int vkr_init(void) {
    if (init_done) return vkr_on;
    init_done = 1;
    const char *sel = getenv("RB_RENDERER");
    if (sel && strcmp(sel, "vulkan")) return 0;                     /* default: Vulkan when available */
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "3dsrecomp", .apiVersion = VK_API_VERSION_1_3 };
    const char *layer = "VK_LAYER_KHRONOS_validation";
    int validate_on = getenv("RB_VK_VALIDATE") != 0;
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .enabledLayerCount = validate_on ? 1 : 0, .ppEnabledLayerNames = &layer,
        .enabledExtensionCount = mk_surface ? ninst_exts : 0, .ppEnabledExtensionNames = mk_surface ? inst_exts : 0 };
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
    VkPhysicalDeviceFeatures feats; vkGetPhysicalDeviceFeatures(pdev, &feats);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE, .shaderDemoteToHelperInvocation = VK_TRUE };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13, .features = { .logicOp = VK_TRUE, .samplerAnisotropy = feats.samplerAnisotropy } };
    const char *exts[2] = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    if (mk_surface) {                                              /* window presentation: surface on this queue family */
        uint64_t sh = 0; VkBool32 ok = VK_FALSE;
        if (mk_surface(inst, &sh)) { vsurf = (VkSurfaceKHR)(uintptr_t)sh; vkGetPhysicalDeviceSurfaceSupportKHR(pdev, qfam, vsurf, &ok); }
        presenting = ok == VK_TRUE;
        if (!presenting) LOG(1, "vulkan: cannot present to the window, using SDL presentation");
    }
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = presenting ? 2 : 1, .ppEnabledExtensionNames = exts };
    if (vkCreateDevice(pdev, &dci, 0, &dev) != VK_SUCCESS) { LOG(1, "vulkan: device creation failed, using the software renderer"); return 0; }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    push_desc = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
    if (!push_desc) fatal("vulkan: vkCmdPushDescriptorSetKHR missing");

    for (int c = 0; c < 2; c++) {
        Ctx *x = &ctxs[c];
        VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam };
        CHK(vkCreateCommandPool(dev, &cpi, 0, &x->pool));
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = x->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        CHK(vkAllocateCommandBuffers(dev, &cai, &x->cb));
        CHK(vkAllocateCommandBuffers(dev, &cai, &x->cb2));
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        CHK(vkCreateFence(dev, &fci, 0, &x->fen));
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        CHK(vkCreateSemaphore(dev, &si, 0, &x->acq));
    }
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
    mkimg_full(&dummy, 1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    submit_wait();
    const char *fm = getenv("RB_TEX_FILTER");
    const char *an = getenv("RB_ANISOTROPY");
    max_aniso = an ? (float)atof(an) : 1; if (max_aniso < 1) max_aniso = 1;
    if (!feats.samplerAnisotropy) max_aniso = 1;
    if (max_aniso > props.limits.maxSamplerAnisotropy) max_aniso = props.limits.maxSamplerAnisotropy;
    filter_mode = !fm || !strcmp(fm, "console") ? 1 : !strcmp(fm, "nearest") ? 0 : !strcmp(fm, "linear") ? 2 : 1;
    const char *sc = getenv("RB_SCALE");
    scale = sc ? (uint32_t)atoi(sc) : 1; if (scale < 1) scale = 1; if (scale > 4) scale = 4;
    vkr_on = 1;
    LOG(1, "vulkan: rendering on %s at %ux (depth-stencil format %s)", props.deviceName, scale, d24fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32FS8");
    return 1;
}
