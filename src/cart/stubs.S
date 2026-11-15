# Small handlers the cart season's patches jump to (src/cart/season.c), and the entry the boot loader calls.
.set noreorder
.set noat

.section .text.entry, "ax"
.globl cs_entry
cs_entry:                       # 0x800E4700: the boot loader's jal (tools/rom_picker/cart32.asm)
    j     cart_init
    nop

.text
# League leaders (ovl_b 802FFED8, by j; its delay slot `addiu v1, v1, 0x1C` has run): team 31's stats are at record
# +0x704 (0x802EFDF4), not +0x368 where the game's 28-byte rows would put them. a3 the team, t1 the stat's offset.
.globl cs_fix_leaders_a
cs_fix_leaders_a:
    addiu $at, $zero, 31
    bne   $a3, $at, 1f
    nop
    lui   $v0, 0x802F
    addiu $v0, $v0, -0x20C
    addu  $v0, $v0, $t1
1:  lwc1  $f0, 0($v0)
    j     0x802FFEE0
    nop

# The same at ovl_b 802FFF88 (delay slot `addiu a1, a1, 1`): v1 the team, a2 the stat's offset.
.globl cs_fix_leaders_b
cs_fix_leaders_b:
    addiu $at, $zero, 31
    bne   $v1, $at, 1f
    nop
    lui   $v0, 0x802F
    addiu $v0, $v0, -0x20C
    addu  $v0, $v0, $a2
1:  lwc1  $f0, 0($v0)
    j     0x802FFF90
    nop

# Standings screen division menu (ovl_b 802FF480, in place of `jal 80279670`; its delay slot `addiu s1, s1, 1` has
# run, so the entry is s1 - 1): AFC East..West 220 down from 220 in 14 px steps, the NFC's from 98; x 70, z 96.0.
.globl cs_menu_pos
cs_menu_pos:
    addiu $t0, $s1, -1
    slti  $t1, $t0, 4
    bnez  $t1, 1f
    addiu $a1, $zero, 220
    addiu $t0, $t0, -4
    addiu $a1, $zero, 98
1:  sll   $t2, $t0, 4
    sll   $t3, $t0, 1
    subu  $t2, $t2, $t3          # 14 * row
    subu  $a1, $a1, $t2
    addiu $a0, $zero, 70
    j     0x80279670
    lui   $a2, 0x42C0            # 96.0f

# Team schedule screen (ovl_b 80301AF8, in place of `subu s2, t0, t1`, by jal; its delay slot `addu s0, zero, zero`
# has run): weeks 1-11 in the left column, 12-22 on the right. s4 the week.
.globl cs_schedule_column
cs_schedule_column:
    slti  $at, $s4, 11
    beqz  $at, 1f
    nop
    addiu $s6, $zero, 0x87
    addiu $fp, $zero, 0x1E
    b     2f
    move  $t2, $s4
1:  addiu $s6, $zero, 0x18B
    addiu $fp, $zero, 0x10C
    addiu $t2, $s4, -11
2:  sll   $t1, $t2, 4
    subu  $t1, $t1, $t2          # 15 * row
    jr    $ra
    subu  $s2, $t0, $t1

# The season record's reset (main 80263AEC, in place of `lw ra, 0x14(sp)`, by j; its delay slot `lw s0, 0x10(sp)` has
# run): the cart season's own reset, then the function's epilogue.
.globl cs_new_season_tail
cs_new_season_tail:
    addiu $sp, $sp, -0x20
    sw    $v0, 0x10($sp)
    jal   cs_new_season
    nop
    lw    $v0, 0x10($sp)
    addiu $sp, $sp, 0x20
    lw    $ra, 0x14($sp)
    jr    $ra
    addiu $sp, $sp, 0x18
