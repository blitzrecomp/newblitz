// Advanced mode's season engine, Stage B (docs/ADVANCED_SEASON.md): the content pack's league (8
// divisions of 4, an 18-week schedule), NFL tiebreakers and the 7-team playoff bracket, run by host
// code in place of the game's season functions. The game's own screens, sim and match code stay;
// hooks (tools/teamplus.py ADVANCED) replace these ovl_b functions when blitz_adv is set:
//   802FBBE4 week driver      plays or simulates a week, records it, seeds, runs the playoffs
//   802FDEF4 week builder     a week's games in the game's 18 x 6-byte format (screens read it)
//   802FEA74 division standings, in the game's 0x28-byte entry format (standings screen)
// and the season rating screen (80300BA4) is computed here before its display functions are called.
//
// Season record layout in Advanced (the game copies the whole 0x774 bytes in and out of saves):
//   +0 user team, +1 week (0-17 regular, 18 wild card, 19 divisional, 20 conference, 21 Super Bowl,
//   22 season over), +4 stats f32[31][7] (team 31's at +0x704), +0x368 season highs (written, never
//   shown), +0x37F results [22 weeks][16 games] {home score, away score} (0xFF = not played),
//   +0x758 seeds [2 conferences][7].
// The schedule itself comes from the pack, so a season must be loaded with the same pack (saves are
// kept per pack).
#if BLITZ_MODERN
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "recomp.h"
#if BLITZ_MODERN
#include "team_store.h"
#endif
#include "blitz_hooks.h"
#include "blitz_savestate.h"
#include "adv_league.h"
#ifdef BLITZ_SAVESTATES
#include "ss_runtime.h"
extern "C" ss_stack* ss_current_stack(void);                       // ss_runtime.c
extern "C" uint32_t blitz_ss_site(uint32_t fn, uint32_t call_vram);  // savestate.cpp
#endif
#include "season_data.h"

extern "C" {
    void ovl_b_func_802FD094(uint8_t* rdram, recomp_context* ctx);  // play or simulate one game
    void ovl_b_func_802FBAAC(uint8_t* rdram, recomp_context* ctx);  // CPU skill (adv_season.cpp when Advanced)
    void ovl_b_func_80300FA0(uint8_t* rdram, recomp_context* ctx);  // rating formula
    void ovl_b_func_80300858(uint8_t* rdram, recomp_context* ctx);  // rating screen
    void ovl_b_func_8030052C(uint8_t* rdram, recomp_context* ctx);  // rating screen, 90+
    void ovl_b_func_803003AC(uint8_t* rdram, recomp_context* ctx);  // message 0: bye week
    void ovl_b_func_8030040C(uint8_t* rdram, recomp_context* ctx);  // 1: missed the playoffs
    void ovl_b_func_8030046C(uint8_t* rdram, recomp_context* ctx);  // 2: won the division
    void ovl_b_func_8030048C(uint8_t* rdram, recomp_context* ctx);  // 3: advances to the second round
    void ovl_b_func_803004AC(uint8_t* rdram, recomp_context* ctx);  // 4: wild card
    void ovl_b_func_803004CC(uint8_t* rdram, recomp_context* ctx);  // 5: conference finals
    void ovl_b_func_803004EC(uint8_t* rdram, recomp_context* ctx);  // 6: Super Bowl bound
    void ovl_b_func_8030042C(uint8_t* rdram, recomp_context* ctx);  // 7: eliminated
    void ovl_b_func_8030044C(uint8_t* rdram, recomp_context* ctx);  // 8: lost the Super Bowl
    void ovl_b_func_803003DC(uint8_t* rdram, recomp_context* ctx);  // 9: first-round bye
    void ovl_b_func_8030050C(uint8_t* rdram, recomp_context* ctx);  // 10: Super Bowl challenge
    void func_8025A26C(uint8_t* rdram, recomp_context* ctx);        // options bank for the mode
    void func_8025F098(uint8_t* rdram, recomp_context* ctx);        // option word
    void func_8027BF50(uint8_t* rdram, recomp_context* ctx);        // music (rating screen)
    void func_8027BF6C(uint8_t* rdram, recomp_context* ctx);
    void func_80005718(uint8_t* rdram, recomp_context* ctx);
    void func_8027BCF4(uint8_t* rdram, recomp_context* ctx);
}

