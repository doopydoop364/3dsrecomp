/* DSP audio, high level: the 24 voices the game configures in DSP shared memory are decoded (PCM8, PCM16,
 * DSP-ADPCM), resampled, mixed and sent to the host audio device, and the per-voice status the game's sound
 * library waits on (buffer ids, play positions, sync counts) is reported back.
 *
 * Shared-memory layout, queue order and the ADPCM decoder follow the Azahar HLE
 * (audio_core/hle/shared_memory.h, source.cpp, codec.cpp). One audio frame is 160 stereo samples at
 * 32728 Hz. The two shared regions sit at +0x50000 and +0x70000 of DSP memory; configuration found in either
 * one is applied, and status is written to both.
 *
 * Not done: polyphase interpolation (linear is used), per-voice filters, gain ramps, aux effects
 * (delay, reverb) and the surround/compressor stages. */
#include "audio.h"
#include "host.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DSP_MEM 0x1ff00000u
#define NSRC 24
#define FRAME 160
#define OFF_FINAL 0xa80u
#define OFF_STATUS 0xd00u
#define OFF_DSPCONF 0x2860u
#define OFF_CONFIG 0x3d24u
#define OFF_ADPCM 0x4f24u
static const uint32_t regions[2] = { 0x50000u, 0x70000u };

enum { FMT_PCM8, FMT_PCM16, FMT_ADPCM };

typedef struct {
    uint32_t addr, len, start; uint16_t id; uint8_t loop, from_queue, played, adpcm_dirty, stereo, fmt;
    int16_t yn1, yn2;
} Buf;

typedef struct {
    int enabled, buffer_update, have_cur; uint16_t sync, cur_id, last_id;
    int stereo, fmt;
    float rate, gain[3][4];
    int16_t coef[16], yn1, yn2;
    Buf q[8]; int nq;
    int16_t *pcm; uint32_t pcm_len, pos; double frac;   /* current buffer, decoded to stereo */
} Voice;

static Voice v[NSRC];
static float master_volume = 1.0f, aux_return[2] = { 1.0f, 1.0f };

static uint32_t dspw(const uint8_t *p) { uint32_t x; memcpy(&x, p, 4); return (x << 16) | (x >> 16); }
static uint16_t rd16(const uint8_t *p) { uint16_t x; memcpy(&x, p, 2); return x; }
static uint8_t *region(int r) { return (uint8_t *)(uintptr_t)(DSP_MEM + regions[r]); }

static const uint8_t *phys_ptr(uint32_t p, uint32_t size) {
    uint32_t a;
    if (p - 0x20000000u < 0x08000000u) a = p - 0x20000000u + 0x30000000u;        /* FCRAM, linear mapping */
    else if (p - 0x18000000u < 0x00600000u) a = p - 0x18000000u + 0x1f000000u;   /* VRAM */
    else if (p - 0x1ff00000u < 0x00080000u) a = p;                               /* DSP memory */
    else return 0;
    return mem_valid(a, size ? size : 1) ? (const uint8_t *)(uintptr_t)a : 0;
}

/* Decode a whole buffer to interleaved stereo; ADPCM keeps its history in the voice like the hardware. */
static void decode(Voice *s, const Buf *b) {
    free(s->pcm); s->pcm = 0; s->pcm_len = 0;
    uint32_t n = b->len, ch = b->stereo ? 2 : 1;
    if (!n || n > (64u << 20)) return;
    uint32_t bytes = b->fmt == FMT_ADPCM ? (n + 13) / 14 * 8 : n * ch * (b->fmt == FMT_PCM16 ? 2 : 1);
    const uint8_t *d = phys_ptr(b->addr & ~3u, bytes);
    if (!d) { LOG(1, "dsp: voice buffer at physical %08x (%u samples) is not mapped", b->addr, n); return; }
    int16_t *o = malloc((size_t)n * 4);
    if (!o) return;
    if (b->fmt == FMT_PCM16) {
        for (uint32_t i = 0; i < n; i++) {
            int16_t l, r; memcpy(&l, d + i * ch * 2, 2); if (ch == 2) memcpy(&r, d + i * 4 + 2, 2); else r = l;
            o[2 * i] = l; o[2 * i + 1] = r;
        }
    } else if (b->fmt == FMT_PCM8) {
        for (uint32_t i = 0; i < n; i++) {
            int16_t l = (int16_t)((int8_t)d[i * ch] * 256), r = ch == 2 ? (int16_t)((int8_t)d[i * 2 + 1] * 256) : l;
            o[2 * i] = l; o[2 * i + 1] = r;
        }
    } else {                                                            /* DSP-ADPCM, mono: 8-byte frames of 14 samples */
        static const int nib[16] = { 0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1 };
        int32_t y1 = s->yn1, y2 = s->yn2;
        if (b->adpcm_dirty) { y1 = b->yn1; y2 = b->yn2; }
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *f = d + (i / 14) * 8;
            int hdr = f[0], scale = 1 << (hdr & 15), idx = (hdr >> 4) & 7, k = i % 14;
            int byte = f[1 + k / 2], q = (k & 1) ? (byte & 15) : (byte >> 4);
            int32_t x = ((nib[q] * scale) << 11) + 0x400 + s->coef[idx * 2] * y1 + s->coef[idx * 2 + 1] * y2;
            x >>= 11;
            if (x > 32767) x = 32767; if (x < -32768) x = -32768;
            y2 = y1; y1 = x;
            o[2 * i] = o[2 * i + 1] = (int16_t)x;
        }
        s->yn1 = (int16_t)y1; s->yn2 = (int16_t)y2;
    }
    s->pcm = o; s->pcm_len = n;
}

