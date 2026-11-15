#include "recomp.h"
#include "funcs.h"

// Hand-written in N64Recomp's output style (same call/delay-slot/after_N/jump-table shapes as
// build/RecompiledFuncs) to exercise resumable.py: a direct call in a loop, a jump table, an
// indirect call, callee-saved registers kept on the guest stack across sleeps, a tail call.
// Guest addresses: functions at 0x801000xx, data at 0x80200000, stack below 0x80300000.

RECOMP_FUNC void proc_entry(uint8_t* rdram, recomp_context* ctx) {
    uint64_t hi = 0, lo = 0, result = 0;
    int c1cs = 0;
    // 0x80100000: addiu       $sp, $sp, -0x20
    ctx->r29 = ADD32(ctx->r29, -0X20);
    // 0x80100004: sw          $s0, 0x10($sp)
    MEM_W(0X10, ctx->r29) = ctx->r16;
    // 0x80100008: addu        $s0, $zero, $zero
    ctx->r16 = ADD32(0, 0);
L_8010000C:
    // 0x8010000C: jal         0x80100100
    // 0x80100010: addu        $a0, $s0, $zero
    ctx->r4 = ADD32(ctx->r16, 0);
    mid_fn(rdram, ctx);
        goto after_0;
    // 0x80100010: addu        $a0, $s0, $zero
    ctx->r4 = ADD32(ctx->r16, 0);
    after_0:
    // 0x80100014: mult        $v0, $s0
    result = S64(S32(ctx->r2)) * S64(S32(ctx->r16)); lo = S32(result >> 0); hi = S32(result >> 32);
    // 0x80100018: mflo        $t0
    ctx->r8 = lo;
    // 0x8010001C: lui         $t1, 0x8020
    ctx->r9 = S32(0X8020 << 16);
    // 0x80100020: sll         $t2, $s0, 2
    ctx->r10 = S32(ctx->r16 << 2);
    // 0x80100024: addu        $t1, $t1, $t2
    ctx->r9 = ADD32(ctx->r9, ctx->r10);
    // 0x80100028: sw          $t0, 0x0($t1)
    MEM_W(0X0, ctx->r9) = ctx->r8;
    // 0x8010002C: addiu       $s0, $s0, 0x1
    ctx->r16 = ADD32(ctx->r16, 0X1);
    // 0x80100030: addiu       $at, $zero, 0x5
    ctx->r1 = ADD32(0, 0X5);
    // 0x80100034: bne         $s0, $at, L_8010000C
    if (ctx->r16 != ctx->r1) {
        // 0x80100038: nop

            goto L_8010000C;
    }
    // 0x80100038: nop

    // 0x8010003C: lw          $s0, 0x10($sp)
    ctx->r16 = MEM_W(ctx->r29, 0X10);
    // 0x80100040: j           0x80100400
    // 0x80100044: addiu       $sp, $sp, 0x20
    ctx->r29 = ADD32(ctx->r29, 0X20);
    tail_fn(rdram, ctx);
    return;
    // 0x80100044: addiu       $sp, $sp, 0x20
    ctx->r29 = ADD32(ctx->r29, 0X20);
;}

RECOMP_FUNC void mid_fn(uint8_t* rdram, recomp_context* ctx) {
    uint64_t hi = 0, lo = 0, result = 0;
    int c1cs = 0;
    // 0x80100100: addiu       $sp, $sp, -0x20
    ctx->r29 = ADD32(ctx->r29, -0X20);
    // 0x80100104: sw          $s1, 0x14($sp)
    MEM_W(0X14, ctx->r29) = ctx->r17;
    // 0x80100108: addu        $s1, $a0, $zero
    ctx->r17 = ADD32(ctx->r4, 0);
    // 0x8010010C: andi        $v0, $a0, 0x1
    ctx->r2 = ctx->r4 & 0X1;
    // 0x80100110: sll         $v0, $v0, 2
    ctx->r2 = S32(ctx->r2 << 2);
    // 0x80100114: jr          $v0
    // 0x80100118: nop

    gpr jr_addend_80100114 = ctx->r2;
    switch (jr_addend_80100114 >> 2) {
        case 0: goto L_8010011C; break;
        case 1: goto L_80100140; break;
        default: switch_error(__func__, 0x80100114, 0x80250000);
    }
L_8010011C:
    // 0x8010011C: lui         $v0, 0x8010
    ctx->r2 = S32(0X8010 << 16);
    // 0x80100120: addiu       $v0, $v0, 0x200
    ctx->r2 = ADD32(ctx->r2, 0X200);
    // 0x80100124: jalr        $v0
    // 0x80100128: addiu       $a0, $s1, 0x64
    ctx->r4 = ADD32(ctx->r17, 0X64);
    LOOKUP_FUNC(ctx->r2)(rdram, ctx);
        goto after_0;
    // 0x80100128: addiu       $a0, $s1, 0x64
    ctx->r4 = ADD32(ctx->r17, 0X64);
    after_0:
    // 0x8010012C: b           L_80100150
    // 0x80100130: nop

        goto L_80100150;
    // 0x80100130: nop

L_80100140:
    // 0x80100140: jal         0x80100300
    // 0x80100144: addu        $a0, $s1, $zero
    ctx->r4 = ADD32(ctx->r17, 0);
    leaf_b(rdram, ctx);
        goto after_1;
    // 0x80100144: addu        $a0, $s1, $zero
    ctx->r4 = ADD32(ctx->r17, 0);
    after_1:
L_80100150:
    // 0x80100150: jal         0x80100500
    // 0x80100154: nop

    no_sleep(rdram, ctx);
        goto after_2;
    // 0x80100154: nop

    after_2:
    // 0x80100158: addu        $v0, $v0, $s1
    ctx->r2 = ADD32(ctx->r2, ctx->r17);
    // 0x8010015C: lw          $s1, 0x14($sp)
    ctx->r17 = MEM_W(ctx->r29, 0X14);
    // 0x80100160: jr          $ra
    // 0x80100164: addiu       $sp, $sp, 0x20
    ctx->r29 = ADD32(ctx->r29, 0X20);
    return;
    // 0x80100164: addiu       $sp, $sp, 0x20
    ctx->r29 = ADD32(ctx->r29, 0X20);
;}

