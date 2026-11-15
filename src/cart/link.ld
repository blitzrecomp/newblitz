/* The cart season (src/cart), linked twice by tools/cart/build.sh (--defsym SEASON_BASE, SEASON_END):
 *   Lite: Season: 0x800E4700, the default heap's old top below the cart32 block (the export cuts the heap to 0x45C00),
 *                 so a plain 4 MB N64 runs it (docs/NO_EXPANSION_PAK.md);
 *   Full:         0x80400000, the Expansion Pak.
 * The entry at the base, the league block (cart_season.h CartLeague) at +0x40, code from +0x600. */
ENTRY(cs_entry)
SECTIONS {
    . = SEASON_BASE;
    .entry : { KEEP(*(.text.entry)) }
    . = SEASON_BASE + 0x40;
    .league : { KEEP(*(.league)) }
    . = SEASON_BASE + 0x600;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) *(.sdata*) *(.bss*) *(.sbss*) *(COMMON) }
    .got : { *(.got) }
    ASSERT(. <= SEASON_END, "the cart season is bigger than its room")
    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) *(.gnu.attributes) *(.mdebug*) }
}