static void push(Voice *s, Buf b) { if (s->nq < 8) s->q[s->nq++] = b; else LOG(1, "dsp: voice queue full, buffer %u dropped", b.id); }

static int dequeue(Voice *s) {
    if (!s->nq) return 0;
    int best = 0;
    for (int i = 1; i < s->nq; i++) if (s->q[i].id < s->q[best].id) best = i;
    Buf b = s->q[best]; s->q[best] = s->q[--s->nq];
    decode(s, &b);
    s->have_cur = 1; s->cur_id = b.id; s->last_id = 0;
    s->pos = b.played ? 0 : b.start; s->frac = 0;
    s->buffer_update = b.from_queue && !b.played;
    if (b.loop) { b.played = 1; b.adpcm_dirty = 1; b.yn1 = 0; b.yn2 = 0; push(s, b); }
    return 1;
}

static void apply_config(int i, uint8_t *reg) {
    Voice *s = &v[i];
    uint8_t *c = reg + OFF_CONFIG + 192u * i;
    uint32_t dirty; memcpy(&dirty, c, 4);
    if (!dirty) return;
    if (dirty & (1u << 29)) { free(s->pcm); memset(s, 0, sizeof *s); }          /* reset */
    if (dirty & (1u << 4)) s->nq = 0;                                            /* partial reset: drop the queue */
    if (dirty & (1u << 16)) s->enabled = c[160] != 0;
    if (dirty & (1u << 28)) s->sync = rd16(c + 162);
    if (dirty & (1u << 18)) { memcpy(&s->rate, c + 52, 4); if (!(s->rate > 0)) s->rate = 1.0f; }
    if (dirty & (1u << 2)) memcpy(s->coef, reg + OFF_ADPCM + 32u * i, 32);
    for (int m = 0; m < 3; m++) if (dirty & (1u << (25 + m))) memcpy(s->gain[m], c + 4 + 16 * m, 16);
    uint16_t f1 = rd16(c + 180);
    if (dirty & ((1u << 0) | (1u << 30))) s->fmt = (f1 >> 2) & 3;
    if (dirty & ((1u << 1) | (1u << 30))) s->stereo = (f1 & 3) == 2;
    uint32_t start = (dirty & (1u << 21)) ? dspw(c + 164) : 0;
    if (dirty & (1u << 3)) LOG(1, "dsp: voice %d partial embedded-buffer update is not implemented", i);
    if (dirty & (1u << 30)) {                                                    /* embedded buffer */
        int32_t len = (int32_t)dspw(c + 176); uint16_t f2 = rd16(c + 188);
        if (len >= 0) push(s, (Buf){ dspw(c + 172), (uint32_t)len, start, rd16(c + 190), (uint8_t)((f2 >> 1) & 1), 0, 0,
                                     (uint8_t)(f2 & 1), (uint8_t)s->stereo, (uint8_t)s->fmt, (int16_t)rd16(c + 184), (int16_t)rd16(c + 186) });
    }
    if (dirty & (1u << 19)) {                                                    /* buffer queue */
        uint16_t bd = rd16(c + 74);
        for (int k = 0; k < 4; k++) if (bd & (1 << k)) {
            const uint8_t *b = c + 76 + 20 * k; int32_t len = (int32_t)dspw(b + 4);
            if (len >= 0) push(s, (Buf){ dspw(b), (uint32_t)len, 0, rd16(b + 16), b[15] != 0, 1, 0, b[14] != 0,
                                         (uint8_t)s->stereo, (uint8_t)s->fmt, (int16_t)rd16(b + 10), (int16_t)rd16(b + 12) });
        }
        memset(c + 74, 0, 2);
    }
    memset(c, 0, 4);
}

