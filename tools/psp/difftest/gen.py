#!/usr/bin/env python3
"""Writes ops.c for tools/psp/difftest/run.sh (docs/PSP_PORT.md section 3.5)."""
# Differential test: each construct class the generator emits (statement text in the generated form), run with
# 64-bit gpr (stock recomp.h) and 32-bit gpr (RECOMP_GPR32). Inputs are sign-extended 32-bit values.
ops = []   # (name, statement(s), output kind: 'r2' | 'lohi' | 'mem' | 'c1cs' | 'f' | 'branch')
def op(name, stmt, out='r2'): ops.append((name, stmt, out))
# ALU
op('addiu', 'ctx->r2 = ADD32(ctx->r4, IMM16);')
op('addu', 'ctx->r2 = ADD32(ctx->r4, ctx->r5);')
op('li', 'ctx->r2 = ADD32(0, IMM16);')
op('subu', 'ctx->r2 = SUB32(ctx->r4, ctx->r5);')
op('negu', 'ctx->r2 = SUB32(0, ctx->r5);')
op('lui', 'ctx->r2 = S32(UIMM16 << 16);')
op('andi', 'ctx->r2 = ctx->r4 & UIMM16;')
op('ori', 'ctx->r2 = ctx->r4 | UIMM16;')
op('xori', 'ctx->r2 = ctx->r4 ^ UIMM16;')
op('ori0', 'ctx->r2 = 0 | UIMM16;')
op('and', 'ctx->r2 = ctx->r4 & ctx->r5;')
op('or', 'ctx->r2 = ctx->r4 | ctx->r5;')
op('xor', 'ctx->r2 = ctx->r4 ^ ctx->r5;')
op('nor', 'ctx->r2 = ~(ctx->r4 | ctx->r5);')
op('nor0', 'ctx->r2 = ~(0 | ctx->r5);')
op('sll', 'ctx->r2 = S32(ctx->r4 << SA);')
op('sllv', 'ctx->r2 = S32(ctx->r4 << (ctx->r5 & 31));')
op('srl', 'ctx->r2 = S32(U32(ctx->r4) >> SA);')
op('srlv', 'ctx->r2 = S32(U32(ctx->r4) >> (ctx->r5 & 31));')
op('sra', 'ctx->r2 = S32(SIGNED(ctx->r4) >> SA);')
op('srav', 'ctx->r2 = S32(SIGNED(ctx->r4) >> (ctx->r5 & 31));')
op('slti', 'ctx->r2 = SIGNED(ctx->r4) < IMM16 ? 1 : 0;')
op('sltiu', 'ctx->r2 = ctx->r4 < IMM16 ? 1 : 0;')
op('slt', 'ctx->r2 = SIGNED(ctx->r4) < SIGNED(ctx->r5) ? 1 : 0;')
op('slt0', 'ctx->r2 = SIGNED(0) < SIGNED(ctx->r5) ? 1 : 0;')
op('sltu', 'ctx->r2 = ctx->r4 < ctx->r5 ? 1 : 0;')
op('sltu0', 'ctx->r2 = 0 < ctx->r5 ? 1 : 0;')
# branches
for nm, cond in [('beq', 'ctx->r4 == ctx->r5'), ('bne', 'ctx->r4 != ctx->r5'), ('beqz', 'ctx->r4 == 0'), ('bnez', 'ctx->r4 != 0'),
                 ('blez', 'SIGNED(ctx->r4) <= 0'), ('bgez', 'SIGNED(ctx->r4) >= 0'), ('bltz', 'SIGNED(ctx->r4) < 0'), ('bgtz', 'SIGNED(ctx->r4) > 0')]:
    op(nm, f'ctx->r2 = 0; if ({cond}) {{ ctx->r2 = 1; }}')
