// Shadow call stack for resumable recompiled functions (tools/savestate/resumable.py).
//
// Each game thread keeps the call-site ids of the instrumented calls it is inside. A snapshot
// copies this (plus the thread's recomp_context); a restore loads it into a new host thread with
// `resuming` set, and each instrumented function, on entry, jumps straight to the call it was in.
//
// Storage: thread-local by default. Define SS_STACK_OF(ctx) before including this header to keep
// the stack next to the recomp_context instead (e.g. a struct wrapping both, set up in
// run_thread_function), which avoids the TLS lookup on every instrumented call.
#ifndef SS_RUNTIME_H
#define SS_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SS_MAX_DEPTH
#define SS_MAX_DEPTH 64  // deepest observed chain to process_sleep is 18 frames
#endif

typedef struct {
    uint32_t site;    // resumable.py site id (1..SS_SITE_COUNT)
    uint32_t target;  // indirect calls: the guest address that was called; 0 otherwise
} ss_entry;

typedef struct {
    uint32_t depth;       // entries in use
    uint32_t resume_pos;  // next entry to consume while resuming
    uint32_t resuming;    // non-zero while a restored thread is walking its chain down
    uint32_t overflow;    // pushes past SS_MAX_DEPTH (the stack is then unusable for a snapshot)
    ss_entry e[SS_MAX_DEPTH];
} ss_stack;

#ifndef SS_STACK_OF
extern _Thread_local ss_stack ss_tls_stack;
#define SS_STACK_OF(ctx) (&ss_tls_stack)
#endif

// Called when a resuming function finds a site id it doesn't contain (snapshot from another
// build, or a corrupted stack). Must not return.
void ss_resume_fail(void* ctx, uint32_t func_vram, uint32_t site);

static inline void ss_push(ss_stack* s, uint32_t site, uint32_t target) {
    if (__builtin_expect(s->depth < SS_MAX_DEPTH, 1)) {
        s->e[s->depth].site = site;
        s->e[s->depth].target = target;
    } else {
        s->overflow++;
    }
    s->depth++;
}

static inline void ss_pop(ss_stack* s) {
    s->depth--;
}

static inline uint32_t ss_resume_site(ss_stack* s) {
    return s->resume_pos < s->depth ? s->e[s->resume_pos++].site : 0;
}

// Target of the entry just consumed by ss_resume_site.
static inline uint32_t ss_resume_target(ss_stack* s) {
    return s->e[s->resume_pos - 1].target;
}

// Site id -> the function containing it (generated ss_site_funcs.c).
extern void* const ss_site_func[];

// While resuming, an indirect call goes to the function the next entry was recorded in: overlays
// share addresses, and the one registered now need not be the one that made the call.
static inline void* ss_resume_callee(ss_stack* s) {
    return s->resume_pos < s->depth && s->resume_pos < SS_MAX_DEPTH ? ss_site_func[s->e[s->resume_pos].site] : 0;
}
#define SS_INDIRECT(ctx, tgt) \
    ((SS_RESUMING(ctx) && ss_resume_callee(SS_STACK_OF(ctx))) ? (recomp_func_t*)ss_resume_callee(SS_STACK_OF(ctx)) : LOOKUP_FUNC(tgt))

#define SS_RESUMING(ctx)       __builtin_expect(SS_STACK_OF(ctx)->resuming != 0, 0)
#define SS_RESUME_SITE(ctx)    ss_resume_site(SS_STACK_OF(ctx))
#define SS_RESUME_TARGET(ctx)  ss_resume_target(SS_STACK_OF(ctx))
#define SS_RESUME_DONE(ctx)    (SS_STACK_OF(ctx)->resuming = 0)
#define SS_RESUME_FAIL(ctx, fv) ss_resume_fail((void*)(ctx), (fv), SS_STACK_OF(ctx)->resume_pos ? SS_STACK_OF(ctx)->e[SS_STACK_OF(ctx)->resume_pos - 1].site : 0)
#define SS_PUSH(ctx, site)     ss_push(SS_STACK_OF(ctx), (site), 0)
#define SS_PUSH_T(ctx, site, t) ss_push(SS_STACK_OF(ctx), (site), (t))
#define SS_POP(ctx)            ss_pop(SS_STACK_OF(ctx))

#ifdef __cplusplus
}
#endif

#endif
