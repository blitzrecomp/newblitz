// Blitz Party tournaments (both apps): the game's own Tournament mode (up to 8 named entrants, byes,
// single game or best of 3, a champion screen), made quick and safe for a group.
//   - Settings → Party: type the names, pick single or best of 3, Create. The next time the
//     Tournament menu opens, the bracket is there (no letter-by-letter name entry).
//   - The tournament is autosaved to <app folder>/tournament.bin after every game and comes back
//     when the Tournament menu opens after a restart (the game itself only saves to the Controller
//     Pak when asked).
//   - Party tournaments play the shortest quarters (Tournament's own quarter option).
//   - The Party tab shows the bracket and each entrant's record; a result posts a short notice.
// Game data (main, same in vanilla and team+; research in build/analysis/tournament):
//   0x802B8800  tournament, 0x238 bytes: +0 name (6), +6 u16 entrants, +8 series flag (best of 3),
//               +0xC entrant[8] x 0x3C {+0 name (10), +0xA u16 games, +0xC u16 wins, +0x10/+0x12 u16
//               points for/against, ...}, +0x1EC node[15] x 5 {entrant or winner (-1), parent, left,
//               right, series wins}; node 0 is the final.
//   0x8028BF48  flags word: 0x8000 = a tournament is loaded
//   0x802B87E2  Tournament's options (u16): bits 4-6 quarter length index into {1, 2, 4, 6}
// Hooks (blitz2000.toml): Tournament menu entry main 0x80257EB0, after a game is recorded ovl_a
// 0x80303434 (after the jal to main 802617C4, which returns early on a tie).
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "recomp.h"
#include "util/file.h"
#include "recompui/config.h"
#include "recompui/recompui.h"

namespace blitz {
    void post_notice(const std::string& msg);  // notices.cpp
    void post_status(const std::string& msg);  // notices.cpp: a toast
}

namespace {
    constexpr uint32_t tourney = 0x802B8800, tourney_size = 0x238;
    constexpr uint32_t loaded_flag = 0x8028BF48, options = 0x802B87E2, current_match = 0x8028BF4C;
    constexpr uint32_t entrants_at = 0xC, entrant_size = 0x3C, nodes_at = 0x1EC;
    constexpr const char* tab = "party";

    std::mutex snapshot_mutex;
    std::vector<uint8_t> snapshot;  // the latest tournament image, for the Party tab (UI thread)

    uint8_t rb(uint8_t* rdram, uint32_t a) { return (uint8_t)MEM_B(0, (gpr)(int32_t)a); }
    void wb(uint8_t* rdram, uint32_t a, uint8_t v) { MEM_B(0, (gpr)(int32_t)a) = (int8_t)v; }

    std::filesystem::path save_file() { return recompui::file::get_app_folder_path() / "tournament.bin"; }

    std::vector<uint8_t> read_image(uint8_t* rdram) {
        std::vector<uint8_t> img(tourney_size);
        for (uint32_t i = 0; i < tourney_size; i++) img[i] = rb(rdram, tourney + i);
        return img;
    }

    void write_image(uint8_t* rdram, const std::vector<uint8_t>& img) {
        for (uint32_t i = 0; i < tourney_size && i < img.size(); i++) wb(rdram, tourney + i, img[i]);
    }

    uint16_t be16(const std::vector<uint8_t>& img, size_t o) { return (uint16_t)(img[o] << 8 | img[o + 1]); }
    std::string text(const std::vector<uint8_t>& img, size_t o, size_t n) {
        std::string s;
        for (size_t i = 0; i < n && img[o + i]; i++) s += (char)img[o + i];
        return s;
    }

    // tournament.bin is written whole to a temp file and renamed, so a crash mid-write keeps the last bracket.
    void save_image(const std::vector<uint8_t>& img) {
        std::filesystem::path path = save_file(), tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(img.data()), img.size());
            if (!f) {
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                fprintf(stderr, "[party] couldn't save %s\n", path.string().c_str());
                return;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
    }

