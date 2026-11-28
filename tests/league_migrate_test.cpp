// Unit tests for Settings → Teams from before 1.0.6 (src/game/league_migrate.h): a Rosters that named a league pack becomes
// League = that pack with Rosters = Auto; Lineups' Built-In and pinned weeks become Current; a newer file's League wins.
// Run with `make test-league`.
#include <cstdio>
#include <string>

#include "../src/game/league_migrate.h"

using namespace blitz::league_migrate;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

// An old season.json's two keys, read as season_settings.cpp's load does: (rosters, league, lineups, written again).
struct Loaded {
    uint32_t rosters, lineups;
    std::string league;
    bool rewrite;
};
static Loaded load(const std::string& rosters_key, const std::string& lineups_key, const std::string& league_saved = "") {
    RostersRead r = rosters(rosters_key);
    LineupsRead l = lineups(lineups_key);
    std::string league = league_after(league_saved, r.league);
    return {r.value, l.value, league, l.changed || !r.league.empty()};
}

int main() {
    // The Duo's case: Rosters = CFL 2026, Lineups = Week 12 (1.0.5 and the 1.0.6 test builds).
    {
        Loaded s = load("cfl", "2026-w12");
        CHECK(s.league == "cfl");
        CHECK(s.rosters == rosters_auto);
        CHECK(s.lineups == lineups_current);
        CHECK(s.rewrite);
    }
    // Pac-12 (bundled) and Built-In.
    {
        Loaded s = load("pac12", "packed");
        CHECK(s.league == "pac12" && s.rosters == rosters_auto && s.lineups == lineups_current && s.rewrite);
    }
    // The NFL choices stay as they were, with nothing to write.
    {
        Loaded s = load("rom", "full");
        CHECK(s.league.empty() && s.rosters == rosters_rom && s.lineups == lineups_full && !s.rewrite);
        s = load("blitz2026", "week");
        CHECK(s.league.empty() && s.rosters == rosters_2026 && s.lineups == lineups_current && !s.rewrite);
        s = load("auto", "week");
        CHECK(s.league.empty() && s.rosters == rosters_auto && !s.rewrite);
    }
    // A fresh file (no keys): the defaults.
    {
        Loaded s = load("", "");
        CHECK(s.league.empty() && s.rosters == rosters_auto && s.lineups == lineups_current && !s.rewrite);
    }
    // Any pinned week, the playoffs too, reads as Current.
    for (const char* w : {"2026-w01", "2026-w18", "2026-post1", "2026-post5"}) {
        LineupsRead l = lineups(w);
        CHECK(l.value == lineups_current && l.changed);
    }
    // A 1.0.6 file synced back from an older device (Cloud Sync): its own League wins over a Rosters pack.
    {
        Loaded s = load("pac12", "week", "cfl");
        CHECK(s.league == "cfl" && s.rosters == rosters_auto);
    }
    printf("league_migrate: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
