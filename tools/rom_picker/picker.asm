; Uniform Picker for exported Modern Blitz ROMs (Export ROM, src/game/export_rom.cpp): at team select, C-left
; (the app's △) steps the hovered team through the game's choice, its 1999 home and away, then its extra
; sets, and redresses the player. Runs on a real N64 or an emulator; no Expansion Pak needed.
;
; Where it lives: libultra's guLookAtF (boot 0x8001C0A0, 0x380 bytes), which the game links but never
; calls (no jal, no address of it anywhere); the boot loader copies it into RAM with the rest of boot.
; How it runs: team select's per-frame `jal process_sleep` (ovl_b 0x802F9C90, delay slot a0 = 1) becomes
; `jal picker`; the picker returns through process_sleep with a0 = 1, so the loop carries on as before.
; In that loop s5 is the side (0 left, 1 right), s6 the hovered team, s7 the side's player slot.
;
; A pick goes into both the home and the away slots of the team's block (ovl_b data), so the side wears it
; whatever the game decides at kickoff; ovl_b is reloaded from ROM after each game, so nothing lasts.
; Set data comes from a table appended to the ROM by the export (TABLE word below): per team 0x100 bytes,
; set k's 10 part ids (u16) at k * 0x18. COUNTS holds each team's number of sets.
;
; With -equ ORIG 1 (picker_orig.bin), the same for ROMs exported from Modern on the original game: each team's
; uniform is the main segment's two tables of 10 asset ids (home 0x8028AAB0, away 0x8028AF88, 0x28 per team). Main
; stays loaded, so a pick would outlast the game: team select's start (reset, chained to the call it displaced, as
; order_orig.asm's) puts back any team a side changed.
;
; Assemble: tools/rom_picker/build.sh (armips) -> src/game/rom_picker_code.inc.

.n64
.if ORIG
.create "picker_orig.bin", 0x8001C0A0
.else
.create "picker.bin", 0x8001C0A0
.endif

DMA_READ      equ 0x800016B8   ; (rom, ram, size): osInvalDCache, then PI DMA
PROCESS_SLEEP equ 0x80273CCC
DRESS         equ 0x80256580   ; (side, team, alt): builds the side's uniform
REDRAW        equ 0x802FA080   ; ovl_b (team, side, 1): the side's list, logo and QB model
BLOCKS_RAM    equ 0x80330E1C   ; team 0's uniform ids (block 0x80330C9C + 0x180)
BLOCKS_ROM    equ 0x0011F2FC   ; the same in ovl_b's ROM image
BLOCK_SIZE    equ 0x220
CLEFT         equ 0x0002
OPP_STATE     equ 0x8001D160   ; opponent.asm's WOKEN, CPU side (checked by build.sh)
UNI_HOME      equ 0x8028AAB0   ; the original game: main's uniform tables (asset ids)
UNI_AWAY      equ 0x8028AF88
UNI_HOME_ROM  equ 0x000717D0   ; the same in main's ROM image (0x2FC20 + offset)
UNI_AWAY_ROM  equ 0x00071CA8
ASSET         equ 0x0A00       ; an asset id's upper half

.org 0x8001C0A0
picker:
    addiu sp, sp, -0x30
    sw    ra, 0x2C(sp)
    sw    s0, 0x28(sp)
    sw    s1, 0x24(sp)
    sw    s2, 0x20(sp)
    sltiu t0, s5, 2
    beqz  t0, @@out
    nop
    ; the side still choosing (chosen[side] at 0x802EA434 is -1)
    sll   t1, s5, 2
    lui   t0, 0x802F
    addu  t0, t0, t1
    lw    t0, -0x5BCC(t0)
    addiu t1, zero, -1
    bne   t0, t1, @@out
    nop
    ; the controller in the side's player slot ([controller] slot at 0x8002AD94); on the opponent list
    ; (opponent.asm, its state zeroed when not exported) the player's, across from the list's own slot
    move  t5, s7
    la    t1, OPP_STATE
    lw    t2, 0(t1)
    beqz  t2, @@slot
    lw    t2, 4(t1)
    bne   t2, s5, @@slot
    nop
    xori  t5, s7, 2
@@slot:
    lui   t1, 0x8003
    addiu t1, t1, -0x526C
    move  t3, zero
@@find:
    lw    t2, 0(t1)
    beq   t2, t5, @@found
    nop
    addiu t3, t3, 1
    slti  t4, t3, 4
    bnez  t4, @@find
    addiu t1, t1, 4
    b     @@out
    nop
@@found:
    ; its buttons: the game's OSContPad array at 0x800311E0, 6 bytes each
    sll   t4, t3, 1
    addu  t4, t4, t3
    sll   t4, t4, 1
    lui   t5, 0x8003
    addu  t5, t5, t4
    lhu   t6, 0x11E0(t5)
    la    s0, VARS
    sll   t7, s5, 1
    addu  t7, s0, t7
    lhu   t8, 4(t7)            ; last frame's buttons
    sh    t6, 4(t7)
    nor   t8, t8, zero
    and   t6, t6, t8
    andi  s1, t6, CLEFT        ; s1: C-left pressed this frame
    ; the cursor moved: the team left behind gets its own uniform back
    addu  s2, s0, s5           ; s2: &VARS[side] (last team; +2 the set)
    lb    t8, 0(s2)
    beq   t8, s6, @@same
    nop
    lb    t9, 2(s2)
    sb    s6, 0(s2)
    sb    zero, 2(s2)
    bltz  t8, @@same
    nop
    beqz  t9, @@same
    nop
    jal   restore
    move  a0, t8
@@same:
    beqz  s1, @@out
    nop
    la    t0, COUNTS
    addu  t0, t0, s6
    lbu   t1, 0(t0)
    beqz  t1, @@out
    nop
    lb    t2, 2(s2)
    addiu t2, t2, 1
    slt   t3, t1, t2           ; past the last set: back to the game's choice
    beqz  t3, @@keep
    nop
    move  t2, zero
@@keep:
    sb    t2, 2(s2)
    beqz  t2, @@default
    nop
    move  a0, s6
    jal   apply
    addiu a1, t2, -1
    b     @@dress
    nop
@@default:
    jal   restore
    move  a0, s6
@@dress:
    lui   t0, 0x8034           ; 0x8033BAB0: set while team select re-dresses a model
    addiu t1, zero, 1
    sw    t1, -0x4550(t0)
    move  a0, s5
    move  a1, s6
    jal   DRESS
    move  a2, zero
    lui   t0, 0x8034
    sw    zero, -0x4550(t0)
    move  a0, s6
    move  a1, s5
    jal   REDRAW
    addiu a2, zero, 1
@@out:
    lw    ra, 0x2C(sp)
    lw    s0, 0x28(sp)
    lw    s1, 0x24(sp)
    lw    s2, 0x20(sp)
    addiu sp, sp, 0x30
    j     PROCESS_SLEEP
    addiu a0, zero, 1

.if ORIG
; restore and reset: the second and third parts, below
.else
; restore(team): the team's 20 ids (home and away of 10 parts) from ovl_b's ROM image
restore:
    addiu sp, sp, -0x20
    sw    ra, 0x1C(sp)
    sw    s0, 0x18(sp)
    sll   t0, a0, 5
    sll   t1, a0, 9
    addu  s0, t0, t1           ; s0: team * 0x220
    li    t1, BLOCKS_ROM
    addu  a0, s0, t1
    la    a1, BUF
    jal   DMA_READ
    addiu a2, zero, 40
    li    t1, BLOCKS_RAM
    addu  t1, t1, s0
    la    t2, BUF
    addiu t3, zero, 20
@@copy:
    lhu   t4, 0(t2)
    sh    t4, 0(t1)
    addiu t2, t2, 2
    addiu t3, t3, -1
    bnez  t3, @@copy
    addiu t1, t1, 2
    lw    ra, 0x1C(sp)
    lw    s0, 0x18(sp)
    jr    ra
    addiu sp, sp, 0x20

.endif

; apply(team, set): the set's 10 ids from the table into the team's home and away slots
apply:
    addiu sp, sp, -0x20
    sw    ra, 0x1C(sp)
    sw    s0, 0x18(sp)
.if ORIG
    sll   t0, a0, 3
    sll   t1, a0, 5
    addu  s0, t0, t1           ; s0: team * 0x28
.else
    sll   t0, a0, 5
    sll   t1, a0, 9
    addu  s0, t0, t1           ; s0: team * 0x220
.endif
    sll   t0, a0, 8            ; team * 0x100
    sll   t1, a1, 3
    sll   t2, a1, 4
    addu  t1, t1, t2           ; set * 0x18
    la    t2, TABLE
    lw    t2, 0(t2)
    addu  a0, t2, t0
    addu  a0, a0, t1
    la    a1, BUF
    jal   DMA_READ
    addiu a2, zero, 24
.if ORIG
    la    t5, UNI
    lw    t1, 0(t5)            ; the home table
    lw    t6, 8(t5)            ; away - home
    addu  t1, t1, s0
    addu  t5, t1, t6
    la    t2, BUF
    addiu t3, zero, 10
@@part:
    lhu   t4, 0(t2)
    lui   t6, ASSET
    or    t4, t4, t6
    sw    t4, 0(t1)
    sw    t4, 0(t5)
    addiu t2, t2, 2
    addiu t1, t1, 4
    addiu t3, t3, -1
    bnez  t3, @@part
    addiu t5, t5, 4
.else
    li    t1, BLOCKS_RAM
    addu  t1, t1, s0
    la    t2, BUF
    addiu t3, zero, 10
@@part:
    lhu   t4, 0(t2)
    sh    t4, 0(t1)
    sh    t4, 2(t1)
    addiu t2, t2, 2
    addiu t3, t3, -1
    bnez  t3, @@part
    addiu t1, t1, 4
.endif
    lw    ra, 0x1C(sp)
    lw    s0, 0x18(sp)
    jr    ra
    addiu sp, sp, 0x20

.if . > 0x8001C380
.error "picker.asm runs into its data"
.endif
.org 0x8001C380
COUNTS:   .fill 32, 0          ; filled by the export (32 teams on the 32-team cart)
VARS:     .byte 0xFF, 0xFF     ; last team per side (-1: none)
          .byte 0, 0           ; set per side (0: the game's choice)
          .halfword 0, 0       ; last frame's buttons per side
TABLE:    .word 0              ; ROM address of the set table, filled by the export
.if ORIG
NEXT:     .word 0x8024AEA4     ; the call reset displaced (the export sets it)
UNI:      .word UNI_HOME, UNI_HOME_ROM, UNI_AWAY - UNI_HOME  ; the uniform tables (RAM, ROM, away - home): the
                               ; game's own, or the 32-team cart's (the export sets them)
.endif
.align 16
BUF:      .fill 48, 0
.org 0x8001C420
.close

.if ORIG
; restore(team): the team's home and away ids from main's ROM image. Over _frexpf (boot 0x80019A58, 0xE0 bytes),
; which nothing calls or names.
.create "picker_orig2.bin", 0x80019A58
.org 0x80019A58
restore:
    addiu sp, sp, -0x20
    sw    ra, 0x1C(sp)
    sw    s0, 0x18(sp)
    sw    s1, 0x14(sp)
    sll   t0, a0, 3
    sll   t1, a0, 5
    addu  t0, t0, t1           ; team * 0x28
    la    t1, UNI
    lw    s1, 4(t1)            ; the home table in the ROM
    addu  s1, s1, t0
    addiu s0, zero, 2          ; home, then away (UNI + 8 further on in both)
@@table:
    move  a0, s1
    la    a1, BUF
    jal   DMA_READ
    addiu a2, zero, 40
    la    t1, UNI
    lw    t5, 0(t1)
    lw    t1, 4(t1)
    subu  t1, t5, t1           ; RAM - ROM
    addu  t1, t1, s1
    la    t2, BUF
    addiu t3, zero, 10
@@copy:
    lw    t4, 0(t2)
    sw    t4, 0(t1)
    addiu t2, t2, 4
    addiu t3, t3, -1
    bnez  t3, @@copy
    addiu t1, t1, 4
    addiu s0, s0, -1
    la    t1, UNI
    lw    t1, 8(t1)
    bnez  s0, @@table
    addu  s1, s1, t1
    lw    ra, 0x1C(sp)
    lw    s0, 0x18(sp)
    lw    s1, 0x14(sp)
    jr    ra
    addiu sp, sp, 0x20
.if . > 0x80019B38
.error "restore is past _frexpf"
.endif
.close

; Team select starts: the teams the sides changed get theirs back; then on to the call the site made. Over osViFade
; (boot 0x8001F890, 0x80 bytes), which nothing calls or names.
.create "picker_orig3.bin", 0x8001F890
.org 0x8001F890
reset:
    addiu sp, sp, -0x20
    sw    ra, 0x1C(sp)
    sw    s0, 0x18(sp)
    la    s0, VARS
@@side:
    lb    a0, 0(s0)
    lb    t0, 2(s0)
    addiu t1, zero, -1
    sb    t1, 0(s0)
    bltz  a0, @@next
    sb    zero, 2(s0)
    beqz  t0, @@next
    nop
    jal   restore
    nop
@@next:
    la    t0, VARS + 1
    bne   s0, t0, @@side
    addiu s0, s0, 1
    lw    ra, 0x1C(sp)
    lw    s0, 0x18(sp)
    addiu sp, sp, 0x20
    la    t0, NEXT
    lw    t0, 0(t0)
    jr    t0
    nop
.if . > 0x8001F910
.error "reset is past osViFade"
.endif
.close
.endif
