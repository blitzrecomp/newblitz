// Settings → Rules → CPU (both apps): Smart CPU Opponent and Smarter Play Calling.
//
// The CPU calls its plays in ovl_a 803300E8 (side): at the end of play select, for every side whose play
// is still unset (the CPU's, or a human's whose play clock ran out). For the offense it first picks a
// call: 0 punt, 1 field goal, 2 normal, 3 long (deep passes), held in s1 when it reaches the switch at
// 0x80330300, then turns it into a play (fake punts and fake field goals included, by the game's rand).
// The conversion after a touchdown is decided by 8033067C (go for two: 1). Same code and addresses in
// vanilla and team+. Research and the full decision tree: build/analysis/cpu_logic.md.
//
// Smarter Play Calling (off: every hook below returns without changing anything), CPU sides only:
//  - End of a half: with 13 s or less left in the 2nd or 4th quarter (or overtime) and a field goal in
//    range, kick it (in the 4th and overtime only when it ties or takes the lead, or the CPU leads).
//    The game only does so under 8 s, and a play can take 13 s of clock, so the half could run out.
//  - No long tries: the game kicks on any 4th down in the other half in the 2nd quarter (a 65-yard
//    try, say). Beyond 55 yards it goes for it instead with 5 yards or less to go, else punts; on the
//    last play of a half it throws deep.
//  - Fewer fakes: the game swaps half its 4th-down tries for a fake punt and fakes about a quarter of
//    its punts; now a fake punt only with 5 yards or less to go before the 4th quarter, one time in
//    five; a fake field goal (the game: most of them when 4 or more behind) only on 4th and 5 or less
//    and never at the end of a half.
//  - Two-point tries by the clock: before the 4th quarter the CPU kicks; from the 4th on it follows the
//    usual chart (go for two when behind by 2, 5, 9, 10, 12 or 16, or ahead by 1, 5 or 12).
// Yards to go are measured to the first-down marker, so they hold for any first-down distance.
//
// Rolls use the game's own random seed (read, never advanced), so a game plays the same on Redo Play.
#include <cstdint>
#include <cstdio>
#include <string>

#include "recomp.h"
#include "recompui/config.h"

namespace {
    constexpr const char* tab = "rules";

    // Game state (the same in both ROMs; play_log.cpp).
    constexpr uint32_t quarter = 0x802EA424;   // 0-3 Q1-Q4, 4+ overtime
    constexpr uint32_t game_clock = 0x802EA428;// minutes << 16 | seconds << 8 | tick
    constexpr uint32_t points = 0x802EA43C;    // [2][7]
    constexpr uint32_t offense = 0x802EA47C;
    constexpr uint32_t spot = 0x802EA48C;      // yards from the offense's own goal line
    constexpr uint32_t down = 0x802EA498;
    constexpr uint32_t marker = 0x802EA49C;    // first-down marker (100 = goal to go)
    constexpr uint32_t play_kind = 0x802EA508; // 4 = conversion try
    constexpr uint32_t humans = 0x802EA418;    // controllers in the game, by port
    constexpr uint32_t four_player = 0x80283BE8;  // ports 0-1 side 0, 2-3 side 1 (else port n = side n)
    constexpr uint32_t rand_seed = 0x8005BAF0; // boot rand()'s LCG state
    constexpr uint32_t smart_cpu_code = 0x80282EB0;  // "SMART CPU OPPONENT" (3-1-4 Down)

    constexpr int max_fg_yards = 55;  // the CPU's kick: 3 * spot - 125 percent before its skill bonus
    constexpr int end_of_half = 13;   // seconds: the longest plays take about this much clock
    constexpr int last_play = 8;      // the game's own "last play" line

    int32_t rd(uint8_t* rdram, uint32_t a) { return MEM_W(0, (gpr)(int32_t)a); }

    bool option(const char* id) {
        try {
            return std::get<bool>(recompui::config::get_config(tab).get_option_value(id));
        }
        catch (...) {
            return false;
        }
    }

    int score(uint8_t* rdram, int side) {
        int t = 0;
        for (int q = 0; q < 7; q++) t += rd(rdram, points + (side * 7 + q) * 4);
        return t;
    }

    bool side_has_human(uint8_t* rdram, int side) {
        uint32_t h = (uint32_t)rd(rdram, humans);
        uint32_t mask = rd(rdram, four_player) ? (side ? 0xC : 0x3) : (side ? 0x2 : 0x1);
        return (h & mask) != 0;
    }

    // 0..n-1 from the game's seed without advancing it (bits above the ones rand() returns last).
    int roll(uint8_t* rdram, int n) { return (int)(((uint32_t)rd(rdram, rand_seed) >> 20) % (uint32_t)n); }

    struct Situation {
        int side, quarter, seconds, spot, togo, down, deficit;
        bool end_of_half() const { return (quarter == 1 || quarter >= 3) && seconds <= ::end_of_half; }
        int fg_yards() const { return 100 - spot + 17; }
        bool in_range() const { return fg_yards() <= max_fg_yards; }
    };

    Situation situation(uint8_t* rdram, int side) {
        Situation s;
        s.side = side;
        s.quarter = rd(rdram, quarter);
        int c = rd(rdram, game_clock);
        s.seconds = ((c >> 16) & 0xFF) * 60 + ((c >> 8) & 0xFF);
        s.spot = rd(rdram, spot);
        int mk = rd(rdram, marker);
        s.togo = (mk >= 100 ? 100 : mk) - s.spot;
        s.down = rd(rdram, down);
        s.deficit = score(rdram, side ? 0 : 1) - score(rdram, side);
        return s;
    }

