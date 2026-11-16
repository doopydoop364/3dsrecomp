/* Vulkan rasteriser for PICA200 draws (runtime/vk.c). The CPU still runs vertex shading, clipping
 * and primitive assembly (pica.c); this backend rasterises the resulting screen-space triangles and
 * runs the fragment stage on the GPU. Guest memory stays the truth: render targets live in a surface
 * cache and are written back to guest memory when something else needs those bytes. */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t on, addr, w, h, fmt, size;   /* addr: guest virtual; size in bytes */
    uint32_t wrap, border;                /* wrap_s | wrap_t << 8 */
    uint32_t levels, param, lod;          /* mip levels present; raw filter (0x083-style) and LOD registers */
    uint32_t faces, face_addr[6];         /* 6 for a cube map (unit 0), else 1; face_addr[0] = addr */
} VkrTex;
typedef struct {
    uint32_t caddr, daddr;                /* guest virtual, 0 when unmapped */
    uint32_t width, height, cfmt, dfmt;
    const uint32_t *regs;                 /* 0x300 PICA registers */
    const uint32_t *lut;                  /* 24 x 256 lighting LUT entries */
    const uint32_t *fog_lut;              /* 128 fog LUT entries */
    uint64_t lut_version;
    float lights[48][4], gamb[4], zscale, zoff;
    uint32_t flat, flat_color, nlights;
    VkrTex tex[3];
} VkrDraw;

int vkr_init(void);                      /* 1 when the Vulkan backend is selected and ready */
void vkr_draw(const VkrDraw *d, const void *tris, int ntris, void (*decode)(int unit, int face, int level, uint8_t *rgba));
void vkr_sync_range(uint32_t addr, uint32_t size, int will_write);   /* before anything else touches guest memory */
int vkr_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t ctl);   /* 1 if done on the GPU */
void vkr_sync_all(void);
void vkr_new_gen(void);                  /* guest memory may have changed (between GPU submissions) */
void vkr_frame(void);                    /* end of a video frame */
extern int vkr_on;
/* Before a display transfer. Returns 1 if the window will show a GPU snapshot of the source, so the transfer into guest
 * memory can be skipped (it would force the CPU to wait for the GPU every frame). */
int vkr_display_link(uint32_t in, uint32_t out, uint32_t iw, uint32_t ih, uint32_t flags);
/* Full-resolution RGBA (rows = PICA y, 240*scale wide) of the surface last transferred to `out`, or 0. */
const uint8_t *vkr_hires(uint32_t out, uint32_t want_h, uint32_t *w, uint32_t *h, int *flip, uint32_t *scale);

/* Window presentation through Vulkan (video.present = "vulkan"). vkr_init_present: before the first draw, with the
 * window system's instance extensions and a callback that creates the window surface; returns 1 if presenting. */
int vkr_init_present(const char *const *exts, uint32_t n, int (*mksurf)(void *instance, uint64_t *surface));
int vkr_presenting(void);
typedef struct {
    uint32_t fb;                 /* guest framebuffer address shown on this screen (matches a display-transfer output) */
    const uint8_t *native;       /* the native screen image (RGBA, nw x 240, row stride bytes), used without a snapshot */
    uint32_t nw; size_t stride;
    float rect[4];               /* x, y, w, h in window pixels; w <= 0 hides the screen */
} VkrScreen;
void vkr_present(const VkrScreen scr[2], uint32_t win_w, uint32_t win_h, const uint8_t *overlay, uint32_t ow, uint32_t oh, int overlay_dirty, int fxaa);
void vkr_counters(uint64_t out[4]);
extern int vkr_why;   /* set before vkr_sync_range: 1 dma, 2 fill, 3 display transfer, 4 texture copy, 5 vblank, 6 surface, 7 texture */
void vkr_sync_counts(uint64_t waits[8], uint64_t downloads[8]);
