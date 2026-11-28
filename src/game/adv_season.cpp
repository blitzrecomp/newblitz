// Advanced mode's season: per-week CPU skill and the new-season reset (the engine is adv_engine.cpp).
#if BLITZ_MODERN
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "recomp.h"
#if BLITZ_MODERN
#include "team_store.h"
#endif
#include "blitz_hooks.h"
#include "adv_league.h"
#include "blitz_savestate.h"

extern "C" void srand_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void rand_mod(uint8_t* rdram, recomp_context* ctx);

namespace blitz {
    void adv_save_new_season();  // adv_save.cpp
    void season_new_started();   // season_settings.cpp: "Standard or Advanced season?" (league_select.cpp)
    void lineup_overlay_now(uint8_t* rdram);  // content_pack_tp.cpp: this week's lineup
}

namespace {
    constexpr int team_count = adv::teams;
    constexpr uint32_t cpu_skill = 0x8033B988;      // s32 per team (ovl_b bss; room for 32)
    constexpr uint32_t seed_store = 0x8032E91C;     // the game keeps its srand value here (nothing reads it)
    constexpr uint32_t rec = 0x802EF6F0, rec_week = rec + 1;
    constexpr uint32_t rec_results = rec + 0x37F, rec_results_end = rec_results + adv::total_weeks * adv::max_games * 2;
    constexpr uint32_t rec_stats31 = rec + 0x704, rec_seeds = rec + 0x758;

    uint64_t season_seed = 0;
    [[maybe_unused]] const bool registered = blitz::savestate_register_pod("adv_season_seed", &season_seed);  // save states

    gpr guest(uint32_t vaddr) { return (gpr)(int32_t)vaddr; }
    void wr8(uint8_t* rdram, uint32_t a, uint8_t v) { MEM_B(0, guest(a)) = (int8_t)v; }
    uint8_t rd8(uint8_t* rdram, uint32_t a) { return (uint8_t)MEM_B(0, guest(a)); }
}

extern "C" {
    // BLITZ_ADV_AUTOSIM=1 (tests): the user's games are simulated like everyone else's (Advanced, and
    // Standard's 32-team seasons).
    int blitz_adv_autosim(void) {
        static const int on = [] {
            const char* e = getenv("BLITZ_ADV_AUTOSIM");
            return e != nullptr && e[0] == '1';
        }();
        return (blitz_adv || blitz_s32) && on;
    }

    // End of the season record reset (main 0x80263AEC): a new season. Results "not played", no
    // seeds, team 31's stats zero (the reset's 0xFF fill would be NaN), a new seed and save identity.
    void blitz_adv_new_season(uint8_t* rdram) {
        for (uint32_t a = rec_results; a < rec_results_end; a++) wr8(rdram, a, 0xFF);
        for (uint32_t a = rec_stats31; a < rec_stats31 + 28; a++) wr8(rdram, a, 0);
        for (uint32_t a = rec_seeds; a < rec_seeds + 2 * adv::seeds; a++) wr8(rdram, a, 0xFF);
        std::random_device rd;
        season_seed = ((uint64_t)rd() << 32) | rd();
        blitz::adv_save_new_season();
        fprintf(stderr, "[adv] new season, seed %016llX\n", (unsigned long long)season_seed);
        blitz::season_new_started();
    }

    // Host version of ovl_b 0x802FBAAC (each week, before the games): every team's CPU skill from its
    // team block; from week 2 on, four random teams get 13 - skill; in the playoffs skill ramps up a
    // point a week (capped at 11). The game seeds rand with a CRC of week 1's results (the same value
    // every week); Advanced seeds it from the season and the week.
    void blitz_adv_cpu_skill(uint8_t* rdram, recomp_context* ctx) {
        blitz::lineup_overlay_now(rdram);
        for (int t = 0; t < team_count; t++) {
            MEM_W(0, guest(cpu_skill + 4 * t)) = (int32_t)blitz::teamdata::live::cpu_skill(rdram, (int)t);
        }
        uint32_t week = rd8(rdram, rec_week);
        if (week != 0) {
            uint32_t seed = (uint32_t)(season_seed ^ (season_seed >> 32)) ^ (week * 0x9E3779B9u);
            MEM_W(0, guest(seed_store)) = (int32_t)seed;
            ctx->r4 = (int32_t)seed;
            srand_recomp(rdram, ctx);
            for (int i = 0; i < 4; i++) {
                ctx->r4 = team_count;
                rand_mod(rdram, ctx);
                uint32_t a = cpu_skill + 4 * ((uint32_t)ctx->r2 % team_count);
                MEM_W(0, guest(a)) = 13 - (int32_t)MEM_W(0, guest(a));
            }
        }
        if (week >= (uint32_t)adv::wildcard && week < (uint32_t)adv::total_weeks) {
            for (int t = 0; t < team_count; t++) {
                int32_t v = (int32_t)MEM_W(0, guest(cpu_skill + 4 * t)) - adv::regular_weeks + 1 + (int32_t)week;
                MEM_W(0, guest(cpu_skill + 4 * t)) = v < 12 ? v : 11;
            }
        }
    }
}

namespace blitz {
    uint64_t adv_season_seed() { return season_seed; }
    void adv_set_season_seed(uint64_t seed) { season_seed = seed; }
}
#endif
