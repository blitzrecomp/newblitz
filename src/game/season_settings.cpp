// The Season settings tab: Season Mode (2026 app, adv_mode.cpp) and the tiebreakers for the classic
// Season (season_tiebreak.cpp).
//
// League (1.0.6, branch league-select): NFL 2026 or an installed league pack (a league pak's packs/<id>, the app folder's
// packs/<id>), a cycling row so any number fits. The list is read again whenever a pak goes in or out (content_dirs'
// generation) and when Settings is shown again, so a pak added at run time appears without a relaunch. Rosters and
// Lineups are the NFL's and hide while a league pack is chosen. Before 1.0.6 a league pack was a Rosters choice: a saved
// Rosters that names a pack becomes League = that pack, Rosters = Auto (the load callback below), and Lineups' Built-In
// and Week N become Current (rosters_live.cpp).
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <functional>
#include <map>

#include "json/json.hpp"
#include "recompui/config.h"
#include "librecomp/game.hpp"
#include "util/file.h"
#include "season_pak.h"
#include "league_migrate.h"

namespace blitz {
#if BLITZ_MODERN
    void add_season_mode_option(recomp::config::Config& config);  // adv_mode.cpp
    void add_game_option(recomp::config::Config& config);         // adv_mode.cpp: New Blitz / Original 1999
    bool original_game();
    void add_lineups_option(recomp::config::Config& config);      // rosters_live.cpp
    void add_live_rosters_option(recomp::config::Config& config);
    void roster_room_open();                                       // roster_room.cpp
    void original_settings_visibility();                           // adv_mode.cpp
    void post_status(const std::string& msg);                      // notices.cpp
    void post_notice(const std::string& msg);
    std::vector<recompui::config::CycleChoice> league_choices();
    void season_option_changed(const char* id, recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant prev,
                               recomp::config::OptionChangeContext ctx);
#endif
}

namespace {
    constexpr const char* opt_tiebreakers = "tiebreakers";
    enum class Tiebreakers : uint32_t { Classic = 0, NFL = 1 };
    constexpr const char* opt_rosters = "rosters";
    enum class Rosters : uint32_t { Auto = 0, Blitz2026 = 1, RomOwn = 2 };
    constexpr const char* opt_league = "league";  // "" NFL 2026, else an installed pack's id

#if BLITZ_MODERN
    // NFL 2026 is the League row's first choice. BRANDING: the NFL season's name in the League row
    constexpr const char* nfl_label = "NFL 2026";

    // The installed packs besides Blitz 2026 (a league pak like the CFL's or Pac-12's, bundled, in a pak or in
    // <app folder>/packs), by id, with their titles. Read again when a pak goes in or out, and when Settings is shown again
    // (the row asks every frame it's on screen, so a pause in the asking means it was away).
    struct Pack {
        std::string id, title;
    };
    std::mutex packs_mutex;
    std::vector<Pack> packs;
    std::map<std::string, std::string> titles_seen;  // every pack's title read this run, for "X isn't installed now"
    int packs_generation = -1;
    std::chrono::steady_clock::time_point packs_asked{};

    std::vector<Pack> scan_packs() {
        std::vector<Pack> out;
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
                if (id == "blitz2026" || id == "examples" ||
                    std::find_if(out.begin(), out.end(), [&](const Pack& p) { return p.id == id; }) != out.end()) continue;
                std::ifstream in{dir / "pack.json"};
                if (!in) continue;
                try {
                    nlohmann::json j = nlohmann::json::parse(in);
                    if (j.value("format", "") != "blitz-pack/2") continue;
                    out.push_back({id, j.value("title", id)});
                }
                catch (...) {
                }
            }
        }
        return out;
    }

    // Call with packs_mutex held.
    void packs_fresh() {
        auto now = std::chrono::steady_clock::now();
        bool away = now - packs_asked > std::chrono::seconds(2);
        packs_asked = now;
        int gen = blitz::content_dirs_generation();
        if (gen == packs_generation && !away) return;
        packs_generation = gen;
        packs = scan_packs();
        for (const Pack& p : packs) titles_seen[p.id] = p.title;
    }

    std::string league_value() {
        return std::get<std::string>(recompui::config::get_config("season").get_option_value(opt_league));
    }

    // From an old season.json (before 1.0.6): Rosters named a pack. Read as Rosters = Auto, then League = that pack once
    // the file is loaded. Lineups' old choices (Built-In, Week N) read as Current, and the file is written again so it
    // says so.
    std::string legacy_league;
    bool lineups_legacy = false;
