#include <chrono>
// Uniform picker (Settings → Rules → Uniform Picker): at team select, △ / Y (the N64's C-left; on a phone the
// UNIFORM button the touch controls show at team select) cycles the side's
// uniform for the coming game: game decides → home → away → the team's extra sets (Modern Blitz). The QB model
// re-dresses on the spot and a toast says which; at kickoff the choice replaces the game's clash rule for that side,
// and a side left on "game decides" follows it so the two don't clash. Same addresses in both apps.
//
// How the game dresses players: func_80256580(side, team, set) loads a team's 10 uniform parts from its block
// (+0x180 + 4 * part + 2 * set: 0 home, 1 away) into the side's texture buffers; at kickoff ovl_a func_80302ED4
// calls func_80256850(team0, set0, team1, set1), giving the alternate set to one side when the home jerseys clash
// (D_802E2182). Hooked just before that call (0x80302F40), with a1 / a3 holding the sets.
//
// Extra sets (Modern Blitz; tools/import_uniform_sets.py): <app folder>/uniforms/<TEAM>/<set>/ holds set.json
// ({"name", "dark"}) and up to 10 part images (helmet_side.wms ... numbers.wms), registered as added game files at
// startup. Picking one lends it the team block's away slots (the old ids kept and put back), and the side then
// wears "away": the confirm copy (ovl_b 0x802F5360) carries the block into the game. The blocks are ovl_b data,
// reloaded with every team select, so nothing outlasts the game.
#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "recomp.h"
#include "recompui/config.h"
#include "librecomp/game.hpp"
#include "util/file.h"
#include "blitz_savestate.h"
#include "team_store.h"
#include "season_pak.h"

extern "C" {
    void func_80256580(uint8_t* rdram, recomp_context* ctx);  // a side's QB model: (side, team, set)
    void ovl_b_func_802FA080(uint8_t* rdram, recomp_context* ctx);  // a side's list, logo, ratings, QB record
}

extern "C" int blitz_opp_choosing_side();  // opponent_select.cpp
#if BLITZ_MODERN
extern "C" void blitz_crew_kickoff(uint8_t* rdram, int32_t team0, int32_t team1, int32_t set0, int32_t set1);  // crew.cpp
extern "C" void blitz_mixed_kickoff(uint8_t* rdram, recomp_context* ctx, int32_t team0, int32_t team1);  // mixed_uniforms.cpp
#endif
extern "C" int blitz_opp_choosing_port();
extern "C" void blitz_team_select_frame();  // savestate.cpp

namespace blitz {
#if BLITZ_MODERN
    bool original_game();  // adv_mode.cpp: Original 1999
#endif
    bool get_input(int controller_num, uint16_t* buttons, float* x, float* y);  // dev_input.cpp
    void inject_press(int port, uint16_t buttons, int ms);
    extern std::atomic<bool> crews_changed;      // roster_room.cpp
    void lineup_overlay_now(uint8_t* rdram);    // content_pack_tp.cpp
    void post_status(const std::string& msg);  // notices.cpp: a toast
    void hints_mark_team_select();             // hints.cpp
    uint8_t* monitor_rdram();                  // dev_monitor.cpp
#if BLITZ_MODERN
    bool fs_register_file(const std::string& name, std::vector<uint8_t> data);  // art_dropin.cpp: added game files
    int32_t fs_file_index(const std::string& name);
    uint32_t fs_ram_count(uint8_t* rdram);
    std::vector<std::string> team_franchises();  // content_pack_tp.cpp: franchise id of each team index
    int team_of_slot(int slot);                         // content_pack_tp.cpp: the team in a game slot
    bool league_active();  // content_pack_tp.cpp: a league pak (its teams sit on NFL blocks, so no NFL sets)
    std::string active_pack_id();  // content_pack_tp.cpp
    std::string probowl_code(int side, bool human);  // crew.cpp
    bool probowl_code_saved(int side);
    void probowl_ask_code(int side);
    int probowl_ask_state(int side);  // 1 asking, 2 typed, 3 cancelled
    void crew_hover(int side, int slot);  // crew.cpp: headshots for the team a cursor rests on
#endif
}

namespace {
    constexpr uint16_t button = 0x0002;          // N64 C-left: △ / Y by default (Turbo, tried 2026-10-22, didn't take)
    constexpr uint32_t busy = 0x8033BAB0;        // set while team select re-dresses a model
    constexpr uint32_t chosen = 0x802EA434;      // [side] team, -1 while that side is still choosing
    constexpr uint32_t attract_phase = 0x802EA420; // 7 during the attract demo (opponent_select.cpp)
    constexpr uint32_t slot_of_pad = 0x8002AD94; // [controller] the player slot it sits in (controller select)
    constexpr uint32_t game_mode = 0x8028B998;   // 0 Arcade, 1 Tournament, 2 Season, 3 Play Editor

    // set: -1 game decides, 0 home, 1 away, 2 + k the team's extra set k.
    struct State {
        int32_t set[2] = {-1, -1};
        int32_t team[2] = {-1, -1};  // the team the model last showed
        uint16_t held[2] = {0, 0};
        int32_t redress[2] = {0, 0}; // frames until the model is re-dressed after a move
        int32_t dark[2] = {0, 0};    // the picked extra set has a colour jersey
        int32_t lent_team[2] = {-1, -1};  // the block whose away slots hold this side's extra set
        uint16_t lent[2][10] = {};        // and the ids they held
        int32_t auto_key = -1;            // Random: the matchup (team0 * 64 + team1) its picks are for
        int32_t auto_pick[2] = {-1, -1};  // and the picks (as set numbers: 0 home, 1 away, 2 + k extra)
    } g;
    [[maybe_unused]] const bool registered = blitz::savestate_register_pod("uniform_picker", &g);