RECOMP_FUNC void leaf_a(uint8_t* rdram, recomp_context* ctx) {
    uint64_t hi = 0, lo = 0, result = 0;
    int c1cs = 0;
    // 0x80100200: addiu       $sp, $sp, -0x18
    ctx->r29 = ADD32(ctx->r29, -0X18);
    // 0x80100204: sw          $s0, 0x10($sp)
    MEM_W(0X10, ctx->r29) = ctx->r16;
    // 0x80100208: addu        $s0, $a0, $zero
    ctx->r16 = ADD32(ctx->r4, 0);
    // 0x8010020C: lui         $t0, 0x8020
    ctx->r8 = S32(0X8020 << 16);
    // 0x80100210: lw          $t1, 0x100($t0)
    ctx->r9 = MEM_W(ctx->r8, 0X100);
    // 0x80100214: addiu       $t1, $t1, 0x1
    ctx->r9 = ADD32(ctx->r9, 0X1);
    // 0x80100218: jal         0x80100600
    // 0x8010021C: sw          $t1, 0x100($t0)
    MEM_W(0X100, ctx->r8) = ctx->r9;
    process_sleep(rdram, ctx);
        goto after_0;
    // 0x8010021C: sw          $t1, 0x100($t0)
    MEM_W(0X100, ctx->r8) = ctx->r9;
    after_0:
    // 0x80100220: lui         $t0, 0x8020
    ctx->r8 = S32(0X8020 << 16);
    // 0x80100224: lw          $v0, 0x100($t0)
    ctx->r2 = MEM_W(ctx->r8, 0X100);
    // 0x80100228: addu        $v0, $v0, $s0
    ctx->r2 = ADD32(ctx->r2, ctx->r16);
    // 0x8010022C: lw          $s0, 0x10($sp)
    ctx->r16 = MEM_W(ctx->r29, 0X10);
    // 0x80100230: jr          $ra
    // 0x80100234: addiu       $sp, $sp, 0x18
    ctx->r29 = ADD32(ctx->r29, 0X18);
    return;
    // 0x80100234: addiu       $sp, $sp, 0x18
    ctx->r29 = ADD32(ctx->r29, 0X18);
;}

RECOMP_FUNC void leaf_b(uint8_t* rdram, recomp_context* ctx) {
    uint64_t hi = 0, lo = 0, result = 0;
    int c1cs = 0;
    // 0x80100300: addiu       $sp, $sp, -0x18
    ctx->r29 = ADD32(ctx->r29, -0X18);
    // 0x80100304: sw          $s0, 0x10($sp)
    MEM_W(0X10, ctx->r29) = ctx->r16;
    // 0x80100308: sll         $s0, $a0, 3
    ctx->r16 = S32(ctx->r4 << 3);
    // 0x8010030C: jal         0x80100600
    // 0x80100310: addiu       $a0, $zero, 0x2
    ctx->r4 = ADD32(0, 0X2);
    process_sleep(rdram, ctx);
        goto after_0;
    // 0x80100310: addiu       $a0, $zero, 0x2
    ctx->r4 = ADD32(0, 0X2);
    after_0:
    // 0x80100314: addiu       $s0, $s0, 0x3
    ctx->r16 = ADD32(ctx->r16, 0X3);
    // 0x80100318: jal         0x80100600
    // 0x8010031C: addiu       $a0, $zero, 0x4
    ctx->r4 = ADD32(0, 0X4);
    process_sleep(rdram, ctx);
        goto after_1;
    // 0x8010031C: addiu       $a0, $zero, 0x4
    ctx->r4 = ADD32(0, 0X4);
    after_1:
    // 0x80100320: addu        $v0, $s0, $zero
    ctx->r2 = ADD32(ctx->r16, 0);
    // 0x80100324: lw          $s0, 0x10($sp)
    ctx->r16 = MEM_W(ctx->r29, 0X10);
    // 0x80100328: jr          $ra
    // 0x8010032C: addiu       $sp, $sp, 0x18
    ctx->r29 = ADD32(ctx->r29, 0X18);
    return;
    // 0x8010032C: addiu       $sp, $sp, 0x18
    ctx->r29 = ADD32(ctx->r29, 0X18);
;}

