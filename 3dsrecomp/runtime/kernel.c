/* HLE kernel: address space, guest threads (cooperative coroutines on one host thread),
 * synchronisation objects and the supervisor calls the game uses. */
#define _GNU_SOURCE
#include "host.h"
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>

enum { T_READY, T_RUNNING, T_WAITING, T_DEAD };

KThread *cur;
int log_level = 1;
static KThread *threads[MAX_THREADS];
static int nthreads;
static uint64_t seq_counter;
static ucontext_t sched_ctx;
static KObject *handles[2048];
static KObject process_obj = { .type = K_PROCESS, .refs = 1 };
static uint32_t mem_used;

void fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "\nFATAL: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    if (cur) {
        CPU *c = &cur->cpu;
        fprintf(stderr, "thread %d  lr=%08x sp=%08x last-return=%08x\n", cur->id, c->r[14], c->r[13], c->r[15]);
        for (int i = 0; i < 13; i++) fprintf(stderr, "r%d=%08x%s", i, c->r[i], i % 4 == 3 ? "\n" : " ");
        fputc('\n', stderr);
    }
    exit(1);
}
uint32_t recomp_site;
void recomp_continue(CPU *c, uint32_t expected) {
    uint32_t t = c->r[15];
    recomp_fn f = recomp_lookup(t);
    if (!f) recomp_trap(c, expected, t, "callee returned to an unexpected address");
    f(c);                                                  /* the rest of the caller, from t on */
}
void recomp_trap(CPU *c, uint32_t addr, uint32_t info, const char *why) {
    (void)c;
    fprintf(stderr, "last computed return/jump was the instruction at %08x\n", recomp_site);
    if (!strncmp(why, "branch to unknown code", 22)) note_missing_function(addr);
    /* a computed jump to code that is not a known function looks like a return to the wrong place */
    if (!strncmp(why, "callee returned to an unexpected address", 40) && info - RECOMP_TEXT_BASE < RECOMP_TEXT_USED && !recomp_lookup(info))
        note_missing_function(info);
    fatal("recompiled code trap: %s (addr %08x, info %08x)", why, addr, info);
}
const char *host_game_dir = ".";
/* Code the static analysis missed: record it so a later rebuild can include it. */
void note_missing_function(uint32_t addr) {
    char path[600];
    snprintf(path, sizeof path, "%s/missing_functions.txt", host_game_dir);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%08x # found at run time\n", addr); fclose(f); }
}
void call_indirect(CPU *c, uint32_t addr) {
    recomp_fn f = recomp_lookup(addr);
    if (!f) {
        note_missing_function(addr);
        fatal("indirect call to %08x, which is not a known function (recorded in %s/missing_functions.txt)", addr, host_game_dir);
    }
    f(c);
}

/* ------------------------------------------------------------------ memory */
typedef struct { uint32_t base, size; int state, perm; const char *what; } Region;
static Region regions[512];
static int nregions;
uint32_t linear_base = 0x30000000u, app_mem_size = 0x4000000u;
int is_new_3ds;
static uint32_t linear_next, shared_next = SHARED_BASE;

