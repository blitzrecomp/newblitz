// See psp_memcheck.h.
#include <atomic>
#include <cstdint>

#include "psp_log.h"
#include "psp_memcheck.h"

namespace {
    std::atomic<uint64_t> g_escapes{0};
}

extern "C" uint32_t blitz_psp_memcheck_escape(uint32_t addr, const char* func) {
    uint64_t n = ++g_escapes;
    if (n <= 50) {
        PERR("memcheck", "escape %llu: address %08X in %s (masked to %08X)", (unsigned long long)n, addr, func ? func : "?",
             0x80000000u | (addr & 0x007FFFFFu));
    } else if (n == 51) {
        PERR("memcheck", "more escapes: counting only");
    }
    return addr & 0x007FFFFFu;
}

extern "C" uint64_t blitz_psp_memcheck_escapes(void) {
    return g_escapes.load();
}