namespace blitz {
    void post_notice(const std::string& msg);                       // notices.cpp
    std::vector<std::string> team_franchises();               // content_pack_tp.cpp
    uint64_t adv_season_seed();                                       // adv_season.cpp
}

namespace {
    using namespace adv;

    constexpr uint32_t rec = 0x802EF6F0, rec_team = rec, rec_week = rec + 1;
    constexpr uint32_t rec_stats = rec + 4, rec_stats31 = rec + 0x704;
    constexpr uint32_t rec_highs = rec + 0x368, rec_high_teams = rec + 0x376;
    constexpr uint32_t rec_results = rec + 0x37F, rec_seeds = rec + 0x758;
    constexpr int season_over = total_weeks;

    constexpr uint32_t sim_slot = 0x802EA1D0;      // two 0x48-byte team blocks: home, away
    constexpr uint32_t abort_flag = 0x8028BFC4;   // the user's match ended abnormally
    constexpr uint32_t pad_store = 0x8028BFD4;    // the driver's argument, read by the match code
    constexpr uint32_t sim_week = 0x8028BFD8;     // "simulate week": no match, no messages

    // Text for the screens, in RDRAM the game never uses (0x80400000.., docs/ADVANCED_SEASON.md §2).
    constexpr uint32_t mirror = 0x80400100;
    constexpr uint32_t mirror_div_names = mirror;              // 8 pointers
    constexpr uint32_t mirror_week_names = mirror + 0x40;      // 22 pointers
    constexpr uint32_t mirror_strings = mirror + 0x100;

    League league;
    bool league_ok = false;

