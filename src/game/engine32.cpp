// Modern Blitz on the original game: 32 teams without the Team Extension (docs/BLITZ2026.md "The original-game
// engine"). The "modern" build is made from build/engine32 (tools/engine32_toml.py): the original game's code with
// its team tables moved to 32-team copies (engine32_layout.h) and the hooks below.
//
// Every team is a record in the team store (team_store.h): the ROM's 31 and Houston (tools/make_houston.py: the
// Titans' record with our names, art and uniforms), with the content pack's edits on top. Store index = game slot (Houston is 31), so a pick, a season or a record means one team
// everywhere. The store's original-game encoder writes the moved tables; they sit outside the overlays, so a write
// lasts until the next.
//
// Hooks:
//   ovl_b 0x802F9B74  the cursor has moved by +-1 (s6): redo the move in A-Z order over 32;
//   ovl_b 0x802FA3C4  each list row above the cursor (s3): the previous team in that order;
//   ovl_b 0x802FA4F8  each list row below the cursor (s3): the next one;
//   ovl_b 0x802F5568  this game's jersey clash (the game's 31 x 31 table can't hold 32): from the store.
//   ovl_b 0x803017A8  the season schedule prints each team by its list name (as team+), not its city;
//   ovl_b 0x802F02C4 / 0x802F0650  the credits page, once, before the attract sequence's New Features screen.
#if BLITZ_ENGINE_ORIGINAL
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "util/file.h"
#include "json/json.hpp"
#include "team_store.h"
#include "engine32_layout.h"
#include "season_pak.h"

namespace blitz {
    bool original_game();  // adv_mode.cpp: Original 1999
    int uniform_live_2026(uint8_t* rdram);  // uniform_picker.cpp: Unpicked Uniforms 2026 into the live tables
    bool fs_register_file(const std::string& name, std::vector<uint8_t> data);  // art_dropin.cpp
    int32_t fs_file_index(const std::string& name);
    void post_status(const std::string& msg);  // notices.cpp: a toast
}

#if BLITZ_PSP
std::vector<uint8_t> psp_rom_head();  // psp_artfs.cpp: ROM bytes 0 to the file system's start
#endif

namespace {
    namespace td = blitz::teamdata;
    namespace orig = blitz::teamdata::original;

    constexpr int teams = blitz::engine32::teams, hou = 31, game_list = 31;
    std::vector<int> order;  // the team select list: A-Z by city, then list name (as Modern)

    // Houston's record and files (tools/make_houston.py): <app folder>/houston, else the 2026 Season pak's, else the
    // app's own (a private build's make app-modern puts them in). Empty without any: the 32nd team is then a neutral
    // stand-in (houston() below). (The dev fallback to build/engine32/houston went 2026-11-13: a public build run from the
    // source folder found it and wasn't public any more.)
    std::filesystem::path houston_dir() {
        std::error_code ec;
        std::vector<std::filesystem::path> tries{recomp::get_config_path() / "houston"};
        for (const auto& root : blitz::content_dirs_newest_first()) tries.push_back(root / "houston");
        for (const std::filesystem::path& p : tries)
            if (std::filesystem::is_regular_file(p / "houston.json", ec)) return p;
        return {};
    }

