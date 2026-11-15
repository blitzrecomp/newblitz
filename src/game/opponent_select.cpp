// Choose your CPU opponent (both apps; Settings → Rules → Choose CPU Opponent).
//
// In a 1-player Arcade game the game picks the CPU's team itself (a ladder of team tiers, the first
// with a team the player's record hasn't beaten yet; a random team for guests). With the setting on,
// the empty side of team select becomes the opponent list once the player has picked: that side's
// selection process (ovl_b 802F90B8) already runs, parked until a human joins its side, so it is let
// in with the player's controller, as if a challenger had joined. The cursor starts on the team the
// game would have picked, so confirming straight away plays like the original. The pick replaces the
// game's own CPU pick after its CPU block, which still sets the difficulty bookkeeping.
//
// The same last step also forces the CPU's team when an interrupted game is being resumed
// (checkpoint.cpp), with or without the setting.
//
// Hooks (blitz2000.toml; same addresses and code in vanilla and team+): 802F8594 entry (reset),
// 802F90B8 0x802F9174 (wake: the join test passes) and 0x802F91F8 (controller and start team),
// 802F5868 0x802F591C (the "please wait" panel leaves), 802F8594 0x802F89CC (team select waits for
// our pick) and 0x802F8BC8 (after the CPU block: our team). Research: build/analysis/opponent.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "recomp.h"
#include "recompui/config.h"
#include "blitz_savestate.h"

extern "C" {
    void func_80256580(uint8_t* rdram, recomp_context* ctx);        // a side's QB model: team's uniform
    // Text (main): font, group, position, then create from a format string; delete by group.
    void func_802796A4(uint8_t* rdram, recomp_context* ctx);
    void func_80279748(uint8_t* rdram, recomp_context* ctx);
    void func_80279670(uint8_t* rdram, recomp_context* ctx);
    void func_8027A374(uint8_t* rdram, recomp_context* ctx);
    void func_8027A010(uint8_t* rdram, recomp_context* ctx);
    void ovl_b_func_802FA080(uint8_t* rdram, recomp_context* ctx);  // a side's list, logo, ratings, QB record
}

namespace blitz {
    int checkpoint_opponent(uint8_t* rdram, int human_side, int human_team);  // checkpoint.cpp, or -1
}

namespace {
    constexpr uint32_t game_mode = 0x8028B998;  // 0 Arcade, 1 Tournament, 2 Season, 3 Play Editor
    constexpr uint32_t phase = 0x802EA420;      // 7 = attract demo
    constexpr uint32_t humans = 0x802EA418;     // bit per port with a human
    constexpr uint32_t four_player = 0x80283BE8;
    constexpr uint32_t slot_mask = 0x80283C68;
    constexpr uint32_t chosen = 0x802EA434;     // [side] team, -1 while choosing
    constexpr uint32_t busy = 0x802BD864;       // u16, a bit per port whose selection is running
    constexpr uint32_t ladder = 0x802AB0E0;     // 9 tiers x 5 words, each ended by -1
    constexpr uint32_t ladder_done = 0x80338C28;// 4 teams once every team is beaten
    // Win records of the human side's two ports (byte 0 set = a record is loaded; +0x14 = beaten mask).
    constexpr uint32_t records[2][2] = {{0x802BE440, 0x802E03F8}, {0x802CA790, 0x802EB2A0}};

    int32_t rd(uint8_t* rdram, uint32_t a) { return MEM_W(0, (gpr)(int32_t)a); }
    void wr(uint8_t* rdram, uint32_t a, int32_t v) { MEM_W(0, (gpr)(int32_t)a) = v; }

    bool setting() {
        return std::get<bool>(recompui::config::get_config("rules").get_option_value("choose_opponent"));
    }

    struct State {
        bool woken = false;
        int cpu = -1, slot = 0, team = -1, frames = 0, start = -1;
        bool label = false;
    } g;
    [[maybe_unused]] const bool registered = blitz::savestate_register_pod("opponent_select", &g);  // save states