    constexpr const char* part_names[10] = {"helmet_side", "helmet_stripe_front", "helmet_stripe_back", "facemask", "shoulder",
                                            "jersey", "pant_stripe", "pants", "socks", "numbers"};
    struct ExtraSet {
        std::string id;      // the folder name without the importer's order prefix: home_2026, open_in_orange …
        std::string name;
        bool dark = true;
        std::array<std::string, 10> files;  // added file names; "" keeps the team's away part
    };
    std::map<std::string, std::vector<ExtraSet>> extra_sets;  // franchise id -> its sets (filled once at startup)

    bool enabled() {
        return std::get<bool>(recompui::config::get_config("rules").get_option_value("uniform_picker"));
    }

    // Rules → Unpicked Uniforms (Modern): 1 2026 (default), 0 1999 (the game's own), 2 Random.
    uint32_t unpicked_mode() {
#if BLITZ_MODERN
        return std::get<uint32_t>(recompui::config::get_config("rules").get_option_value("cpu_uniforms"));
#else
        return 0;
#endif
    }

    // A team's extra sets, by the store's team index (team_store.h).
    const std::vector<ExtraSet>* sets_of_team(int team) {
#if BLITZ_MODERN
        if (blitz::league_active()) return nullptr;
        const auto& f = blitz::team_franchises();
        if (team < 0 || (size_t)team >= f.size()) return nullptr;
        auto it = extra_sets.find(f[(size_t)team]);
        return it == extra_sets.end() ? nullptr : &it->second;
#else
        (void)team;
        return nullptr;
#endif
    }
    // The same for a game slot (the cursor, a pick): the team in it.
    const std::vector<ExtraSet>* sets_of(int slot) {
#if BLITZ_MODERN
        return sets_of_team(slot >= 0 ? blitz::team_of_slot(slot) : slot);
#else
        return sets_of_team(slot);
#endif
    }

    // The controller in a player slot. The team-select loop and the opponent picker name a slot (players 1-2
    // on the left, 3-4 on the right), not a controller: a second controller that joins the right side sits in
    // slot 3 (index 2). The game reads a slot the same way (boot func_80000D6C).
    int pad_in_slot(uint8_t* rdram, int slot) {
        for (int c = 0; c < 4; c++) {
            if (MEM_W(0, (gpr)(int32_t)(slot_of_pad + 4 * (uint32_t)c)) == slot) return c;
        }
        return -1;
    }


    // Put back the away slots this side's extra set borrowed.
    void return_slots(uint8_t* rdram, int side) {
        int team = g.lent_team[side];
        if (team < 0) return;
#if BLITZ_MODERN  // Classic never lends (no extra sets)
        for (int p = 0; p < 10; p++) blitz::teamdata::live::set_uniform_part(rdram, team, p, 1, g.lent[side][p]);
#endif
        g.lent_team[side] = -1;
    }

    // Lend the team's away slots to its extra set k.
    void lend_slots(uint8_t* rdram, int side, int team, int k) {
#if BLITZ_MODERN
        const auto* sets = sets_of(team);
        if (!sets || (int)sets->size() <= k) return;
        const ExtraSet& s = (*sets)[(size_t)k];
        return_slots(rdram, side);
        g.lent_team[side] = team;
        for (int p = 0; p < 10; p++) g.lent[side][p] = blitz::teamdata::live::uniform_part(rdram, team, p, 1);
        for (int p = 0; p < 10; p++) {
            if (s.files[(size_t)p].empty()) continue;
            int32_t idx = blitz::fs_file_index(s.files[(size_t)p]);
            if (idx >= 0 && (uint32_t)idx < blitz::fs_ram_count(rdram)) blitz::teamdata::live::set_uniform_part(rdram, team, p, 1, (uint16_t)(idx + 1));
        }
        g.dark[side] = s.dark ? 1 : 0;
#else
        (void)rdram; (void)side; (void)team; (void)k;
#endif
    }

    // home_2026 / away_2026, which the Home and Away slots already are at Unpicked Uniforms 2026 (uniform_live_2026).
    bool is_base_2026(const ExtraSet& s) { return s.id == "home_2026" || s.id == "away_2026"; }

    std::string set_name(int team, int set) {
        if (set == -1) return "game default";
        if ((set == 0 || set == 1) && unpicked_mode() == 1) {
            if (const auto* sets = sets_of(team)) {
                for (const ExtraSet& s : *sets) {
                    if (s.id == (set == 0 ? "home_2026" : "away_2026")) return s.name;
                }
            }
        }
        if (set == 0) return "home uniform";
        if (set == 1) return "away uniform";
        const auto* sets = sets_of(team);
        return sets && (int)sets->size() > set - 2 ? (*sets)[(size_t)(set - 2)].name : "away uniform";
    }

    void dress(uint8_t* rdram, recomp_context* ctx, int side, int team, int set) {
        recomp_context saved = *ctx;
        ctx->r4 = side;
        ctx->r5 = team;
        ctx->r6 = set;
        MEM_W(0, (gpr)(int32_t)busy) = 1;
        func_80256580(rdram, ctx);
        MEM_W(0, (gpr)(int32_t)busy) = 0;
        // Then the redraw a move does (as opponent_select.cpp): it re-points the model at the team's QB record
        // and rebuilds the side's list text, which the confirm flash (ovl_b 802F83C0) later reads.
        ctx->r4 = team;
        ctx->r5 = side;
        ctx->r6 = 1;
        ovl_b_func_802FA080(rdram, ctx);
        *ctx = saved;
    }
}