    // A folder's files by name (a listing's order is the disk's own; the order files are added in decides their
    // indices, which an exported ROM carries).
    std::vector<std::filesystem::path> sorted_files(const std::filesystem::path& dir) {
        std::vector<std::filesystem::path> out;
        std::error_code ec;
        if (dir.empty()) return out;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.is_regular_file(ec)) out.push_back(e.path());
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<uint8_t> read_file(const std::filesystem::path& p) {
        std::ifstream f{p, std::ios::binary};
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(f), {}};
    }

    const std::vector<td::Team>& records() { return td::store().slots(); }
    // The store's teams: the NFL's 32, or a league pak's 2-31 (they fill the game's slots in turn, so whatever slot
    // the game picks on its own, say in the attract demo, is one of them).
    int count() { return (int)records().size(); }
    bool ready() { return count() >= 2 && count() <= teams; }
    int team_in(int slot) { return ready() ? ((slot % count()) + count()) % count() : slot; }

    // Houston (houston.json, tools/make_houston.py): the base team's record (its CPU plays, skill, selector, VS pause
    // and player sizes) with our names, art and 2026 uniforms. Ratings and players come from the pack, as for every
    // team. Nothing of the Team Extension's.
    nlohmann::json houston_json() {
        if (houston_dir().empty()) return nlohmann::json{};
        std::ifstream f{houston_dir() / "houston.json"};
        try {
            return f ? nlohmann::json::parse(f) : nlohmann::json{};
        }
        catch (const std::exception& e) {
            fprintf(stderr, "[e32] houston.json: %s\n", e.what());
            return nlohmann::json{};
        }
    }

    td::Team houston(const std::vector<td::Team>& rom_teams, const std::function<std::string(const std::string&)>& franchise_of,
                     const nlohmann::json& h) {
        td::Team t;
        for (const td::Team& r : rom_teams)
            if (franchise_of && franchise_of(r.nickname()) == h.value("base", "TEN")) t = r;
        auto id = [](const nlohmann::json& name) -> uint16_t {
            int32_t idx = name.is_string() ? blitz::fs_file_index(name.get<std::string>()) : -1;
            return idx >= 0 ? (uint16_t)(idx + 1) : 0;
        };
        // The names are houston.json's (the 2026 Season pak's). Without it, a neutral stand-in: no club's name is
        // built into the app (2026-11-13). The slot stays, so the 32-team Season, saves and Export work the same with
        // or without the pak, and the team keeps the HOU id inside the app (season32.cpp's divisions, the pack).
        t.set_city(h.value("city", "Expansion"));
        t.set_nickname(h.value("nickname", "Expansion"));
        t.set_select(h.value("select", "Expansion"));
        t.set_abbr(h.value("abbr", "EXP"));
        std::string prefix = h.value("prefix", "EXP");
        t.prefix.fill(0);
        for (size_t i = 0; i < prefix.size() && i + 1 < t.prefix.size(); i++) t.prefix[i] = prefix[i];
        // Without Houston's files (no houston.json anywhere): the Titans' own art and uniforms under Houston's names.
        if (!h.contains("art") || !h.contains("uniform")) {
            for (int s = 0; s < td::players_per_team; s++) t.clear_call(s);
            return t;
        }
        const auto& art = h["art"];
        t.logo = id(art["logo"]);
        t.small_logo = id(art["small_logo"]);
        t.title_city = id(art["title_city"]);
        t.title_nick = id(art["title_nick"]);
        t.win = id(art["win"]);
        t.vs_left = id(art["vs_left"]);
        t.vs_right = id(art["vs_right"]);
        for (int k = 0; k < 3; k++) t.end_zones[(size_t)k] = id(art["end_zone_" + std::to_string(k + 1)]);
        for (int p = 0; p < 10; p++) {
            t.set_uniform_part(p, 0, id(h["uniform"]["home"][(size_t)p]));
            t.set_uniform_part(p, 1, id(h["uniform"]["away"][(size_t)p]));
        }
        for (int s = 0; s < td::players_per_team; s++) t.clear_call(s);  // the announcer has no Texans names
        return t;
    }

#if BLITZ_PSP
    // The PSP reads the ROM through a provider (get_rom() is empty there): the code region it needs is read once, the teams
    // loaded from it, and the buffer freed (docs/PSP_PORT.md §4.3).
    const td::Store& psp_rom_teams() {
        static td::Store teams;
        static bool done = false;
        if (!done) {
            done = true;
            std::vector<uint8_t> head = ::psp_rom_head();
            orig::load(teams, head, {});
            fprintf(stderr, "[e32] the ROM's teams from its first %zu KB\n", head.size() / 1024);
        }
        return teams;
    }
