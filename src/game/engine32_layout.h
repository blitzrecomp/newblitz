// Where Modern Blitz keeps the original game's team tables, sized for 32 teams (engine32.cpp). The game's code is
// patched to read them here (tools/engine32_toml.py reads this file: every reference to a table's original address,
// both halves, moves by the same amount). The tables the code reaches directly share one 64 KB window below +0x8000,
// so every patched address load keeps one upper half. Rosters and title boxes are reached through each team record's
// pointers (+0x58, +0x60), which the encoder sets.
#pragma once
#include <cstdint>
#include <iterator>

namespace blitz::engine32 {
    constexpr int teams = 32;

    struct Table { const char* name; uint32_t original, entry, now; };
    // name, original address, entry size, new address
    constexpr Table tables[] = {
        {"team",     0x8033800C, 0x64, 0x80720000},  // records (ovl_b)
        {"select",   0x80338E68, 0x10, 0x80720C80},  // team select names (ovl_b)
        {"uni_home", 0x8028AAB0, 0x28, 0x80720E80},  // uniform parts, home (main)
        {"uni_away", 0x8028AF88, 0x28, 0x80721380},  // away (main)
        {"vs_left",  0x802836E4, 0x04, 0x80721880},  // VS helmet (main)
        {"vs_pause", 0x802AAFD8, 0x04, 0x80721900},  // VS call pause (main)
        {"win",      0x8033460C, 0x08, 0x80721980},  // win text (ovl_a)
        {"ez1",      0x80335428, 0x04, 0x80721A80},  // end zones (ovl_a): three arrays of one image per team
        {"ez2",      0x803354A4, 0x04, 0x80721B00},
        {"ez3",      0x80335520, 0x04, 0x80721B80},
        {"logo30",   0x80337280, 0x04, 0x80721C00},  // small logos (ovl_a)
        {"selector", 0x80333D48, 0x04, 0x80721C80},  // per-team selector (ovl_a)
        {"cpu_off",  0x80336E20, 0x1B, 0x80721D00},  // CPU offense plays (ovl_a)
        {"cpu_def",  0x80337168, 0x09, 0x80722060},  // CPU defense plays (ovl_a)
        // The VS screen's own right-side helmets: a list (team number, file id), not one entry per team; the
        // game scans 9 of its 10 entries (main 0x8024C680), the moved list 32.
        {"vsr_team", 0x802836AC, 0x02, 0x80722180},  // (main)
        {"vsr_id",   0x802836C0, 0x04, 0x80722200},  // (main)
    };
    constexpr int vs_right_entries = teams;  // in the moved list (the game's own: 10)
    constexpr uint32_t rosters = 0x80710000;      // 32 x 0x3C0
    constexpr uint32_t title_boxes = 0x80718000;  // 32 x 8

    constexpr uint32_t now(uint32_t original) {
        for (const Table& t : tables) if (t.original == original) return t.now;
        return original;
    }

    // Export ROM's 32-team cart (a plain N64, no Expansion Pak): the same tables, in the same order, in the last
    // 16 KB of the game's default heap (0x8009EB00, 0x53C00 bytes, which the export cuts: see heap_size), which a
    // boot loader fills from the ROM. One upper half (0x800F, carried) reaches all of it. Slots 0-30 keep their
    // rosters and title boxes in ovl_b; Houston's follow the tables, then the 32 x 32 jersey clash table and the
    // cart's code (tools/rom_picker/cart32.asm).
    namespace cart {
        constexpr uint32_t base = 0x800EE700, end = 0x800F2700;
        // The heap ends at the block (0x4FC00, was 0x53C00). Lite: Season's cart also puts the season below it
        // (src/cart/link.ld), so it needs no Expansion Pak, and its heap ends under that (0x45C00): that cart keeps
        // only the files it needs (Export ROM), as the N64 holds the file table in this heap. With a team on fire the
        // heap peaks near 216 KB there (docs/NO_EXPANSION_PAK.md). Audio stays at 0x800F2700.
        constexpr uint32_t season_lean = 0x800E4700;
        constexpr uint32_t heap_size = base - 0x8009EB00, heap_size_lean = season_lean - 0x8009EB00;
        static_assert(heap_size == 0x4FC00 && heap_size_lean == 0x45C00);
        constexpr uint32_t size_of(const Table& t) { return t.entry * (uint32_t)teams; }
        constexpr uint32_t table_at(size_t index) {
            uint32_t a = base;
            for (size_t i = 0; i < index; i++) a = (a + size_of(tables[i]) + 3) & ~3u;
            return a;
        }
        constexpr uint32_t tables_end = table_at(std::size(tables));
        constexpr uint32_t houston_roster = tables_end, houston_titles = houston_roster + 0x3C0;
        constexpr uint32_t clash = (houston_titles + 8 + 15) & ~15u;
        constexpr uint32_t code = clash + teams * teams;
        constexpr uint32_t now(uint32_t original) {
            for (size_t i = 0; i < std::size(tables); i++) if (tables[i].original == original) return table_at(i);
            return original;
        }
        static_assert(code < end - 0x800, "the cart's tables leave its code 2 KB");
    }
}
