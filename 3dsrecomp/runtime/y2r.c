/* y2r:u — the YUV to RGB converter that video players use to turn decoded frames into textures.
 * The whole conversion runs synchronously in StartConversion, then the transfer-end event is signalled.
 * Hardware behaviour (coefficient math, 8-line strips, transfer unit and gap, 8x8 Morton blocks,
 * rotation, output encodings) follows the Citra/Azahar implementation, which is bit-exact as far as
 * it has been tested. Nothing here depends on the game. */
#include "host.h"
#include <string.h>

typedef struct { uint32_t addr, size, unit, gap; } Buf;
enum { IN_422_8, IN_420_8, IN_422_16, IN_420_16, IN_YUYV };
enum { OUT_RGBA8, OUT_RGB8, OUT_RGB5A1, OUT_RGB565 };

static struct {
    uint32_t in_fmt, out_fmt, rotation, block, width, lines, alpha, end_irq, std_coef;
    int16_t coef[8];
    Buf y, u, v, yuyv, dst;
    uint32_t spatial, temporal; uint16_t dither[16];
} c;
static KObject *end_event;

static const int16_t standard[4][8] = {
    { 0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B },   /* ITU-R BT.601 */
    { 0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51 },    /* ITU-R BT.709 */
    { 0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B },   /* BT.601, 16-235 input range */
    { 0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421 },    /* BT.709, 16-235 input range */
};

static uint8_t *gptr(uint32_t addr, uint32_t size, const char *what) {
    if (!mem_valid(addr, size)) { LOG(1, "y2r: %s buffer %08x+%x is not mapped, conversion skipped", what, addr, size); return 0; }
    return (uint8_t *)(uintptr_t)addr;
}

/* Gather `count` samples from a source buffer: `unit` bytes, then skip `gap`. 16-bit inputs keep the low byte. */
static int receive(uint8_t *out, Buf *b, uint32_t count, int bytes, const char *what) {
    uint32_t per_unit = b->unit / bytes;
    if (!per_unit) { LOG(1, "y2r: %s transfer unit is 0", what); return 0; }
    while (count) {
        uint32_t n = count < per_unit ? count : per_unit;
        const uint8_t *in = gptr(b->addr, n * bytes, what);
        if (!in) return 0;
        for (uint32_t i = 0; i < n; i++) out[i] = in[i * bytes];
        out += n; count -= n;
        b->addr += b->unit + b->gap;
    }
    return 1;
}