    gpr guest(uint32_t a) { return (gpr)(int32_t)a; }
    uint8_t rd8(uint8_t* rdram, uint32_t a) { return (uint8_t)MEM_B(0, guest(a)); }
    void wr8(uint8_t* rdram, uint32_t a, uint8_t v) { MEM_B(0, guest(a)) = (int8_t)v; }
    void wr16(uint8_t* rdram, uint32_t a, uint16_t v) { MEM_H(0, guest(a)) = (int16_t)v; }
    int32_t rd32(uint8_t* rdram, uint32_t a) { return MEM_W(0, guest(a)); }
    void wr32(uint8_t* rdram, uint32_t a, int32_t v) { MEM_W(0, guest(a)) = v; }
    float rdf(uint8_t* rdram, uint32_t a) {
        int32_t bits = rd32(rdram, a);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    void wrf(uint8_t* rdram, uint32_t a, float f) {
        int32_t bits;
        std::memcpy(&bits, &f, 4);
        wr32(rdram, a, bits);
    }

    uint32_t result_addr(int week, int game) { return rec_results + (uint32_t)(week * max_games + game) * 2; }
    uint32_t stats_addr(int team) { return team == 31 ? rec_stats31 : rec_stats + 28u * team; }

    Results read_results(uint8_t* rdram) {
        Results r;
        for (int w = 0; w < total_weeks; w++) {
            for (int g = 0; g < max_games; g++) {
                r.score[w][g].home = rd8(rdram, result_addr(w, g));
                r.score[w][g].away = rd8(rdram, result_addr(w, g) + 1);
            }
        }
        return r;
    }

    std::array<std::array<int, seeds>, conferences> read_seeds(uint8_t* rdram, bool* complete = nullptr) {
        std::array<std::array<int, seeds>, conferences> s{};
        bool ok = true;
        for (int c = 0; c < conferences; c++) {
            for (int i = 0; i < seeds; i++) {
                s[c][i] = rd8(rdram, rec_seeds + c * seeds + i);
                ok = ok && s[c][i] < teams;
            }
        }
        if (complete) *complete = ok;
        return s;
    }

    // The games of a week: the schedule, or the playoff round from the seeds and earlier results.
    std::vector<Game> games_of(uint8_t* rdram, int week, const Results& r) {
        if (week < regular_weeks) {
            return week < (int)league.schedule.size() ? league.schedule[week] : std::vector<Game>{};
        }
        bool seeded = false;
        auto s = read_seeds(rdram, &seeded);
        return seeded ? playoff_games(s, r, week) : std::vector<Game>{};
    }

    void write_strings(uint8_t* rdram, const std::vector<std::string>& texts, uint32_t table, uint32_t& at) {
        for (size_t i = 0; i < texts.size(); i++) {
            wr32(rdram, table + 4 * (uint32_t)i, (int32_t)at);
            for (char c : texts[i]) wr8(rdram, at++, (uint8_t)c);
            wr8(rdram, at++, 0);
        }
    }

    // ---- calling the game ----

    void call(uint8_t* rdram, recomp_context* ctx, void (*fn)(uint8_t*, recomp_context*), int32_t a0 = 0) {
        ctx->r4 = a0;
        fn(rdram, ctx);
    }

    int quarter_length(uint8_t* rdram, recomp_context* ctx) {
        func_8025A26C(rdram, ctx);
        ctx->r4 = ctx->r2;
        ctx->r5 = 1;
        func_8025F098(rdram, ctx);
        return (int32_t)ctx->r2 >> 16;
    }

    // Save states in an Advanced Season game (docs/SAVE_STATES.md). The user's match sleeps inside 802FD094, called from
    // here, host code no restored thread can come back through. So play() leaves the vanilla driver's own call site's
    // entry for that call (802FBBE4 -> 802FD094 at 802FBD80) on the shadow stack: the chain reads as unbroken, and on a
    // restore the generated 802FBBE4 hands it to blitz_adv_week_driver (resumable.py keeps the hook ahead of its resume
    // switch), which resumes the match and finishes the week from adv_live, kept with the state.
    struct AdvLive { int32_t week = -1, game = -1, qlen = 0, pad = 0; } adv_live;
    [[maybe_unused]] const bool adv_live_registered = blitz::savestate_register_pod("adv_live", &adv_live);
    constexpr uint32_t week_driver = 0x802FBBE4, match_call = 0x802FBD80;

    // Play (the user's game) or simulate one game into sim slot 0; home is the game's team A.
    void play(uint8_t* rdram, recomp_context* ctx, const Game& g, int qlen) {
        gpr sp = ctx->r29;
        ctx->r29 = sp - 0x20;
        wr32(rdram, (uint32_t)ctx->r29 + 0x10, (int32_t)blitz::teamdata::live::cpu_skill(rdram, g.home));
        wr32(rdram, (uint32_t)ctx->r29 + 0x14, (int32_t)blitz::teamdata::live::cpu_skill(rdram, g.away));
        ctx->r4 = g.home;
        ctx->r5 = g.away;
        ctx->r6 = (int32_t)sim_slot;
        ctx->r7 = qlen;
#ifdef BLITZ_SAVESTATES
        static const uint32_t site = blitz_ss_site(week_driver, match_call);
        if (site) ss_push(ss_current_stack(), site, 0);
#endif
        ovl_b_func_802FD094(rdram, ctx);
#ifdef BLITZ_SAVESTATES
        if (site) ss_pop(ss_current_stack());
#endif
        ctx->r29 = sp;
    }

    // Store sim slot 0 as the result of (week, game); during the regular season also the teams'
    // running per-game stat averages and the season highs, as the game does (ovl_b 802FE29C).
    void record(uint8_t* rdram, int week, int game, const Game& g) {
        auto clamp = [](int32_t s) { return (uint8_t)(s < 0 ? 0 : s > 254 ? 254 : s); };
        // Test switch BLITZ_ADV_USER_WINS=1: the user's team wins its simulated games (scores swapped,
        // a tie broken by a field goal), to reach late playoff rounds and the Super Bowl quickly.
        static const bool user_wins = getenv("BLITZ_ADV_USER_WINS") != nullptr;
        if (user_wins && rd32(rdram, sim_week) != 0) {
            int user = rd8(rdram, rec_team);
            if (g.home == user || g.away == user) {
                int32_t h = rd32(rdram, sim_slot + 0x1C), a = rd32(rdram, sim_slot + 0x48 + 0x1C);
                int32_t mine = g.home == user ? h : a, theirs = g.home == user ? a : h;
                if (mine <= theirs) {
                    int32_t win = mine == theirs ? theirs + 3 : theirs, lose = mine == theirs ? mine : mine;
                    wr32(rdram, sim_slot + (g.home == user ? 0x1C : 0x48 + 0x1C), win);
                    wr32(rdram, sim_slot + (g.home == user ? 0x48 + 0x1C : 0x1C), lose);
                }
            }
        }
        wr8(rdram, result_addr(week, game), clamp(rd32(rdram, sim_slot + 0x1C)));
        wr8(rdram, result_addr(week, game) + 1, clamp(rd32(rdram, sim_slot + 0x48 + 0x1C)));
        if (week >= regular_weeks) return;
        Results r = read_results(rdram);
        for (int side = 0; side < 2; side++) {
            int team = side == 0 ? g.home : g.away;
            uint32_t block = sim_slot + 0x48 * side;
            int n = 0;
            for (int w = 0; w < regular_weeks && w < (int)league.schedule.size(); w++) {
                for (size_t k = 0; k < league.schedule[w].size(); k++) {
                    const Game& o = league.schedule[w][k];
                    if ((o.home == team || o.away == team) && r.score[w][k].played()) n++;
                }
            }
            if (n == 0) continue;
            uint32_t st = stats_addr(team);
            float games_before = (float)(n - 1);
            for (int k = 0; k < 7; k++) {
                float product = rdf(rdram, st + 4 * k) * games_before;
                float sum = product + (float)rd32(rdram, block + 0x2C + 4 * k);
                float value = sum;
                if (0.0f < sum) value = sum / (float)n;
                wrf(rdram, st + 4 * k, value);
            }
            for (int k = 0; k < 7; k++) {
                int32_t x = rd32(rdram, block + 0x2C + 4 * k);
                uint16_t best = (uint16_t)((rd8(rdram, rec_highs + 2 * k) << 8) | rd8(rdram, rec_highs + 2 * k + 1));
                if ((int32_t)best < x) {
                    wr8(rdram, rec_highs + 2 * k, (uint8_t)(x >> 8));
                    wr8(rdram, rec_highs + 2 * k + 1, (uint8_t)x);
                    wr8(rdram, rec_high_teams + k, (uint8_t)team);
                }
            }
        }
    }

    void seed_playoffs(uint8_t* rdram) {
        Results r = read_results(rdram);
        for (int c = 0; c < conferences; c++) {
            auto s = conference_seeds(league, r, c);
            for (int i = 0; i < seeds; i++) wr8(rdram, rec_seeds + c * seeds + i, (uint8_t)s[i]);
        }
    }

    int seed_of_user(uint8_t* rdram) {
        int user = rd8(rdram, rec_team);
        auto s = read_seeds(rdram);
        for (int c = 0; c < conferences; c++) {
            for (int i = 0; i < seeds; i++) if (s[c][i] == user) return i + 1;
        }
        return 0;
    }

    // The Super Bowl winner's season rating (ovl_b 80300BA4): losses, average winning margin and
    // points allowed per week, completion percentage. The game weights points allowed by its
    // quarter length; it recovered that from a fixed-margin CPU game each week, Advanced knows it.
    void season_rating(uint8_t* rdram, recomp_context* ctx, int qlen) {
        int user = rd8(rdram, rec_team), losses = 0, margin = 0, allowed = 0;
        Results r = read_results(rdram);
        for (int w = 0; w < total_weeks; w++) {
            std::vector<Game> games = games_of(rdram, w, r);
            for (size_t g = 0; g < games.size(); g++) {
                const Score& s = r.score[w][g];
                if (!s.played() || (games[g].home != user && games[g].away != user)) continue;
                int own = games[g].home == user ? s.home : s.away, opp = games[g].home == user ? s.away : s.home;
                float weighted = (float)opp / ((float)qlen * 0.5f);
                allowed = (int)((float)allowed + weighted);
                if (own < opp) losses++;
                else margin += own - opp;
            }
        }
        float attempts = rdf(rdram, stats_addr(user) + 12), completions = rdf(rdram, stats_addr(user) + 8);
        int pct = attempts == 0.0f ? 0 : (int)((completions / attempts) * 100.0f);
        ctx->r4 = losses;
        ctx->r5 = margin / total_weeks;
        ctx->r6 = allowed / total_weeks;
        ctx->r7 = pct;
        ovl_b_func_80300FA0(rdram, ctx);
        int32_t rating = (int32_t)ctx->r2;
        call(rdram, ctx, func_8027BF50, (int32_t)0x80339FD8);
        gpr sp = ctx->r29;
        ctx->r29 = sp - 0x20;
        wr32(rdram, (uint32_t)ctx->r29 + 0x10, 0x8000);
        ctx->r4 = (int32_t)0x80339FD8;
        ctx->r5 = 0;
        ctx->r6 = 0xFF;
        ctx->r7 = 0x7F;
        func_8027BF6C(rdram, ctx);
        ctx->r29 = sp;
        call(rdram, ctx, rating < 900 ? ovl_b_func_80300858 : ovl_b_func_8030052C, rating);
        call(rdram, ctx, func_80005718);
        call(rdram, ctx, func_8027BCF4, (int32_t)0x80339FD8);
    }

    bool messages(uint8_t* rdram) { return rd32(rdram, sim_week) == 0; }

    // One week. `nested` weeks are the rest of the playoffs, simulated after the user is out: no
    // messages and no week++ (the caller sets the week).
    // Returns false when the user's match was aborted.
    // `resumed`: the rest of a week whose user game a save state just finished (blitz_adv_week_driver): the week's CPU
    // skills and RNG seed are already in the restored RDRAM, its quarter length in adv_live, its Super Bowl intro shown.
    bool run_week(uint8_t* rdram, recomp_context* ctx, int32_t pad, bool nested, bool resumed = false) {
        int week = rd8(rdram, rec_week);
        if (week >= season_over) return true;
        int qlen;
        if (resumed) qlen = adv_live.qlen;
        else {
            call(rdram, ctx, ovl_b_func_802FBAAC);  // CPU skill for the week (host version)
            qlen = quarter_length(rdram, ctx);
        }
        if (!nested) adv_live = AdvLive{week, -1, qlen, pad};
        int user = rd8(rdram, rec_team);
        bool show = messages(rdram) && !nested;
        Results r = read_results(rdram);
        std::vector<Game> games = games_of(rdram, week, r);
        bool user_plays = false;
        for (const Game& g : games) user_plays = user_plays || g.home == user || g.away == user;

        if (week == super_bowl && user_plays && show && !resumed) call(rdram, ctx, ovl_b_func_8030050C, pad);
        for (size_t i = 0; i < games.size(); i++) {
            if (r.score[week][i].played()) continue;  // already recorded (a week resumed after an abort)
            if (!nested) adv_live.game = (int32_t)i;
            play(rdram, ctx, games[i], qlen);
            record(rdram, week, (int)i, games[i]);
            if (rd32(rdram, abort_flag) != 0) return false;
        }
        r = read_results(rdram);

        auto finish_playoffs = [&](int from) {
            for (int w = from; w < total_weeks; w++) {
                wr8(rdram, rec_week, (uint8_t)w);
                if (!run_week(rdram, ctx, pad, true)) return false;
            }
            wr8(rdram, rec_week, (uint8_t)season_over);
            return true;
        };

        if (week < regular_weeks) {
            if (!user_plays && show) call(rdram, ctx, ovl_b_func_803003AC, pad);
            if (week == regular_weeks - 1) {
                seed_playoffs(rdram);
                int seed = seed_of_user(rdram);
                if (seed >= 1 && seed <= 4 && show) call(rdram, ctx, ovl_b_func_8030046C, pad);
                if (seed >= 5 && show) call(rdram, ctx, ovl_b_func_803004AC, pad);
                if (seed == 0) {
                    if (show) call(rdram, ctx, ovl_b_func_8030040C, pad);
                    return finish_playoffs(wildcard);
                }
            }
        }
        else if (!nested) {
            bool won = false;
            for (size_t i = 0; i < games.size(); i++) {
                if (games[i].home == user || games[i].away == user) won = winner(games[i], r.score[week][i]) == user;
            }
            if (week == super_bowl) {
                if (user_plays && show) {
                    if (won) season_rating(rdram, ctx, qlen);
                    else call(rdram, ctx, ovl_b_func_8030044C, pad);
                }
            }
            else if (!user_plays) {
                if (week == wildcard && show) call(rdram, ctx, ovl_b_func_803003DC, pad);  // the top seed rests
            }
            else if (won) {
                if (show) call(rdram, ctx, week == wildcard ? ovl_b_func_8030048C : week == divisional ? ovl_b_func_803004CC : ovl_b_func_803004EC, pad);
            }
            else {
                if (show) call(rdram, ctx, ovl_b_func_8030042C, pad);
                return finish_playoffs(week + 1);
            }
        }
        if (nested) return true;
        if (rd32(rdram, abort_flag) == 1) return false;
        wr8(rdram, rec_week, (uint8_t)(week + 1));
        return true;
    }
}

namespace blitz {
    // FNV-1a over the pack's divisions and schedule (saves record it; adv_save.cpp).
    uint64_t adv_schedule_fingerprint() {
        const SeasonData* sd = pack_season();
        if (sd == nullptr) return 0;
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const std::string& t) {
            for (unsigned char c : t) h = (h ^ c) * 1099511628211ull;
            h = (h ^ 0xFF) * 1099511628211ull;
        };
        for (const auto& d : sd->divisions) for (const auto& f : d) mix(f);
        for (const auto& w : sd->weeks) {
            for (const auto& [away, home] : w) { mix(away); mix(home); }
            mix("|");
        }
        return h;
    }

