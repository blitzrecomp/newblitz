// Advanced mode's league model (docs/ADVANCED_SEASON.md, Stage B): 32 teams in 8 divisions of 4, an
// 18-week schedule, standings with the NFL's tiebreaking procedures and a 7-team-per-conference
// playoff bracket. Pure C++ (no game memory), so it is unit-tested on its own (tests/adv_league_test.cpp);
// adv_engine.cpp connects it to the game.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace adv {
    constexpr int teams = 32, divisions = 8, division_size = 4, conferences = 2;
    constexpr int regular_weeks = 18, playoff_weeks = 4, total_weeks = regular_weeks + playoff_weeks;
    constexpr int max_games = 16, seeds = 7;
    constexpr uint8_t unplayed = 0xFF;

    // Week indices: 0-17 regular season, then the playoff rounds.
    enum Round { wildcard = regular_weeks, divisional, conference, super_bowl };

    struct Game {
        int away = -1, home = -1;  // team indices
    };

    struct Score {
        uint8_t away = unplayed, home = unplayed;
        bool played() const { return away != unplayed && home != unplayed; }
    };

    struct League {
        int team_count = teams;
        std::array<int, teams> division{};       // per team
        std::vector<std::vector<int>> members;   // per division, team indices
        std::vector<int> conference;             // per division: 0 = AFC, 1 = NFC
        std::vector<std::vector<Game>> schedule; // regular weeks
        uint64_t seed = 0;                       // coin tosses

        int conference_of(int t) const { return conference[division[t]]; }
    };

    // Everything that happened: scores per week and game (regular weeks follow League::schedule;
    // playoff weeks follow playoff_games()).
    struct Results {
        std::array<std::array<Score, max_games>, total_weeks> score{};
    };

    struct Record {
        int w = 0, l = 0, t = 0;
        int games() const { return w + l + t; }
        double pct() const { return games() ? (w + 0.5 * t) / games() : 0.0; }
    };

    struct TeamLine {  // what the standings screen shows
        int team = -1;
        Record all, home, away, division, conf;
        int points_for = 0, points_against = 0;
    };

    // Standings of one division, best first, ties broken by the NFL's division procedure (using only
    // regular-season games played so far).
    std::vector<TeamLine> division_standings(const League& lg, const Results& r, int div);

    // The seeds of a conference after the regular season: its division winners by record, then wild
    // cards up to `count` (7 today; 6 in the 1999 format), all ties broken by the NFL procedures.
    std::vector<int> conference_seeds(const League& lg, const Results& r, int conf, int count = seeds);

    // Playoff games of a round, given the seeds ([conf][seed-1]) and the earlier rounds' results.
    // Wild card: 2v7, 3v6, 4v5 per conference (seed 1 rests). Divisional: 1 v lowest remaining seed,
    // the other two. Conference: the two winners. Higher seed at home; the Super Bowl lists AFC first.
    std::vector<Game> playoff_games(const std::array<std::array<int, seeds>, conferences>& seeds_of, const Results& r, int week);

    // Winner of a played game (away on a tie: Blitz plays overtime, so it shouldn't happen).
    int winner(const Game& g, const Score& s);
}