    // A saved bracket goes into game memory only if its numbers are ones the game's tables can take.
    bool image_valid(const std::vector<uint8_t>& img) {
        if (img.size() != tourney_size) return false;
        int n = be16(img, 6);
        if (n < 2 || n > 8) return false;
        for (int i = 0; i < 15; i++) {
            const int8_t* node = reinterpret_cast<const int8_t*>(img.data() + nodes_at + 5 * i);
            if (node[0] < -1 || node[0] >= n) return false;
            for (int k = 1; k <= 3; k++) {
                if (node[k] < -1 || node[k] > 14) return false;
            }
        }
        return true;
    }

    void remember(const std::vector<uint8_t>& img) {
        std::lock_guard lock{snapshot_mutex};
        snapshot = img;
    }

    recomp::config::Config& config() { return recompui::config::get_config(tab); }

    // A new tournament image from the Party tab (the same bracket the game's own setup builds).
    std::vector<uint8_t> build_image(const std::vector<std::string>& names, bool best_of_3, bool shuffle) {
        std::vector<uint8_t> img(tourney_size, 0);
        const char* title = "PARTY";
        for (int i = 0; title[i] && i < 5; i++) img[i] = (uint8_t)title[i];
        int n = (int)names.size();
        img[6] = 0;
        img[7] = (uint8_t)n;
        img[8] = best_of_3 ? 1 : 0;
        for (int e = 0; e < n; e++) {
            for (size_t k = 0; k < names[e].size() && k < 9; k++) img[entrants_at + e * entrant_size + k] = (uint8_t)names[e][k];
        }
        // The bracket template (ovl_c 0x802F4320) and the game's pruning for fewer than 8 (802F263C).
        static const int8_t tmpl[15][5] = {{-1, -1, 1, 2, 0}, {-1, 0, 3, 4, 0}, {-1, 0, 5, 6, 0}, {-1, 1, 7, 8, 0}, {-1, 1, 9, 10, 0},
                                           {-1, 2, 11, 12, 0}, {-1, 2, 13, 14, 0}, {-1, 3, -1, -1, 0}, {-1, 3, -1, -1, 0}, {-1, 4, -1, -1, 0},
                                           {-1, 4, -1, -1, 0}, {-1, 5, -1, -1, 0}, {-1, 5, -1, -1, 0}, {-1, 6, -1, -1, 0}, {-1, 6, -1, -1, 0}};
        int8_t node[15][5];
        std::copy(&tmpl[0][0], &tmpl[0][0] + 75, &node[0][0]);
        for (int k = 6; k >= 1; k--) {
            if (n < k + 2) {
                node[node[k][2]][1] = -1;
                node[node[k][3]][1] = -1;
                node[k][2] = node[k][3] = -1;
            }
        }
        std::vector<int> leaves;
        for (int k = 0; k < 15; k++) {
            if (node[k][2] == -1 && (k == 0 || node[k][1] != -1)) leaves.push_back(k);
        }
        std::vector<int> order(n);
        for (int e = 0; e < n; e++) order[e] = e;
        if (shuffle) std::shuffle(order.begin(), order.end(), std::mt19937{std::random_device{}()});
        for (size_t i = 0; i < leaves.size() && i < order.size(); i++) node[leaves[i]][0] = (int8_t)order[i];
        for (int k = 0; k < 15; k++)
            for (int f = 0; f < 5; f++) img[nodes_at + k * 5 + f] = (uint8_t)node[k][f];
        return img;
    }

    std::vector<uint8_t> pending;  // a tournament made in the Party tab, waiting for the menu
    std::mutex pending_mutex;
}

namespace blitz {
    std::string party_standings();
}

