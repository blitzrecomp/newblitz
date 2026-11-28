// Settings → Teams from before 1.0.6, read into 1.0.6's rows (season_settings.cpp's season.json parse hooks). Header-only,
// so tests/league_migrate_test.cpp checks the same rules (`make test-league`).
//   Rosters: "auto", "blitz2026", "rom" stay; any other key was a league pack chosen there (before League existed): it
//            becomes League = that pack, Rosters = Auto.
//   Lineups: "week" (This Week, now Current) and "full" (Full Strength, now All Starters) stay; Built-In ("packed") and a
//            pinned week ("2026-w12") become Current.
#pragma once
#include <cstdint>
#include <string>

namespace blitz::league_migrate {
    // Rosters' values (season_settings.cpp) and Lineups' (rosters_live.cpp).
    inline constexpr uint32_t rosters_auto = 0, rosters_2026 = 1, rosters_rom = 2;
    inline constexpr uint32_t lineups_current = 0, lineups_full = 1;

    struct RostersRead {
        uint32_t value = rosters_auto;
        std::string league;  // non-empty: League takes this pack's id
    };
    inline RostersRead rosters(const std::string& key) {
        if (key == "blitz2026") return {rosters_2026, {}};
        if (key == "rom") return {rosters_rom, {}};
        if (key.empty() || key == "auto") return {rosters_auto, {}};
        return {rosters_auto, key};
    }

    struct LineupsRead {
        uint32_t value = lineups_current;
        bool changed = false;  // an old choice, read as Current: the file is written again
    };
    inline LineupsRead lineups(const std::string& key) {
        if (key == "full") return {lineups_full, false};
        if (key.empty() || key == "week") return {lineups_current, false};
        return {lineups_current, true};
    }

    // League's value after the load: an old Rosters pack fills it only while League is still NFL 2026 ("", or missing in
    // an old file), so a newer file's own League always wins.
    inline std::string league_after(const std::string& league_saved, const std::string& from_rosters) {
        return league_saved.empty() && !from_rosters.empty() ? from_rosters : league_saved;
    }
}
