// Self-test for resumable.py: run the transformed fixture on a host thread, snapshot at the k-th
// sleep (RDRAM + recomp_context + shadow stack), abandon that thread's C stack with longjmp (as if
// it were destroyed), scribble over RDRAM, then restore and resume the chain on a *new* host
// thread. The rest of the run must match an uninterrupted reference run exactly: the state seen
// at every later sleep, the number of sleeps, and the final RDRAM.
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp.h"
#include "funcs.h"
#include "ss_runtime.h"
#include "ss_sites.h"

_Thread_local ss_stack ss_tls_stack;

#define RDRAM_SIZE 0x400000
static uint8_t* g_rdram;

recomp_func_t* get_function(int32_t vram) {
    switch ((uint32_t)vram) {
        case 0x80100000: return proc_entry;
        case 0x80100100: return mid_fn;
        case 0x80100200: return leaf_a;
        case 0x80100300: return leaf_b;
        case 0x80100400: return tail_fn;
        case 0x80100500: return no_sleep;
        case 0x80100600: return process_sleep;
    }
    fprintf(stderr, "get_function: no function at %08X\n", (uint32_t)vram);
    exit(2);
}
void switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    fprintf(stderr, "switch_error %s %08X\n", func, vram);
    exit(2);
}
void ss_resume_fail(void* ctx, uint32_t func_vram, uint32_t site) {
    fprintf(stderr, "ss_resume_fail in %08X, site %u\n", func_vram, site);
    exit(3);
}

typedef struct { uint64_t ctxh, memh; } event;
static event ref_trace[64], cur_trace[64];
static int ref_n, cur_n, snap_at;
static uint64_t ref_final;
static jmp_buf abandon;
static struct { recomp_context ctx; ss_stack ss; uint8_t* mem; int n; } snap;

static uint64_t fnv(const void* p, size_t n) {
    const uint8_t* b = p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static uint64_t ctx_hash(const recomp_context* c) {
    return fnv(&c->r0, 32 * sizeof(gpr)) ^ (fnv(&c->f0, 32 * sizeof(fpr)) * 31);
}

void blitz_osStopThread(uint8_t* rdram, recomp_context* ctx) {
    int k = ++cur_n;
    cur_trace[k - 1] = (event){ctx_hash(ctx), fnv(rdram, RDRAM_SIZE)};
    if (k == snap_at) {
        snap.ctx = *ctx;
        snap.ss = ss_tls_stack;
        memcpy(snap.mem, rdram, RDRAM_SIZE);
        snap.n = k;
        longjmp(abandon, 1);  // this host stack is never returned into again
    }
}

// Chain continuity: entry i+1 must run in the function that site i calls (its target, for an
// indirect call). A host frame in between (a hook calling back into game code) breaks this.
static int chain_contiguous(const ss_stack* s) {
    for (uint32_t i = 0; i + 1 < s->depth; i++) {
        uint32_t callee = ss_site_vrams[s->e[i].site][2];
        if (callee == 0) callee = s->e[i].target;
        if (callee != ss_site_vrams[s->e[i + 1].site][0]) return 0;
    }
    return s->depth > 0 && !s->overflow;
}

static void* run_fresh(void* arg) {
    (void)arg;
    recomp_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.r29 = (gpr)(int32_t)0x80300000;
    ctx.f_odd = &ctx.f0.u32h;
    memset(&ss_tls_stack, 0, sizeof ss_tls_stack);
    if (!setjmp(abandon)) {
        proc_entry(g_rdram, &ctx);
        if (ss_tls_stack.depth != 0) { fprintf(stderr, "fresh run: depth %u at exit\n", ss_tls_stack.depth); exit(4); }
    }
    return NULL;
}

static void* run_resume(void* arg) {
    (void)arg;
    recomp_context ctx = snap.ctx;
    ctx.f_odd = &ctx.f0.u32h;  // the only host pointer in recomp_context
    ss_tls_stack = snap.ss;
    ss_tls_stack.resume_pos = 0;
    ss_tls_stack.resuming = 1;
    uint32_t entry = ss_site_vrams[ss_tls_stack.e[0].site][0];
    get_function((int32_t)entry)(g_rdram, &ctx);
    if (ss_tls_stack.depth != 0 || ss_tls_stack.resuming) { fprintf(stderr, "resume: depth %u resuming %u at exit\n", ss_tls_stack.depth, ss_tls_stack.resuming); exit(4); }
    return NULL;
}

static void on_thread(void* (*fn)(void*)) {
    pthread_t t;
    pthread_create(&t, NULL, fn, NULL);
    pthread_join(t, NULL);
}

int main(void) {
    g_rdram = calloc(1, RDRAM_SIZE);
    snap.mem = malloc(RDRAM_SIZE);

    snap_at = 0;
    cur_n = 0;
    on_thread(run_fresh);
    ref_n = cur_n;
    memcpy(ref_trace, cur_trace, sizeof ref_trace);
    ref_final = fnv(g_rdram, RDRAM_SIZE);
    printf("reference: %d sleeps, final rdram %016llx, marker %08X\n", ref_n, (unsigned long long)ref_final,
           *(uint32_t*)(g_rdram + 0x200200));

    int fails = 0;
    for (int k = 1; k <= ref_n; k++) {
        memset(g_rdram, 0, RDRAM_SIZE);
        memset(cur_trace, 0, sizeof cur_trace);
        cur_n = 0;
        snap_at = k;
        on_thread(run_fresh);  // runs to the k-th sleep, snapshots, abandons its stack

        memset(g_rdram, 0xAB, RDRAM_SIZE);  // whatever the game did after the snapshot
        memcpy(g_rdram, snap.mem, RDRAM_SIZE);
        cur_n = snap.n;
        snap_at = 0;
        on_thread(run_resume);  // new host thread walks the recorded chain back down

        int ok = cur_n == ref_n && fnv(g_rdram, RDRAM_SIZE) == ref_final &&
                 memcmp(&cur_trace[k], &ref_trace[k], sizeof(event) * (ref_n - k)) == 0;
        printf("snapshot at sleep %d: chain depth %u [", k, snap.ss.depth);
        for (uint32_t i = 0; i < snap.ss.depth; i++) {
            printf("%s%u%s", i ? " " : "", snap.ss.e[i].site, snap.ss.e[i].target ? "*" : "");
        }
        int contiguous = chain_contiguous(&snap.ss);
        printf("] %s -> %s\n", contiguous ? "contiguous" : "BROKEN CHAIN", ok ? "identical to reference" : "MISMATCH");
        fails += !ok || !contiguous;
    }
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
