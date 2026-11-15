// MEMCHECK builds (-DBLITZ_PSP_MEMCHECK, docs/PSP_PORT.md §4.2): force-included before everything (CMake -include), so it
// replaces recomp.h's and ultra64.h's RDRAM_OFF: every guest address the game's code or the runtime turns into an RDRAM
// offset is checked before it is used. Inside 0x80000000-0x807FFFFF (KSEG0) or 0xA0000000-0xA07FFFFF (KSEG1) it maps as
// the PSP's mask does; anything else is an escape: counted, the first 50 logged with the function (whose name carries
// the vram), then masked onto RDRAM exactly as RECOMP_RDRAM_MASK does on the device.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
uint32_t blitz_psp_memcheck_escape(uint32_t addr, const char* func);
uint64_t blitz_psp_memcheck_escapes(void);
#ifdef __cplusplus
}
#endif

static inline uint32_t blitz_psp_rdram_off(uint32_t addr, const char* func) {
    if (__builtin_expect(addr - 0x80000000u < 0x00800000u, 1)) return addr - 0x80000000u;
    if (addr - 0xA0000000u < 0x00800000u) return addr & 0x007FFFFFu;
    return blitz_psp_memcheck_escape(addr, func);
}

#define RDRAM_OFF(addr) blitz_psp_rdram_off((uint32_t)(addr), __func__)
