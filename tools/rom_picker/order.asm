; Team select in name order for exported Modern Blitz ROMs, as the app does (src/game/team_select.cpp): team+ lists
; teams in block order (the 1999 cities, Houston last), so UP/DOWN steps to the next team in name order and the rows
; above and below the cursor show its neighbours in that order. The cursor, the stored choice and the CPU's picks
; stay real team slots. The export fills N, ORDER and POS from the teams' names (city, then select name).
;
; Where it lives: libultra's osPfsIsPlug (boot 0x80024EC0, 0x17C bytes), which the game links but never calls.
; The patched sites (ovl_b, team+), each `jal` with its displaced instruction redone here:
;   802F9B60  slt at, s6, v0     -> jal moved   (UP/DOWN moved s6 by +-1; v0 the team count; its delay slot
;                                               `negu v1, at` is redone)
;   802FA3AC  lw v0, 0x54(sp)    -> jal above   (each row above the cursor; delay slot `sltu at, s3, v0` redone)
;   802FA4EC  addiu a0, zero, 0x14 -> jal below (each row below; delay slot `slt s1, s3, fp` redone)
; Only t registers are used besides the ones each site sets.

.n64
.create "order.bin", 0x80024EC0

.org 0x80024EC0
; step: t0 the slot, t1 the direction (+-1), t2 the team count -> t0 the next slot that way in name order. Linked t8.
step:
    sltiu t3, t2, 33
    beqz  t3, @@arith
    la    t4, N
    lbu   t4, 0(t4)
    bne   t4, t2, @@arith       ; tables for another count: plain order
    sltu  t3, t0, t2
    beqz  t3, @@arith           ; a slot outside the list: plain order
    la    t4, POS
    addu  t4, t4, t0
    lbu   t3, 0(t4)
    addu  t3, t3, t1
    bgez  t3, @@low
    nop
    addu  t3, t3, t2
@@low:
    slt   t4, t3, t2
    bnez  t4, @@pick
    nop
    subu  t3, t3, t2
@@pick:
    la    t4, ORDER
    addu  t4, t4, t3
    jr    t8
    lbu   t0, 0(t4)
@@arith:
    addu  t0, t0, t1
    bgez  t0, @@a1
    nop
    addu  t0, t0, t2
@@a1:
    slt   t4, t0, t2
    bnez  t4, @@a2
    nop
    subu  t0, t0, t2
@@a2:
    jr    t8
    nop

; UP/DOWN: the game moved s6 by +-1 for direction code [sp+0x4C] (its jump table); redo the move in name order.
moved:
    lw    t1, 0x4C(sp)
    sltiu t3, t1, 11
    beqz  t3, @@set
    la    t4, STEP_OF
    addu  t4, t4, t1
    lb    t1, 0(t4)
    beqz  t1, @@set
    subu  t0, s6, t1            ; where the cursor was
    move  t2, v0
    la    t5, step
    jalr  t5, t8                ; armips: jalr target, link
    nop
    move  s6, t0
@@set:
    slt   at, s6, v0
    jr    ra
    negu  v1, at

; Each row above the cursor: the team before the last one shown (s3 + 1) in name order.
above:
    lw    t2, 0x54(sp)
    addiu t0, s3, 1
    addiu t1, zero, -1
    la    t5, step
    jalr  t5, t8
    nop
    move  s3, t0
    lw    v0, 0x54(sp)
    jr    ra
    sltu  at, s3, v0

; Each row below: the team after the last one shown (s3 - 1); fp is the team count here.
below:
    move  t2, fp
    addiu t0, s3, -1
    addiu t1, zero, 1
    la    t5, step
    jalr  t5, t8
    nop
    move  s3, t0
    addiu a0, zero, 0x14
    jr    ra
    slt   s1, s3, fp

STEP_OF: .byte 0, -1, 1, 0, -1, -1, 1, 0, 1, -1, 1
N:       .byte 0                ; filled by the export (0: plain order)
ORDER:   .fill 32, 0            ; position -> team slot
POS:     .fill 32, 0            ; team slot -> position
.if . > 0x8002503C
.error "order.asm is past osPfsIsPlug"
.endif
.close