namespace blitz {
    // At on_init, before the file system loads (game_init.cpp): uniforms/<TEAM>/<set>/ become extra sets, in
    // folder-name order, at most 10 a team: the app's own (Resources/uniforms, make app-modern), then yours
    // (<app folder>/uniforms), which win for a set folder of the same name. Added files are named
    // u<team><set><part>.wms (e.g. uden05.wms).
#if BLITZ_MODERN && BLITZ_PSP
    // The PSP (docs/PSP_PORT.md §4.3): the kit's ROM has every set's files merged on the Mac (psp-host --make-kit, which runs
    // the code below), so the EBOOT takes the sets from the kit's list instead of reading 1,000-odd files at boot. One line
    // a set: "uniform <franchise>|<id>|<name>|<dark 0/1>|<file 0>|...|<file 9>" (a file "" keeps the team's away part).
    std::string uniform_sets_export() {
        std::string out;
        for (const auto& [franchise, sets] : extra_sets)
            for (const ExtraSet& s : sets) {
                out += "uniform " + franchise + "|" + s.id + "|" + s.name + "|" + (s.dark ? "1" : "0");
                for (const std::string& f : s.files) out += "|" + f;
                out += "\n";
            }
        return out;
    }
    size_t uniform_sets_import(const std::string& text) {
        extra_sets.clear();
        size_t n = 0;
        for (size_t at = 0; at < text.size();) {
            size_t end = text.find('\n', at);
            std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? text.size() : end + 1;
            if (line.rfind("uniform ", 0) != 0) continue;
            std::vector<std::string> f;
            for (size_t p = 8;;) {
                size_t bar = line.find('|', p);
                f.push_back(line.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
                if (bar == std::string::npos) break;
                p = bar + 1;
            }
            if (f.size() != 14) continue;
            ExtraSet s;
            s.id = f[1];
            s.name = f[2];
            s.dark = f[3] == "1";
            for (int k = 0; k < 10; k++) s.files[(size_t)k] = f[4 + (size_t)k];
            extra_sets[f[0]].push_back(std::move(s));
            n++;
        }
        return n;
    }
#endif
#if BLITZ_MODERN && BLITZ_PSP
    std::string psp_kit_text();  // psp_artfs.cpp: the kit's rom/kit.txt ("" in the kit maker, which reads the folders)
#endif

    void uniform_sets_register(int pass) {
#if BLITZ_MODERN && BLITZ_PSP
        if (std::string kit = psp_kit_text(); !kit.empty()) {
            if (pass == 1) {
                size_t n = uniform_sets_import(kit);
                fprintf(stderr, "[uniform] %zu extra set(s) from the kit, for %zu team(s)\n", n, extra_sets.size());
            }
            return;
        }
#endif
#if BLITZ_MODERN
        std::error_code ec;
        auto subdirs = [&](const std::filesystem::path& p) {
            std::vector<std::filesystem::path> out;
            for (const auto& e : std::filesystem::directory_iterator(p, ec)) if (e.is_directory(ec)) out.push_back(e.path());
            std::sort(out.begin(), out.end());
            return out;
        };
        std::map<std::string, std::map<std::string, std::filesystem::path>> teams;  // DEN -> set folder -> its path
        // Resources (a private build's), the 2026 Season pak's, then yours: a later set of the same name replaces one before.
        std::vector<std::filesystem::path> roots;
        for (const auto& c : blitz::content_dirs()) roots.push_back(c / "uniforms");
        roots.push_back(recomp::get_config_path() / "uniforms");
        for (const std::filesystem::path& root : roots)
            for (const auto& tdir : subdirs(root)) {
                std::string franchise = tdir.filename().string();
                for (char& c : franchise) c = (char)std::toupper((unsigned char)c);
                for (const auto& sdir : subdirs(tdir)) teams[franchise][sdir.filename().string()] = sdir;
            }
        // Two passes (game_init.cpp): each team's 2026 home and away first (Lite: Season's carts keep only those
        // files), then every other set. A part file that's byte for byte one already added is shared, so the game's
        // file table, which the N64 keeps in its heap (40 bytes a file), holds it once.
        static std::map<std::vector<uint8_t>, std::string> added;
        static size_t files = 0, count = 0, shared = 0;
        if (pass == 0) {  // a restart registers everything again
            added.clear();
            files = count = shared = 0;
        }
        for (const auto& [franchise, sets] : teams) {
            std::string lower = franchise;
            for (char& c : lower) c = (char)std::tolower((unsigned char)c);
            if (lower.size() > 3) continue;  // a franchise id (DEN, GB); keeps names within the TOC's 14 characters
            std::vector<ExtraSet>& list = extra_sets[franchise];
            for (const auto& [_, sdir] : sets) {
                if (list.size() >= 10) break;
                ExtraSet s;
                s.name = sdir.filename().string();
                s.id = s.name;
                if (s.id.size() > 3 && std::isdigit((unsigned char)s.id[0]) && std::isdigit((unsigned char)s.id[1]) && s.id[2] == '_') s.id = s.id.substr(3);
                std::ifstream in{sdir / "set.json"};
                if (in) {
                    try {
                        nlohmann::json j = nlohmann::json::parse(in);
                        s.name = j.value("name", s.name);
                        s.dark = j.value("dark", true);
                    }
                    catch (...) {
                        fprintf(stderr, "[uniform] %s: set.json isn't valid JSON\n", sdir.string().c_str());
                    }
                }
                if ((s.id == "home_2026" || s.id == "away_2026") != (pass == 0)) continue;
                bool any = false;
                for (int p = 0; p < 10; p++) {
                    std::ifstream f{sdir / (std::string(part_names[p]) + ".wms"), std::ios::binary};
                    if (!f) continue;
                    std::vector<uint8_t> data{std::istreambuf_iterator<char>(f), {}};
                    if (auto same = added.find(data); same != added.end()) {
                        s.files[(size_t)p] = same->second;
                        any = true;
                        shared++;
                        continue;
                    }
                    std::string name = "u" + lower + std::to_string(list.size()) + std::to_string(p) + ".wms";
                    if (data.size() >= 32 && data[0] == 'w' && data[1] == 'n') added.emplace(data, name);
                    if (data.size() < 32 || data[0] != 'w' || data[1] != 'n' || !blitz::fs_register_file(name, std::move(data))) {
                        fprintf(stderr, "[uniform] %s: %s.wms isn't a game image\n", sdir.string().c_str(), part_names[p]);
                        continue;
                    }
                    s.files[(size_t)p] = name;
                    any = true;
                    files++;
                }
                if (any) {
                    list.push_back(std::move(s));
                    count++;
                }
            }
            if (list.empty()) extra_sets.erase(franchise);
        }
        if (pass == 1 && count)
            fprintf(stderr, "[uniform] %zu extra set(s), %zu files (%zu more shared), for %zu team(s)\n", count, files, shared,
                    extra_sets.size());
#endif
    }
}

#if BLITZ_MODERN
namespace {
    // Each team's home_2026 / away_2026 parts (team < count), as file ids. Returns how many teams have one.
    int uniform_2026_each(size_t count, const std::function<void(int, int, int, uint16_t)>& set) {
        int changed = 0;
        const auto& f = blitz::team_franchises();
        for (size_t t = 0; t < count && t < f.size(); t++) {
            auto it = extra_sets.find(f[t]);
            if (it == extra_sets.end()) continue;
            bool any = false;
            for (const ExtraSet& s : it->second) {
                int side = s.id == "home_2026" ? 0 : s.id == "away_2026" ? 1 : -1;
                if (side < 0) continue;
                for (int p = 0; p < 10; p++) {
                    if (s.files[(size_t)p].empty()) continue;
                    int32_t idx = blitz::fs_file_index(s.files[(size_t)p]);
                    if (idx < 0) continue;
                    set((int)t, p, side, (uint16_t)(idx + 1));
                    any = true;
                }
            }
            if (any) changed++;
        }
        return changed;
    }
}
#endif

namespace blitz {
    // Export ROM (export_rom.cpp), on a scratch copy of ovl_b in RDRAM word order: each team's home and away slots
    // point at its home_2026 / away_2026 set, where it has one. Returns how many teams changed.
    int uniform_export_2026(uint8_t* rdram) {
#if BLITZ_MODERN
        return uniform_2026_each(blitz::team_franchises().size(), [&](int t, int part, int side, uint16_t id) {
            blitz::teamdata::live::set_uniform_part(rdram, t, part, side, id);
        });
#else
        (void)rdram;
        return 0;
#endif
    }
    // Rules → Unpicked Uniforms at 2026 (the default): every team's home and away slots in the game's own tables point at its
    // home_2026 / away_2026 parts, so team select, the picker's Home and Away and the game all show them, not only the field
    // from kickoff on (team: "the default jerseys are supposed to be 2026"). After each store refresh (engine32.cpp), whose
    // tables are the store's 1999 ids; the store itself keeps those, for 1999 mode and Pro Bowl. Returns teams changed.
    int uniform_live_2026(uint8_t* rdram) {
#if BLITZ_MODERN
        if (blitz::original_game() || unpicked_mode() != 1) return 0;
        const uint32_t files = blitz::fs_ram_count(rdram);
        return uniform_2026_each(blitz::team_franchises().size(), [&](int t, int part, int side, uint16_t id) {
            if ((uint32_t)(id - 1) < files) blitz::teamdata::live::set_uniform_part(rdram, t, part, side, id);
        });
#else
        (void)rdram;
        return 0;
#endif
    }
#if BLITZ_MODERN
    // The same onto store records (Export ROM on the original game): team index = store index.
    int uniform_export_2026(std::vector<blitz::teamdata::Team>& teams) {
        return uniform_2026_each(std::min(teams.size(), blitz::team_franchises().size()), [&](int t, int part, int side, uint16_t id) {
            teams[(size_t)t].uniform[(size_t)part][(size_t)side] = id;
        });
    }
#endif
}

namespace {
    // Pro Bowl (Modern): R at team select makes a side a Pro Bowl crew (crew.cpp builds it after team select). Not
    // in the save-state struct above, so older states still load.
    bool pro_side[2] = {false, false};
    uint16_t pro_held[2] = {0, 0};
    bool pro_asking[2] = {false, false};  // waiting on the keyboard for this side's team code
    struct Lent { int team = -1; uint16_t logo = 0; } pro_logo[2];  // the team-select logo the star replaced
    [[maybe_unused]] const bool pro_registered = blitz::savestate_register_pod("probowl_sides", &pro_side);
    constexpr uint16_t pro_button = 0x0010;  // N64 R

