// Advanced mode switch for the 2026 flavour (docs/ADVANCED_SEASON.md §6).
//
// Modern (the default) is team+'s game: the pak's own save format, team+'s team order, and Season in
// the six 1999 divisions (32 teams since season32.cpp; older seasons keep 31). Advanced has today's
// eight divisions, uses its own saves (saves/advanced/<pack>/) and puts the teams in the pack's menu
// order. The setting lives on the "Season" settings tab and is read
// once, when the game starts: switching needs a restart, since saves and season data follow it.
// BLITZ_ADVANCED=0/1 overrides the setting (test runs).
#if BLITZ_MODERN
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "recompui/config.h"
#include "ultramodern/ultramodern.hpp"
#include "blitz_savestate.h"

#if BLITZ_MODERN
namespace blitz {
    bool league_active();  // content_pack_tp.cpp
    bool original_game();
    struct SeasonData;
    const SeasonData* pack_season();  // content_pack_tp.cpp: the active pack's season, else the built-in one
    bool season_content();  // content_dirs.cpp: the 2026 Season is here (bundled or the pak)
    bool league_selected();  // season_settings.cpp: League is a league pack, not NFL 2026
    void season_option_changed(const char* id, recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant prev,
                               recomp::config::OptionChangeContext ctx);
}
#endif

extern "C" {
    int blitz_adv = 0;  // blitz_hooks.h
}

namespace {
    constexpr const char* opt_mode = "season_mode";
    constexpr const char* opt_game = "game";  // New Blitz (the 2026 teams) or the original 1999 game

    std::once_flag original_once;
    bool original = false;

    void latch_original() {
        if (const char* env = getenv("BLITZ_ORIGINAL"); env != nullptr && *env != '\0') original = env[0] == '1';
        else original = std::get<uint32_t>(recompui::config::get_config("season").get_option_value(opt_game)) == 1 ||
                        !blitz::season_content();
        fprintf(stderr, "[mode] game: %s%s\n", original ? "Original 1999" : "New Blitz",
                original && !blitz::season_content() ? " (no 2026 Season: New Blitz comes with it)" : "");
    }

    // Shown as Standard / Advanced (the app is "Modern Blitz"); "Modern" stays the stored key in season.json.
    enum class SeasonMode : uint32_t { Modern = 0, Advanced = 1 };

    std::once_flag latch_once;
    bool advanced = false;

    void latch() {
        if (const char* env = getenv("BLITZ_ADVANCED"); env != nullptr && *env != '\0') {
            advanced = env[0] == '1';
        }
        else {
            advanced = std::get<uint32_t>(recompui::config::get_config("season").get_option_value(opt_mode)) == (uint32_t)SeasonMode::Advanced;
        }
#if BLITZ_MODERN
        if (advanced && blitz::original_game()) {
            advanced = false;  // the original game's Season is the game's own
        }
        if (advanced && blitz::league_active()) {
            advanced = false;  // a league pak has no Season (content_pack_tp.cpp)
            fprintf(stderr, "[mode] Advanced is off for a league pak\n");
        }
        // No season to play (no divisions or schedule: a public build without the 2026 Season pak): the game's own
        // Season, quietly, rather than an Advanced mode with no league (2026-11-13). The setting keeps its value, so
        // adding the pak brings Advanced back.
        if (advanced && blitz::pack_season() == nullptr) {
            advanced = false;
            fprintf(stderr, "[mode] Advanced needs a season: Standard for this run (no 2026 Season)\n");
        }
#endif
        blitz_adv = advanced ? 1 : 0;
        fprintf(stderr, "[mode] %s\n", advanced ? "Advanced" : "Modern");
    }

    // Restart in place (soft_restart.cpp): the game starts again, so Game and Season Mode are taken again from Settings.
    [[maybe_unused]] const bool mode_soft_reset = blitz::on_soft_reset("game mode", [] {
        blitz::reset_once(original_once);
        blitz::reset_once(latch_once);
        original = advanced = false;
        blitz_adv = 0;
    });
}

namespace blitz {
    // Original 1999 (team, 2026-10-25: Classic folded into the one app as a mode): the game as it shipped, on this
    // engine. The ROM's own 31 teams and art (no Houston, no 2026 art, uniforms or logos), the game's own Season (31
    // teams), and none of the team-select extras (uniform sets, crews, Pro Bowl). Fixed for the run, like Advanced.
    bool original_game() {
        // Without the 2026 Season (a public build before the pak, 2026-11-13) New Blitz isn't offered: the cartridge as
        // it shipped, rather than New Blitz's 32-team layout with a stand-in where Houston goes. The setting keeps its
        // value, so adding the pak brings New Blitz back.
        if (!ultramodern::is_game_started()) {  // the launcher (Blitz Builder) may still change it: no latch yet
            return std::get<uint32_t>(recompui::config::get_config("season").get_option_value(opt_game)) == 1 ||
                   !blitz::season_content();
        }
        std::call_once(original_once, latch_original);
        return original;
    }