/* Pack RGBA (r<<24 | g<<16 | b<<8) pixels into the output format, `unit` bytes then `gap` skipped. */
static int send(const uint32_t *px, uint32_t count, Buf *b) {
    static const int bpp[4] = { 4, 3, 2, 2 };
    int bp = bpp[c.out_fmt & 3];
    uint8_t a = (uint8_t)c.alpha;
    while (count) {
        uint32_t n = b->unit / bp;
        if (!n) { LOG(1, "y2r: output transfer unit is 0"); return 0; }
        if (n > count) n = count;
        uint8_t *o = gptr(b->addr, n * bp, "output");
        if (!o) return 0;
        for (uint32_t i = 0; i < n; i++, px++) {
            uint8_t r = *px >> 24, g = *px >> 16, bl = *px >> 8;
            switch (c.out_fmt & 3) {
            case OUT_RGBA8: o[0] = a; o[1] = bl; o[2] = g; o[3] = r; o += 4; break;
            case OUT_RGB8:  o[0] = bl; o[1] = g; o[2] = r; o += 3; break;
            case OUT_RGB5A1: { uint16_t v = (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (bl >> 3) << 1 | (a >> 7)); memcpy(o, &v, 2); o += 2; break; }
            case OUT_RGB565: { uint16_t v = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (bl >> 3)); memcpy(o, &v, 2); o += 2; break; }
            }
        }
        count -= n;
        b->addr += b->unit + b->gap;
    }
    return 1;
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void convert(void) {
    uint32_t w = c.width, lines = c.lines, tiles = w / 8;
    if (!w || w % 8 || w > 1024 || (c.block && lines % 8)) {
        LOG(1, "y2r: unsupported size %ux%u (block mode %u)", w, lines, c.block); return;
    }
    uint8_t morton[64], linear[64];
    for (int i = 0; i < 64; i++) {
        int x = i & 7, y = i >> 3;
        linear[i] = (uint8_t)i;
        morton[i] = (uint8_t)((x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3);
    }
    const uint8_t *remap = c.block ? morton : linear;
    Buf y = c.y, u = c.u, v = c.v, yuyv = c.yuyv, dst = c.dst;
    uint8_t *in_y = malloc(w * 8 * 2), *in_u = malloc(w * 8), *in_v = malloc(w * 8);
    uint32_t (*tile)[64] = malloc(tiles * sizeof *tile), *out = malloc(w * 8 * 4), tmp[64];
    const int16_t *k = c.coef;

    for (uint32_t row = 0; row < lines; row += 8) {
        uint32_t h = lines - row < 8 ? lines - row : 8, n = h * w;
        int ok = 1, bytes = (c.in_fmt == IN_422_16 || c.in_fmt == IN_420_16) ? 2 : 1;
        switch (c.in_fmt) {
        case IN_422_8: case IN_422_16:
            ok = receive(in_y, &y, n, bytes, "Y") && receive(in_u, &u, n / 2, bytes, "U") && receive(in_v, &v, n / 2, bytes, "V"); break;
        case IN_420_8: case IN_420_16:
            ok = receive(in_y, &y, n, bytes, "Y") && receive(in_u, &u, n / 4, bytes, "U") && receive(in_v, &v, n / 4, bytes, "V"); break;
        case IN_YUYV: ok = receive(in_y, &yuyv, n * 2, 1, "YUYV"); break;
        default: LOG(1, "y2r: input format %u is not valid", c.in_fmt); ok = 0;
        }
        if (!ok) break;

        for (uint32_t yy = 0; yy < h; yy++)
            for (uint32_t x = 0; x < w; x++) {
                int Y, U, V;
                switch (c.in_fmt) {
                case IN_422_8: case IN_422_16: Y = in_y[yy * w + x]; U = in_u[(yy * w + x) / 2]; V = in_v[(yy * w + x) / 2]; break;
                case IN_420_8: case IN_420_16: Y = in_y[yy * w + x]; U = in_u[((yy / 2) * w + x) / 2]; V = in_v[((yy / 2) * w + x) / 2]; break;
                default: Y = in_y[(yy * w + x) * 2]; U = in_y[(yy * w + (x / 2) * 2) * 2 + 1]; V = in_y[(yy * w + (x / 2) * 2) * 2 + 3];
                }
                int cy = k[0] * Y;
                int r = ((cy + k[1] * V) >> 3) + k[5] + 0x18;
                int g = ((cy - k[2] * V - k[3] * U) >> 3) + k[6] + 0x18;
                int b = ((cy + k[4] * U) >> 3) + k[7] + 0x18;
                tile[x / 8][yy * 8 + x % 8] = (uint32_t)clamp8(r >> 5) << 24 | (uint32_t)clamp8(g >> 5) << 16 | (uint32_t)clamp8(b >> 5) << 8;
            }

        uint32_t *o = out;
        for (uint32_t t = 0; t < tiles; t++) {
            int oi = 0, strip_w = 8, stride = 8 * h;
            const uint32_t *src = tile[(c.rotation == 2 || c.rotation == 3) ? tiles - 1 - t : t];
            memset(tmp, 0, sizeof tmp);
            switch (c.rotation) {
            case 0: for (uint32_t i = 0; i < h * 8; i++) tmp[remap[i]] = src[i]; strip_w = w; stride = 8; break;
            case 1: for (int x = 0; x < 8; x++) for (int yy = h - 1; yy >= 0; yy--) tmp[remap[oi++]] = src[yy * 8 + x]; break;
            case 2: for (int i = h * 8 - 1; i >= 0; i--) tmp[remap[oi++]] = src[i]; strip_w = w; stride = 8; break;
            default: for (int x = 7; x >= 0; x--) for (uint32_t yy = 0; yy < h; yy++) tmp[remap[oi++]] = src[yy * 8 + x]; break;
            }
            if (c.block) { memcpy(o, tmp, sizeof tmp); o += 64; }
            else {
                for (uint32_t yy = 0; yy < h; yy++) memcpy(o + yy * strip_w, tmp + yy * 8, 8 * sizeof *tmp);
                o += stride;
            }
        }
        if (!send(out, n, &dst)) break;
    }
    free(in_y); free(in_u); free(in_v); free(tile); free(out);
}

static void reply(uint32_t *cmd, int normal, int translate) {
    cmd[0] = (cmd[0] & 0xffff0000u) | ((uint32_t)normal << 6) | (uint32_t)translate;
    cmd[1] = RES_OK;
}
static void set_buf(Buf *b, const uint32_t *cmd) { b->addr = cmd[1]; b->size = cmd[2]; b->unit = cmd[3] & 0xffff; b->gap = cmd[4] & 0xffff; }
static void get1(uint32_t *cmd, uint32_t v) { reply(cmd, 2, 0); cmd[2] = v; }

void y2r_service(KObject *s, uint32_t *cmd) {
    (void)s;
    uint32_t a = cmd[1];
    switch (cmd[0] >> 16) {
    case 0x01: c.in_fmt = a; reply(cmd, 1, 0); break;               /* SetInputFormat */
    case 0x02: get1(cmd, c.in_fmt); break;
    case 0x03: c.out_fmt = a; reply(cmd, 1, 0); break;              /* SetOutputFormat */
    case 0x04: get1(cmd, c.out_fmt); break;
    case 0x05: c.rotation = a & 3; reply(cmd, 1, 0); break;         /* SetRotation */
    case 0x06: get1(cmd, c.rotation); break;
    case 0x07: c.block = a; reply(cmd, 1, 0); break;                /* SetBlockAlignment: 0 lines, 1 8x8 blocks */
    case 0x08: get1(cmd, c.block); break;
    case 0x09: c.spatial = a; reply(cmd, 1, 0); break;              /* dithering: accepted, not applied */
    case 0x0a: get1(cmd, c.spatial); break;
    case 0x0b: c.temporal = a; reply(cmd, 1, 0); break;
    case 0x0c: get1(cmd, c.temporal); break;
    case 0x0d: c.end_irq = a; reply(cmd, 1, 0); break;              /* SetTransferEndInterrupt */
    case 0x0e: get1(cmd, c.end_irq); break;
    case 0x0f:                                                      /* GetTransferEndEvent */
        if (!end_event) end_event = k_event(0);
        reply(cmd, 1, 2); cmd[2] = 0; cmd[3] = h_new(end_event); break;
    case 0x10: set_buf(&c.y, cmd); reply(cmd, 1, 0); break;         /* SetSendingY/U/V/YUYV(addr, size, unit, gap) */
    case 0x11: set_buf(&c.u, cmd); reply(cmd, 1, 0); break;
    case 0x12: set_buf(&c.v, cmd); reply(cmd, 1, 0); break;
    case 0x13: set_buf(&c.yuyv, cmd); reply(cmd, 1, 0); break;
    case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: get1(cmd, 1); break;   /* IsFinishedSending* / Receiving */
    case 0x18: set_buf(&c.dst, cmd); reply(cmd, 1, 0); break;       /* SetReceiving */
    case 0x1a: c.width = a & 0xffff; reply(cmd, 1, 0); break;       /* SetInputLineWidth */
    case 0x1b: get1(cmd, c.width); break;
    case 0x1c: c.lines = a & 0xffff; reply(cmd, 1, 0); break;       /* SetInputLines */
    case 0x1d: get1(cmd, c.lines); break;
    case 0x1e: memcpy(c.coef, &cmd[1], 16); reply(cmd, 1, 0); break;   /* SetCoefficient */
    case 0x1f: reply(cmd, 5, 0); memcpy(&cmd[2], c.coef, 16); break;
    case 0x20: c.std_coef = a & 3; memcpy(c.coef, standard[a & 3], 16); reply(cmd, 1, 0); break;   /* SetStandardCoefficient */
    case 0x21: reply(cmd, 5, 0); memcpy(&cmd[2], standard[a & 3], 16); break;                      /* GetStandardCoefficient */
    case 0x22: c.alpha = a & 0xff; reply(cmd, 1, 0); break;         /* SetAlpha */
    case 0x23: get1(cmd, c.alpha); break;
    case 0x24: memcpy(c.dither, &cmd[1], 32); reply(cmd, 1, 0); break;
    case 0x25: reply(cmd, 9, 0); memcpy(&cmd[2], c.dither, 32); break;
    case 0x26:                                                      /* StartConversion */
        convert();
        if (c.end_irq && end_event) k_signal(end_event);
        reply(cmd, 1, 0); break;
    case 0x27: reply(cmd, 1, 0); break;                             /* StopConversion */
    case 0x28: get1(cmd, 0); break;                                 /* IsBusyConversion */
    case 0x29: {                                                    /* SetPackageParameter: packed 12-byte block */
        const uint8_t *p = (const uint8_t *)&cmd[1];
        c.in_fmt = p[0]; c.out_fmt = p[1]; c.rotation = p[2] & 3; c.block = p[3];
        c.width = p[4] | p[5] << 8; c.lines = p[6] | p[7] << 8;
        c.std_coef = p[8] & 3; memcpy(c.coef, standard[c.std_coef], 16); c.alpha = p[10];
        reply(cmd, 1, 0); break;
    }
    case 0x2a: get1(cmd, 0); break;                                 /* PingProcess */
    case 0x2b: case 0x2c: reply(cmd, 1, 0); break;                  /* DriverInitialize / Finalize */
    default:
        LOG(1, "STUB y2r:u     cmd %08x  args %08x %08x %08x %08x", cmd[0], cmd[1], cmd[2], cmd[3], cmd[4]);
        reply(cmd, 1, 0);
    }
}


/* ---- save states ---- */
void y2r_state(SS *s) { ss_tag(s, "y2r"); SS_V(s, c); ss_obj(s, &end_event); }