static void apply_dsp_config(uint8_t *reg) {
    uint8_t *c = reg + OFF_DSPCONF;
    uint32_t dirty; memcpy(&dirty, c, 4);
    if (!dirty) return;
    if (dirty & (1u << 16)) memcpy(&master_volume, c + 4, 4);
    if (dirty & (1u << 24)) memcpy(&aux_return[0], c + 8, 4);
    if (dirty & (1u << 25)) memcpy(&aux_return[1], c + 12, 4);
    memset(c, 0, 4);
}

/* Render one frame of a voice into out[FRAME][2]; returns 0 when nothing played. */
static int render(Voice *s, float out[FRAME][2]) {
    if (!s->enabled) return 0;
    if (!s->have_cur) {
        if (dequeue(s)) return 0;                     /* like the DSP: a fresh buffer starts on the next frame */
        s->enabled = 0; s->buffer_update = 1; s->last_id = s->cur_id; s->cur_id = 0; return 0;
    }
    double rate = s->rate > 0 ? s->rate : 1.0;
    for (int i = 0; i < FRAME; i++) {
        while (s->pos >= s->pcm_len) {
            s->have_cur = 0;
            if (!dequeue(s)) { for (; i < FRAME; i++) out[i][0] = out[i][1] = 0; return 1; }
        }
        const int16_t *p = s->pcm + 2 * s->pos;
        int nx = s->pos + 1 < s->pcm_len;
        float t = (float)s->frac;
        out[i][0] = p ? p[0] + (nx ? (p[2] - p[0]) * t : 0) : 0;
        out[i][1] = p ? p[1] + (nx ? (p[3] - p[1]) * t : 0) : 0;
        s->frac += rate;
        uint32_t adv = (uint32_t)s->frac; s->frac -= adv; s->pos += adv;
    }
    if (s->pos >= s->pcm_len) {                       /* finished exactly at the frame end */
        s->have_cur = 0;
        if (!dequeue(s)) { s->have_cur = 0; }
    }
    return 1;
}

void dsp_audio_frame(void) {
    for (int r = 0; r < 2; r++) apply_dsp_config(region(r));
    float mix[3][FRAME][4]; memset(mix, 0, sizeof mix);
    for (int i = 0; i < NSRC; i++) {
        Voice *s = &v[i];
        for (int r = 0; r < 2; r++) apply_config(i, region(r));
        float out[FRAME][2];
        if (render(s, out))
            for (int m = 0; m < 3; m++) {
                const float *g = s->gain[m];
                if (!g[0] && !g[1] && !g[2] && !g[3]) continue;
                for (int k = 0; k < FRAME; k++) {
                    mix[m][k][0] += g[0] * out[k][0]; mix[m][k][1] += g[1] * out[k][1];
                    mix[m][k][2] += g[2] * out[k][0]; mix[m][k][3] += g[3] * out[k][1];
                }
            }
        uint8_t st[12]; memset(st, 0, sizeof st);
        st[0] = (uint8_t)s->enabled; st[1] = (uint8_t)s->buffer_update;
        memcpy(st + 2, &s->sync, 2);
        uint32_t pos = (s->pos << 16) | (s->pos >> 16); memcpy(st + 4, &pos, 4);
        memcpy(st + 8, &s->cur_id, 2); memcpy(st + 10, &s->last_id, 2);
        s->buffer_update = 0;
        for (int r = 0; r < 2; r++) memcpy(region(r) + OFF_STATUS + 12u * i, st, 12);
    }
    int16_t final[FRAME][2];
    const float vol[3] = { master_volume, aux_return[0], aux_return[1] };
    for (int k = 0; k < FRAME; k++)
        for (int c = 0; c < 2; c++) {
            float x = 0;
            for (int m = 0; m < 3; m++) x += vol[m] * (mix[m][k][c] + mix[m][k][c + 2]);   /* quad down to stereo */
            long q = lrintf(x);
            final[k][c] = (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
        }
    for (int r = 0; r < 2; r++) memcpy(region(r) + OFF_FINAL, final, sizeof final);
    audio_push(&final[0][0], FRAME);
}