    // What means nothing in the original game hides while Game is Original 1999 (config.cpp, once the tabs exist, and
    // on every change): the 2026 rosters and lineups, Season Mode, the Roster Room, and the team-select extras.
    // Also without the 2026 Season (a public build without the pak, 2026-11-13): what needs its rosters, lineups,
    // uniform sets or schedule hides until the pak is added (Add-Ons, Blitz Builder).
    void original_settings_visibility() {
        bool none = !blitz::season_content();
        bool o = std::get<uint32_t>(recompui::config::get_config("season").get_option_value(opt_game)) == 1 || none;
        recomp::config::Config& season = recompui::config::get_config("season");
        season.update_option_hidden(opt_game, none);  // only Original 1999 until the pak is added
        // League (1.0.6): the NFL or a league pack, New Blitz only. Rosters, Lineups and the Roster Room are the NFL's.
        bool league = !o && blitz::league_selected();
        season.update_option_hidden("league", o);
        season.update_option_hidden("rosters", o || league);
        for (const char* id : {"lineups", "roster_room"}) season.update_option_hidden(id, o || none || league);
        season.update_option_hidden("season_mode", o || none);
        // Content updates (rosters, uniforms and the posted paks, 1.0.6) mean something in either game: the paks come
        // either way, so the switch for them is always there.
        season.update_option_hidden("live_rosters", false);
        recomp::config::Config& rules = recompui::config::get_config("rules");
        for (const char* id : {"uniform_picker", "cpu_uniforms", "pick_crew"}) rules.update_option_hidden(id, o || none);
    }

    // The 2026 Season just arrived (Add-Ons: a download or Add…): New Blitz is what it's for, so Game turns to it (from
    // the next start, as any change of Game). Someone who wants the cartridge's own game switches back in Settings.
    void season_arrived() {
        recomp::config::Config& season = recompui::config::get_config("season");
        if (std::get<uint32_t>(season.get_option_value(opt_game)) != 0) {
            season.update_option_value(opt_game, 0u);
            season.save_config();
        }
        original_settings_visibility();
    }

    // Settings → Season → Game (season_settings.cpp), first on the tab.
    void add_game_option(recomp::config::Config& config) {
        config.add_enum_option(
            opt_game,
            "Game",
            // BRANDING: the Game setting's mode names (New Blitz / Original 1999)
            "<b>New Blitz</b>: today's 32 teams, rosters, uniforms and art, from the 2026 Season (Add-Ons)."
            "<br /><br />"
            "<b>Original 1999</b>: NFL Blitz 2000 as it shipped: its 31 teams and players, its art and its own "
            "Season. Keeps its own saves."
            "<br /><br />"
            "<b>Takes effect the next time the game starts</b>: during a game, New Blitz asks to restart.",
            {{0, "new", "New Blitz"}, {1, "original", "Original 1999"}}, 0);
        config.add_option_change_callback(opt_game, [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant prev,
                                                       recomp::config::OptionChangeContext ctx) {
            original_settings_visibility();
            blitz::season_option_changed(opt_game, cur, prev, ctx);  // during a game: asks to restart (league_select.cpp)
        });
    }

    // The Season Mode option, on the Season settings tab (season_settings.cpp).
    void add_season_mode_option(recomp::config::Config& config) {
        config.add_enum_option(
            opt_mode,
            "Season Mode",
            "<b>Standard</b>: the game's own Season, with all 32 teams in its six classic "
            "divisions (Houston in the AFC West, Seattle in the NFC West) and a 17-week schedule. "
            "Seasons saved with 31 teams keep playing as they were."
            "<br /><br />"
            "<b>Advanced</b>: all 32 teams in today's eight divisions, the real schedule from the content "
            "pack (2026: 18 weeks, 17 games), NFL tiebreakers and 7-team playoffs. Teams are listed in the "
            "pack's order. Advanced keeps its own saves."
            "<br /><br />"
            "A new season asks which one to play, starting from this setting."
            "<br /><br />"
            "<b>Takes effect the next time the game starts</b>: during a game, New Blitz asks to restart.",
            {{SeasonMode::Modern, "Modern", "Standard"}, {SeasonMode::Advanced, "Advanced", "Advanced"}},
            SeasonMode::Advanced);  // the default since 2026-10-21 (team): today's divisions and schedule
        config.add_option_change_callback(opt_mode, [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant prev,
                                                       recomp::config::OptionChangeContext ctx) {
            blitz::season_option_changed(opt_mode, cur, prev, ctx);  // during a game: asks to restart (league_select.cpp)
        });
    }

    // Fixes the mode for this run (called at game start; later calls return the same answer).
    bool advanced_mode() {
        std::call_once(latch_once, latch);
        return advanced;
    }
}

namespace {
    // Save states: the mode is latched at launch, so a state from the other mode can't be loaded.
    [[maybe_unused]] const bool registered_game = (blitz::savestate_register(
        "game",
        [] { return blitz::StateBlob{(uint8_t)(blitz::original_game() ? 1 : 0)}; },
        [](const blitz::StateBlob&) {},
        [](const blitz::StateBlob& b) -> std::string {
            if (b.size() != 1 || (b[0] != 0) == blitz::original_game()) return "";
            // BRANDING: mode name in a save-state message
            return std::string("This state was saved in ") + (b[0] ? "Original 1999" : "New Blitz") +
                   ". Change Settings → Teams → Game and restart the app to load it.";
        }), true);
    [[maybe_unused]] const bool registered = (blitz::savestate_register(
        "season_mode",
        [] { return blitz::StateBlob{(uint8_t)(blitz::advanced_mode() ? 1 : 0)}; },
        [](const blitz::StateBlob&) {},
        [](const blitz::StateBlob& b) -> std::string {
            if (b.size() != 1 || (b[0] != 0) == blitz::advanced_mode()) return "";
            return std::string("This state was saved with Season Mode set to ") + (b[0] ? "Advanced" : "Standard") +
                   ". Change it in Settings → Teams and restart the app to load it.";
        }), true);
}

extern "C" bool blitz_adv_on() {
    return blitz::advanced_mode();
}
#endif
