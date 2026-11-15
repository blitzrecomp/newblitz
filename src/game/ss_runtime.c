// Save-state builds only (BLITZ_SAVESTATES): storage for the resumable code's shadow call stacks
// (tools/savestate/ss_runtime.h).
#include <stdio.h>
#include <stdlib.h>

#include "ss_runtime.h"

_Thread_local ss_stack ss_tls_stack;

// The calling thread's stack, for host code (savestate.cpp).
ss_stack* ss_current_stack(void) {
    return &ss_tls_stack;
}

void ss_resume_fail(void* ctx, uint32_t func_vram, uint32_t site) {
    (void)ctx;
    fprintf(stderr, "[savestate] resume failed: function %08X has no call site %u\n", func_vram, site);
    abort();
}