    // After every ovl_b load (content_pack_tp.cpp): the league from the pack and the screens' text.
    void adv_on_ovl_b(uint8_t* rdram, const std::vector<std::string>& franchise_of_slot) {
        league_ok = false;
        const SeasonData* sd = pack_season();
        if (sd == nullptr || franchise_of_slot.size() != teams) {
            static bool told = false;
            if (!told) {
                told = true;
                post_notice(sd == nullptr ? "Advanced mode needs a season (divisions and schedule); the content pack has none."
                                          : "Advanced mode's Season needs the 32 current teams.");
            }
            return;
        }
        auto index_of = [&](const std::string& f) {
            for (int t = 0; t < teams; t++) if (franchise_of_slot[t] == f) return t;
            return -1;
        };
        League lg;
        lg.members.assign(divisions, std::vector<int>(division_size));
        for (int d = 0; d < divisions; d++) lg.conference.push_back(d / 4);
        for (int d = 0; d < divisions; d++) {
            for (int k = 0; k < division_size; k++) {
                int t = index_of(sd->divisions[d][k]);
                if (t < 0) return;
                lg.members[d][k] = t;
                lg.division[t] = d;
            }
        }
        for (const auto& week : sd->weeks) {
            auto& games = lg.schedule.emplace_back();
            for (const auto& [away, home] : week) games.push_back({index_of(away), index_of(home)});
        }
        lg.seed = adv_season_seed();
        league = std::move(lg);
        league_ok = true;

        std::vector<std::string> div_names, week_names;
        for (const std::string& n : sd->division_names) {
            std::string word = n.substr(n.find(' ') == std::string::npos ? 0 : n.find(' ') + 1);
            for (char& c : word) c = (char)std::toupper((unsigned char)c);
            div_names.push_back(word);
        }
        for (int w = 0; w < regular_weeks; w++) week_names.push_back("Week " + std::to_string(w + 1));
        for (const char* n : {"Wildcard", "Divisional", "Championship", "SuperBowl"}) week_names.push_back(n);
        uint32_t at = mirror_strings;
        write_strings(rdram, div_names, mirror_div_names, at);
        write_strings(rdram, week_names, mirror_week_names, at);
    }

}