void *mem_map(uint32_t addr, uint32_t size, const char *what, int state, int perm) {
    size = (size + 0xfff) & ~0xfffu;
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)addr) fatal("cannot map guest memory %08x+%x (%s)", addr, size, what);
    if (nregions == 512) fatal("too many memory regions");
    regions[nregions++] = (Region){ addr, size, state, perm, what };
    LOG(2, "map %08x-%08x %s", addr, addr + size, what);
    return p;
}
uint32_t linear_alloc(uint32_t size) {
    if (!linear_next) linear_next = LINEAR_BASE;
    uint32_t a = linear_next; size = (size + 0xfff) & ~0xfffu;
    mem_map(a, size, "linear heap", 7, 3); linear_next += size; mem_used += size;
    return a;
}
uint32_t shared_alloc(uint32_t size) {
    uint32_t a = shared_next; shared_next += (size + 0xfff) & ~0xfffu;
    return a;
}
int mem_valid(uint32_t addr, uint32_t size) {
    for (int i = 0; i < nregions; i++)
        if (addr - regions[i].base < regions[i].size) return size <= regions[i].size - (addr - regions[i].base);
    return 0;
}
/* Write every mapped guest region to <dir>/<base>.bin (for comparing memory with a reference). */
void mem_dump_all(const char *dir) {
    char path[600];
    mkdir(dir, 0755);
    for (int i = 0; i < nregions; i++) {
        snprintf(path, sizeof path, "%s/%08x.bin", dir, regions[i].base);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite((void *)(uintptr_t)regions[i].base, 1, regions[i].size, f); fclose(f); }
    }
    fprintf(stderr, "guest memory written to %s (%d regions)\n", dir, nregions);
}
static Region *region_find(uint32_t addr) {
    for (int i = 0; i < nregions; i++)
        if (addr - regions[i].base < regions[i].size) return &regions[i];
    return 0;
}

/* ------------------------------------------------------------------ objects */
/* Guest time is virtual: it only moves forward when every guest thread is waiting (then it jumps to
 * the next event), plus a tiny step on each clock read so polling loops progress. The game therefore
 * sees a steady 60 fps however long the host takes per frame, and runs are reproducible.
 * Wall-clock pacing keeps it from running faster than real time (RB_FAST=1 turns pacing off). */
static uint64_t vclock = 1000000000ull, pace_wall0, pace_v0;
static int pacing = -1, fast_forward;
void k_set_fast_forward(int on) { fast_forward = on; }
uint64_t k_paced_ns;                                  /* total time slept for pacing (FPS overlay) */   /* host hotkey: run as fast as the host can */
static uint64_t wall_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t now_ns(void) { return vclock; }
static void advance_to(uint64_t t) {
    if (t <= vclock) return;
    vclock = t;
    if (pacing < 0) { pacing = getenv("RB_FAST") == 0; pace_wall0 = wall_ns(); pace_v0 = vclock; }
    if (!pacing || fast_forward) return;
    uint64_t target = pace_wall0 + (vclock - pace_v0), w = wall_ns();
    if (target > w + 50000000ull) { pace_wall0 = w - (vclock - pace_v0); return; }   /* paused or debugging: resync */
    if (target > w) { struct timespec ts = { (time_t)((target - w) / 1000000000ull), (long)((target - w) % 1000000000ull) }; nanosleep(&ts, 0); k_paced_ns += wall_ns() - w; }
    else if (w > target + 34000000ull) pace_wall0 = w - (vclock - pace_v0);          /* behind by more than 2 frames: resync instead of
                                                                                        running fast to catch up (audio would be cut) */
}
KObject *k_new(KType t) { KObject *o = calloc(1, sizeof *o); o->type = t; return o; }
uint32_t h_new(KObject *o) {
    for (uint32_t i = 1; i < 2048; i++)
        if (!handles[i]) { handles[i] = o; o->refs++; return 0x00010000u | (i << 2); }
    fatal("out of handles");
}
KObject *h_get(uint32_t h) {
    if (h == 0xffff8000u) return cur->obj;
    if (h == 0xffff8001u) return &process_obj;
    uint32_t i = (h & 0xffffu) >> 2;
    return ((h >> 16) == 1 && i < 2048) ? handles[i] : 0;
}
KObject *k_event(int reset) { KObject *o = k_new(K_EVENT); o->reset = reset; return o; }
KObject *k_mutex(void) { return k_new(K_MUTEX); }
KObject *k_semaphore(int initial, int max) { KObject *o = k_new(K_SEMAPHORE); o->count = initial; o->max = max; return o; }
KObject *k_shmem(uint32_t size) {
    KObject *o = k_new(K_SHMEM);
    o->size = (size + 0xfff) & ~0xfffu; o->mem = calloc(1, o->size);
    return o;
}
KObject *k_session(const char *name) {
    KObject *o = k_new(K_SESSION);
    snprintf(o->name, sizeof o->name, "%s", name);
    o->handler = service_lookup(name);
    return o;
}