#endif
}

namespace blitz {
    void create_season_tab() {
        recomp::config::Config& c = recompui::config::create_config_tab("Teams", "season", false);  // the rosters, lineups and Season: "Teams" (the team's settings pass), id kept
#if BLITZ_MODERN
        add_game_option(c);
        c.add_string_option(
            opt_league,
            "League",
            // BRANDING: the NFL season's name in the League row's description
            "Whose league you play: <b>NFL 2026</b>, or any league pack you've added (Add-Ons), like the CFL's."
            "<br /><br />"
            "A league pack brings its own teams and plays Arcade and Tournament (Season, Advanced and the Play Editor "
            "are the NFL's). Rosters and Lineups are the NFL's, so they hide while a league pack is chosen."
            "<br /><br />"
            "Left and right look through them; <b>× chooses</b> (or tap the name). Each league keeps its own saves. "
            "<b>Takes effect the next time the game starts</b>: during a game, New Blitz asks to restart now or later.",
            "");
        recompui::config::set_option_cycle("season", opt_league, [] { return blitz::league_choices(); }, true);  // browse, then ×
        c.add_option_change_callback(opt_league, [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant prev,
                                                    recomp::config::OptionChangeContext ctx) {
            original_settings_visibility();  // Rosters and Lineups are the NFL's
            blitz::season_option_changed(opt_league, cur, prev, ctx);
        });
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
            "Each roster choice keeps its own saves. <b>Takes effect the next time the game starts</b>: during a game, "
            "New Blitz asks to restart.",
            // BRANDING: "Blitz 2026" is the roster pack's shown name
            {{Rosters::Auto, "auto", "Auto"}, {Rosters::Blitz2026, "blitz2026", "2026 Season"}, {Rosters::RomOwn, "rom", "ROM's Own"}},
            Rosters::Auto);
        // A pack's id here is a League from before 1.0.6 (see legacy_league).
        c.on_json_parse_option(opt_rosters, [](const nlohmann::json& v) -> recomp::config::ConfigValueVariant {
            auto r = blitz::league_migrate::rosters(v.is_string() ? v.get<std::string>() : std::string{});
            if (!r.league.empty()) {
                legacy_league = r.league;
                fprintf(stderr, "[league] season.json's Rosters names the pack '%s': League from now on\n", r.league.c_str());
            }
            return r.value;
        });
        c.add_option_change_callback(opt_rosters, [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant prev,
                                                     recomp::config::OptionChangeContext ctx) {
            blitz::season_option_changed(opt_rosters, cur, prev, ctx);
        });
        add_lineups_option(c);
        c.on_json_parse_option("lineups", [](const nlohmann::json& v) -> recomp::config::ConfigValueVariant {
            auto l = blitz::league_migrate::lineups(v.is_string() ? v.get<std::string>() : std::string{});
            if (l.changed) lineups_legacy = true;  // Built-In or a pinned week: Current now
            return l.value;
        });
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
        // A hidden mark: the next new season isn't asked "Standard or Advanced?" (that answer just restarted the game).
        c.add_bool_option("season_ask_skip", "", "", false, true);
        c.set_load_callback([] {
            recomp::config::Config& season = recompui::config::get_config("season");
            bool save = lineups_legacy;
            if (!legacy_league.empty()) {
                std::string league = blitz::league_migrate::league_after(league_value(), legacy_league);
                if (league != league_value()) season.update_option_value(opt_league, league);
                fprintf(stderr, "[league] moved from Rosters: League %s, Rosters Auto\n", legacy_league.c_str());
                legacy_league.clear();
                save = true;
            }
            if (lineups_legacy) fprintf(stderr, "[league] Lineups' Built-In or Week N is Current now\n");
            lineups_legacy = false;
            if (save) season.save_config();
        });
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
    // Read once, when the roster pack loads at game start (content_pack_tp.cpp): 0 Auto, 1 Blitz 2026, 2 the ROM's own.
    uint32_t rosters_setting() {
        if (original_game()) return 2;  // the original game: the ROM's own teams
        return std::get<uint32_t>(recompui::config::get_config("season").get_option_value(opt_rosters));
    }

