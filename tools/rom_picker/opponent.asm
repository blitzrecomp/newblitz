; Choose CPU Opponent for exported ROMs (Export ROM, src/game/export_rom.cpp), as src/game/opponent_select.cpp
; does in the app: in a 1-player Arcade game, once the player has picked, the empty side of team select becomes
; the opponent list, driven by the player's controller (the list's marker reads "3"). The CPU side's selection
; process (ovl_b 802F90B8) already runs, parked at its join test; it's let in as if a challenger had joined.
; "CHOOSE OPPONENT" goes over the CPU's logo as the list opens, and stays until team select ends (the game deletes its
; group). The list opens on its first team (the QB model is set up for that one only); three frames in, the cursor moves
; to a team the game might pick (rand(31), never the player's) and the QB re-dresses, as a move does (as the app's
; blitz_opp_uniform).
;
; Where it lives: libultra's guMtxIdent and guPerspectiveF (boot 0x8001CBD0, 0x2A0 bytes) and func_800277F0 (0xC0),
; which the game links but
; never calls,
; and the free tail of rules.asm's cave (0x8001D160..0x8001D300, inside guPosition, also never called; the export
; writes this after the rules code, whose image is zeros there).
; The patched sites (ovl_b; same code in Classic and Modern):
;   802F85E0  jal 8024AEA4          -> jal reset         (team select starts)
;   802F9178  lw v0, humans         -> jal wake          (the join test; its delay slot `and v0, v0, s0` is redone)
;   802F91F8  lui v1 / lhu v1, busy -> jal setup / nop   (the side's player slot, s7)
;   802F9A1C, 802F9A28, 802F9A34    -> jal in_a/b/c      (the list's input reads: the player's controller)
;   802F590C  lui v0 / lw v0, humans -> jal panel / nop  (the "please wait" panel leaves)
;   802F89C4  lui v0 / lh v0, busy  -> jal hold / nop    (team select waits for the pick)
;   802F8BC8  jal 8027B950          -> jal restore       (after the game's own CPU pick: ours)
; Only t registers are used: none is live at these sites.

.n64
.create "opponent.bin", 0x8001CBD0

GAME_MODE   equ 0x8028B998   ; 0 Arcade
PHASE       equ 0x802EA420   ; 7 attract demo
HUMANS      equ 0x802EA418   ; a bit per player slot with a human (0-1 left, 2-3 right)
FOUR_PLAYER equ 0x80283BE8
SLOT_MASK   equ 0x80283C68
CHOSEN      equ 0x802EA434   ; [side] team, -1 while choosing
BUSY        equ 0x802BD864   ; u16, a bit per slot whose selection runs
RAND_MOD    equ 0x8027B950   ; rand() % a0
DRESS       equ 0x80256580   ; (side, team, alt): builds the side's uniform
REDRAW      equ 0x802FA080   ; ovl_b (team, side, 1): the side's list, logo and QB model
REDRESSING  equ 0x8033BAB0   ; set while team select re-dresses a model

.org 0x8001CBD0
; Team select starts: no opponent list yet, and neither side has picked.
reset:
    la    t0, WOKEN
    sw    zero, 0(t0)
    addiu t1, zero, -1
    sw    t1, 8(t0)             ; TEAM
    lui   t0, hi(CHOSEN)
    sw    t1, lo(CHOSEN)(t0)
    sw    t1, lo(CHOSEN + 4)(t0)
    j     0x8024AEA4
    nop

; A 1-player-side Arcade game: t2 = the CPU side (-1 if not one), t3 = the player's slot. Called linking t8.
context:
    addiu t2, zero, -1
    lui   t0, hi(GAME_MODE)
    lw    t0, lo(GAME_MODE)(t0)
    bnez  t0, @@out
    lui   t0, hi(PHASE)
    lw    t0, lo(PHASE)(t0)
    addiu t1, zero, 7
    beq   t0, t1, @@out
    lui   t0, hi(FOUR_PLAYER)
    lw    t0, lo(FOUR_PLAYER)(t0)
    beqz  t0, @@out
    lui   t0, hi(HUMANS)
    lw    t0, lo(HUMANS)(t0)
    lui   t1, hi(SLOT_MASK)
    lw    t1, lo(SLOT_MASK)(t1)
    andi  t4, t0, 3             ; left has a human
    andi  t5, t0, 0xC           ; right has a human
    sltu  t4, zero, t4
    sltu  t5, zero, t5
    beq   t4, t5, @@out
    move  t3, zero
    beqz  t4, @@right
    nop
    addiu t2, zero, 1           ; the player's on the left: the CPU's the right
    andi  t0, t1, 1
    bnez  t0, @@out
    andi  t0, t1, 2
    b     @@out
    sltu  t3, zero, t0          ; slot 1 if only it
@@right:
    move  t2, zero
    andi  t0, t1, 4
    bnez  t0, @@out
    addiu t3, zero, 2
    andi  t0, t1, 8
    beqz  t0, @@out
    move  t3, zero
    addiu t3, zero, 3
@@out:
    jr    t8
    nop

; The CPU side's join test: once the player has picked (and nothing else is choosing), its list opens.
wake:
    la    t6, WOKEN
    lw    t7, 0(t6)
    bnez  t7, @@woken
    nop
    la    t0, context
    jalr  t0, t8                ; armips: jalr target, link
    nop
    bne   t2, s5, @@test        ; not the CPU side (or not a 1-player game)
    xori  t0, t2, 1
    sll   t0, t0, 2
    lui   t1, hi(CHOSEN)
    addu  t1, t1, t0
    lw    t1, lo(CHOSEN)(t1)    ; the player's team
    addiu t0, zero, -1
    beq   t1, t0, @@test
    lui   t0, hi(BUSY)
    lhu   t0, lo(BUSY)(t0)
    bnez  t0, @@test
    addiu t7, zero, 1
    sw    t7, 0(t6)             ; WOKEN
    sw    t2, 4(t6)             ; CPU
    sw    t3, 12(t6)            ; SLOT
    sw    zero, 16(t6)          ; FRAMES
@@woken:
    lw    t0, 4(t6)
    bne   t0, s5, @@test
    nop
    addiu s0, zero, 0xF         ; the join test passes
@@test:
    lui   v0, hi(HUMANS)
    lw    v0, lo(HUMANS)(v0)
    jr    ra
    and   v0, v0, s0

; "CHOOSE OPPONENT" over the CPU side's (a0) logo, in the list's font, yellow like the selected team, as
; opponent_select.cpp's show_label. The text settings are globals every process shares, so they're kept and put back
; (all but the list head at 0x802A9C5C, which the new text joins). Group 0x666: unused by the game, and team select's
; exit deletes it.
label:
    addiu sp, sp, -0x68
    sw    ra, 0x64(sp)
    sw    s0, 0x60(sp)
    move  s0, a0
    lui   t0, 0x802B
    addiu t0, t0, -0x63B0       ; 0x802A9C50
    addiu t1, sp, 0x10
    addiu t2, zero, 18
@@save:
    lw    t3, 0(t0)
    sw    t3, 0(t1)
    addiu t0, t0, 4
    addiu t2, t2, -1
    bnez  t2, @@save
    addiu t1, t1, 4
    jal   0x802796A4            ; the list's font
    addiu a0, zero, 0x14
    jal   0x80279748            ; the group
    addiu a0, zero, 0x666
    bnez  s0, @@right
    addiu a0, zero, 435         ; over the right logo (512x384, y up, centred)
    addiu a0, zero, 77
@@right:
    addiu a1, zero, 346
    jal   0x80279670
    lui   a2, 0x3F80            ; z 1.0
    la    a0, OPP_TEXT
    addiu a1, zero, 0xA         ; centred
    jal   0x8027A374
    addiu a2, zero, -0x100      ; yellow
    lui   t0, 0x802B
    addiu t0, t0, -0x63B0
    addiu t1, sp, 0x10
    addiu t2, zero, 18
@@put:
    addiu t4, zero, 15          ; the list head (the 4th word) stays as it is now
    beq   t2, t4, @@next
    lw    t3, 0(t1)
    sw    t3, 0(t0)
@@next:
    addiu t0, t0, 4
    addiu t2, t2, -1
    bnez  t2, @@put
    addiu t1, t1, 4
    lw    ra, 0x64(sp)
    lw    s0, 0x60(sp)
    jr    ra
    addiu sp, sp, 0x68

OPP_TEXT: .ascii 0x1B, "%dj", 0x1B, "%dcCHOOSE OPPONENT"
        .byte 0
.align 4

; Once a frame (the first of the list's three reads; in_a, below): on the opened list's third frame, move_start.
first:
    la    t6, WOKEN
    lw    t7, 0(t6)
    beqz  t7, fix_port
    lw    t0, 4(t6)
    bne   t0, s5, fix_port
    lw    t1, 16(t6)
    addiu t1, t1, 1
    sw    t1, 16(t6)
    addiu t2, zero, 3
    bne   t1, t2, fix_port
    nop
    j     move_start
    nop

.if . > 0x8001CE70
.error "opponent.asm's first part is past guPerspectiveF"
.endif
.close

.create "opponent2.bin", 0x8001D160
.org 0x8001D160                 ; the picker (picker.asm, OPP_STATE) reads it here
WOKEN:  .word 0                 ; +0 the list is open, +4 CPU side, +8 TEAM picked, +12 SLOT the player's,
        .word -1                ; +16 FRAMES since it opened
        .word -1
        .word 0
        .word 0

; The opened list's player slot: the one across from the player's (its marker reads 3 for player 1), while its
; input reads are pointed back at the player (in_a/b/c).
setup:
    la    t6, WOKEN
    lw    t7, 0(t6)
    beqz  t7, @@done
    lw    t0, 4(t6)
    bne   t0, s5, @@done
    lw    t0, 12(t6)
    xori  s7, t0, 2
    addiu sp, sp, -0x18
    sw    ra, 0x14(sp)
    jal   label
    move  a0, s5
    lw    ra, 0x14(sp)
    addiu sp, sp, 0x18
@@done:
    lui   v1, hi(BUSY)
    jr    ra
    lhu   v1, lo(BUSY)(v1)

in_a:
    lui   t9, 0x8025
    b     first
    addiu t9, t9, 0xD5E0 - 0x10000
in_b:
    lui   t9, 0x8025
    b     fix_port
    addiu t9, t9, 0xD548 - 0x10000
in_c:
    lui   t9, 0x8025
    addiu t9, t9, 0xD57C - 0x10000
fix_port:                       ; the opened list reads the player's controller, then on to the game's read
    la    t6, WOKEN
    lw    t7, 0(t6)
    beqz  t7, @@go
    lw    t0, 4(t6)
    bne   t0, s5, @@go
    nop
    xori  a0, a0, 2
@@go:
    jr    t9
    nop

; The "please wait" panel of slot s1 leaves when the list on its side opens.
panel:
    lui   v0, hi(HUMANS)
    lw    v0, lo(HUMANS)(v0)
    la    t6, WOKEN
    lw    t7, 0(t6)
    beqz  t7, @@done
    lw    t0, 4(t6)             ; the CPU side
    slti  t1, s1, 2
    xori  t1, t1, 1             ; the slot's side
    bne   t0, t1, @@done
    addiu t1, zero, 1
    sllv  t1, t1, s1
    or    v0, v0, t1
@@done:
    jr    ra
    nop

; Team select waits for every running selection, and in a 1-player game for the opponent list's pick too.
hold:
    move  t9, ra
    la    t0, context
    jalr  t0, t8                ; armips: jalr target, link
    nop
    move  ra, t9
    lui   v0, hi(BUSY)
    bltz  t2, @@done
    lh    v0, lo(BUSY)(v0)
    la    t6, WOKEN
    lw    t7, 0(t6)
    sll   t0, t2, 2
    lui   t1, hi(CHOSEN)
    addu  t1, t1, t0
    lw    t1, lo(CHOSEN)(t1)    ; the CPU side's pick
    beqz  t7, @@wait
    addiu t0, zero, -1
    beq   t1, t0, @@wait
    nop
    b     @@done
    sw    t1, 8(t6)             ; TEAM: kept for restore
@@wait:
    addiu v0, zero, 1
@@done:
    jr    ra
    nop

; After the game's CPU block picked (and set its difficulty bookkeeping): the player's pick for the CPU.
restore:
    la    t6, WOKEN
    lw    t7, 0(t6)
    beqz  t7, @@go
    lw    t0, 8(t6)             ; TEAM
    bltz  t0, @@go
    lw    t1, 4(t6)             ; CPU
    sll   t1, t1, 2
    lui   t2, hi(CHOSEN)
    addu  t2, t2, t1
    sw    t0, lo(CHOSEN)(t2)
@@go:
    sw    zero, 0(t6)
    j     0x8027B950
    nop

.if . > 0x8001D300
.error "opponent.asm's second part runs into rules.asm's data"
.endif
.close

; The opened list's cursor (s6) moves to the game's kind of pick, rand(31) but not the player's team, and the QB
; re-dresses as after a move. Over func_800277F0 (boot, 0xC0 bytes), which nothing calls or names.
.create "opponent3.bin", 0x800277F0
.org 0x800277F0
move_start:
    addiu sp, sp, -0x20
    sw    ra, 0x1C(sp)
    sw    a0, 0x18(sp)
    sw    t9, 0x14(sp)
    jal   RAND_MOD
    addiu a0, zero, 31
    la    t6, WOKEN
    lw    t0, 4(t6)
    xori  t0, t0, 1
    sll   t0, t0, 2
    lui   t1, hi(CHOSEN)
    addu  t1, t1, t0
    lw    t1, lo(CHOSEN)(t1)    ; the player's team
    bne   v0, t1, @@pick
    addiu t2, zero, 30
    bne   v0, t2, @@pick
    addiu v0, v0, 1
    move  v0, zero
@@pick:
    move  s6, v0
    lui   t0, hi(REDRESSING)
    addiu t1, zero, 1
    sw    t1, lo(REDRESSING)(t0)
    move  a0, s5
    move  a1, s6
    jal   DRESS
    move  a2, zero
    lui   t0, hi(REDRESSING)
    sw    zero, lo(REDRESSING)(t0)
    move  a0, s6
    move  a1, s5
    jal   REDRAW
    addiu a2, zero, 1
    lw    ra, 0x1C(sp)
    lw    a0, 0x18(sp)
    lw    t9, 0x14(sp)
    j     fix_port
    addiu sp, sp, 0x20
.if . > 0x800278B0
.error "move_start is past func_800277F0"
.endif
.close
