; Export ROM options (src/game/export_rom.cpp): the standing Blitz codes and Home Field at the VS screen, and a
; kickoff's touchback spot. Lives over libultra's guPositionF and guPosition (boot 0x8001D040, 0x3C0 bytes),
; which the game links but never calls. Its data block is filled by the export.
;
; rules: main 0x8024B880 (`move s4, zero`, right after the VS screen clears the codes) becomes `jal rules`.
; touchback: ovl_a 0x80301870 (`li v0, 20`, the touchback spot) becomes `jal touchback`.
; Both callers saved ra in their prologues and reload what they need after the call.

.n64
.create "rules.bin", 0x8001D040

GAME_MODE  equ 0x8028B998   ; 2 = Season
CHOSEN1    equ 0x802EA438   ; the home (right) team
LAST_PLAY  equ 0x802EA50C   ; 1 after a kickoff

.org 0x8001D040
rules:
    move  s4, zero              ; the instruction the call replaced
    lui   t0, hi(GAME_MODE)
    lw    t0, lo(GAME_MODE)(t0)
    addiu t1, zero, 0xF         ; a code's flag: on for both teams
    la    t2, FLAGS
@@all:
    lw    t3, 0(t2)
    beqz  t3, @@gameplay
    addiu t2, t2, 4
    sw    t1, 0(t3)
    b     @@all
    nop
@@gameplay:                     ; gameplay codes: not in Season, as the game itself rules
    addiu t4, zero, 2
    beq   t0, t4, @@home
    nop
    la    t2, FLAGS_PLAY
@@play:
    lw    t3, 0(t2)
    beqz  t3, @@home
    addiu t2, t2, 4
    sw    t1, 0(t3)
    b     @@play
    nop
@@home:
    la    t2, HOME
    lbu   t3, 0(t2)             ; bit 0: set the field by stadium; bit 1: no weather under a roof
    beqz  t3, @@done
    nop
    lui   t5, hi(CHOSEN1)
    lw    t5, lo(CHOSEN1)(t5)
    sltiu t6, t5, 32
    beqz  t6, @@done
    nop
    addu  t6, t2, t5
    lbu   t6, 1(t6)             ; the team's stadium: bit 0 known, 1 turf, 2 roof
    andi  t7, t6, 1
    beqz  t7, @@done
    nop
    lui   t8, 0x8028
    andi  t7, t3, 1
    beqz  t7, @@weather
    nop
    andi  t7, t6, 2
    beqz  t7, @@grass
    nop
    b     @@weather
    sw    t1, 0x2F64(t8)        ; turf
@@grass:
    sw    t1, 0x2F60(t8)        ; grass
@@weather:
    andi  t7, t3, 2
    beqz  t7, @@done
    nop
    andi  t7, t6, 4
    beqz  t7, @@done
    nop
    sw    t1, 0x2F30(t8)        ; clear weather
@@done:
    jr    ra
    nop

touchback:
    lui   v0, hi(LAST_PLAY)
    lw    v0, lo(LAST_PLAY)(v0)
    addiu v0, v0, -1
    bnez  v0, @@other
    nop
    la    v0, TB_SPOT
    jr    ra
    lw    v0, 0(v0)
@@other:
    jr    ra
    addiu v0, zero, 20

.if . > 0x8001D160
.error "rules.asm runs into opponent.asm's part (0x8001D160)"
.endif
.org 0x8001D300
FLAGS:      .fill 0x60, 0       ; up to 23 code flags, 0-ended (any game)
FLAGS_PLAY: .fill 0x60, 0       ; up to 23 gameplay code flags, 0-ended (not in Season)
HOME:       .fill 0x24, 0       ; options byte, then 32 team stadiums
TB_SPOT:    .word 20
.org 0x8001D400
.close