    // A 1-player-side Arcade team select: the CPU side and the human side's choosing port.
    bool context(uint8_t* rdram, int& cpu, int& slot) {
        if (rd(rdram, game_mode) != 0 || rd(rdram, phase) == 7 || rd(rdram, four_player) == 0) return false;
        uint32_t h = (uint32_t)rd(rdram, humans), m = (uint32_t)rd(rdram, slot_mask);
        bool left = (h & 3) != 0, right = (h & 0xC) != 0;
        if (left == right) return false;
        cpu = left ? 1 : 0;
        slot = left ? ((m & 1) ? 0 : (m & 2) ? 1 : 0) : ((m & 4) ? 2 : (m & 8) ? 3 : 0);
        return true;
    }

    int team_count(uint8_t* rdram) {
#if BLITZ_TEAMPLUS
        int n = (uint8_t)MEM_B(0, (gpr)(int32_t)0x800248FC);
        return n > 0 && n <= 64 ? n : 31;
#elif BLITZ_ENGINE_ORIGINAL
        (void)rdram;
        return 32;  // Modern on the original game: its team tables hold 32 (engine32.cpp)
#else
        (void)rdram;
        return 31;
#endif
    }

    // The team the game would pick (ovl_b 802FA5F4), to start the cursor on.
    int game_pick(uint8_t* rdram, int cpu) {
        if (const char* e = getenv("BLITZ_OPP_START")) return atoi(e);  // tests: a fixed starting team
        int human_side = 1 - cpu, human_team = rd(rdram, chosen + 4 * human_side);
        uint32_t beaten = 0;
        bool record = false;
        for (uint32_t r : records[human_side]) {
            if ((uint8_t)MEM_B(0, (gpr)(int32_t)r) != 0) {
                record = true;
                beaten |= (uint32_t)rd(rdram, r + 0x14);
            }
        }
        if (!record) {
            int n = team_count(rdram), t = rand() % n;  // every team (Modern's game pick is rand(32) too); a league pak has fewer
            return t == human_team ? (t + 1) % n : t;
        }
        for (int tier = 0; tier < 9; tier++) {
            std::vector<int> open;
            for (int k = 0; k < 5; k++) {
                int t = rd(rdram, ladder + tier * 0x14 + 4 * k);
                if (t < 0) break;
                if (!((beaten >> t) & 1)) open.push_back(t);
            }
            if (!open.empty()) return open[rand() % open.size()];
        }
        return rd(rdram, ladder_done + 4 * (rand() % 4));
    }
}

