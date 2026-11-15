// Overlay registration. Blitz keeps three code overlays at the same VRAM (0x802EFE70) and
// loads them through dma_read in 16 KB chunks. The runtime only knows a section is loaded
// when told with the whole range, so we tell it here, using the game's own load requests.
#include <cstdio>
#include <iterator>

#include "blitz_hooks.h"

extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size);
extern "C" void unload_overlays(int32_t ram_addr, uint32_t size);

namespace {
    struct CodeSegment {
        const char* name;
        uint32_t rom_start;
        uint32_t rom_end;
        uint32_t vram;
    };

    // Same table as tools/gen_syms.py SEGMENTS (NOTES.md "ROM map").
    constexpr CodeSegment segments[] = {
        { "main",  0x0002FC20, 0x00093EC0, 0x80248F00 },
        { "ovl_a", 0x00093EC0, 0x000DE350, 0x802EFE70 },
        { "ovl_b", 0x000DE350, 0x00129A90, 0x802EFE70 },
        { "ovl_c", 0x00129A90, 0x0012E860, 0x802EFE70 },
    };

    // Largest overlay image (ovl_b, 0x4B740 bytes) rounded up: anything inside this window
    // at 0x802EFE70 is an overlay being replaced.
    constexpr int32_t overlay_vram = (int32_t)0x802EFE70;
    constexpr uint32_t overlay_window = 0x50000;

    // Stadium blobs: the stadium's texture-loader function followed by its display lists and
    // vertices. ovl_a 0x803244AC DMAs the whole blob (rom_start..rom_end) to 0x80377D20; only the
    // code at its start is a recompiled section (ovl_stadium1-5 in tools/gen_syms.py).
    constexpr CodeSegment stadium_blobs[] = {
        { "stadium1", 0x0012E860, 0x001348D0, 0x80377D20 },
        { "stadium2", 0x001348D0, 0x0013BEB0, 0x80377D20 },
        { "stadium3", 0x0013BEB0, 0x00142B30, 0x80377D20 },
        { "stadium4", 0x00142B30, 0x00148C60, 0x80377D20 },
        { "stadium5", 0x00148C60, 0x00150490, 0x80377D20 },
    };
    constexpr int32_t stadium_vram = (int32_t)0x80377D20;
    constexpr uint32_t stadium_code_window = 0x410;  // largest loader (stadium3)

    // What is loaded now (indexes into the tables above; -1 none), for save states.
    int loaded_overlay = -1, loaded_stadium = -1;
}

extern "C" void blitz_overlay_current(int* segment, int* stadium) {
    *segment = loaded_overlay;
    *stadium = loaded_stadium;
}

// Save-state restore: register the snapshot's overlay and stadium code again (RDRAM already holds it).
extern "C" void blitz_overlay_load(int segment, int stadium) {
    if (segment != loaded_overlay && segment >= 1 && segment < (int)std::size(segments)) {
        const CodeSegment& seg = segments[segment];
        unload_overlays(overlay_vram, overlay_window);
        load_overlays(seg.rom_start, (int32_t)seg.vram, seg.rom_end - seg.rom_start);
        loaded_overlay = segment;
    }
    if (stadium != loaded_stadium && stadium >= 0 && stadium < (int)std::size(stadium_blobs)) {
        const CodeSegment& blob = stadium_blobs[stadium];
        unload_overlays(stadium_vram, stadium_code_window);
        load_overlays(blob.rom_start, stadium_vram, blob.rom_end - blob.rom_start);
        loaded_stadium = stadium;
    }
    fprintf(stderr, "[blitz] save state: overlay %d, stadium %d registered\n", segment, stadium);
}

extern "C" void blitz_on_code_dma(uint8_t*, uint32_t rom, int32_t ram, uint32_t size) {
    for (const auto& blob : stadium_blobs) {
        if (rom != blob.rom_start || ram != stadium_vram) {
            continue;
        }
        if (size != blob.rom_end - blob.rom_start) {
            fprintf(stderr, "[blitz] %s DMA with unexpected size 0x%X (expected 0x%X)\n",
                blob.name, size, blob.rom_end - blob.rom_start);
        }
        unload_overlays(stadium_vram, stadium_code_window);
        load_overlays(rom, ram, size);
        loaded_stadium = (int)(&blob - stadium_blobs);
        fprintf(stderr, "[blitz] loaded %s (ROM 0x%X -> 0x%08X)\n", blob.name, rom, (uint32_t)ram);
        return;
    }
    for (const auto& seg : segments) {
        if (rom != seg.rom_start || ram != (int32_t)seg.vram) {
            continue;
        }
        if (size != seg.rom_end - seg.rom_start) {
            fprintf(stderr, "[blitz] %s DMA with unexpected size 0x%X (expected 0x%X)\n",
                seg.name, size, seg.rom_end - seg.rom_start);
        }
        if (ram == overlay_vram) {
            unload_overlays(overlay_vram, overlay_window);
        }
        load_overlays(rom, ram, seg.rom_end - seg.rom_start);
        if (ram == overlay_vram) loaded_overlay = (int)(&seg - segments);
        fprintf(stderr, "[blitz] loaded %s (ROM 0x%X -> 0x%08X)\n", seg.name, rom, (uint32_t)ram);
        return;
    }
}
