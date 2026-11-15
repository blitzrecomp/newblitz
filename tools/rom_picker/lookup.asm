; File lookup by name for files an exported ROM adds (Export ROM, src/game/export_rom.cpp): the game finds a file
; by binary search over its TOC (boot func_80001C2C), so a file appended past the sorted names (your own announcer
; calls for the Commanders or Texans) is never found. When the search misses, this scans the whole TOC once.
; The app does the same with hooks (tools/teamplus.py DROPIN).
;
; Where it lives: libultra's guMtxXFML (boot 0x8001C9B4, 0x8C bytes), which the game links but never calls.
; The site: boot 0x80001CDC `beqz s3, not_found` becomes `jal scan` (its delay slot `addu v0, zero, zero` runs as
; before). On return s3 = 1 and s0 = the index (found, by the search or here), or s0 = -1, which the next
; instruction (`bltz s0`) takes to the not-found return. The caller's name (lower-cased) is at its sp + 0x10.
; Two builds: team+'s 32-byte 'zs' entries (lookup.bin) and, with -equ FS40 1, the original game's 40-byte 'fs'
; entries (lookup40.bin: Modern's export from the original game).

.n64
.if FS40
.create "lookup40.bin", 0x8001C9B4
.else
.create "lookup.bin", 0x8001C9B4
.endif

TOC_HDR equ 0x8005BD88   ; the loaded header: +0xC the number of entries
TOC     equ 0x8005BDF4   ; the loaded TOC: 32-byte entries in team+ ('zs'), 40 in the original ('fs'); the name first
COMPARE equ 0x80026E20   ; (name, entry name) -> 0 when equal

.org 0x8001C9B4
scan:
    bnez  s3, @@quick           ; the search found it
    nop
    addiu sp, sp, -0x18
    sw    ra, 0x14(sp)
    lui   t0, hi(TOC_HDR)
    lw    t0, lo(TOC_HDR)(t0)
    lw    s2, 0xC(t0)           ; s2: the count (the caller is done with s1 / s2)
    move  s1, zero
@@next:
    beq   s1, s2, @@none
    addiu s0, zero, -1
    lui   t0, hi(TOC)
    lw    t0, lo(TOC)(t0)
.if FS40
    sll   a1, s1, 5             ; the original's 40-byte entries: 32 + 8
    sll   t1, s1, 3
    addu  a1, a1, t1
.else
    sll   a1, s1, 5             ; team+'s 32-byte entries
.endif
    addu  a1, a1, t0
    jal   COMPARE
    addiu a0, sp, 0x18 + 0x10
    beqz  v0, @@found
    move  s0, s1
    b     @@next
    addiu s1, s1, 1
@@found:
    addiu s3, zero, 1
@@none:
    lw    ra, 0x14(sp)
    addiu sp, sp, 0x18
@@quick:
    jr    ra
    addu  v0, zero, zero
.if . > 0x8001CA40
.error "lookup.asm is past guMtxXFML"
.endif
.close
