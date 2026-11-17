// Weekly rosters (rosters_live.cpp): the lineup laid over the Blitz 2026 pack's 15 slots per team.
#pragma once
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace blitz {
    struct LineupPlayer {
        std::string last;
        int number = 0;
        int heavy = 0;  // the slot's body (linemen always heavy, quarterbacks and kickers never)
        int skin = -1;  // -1: not picked yet (the pack's shade for the slot stays)
    };
    struct Lineup {
        std::string label;  // "2026-w07", "full"
        std::map<std::string, std::array<std::optional<LineupPlayer>, 15>> teams;  // franchise -> slots
        std::map<std::string, std::array<std::string, 15>> ids;                    // franchise -> ESPN ids ("" = none)
    };

    // A player from the player file (players.json).
    struct RosterPlayer {
        std::string id, first, last;
        std::string team;    // franchise ("" for a free agent)
        int number = -1;     // -1: none yet
        std::string pos;     // ESPN's: QB, WR, OT, DE, CB, ...
        std::string depth;   // depth-chart spot ("wr2"), or ""
        std::string status;  // active, out, reserve, practice, free
        int skin = -1;       // -1: not picked yet
        int heavy = 0;       // the body away from the always-heavy and never-heavy slots
    };
    std::filesystem::path rosters_dir();  // <app folder>/rosters
    // The build's or the season pak's roster snapshot into <app folder>/rosters when it's needed (rosters_live.cpp).
    void rosters_seed();
    // A roster source the build carries itself (a private build): Roster Updates defaults to on.
    bool rosters_build_source();
    void rosters_check_again();  // another check now, in the background
    // The club's players (active, out, reserve lists; no practice squad), in the file's order. Empty without a player file.
    std::vector<RosterPlayer> roster_of(const std::string& franchise);
    std::optional<RosterPlayer> roster_player(const std::string& id);
    // The club's 15 slots in the lineup lineup_now(adv_week) plays, as ESPN ids ("" = the pack's).
    std::array<std::string, 15> lineup_ids(const std::string& franchise, int adv_week = -1);
    // The club's full-strength lineup (injuries ignored), as ESPN ids.
    std::array<std::string, 15> full_ids(const std::string& franchise);

    // The lineup to play: the Lineups setting's (Settings → Season), or in an Advanced season on This Week,
    // that season week's checkpoint (adv_week 0-based; -1 outside Advanced). Null: the pack as it is.
    std::shared_ptr<const Lineup> lineup_now(int adv_week);
    // Export ROM's Rosters, whatever the Lineups setting: this week's (the latest downloaded; full strength before
    // any) or full strength. Null: the pack as it is.
    std::shared_ptr<const Lineup> lineup_for_export(bool full_strength);
}