#endif

    void build_store(const std::function<std::string(const std::string&)>& franchise_of) {
        orig::use_engine32_layout();
#if BLITZ_PSP
        td::Store rom_teams = psp_rom_teams();
#else
        td::Store rom_teams;
        orig::load(rom_teams, recomp::get_rom(), {});  // in place: a copy here was the whole ROM (about 21 MB) at once
#endif
        std::vector<td::Team> list = rom_teams.slots();
        if (blitz::original_game()) {  // Original 1999: the game's 31, as they shipped
            for (auto& t : list) if (franchise_of) t.franchise = franchise_of(t.nickname());
            td::store().set_all(std::move(list), std::vector<std::vector<bool>>(rom_teams.clash_matrix()));
            fprintf(stderr, "[e32] store: the original 31 teams\n");
            return;
        }
        nlohmann::json h = houston_json();
        if (h.empty()) {
            // No 2026 Season (a public build without the pak): the 32nd team is a stand-in in the Titans' art. Only a
            // build that should have Houston says so.
            fprintf(stderr, "[e32] no houston.json (the 2026 Season pak brings it): a stand-in 32nd team in the Titans' art\n");
            if (blitz::season_content()) blitz::post_status("Houston's art is missing: Houston wears the Titans' for now");
            h = {{"base", "TEN"}, {"no_clash", {"DAL"}}};
        }
        list.push_back(houston(list, franchise_of, h));
        for (auto& t : list) if (franchise_of) t.franchise = franchise_of(t.nickname());
        if (list.back().franchise.empty()) list.back().franchise = h.value("franchise", "HOU");  // the stand-in's id
        std::vector<std::vector<bool>> m(teams, std::vector<bool>(teams));
        const auto& rm = rom_teams.clash_matrix();
        for (int a = 0; a < orig::slots; a++)
            for (int b = 0; b < orig::slots; b++) m[(size_t)a][(size_t)b] = rm[(size_t)a][(size_t)b];
        // Houston's games: the visitor changes to white, but for the teams houston.json names (Dallas, white at home).
        std::vector<std::string> no_clash = h.contains("no_clash") ? h["no_clash"].get<std::vector<std::string>>() : std::vector<std::string>{};
        for (int t = 0; t < teams; t++) {
            bool none = std::find(no_clash.begin(), no_clash.end(), list[(size_t)t].franchise) != no_clash.end();
            m[(size_t)t][hou] = m[hou][(size_t)t] = !none;
        }
        // Your own right-side VS helmets (<app folder>/art/vs_right/<TEAM>.wms, tools/import_team_art.py): files added at
        // on_init (e32_register_files) and named in the teams' records.
        for (auto& t : list) {
            if (t.franchise.empty()) continue;
            std::string n = "vr" + t.franchise;
            for (char& c : n) c = (char)std::tolower((unsigned char)c);
            int32_t idx = blitz::fs_file_index(n + ".wms");
            if (idx >= 0) t.vs_right = (uint16_t)(idx + 1);
        }
        td::store().set_all(std::move(list), std::move(m));
        fprintf(stderr, "[e32] store: %zu teams (%s the 32nd)\n", records().size(), records().back().nickname().c_str());
    }

    // A-Z by city, then list name (the store's, after the pack's edits).
    void sort_order() {
        order.resize((size_t)count());
        for (int i = 0; i < count(); i++) order[(size_t)i] = i;
        auto text = [](const char* p, size_t n) {
            std::string s(p, strnlen(p, n));
            for (char& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        const auto& t = records();
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            std::string ca = text(t[(size_t)a].city_name.data(), 32), cb = text(t[(size_t)b].city_name.data(), 32);
            if (ca != cb) return ca < cb;
            return text(t[(size_t)a].select.data(), 16) < text(t[(size_t)b].select.data(), 16);
        });
    }
    int pos_of(int team) {
        for (int i = 0; i < (int)order.size(); i++) if (order[(size_t)i] == team) return i;
        return 0;
    }

    int cur[2] = {-1, -1};  // each list's cursor team, as last shown
}

namespace blitz {
    void e32_build_store(const std::function<std::string(const std::string&)>& franchise_of) { build_store(franchise_of); }
#if BLITZ_PSP
    // Before recomp::start (psp_main.cpp): the ROM's teams while the heap still has room for the 1.3 MB read.
    void e32_preload_rom_teams() {
        orig::use_engine32_layout();
        (void)psp_rom_teams();
    }
#endif
    int e32_team_of_slot(int slot) { return team_in(slot); }

