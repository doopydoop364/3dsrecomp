/* PICA200 GPU: command processing, vertex pipeline and software rasteriser. */
#pragma once
#include "host.h"

typedef struct {
    uint32_t code[512];        /* program words */
    uint32_t desc[128];        /* operand descriptors (swizzle patterns) */
    float uniform[96][4];      /* c0-c95, x y z w */
    uint32_t bools;            /* b0-b15 */
    uint32_t ints[4];          /* i0-i3: x y z w bytes */
} PicaShader;

/* Geometry shader output: SETEMIT sets the slot (0-2), whether the next EMIT completes a triangle and its winding;
 * EMIT hands the current output registers to emit(). */
typedef struct PicaEmitter {
    int vertex_id, prim_emit, winding;
    void (*emit)(struct PicaEmitter *em, const float out[16][4]);
    int own_bools; uint32_t bools;     /* own_bools: this invocation's b0-b15 instead of the unit's (b15, see pica.c) */
} PicaEmitter;

void pica_shader_prepare(const PicaShader *sh);   /* decode the program when it changed (before running it) */
/* The same, trusting a version number the caller bumps on every program or swizzle write (no comparison). */
void pica_shader_prepare_ver(const PicaShader *sh, uint64_t ver);
int pica_shader_run(const PicaShader *sh, uint32_t entry, const float in[16][4], float out[16][4]);
int pica_shader_run_gs(const PicaShader *sh, uint32_t entry, const float in[16][4], float out[16][4], PicaEmitter *em);

void pica_run_list(uint32_t vaddr, uint32_t size);   /* execute a command list (virtual address) */
void pica_report(void);
int pica_save_state(const char *path);
int pica_load_state(const char *path);
uint64_t pica_draw_count(void);
uint64_t gpu_frame(void);
void pica_counters(uint64_t out[9]);   /* profiler totals, see pica.c */