namespace blitz {
    // Settings → Party.
    void create_party_tab() {
        recomp::config::Config& c = recompui::config::create_config_tab("Bracket", tab, false);  // "Party" clashed with Quick Setup's Party preset; id kept
        c.add_string_option("names", "Player Names",
                            "2 to 8 names, separated by commas (up to 9 letters each), e.g. \"SAM, ALEX, JO, PAT\". "
                            "Teams are picked before each game, so these are the people.", "");
        c.add_enum_option("series", "Each Round", "One game per match, or the first to win two.",
                          std::vector<recomp::config::ConfigOptionEnumOption>{{0, "single", "One game"}, {1, "best3", "Best of 3"}}, 0u);
        c.add_enum_option("seeding", "Matchups", "Random, or in the order typed (1 v 2, 3 v 4, ...).",
                          std::vector<recomp::config::ConfigOptionEnumOption>{{0, "random", "Random"}, {1, "ordered", "As typed"}}, 0u);
        c.add_bool_option("create", "Create Tournament",
                          "Sets the bracket up from the names above. Then close Settings and open Tournament from the game's main "
                          "menu: choose PLAY and the bracket is there. Quarters are the shortest length; the bracket is saved "
                          "after every game.", false);
        c.add_option_change_callback("create", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                  recomp::config::OptionChangeContext ctx) {
            if (ctx != recomp::config::OptionChangeContext::Permanent || !std::get<bool>(cur)) return;
            recomp::config::Config& c = config();
            c.update_option_value("create", false);
            // A held button presses the switch again and again (the team got 86 "Tournament ready" boxes): one per 2 s.
            static auto last = std::chrono::steady_clock::time_point{};
            auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(2)) return;
            last = now;
            std::vector<std::string> names;
            std::string all = std::get<std::string>(c.get_option_value("names")), cur_name;
            auto flush = [&] {
                std::string s;
                for (char ch : cur_name) if (ch != ' ' || !s.empty()) s += (char)toupper((unsigned char)ch);
                while (!s.empty() && s.back() == ' ') s.pop_back();
                if (!s.empty()) names.push_back(s.substr(0, 9));
                cur_name.clear();
            };
            for (char ch : all) {
                if (ch == ',') flush();
                else cur_name += ch;
            }
            flush();
            if (names.size() < 2 || names.size() > 8) {
                blitz::post_notice("Tournament: type 2 to 8 names in Players, separated by commas, then Create.");
                return;
            }
            bool best3 = std::get<uint32_t>(c.get_option_value("series")) == 1;
            bool shuffle = std::get<uint32_t>(c.get_option_value("seeding")) == 0;
            {
                std::lock_guard lock{pending_mutex};
                pending = build_image(names, best3, shuffle);
            }
            remember(pending);
            // (No arrows: the toast's display font doesn't have them.)
            blitz::post_status("Tournament ready: " + std::to_string(names.size()) + " players. Open Tournament on the main menu");
        });
        recompui::config::set_option_action(tab, "create", "Create");
        c.add_bool_option("standings", "Show Standings", "The bracket so far: each player's record and points, and the champion.", false);
        c.add_option_change_callback("standings", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                     recomp::config::OptionChangeContext ctx) {
            if (ctx != recomp::config::OptionChangeContext::Permanent || !std::get<bool>(cur)) return;
            config().update_option_value("standings", false);
            recompui::open_info_prompt("Standings", blitz::party_standings(), "OK", [] {}, recompui::ButtonStyle::Secondary);
        });
        recompui::config::set_option_action(tab, "standings", "Show");
    }

    void party_tab_refresh() {}  // standings open on demand (Show Standings)

    // The bracket as text, for the Party tab (UI thread).
    std::string party_standings() {
        std::vector<uint8_t> img;
        {
            std::lock_guard lock{snapshot_mutex};
            img = snapshot;
        }
        if (img.size() < tourney_size) {  // the game hasn't opened the menu yet: the autosave
            std::ifstream f{save_file(), std::ios::binary};
            img.assign(std::istreambuf_iterator<char>(f), {});
        }
        if (img.size() < tourney_size || img[7] < 2 || img[7] > 8) return "No tournament yet.";
        int n = img[7];
        std::string out;
        auto name = [&](int e) { return e >= 0 && e < n ? text(img, entrants_at + e * entrant_size, 10) : std::string("-"); };
        int8_t champ = (int8_t)img[nodes_at + 0];
        if (champ >= 0) out += "CHAMPION: " + name(champ) + "\n\n";
        for (int e = 0; e < n; e++) {
            size_t r = entrants_at + e * entrant_size;
            int games = be16(img, r + 0xA), wins = be16(img, r + 0xC);
            out += name(e) + "  " + std::to_string(wins) + "-" + std::to_string(games - wins) + "  (" + std::to_string(be16(img, r + 0x10)) +
                   " for, " + std::to_string(be16(img, r + 0x12)) + " against)\n";
        }
        return out;
    }
}