RECOMP_FUNC void tail_fn(uint8_t* rdram, recomp_context* ctx) {
    uint64_t hi = 0, lo = 0, result = 0;
    int c1cs = 0;
    // 0x80100400: jal         0x80100600
    // 0x80100404: addiu       $a0, $zero, 0x8
    ctx->r4 = ADD32(0, 0X8);
    process_sleep(rdram, ctx);
        goto after_0;
    // 0x80100404: addiu       $a0, $zero, 0x8
    ctx->r4 = ADD32(0, 0X8);
    after_0:
    // 0x80100408: lui         $t0, 0x8020
    ctx->r8 = S32(0X8020 << 16);
    // 0x8010040C: addiu       $t1, $zero, 0x1234
    ctx->r9 = ADD32(0, 0X1234);
    // 0x80100410: jr          $ra
    // 0x80100414: sw          $t1, 0x200($t0)
    MEM_W(0X200, ctx->r8) = ctx->r9;
    return;
    // 0x80100414: sw          $t1, 0x200($t0)
    MEM_W(0X200, ctx->r8) = ctx->r9;
;}

RECOMP_FUNC void no_sleep(uint8_t* rdram, recomp_context* ctx) {
    uint64_t hi = 0, lo = 0, result = 0;
    int c1cs = 0;
    // 0x80100500: lui         $t0, 0x8020
    ctx->r8 = S32(0X8020 << 16);
    // 0x80100504: lw          $t1, 0x104($t0)
    ctx->r9 = MEM_W(ctx->r8, 0X104);
    // 0x80100508: addiu       $t1, $t1, 0x11
    ctx->r9 = ADD32(ctx->r9, 0X11);
    // 0x8010050C: jr          $ra
    // 0x80100510: sw          $t1, 0x104($t0)
    MEM_W(0X104, ctx->r8) = ctx->r9;
    return;
    // 0x80100510: sw          $t1, 0x104($t0)
    MEM_W(0X104, ctx->r8) = ctx->r9;
;}

RECOMP_FUNC void process_sleep(uint8_t* rdram, recomp_context* ctx) {
    uint64_t hi = 0, lo = 0, result = 0;
    int c1cs = 0;
    // 0x80100600: lui         $v1, 0x8020
    ctx->r3 = S32(0X8020 << 16);
    // 0x80100604: lw          $v0, 0x300($v1)
    ctx->r2 = MEM_W(ctx->r3, 0X300);
    // 0x80100608: or          $v0, $v0, $a0
    ctx->r2 = ctx->r2 | ctx->r4;
    // 0x8010060C: addu        $a0, $zero, $zero
    ctx->r4 = ADD32(0, 0);
    // 0x80100610: jal         0x8001E8A0
    // 0x80100614: sw          $v0, 0x300($v1)
    MEM_W(0X300, ctx->r3) = ctx->r2;
    blitz_osStopThread(rdram, ctx);
        goto after_0;
    // 0x80100614: sw          $v0, 0x300($v1)
    MEM_W(0X300, ctx->r3) = ctx->r2;
    after_0:
    // 0x80100618: jr          $ra
    // 0x8010061C: nop

    return;
    // 0x8010061C: nop

;}

// Positive control for the liveness check: lo is read after a call that can sleep.
RECOMP_FUNC void live_bad(uint8_t* rdram, recomp_context* ctx) {
    uint64_t hi = 0, lo = 0, result = 0;
    int c1cs = 0;
    // 0x80100700: mult        $a0, $a1
    result = S64(S32(ctx->r4)) * S64(S32(ctx->r5)); lo = S32(result >> 0); hi = S32(result >> 32);
    // 0x80100704: jal         0x80100600
    // 0x80100708: nop

    process_sleep(rdram, ctx);
        goto after_0;
    // 0x80100708: nop

    after_0:
    // 0x8010070C: mflo        $v0
    ctx->r2 = lo;
    // 0x80100710: jr          $ra
    // 0x80100714: nop

    return;
    // 0x80100714: nop

;}
