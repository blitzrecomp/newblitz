// NFL tiebreakers for the classic Season (Classic, and Modern in the 2026 app): Settings → Season →
// Tiebreakers. Replaces the game's division standings (ovl_b 802FEA74) and playoff seeding
// (802FEEC4) with host versions over the same league, read from the game's own data: the 1999
// divisions (division map 0x8032E898, 31 teams, or 32 in the 2026 app's 32-team seasons, season32.cpp),
// the 17-week schedule blocks and the season record's results. The outputs keep the game's formats (0x28-byte standings entries; 3 division winners and
// 3 wild cards per conference at record +0x758/+0x75E), so screens, playoffs and saves are unchanged.
// In the 2026 app the same standings hook serves Advanced mode (adv_engine.cpp).
#include <cstring>
#include <vector>

#include "recomp.h"
#include "blitz_hooks.h"
#include "adv_league.h"

namespace blitz {
    bool nfl_tiebreaks();  // season_settings.cpp
}

namespace {
    using namespace adv;

    constexpr uint32_t div_map = 0x8032E898, week_ptrs = 0x8032E844;
    constexpr uint32_t rec = 0x802EF6F0, rec_rows = rec + 0x37F, rec_seeds = rec + 0x758;
    constexpr int classic_divisions = 6, classic_weeks = 17, row = 47;
    constexpr int classic_seeds = 6;

    gpr guest(uint32_t a) { return (gpr)(int32_t)a; }
    uint8_t rd8(uint8_t* rdram, uint32_t a) { return (uint8_t)MEM_B(0, guest(a)); }
    void wr8(uint8_t* rdram, uint32_t a, uint8_t v) { MEM_B(0, guest(a)) = (int8_t)v; }
    void wr32(uint8_t* rdram, uint32_t a, int32_t v) { MEM_W(0, guest(a)) = v; }
    void wrf(uint8_t* rdram, uint32_t a, float f) {
        int32_t bits;
        std::memcpy(&bits, &f, 4);
        wr32(rdram, a, bits);
    }

    // The league as the game has it: teams 0-30 by (division, slot), the schedule's games by the same
    // pairs (a pair no team has stays a placeholder, so game g still matches results byte 3g). A 32-team
    // season (2026 app) has team 31 and 16-game weeks; game 16's second score is the next results row's
    // first byte, which the same arithmetic reaches. team+'s 15-game weeks have no terminator, so they
    // stop at 15.
    League classic_league(uint8_t* rdram, Results& r) {
#if BLITZ_MODERN
        const int classic_teams = blitz_s32 ? 32 : 31, classic_games = blitz_s32 ? 16 : 15;
#else
        const int classic_teams = 31, classic_games = 15;
#endif
        League lg;
        lg.team_count = classic_teams;
        lg.members.assign(classic_divisions, {});
        for (int d = 0; d < classic_divisions; d++) lg.conference.push_back(d < 3 ? 0 : 1);
        int at[classic_divisions][8];
        for (auto& d : at) for (int& t : d) t = -1;
        for (int t = 0; t < classic_teams; t++) {
            int d = (int8_t)rd8(rdram, div_map + 2 * t), s = (int8_t)rd8(rdram, div_map + 2 * t + 1);
            if (d < 0 || d >= classic_divisions || s < 0 || s >= 8) continue;
            at[d][s] = t;
            lg.division[t] = d;
        }
        for (int d = 0; d < classic_divisions; d++) {
            for (int s = 0; s < 8; s++) if (at[d][s] >= 0) lg.members[d].push_back(at[d][s]);
        }
        auto team_at = [&](int d, int s) { return d >= 0 && d < classic_divisions && s >= 0 && s < 8 ? at[d][s] : -1; };
        for (int w = 0; w < classic_weeks; w++) {
            uint32_t block = (uint32_t)MEM_W(0, guest(week_ptrs + 4 * w));
            auto& games = lg.schedule.emplace_back();
            for (int g = 0; g < classic_games; g++) {
                uint32_t e = block + 5 * g;
                if ((int8_t)rd8(rdram, e + 2) == -1) break;
                // First-listed pair = away, second-listed = home (the game's own convention).
                games.push_back({team_at((int8_t)rd8(rdram, e), (int8_t)rd8(rdram, e + 1)),
                                 team_at((int8_t)rd8(rdram, e + 2), (int8_t)rd8(rdram, e + 3))});
                uint32_t res = rec_rows + row * w + 3 * g;
                r.score[w][g].home = rd8(rdram, res + 1);
                r.score[w][g].away = rd8(rdram, res + 2);
            }
        }
        return lg;
    }
}

extern "C" {
    // Entry of ovl_b 802FEA74 (out, div). Returns 1 when the host wrote the standings.
    int blitz_season_standings(uint8_t* rdram, int32_t out, int32_t div) {
#if BLITZ_MODERN
        if (blitz_adv) {
            blitz_adv_standings(rdram, out, div);
            return 1;
        }
#endif
        if (!blitz::nfl_tiebreaks() || div < 0 || div >= classic_divisions) return 0;
        Results r;
        League lg = classic_league(rdram, r);
        auto lines = division_standings(lg, r, div);
        for (uint32_t i = 0; i < 6; i++) {
            uint32_t e = (uint32_t)out + 0x28 * i;
            for (uint32_t k = 0; k < 0x28; k++) wr8(rdram, e + k, 0);
            if (i >= lines.size()) {
                wr8(rdram, e, 0xFF);
                continue;
            }
            const TeamLine& l = lines[i];
            const Record* recs[] = {&l.all, &l.home, &l.away, &l.division, &l.conf};
            wr8(rdram, e, (uint8_t)l.team);
            for (int k = 0; k < 5; k++) {
                wr8(rdram, e + 1 + 2 * k, (uint8_t)recs[k]->w);
                wr8(rdram, e + 2 + 2 * k, (uint8_t)recs[k]->l);
                wrf(rdram, e + 0x14 + 4 * k, (float)recs[k]->pct());
            }
            wr32(rdram, e + 0xC, l.points_for);
            wr32(rdram, e + 0x10, l.points_against);
        }
        return 1;
    }

    // Entry of ovl_b 802FEEC4 (after the last regular week). Returns 1 when the host wrote the seeds.
    int blitz_season_seeding(uint8_t* rdram) {
#if BLITZ_MODERN
        if (blitz_adv) return 0;  // Advanced seeds in its own driver; this function isn't reached
#endif
        if (!blitz::nfl_tiebreaks()) return 0;
        Results r;
        League lg = classic_league(rdram, r);
        for (int c = 0; c < 2; c++) {
            std::vector<int> s = conference_seeds(lg, r, c, classic_seeds);
            for (int i = 0; i < classic_seeds; i++) wr8(rdram, rec_seeds + 6 * c + i, (uint8_t)s[i]);
        }
        return 1;
    }
}
