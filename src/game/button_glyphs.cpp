// Each player's own controller in the game's button icons (both apps).
//
// tools/make_pad_glyphs.py draws our own icons for PlayStation, Xbox and Switch (by position) at the sizes
// and origins of the game's N64 ones (n64bttn*.wms), plus right-stick arrows for the C buttons, in
// assets/pad_glyphs (bundled in Resources/assets). At startup they're added to the game's file system as
// new files (~ps_a.wms ..., art_dropin.cpp fs_register_file), so every variant has its own TOC index and
// name: the game caches textures by name, and one screen can show several players' icons at once.
//
// Every button icon is a sprite made by func_8027785C (a0 = asset id 0x0A000000 | (index + 1)). Three
// hooks (blitz2000.toml, so both apps) swap the id for the owner's variant:
//   - func_80260620 (an action's button mask for a controller) records its controller (a0): the
//     play-select help overlay and Controller Setup call it for a controller right before drawing that
//     controller's buttons with func_80260768;
//   - func_80260768 (a row of button icons from the table at 0x8028BEFC) swaps for that controller;
//   - func_8027785C swaps the rest (menu footers and the like) for the menu owner: the last port that
//     pressed a button.
// docs/CONTENT_MAP.md "Button icons" has the screens and their owners.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "util/file.h"
#include "blitz_soft_reset.h"

namespace blitz {
    int port_glyph_family(int port);  // controllers.cpp: 0 PlayStation, 1 Xbox, 2 Switch, 3 N64 (the game's own)
    int menu_owner_port();            // controllers.cpp: the last port that pressed a button
    bool fs_register_file(const std::string& name, std::vector<uint8_t> data);  // art_dropin.cpp
    int32_t fs_file_index(const std::string& name);
    int32_t fs_rom_index(const std::string& name);
    uint32_t fs_ram_count(uint8_t* rdram);
    bool fs_restore_entry(uint8_t* rdram, int32_t index);
}

namespace {
    // The game's icons, in the order of our glyph names (tools/make_pad_glyphs.py GLYPHS).
    constexpr std::array<const char*, 9> game_files = {"n64bttna.wms", "n64bttnb.wms", "n64bttnz.wms", "n64bttnshl.wms", "n64bttnshr.wms",
                                                       "n64bttnup.wms", "n64bttndn.wms", "n64bttnlf.wms", "n64bttnrt.wms"};
    constexpr std::array<const char*, 9> glyph_names = {"a", "b", "z", "l", "r", "up", "dn", "lf", "rt"};
    constexpr std::array<const char*, 3> family_prefix = {"ps", "xb", "sw"};
    constexpr int families = 3;

    std::once_flag resolved;
    std::array<int32_t, 9> game_index{};                     // TOC index of each game icon (-1: not in this ROM)
    std::array<std::array<int32_t, 9>, families> variant{};  // TOC index of each family's icon (-1: none)
    int owner = -1;                                          // game thread: the controller of the current row

    // The file for a family's glyph: the C buttons share the right-stick arrows.
    std::string file_of(int fam, int g) {
        return std::string(g >= 5 ? "rs" : family_prefix[fam]) + "_" + glyph_names[g] + ".wms";
    }

    void resolve() {
        for (int g = 0; g < 9; g++) game_index[g] = blitz::fs_rom_index(game_files[g]);
        for (int f = 0; f < families; f++)
            for (int g = 0; g < 9; g++) variant[f][g] = blitz::fs_file_index(file_of(f, g));
    }

    // Restart in place: the next boot's file table may place the icons elsewhere.
    [[maybe_unused]] const bool glyphs_soft_reset = blitz::on_soft_reset("glyphs", [] {
        blitz::reset_once(resolved);
        owner = -1;
    });
}

namespace blitz {
    // At on_init, after the art drop-in, before fs_commit_files.
    void glyphs_init() {
        std::filesystem::path dir = recompui::file::get_program_path() / "assets" / "pad_glyphs";
        int n = 0;
        std::vector<std::string> done;
        for (int f = 0; f < families; f++) {
            for (int g = 0; g < 9; g++) {
                std::string file = file_of(f, g);
                if (std::find(done.begin(), done.end(), file) != done.end()) continue;
                done.push_back(file);
                std::ifstream in{dir / file, std::ios::binary};
                std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
                if (data.size() < 32 || data[0] != 'w' || data[1] != 'n') {
                    fprintf(stderr, "[glyphs] %s missing from %s\n", file.c_str(), dir.string().c_str());
                    continue;
                }
                if (fs_register_file(file, std::move(data))) n++;
            }
        }
        fprintf(stderr, "[glyphs] %d controller button icons\n", n);
    }
}

extern "C" {
    // func_80260620, before 0x80260624: a0 = the controller whose buttons the game is about to read.
    void blitz_glyph_owner(int32_t controller) {
        owner = controller;
    }

    // An icon's asset id -> the one for its owner's controller (per_player: the controller recorded by
    // blitz_glyph_owner; otherwise the menu owner). Anything that isn't one of the N64 icons is unchanged.
    int32_t blitz_glyph_id(uint8_t* rdram, int32_t id, int per_player) {
        if (((uint32_t)id >> 24) != 0x0A) return id;
        std::call_once(resolved, resolve);
        int32_t index = (int32_t)((uint32_t)id & 0xFFFFFF) - 1;
        int g = 0;
        while (g < 9 && game_index[g] != index) g++;
        if (g == 9 || index < 0) return id;
        // A save state from a build that pointed the game's icon entries at its own PlayStation art (past the
        // ROM end, where this build keeps other files): point them back at the game's own icons.
        if (blitz::fs_restore_entry(rdram, index)) fprintf(stderr, "[glyphs] %s: TOC entry from an older build restored\n", game_files[g]);
        int port = per_player ? owner : blitz::menu_owner_port();
        int fam = blitz::port_glyph_family(port);  // -1 (a column with no controller): the fallback
        if (fam < 0 || fam >= families) return id;  // N64: the game's own
        int32_t v = variant[fam][g];
        static const bool log = getenv("BLITZ_GLYPH_LOG") != nullptr;  // dev: which icon went to whom
        if (log) fprintf(stderr, "[glyphs] %s for %s %d: family %d\n", game_files[g], per_player ? "controller" : "menu owner", port + 1, fam);
        // A save state made before these files were added has the shorter TOC in RDRAM.
        if (v < 0 || (uint32_t)v >= blitz::fs_ram_count(rdram)) return id;
        return (int32_t)(0x0A000000u | (uint32_t)(v + 1));
    }
}
