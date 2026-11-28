; Team select in name order for ROMs exported from Modern on the original game (Export ROM,
; src/game/export_rom.cpp), as the app does (src/game/engine32.cpp blitz_v32_moved / blitz_v32_row): the game lists
; its 31 slots in slot order, so UP/DOWN steps to the next team in name order and the rows above and below the
; cursor show its neighbours in that order. The cursor, the stored choice and the CPU's picks stay real slots. The
; export fills ORDER and POS from the teams' names (city, then select name) and NEXT with the call it displaces.
;
; Where it lives: libultra's osPfsIsPlug (boot 0x80024EC0, 0x17C bytes), which the game links but never calls (the
; Team Extension's order.asm uses it on its ROMs; the two are never on one ROM).
; The patched sites (ovl_b, the original game), each `jal` with its displaced instruction redone here:
;   802F85E0  jal 8024AEA4 (or Choose CPU Opponent's reset) -> jal reset  (team select starts: no cursor yet)
;   802F9B74  sll s0, s6, 16       -> jal moved      (the cursor's slot s6 after a move or a jump; s0 = s6 redone)
;   802FA3A8  addiu s3, fp, -1     -> jal above_0    (the first row above the cursor fp)
;   802FA454  addiu s3, s3, -1     -> jal above_n    (each next row above)
;   802FA47C  addiu s3, fp, 1      -> jal below_0    (the first row below)
;   802FA53C  lwc1 f2, 0(s7)       -> jal below_n    (each next row below: s1 the row just drawn)
; Only t3-t9 are used (t0 is live across the row sites; none of these is).
; A league pak (2-30 teams) fills the game's 31 slots with its teams over and over (slot s = team s mod N, as the app's
; engine32.cpp): ORDER lists the N, POS every slot's position among them, so team select shows each team once.

.n64
.create "order_orig.bin", 0x80024EC0

TEAMS equ 31

.org 0x80024EC0
; t3 a slot, t4 the direction (+-1) -> t3 the next slot that way in name order, over the N teams in ORDER (31, or a
; league pak's 2-30 repeated through the game's slots). Linked t9; t8 is free here (moved and row are done with it).
step:
    sltiu t5, t3, TEAMS
    bnez  t5, @@in
    nop
    move  t3, zero
@@in:
    la    t8, ORDER
    addu  t5, t8, t3
    lbu   t6, TEAMS(t5)         ; POS[slot] (POS follows ORDER)
    addu  t6, t6, t4
    lbu   t5, -1(t8)            ; N
    bgez  t6, @@low
    nop
    addu  t6, t6, t5
@@low:
    slt   t3, t6, t5
    bnez  t3, @@pick
    nop
    subu  t6, t6, t5
@@pick:
    addu  t5, t8, t6
    jr    t9
    lbu   t3, 0(t5)

; Team select starts: neither list has a cursor yet; then on to the call the site made.
reset:
    addiu t3, zero, -1
    la    t5, CUR
    sb    t3, 0(t5)
    sb    t3, 1(t5)
    la    t5, NEXT
    lw    t5, 0(t5)
    jr    t5
    nop

; The game stepped s6 by one in its own order (up: was < 30 ? was + 1 : 0; down: was > 0 ? was - 1 : 30), or jumped
; (the list's first team, the CPU's pick): a step goes to the next team in name order, a jump to the same team's own
; slot (ORDER[POS[slot]]: itself with 31 teams; a league's highest copy, which the right-helmet list names).
moved:
    la    t7, CUR
    sltiu t5, s5, 2
    beqz  t5, @@done
    addu  t7, t7, s5
    lb    t3, 0(t7)             ; where the cursor was (-1: nowhere yet)
    bltz  t3, @@take
    slti  t5, t3, TEAMS - 1
    bnez  t5, @@up
    addiu t6, t3, 1
    move  t6, zero
@@up:
    beq   t6, s6, @@step
    addiu t4, zero, 1
    bgtz  t3, @@down
    addiu t6, t3, -1
    addiu t6, zero, TEAMS - 1
@@down:
    beq   t6, s6, @@step
    addiu t4, zero, -1
@@take:                         ; a jump: the same team at its own slot (a step of 0 from s6), so a league team's
    move  t3, s6                ; pick is always the slot the VS screen's right-helmet list names (22-30)
    move  t4, zero
@@step:
    la    t8, step
    jalr  t8, t9                ; armips: jalr target, link
    nop
    move  s6, t3
    sb    s6, 0(t7)
@@done:
    sll   s0, s6, 16
    jr    ra
    sra   s0, s0, 16

above_0:
    move  t3, fp
    b     row
    addiu t4, zero, -1
above_n:
    move  t3, s3
    b     row
    addiu t4, zero, -1
below_0:
    move  t3, fp
    b     row
    addiu t4, zero, 1
below_n:
    lwc1  f2, 0(s7)
    move  t3, s1
    addiu t4, zero, 1
row:                            ; s3 = the next row's team
    la    t8, step
    jalr  t8, t9
    nop
    jr    ra
    move  s3, t3

.align 4
NEXT:    .word 0x8024AEA4       ; the call reset displaced (the export sets it)
CUR:     .byte -1, -1           ; each list's cursor slot, as last shown
N:       .byte TEAMS            ; the teams team select lists (the export sets it; just before ORDER)
ORDER:   .fill TEAMS, 0         ; position -> slot (the export fills its first N)
POS:     .fill TEAMS, 0         ; slot -> position (every game slot: a league's repeats too)
.if . > 0x8002503C
.error "order_orig.asm is past osPfsIsPlug"
.endif
.close