    // Smarter Play Calling applies to this call: the option is on and a CPU offense is calling a scrimmage play.
    bool smart_offense(uint8_t* rdram, int side) {
        return option("smart_calls") && rd(rdram, play_kind) != 4 && rd(rdram, offense) == side && !side_has_human(rdram, side);
    }

    const char* call_name(int call) {
        static const char* names[] = {"punt", "field goal", "normal", "long"};
        return call >= 0 && call <= 3 ? names[call] : "?";
    }
}

namespace blitz {
    // Settings → Rules: the CPU section (after Play Clock).
    void add_cpu_rules(recomp::config::Config& c) {
        c.add_bool_option("smart_cpu", "Max-Skill CPU (code)",
                          "The game's own Smart CPU code (3-1-4 Down): the CPU plays at its top skill level all game. "
                          "It makes the CPU tougher, not smarter: its catches and interceptions get its best odds and "
                          "its field goals always go in. 1-player games; not in Season (the game doesn't allow it there).",
                          false);
        c.add_bool_option("smart_calls", "Smarter Play Calling",
                          "The CPU calls its plays more like a coach. It kicks the field goal with 13 seconds or less "
                          "left in a half when one is in range (in the 4th quarter when the kick ties or takes the "
                          "lead). It doesn't try field goals longer than 55 yards: it goes for it on 4th and short, "
                          "or punts. It fakes punts and field goals far less often. It goes for two only from the "
                          "4th quarter on, by the usual chart. Off: the CPU calls plays as the game shipped.",
                          false);
        recompui::config::set_option_section(tab, "smart_cpu", "CPU");
    }

    // VS screen, after the game clears its codes (house_rules.cpp): the Smart CPU code.
    void cpu_rules_vs_screen(uint8_t* rdram, bool season) {
        if (option("smart_cpu") && !season) MEM_W(0, (gpr)(int32_t)smart_cpu_code) = 0xF;
    }
}

extern "C" {
    // ovl_a 0x80330300: the offense's call is in s1 (r17), the side in s2 (r18).
    void blitz_cpu_offense_call(uint8_t* rdram, recomp_context* ctx) {
        int side = (int32_t)ctx->r18;
        if (!smart_offense(rdram, side)) return;
        Situation s = situation(rdram, side);
        int call = (int32_t)ctx->r17, was = call;
        bool kick_ok = s.quarter == 1 || s.deficit <= 3;  // 4th quarter and overtime: ties, leads or extends
        if (s.end_of_half() && s.in_range() && kick_ok) {
            call = 1;
        }
        else if (call == 1 && !s.in_range()) {
            if (s.end_of_half() && s.seconds < last_play) call = 3;  // a deep throw beats a hopeless kick
            else if (s.down == 4) call = s.togo <= 5 ? 2 : 0;
            else call = 2;
        }
        if (call != was) {
            ctx->r17 = (gpr)call;
            fprintf(stderr, "[cpu] Q%d %d:%02d, down %d, %d to go at %d, %+d: %s instead of %s\n", s.quarter + 1,
                    s.seconds / 60, s.seconds % 60, s.down, s.togo, s.spot, -s.deficit, call_name(call), call_name(was));
        }
    }

    // ovl_a 0x803303FC: a 4th-down try's rand(2) is in v0 (r2); not 0 swaps the play for a fake punt.
    void blitz_cpu_fourth_down_fake(uint8_t* rdram, recomp_context* ctx) {
        int side = (int32_t)ctx->r18;
        if (!smart_offense(rdram, side) || ctx->r2 == 0) return;
        Situation s = situation(rdram, side);
        if (s.togo <= 5 && s.quarter < 3 && roll(rdram, 5) == 0) return;  // keep this fake
        ctx->r2 = 0;
    }

    // ovl_a 0x80330334 (rand(5)) and 0x8033034C (rand(10)): a punt is faked when v0 is 0.
    void blitz_cpu_punt_fake(uint8_t* rdram, recomp_context* ctx, int second_roll) {
        int side = (int32_t)ctx->r18;
        if (!smart_offense(rdram, side) || ctx->r2 != 0) return;
        Situation s = situation(rdram, side);
        if (!second_roll && s.togo <= 5 && s.quarter < 3) return;  // the game's 1 in 5, on 4th and short only
        ctx->r2 = 1;
    }

    // ovl_a 0x80330388: the field goal call's rand(100) in v0; under 16 kicks (else a fake when 4+ behind).
    void blitz_cpu_fg_fake(uint8_t* rdram, recomp_context* ctx) {
        int side = (int32_t)ctx->r18;
        if (!smart_offense(rdram, side)) return;
        Situation s = situation(rdram, side);
        if (s.togo > 5 || s.end_of_half()) ctx->r2 = 0;
    }

    // ovl_a 8033067C (side) entry: 1 = go for two. Returns 1 when it has answered in v0 (r2).
    int blitz_cpu_go_for_two(uint8_t* rdram, recomp_context* ctx) {
        if (!option("smart_calls")) return 0;
        int side = (int32_t)ctx->r4;
        Situation s = situation(rdram, side);
        bool go = false;
        if (s.quarter >= 3) {
            switch (s.deficit) {
                case 2: case 5: case 9: case 10: case 12: case 16:  // behind by
                case -1: case -5: case -12:                        // ahead by
                    go = true;
                    break;
                default:
                    break;
            }
        }
        ctx->r2 = go ? 1 : 0;
        fprintf(stderr, "[cpu] conversion, Q%d, %+d: %s\n", s.quarter + 1, -s.deficit, go ? "go for two" : "kick");
        return 1;
    }
}