extern "C" {
    // Tournament menu entry (main 0x80257EB0): a tournament made in the Party tab, or the autosave.
    void blitz_tournament_menu(uint8_t* rdram) {
        std::vector<uint8_t> img;
        bool party = false;
        {
            std::lock_guard lock{pending_mutex};
            if (!pending.empty()) {
                img.swap(pending);
                party = true;
            }
        }
        if (img.empty() && !(MEM_W(0, (gpr)(int32_t)loaded_flag) & 0x8000)) {
            std::ifstream f{save_file(), std::ios::binary};
            std::vector<uint8_t> saved{std::istreambuf_iterator<char>(f), {}};
            if (image_valid(saved)) {
                img = std::move(saved);
                fprintf(stderr, "[party] tournament restored from %s\n", save_file().string().c_str());
            }
        }
        if (!img.empty()) {
            write_image(rdram, img);
            MEM_W(0, (gpr)(int32_t)loaded_flag) = 0x8000;
            if (party) {
                save_image(img);
                fprintf(stderr, "[party] tournament created\n");
                // The menu's cursor (0 PLAY ... 3 NEW, the same in both apps) can be left on NEW, where ✕ would
                // offer to wipe the bracket just made: start on PLAY.
                MEM_W(0, (gpr)(int32_t)0x8028B9A0) = 0;
            }
        }
        if (MEM_W(0, (gpr)(int32_t)loaded_flag) & 0x8000) {
            std::vector<uint8_t> now = read_image(rdram);
            remember(now);
            // A party tournament (named PARTY) plays the shortest quarters, every time the menu opens:
            // loading the system options from the pak would put the old length back.
            if (text(now, 0, 6) == "PARTY") {
                uint16_t o = (uint16_t)MEM_HU(0, (gpr)(int32_t)options) & ~0x70;  // quarter index 0
                MEM_H(0, (gpr)(int32_t)options) = (int16_t)o;
            }
        }
    }

    // After the game recorded a tournament result (ovl_a 0x80303434): save, and say what happened.
    void blitz_tournament_recorded(uint8_t* rdram) {
        if (!(MEM_W(0, (gpr)(int32_t)loaded_flag) & 0x8000)) return;
        std::vector<uint8_t> before;
        {
            std::lock_guard lock{snapshot_mutex};
            before = snapshot;
        }
        std::vector<uint8_t> img = read_image(rdram);
        save_image(img);
        remember(img);
        int score[2] = {0, 0};
        for (int s = 0; s < 2; s++)
            for (int q = 0; q < 7; q++) score[s] += MEM_W(4 * q + 0x1C * s, (gpr)(int32_t)0x802EA43C);
        if (score[0] == score[1]) {
            blitz::post_notice("Tie game: tournament games can't end in a tie, so this match is played again.");
        }
        fprintf(stderr, "[party] tournament saved after a game (%d-%d)\n", score[0], score[1]);
    }
}