    // A side's QB model at team select: ovl_b func_802F9E08's player object (D_802D1590 + slot * 0xB98: slot 3 the
    // left side's, 10 the right's; +0xB14 the slot, +0xB1C the side), drawn each frame from its position at +0 / +4 / +8
    // (about x ±20, y -4, z 65; the idle animation moves x and z, not y). A Pro Bowl side's model goes far below the
    // view (team): the crew isn't the hovered team's QB.
    constexpr uint32_t models = 0x802D1590, model_size = 0xB98;
    void hide_model(uint8_t* rdram, int side) {
        uint32_t slot = side == 0 ? 3 : 10, o = models + slot * model_size;
        if ((uint32_t)MEM_W(0, (gpr)(int32_t)(o + 0xB14)) != slot || MEM_W(0, (gpr)(int32_t)(o + 0xB1C)) != side) return;
        if ((uint32_t)MEM_W(0, (gpr)(int32_t)(o + 4)) != 0xC4FA0000u) fprintf(stderr, "[probowl] side %d: model hidden\n", side);
        MEM_W(0, (gpr)(int32_t)(o + 4)) = (int32_t)0xC4FA0000u;  // y -2000.0
        // And the QB's name under the logo ("7-BRISSETT"): the redraw keeps its text object at D_8032DF64[side];
        // +0xD0 is its colour (ARGB), so alpha 0.
        uint32_t t = (uint32_t)MEM_W(0, (gpr)(int32_t)(0x8032DF64 + 4 * (uint32_t)side));
        if (t >= 0x80000400u && t < 0x80800000u) MEM_W(0, (gpr)(int32_t)(t + 0xD0)) = MEM_W(0, (gpr)(int32_t)(t + 0xD0)) & 0x00FFFFFF;
    }
}

namespace blitz {
    bool probowl_on(int side) { return side >= 0 && side < 2 && pro_side[side]; }