extern "C" {
    // ovl_b 802FBBE4 (a0 = pointer to the controller/pad value).
    void blitz_adv_week_driver(uint8_t* rdram, recomp_context* ctx) {
#ifdef BLITZ_SAVESTATES
        // A restored thread coming back into the user's match (see AdvLive). ctx is the restored one: r4 isn't the
        // pad's address any more, so nothing is read through it here.
        if (ss_stack* st = ss_current_stack(); st->resuming) {
            static const uint32_t site = blitz_ss_site(week_driver, match_call);
            const int week = rd8(rdram, rec_week);
            if (!league_ok || site == 0 || ss_resume_site(st) != site || week != adv_live.week || adv_live.game < 0) {
                fprintf(stderr, "[adv] can't resume the week (league %d, site %u, week %d vs %d, game %d)\n", league_ok, site, week,
                        adv_live.week, adv_live.game);
                ss_resume_fail(ctx, week_driver, site);
            }
            league.seed = blitz::adv_season_seed();
            ovl_b_func_802FD094(rdram, ctx);  // the match, from where it slept
            ctx->r29 += 0x20;                 // back to the host frame play() made below the driver's sp
            ss_pop(st);
            Results r = read_results(rdram);
            std::vector<Game> games = games_of(rdram, week, r);
            if ((size_t)adv_live.game < games.size()) record(rdram, week, adv_live.game, games[(size_t)adv_live.game]);
            fprintf(stderr, "[adv] resumed week %d game %d from a save state\n", week, adv_live.game);
            if (rd32(rdram, abort_flag) == 0) run_week(rdram, ctx, adv_live.pad, false, true);
            return;
        }
#endif
        int32_t pad = rd32(rdram, (uint32_t)ctx->r4);
        wr32(rdram, pad_store, pad);
        if (!league_ok) return;
        league.seed = blitz::adv_season_seed();
        run_week(rdram, ctx, pad, false);
    }

    // ovl_b 802FDEF4 (week, out): 18 entries {home, away, home score, away score, u16 flags}; games
    // first, then byes (flag 0x80), then empty entries. Undecided playoff games are {-1, -1, .., 0x400}
    // (the week screen shows "TBD"). Returns 0 for a week past the season.
    int32_t blitz_adv_build_week(uint8_t* rdram, int32_t week, int32_t out) {
        if (week < 0 || week >= total_weeks) return 0;
        Results r = read_results(rdram);
        std::vector<std::array<uint8_t, 4>> entries;
        std::vector<uint16_t> flags;
        std::vector<Game> games = league_ok ? games_of(rdram, week, r) : std::vector<Game>{};
        std::array<bool, teams> busy{};
        for (size_t g = 0; g < games.size() && entries.size() < 18; g++) {
            const Game& gm = games[g];
            const Score& s = r.score[week][g];
            entries.push_back({(uint8_t)gm.home, (uint8_t)gm.away, s.home, s.away});
            uint16_t f = 1;
            if (league.division[gm.home] == league.division[gm.away]) f |= 0x600;
            else if (league.conference_of(gm.home) == league.conference_of(gm.away)) f |= 0x400;
            flags.push_back(f);
            busy[gm.home] = busy[gm.away] = true;
        }
        if (week >= regular_weeks) {
            static constexpr int round_games[] = {6, 4, 2, 1};
            while ((int)entries.size() < round_games[week - regular_weeks]) {
                entries.push_back({0xFF, 0xFF, 0xFF, 0xFF});
                flags.push_back(0x401);
            }
            bool seeded = false;
            auto s = read_seeds(rdram, &seeded);
            if (week == wildcard && seeded) {
                for (int c = 0; c < conferences; c++) {
                    entries.push_back({(uint8_t)s[c][0], 0xFF, 0xFF, 0xFF});
                    flags.push_back(0x80);
                }
            }
        }
        else if (league_ok) {
            for (int t = 0; t < teams && entries.size() < 18; t++) {
                if (busy[t]) continue;
                entries.push_back({(uint8_t)t, 0xFF, 0xFF, 0xFF});
                flags.push_back(0x80);
            }
        }
        for (uint32_t i = 0; i < 18; i++) {
            uint32_t e = (uint32_t)out + 6 * i;
            if (i < entries.size()) {
                for (int k = 0; k < 4; k++) wr8(rdram, e + k, entries[i][k]);
                wr16(rdram, e + 4, flags[i]);
            }
            else {
                for (int k = 0; k < 4; k++) wr8(rdram, e + k, 0xFF);
                wr16(rdram, e + 4, 0);
            }
        }
        return 1;
    }

    // ovl_b 802FEA74 (out, div): the division's 4 teams in standings order, 0x28 bytes each:
    // team, W, L, home W/L, away W/L, division W/L, conference W/L, pad, s32 points for/against,
    // f32 win %, home %, away %, division %, conference %.
    void blitz_adv_standings(uint8_t* rdram, int32_t out, int32_t div) {
        if (!league_ok || div < 0 || div >= divisions) return;
        league.seed = blitz::adv_season_seed();
        auto lines = division_standings(league, read_results(rdram), div);
        for (size_t i = 0; i < lines.size(); i++) {
            const TeamLine& l = lines[i];
            uint32_t e = (uint32_t)out + 0x28 * (uint32_t)i;
            const Record* recs[] = {&l.all, &l.home, &l.away, &l.division, &l.conf};
            wr8(rdram, e, (uint8_t)l.team);
            for (int k = 0; k < 5; k++) {
                wr8(rdram, e + 1 + 2 * k, (uint8_t)recs[k]->w);
                wr8(rdram, e + 2 + 2 * k, (uint8_t)recs[k]->l);
                wrf(rdram, e + 0x14 + 4 * k, (float)recs[k]->pct());
            }
            wr8(rdram, e + 0xB, 0);
            wr32(rdram, e + 0xC, l.points_for);
            wr32(rdram, e + 0x10, l.points_against);
        }
    }

    // Screen helpers (hooks in tools/teamplus.py).
    int32_t blitz_adv_week_name(int32_t week) {
        if (week < 0) week = 0;
        if (week >= total_weeks) week = total_weeks - 1;
        return (int32_t)(mirror_week_names + 4 * week);
    }
    int32_t blitz_adv_div_names(void) { return (int32_t)mirror_div_names; }
}

// Standings screen division menu: AFC East..West under the AFC header, NFC under the NFC header.
extern "C" void blitz_adv_div_menu_pos(recomp_context* ctx) {
    int i = (int32_t)ctx->r17;
    float y = i < 4 ? 220.0f - 14.0f * i : 98.0f - 14.0f * (i - 4);  // as the game: 220 down, and below the NFC header
    float z = 97.0f - 1.0f;
    int32_t zbits;
    std::memcpy(&zbits, &z, 4);
    ctx->r4 = 70;
    ctx->r5 = (int32_t)y;
    ctx->r6 = zbits;
}

extern "C" int32_t blitz_adv_nfc_header_y(void) {
    float y = 165.0f;
    int32_t bits;
    std::memcpy(&bits, &y, 4);
    return bits;
}

// Team schedule screen: weeks 1-11 in the left column, 12-22 on the right.
extern "C" void blitz_adv_schedule_column(recomp_context* ctx) {
    int w = (int32_t)ctx->r20;
    bool left = w < 11;
    ctx->r22 = left ? 0x87 : 0x18B;
    ctx->r30 = left ? 0x1E : 0x10C;
    ctx->r9 = 15 * (left ? w : w - 11);
}
#endif
