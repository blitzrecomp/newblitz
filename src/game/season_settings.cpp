// The Season settings tab: Season Mode (2026 app, adv_mode.cpp) and the tiebreakers for the classic
// Season (season_tiebreak.cpp).
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "recompui/config.h"
#include "librecomp/game.hpp"
#include "util/file.h"
#include "season_pak.h"

namespace blitz {
#if BLITZ_MODERN
    void add_season_mode_option(recomp::config::Config& config);  // adv_mode.cpp
    void add_game_option(recomp::config::Config& config);         // adv_mode.cpp: New Blitz / Original 1999
    bool original_game();
    void add_lineups_option(recomp::config::Config& config);      // rosters_live.cpp
    void add_live_rosters_option(recomp::config::Config& config);
    void roster_room_open();                                       // roster_room.cpp
#endif
}

namespace {
    constexpr const char* opt_tiebreakers = "tiebreakers";
    enum class Tiebreakers : uint32_t { Classic = 0, NFL = 1 };
    constexpr const char* opt_rosters = "rosters";
    enum class Rosters : uint32_t { Auto = 0, Blitz2026 = 1, RomOwn = 2 };
    std::vector<std::string> extra_packs;  // Rosters choices 3, 4, ...: other installed packs by id

#if BLITZ_MODERN
    // Packs besides Blitz 2026 (a league pak like Blitz: Pac-12), bundled or in <app folder>/packs, by title.
    std::vector<recomp::config::ConfigOptionEnumOption> pack_choices() {
        std::vector<recomp::config::ConfigOptionEnumOption> out;
        std::vector<std::filesystem::path> roots;
        for (const auto& c : blitz::content_dirs()) roots.push_back(c / "packs");
        roots.push_back(recomp::get_config_path() / "packs");
        for (const std::filesystem::path& root : roots) {
            std::error_code ec;
            if (!std::filesystem::is_directory(root, ec)) continue;
            std::vector<std::filesystem::path> dirs;
            for (const auto& e : std::filesystem::directory_iterator(root, ec)) dirs.push_back(e.path());
            std::sort(dirs.begin(), dirs.end());
            for (const auto& dir : dirs) {
                std::string id = dir.filename().string();
                if (id == "blitz2026" || id == "examples" || std::find(extra_packs.begin(), extra_packs.end(), id) != extra_packs.end()) continue;
                std::ifstream in{dir / "pack.json"};
                if (!in) continue;
                std::string title = id;
                try {
                    nlohmann::json j = nlohmann::json::parse(in);
                    if (j.value("format", "") != "blitz-pack/2") continue;
                    title = j.value("title", id);
                }
                catch (...) {
                    continue;
                }
                out.emplace_back((uint32_t)(3 + extra_packs.size()), id, title);
                extra_packs.push_back(id);
            }
        }
        return out;
    }
#endif
}

namespace blitz {
    void create_season_tab() {
        recomp::config::Config& c = recompui::config::create_config_tab("Teams", "season", false);  // the rosters, lineups and Season: "Teams" (the team's settings pass), id kept
#if BLITZ_MODERN
        add_game_option(c);
        c.add_enum_option(
            opt_rosters,
            "Rosters",
            "Whose teams and players you play with."
            "<br /><br />"
#if BLITZ_TEAMPLUS
            "<b>Auto</b>: Blitz 2026 on the Team Extension ROM; a community release with its own branding "
            "(like Resurrection's Gold or Silver Edition) plays its own rosters."
#else
            "<b>Auto</b>: the 2026 Season's, or a community ROM's own when it has its own branding."
#endif
            "<br /><br />"
            "<b>2026 Season</b>: the 2026 Season's rosters, ratings and team order, on any ROM."
            "<br /><br />"
            "<b>ROM's Own</b>: whatever teams the loaded ROM has."
            "<br /><br />"
            "Any other installed pack is listed too. A league pack brings its own teams and plays Arcade only."
            "<br /><br />"
            "Each roster choice keeps its own saves. <b>Takes effect the next time the game starts.</b>",
            [] {
                std::vector<recomp::config::ConfigOptionEnumOption> o = {
                    // BRANDING: "Blitz 2026" is the roster pack's shown name
                    {Rosters::Auto, "auto", "Auto"}, {Rosters::Blitz2026, "blitz2026", "2026 Season"}, {Rosters::RomOwn, "rom", "ROM's Own"}};
                for (auto& c : pack_choices()) o.push_back(c);
                return o;
            }(),
            Rosters::Auto);
        add_lineups_option(c);
        add_live_rosters_option(c);
        c.add_bool_option("roster_room", "Roster Room",
                          "Set any team's default crew: who plays each slot, over this week's lineup. It opens in your "
                          "browser (served by the app, on this computer only); <b>Lock In</b> saves and closes it. Your "
                          "crews play from the next game, in every mode, and go into an exported ROM.", false);
        recompui::config::set_option_action("season", "roster_room", "Open");
        c.add_option_change_callback("roster_room", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                       recomp::config::OptionChangeContext ctx) {
            if (ctx != recomp::config::OptionChangeContext::Permanent || !std::get<bool>(cur)) return;
            recompui::config::get_config("season").update_option_value("roster_room", false);
            blitz::roster_room_open();
        });
        add_season_mode_option(c);
#endif
        c.add_enum_option(
            opt_tiebreakers,
            "Tiebreakers",
            "How Season standings and playoff seeds break ties."
            "<br /><br />"
            "<b>Classic</b>: the original game's order (win %, then home record and point differential)."
            "<br /><br />"
            "<b>NFL</b>: the NFL's procedures (head-to-head, division and conference records, common "
            "games, strength of victory and schedule, points). Standings and seeds only: saves are unchanged."
#if BLITZ_MODERN
            "<br /><br />Advanced seasons always use NFL tiebreakers."
#endif
            ,
            {{Tiebreakers::Classic, "Classic", "Classic"}, {Tiebreakers::NFL, "NFL", "NFL"}},
            Tiebreakers::Classic);
    }

#if BLITZ_MODERN
    // Read once, when the roster pack loads at game start (content_pack_tp.cpp).
    uint32_t rosters_setting() {
        if (original_game()) return 2;  // the original game: the ROM's own teams
        return std::get<uint32_t>(recompui::config::get_config("season").get_option_value(opt_rosters));
    }

    // The chosen pack's id when Rosters names another installed pack, else empty.
    std::string rosters_pack_id() {
        uint32_t v = rosters_setting();
        return v >= 3 && v - 3 < extra_packs.size() ? extra_packs[v - 3] : std::string{};
    }

    // From the roster question after Load ROM / Change ROM (main.cpp).
    void set_rosters(uint32_t value) {
        recomp::config::Config& c = recompui::config::get_config("season");
        c.update_option_value(opt_rosters, value);
        c.save_config();
    }
#endif

    // Read each time (it takes effect at the next standings or seeding). BLITZ_NFL_TIEBREAKS=0/1
    // overrides it for test runs.
    bool nfl_tiebreaks() {
        if (const char* env = getenv("BLITZ_NFL_TIEBREAKS"); env != nullptr && *env != '\0') {
            return env[0] == '1';
        }
        // Look the config up each time: configs live in a growing list, so a kept reference can dangle.
        return std::get<uint32_t>(recompui::config::get_config("season").get_option_value(opt_tiebreakers)) == (uint32_t)Tiebreakers::NFL;
    }
}
