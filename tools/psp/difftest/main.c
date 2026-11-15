/* gpr32 differential test driver: the stock 64-bit gpr build (s), gpr32 (g) and gpr32 + RECOMP_RDRAM_MASK (m) of ops.c
   run the same random sign-extended inputs; any difference in the low 32 bits or in RDRAM is a mismatch. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int32_t imm16; int32_t uimm16; int sa; int32_t offw, offh, offb, offd; } imm_t;
#define DECL(m) void RUNW_##m(int, uint8_t*, const uint64_t*, const imm_t*, int, uint64_t*); int NOPSF_##m(void); const char* OPNAMEF_##m(int);
DECL(s) DECL(g) DECL(m)
static uint64_t X = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { X ^= X << 13; X ^= X >> 7; X ^= X << 17; return X; }
static uint64_t sext(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }
static uint32_t val(void) {
    static const uint32_t edge[] = {0, 1, 2, 3, 4, 31, 32, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000u, 0x80000001u,
        0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFF8000u, 0x80001000u, 0x807FFFFCu, 0x40000000u, 0xC0000000u, 0xFFFFFFE0u};
    switch (rnd() % 4) { case 0: return edge[rnd() % (sizeof edge / sizeof *edge)]; case 1: return (uint32_t)rnd() & 0xFFFF;
        case 2: return (uint32_t)(int32_t)(int16_t)rnd(); default: return (uint32_t)rnd(); }
}
#define RD (8u << 20)
int main(int argc, char** argv) {
    long iters = argc > 1 ? atol(argv[1]) : 200000;
    uint8_t *ms = malloc(RD), *mg = malloc(RD), *mm = malloc(RD);
    for (uint32_t i = 0; i < RD; i++) ms[i] = (uint8_t)(rnd() >> 7);
    memcpy(mg, ms, RD); memcpy(mm, ms, RD);
    int n = NOPSF_s(); long bad[256] = {0}, badinv[256] = {0}, runs[256] = {0};
    for (long it = 0; it < iters; it++) {
        uint64_t regs[32]; for (int i = 0; i < 32; i++) regs[i] = sext(val());
        uint32_t base = 0x80000000u + 0x10000u + (uint32_t)(rnd() % (RD - 0x20000u));
        regs[6] = sext(base & ~7u); regs[7] = sext((uint32_t)(rnd() % 0x1000));
        imm_t im = { (int16_t)val(), (int32_t)(val() & 0xFFFF), (int)(rnd() % 32), 0, 0, 0, 0 };
        int32_t off = (int16_t)rnd(); im.offw = off & ~3; im.offh = off & ~1; im.offb = off & ~3 ? (off & ~3) + (int)(rnd() % 4) - 0 : 0; im.offd = off & ~7;
        im.offb = (off & ~3) + (int)(rnd() % 4); if (im.offb > 0x7FF0) im.offb = 0x7FF0;
        int k = (int)(rnd() % n);
        uint64_t os[2], og[2], om[2];
        RUNW_s(k, ms, regs, &im, 0, os); RUNW_g(k, mg, regs, &im, 0, og); RUNW_m(k, mm, regs, &im, 0, om);
        runs[k]++;
        int b = 0;
        for (int j = 0; j < 2; j++) {
            if ((uint32_t)os[j] != (uint32_t)og[j] || (uint32_t)os[j] != (uint32_t)om[j]) b = 1;
            if (os[j] != sext((uint32_t)os[j])) badinv[k]++;   /* the stock build's invariant: sign-extended */
        }
        uint32_t lo = (base & ~7u) - 0x80000000u + (uint32_t)im.offd - 64u;
        if (memcmp(ms + lo, mg + lo, 256) || memcmp(ms + lo, mm + lo, 256)) b = 1;
        if (b) { if (bad[k]++ < 3) fprintf(stderr, "MISMATCH %s: r4=%08x r5=%08x imm=%d sa=%d -> stock %016llx %016llx gpr32 %08x %08x mask %08x %08x\n",
            OPNAMEF_s(k), (uint32_t)regs[4], (uint32_t)regs[5], im.imm16, im.sa, (unsigned long long)os[0], (unsigned long long)os[1],
            (uint32_t)og[0], (uint32_t)og[1], (uint32_t)om[0], (uint32_t)om[1]);
            memcpy(mg + lo, ms + lo, 256); memcpy(mm + lo, ms + lo, 256); }
    }
    long tb = 0;
    for (int k = 0; k < n; k++) { printf("%-12s runs %7ld mismatches %ld  stock-not-sext %ld\n", OPNAMEF_s(k), runs[k], bad[k], badinv[k]); tb += bad[k]; }
    /* div by zero: what each build gives (the game never uses it: every div is followed by break 7) */
    uint64_t regs[32] = {0}; imm_t im = {0}; uint64_t os[2], og[2];
    for (int s = 0; s < 2; s++) { regs[4] = sext(s ? 0xFFFFFFF0u : 5); regs[5] = 0;
        int kd = -1; for (int k = 0; k < n; k++) if (!strcmp(OPNAMEF_s(k), "div")) kd = k;
        RUNW_s(kd, ms, regs, &im, 1, os); RUNW_g(kd, mg, regs, &im, 1, og);
        printf("div %d / 0: stock lo=%08x hi=%08x  gpr32 (VR4300) lo=%08x hi=%08x\n", (int32_t)regs[4], (uint32_t)os[0], (uint32_t)os[1], (uint32_t)og[0], (uint32_t)og[1]); }
    printf("TOTAL mismatches %ld over %ld iterations\n", tb, iters);
    return tb != 0;
}
