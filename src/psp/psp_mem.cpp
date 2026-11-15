// The PSP's memory (docs/PSP_PORT.md §4.2): RDRAM is a static 8 MB block in .bss between two 64 KB guards, at the same
// address every run. RECOMP_SMALL_RDRAM's recomp::platform_rdram hands it to the runtime, and RECOMP_RDRAM_MASK keeps every
// guest address inside it. The guards stay zero unless host code (the runtime, a hook) writes past RDRAM: mem_stats checks
// them (the watchdog does every 10 s and logs [mem]).
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>

#include <pspkernel.h>
#include <pspsysmem.h>

#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "psp_log.h"
#include "host/host_backends.h"

namespace {
    constexpr size_t rdram_size = 8u << 20;
    constexpr size_t guard_size = 64u << 10;
    alignas(64) uint8_t g_block[guard_size + rdram_size + guard_size];
    bool g_given = false;

    // The first nonzero byte of a guard, or 0 if it is clean.
    uint32_t check_guard(const uint8_t* g) {
        const uint32_t* w = reinterpret_cast<const uint32_t*>(g);
        for (size_t i = 0; i < guard_size / 4; i++) {
            if (w[i] != 0) return (uint32_t)(i * 4) + 1;
        }
        return 0;
    }
}

uint8_t* recomp::platform_rdram(size_t size) {
    if (size != rdram_size) {
        PERR("mem", "RDRAM of %u bytes asked for; the PSP's block is 8 MB", (unsigned)size);
        return nullptr;
    }
    g_given = true;
    PLOG("mem", "RDRAM 8192 KB at %p (static, guards of 64 KB at %p and %p)", (void*)(g_block + guard_size), (void*)g_block,
         (void*)(g_block + guard_size + rdram_size));
    return g_block + guard_size;
}

// newlib's malloc (mallocr.c) keeps the most memory it has ever taken from sbrk: the heap's real high-water, including
// fragmentation (mallinfo().arena is the current amount, which goes down again when the top of the heap is trimmed).
extern "C" unsigned long __malloc_max_sbrked_mem;

namespace host {
    int heap_high_kb() { return (int)(__malloc_max_sbrked_mem / 1024); }

    uint8_t* rdram() { return g_given ? g_block + guard_size : nullptr; }

    MemStats mem_stats() {
        MemStats m{};
        m.free_kb = (int)(sceKernelTotalFreeMemSize() / 1024);
        m.max_kb = (int)(sceKernelMaxFreeMemSize() / 1024);
        struct mallinfo mi = mallinfo();
        m.heap_kb = (int)(mi.arena / 1024);          // taken from sbrk now (the high-water is heap_high_kb)
        m.heap_used_kb = (int)(mi.uordblks / 1024);  // in use now
        uint32_t lo = check_guard(g_block), hi = check_guard(g_block + guard_size + rdram_size);
        m.guards_ok = lo == 0 && hi == 0;
        m.guard_low_bad = lo;     // 1 + the offset of the first nonzero byte, or 0
        m.guard_high_bad = hi;
        return m;
    }

    // RDRAM as the N64 sees it (byte addresses, big-endian words), as hex lines of 16 bytes.
    bool peek(uint32_t addr, uint32_t len, std::string& out) {
        if (!g_given) {
            out = "peek: RDRAM isn't up yet\n";
            return false;
        }
        const uint8_t* rdram = g_block + guard_size;
        char line[96];
        out.clear();
        for (uint32_t at = 0; at < len; at += 16) {
            int n = snprintf(line, sizeof line, "%08X:", (unsigned)(addr + at));
            for (uint32_t i = 0; i < 16 && at + i < len; i++) {
                uint32_t off = ((addr + at + i) & 0x007FFFFFu) ^ 3;
                n += snprintf(line + n, sizeof line - n, " %02X", rdram[off]);
            }
            out += line;
            out += '\n';
        }
        return true;
    }
}
