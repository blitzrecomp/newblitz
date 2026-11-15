// libultra functions that N64Recomp treats as runtime-provided but this runtime version lacks.
#include <chrono>

#include "recomp.h"

// osViGetCurrentLine: N64Recomp lists it as reimplemented, N64ModernRuntime ca568b6 has no
// implementation. Blitz only uses it to seed srand (func_80249268), so return the half-line
// an NTSC VI would be on right now (525 half-lines per 1/60 s field pair), which still varies
// from run to run like the real register.
extern "C" void osViGetCurrentLine_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    using namespace std::chrono;
    uint64_t us = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    ctx->r2 = (gpr)((us % 16683) * 525 / 16683);
}

// COP0 Count for the 2026 flavour: team+'s attract-mode team pick (func_80249268) seeds rand from
// it with mfc0, which N64Recomp can't translate; tools/teamplus.py replaces each read with this.
// The N64 counter runs at half the CPU clock (46.875 MHz).
extern "C" uint64_t blitz_cop0_count(void) {
    using namespace std::chrono;
    uint64_t ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    return (uint64_t)(int64_t)(int32_t)(uint32_t)(ns * 3 / 64);
}