    // League's pack id when it names an installed pack (not in the original game), else empty: the NFL.
    std::string rosters_pack_id() {
        if (original_game()) return {};
        std::string id = league_value();
        if (id.empty()) return {};
        std::lock_guard lock{packs_mutex};
        packs_generation = -1;  // at a game's start: what's there now
        packs_fresh();
        for (const Pack& p : packs) if (p.id == id) return id;
        fprintf(stderr, "[league] %s isn't installed: NFL 2026\n", id.c_str());
        return {};
    }

    // From the roster question after Load ROM / Change ROM (main.cpp).
    void set_rosters(uint32_t value) {
        recomp::config::Config& c = recompui::config::get_config("season");
        c.update_option_value(opt_rosters, value);
        c.save_config();
    }

    // The League row's choices: NFL 2026, then every installed pack by id.
    std::vector<recompui::config::CycleChoice> league_choices() {
        std::lock_guard lock{packs_mutex};
        packs_fresh();
        std::vector<recompui::config::CycleChoice> out{{"", nfl_label}};
        for (const Pack& p : packs) out.push_back({p.id, p.title});
        return out;
    }

    // A pack's title ("CFL 2026"), NFL 2026 for "", else the id.
    std::string league_title(const std::string& id) {
        if (id.empty()) return nfl_label;
        std::lock_guard lock{packs_mutex};
        packs_fresh();
        auto it = titles_seen.find(id);
        return it == titles_seen.end() ? id : it->second;
    }

    std::string league_id() { return league_value(); }
    bool league_selected() { return !league_value().empty(); }

    // League's pack, still installed? When it's gone (removed in Add-Ons, its pak gone), League goes back to NFL 2026 and
    // returns what to tell the player ("" while it's there). now: read the list again first.
    std::string league_check_installed(bool now) {
        std::string id = league_value(), title;
        if (id.empty()) return {};
        {
            std::lock_guard lock{packs_mutex};
            if (now) packs_generation = -1;
            packs_fresh();
            for (const Pack& p : packs) if (p.id == id) return {};
            auto it = titles_seen.find(id);
            title = it == titles_seen.end() ? "The league pack \"" + id + "\"" : it->second;
        }
        recomp::config::Config& season = recompui::config::get_config("season");
        season.update_option_value(opt_league, std::string{});
        season.save_config();
        original_settings_visibility();
        fprintf(stderr, "[league] %s isn't installed now: League is NFL 2026\n", id.c_str());
        return title + " isn't installed now: League is " + nfl_label + " again";
    }

    void set_league(const std::string& id) {
        recomp::config::Config& c = recompui::config::get_config("season");
        c.update_option_value(opt_league, id);
        c.save_config();
    }

    // A setting that takes effect at the next start changed (league_select.cpp asks to restart during a game). The PSP:
    // nothing more.
    std::function<void(const char*, recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant)> season_change_hook;
    void season_option_changed(const char* id, recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant prev,
                               recomp::config::OptionChangeContext ctx) {
        if (ctx != recomp::config::OptionChangeContext::Permanent || cur == prev) return;
        // Saved now, not when the tab closes: a restart (the prompt's, the header's) closes Settings without its save.
        recompui::config::get_config("season").save_config();
        if (season_change_hook) season_change_hook(id, cur, prev);
    }

    // The main menu with a league pack (content_pack_tp.cpp, the game thread): what it plays, once. league_select.cpp's
    // modal where there is one (with a way back to NFL 2026), else the notice.
    std::function<void(const std::string&, const std::string&)> league_menu_hook;
    void league_menu_notice(const std::string& title, const std::string& modes) {
        if (league_menu_hook) {
            league_menu_hook(title, modes);
            return;
        }
        post_notice(title + " plays " + modes + ". To play a Season, switch League to " + nfl_label + " in Settings → Teams.");
    }

    // A message from the league and pak code (a pack that didn't load, a pak that didn't install): our modal where there is
    // one (league_select.cpp: queued, shown at a menu, never a system dialog), else the notice (the PSP).
    std::function<void(const std::string&, const std::string&)> league_notice_hook;
    void league_notice(const std::string& header, const std::string& msg) {
        if (league_notice_hook) {
            fprintf(stderr, "[notice] %s\n", msg.c_str());
            league_notice_hook(header, msg);
            return;
        }
        post_notice(msg);
    }

    // A new season begins (the game's season record reset, adv_season.cpp / season32.cpp, on the game thread).
    std::function<void()> new_season_hook;
    void season_new_started() {
        if (new_season_hook) new_season_hook();
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
