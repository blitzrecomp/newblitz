; Houston on the cart: 32 teams on a plain N64 (Export ROM, src/game/export_rom.cpp), as Modern on the original game
; does in the app (src/game/engine32.cpp). The export moves the game's sixteen directly read team tables to 32-team
; copies at the end of the default heap (engine32_layout.h cart: the heap is cut by 16 KB) by patching their 180
; address loads, and appends that 16 KB block (tables, Houston's roster and title box, the 32 x 32 jersey clash table,
; and the code below) to the ROM. Boot loads it.
;
; cart32_boot.bin, over osPfsIsPlug (boot 0x80024EC0, which the game never calls; order_orig.asm's place on 31-team
; carts): the loader. boot 0x800014A8 `jal 80004A78` (after main is loaded and the file system mounted) -> jal load.
; It also loads the season (src/cart, tools/cart/build.sh) and calls it: below the block (Lite: Season) or, with an
; Expansion Pak, at 0x80400000 (Full).
;
; cart32.bin, in the loaded block (CODE, engine32_layout.h cart::code):
;   team select in name order over 32 (as order_orig.asm, whose sites it takes; the game's own steps stay 31-based)
;   802FA504  slti s1, s1, 0x1F -> 0x20 (the export: a row below may be the 32nd team)
;   802F5564  lbu v0, 0(v0)     -> j clash   (the game's 31 x 31 clash read; the delay slot's lui at stays)
; Only t3-t9 are used at the order sites, t0-t3 at the clash.

.n64

DMA_READ      equ 0x800016B8   ; (rom, ram, size)
INVAL_ICACHE  equ 0x8001B830   ; osInvalICache(addr, size)
AFTER_LOAD    equ 0x80004A78   ; the call the boot site made
TEAMS         equ 32
LAST          equ 30           ; the game's own list: 0..30

.create "cart32_boot.bin", 0x80024EC0
.org 0x80024EC0
load:
    addiu sp, sp, -0x20
    sw    ra, 0x1C(sp)
    la    t0, BLOCK
    lw    a0, 0(t0)            ; the block in the ROM
    lw    a1, 4(t0)            ; its place (BASE)
    jal   DMA_READ
    lw    a2, 8(t0)            ; its size
    la    t0, BLOCK
    lw    a0, 4(t0)
    jal   INVAL_ICACHE
    lw    a1, 8(t0)
    ; The season (src/cart): Lite: Season's at 0x800E4700 (the default heap's old top) on any N64, Full's at
    ; 0x80400000 only with an Expansion Pak (osMemSize, 0x80000318, 8 MB); without one the game's own Season plays.
    la    t0, SEASON
    lw    a0, 0(t0)
    beqz  a0, @@done           ; no season on this cart
    lw    a1, 4(t0)            ; its place
    lui   t1, 0x8040
    sltu  t1, a1, t1
    bnez  t1, @@load           ; below the Expansion Pak: always
    lui   t2, 0x8000
    lw    t2, 0x318(t2)
    lui   t1, 0x0080
    sltu  t2, t2, t1
    bnez  t2, @@done           ; in the Expansion Pak, and there's none
    nop
@@load:
    jal   DMA_READ
    lw    a2, 8(t0)
    la    t0, SEASON
    lw    a0, 4(t0)
    jal   INVAL_ICACHE
    lw    a1, 8(t0)
    la    t0, SEASON
    lw    t0, 4(t0)
    jalr  t0                   ; cart_init (cs_entry)
    nop
@@done:
    lw    ra, 0x1C(sp)
    addiu sp, sp, 0x20
    j     AFTER_LOAD
    nop
.align 4
BLOCK:  .word 0, 0, 0          ; ROM address, RAM address, size (the export sets them)
SEASON: .word 0, 0, 0          ; the same for the season (0 = none)
.if . > 0x8002503C
.error "cart32's loader is past osPfsIsPlug"
.endif
.close

.create "cart32.bin", CODE
.org CODE
; t3 a slot, t4 the direction (+-1) -> t3 the next slot that way in name order. Linked t9.
step:
    sltiu t5, t3, TEAMS
    bnez  t5, @@in
    nop
    move  t3, zero
@@in:
    la    t5, POS
    addu  t5, t5, t3
    lbu   t6, 0(t5)
    addu  t6, t6, t4
    bgez  t6, @@low
    nop
    addiu t6, t6, TEAMS
@@low:
    slti  t5, t6, TEAMS
    bnez  t5, @@pick
    nop
    addiu t6, t6, -TEAMS
@@pick:
    la    t5, ORDER
    addu  t5, t5, t6
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

; The game stepped s6 by one in its own order (up: was < 30 ? was + 1 : 0; down: was > 0 ? was - 1 : 30; from Houston,
; 31, the same rules give 0 and 30), or jumped: a step goes to the next team in name order, a jump stays.
moved:
    la    t7, CUR
    sltiu t5, s5, 2
    beqz  t5, @@done
    addu  t7, t7, s5
    lb    t3, 0(t7)             ; where the cursor was (-1: nowhere yet)
    bltz  t3, @@take
    slti  t5, t3, LAST
    addiu t6, t3, 1
    bnez  t5, @@up
    nop
    move  t6, zero
@@up:
    beq   t6, s6, @@step
    addiu t4, zero, 1
    addiu t6, t3, -1
    bgtz  t3, @@down
    nop
    addiu t6, zero, LAST
@@down:
    bne   t6, s6, @@take
    addiu t4, zero, -1
@@step:
    la    t8, step
    jalr  t8, t9                ; armips: jalr target, link
    nop
    move  s6, t3
@@take:
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

; This game's jersey clash (ovl_b func_802F5360's end): the two teams picked (0x802EA434, 0x802EA438) in the 32 x 32
; table; at is the game's lui for its store (the jump's delay slot), ra still the caller's.
clash:
    lui   t0, 0x802F
    lw    t1, -0x5BCC(t0)
    lw    t2, -0x5BC8(t0)
    andi  t1, t1, TEAMS - 1
    andi  t2, t2, TEAMS - 1
    sll   t1, t1, 5
    addu  t1, t1, t2
    lui   t3, hi(CLASH_TABLE)
    addu  t3, t3, t1
    lbu   v0, lo(CLASH_TABLE)(t3)
    jr    ra
    sb    v0, 0x2182(at)

.align 4
NEXT:    .word 0x8024AEA4       ; the call reset displaced (the export sets it)
CUR:     .byte -1, -1           ; each list's cursor slot, as last shown
ORDER:   .fill TEAMS, 0         ; position -> slot (the export fills it)
POS:     .fill TEAMS, 0         ; slot -> position
.if . > END
.error "cart32.asm is past the loaded block"
.endif
.close