    // At on_init, before fs_commit_files (game_init.cpp): the files Houston's record names that the game lacks.
    void e32_register_files() {
        std::error_code ec;
        int n = 0;
        for (const std::filesystem::path& f : sorted_files(houston_dir())) {
            if (f.extension() != ".wms") continue;
            if (blitz::fs_register_file(f.filename().string(), read_file(f))) n++;
        }
        fprintf(stderr, "[e32] %d Houston files\n", n);
        // The app's own art (Resources/art in a private build, make app-modern; the 2026 Season pak's), then yours
        // (<app folder>/art), which wins: a file registered again replaces the first.
        std::set<std::string> helmets;
        std::vector<std::filesystem::path> arts;
        for (const auto& root : blitz::content_dirs()) arts.push_back(root / "art");
        arts.push_back(recomp::get_config_path() / "art");
        for (const std::filesystem::path& art : arts) {
            for (const std::filesystem::path& f : sorted_files(art / "vs_right")) {
                std::string team = f.stem().string();
                if (f.extension() != ".wms" || team.empty() || team.size() > 3) continue;
                for (char& c : team) c = (char)std::tolower((unsigned char)c);
                if (blitz::fs_register_file("vr" + team + ".wms", read_file(f))) helmets.insert(team);
            }
        }
        if (!helmets.empty()) fprintf(stderr, "[e32] %zu right-side VS helmets\n", helmets.size());
    }

    // After the files a cart can carry (game_init.cpp): the BlitzRecomp credits page (art/credits.wms,
    // tools/import_team_art.py), shown once at startup in the app; the app's copy, then yours.
    // BRANDING: the credits page art (art/credits.wms: project name, credit line, notice)
    void e32_register_app_files() {
        bool credits = false;
        for (const std::filesystem::path& art : {recompui::file::get_program_path() / "art", recomp::get_config_path() / "art"}) {
            auto page = read_file(art / "credits.wms");
            if (page.size() > 32 && page[0] == 'w' && blitz::fs_register_file("credits.wms", std::move(page))) credits = true;
        }
        if (credits) fprintf(stderr, "[e32] credits page\n");
    }
}

