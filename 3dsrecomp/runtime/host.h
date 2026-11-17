/* Native replacement for the 3DS kernel and system services (HLE). */
#pragma once
#include "recomp.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- guest address space ---- */
#define HEAP_BASE     0x08000000u
#define STACK_TOP     0x10000000u
#define SHARED_BASE   0x10000000u   /* kernel-chosen shared memory mappings */
#define VRAM_BASE     0x1f000000u
#define VRAM_SIZE     0x00600000u
#define CONFIG_PAGE   0x1ff80000u
#define SHARED_PAGE   0x1ff81000u
#define TLS_BASE      0x1ff82000u
#define TLS_SIZE      0x200u
/* Linear heap start (FCRAM physical 0x20000000), chosen from the kernel version in the ExHeader: titles for
 * kernels older than 2.44 use 0x14000000, newer ones 0x30000000 (as Azahar's Process::GetLinearHeapAreaAddress). */
extern uint32_t linear_base;
#define LINEAR_BASE   linear_base
#define MAX_THREADS   64

#define RES_OK        0u
#define RES_TIMEOUT   0x09401bfeu

typedef enum { K_THREAD = 1, K_MUTEX, K_SEMAPHORE, K_EVENT, K_TIMER, K_ARBITER, K_SHMEM, K_SESSION, K_PROCESS } KType;
typedef struct KThread KThread;
typedef struct KObject KObject;
typedef void (*ServiceFn)(KObject *session, uint32_t *cmd);

struct KObject {
    KType type;
    int refs;
    int signaled, reset;                 /* event, timer: reset 0 = one-shot, 1 = sticky, 2 = pulse */
    KThread *owner; int lock_count;      /* mutex */
    int count, max;                      /* semaphore */
    uint64_t fire_ns, interval_ns;       /* timer */
    uint32_t addr, size; uint8_t *mem;   /* shared memory: guest address once mapped, backing */
    uint32_t prefer;                     /* shared memory: address used when the guest maps it at 0 */
    char name[12]; ServiceFn handler;    /* session */
    KThread *thread;                     /* thread */
    void *user;
};

struct KThread {
    CPU cpu;
    KObject *obj;
    int id, state, prio;
    uint64_t seq;
    KObject *wait[32]; int nwait, wait_all;
    uint64_t wake_ns;
    uint32_t wait_result; int wait_index;
    KObject *arb; uint32_t arb_addr;
    void *hstack;
    void *ctx;
};

extern KThread *cur;
extern const char *host_game_dir;      /* folder with the extracted game, set by main */
void note_missing_function(uint32_t addr);
extern int log_level;
#define LOG(lvl, ...) do { if (log_level >= (lvl)) { fprintf(stderr, "[t%d] ", cur ? cur->id : -1); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* memory */
void *mem_map(uint32_t addr, uint32_t size, const char *what, int state, int perm);
int mem_valid(uint32_t addr, uint32_t size);
void mem_dump_all(const char *dir);   /* whole range inside one mapped region */
uint32_t linear_alloc(uint32_t size);
uint32_t shared_alloc(uint32_t size);

/* kernel objects */
uint64_t now_ns(void);
uint32_t h_new(KObject *o);
KObject *h_get(uint32_t h);
KObject *k_new(KType t);
KObject *k_event(int reset);
KObject *k_mutex(void);
KObject *k_semaphore(int initial, int max);
KObject *k_shmem(uint32_t size);
KObject *k_session(const char *name);
void k_signal(KObject *event);
void k_add_periodic(void (*fn)(void), uint64_t period_ns);
void k_set_fast_forward(int on);
KThread *k_thread_create(uint32_t entry, uint32_t arg, uint32_t sp, int prio);
void k_run(void) __attribute__((noreturn));
static inline uint32_t *cmdbuf(void) { return (uint32_t *)(uintptr_t)(cur->cpu.tls + 0x80); }
static inline uint32_t *staticbufs(void) { return (uint32_t *)(uintptr_t)(cur->cpu.tls + 0x180); }

/* services */
ServiceFn service_lookup(const char *name);
void services_init(const char *game_dir);