static int obj_ready(KObject *o, KThread *t) {
    switch (o->type) {
    case K_EVENT: case K_TIMER: return o->signaled;
    case K_MUTEX: return !o->owner || o->owner == t;
    case K_SEMAPHORE: return o->count > 0;
    case K_THREAD: return o->thread->state == T_DEAD;
    default: return 1;
    }
}
static void obj_acquire(KObject *o, KThread *t) {
    switch (o->type) {
    case K_EVENT: case K_TIMER: if (o->reset != 1) o->signaled = 0; break;
    case K_MUTEX: o->owner = t; o->lock_count++; break;
    case K_SEMAPHORE: o->count--; break;
    default: break;
    }
}
/* Try to complete thread t's pending wait. */
static int wait_try(KThread *t) {
    if (t->wait_all) {
        for (int i = 0; i < t->nwait; i++) if (!obj_ready(t->wait[i], t)) return 0;
        for (int i = 0; i < t->nwait; i++) obj_acquire(t->wait[i], t);
        t->wait_index = 0; return 1;
    }
    for (int i = 0; i < t->nwait; i++)
        if (obj_ready(t->wait[i], t)) { obj_acquire(t->wait[i], t); t->wait_index = i; return 1; }
    return 0;
}
static void wake(KThread *t, uint32_t result) {
    t->state = T_READY; t->nwait = 0; t->arb = 0; t->wake_ns = 0; t->wait_result = result; t->seq = ++seq_counter;
}
/* Wake every waiting thread whose objects became available, highest priority first. */
static void wake_waiters(void) {
    for (int prio = 0; prio < 64; prio++)
        for (int i = 0; i < nthreads; i++) {
            KThread *t = threads[i];
            if (t->prio == prio && t->state == T_WAITING && t->nwait && wait_try(t)) wake(t, RES_OK);
        }
}
void k_signal(KObject *e) {
    e->signaled = 1;
    wake_waiters();
    if (e->reset == 2) e->signaled = 0;
}

/* ------------------------------------------------------------------ scheduler */
static struct { void (*fn)(void); uint64_t period, next; } periodic[8];
static int nperiodic;
void k_add_periodic(void (*fn)(void), uint64_t period_ns) {
    periodic[nperiodic].fn = fn; periodic[nperiodic].period = period_ns; periodic[nperiodic].next = now_ns() + period_ns;
    nperiodic++;
}
static KObject *timers[64];
static int ntimers;