extern "C" {
    // Team select starts. Both picks read "not chosen yet" from here on (each side's selection
    // process sets its own to -1 when it starts, but the CPU side's may run first and see the
    // previous game's value).
    void blitz_opp_reset(uint8_t* rdram) {
        g = State{};
        wr(rdram, chosen, -1);
        wr(rdram, chosen + 4, -1);
    }

    // The CPU side's selection process, parked at its join test: let it in once the player has picked.
    int blitz_opp_wake(uint8_t* rdram, int32_t side) {
        if (g.woken) return side == g.cpu;
        int cpu, slot;
        if (!setting() || !context(rdram, cpu, slot) || side != cpu) return 0;
        if (rd(rdram, chosen + 4 * (1 - cpu)) == -1 || (uint16_t)MEM_HU(0, (gpr)(int32_t)busy) != 0) return 0;
        g.woken = true;
        g.cpu = cpu;
        g.slot = slot;
        return 1;
    }

    // Right after the join. The process's port (s7) picks the player marker and the busy/animation bits,
    // so it gets the matching port on the other side: the list shows "3" for player 1 (players 3 and 4
    // are the other team; the team), "1" for player 3, while its input reads are pointed back at the player
    // (blitz_opp_input).
    // The list still starts on the first team: the QB
    // model process is started with it and only sets itself up right for that one, so the starting
    // team is switched to a few frames later (blitz_opp_uniform), as a move would.
    void blitz_opp_setup(uint8_t* rdram, int32_t side, gpr* controller, gpr* team) {
        (void)team;
        if (!g.woken || side != g.cpu) return;
        *controller = g.slot ^ 2;
        int human_team = rd(rdram, chosen + 4 * (1 - g.cpu));
        int resume = blitz::checkpoint_opponent(rdram, 1 - g.cpu, human_team);
        g.start = resume >= 0 ? resume : game_pick(rdram, g.cpu);
    }

    // "CHOOSE OPPONENT" above the opponent's logo, in the list's font, yellow like the selected team.
    // Text settings are globals shared by every process, so they're saved and put back. Group 0x666 is
    // unused by the game, and team select's exit deletes it (0x802F8CBC), so the label can't outlive it.
    constexpr uint32_t label_text = 0x80404000;  // free RDRAM (Advanced's strings end near 0x80400600)
    constexpr int32_t label_group = 0x666;

    void show_label(uint8_t* rdram, recomp_context* ctx) {
        static const char text[] = "\x1b%dj\x1b%dcCHOOSE OPPONENT";
        for (size_t i = 0; i < sizeof(text); i++) MEM_B(0, (gpr)(int32_t)(label_text + i)) = (int8_t)text[i];
        int32_t saved[17];
        for (int k = 0; k < 3; k++) saved[k] = rd(rdram, 0x802A9C50 + 4 * k);   // not 0x802A9C5C: the list head
        for (int k = 0; k < 14; k++) saved[3 + k] = rd(rdram, 0x802A9C60 + 4 * k);
        gpr regs[5] = {ctx->r2, ctx->r4, ctx->r5, ctx->r6, ctx->r7};
        ctx->r4 = 0x14;  // the list's font
        func_802796A4(rdram, ctx);
        ctx->r4 = label_group;
        func_80279748(rdram, ctx);
        ctx->r4 = g.cpu == 1 ? 435 : 77;  // over the opponent's logo (512x384, y up, centred)
        ctx->r5 = 346;
        ctx->r6 = 0x3F800000;  // z 1.0
        func_80279670(rdram, ctx);
        ctx->r4 = (int32_t)label_text;
        ctx->r5 = 0xA;                     // centred
        ctx->r6 = (int32_t)0xFFFFFF00;     // yellow
        func_8027A374(rdram, ctx);
        for (int k = 0; k < 3; k++) wr(rdram, 0x802A9C50 + 4 * k, saved[k]);
        for (int k = 0; k < 14; k++) wr(rdram, 0x802A9C60 + 4 * k, saved[3 + k]);
        ctx->r2 = regs[0]; ctx->r4 = regs[1]; ctx->r5 = regs[2]; ctx->r6 = regs[3]; ctx->r7 = regs[4];
    }

    // The selection loop's exit (0x802F9CD0, confirm or timeout): the label goes.
    void blitz_opp_label_off(uint8_t* rdram, recomp_context* ctx) {
        if (!g.label || (int32_t)ctx->r21 != g.cpu) return;
        gpr a0 = ctx->r4, a1 = ctx->r5, v0 = ctx->r2;
        ctx->r4 = label_group;
        ctx->r5 = -1;
        func_8027A010(rdram, ctx);
        ctx->r4 = a0; ctx->r5 = a1; ctx->r2 = v0;
        g.label = false;
    }

    // Each frame of the selection loop (0x802F9A10): on the 3rd, the cursor moves to the starting team
    // the way a button press moves it: the uniform (busy flag 0x8033BAB0 set around it), then the
    // list redraw, which also points the QB model at the team's record.
    void blitz_opp_uniform(uint8_t* rdram, recomp_context* ctx) {
        int32_t side = (int32_t)ctx->r21, team = g.start;
        if (g.woken && side == g.cpu && !g.label && getenv("BLITZ_OPP_NO_LABEL") == nullptr) {
            show_label(rdram, ctx);
            g.label = true;
        }
        static const bool skip = getenv("BLITZ_OPP_NO_UNIFORM") != nullptr;  // tests
        if (skip || !g.woken || side != g.cpu || team <= 0 || ++g.frames != 3) return;
        ctx->r22 = team;  // the list's cursor (s6)
        gpr a0 = ctx->r4, a1 = ctx->r5, a2 = ctx->r6;
        ctx->r4 = side;
        ctx->r5 = team;
        ctx->r6 = 0;
        wr(rdram, 0x8033BAB0, 1);
        func_80256580(rdram, ctx);
        wr(rdram, 0x8033BAB0, 0);
        // Then the redraw a move does: it also points the model at the team's QB record (the model
        // process set it for the first team only), without which the model doesn't show.
        ctx->r4 = team;
        ctx->r5 = side;
        ctx->r6 = 1;
        ovl_b_func_802FA080(rdram, ctx);
        ctx->r4 = a0;
        ctx->r5 = a1;
        ctx->r6 = a2;
    }

    // Before each of the selection loop's three input reads (0x802F9A18, 0x802F9A28, 0x802F9A34): the
    // port read is the player's, not the marker's.
    void blitz_opp_input(recomp_context* ctx) {
        if (g.woken && (int32_t)ctx->r21 == g.cpu) ctx->r4 = g.slot;
    }

    // The "please wait" panel on the CPU side leaves when the opponent list opens.
    int blitz_opp_panel_off(int32_t slot) {
        return g.woken && ((slot >= 2) == (g.cpu == 1));
    }

    // Team select waits for every running selection; also for ours while it hasn't started or picked.
    int blitz_opp_hold(uint8_t* rdram) {
        static const bool dbg = getenv("BLITZ_OPP_LOG") != nullptr;
        if (dbg) {
            static int n = 0;
            if (n++ % 30 == 0)
                fprintf(stderr, "[opp] hold mode=%d phase=%d 4p=%d humans=%x mask=%x chosen=%d,%d busy=%x woken=%d setting=%d\n",
                        rd(rdram, game_mode), rd(rdram, phase), rd(rdram, four_player), rd(rdram, humans), rd(rdram, slot_mask),
                        rd(rdram, chosen), rd(rdram, chosen + 4), (uint16_t)MEM_HU(0, (gpr)(int32_t)busy), g.woken, setting());
        }
        int cpu, slot;
        if (!setting() || !context(rdram, cpu, slot)) return 0;
        int32_t t = rd(rdram, chosen + 4 * cpu);
        if (g.woken && t != -1) {
            g.team = t;
            return 0;
        }
        return 1;
    }

    // The CPU side whose list the player is choosing from right now, or -1 (uniform_picker.cpp).
    int blitz_opp_choosing_side() {
        return g.woken ? g.cpu : -1;
    }
    // The port choosing for that side: the player's (its list reads them through blitz_opp_input).
    int blitz_opp_choosing_port() {
        return g.woken ? g.slot : -1;
    }

    // Pick Your Crew (crew.cpp) shows the CPU's team before the game picks it: so it's picked here, as the game
    // would (or the resumed game's opponent), and kept for blitz_opp_restore to put in after the game's CPU
    // block. -1 outside a 1-player Arcade game.
    int blitz_opp_preview(uint8_t* rdram) {
        int cpu, slot;
        if (!context(rdram, cpu, slot)) return -1;
        int32_t t = rd(rdram, chosen + 4 * cpu);
        if (t >= 0) return t;
        if (g.cpu == cpu && g.team >= 0) return g.team;
        int human_team = rd(rdram, chosen + 4 * (1 - cpu));
        int resume = blitz::checkpoint_opponent(rdram, 1 - cpu, human_team);
        g.cpu = cpu;
        g.team = resume >= 0 ? resume : game_pick(rdram, cpu);
        return g.team;
    }

    // After the game's CPU pick: ours, or the resumed game's opponent.
    void blitz_opp_restore(uint8_t* rdram) {
        int cpu = g.cpu, slot;
        if (cpu >= 0 || context(rdram, cpu, slot)) {
            int team = g.team;
            if (team < 0) team = blitz::checkpoint_opponent(rdram, 1 - cpu, rd(rdram, chosen + 4 * (1 - cpu)));
            if (team >= 0) wr(rdram, chosen + 4 * cpu, team);
            g = State{};
        }
        // The game's guest pick is rand(31) (team+ never changed it): with a league pak's fewer teams,
        // a pick past the list becomes one of them, not the other side's.
        int n = team_count(rdram);
        for (int s = 0; s < 2; s++) {
            int t = rd(rdram, chosen + 4 * s), other = rd(rdram, chosen + 4 * (1 - s));
            if (t < n) continue;
            t = rand() % n;
            if (t == other && n > 1) t = (t + 1) % n;
            wr(rdram, chosen + 4 * s, t);
        }
    }
}