extern "C" {
    // The store into the game: every team's tables (after any change: a pack, a lineup, a crew).
    void blitz_e32_refresh(uint8_t* rdram) {
        if (!ready()) return;
        if (count() == teams) {
            for (orig::Segment seg : {orig::Segment::OvlB, orig::Segment::Main, orig::Segment::OvlA}) orig::write(rdram, seg, td::store());
            blitz::uniform_live_2026(rdram);  // store index = game slot here
        }
        else {  // a league: its teams over and over through the 32 slots
            std::vector<td::Team> all;
            std::vector<std::vector<bool>> clash(teams, std::vector<bool>(teams));
            for (int s = 0; s < teams; s++) {
                all.push_back(records()[(size_t)team_in(s)]);
                for (int t = 0; t < teams; t++) clash[(size_t)s][(size_t)t] = td::store().clashes((size_t)team_in(s), (size_t)team_in(t));
            }
            td::Store slots;
            slots.set_all(std::move(all), std::move(clash));
            for (orig::Segment seg : {orig::Segment::OvlB, orig::Segment::Main, orig::Segment::OvlA}) orig::write(rdram, seg, slots);
        }
        sort_order();
        // The 1-player CPU ladder (main 0x802AB0E0: 9 tiers of up to 4 teams, -1 ended): Houston joins the last
        // tier, which the game gives Denver alone. Main is resident, so once is enough. (A league writes its own.)
        constexpr uint32_t last_tier = 0x802AB0E0 + 8 * 0x14;
        if (count() == teams && MEM_W(4, (gpr)(int32_t)last_tier) == -1) {
            MEM_W(4, (gpr)(int32_t)last_tier) = hou;
            MEM_W(8, (gpr)(int32_t)last_tier) = -1;
        }
    }
    // Every ovl_b load (content_pack_tp.cpp): the lists start fresh.
    void blitz_e32_on_ovl_b(uint8_t* rdram) {
        blitz_e32_refresh(rdram);
        cur[0] = cur[1] = -1;
    }
    void blitz_e32_on_ovl_a(uint8_t*) {}  // the moved tables aren't in the overlay: nothing to put back

    // The game stepped s6 by one in its own 31-team order (up: s6 < 30 ? s6 + 1 : 0; down: s6 > 0 ? s6 - 1 : 30), or
    // jumped (the list's first team): the step goes to the next team in ours.
    int32_t blitz_v32_moved(uint8_t* rdram, int32_t side, int32_t s6) {
        (void)rdram;
        if (!ready() || side < 0 || side > 1) return s6;
        int was = cur[side], d = 0;
        if (was >= 0) d = s6 == (was < game_list - 1 ? was + 1 : 0) ? 1 : s6 == (was > 0 ? was - 1 : game_list - 1) ? -1 : 0;
        cur[side] = d == 0 ? team_in(s6) : order[(size_t)((pos_of(was) + d + count()) % count())];
        return cur[side];
    }
    int32_t blitz_v32_row(uint8_t* rdram, int32_t slot, int32_t dir) {
        (void)rdram;
        if (!ready()) return slot;
        return order[(size_t)((pos_of(team_in(slot)) + dir + count()) % count())];
    }
    // The credits page, once per run: the attract sequence's Players Inc screen (ovl_b func_802F0614: one full-screen
    // image, fade in, wait, fade out) run with our image (0x802F0650), at the start of its New Features screen
    // (0x802F02C4), after the title and legal screens (which Skip Intro Screens skips; this still shows).
    bool credits_pass = false, credits_shown = false;
    int32_t blitz_e32_screen_image(int32_t id) {
        if (!credits_pass) return id;
        int32_t idx = blitz::fs_file_index("credits.wms");
        return idx >= 0 ? (int32_t)(0x0A000000u | (uint32_t)(idx + 1)) : id;
    }
    void ovl_b_func_802F0614(uint8_t* rdram, recomp_context* ctx);
    void blitz_e32_credits(uint8_t* rdram, recomp_context* ctx) {
        if (credits_shown || blitz::fs_file_index("credits.wms") < 0) return;
        credits_shown = true;
        credits_pass = true;
        recomp_context saved = *ctx;
        ctx->r4 = 30;   // shown at least a second (frames at 30 Hz), then
        ctx->r5 = 240;  // until a button, or 8 seconds (func_8024CE30)
        ovl_b_func_802F0614(rdram, ctx);
        *ctx = saved;
        credits_pass = false;
    }
    // ovl_b 0x803017A8: the season schedule prints a team's list name (the moved team select names).
    int32_t blitz_e32_list_name(int32_t team) {
        return (int32_t)(blitz::engine32::now(0x80338E68) + 16 * (uint32_t)std::clamp<int32_t>(team, 0, teams - 1));
    }
    // ovl_b 0x802F5568 (after the game's copier read its 31 x 31 table): this game's two teams' clash.
    int32_t blitz_e32_clash(uint8_t* rdram) {
        int a = MEM_W(0, (gpr)(int32_t)0x802EA434), b = MEM_W(4, (gpr)(int32_t)0x802EA434);
        return td::store().clashes((size_t)team_in(std::max(0, a)), (size_t)team_in(std::max(0, b))) ? 1 : 0;
    }
}
#else
#include <cstdint>
extern "C" {
    int32_t blitz_v32_moved(uint8_t*, int32_t, int32_t s6) { return s6; }
    int32_t blitz_v32_row(uint8_t*, int32_t slot, int32_t) { return slot; }
    int32_t blitz_e32_clash(uint8_t*) { return 0; }
    int32_t blitz_e32_list_name(int32_t team) { return team; }
    int32_t blitz_e32_screen_image(int32_t id) { return id; }
    void blitz_e32_refresh(uint8_t*) {}
    void blitz_e32_on_ovl_b(uint8_t*) {}
    void blitz_e32_on_ovl_a(uint8_t*) {}
}
namespace blitz {
    void e32_register_files() {}
    void e32_register_app_files() {}
}
#endif
