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

void pica_shader_prepare(const PicaShader *sh);   /* decode the program when it changed (before running it) */
int pica_shader_run(const PicaShader *sh, uint32_t entry, const float in[16][4], float out[16][4]);

void pica_run_list(uint32_t vaddr, uint32_t size);   /* execute a command list (virtual address) */
void pica_report(void);
int pica_save_state(const char *path);
int pica_load_state(const char *path);
uint64_t pica_draw_count(void);
uint64_t gpu_frame(void);
void pica_counters(uint64_t out[9]);   /* profiler totals, see pica.c */