# mult/div
op('mult', 'result = S64(S32(ctx->r4)) * S64(S32(ctx->r5)); lo = S32(result >> 0); hi = S32(result >> 32); ctx->r2 = lo; ctx->r3 = hi;', 'lohi')
op('multu', 'result = U64(U32(ctx->r4)) * U64(U32(ctx->r5)); lo = S32(result >> 0); hi = S32(result >> 32); ctx->r2 = lo; ctx->r3 = hi;', 'lohi')
op('div', 'DIVSTMT ctx->r2 = lo; ctx->r3 = hi;', 'lohi')
op('divu', 'if (ctx->r5 != 0) { lo = S32(U32(ctx->r4) / U32(ctx->r5)); hi = S32(U32(ctx->r4) % U32(ctx->r5)); } ctx->r2 = lo; ctx->r3 = hi;', 'lohi')
op('mthi_mflo', 'hi = ctx->r4; lo = ctx->r5; ctx->r2 = lo; ctx->r3 = hi;', 'lohi')
# cop1 moves
op('mtc1_mfc1', 'ctx->f0.u32l = ctx->r4; ctx->r2 = (int32_t)ctx->f0.u32l;')
op('cvt_s_w', 'ctx->f0.u32l = ctx->r4; ctx->f2.fl = CVT_S_W(ctx->f0.u32l); ctx->f4.u32l = TRUNC_W_S(ctx->f2.fl); ctx->r2 = (int32_t)ctx->f4.u32l;')
# jump table index
op('jtbl', 'gpr jr_addend_1 = ctx->r4; ctx->r2 = 9; switch (jr_addend_1 >> 2) { case 0: ctx->r2 = 0; break; case 1: ctx->r2 = 1; break; case 2: ctx->r2 = 2; break; default: ctx->r2 = 7; }')
op('lookup_arg', 'ctx->r2 = (gpr)(int32_t)(LOOKUP_ARG(ctx->r4));')
# hook casts
op('hook_i32', 'ctx->r2 = (int32_t)(ctx->r4 + 0x30);')
op('hook_cmp', 'ctx->r2 = (int32_t)ctx->r4 < 32;')
op('hook_ucmp', 'ctx->r2 = (uint32_t)ctx->r4 < 18;')
op('hook_add', 'ctx->r2 = (int32_t)(0x802EFDF4u + (uint32_t)ctx->r4);')
op('hook_i16', 'ctx->r2 = (int32_t)(int16_t)ctx->r4 + 1;')
# memory (r6 = a valid KSEG0 base, OFF = an aligned int16 offset within RDRAM)
op('lw', 'ctx->r2 = MEM_W(ctx->r6, OFFW);', 'r2')
op('lh', 'ctx->r2 = MEM_H(ctx->r6, OFFH);', 'r2')
op('lhu', 'ctx->r2 = MEM_HU(ctx->r6, OFFH);', 'r2')
op('lb', 'ctx->r2 = MEM_B(ctx->r6, OFFB);', 'r2')
op('lbu', 'ctx->r2 = MEM_BU(ctx->r6, OFFB);', 'r2')
op('sw', 'MEM_W(OFFW, ctx->r6) = ctx->r4; ctx->r2 = MEM_W(ctx->r6, OFFW);', 'mem')
op('sh', 'MEM_H(OFFH, ctx->r6) = ctx->r4; ctx->r2 = MEM_H(ctx->r6, OFFH);', 'mem')
op('sb', 'MEM_B(OFFB, ctx->r6) = ctx->r4; ctx->r2 = MEM_B(ctx->r6, OFFB);', 'mem')
op('lwl_lwr', 'ctx->r2 = ctx->r5; ctx->r2 = do_lwl(rdram, ctx->r2, ctx->r6, OFFB); ctx->r2 = do_lwr(rdram, ctx->r2, ctx->r6, OFFB + 3);', 'r2')
op('lwr_only', 'ctx->r2 = ctx->r5; ctx->r2 = do_lwr(rdram, ctx->r2, ctx->r6, OFFB);', 'r2')
op('swl_swr', 'do_swl(rdram, OFFB, ctx->r6, ctx->r4); do_swr(rdram, OFFB + 3, ctx->r6, ctx->r4); ctx->r2 = MEM_W(ctx->r6, OFFW);', 'mem')
op('ldc1_sdc1', 'ctx->f2.u64 = LD(ctx->r6, OFFD); ctx->f2.u64 ^= 0x0123456789ABCDEFull; SD(ctx->f2.u64, OFFD, ctx->r6); ctx->r2 = MEM_W(ctx->r6, OFFD); ctx->r3 = MEM_W(ctx->r6, OFFD + 4);', 'mem')
op('hook_membu', 'ctx->r2 = MEM_BU((int32_t)0x80001000, ctx->r7);', 'r2')
op('hook_membu2', 'ctx->r2 = MEM_BU(0, (int32_t)0x80001001) < 22;', 'r2')

body = []
for k, (name, stmt, out) in enumerate(ops):
    body.append(f'    case {k}: {{ {stmt} }} break;')
src = r'''#include <string.h>
#include "recomp.h"
#ifdef RECOMP_GPR32
#  define DIVSTMT if (ctx->r5 != 0 || DIV_ZERO_TOO) { uint32_t _l, _h; DIV32(ctx->r4, ctx->r5, &_l, &_h); lo = _l; hi = _h; }
#else
#  define DIVSTMT if (ctx->r5 != 0 || DIV_ZERO_TOO) { lo = S32(S64(S32(ctx->r4)) / S64(S32(ctx->r5))); hi = S32(S64(S32(ctx->r4)) % S64(S32(ctx->r5))); }
#endif
#define LOOKUP_ARG(val) ((int32_t)(val))
typedef struct { int32_t imm16; int32_t uimm16; int sa; int32_t offw, offh, offb, offd; } imm_t;
#define IMM16 im->imm16
#define UIMM16 im->uimm16
#define SA im->sa
#define OFFW im->offw
#define OFFH im->offh
#define OFFB im->offb
#define OFFD im->offd
static int OPS_COUNT = OPS_N;
static const char* OPN_TABLE[] = { NAMES };
void RUN(int k, uint8_t* rdram, recomp_context* ctx, const imm_t* im, int DIV_ZERO_TOO) {
    uint64_t result = 0;
#ifdef RECOMP_GPR32
    uint32_t hi = 0, lo = 0;
#else
    uint64_t hi = 0, lo = 0;
#endif
    switch (k) {
BODY
    }
}
void RUNW(int k, uint8_t* rdram, const uint64_t* regs, const imm_t* im, int dz, uint64_t* out) {
    recomp_context c; memset(&c, 0, sizeof c);
    for (int i = 0; i < 32; i++) (&c.r0)[i] = (gpr)regs[i];
    c.r0 = 0;
    RUN(k, rdram, &c, im, dz);
    out[0] = c.r2; out[1] = c.r3;
}
int NOPSF(void) { return OPS_COUNT; }
const char* OPNAMEF(int k) { return OPN_TABLE[k]; }
'''.replace('OPS_N', str(len(ops))).replace('= { NAMES }', '= { ' + ', '.join(f'"{n}"' for n, _, _ in ops) + ' }').replace('BODY', '\n'.join(body))
import sys
open(sys.argv[1], 'w').write(src)