static void to_scheduler(void) { swapcontext((ucontext_t *)cur->ctx, &sched_ctx); }
/* Let a higher-priority thread that just became ready run. */
static void resched(void) {
    for (int i = 0; i < nthreads; i++)
        if (threads[i]->state == T_READY && threads[i]->prio < cur->prio) { cur->state = T_READY; to_scheduler(); return; }
}
static void thread_main(void) {
    KThread *t = cur;
    recomp_fn f = recomp_lookup(t->cpu.r[15]);
    if (!f) fatal("thread entry %08x is not a known function", t->cpu.r[15]);
    f(&t->cpu);
    LOG(1, "thread returned from its entry function");
    t->state = T_DEAD; wake_waiters(); to_scheduler();
    abort();
}
KThread *k_thread_create(uint32_t entry, uint32_t arg, uint32_t sp, int prio) {
    if (nthreads == MAX_THREADS) fatal("too many threads");
    KThread *t = calloc(1, sizeof *t);
    t->id = nthreads; t->prio = prio; t->state = T_READY; t->seq = ++seq_counter;
    t->obj = k_new(K_THREAD); t->obj->thread = t; t->obj->refs = 1;
    t->cpu.r[0] = arg; t->cpu.r[13] = sp; t->cpu.r[14] = 0xffff0ff0u; t->cpu.r[15] = entry;
    t->cpu.tls = TLS_BASE + (uint32_t)t->id * TLS_SIZE;
    memset((void *)(uintptr_t)t->cpu.tls, 0, TLS_SIZE);
    size_t hs = 4u << 20;
    t->hstack = mmap(0, hs, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    ucontext_t *uc = calloc(1, sizeof *uc);
    getcontext(uc);
    uc->uc_stack.ss_sp = t->hstack; uc->uc_stack.ss_size = hs; uc->uc_link = 0;
    makecontext(uc, thread_main, 0);
    t->ctx = uc;
    threads[nthreads++] = t;
    return t;
}
void k_run(void) {
    for (;;) {
        uint64_t now = now_ns(), next = ~0ull;
        for (int i = 0; i < nperiodic; i++) {
            if (now >= periodic[i].next) { periodic[i].next += periodic[i].period; if (periodic[i].next < now) periodic[i].next = now + periodic[i].period; periodic[i].fn(); }
            if (periodic[i].next < next) next = periodic[i].next;
        }
        for (int i = 0; i < ntimers; i++) {
            KObject *tm = timers[i];
            if (tm->fire_ns && now >= tm->fire_ns) { tm->fire_ns = tm->interval_ns ? now + tm->interval_ns : 0; k_signal(tm); }
            if (tm->fire_ns && tm->fire_ns < next) next = tm->fire_ns;
        }
        KThread *best = 0; int alive = 0, timed = 0;
        for (int i = 0; i < nthreads; i++) {
            KThread *t = threads[i];
            if (t->state == T_DEAD) continue;
            alive++;
            if (t->state == T_WAITING && t->wake_ns) {
                if (now >= t->wake_ns) wake(t, RES_TIMEOUT);
                else { timed++; if (t->wake_ns < next) next = t->wake_ns; }
            }
            if (t->state == T_READY && (!best || t->prio < best->prio || (t->prio == best->prio && t->seq < best->seq))) best = t;
        }
        if (!alive) { fprintf(stderr, "all guest threads exited\n"); exit(0); }
        if (!best) {
            if (!timed && !nperiodic && !ntimers) {
                fprintf(stderr, "\nDEADLOCK: every guest thread is waiting and nothing can wake them\n");
                for (int i = 0; i < nthreads; i++)
                    fprintf(stderr, "  thread %d state %d prio %d waits on %d objects (type %d) lr=%08x\n", threads[i]->id, threads[i]->state,
                            threads[i]->prio, threads[i]->nwait, threads[i]->nwait ? (int)threads[i]->wait[0]->type : 0, threads[i]->cpu.r[14]);
                exit(2);
            }
            advance_to(next);
            continue;
        }
        cur = best; best->state = T_RUNNING;
        vfp_set_fpscr(&best->cpu, best->cpu.fpscr);
        swapcontext(&sched_ctx, (ucontext_t *)best->ctx);
        cur = 0;
    }
}
/* Block the current thread on its wait list. Returns the wait result. */
static uint32_t block(int64_t timeout_ns) {
    cur->state = T_WAITING;
    cur->wake_ns = timeout_ns < 0 ? 0 : now_ns() + (uint64_t)timeout_ns;
    to_scheduler();
    return cur->wait_result;
}

/* ------------------------------------------------------------------ supervisor calls */
static const char *svc_name(uint32_t n) {
    static const char *names[0x40] = { [1]="ControlMemory",[2]="QueryMemory",[3]="ExitProcess",[8]="CreateThread",[9]="ExitThread",
        [0xa]="SleepThread",[0xb]="GetThreadPriority",[0xc]="SetThreadPriority",[0x13]="CreateMutex",[0x14]="ReleaseMutex",
        [0x15]="CreateSemaphore",[0x16]="ReleaseSemaphore",[0x17]="CreateEvent",[0x18]="SignalEvent",[0x19]="ClearEvent",
        [0x1a]="CreateTimer",[0x1b]="SetTimer",[0x1c]="CancelTimer",[0x1d]="ClearTimer",[0x1e]="CreateMemoryBlock",
        [0x1f]="MapMemoryBlock",[0x20]="UnmapMemoryBlock",[0x21]="CreateAddressArbiter",[0x22]="ArbitrateAddress",[0x23]="CloseHandle",
        [0x24]="WaitSynchronization1",[0x25]="WaitSynchronizationN",[0x27]="DuplicateHandle",[0x28]="GetSystemTick",[0x29]="GetHandleInfo",
        [0x2a]="GetSystemInfo",[0x2b]="GetProcessInfo",[0x2d]="ConnectToPort",[0x32]="SendSyncRequest",[0x35]="GetProcessId",
        [0x37]="GetThreadId",[0x38]="GetResourceLimit",[0x3c]="Break",[0x3d]="OutputDebugString" };
    return n < 0x40 && names[n] ? names[n] : "?";
}

void svc_call(CPU *c, uint32_t num, uint32_t addr) {
    uint32_t *r = c->r;
    LOG(3, "svc %02x %s (%08x %08x %08x %08x) from %08x lr %08x", num, svc_name(num), r[0], r[1], r[2], r[3], addr, r[14]);
    switch (num) {
    case 0x01: {                                           /* ControlMemory */
        uint32_t op = r[0], a0 = r[1], a1 = r[2], size = r[3];
        switch (op & 0xff) {
        case 3:
            if (op & 0x10000) r[1] = linear_alloc(size);
            else { mem_map(a0, size, "heap", 5, 3); mem_used += size; r[1] = a0; }
            break;
        case 1: r[1] = a0; break;                          /* free: keep the mapping, nothing reuses it yet */
        case 6: r[1] = a0; break;                          /* protect */
        default: fatal("ControlMemory operation %x (addr0 %08x addr1 %08x size %x) is not implemented", op, a0, a1, size);
        }
        LOG(2, "ControlMemory op %x size %x -> %08x", op, size, r[1]);
        r[0] = RES_OK; break;
    }
    case 0x02: {                                           /* QueryMemory */
        uint32_t a = r[2]; Region *g = region_find(a);
        if (g) { r[1] = g->base; r[2] = g->size; r[3] = (uint32_t)g->perm; r[4] = (uint32_t)g->state; }
        else {                                             /* free span between mapped regions */
            uint32_t lo = 0, hi = 0x40000000u;
            for (int i = 0; i < nregions; i++) {
                uint32_t e = regions[i].base + regions[i].size;
                if (e <= a && e > lo) lo = e;
                if (regions[i].base > a && regions[i].base < hi) hi = regions[i].base;
            }
            r[1] = lo; r[2] = hi - lo; r[3] = 0; r[4] = 0;
        }
        r[5] = 0; r[0] = RES_OK; break;
    }
    case 0x03: fprintf(stderr, "guest called ExitProcess\n"); exit(0);
    case 0x08: {                                           /* CreateThread */
        KThread *t = k_thread_create(r[1], r[2], r[3], (int)r[0]);
        LOG(1, "CreateThread entry %08x arg %08x stack %08x prio %d -> thread %d", r[1], r[2], r[3], r[0], t->id);
        r[1] = h_new(t->obj); r[0] = RES_OK; resched(); break;
    }
    case 0x09:                                             /* ExitThread */
        LOG(1, "ExitThread");
        for (int i = 1; i < 2048; i++)
            if (handles[i] && handles[i]->type == K_MUTEX && handles[i]->owner == cur) { handles[i]->owner = 0; handles[i]->lock_count = 0; }
        cur->state = T_DEAD; wake_waiters(); to_scheduler(); abort();
    case 0x0a: {                                           /* SleepThread */
        int64_t ns = (int64_t)(((uint64_t)r[1] << 32) | r[0]);
        if (ns <= 0) { vclock += 1000; cur->state = T_READY; cur->seq = ++seq_counter; to_scheduler(); }
        else { cur->nwait = 0; block(ns); }
        r[0] = RES_OK; break;
    }
    case 0x0b: { KObject *o = h_get(r[1]); r[1] = o && o->type == K_THREAD ? (uint32_t)o->thread->prio : 0x30; r[0] = RES_OK; break; }
    case 0x0c: { KObject *o = h_get(r[0]); if (o && o->type == K_THREAD) o->thread->prio = (int)r[1]; r[0] = RES_OK; resched(); break; }
    case 0x13: { KObject *o = k_mutex(); if (r[1]) { o->owner = cur; o->lock_count = 1; } r[1] = h_new(o); r[0] = RES_OK; break; }
    case 0x14: {                                           /* ReleaseMutex */
        KObject *o = h_get(r[0]);
        if (o && o->type == K_MUTEX && o->owner == cur && --o->lock_count == 0) { o->owner = 0; wake_waiters(); }
        r[0] = RES_OK; resched(); break;
    }
    case 0x15: r[1] = h_new(k_semaphore((int)r[1], (int)r[2])); r[0] = RES_OK; break;
    case 0x16: {                                           /* ReleaseSemaphore */
        KObject *o = h_get(r[1]); uint32_t prev = 0;
        if (o && o->type == K_SEMAPHORE) { prev = (uint32_t)o->count; o->count += (int)r[2]; wake_waiters(); }
        r[1] = prev; r[0] = RES_OK; resched(); break;
    }
    case 0x17: r[1] = h_new(k_event((int)r[1])); r[0] = RES_OK; break;
    case 0x18: { KObject *o = h_get(r[0]); if (o) k_signal(o); r[0] = RES_OK; resched(); break; }
    case 0x19: case 0x1d: { KObject *o = h_get(r[0]); if (o) o->signaled = 0; r[0] = RES_OK; break; }
    case 0x1a: { KObject *o = k_new(K_TIMER); o->reset = (int)r[1]; if (ntimers < 64) timers[ntimers++] = o; r[1] = h_new(o); r[0] = RES_OK; break; }
    case 0x1b: {                                           /* SetTimer: initial r2:r3, interval r1:r4 */
        KObject *o = h_get(r[0]);
        if (o) { o->fire_ns = now_ns() + (((uint64_t)r[3] << 32) | r[2]); o->interval_ns = ((uint64_t)r[4] << 32) | r[1]; if (!o->fire_ns) o->fire_ns = 1; }
        r[0] = RES_OK; break;
    }
    case 0x1c: { KObject *o = h_get(r[0]); if (o) o->fire_ns = 0; r[0] = RES_OK; break; }
    case 0x1e: {                                           /* CreateMemoryBlock: app-owned memory shared with a service */
        KObject *o = k_new(K_SHMEM); o->addr = r[1]; o->size = r[2]; o->mem = (uint8_t *)(uintptr_t)r[1];
        r[1] = h_new(o); r[0] = RES_OK; break;
    }
    case 0x1f: {                                           /* MapMemoryBlock */
        KObject *o = h_get(r[0]);
        if (!o || o->type != K_SHMEM) fatal("MapMemoryBlock on handle %08x, which is not shared memory", r[0]);
        uint32_t a = r[1] ? r[1] : o->prefer ? o->prefer : shared_alloc(o->size);
        if (o->addr == a) { r[0] = RES_OK; break; }               /* already mapped there */
        uint8_t *p = mem_map(a, o->size, "shared memory", 6, 3);
        memcpy(p, o->mem, o->size);
        if (!o->addr) free(o->mem);
        o->mem = p; o->addr = a;
        LOG(2, "MapMemoryBlock %08x -> %08x size %x", r[0], a, o->size);
        r[0] = RES_OK; break;
    }
    case 0x20: r[0] = RES_OK; break;
    case 0x21: r[1] = h_new(k_new(K_ARBITER)); r[0] = RES_OK; break;
    case 0x22: {                                           /* ArbitrateAddress */
        KObject *o = h_get(r[0]); uint32_t a = r[1], type = r[2]; int32_t value = (int32_t)r[3];
        int64_t timeout = (int64_t)(((uint64_t)r[5] << 32) | r[4]);
        if (type == 0) {                                   /* signal: wake up to `value` waiters (negative: all) */
            int n = value;
            for (int prio = 0; prio < 64 && n != 0; prio++)
                for (int i = 0; i < nthreads && n != 0; i++) {
                    KThread *t = threads[i];
                    if (t->prio == prio && t->state == T_WAITING && t->arb == o && t->arb_addr == a) { wake(t, RES_OK); if (n > 0) n--; }
                }
            r[0] = RES_OK; resched(); break;
        }
        int32_t m = (int32_t)ld32(a);
        if (m < value) {
            if (type == 2 || type == 4) st32(a, (uint32_t)(m - 1));
            cur->nwait = 0; cur->arb = o; cur->arb_addr = a;
            r[0] = block((type == 3 || type == 4) ? timeout : -1);
        } else r[0] = RES_OK;
        break;
    }
    case 0x23: {                                           /* CloseHandle */
        uint32_t i = (r[0] & 0xffffu) >> 2;
        if ((r[0] >> 16) == 1 && i < 2048 && handles[i]) { handles[i]->refs--; handles[i] = 0; }
        r[0] = RES_OK; break;
    }
    case 0x24: {                                           /* WaitSynchronization1 */
        KObject *o = h_get(r[0]); int64_t timeout = (int64_t)(((uint64_t)r[3] << 32) | r[2]);
        if (!o) { LOG(1, "WaitSynchronization1 on bad handle %08x", r[0]); r[0] = 0xd8e007f7u; break; }
        cur->wait[0] = o; cur->nwait = 1; cur->wait_all = 0;
        if (wait_try(cur)) { cur->nwait = 0; r[0] = RES_OK; }
        else if (timeout == 0) { cur->nwait = 0; r[0] = RES_TIMEOUT; }
        else r[0] = block(timeout);
        break;
    }
    case 0x25: {                                           /* WaitSynchronizationN */
        uint32_t hp = r[1], n = r[2]; int64_t timeout = (int64_t)(((uint64_t)r[4] << 32) | r[0]);
        if (n > 32) fatal("WaitSynchronizationN with %u handles", n);
        cur->nwait = (int)n; cur->wait_all = (int)r[3];
        for (uint32_t i = 0; i < n; i++) {
            cur->wait[i] = h_get(ld32(hp + 4 * i));
            if (!cur->wait[i]) fatal("WaitSynchronizationN on bad handle %08x", ld32(hp + 4 * i));
        }
        if (n == 0) { cur->nwait = 0; if (timeout) block(timeout); r[0] = timeout ? RES_TIMEOUT : RES_OK; r[1] = 0; break; }
        if (wait_try(cur)) { cur->nwait = 0; r[0] = RES_OK; }
        else if (timeout == 0) { cur->nwait = 0; r[0] = RES_TIMEOUT; cur->wait_index = -1; }
        else { r[0] = block(timeout); if (r[0] != RES_OK) cur->wait_index = -1; }
        r[1] = (uint32_t)cur->wait_index;
        break;
    }
    case 0x27: { KObject *o = h_get(r[1]); if (!o) fatal("DuplicateHandle on bad handle %08x", r[1]); r[1] = h_new(o); r[0] = RES_OK; break; }
    case 0x28: {                                           /* GetSystemTick: 268111856 Hz */
        vclock += 2000;                                    /* polling loops must see time move */
        unsigned __int128 t = (unsigned __int128)now_ns() * 268111856u / 1000000000u;
        r[0] = (uint32_t)t; r[1] = (uint32_t)(t >> 32); break;
    }
    case 0x29: r[1] = 0; r[2] = 0; r[0] = RES_OK; break;   /* GetHandleInfo */
    case 0x2a: r[1] = mem_used; r[2] = 0; r[0] = RES_OK; break;
    case 0x2b: {                                           /* GetProcessInfo */
        uint32_t type = r[2]; uint64_t v = 0;
        if (type == 0 || type == 2) v = mem_used;
        else if (type == 20) v = 0x20000000u - LINEAR_BASE;
        else LOG(1, "GetProcessInfo type %u is not implemented, returning 0", type);
        r[1] = (uint32_t)v; r[2] = (uint32_t)(v >> 32); r[0] = RES_OK; break;
    }
    case 0x2d: {                                           /* ConnectToPort */
        const char *name = (const char *)(uintptr_t)r[1];
        LOG(1, "ConnectToPort \"%s\"", name);
        r[1] = h_new(k_session(name)); r[0] = RES_OK; break;
    }
    case 0x32: {                                           /* SendSyncRequest */
        KObject *o = h_get(r[0]);
        if (!o || o->type != K_SESSION) fatal("SendSyncRequest on handle %08x, which is not a session", r[0]);
        uint32_t *cb = cmdbuf(), q[6];
        memcpy(q, cb, sizeof q);
        o->handler(o, cb);
        LOG(2, "ipc %-8s %08x(%x, %x, %x, %x, %x) -> %08x %x %x %x", o->name, q[0], q[1], q[2], q[3], q[4], q[5], cb[1], cb[2], cb[3], cb[4]);
        r[0] = RES_OK; resched(); break;
    }
    case 0x35: r[1] = 10; r[0] = RES_OK; break;
    /* Process/thread affinity and processor queries: one application core, ideal processor 0. */
    case 0x04: case 0x06: case 0x0f: r[1] = num == 0x04 ? 1 : 0; r[0] = RES_OK; break;   /* Get*AffinityMask, Get*IdealProcessor */
    case 0x05: case 0x07: case 0x0e: case 0x10: r[0] = RES_OK; break;                      /* Set* versions: accepted */
    case 0x0d: { uint8_t one = 1; if (r[0] && r[2] >= 1) memcpy((void *)(uintptr_t)r[0], &one, 1); r[0] = RES_OK; break; }   /* GetThreadAffinityMask */
    case 0x11: r[0] = 0; break;                                                             /* GetCurrentProcessorNumber */
    case 0x34: { KObject *o = 0; for (int i = 0; i < nthreads; i++) if ((uint32_t)threads[i]->id == r[2]) o = threads[i]->obj;   /* OpenThread */
                 r[0] = o ? RES_OK : 0xd8e007f7u; r[1] = o ? h_new(o) : 0; break; }
    case 0x36: r[1] = 10; r[0] = RES_OK; break;                                             /* GetProcessIdOfThread */
    /* Resource limits (values of the application category, per Azahar): a handle stands for the process's limit. */
    case 0x38: { static KObject *rl; if (!rl) rl = k_new(K_PROCESS); r[1] = h_new(rl); r[0] = RES_OK; break; }   /* GetResourceLimit */
    case 0x39: case 0x3a: {                                /* GetResourceLimitLimitValues / CurrentValues(values, limit, names, count) */
        uint32_t out = r[0], names = r[2], count = r[3];
        for (uint32_t i = 0; i < count; i++) {
            uint32_t name = ld32(names + 4 * i); int64_t v = 0;
            if (num == 0x39) {
                const int64_t lim[10] = { 0x18, app_mem_size, 0x20, 0x20, 0x20, 0x08, 0x08, 0x10, 0x02, 0x1e };
                v = name < 10 ? lim[name] : 0;
            } else switch (name) {
                case 0: v = 0x18; break;                       /* priority */
                case 1: v = mem_used; break;                   /* committed memory */
                case 2: v = nthreads; break;                   /* threads */
                default: v = 0;                                /* other kernel objects are not counted */
            }
            memcpy((void *)(uintptr_t)(out + 8 * i), &v, 8);
        }
        r[0] = RES_OK; break;
    }
    case 0x37: { KObject *o = h_get(r[1]); r[1] = o && o->type == K_THREAD ? (uint32_t)o->thread->id : 0; r[0] = RES_OK; break; }
    case 0x3c: fatal("guest called Break (reason %u) from %08x", r[0], addr);
    case 0x3d: fprintf(stderr, "[guest] %.*s\n", (int)r[1], (const char *)(uintptr_t)r[0]); break;
    default: fatal("supervisor call %02x (%s) from %08x is not implemented", num, svc_name(num), addr);
    }
}