    // ovl_b loads before each team select and the attract loop, never between team select and kickoff: the last
    // game is over, so its Pro Bowl sides are too (else the attract demo plays in their crews and uniforms).
    // ovl_b's team blocks were reloaded with it, so the lent logo is already back.
    void probowl_game_over() {
        for (int s = 0; s < 2; s++) {
            pro_side[s] = false;
            pro_held[s] = 0;
            pro_asking[s] = false;
            pro_logo[s].team = -1;
        }
    }

    // A team's uniform sets for the mixed-uniform builder (Pro Bowl, mixed_uniforms.cpp): 0 home and 1 away (the
    // game's own, 1999), 2+k the team's extra sets (2026 ones first). `base` is the team's 20 ids from its block
    // (part p: [2p] home, [2p+1] away); `out` gets the set's 10 part ids.
    int uniform_set_count(int team) {
        const auto* sets = sets_of_team(team);
        return 2 + (sets ? (int)sets->size() : 0);
    }
    int uniform_set_index(int team, const std::string& id) {
        const auto* sets = sets_of_team(team);
        if (!sets) return -1;
        for (size_t k = 0; k < sets->size(); k++) if ((*sets)[k].id == id) return 2 + (int)k;
        return -1;
    }
    // A Pro Bowl player's set (crew.cpp): mode 0 his team's 2026 home or away, 1 the game's own (1999), 2 any of
    // the team's sets in the shade asked for (dark: colour jerseys; light: white ones).
    int uniform_set_pick(int team, int mode, bool dark, uint32_t random) {
        const auto* sets = sets_of_team(team);
        if (mode == 0) {
            int i = uniform_set_index(team, dark ? "home_2026" : "away_2026");
            return i >= 0 ? i : dark ? 0 : 1;
        }
        if (mode == 1 || !sets) return dark ? 0 : 1;
        std::vector<int> fits = {dark ? 0 : 1};
        for (size_t k = 0; k < sets->size(); k++)
            if ((*sets)[k].dark == dark) fits.push_back(2 + (int)k);
        return fits[random % fits.size()];
    }

    void uniform_set_parts(uint8_t* rdram, int team, int set, const uint16_t base[20], uint16_t out[10]) {
        for (int p = 0; p < 10; p++) out[p] = base[2 * p + (set == 0 ? 0 : 1)];
#if BLITZ_MODERN
        const auto* sets = sets_of_team(team);
        if (set < 2 || !sets || set - 2 >= (int)sets->size()) return;
        const ExtraSet& s = (*sets)[(size_t)(set - 2)];
        for (int p = 0; p < 10; p++) {
            if (s.files[(size_t)p].empty()) continue;
            int32_t idx = blitz::fs_file_index(s.files[(size_t)p]);
            if (idx >= 0 && (uint32_t)idx < blitz::fs_ram_count(rdram)) out[p] = (uint16_t)(idx + 1);
        }
#else
        (void)rdram;
#endif
    }
}

namespace blitz {
    // Season's pre-game screen (season_pregame.cpp): a side's choices for a team (a game slot) in the order △ cycles
    // them at team select: -1 game decides, 0 home, 1 away, 2 + k the team's extra sets (not the 2026 home and away,
    // which Home and Away already are at Unpicked Uniforms 2026). Each with its name.
    std::vector<std::pair<int, std::string>> uniform_choices(int slot) {
        std::vector<std::pair<int, std::string>> out = {{-1, "Game decides"}, {0, set_name(slot, 0)}, {1, set_name(slot, 1)}};
        if (const auto* sets = sets_of(slot)) {
            for (size_t k = 0; k < sets->size(); k++) {
                if (unpicked_mode() == 1 && is_base_2026((*sets)[k])) continue;
                out.push_back({2 + (int)k, (*sets)[k].name});
            }
        }
        return out;
    }
    // The set a side wears for this team now (-1 when nothing's picked for it).
    int uniform_current(int side, int slot) {
        if (side < 0 || side > 1 || g.team[side] != slot) return -1;
        return g.set[side];
    }
    bool uniform_picker_on() { return enabled(); }
    // Game thread, before the game: the side's pick for this team, as △ at team select would leave it. An extra set isn't
    // lent to the team's block (ovl_b reloads before the game): the kickoff puts it in this game's copy of the team.
    void uniform_season_pick(uint8_t* rdram, int side, int slot, int set) {
        if (side < 0 || side > 1) return;
        return_slots(rdram, side);
        const auto* sets = sets_of(slot);
        if (set >= 2 && (!sets || set - 2 >= (int)sets->size())) set = -1;
        g.team[side] = slot;
        g.set[side] = set;
        g.redress[side] = 0;
        if (set >= 2) g.dark[side] = (*sets)[(size_t)(set - 2)].dark ? 1 : 0;
        fprintf(stderr, "[uniform] season pick: side %d team %d: %s\n", side, slot, set_name(slot, set).c_str());
    }
}

namespace {
    std::atomic<int64_t> picker_listening_ms{0};  // the selection loop's last frame with the picker on (game thread)
}

namespace blitz {
    std::atomic<bool> picker_season{false};  // the team select listening is Season's
    bool uniform_picker_season() { return picker_season.load(); }

    // The touch controls show a UNIFORM button while the picker is listening (team select, Season too).
    bool uniform_picker_listening() {
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return now - picker_listening_ms.load() < 400;
    }
}

extern "C" {
    // A side's team select starts (ovl_b 0x802F91F8, with blitz_opp_setup): a new game, so no choice yet.
    void blitz_uniform_reset(int32_t side) {
        if (side < 0 || side > 1) return;
        g.set[side] = -1;
        g.team[side] = -1;
        g.redress[side] = 0;
        g.held[side] = 0;
        g.lent_team[side] = -1;  // ovl_b's data was reloaded: nothing is lent any more
        g.auto_key = -1;         // a new game: Random picks again
        pro_side[side] = false;
        pro_held[side] = 0;
        pro_asking[side] = false;
#if BLITZ_MODERN  // Pro Bowl is Modern only
        uint8_t* rdram = blitz::monitor_rdram();
        if (pro_logo[side].team >= 0 && rdram != nullptr) {  // a new team select: the team's own logo back
            blitz::teamdata::live::set_art(rdram, pro_logo[side].team, blitz::teamdata::Art::Logo, pro_logo[side].logo);
        }
#endif
        pro_logo[side].team = -1;
    }

    // Each frame of the selection loop (0x802F9A10): s5 / r21 the side, s7 / r23 its player slot, s6 / r22 the cursor.
    void blitz_uniform_picker(uint8_t* rdram, recomp_context* ctx) {
        blitz_team_select_frame();  // savestate.cpp: □ goes back here, in every mode
        // Season: uniforms too since 2026-11-02 (team), but no Pro Bowl (a crew isn't the team the standings follow), and
        // no team select hint, which names it.
        bool season = MEM_W(0, (gpr)(int32_t)game_mode) == 2;
#if BLITZ_MODERN
        if (blitz::original_game()) return;  // the original game: no uniform sets, crews or Pro Bowl (nor the hint naming them)
        blitz::crew_hover((int32_t)ctx->r21, (int32_t)ctx->r22);  // Pick Your Crew's faces, fetched while the cursor rests
#endif
        if (!season) blitz::hints_mark_team_select();
        blitz::picker_season = season;
        if (enabled()) picker_listening_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count();
#if BLITZ_MODERN
        // Roster Room locked in while ovl_b stayed loaded (it reloads only after a game): its crews now.
        if (blitz::crews_changed.exchange(false)) blitz::lineup_overlay_now(rdram);
#endif
        int side = (int32_t)ctx->r21, port = (int32_t)ctx->r23, team = (int32_t)ctx->r22;
#if BLITZ_MODERN
        for (int s = 0; s < 2; s++) if (pro_side[s]) hide_model(rdram, s);
        // R: this side a Pro Bowl crew (or back to its team), for a side still choosing (or the CPU's, while the
        // player picks the opponent). Modern's Blitz 2026 rosters only: the crews come from the player file.
        // Not in Season (a crew isn't the team the standings follow): Arcade and Tournament. Only with the Blitz 2026
        // pack in play (the 2026 Season; crew.cpp builds nothing without it, which left a half Pro Bowl: 2026-11-13).
        if (side >= 0 && side <= 1 && !blitz::league_active() && !season && blitz::active_pack_id() == "blitz2026" &&
            (MEM_W(0, (gpr)(int32_t)(chosen + 4 * (uint32_t)side)) == -1 || side == blitz_opp_choosing_side())) {
            int pp = side == blitz_opp_choosing_side() ? blitz_opp_choosing_port() : port;
            pp = pp >= 0 && pp <= 3 ? pad_in_slot(rdram, pp) : -1;
            uint16_t b = 0;
            float x = 0, y = 0;
            if (pp >= 0 && pp <= 3 && blitz::get_input(pp, &b, &x, &y)) {
                bool human = ((MEM_W(0, (gpr)(int32_t)0x802EA418) >> (2 * side)) & 3) != 0 && side != blitz_opp_choosing_side();
                bool pressed = (b & pro_button) && !(pro_held[side] & pro_button) && !pro_side[side] && team >= 0;
                // A player's first Pro Bowl: the keyboard asks for the team code first (team), and the pick goes on
                // once it's typed and A is let go (the game reads no buttons while the keyboard is up).
                if (pressed && human && !pro_asking[side] && !blitz::probowl_code_saved(side)) {
                    blitz::probowl_ask_code(side);
                    pro_asking[side] = true;
                    pressed = false;
                }
                if (pro_asking[side]) {
                    int st = blitz::probowl_ask_state(side);
                    if (st == 3) pro_asking[side] = false;  // cancelled: still choosing a team
                    if (st == 2 && !(b & 0x8000) && team >= 0) {
                        pro_asking[side] = false;
                        pressed = true;
                    }
                }
                if (pressed) {
                    // Pro Bowl is the pick (team): the side's star on the hovered team's logo, then A for the player,
                    // so team select confirms and moves on (to the opponent). The team gives the stadium.
                    pro_side[side] = true;
                    int32_t idx = blitz::fs_file_index(side == 0 ? "pb_lg_l.wms" : "pb_lg_r.wms");
                    if (idx >= 0 && (uint32_t)idx < blitz::fs_ram_count(rdram)) {
                        namespace live = blitz::teamdata::live;
                        pro_logo[side] = {team, live::art(rdram, team, blitz::teamdata::Art::Logo)};
                        live::set_art(rdram, team, blitz::teamdata::Art::Logo, (uint16_t)(idx + 1));
                        // The list's line for the hovered team reads the crew's code (team) while the redraw draws it:
                        // the team's list name is lent and put back right after.
                        std::string kept = live::list_name(rdram, team);
                        live::set_list_name(rdram, team, blitz::probowl_code(side, human));
                        recomp_context saved = *ctx;
                        ctx->r4 = team;
                        ctx->r5 = side;
                        ctx->r6 = 1;
                        ovl_b_func_802FA080(rdram, ctx);
                        *ctx = saved;
                        live::set_list_name(rdram, team, kept);
                    }
                    blitz::inject_press(pp, 0x8000, 150);  // N64 A
                    blitz::post_status(std::string(side == 0 ? "Left side: " : "Right side: ") + "Pro Bowl crew");
                    fprintf(stderr, "[probowl] side %d: on\n", side);
                }
                pro_held[side] = b;
            }
        }
#endif
        if (side < 0 || side > 1 || team < 0 || !enabled()) return;
        if (team != g.team[side]) {  // the game re-dressed the model in the new team's home set
            if (g.set[side] >= 2) {  // an extra set belongs to the team it was picked for
                return_slots(rdram, side);
                g.set[side] = -1;
            }
            g.team[side] = team;
            if (g.set[side] == 1) g.redress[side] = 2;
        }
        if (g.redress[side] > 0 && --g.redress[side] == 0) dress(rdram, ctx, side, team, 1);
        // Only a side still choosing: with Choose CPU Opponent both sides' loops read the player's port, and once
        // the player's team is in, the press is for the opponent being picked.
        if (MEM_W(0, (gpr)(int32_t)(chosen + 4 * (uint32_t)side)) != -1 && side != blitz_opp_choosing_side()) return;
        if (side == blitz_opp_choosing_side()) port = blitz_opp_choosing_port();  // the player picks the opponent
        if (port < 0 || port > 3) return;
        port = pad_in_slot(rdram, port);
        uint16_t buttons = 0;
        float x = 0, y = 0;
        if (port < 0 || port > 3 || !blitz::get_input(port, &buttons, &x, &y)) return;
        bool pressed = (buttons & button) && !(g.held[side] & button);
        g.held[side] = buttons;
        if (!pressed) return;
        // Extra sets, unless the other side already lent this team's away slots (the same team on both sides).
        const auto* sets = sets_of(team);
        int last = 1 + (sets && g.lent_team[1 - side] != team ? (int)sets->size() : 0);
        int next = g.set[side] + 1;
        while (next >= 2 && next <= last && unpicked_mode() == 1 && is_base_2026((*sets)[(size_t)(next - 2)])) next++;  // Home and Away are those
        if (next > last) next = -1;
        return_slots(rdram, side);
        g.set[side] = next;
        if (next >= 2) lend_slots(rdram, side, team, next - 2);
        dress(rdram, ctx, side, team, next >= 1 ? 1 : 0);
        std::string what = set_name(team, next);
        // Said in a toast: a label in the game's own text, made from this loop, breaks the list's confirm flash
        // (ovl_b 802F83C0 then writes through a stale text handle and the app crashes).
        blitz::post_status(std::string(side == 0 ? "Left" : "Right") + " team: " + what);
        fprintf(stderr, "[uniform] side %d team %d: %s\n", side, team, what.c_str());
    }

    // ovl_a 0x80302F40, before func_80256850(team0, set0, team1, set1): the picked sets win.
    void blitz_uniform_kickoff(recomp_context* ctx) {
        uint8_t* rdram = blitz::monitor_rdram();
        // The attract demo wears the game's own sets, not the last game's picks.
        if (rdram == nullptr || MEM_W(0, (gpr)(int32_t)attract_phase) == 7) return;
#if BLITZ_MODERN
        // Pick Your Crew's players and Pro Bowl crews (the game's own sets so far: a crew dresses opposite a team).
        blitz_crew_kickoff(rdram, (int32_t)ctx->r4, (int32_t)ctx->r6, (int32_t)ctx->r5, (int32_t)ctx->r7);
#endif
        bool picker = enabled();
        // A pick holds for the team it was made for: Season reaches team select only at a new season, so next week the
        // side may be someone else (or its lent slots gone with ovl_b's reload).
        const int32_t kick_team[2] = {(int32_t)ctx->r4, (int32_t)ctx->r6};
        int32_t set[2] = {picker && g.team[0] == kick_team[0] ? g.set[0] : -1, picker && g.team[1] == kick_team[1] ? g.set[1] : -1};
        bool picked[2] = {set[0] >= 0, set[1] >= 0};
        bool clash = ((int32_t)ctx->r5 | (int32_t)ctx->r7) != 0;  // the game gave one side its away set
        // A side left to the game follows the other's pick: home against a road set or a white extra set, away
        // against a colour extra set, and against home only when the jerseys clash (so the two never both change).
        for (int s = 0; s < 2; s++) {
            int o = set[1 - s];
            if (set[s] >= 0 || o < 0) continue;
            if (o >= 2) set[s] = g.dark[1 - s] ? 1 : 0;
            else set[s] = o == 1 ? 0 : (clash ? 1 : 0);
        }
        for (int s = 0; s < 2; s++) if (set[s] < 0) set[s] = s == 0 ? (int32_t)ctx->r5 : (int32_t)ctx->r7;  // the game's own
        int32_t team[2] = {(int32_t)ctx->r4, (int32_t)ctx->r6};

        // Unpicked sides (the CPU's team, Season games, or a player who didn't press △): Rules → Unpicked Uniforms.
        uint32_t mode = unpicked_mode();
        const ExtraSet* wear[2] = {nullptr, nullptr};
        if (mode == 1) {  // 2026: the team's current home or away, as the game dresses it
            for (int s = 0; s < 2; s++) {
                if (picked[s]) continue;
                const auto* sets = sets_of(team[s]);
                if (!sets) continue;
                const char* want = set[s] == 0 ? "home_2026" : "away_2026";
                for (const ExtraSet& e : *sets) if (e.id == want) wear[s] = &e;
            }
        }
        else if (mode == 2) {  // Random: any of the team's sets, kept for the whole game
            int32_t key = team[0] * 64 + team[1];
            if (g.auto_key != key) {
                g.auto_key = key;
                static std::mt19937 rng{std::random_device{}()};
                for (int s = 0; s < 2; s++) {
                    const auto* sets = sets_of(team[s]);
                    int n = 2 + (sets ? (int)sets->size() : 0);
                    g.auto_pick[s] = std::uniform_int_distribution<int>(0, n - 1)(rng);
                }
            }
            auto is_dark = [&](int s, int pick) {
                const auto* sets = sets_of(team[s]);
                if (pick >= 2 && sets && pick - 2 < (int)sets->size()) return (*sets)[(size_t)(pick - 2)].dark;
                return pick == 0;  // a base home jersey is a colour one, the away a white one
            };
            int pick[2] = {picked[0] ? set[0] : g.auto_pick[0], picked[1] ? set[1] : g.auto_pick[1]};
            // Both in colour: an unpicked side changes to white (the second one, if neither was picked).
            if (is_dark(0, pick[0]) && is_dark(1, pick[1])) {
                int s = !picked[1] ? 1 : !picked[0] ? 0 : -1;
                if (s >= 0) pick[s] = 1;
            }
            for (int s = 0; s < 2; s++) {
                if (picked[s]) continue;
                set[s] = pick[s] >= 1 ? 1 : 0;
                const auto* sets = sets_of(team[s]);
                if (pick[s] >= 2 && sets && pick[s] - 2 < (int)sets->size()) wear[s] = &(*sets)[(size_t)(pick[s] - 2)];
            }
        }
        // An extra set picked on Season's pre-game screen isn't lent to the team's block: this game's copy wears it.
        for (int s = 0; s < 2; s++) {
            if (!picked[s] || set[s] < 2 || g.lent_team[s] == team[s]) continue;
            const auto* sets = sets_of(team[s]);
            if (sets && set[s] - 2 < (int)sets->size()) wear[s] = &(*sets)[(size_t)(set[s] - 2)];
        }
        // A set for an unpicked side goes into this game's copy of the team (ovl_b's confirm copy, which the game
        // dresses from now; each new game copies afresh), worn as "away".
        for (int s = 0; s < 2; s++) {
            if (wear[s] == nullptr) continue;
            if (!blitz::teamdata::live::has_copy(rdram, s)) continue;
            bool any = false;
            for (int p = 0; p < 10; p++) {
                if (wear[s]->files[(size_t)p].empty()) continue;
#if BLITZ_MODERN
                int32_t idx = blitz::fs_file_index(wear[s]->files[(size_t)p]);
                if (idx < 0 || (uint32_t)idx >= blitz::fs_ram_count(rdram)) continue;
                blitz::teamdata::live::set_copy_uniform_part(rdram, s, p, 1, (uint16_t)(idx + 1));
                any = true;
#endif
            }
            if (any) set[s] = 1;
        }
        ctx->r5 = set[0] >= 1 ? 1 : 0;  // an extra set is worn as "away"
        ctx->r7 = set[1] >= 1 ? 1 : 0;
#if BLITZ_MODERN
        blitz_mixed_kickoff(rdram, ctx, (int32_t)ctx->r4, (int32_t)ctx->r6);  // dev: a uniform per player
#endif
        if (picked[0] || picked[1] || wear[0] || wear[1]) {
            fprintf(stderr, "[uniform] kickoff: left %s, right %s\n", wear[0] ? wear[0]->name.c_str() : set[0] ? "away" : "home",
                    wear[1] ? wear[1]->name.c_str() : set[1] ? "away" : "home");
        }
    }
}
