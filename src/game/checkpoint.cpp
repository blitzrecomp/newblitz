// Resume an interrupted game (both apps; Settings → General → Resume Interrupted Games (Experimental),
// off by default). The team's design (STATUS.md): saving is a choice, and a crash asks before recovering.
//   - Quitting the game from the pause menu (YES to "ARE YOU SURE?") asks "SAVE THIS GAME?"; quitting
//     the app mid-game asks "Save this game?" (blitz_quit_prompt). Yes keeps the checkpoint as saved.
//   - After a crash the checkpoint is still an autosave; the next launch asks "Recover last game?"
//     (blitz::checkpoint_startup_prompt). Discard deletes it.
//   - A saved checkpoint is restored when the same teams meet again: the VS screen says "RESUMING ..."
//     and the first-play tutorial tips are skipped.
//
// Before every play the game situation goes to <app folder>/checkpoint.bin: score by quarter, clock,
// quarter, possession, down, spot, kicker of the opening kickoff, play kind, team stats and the
// on-fire state. A game that ends normally deletes it. If the app crashed (or was quit mid-game), the
// next game between the same two teams on the same sides starts from that situation instead of the
// opening kickoff, once: the checkpoint is used up when it is restored.
//
// Hooks (blitz2000.toml; the game loop is ovl_a_func_802FEF08, team+ tp_game_process, with the same
// code at these addresses; research in docs/SAVE_STATES.md and build/analysis/checkpoint):
//   0x802FF684  game init, after every init write and before any process is spawned: restore here,
//               so the scoreboard and clock processes start from the restored values
//   0x802FF8E8  top of the per-play loop: the state is exactly "the next play": save
//   0x80302138  game over: delete
//   0x80303690  the clock process's loop: it keeps the clock in s0 and only ever stores it, so a
//               restored clock is handed to it here, once
#include "../main/platform_compat.h"
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "recomp.h"
#include "util/file.h"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "ultramodern/ultramodern.hpp"
#include "blitz_savestate.h"

extern "C" void blitz_redo_play_top(uint8_t* rdram);  // savestate.cpp
extern "C" void blitz_redo_clear();

namespace blitz {
    void post_notice(const std::string& msg);  // notices.cpp
}

namespace {
    constexpr uint32_t magic = 0x424C4350;  // "BLCP"
    constexpr uint32_t version = 2;  // 2: a "saved" word and the team abbreviations in the header
#if BLITZ_TEAMPLUS
    constexpr uint32_t flavour = 2026;
#elif BLITZ_MODERN
    constexpr uint32_t flavour = 2027;  // Modern on the original game
#else
    constexpr uint32_t flavour = 2000;
#endif
    constexpr uint32_t phase = 0x802EA420, quarter = 0x802EA424, game_clock = 0x802EA428;
    constexpr uint32_t team_slot = 0x802EA434, points = 0x802EA43C, game_mode = 0x8028B998;
    // Word ranges saved and restored, in order.
    struct Range { uint32_t addr, words; };
    constexpr Range ranges[] = {
        {0x802EA424, 1},   // quarter
        {0x802EA43C, 14},  // points [2 sides][7 periods]
        {0x802EA478, 4},   // opening kicker side, offense, offense copies (0x480, 0x484)
        {0x802EA48C, 1},   // ball spot
        {0x802EA498, 2},   // down, first-down marker
        {0x802EA508, 1},   // play kind
        {0x802EA540, 2},   // on fire, per side
        {0x802EA578, 1},   // passer rotation (2 humans on a side)
        {0x802CA7D0, 0x48},// team stats, both sides (0x90 bytes each)
    };

#if BLITZ_TEAMPLUS
    constexpr uint32_t abbr[2] = {0x802E9448, 0x802E94AC};
#else
    constexpr uint32_t abbr[2] = {0x802E9458, 0x802E94BC};
#endif
    constexpr int head_words = 10;  // magic, version, flavour, mode, slot0, slot1, clock, saved, abbr0, abbr1

    std::mutex file_mutex;              // the game thread saves, the UI thread marks or discards
    std::atomic<bool> in_progress{false};  // a checkpointed game is being played right now

    int32_t rd(uint8_t* rdram, uint32_t a) { return MEM_W(0, (gpr)(int32_t)a); }
    void wr(uint8_t* rdram, uint32_t a, int32_t v) { MEM_W(0, (gpr)(int32_t)a) = v; }

    std::filesystem::path file() { return recompui::file::get_app_folder_path() / "checkpoint.bin"; }

    bool enabled() {
        return std::get<bool>(recompui::config::get_general_config().get_option_value("experimental_resume"));
    }

    struct Checkpoint {
        uint32_t mode = 0, slot0 = 0, slot1 = 0, clock = 0, saved = 0;
        char team[2][5] = {};
        std::vector<int32_t> words;

        int quarter() const { return words.empty() ? 0 : words[0]; }
        int score(int side) const {
            int t = 0;
            for (int q = 0; q < 7; q++) t += words[1 + side * 7 + q];
            return t;
        }
    };

    bool read_checkpoint(Checkpoint& cp) {
        std::lock_guard lock{file_mutex};
        std::ifstream f{file(), std::ios::binary};
        if (!f) return false;
        uint32_t head[head_words];
        if (!f.read(reinterpret_cast<char*>(head), sizeof(head))) return false;
        if (head[0] != magic || head[1] != version || head[2] != flavour) return false;
        cp.mode = head[3];
        cp.slot0 = head[4];
        cp.slot1 = head[5];
        cp.clock = head[6];
        cp.saved = head[7];
        memcpy(cp.team[0], &head[8], 4);
        memcpy(cp.team[1], &head[9], 4);
        size_t n = 0;
        for (const Range& r : ranges) n += r.words;
        cp.words.resize(n);
        return (bool)f.read(reinterpret_cast<char*>(cp.words.data()), n * 4);
    }

    void write_file(const std::vector<uint32_t>& out) {
        std::lock_guard lock{file_mutex};
        std::filesystem::path tmp = file();
        tmp += ".tmp";
        {
            std::ofstream f{tmp, std::ios::binary | std::ios::trunc};
            if (!f.write(reinterpret_cast<const char*>(out.data()), out.size() * 4)) return;
        }
        std::error_code ec;
        blitz::replace_file(tmp, file(), ec);  // atomic: a crash mid-write keeps the previous one
    }

    std::atomic_bool keeping_other{false};  // a kept game for other teams is on disk: leave it alone

    void discard() {
        std::lock_guard lock{file_mutex};
        std::error_code ec;
        std::filesystem::remove(file(), ec);
    }

}

namespace blitz {
    // The game was saved (or deliberately left) on quitting through save states: the autosave
    // checkpoint isn't a crash, so it doesn't ask "Recover last game?" next time.
    void checkpoint_discard_on_quit() {
        in_progress = false;
        if (!keeping_other) discard();
    }
}

namespace {
    // Keep the checkpoint as a deliberate save.
    void mark_saved() {
        std::vector<uint32_t> all;
        {
            std::lock_guard lock{file_mutex};
            std::ifstream f{file(), std::ios::binary | std::ios::ate};
            if (!f) return;
            all.resize((size_t)f.tellg() / 4);
            f.seekg(0);
            if (!f.read(reinterpret_cast<char*>(all.data()), all.size() * 4) || all.size() < head_words) return;
        }
        all[7] = 1;
        write_file(all);
    }

    const char* quarter_name(int q) {
        static const char* names[] = {"1st quarter", "2nd quarter", "3rd quarter", "4th quarter", "overtime", "2nd overtime", "3rd overtime"};
        return names[q >= 0 && q <= 6 ? q : 0];
    }

    std::string describe(const Checkpoint& cp) {
        char buf[160];
        snprintf(buf, sizeof(buf), "%s %d - %d %s, %s, %d:%02d left", cp.team[0], cp.score(0), cp.score(1), cp.team[1],
                 quarter_name(cp.quarter()), (cp.clock >> 16) & 0xFF, (cp.clock >> 8) & 0xFF);
        return buf;
    }

    bool matches(uint8_t* rdram, const Checkpoint& cp) {
        return cp.saved && cp.mode == (uint32_t)rd(rdram, game_mode) && cp.slot0 == (uint32_t)rd(rdram, team_slot) &&
               cp.slot1 == (uint32_t)rd(rdram, team_slot + 4);
    }

    bool restore_pending = false;
    uint32_t restored_clock = 0;

    // Save states put these back with the game (blitz_savestate.h).
    struct Saved {
        bool in_progress, restore_pending;
        uint32_t restored_clock;
    };
    [[maybe_unused]] const bool registered = (blitz::savestate_register(
        "checkpoint",
        [] {
            Saved v{in_progress.load(), restore_pending, restored_clock};
            return blitz::StateBlob((const uint8_t*)&v, (const uint8_t*)&v + sizeof v);
        },
        [](const blitz::StateBlob& b) {
            if (b.size() != sizeof(Saved)) return;
            Saved v;
            std::memcpy(&v, b.data(), sizeof v);
            in_progress = v.in_progress;
            restore_pending = v.restore_pending;
            restored_clock = v.restored_clock;
        }), true);
}

namespace blitz {
    // The opponent to force for a resumed game: when a checkpoint exists for this mode and the human
    // side's team, the other side's team (opponent_select.cpp writes it after the CPU pick); else -1.
    int checkpoint_opponent(uint8_t* rdram, int human_side, int human_team) {
        if (!enabled()) return -1;
        Checkpoint cp;
        if (!read_checkpoint(cp) || !cp.saved || cp.mode != (uint32_t)rd(rdram, game_mode)) return -1;
        uint32_t mine = human_side == 0 ? cp.slot0 : cp.slot1, theirs = human_side == 0 ? cp.slot1 : cp.slot0;
        return mine == (uint32_t)human_team ? (int)theirs : -1;
    }
}

extern "C" {
    // Top of the per-play loop.
    void blitz_checkpoint_save(uint8_t* rdram) {
        blitz_redo_play_top(rdram);  // savestate.cpp: Redo Play's snapshot of this play
        if (rd(rdram, phase) != 2 || !enabled()) return;
        // A game the player chose to keep waits for its own teams: another game neither replaces nor clears it.
        {
            Checkpoint kept;
            if (read_checkpoint(kept) && kept.saved && !matches(rdram, kept)) {
                keeping_other = true;
                return;
            }
            keeping_other = false;
        }
        auto abbr_word = [&](int side) {
            char t[4] = {};
            for (int i = 0; i < 3; i++) t[i] = (char)MEM_B(0, (gpr)(int32_t)(abbr[side] + i));
            uint32_t w;
            memcpy(&w, t, 4);
            return w;
        };
        std::vector<uint32_t> out = {magic, version, flavour, (uint32_t)rd(rdram, game_mode), (uint32_t)rd(rdram, team_slot),
                                     (uint32_t)rd(rdram, team_slot + 4), (uint32_t)rd(rdram, game_clock), 0u,
                                     abbr_word(0), abbr_word(1)};
        for (const Range& r : ranges)
            for (uint32_t k = 0; k < r.words; k++) out.push_back((uint32_t)rd(rdram, r.addr + 4 * k));
        write_file(out);  // an autosave ("saved" 0) until the player chooses to keep it
        in_progress = true;
    }

    // Game over: nothing to resume.
    void blitz_checkpoint_clear(uint8_t* rdram) {
        (void)rdram;
        blitz_redo_clear();  // game over: nothing to redo
        in_progress = false;
        if (!keeping_other) discard();
    }

    // Game init, before the scoreboard and clock processes start.
    void blitz_checkpoint_restore(uint8_t* rdram) {
        blitz_redo_clear();  // a new game: Redo Play starts from its first play
        restore_pending = false;
        if (rd(rdram, phase) != 2 || !enabled()) return;
        Checkpoint cp;
        if (!read_checkpoint(cp)) return;
        static const bool any_teams = getenv("BLITZ_CHECKPOINT_ANY_TEAMS") != nullptr;  // tests only
        if (!cp.saved || cp.mode != (uint32_t)rd(rdram, game_mode) ||
            (!any_teams && (cp.slot0 != (uint32_t)rd(rdram, team_slot) || cp.slot1 != (uint32_t)rd(rdram, team_slot + 4)))) {
            return;  // another matchup: keep it for when these teams meet again
        }
        size_t i = 0;
        for (const Range& r : ranges)
            for (uint32_t k = 0; k < r.words; k++) wr(rdram, r.addr + 4 * k, cp.words[i++]);
        wr(rdram, game_clock, (int32_t)cp.clock);
        restored_clock = cp.clock;
        restore_pending = true;
        discard();  // used once; this game's autosaves take over
        // No first-play tutorial tips (they covered the first play back): the values the game itself
        // uses for the attract demo (0x803344DC = 2 skips every tips reader).
        wr(rdram, 0x803344DC, 2);
        wr(rdram, 0x80283C24, 0);

        // Quarters already played count for the players' records (full-game credit at the end).
        int q = rd(rdram, quarter);
        uint32_t humans = (uint32_t)rd(rdram, 0x802EA418);
        for (int port = 0; port < 4; port++) {
            if (humans & (1u << port)) {
                uint32_t a = 0x802E94F0 + 4 * port;
                wr(rdram, a, rd(rdram, a) | ((1 << (q < 0 ? 0 : q > 6 ? 6 : q)) - 1));
            }
        }
        fprintf(stderr, "[checkpoint] resumed %s\n", describe(cp).c_str());
    }

    // The clock process's loop head: hand it the restored clock once (it caches the clock in s0).
    void blitz_checkpoint_clock(uint8_t* rdram, recomp_context* ctx) {
        if (!restore_pending) return;
        restore_pending = false;
        ctx->r16 = (int32_t)restored_clock;
        wr(rdram, game_clock, (int32_t)restored_clock);  // it stored the full quarter there on its way in
    }
}

extern "C" {
    void func_80264A20(uint8_t* rdram, recomp_context* ctx);  // the game's YES/NO dialog
    void func_80279670(uint8_t* rdram, recomp_context* ctx);  // text position
    void func_8027A374(uint8_t* rdram, recomp_context* ctx);  // text create (format, args)

    // Pause menu → QUIT GAME → YES (main func_80262D90, before 0x802634DC): "SAVE THIS GAME?" in the
    // game's own dialog. Its text is the "ARE YOU SURE?" string, swapped for the call (same length).
    void blitz_checkpoint_pause_quit(uint8_t* rdram, recomp_context* ctx) {
        in_progress = false;
        if (!enabled() || rd(rdram, phase) != 2) return;
        {
            std::lock_guard lock{file_mutex};
            std::error_code ec;
            if (!std::filesystem::exists(file(), ec)) return;
        }
        constexpr uint32_t text = 0x802AC558;
        static const char ask[24] = "\x1b%dj\x1b%dcSAVE THIS GAME?";
        uint8_t saved_text[24];
        for (int i = 0; i < 24; i++) {
            saved_text[i] = (uint8_t)MEM_B(0, (gpr)(int32_t)(text + i));
            MEM_B(0, (gpr)(int32_t)(text + i)) = (int8_t)ask[i];
        }
        gpr sp = ctx->r29;
        ctx->r29 = sp - 0x20;
        MEM_W(0x10, ctx->r29) = 0x3F800000;  // z 1.0, as the game passes it
        ctx->r4 = 4;
        ctx->r5 = ctx->r30;                 // the pausing player's port
        ctx->r6 = 0x43C00000;               // x 384.0
        ctx->r7 = 0x432C0000;               // y 172.0
        func_80264A20(rdram, ctx);
        ctx->r29 = sp;
        for (int i = 0; i < 24; i++) MEM_B(0, (gpr)(int32_t)(text + i)) = (int8_t)saved_text[i];
        if (ctx->r2 == 1) {
            mark_saved();
            fprintf(stderr, "[checkpoint] saved at quit\n");
        }
        else {
            discard();
        }
    }

    // VS screen (main func_8024C264, before 0x8024C8E4, after the field and stadium lines): a saved
    // game between these teams says it will be resumed. Font, group (0x59, cleared with the screen)
    // and depth are still set from the stadium line.
    void blitz_checkpoint_vs(uint8_t* rdram, recomp_context* ctx) {
        if (!enabled()) return;
        Checkpoint cp;
        if (!read_checkpoint(cp) || !matches(rdram, cp)) return;
        char msg[64];
        static const char* q[] = {"1ST QTR", "2ND QTR", "3RD QTR", "4TH QTR", "OVERTIME", "OVERTIME", "OVERTIME"};
        snprintf(msg, sizeof(msg), "\x1b%%dj\x1b%%dcRESUMING  %d-%d  %s", cp.score(0), cp.score(1), q[cp.quarter() >= 0 && cp.quarter() <= 6 ? cp.quarter() : 0]);
        constexpr uint32_t str = 0x80404100;  // free RDRAM next to the opponent label (0x80404000)
        for (size_t i = 0; i <= strlen(msg); i++) MEM_B(0, (gpr)(int32_t)(str + i)) = (int8_t)msg[i];
        gpr regs[5] = {ctx->r2, ctx->r4, ctx->r5, ctx->r6, ctx->r7};
        ctx->r4 = 256;
        ctx->r5 = rd(rdram, game_mode) == 2 ? 96 : 120;  // under the helmets; clear of Season's week line
        ctx->r6 = 0x3FC00000;
        func_80279670(rdram, ctx);
        ctx->r4 = (int32_t)str;
        ctx->r5 = 0xA;                   // centred
        ctx->r6 = (int32_t)0xFFFFFF00;   // yellow
        func_8027A374(rdram, ctx);
        ctx->r2 = regs[0]; ctx->r4 = regs[1]; ctx->r5 = regs[2]; ctx->r6 = regs[3]; ctx->r7 = regs[4];
    }
}

namespace blitz {
    // UI thread, once the game runs: an autosave left by a crash asks before it's kept.
    void checkpoint_startup_prompt() {
        static bool asked = false;
        if (asked || !ultramodern::is_game_started()) return;
        asked = true;
        if (!enabled()) return;
        Checkpoint cp;
        if (!read_checkpoint(cp) || cp.saved) return;
        recompui::open_choice_prompt(
            "Recover last game?",
            "The app closed in the middle of this game. Recover it to pick up from the last play: "
            "start an Arcade game with the same team. (Experimental)",
            "Recover", "Discard", [] { mark_saved(); }, [] { discard(); },
            recompui::ButtonStyle::Success, recompui::ButtonStyle::Danger, false);
        recompui::set_prompt_scoreline(describe(cp));
    }

    // The app's quit prompt (RecompFrontend patch 0004): while a game is being played, ask whether to
    // keep it. True when this prompt was shown instead of the usual one.
    bool checkpoint_quit_prompt() {
        if (!enabled() || !in_progress) return false;
        Checkpoint cp;
        bool known = read_checkpoint(cp);
        recompui::open_choice_prompt(
            "Save this game?",
            "Quit and keep this game to pick up later: start an Arcade game with the same team. "
            "To quit without saving, end the game from its pause menu first. (Experimental)",
            "Save & Quit", "Cancel",
            [] { mark_saved(); ultramodern::quit(); }, [] {},
            recompui::ButtonStyle::Success, recompui::ButtonStyle::Tertiary, true);
        recompui::set_prompt_scoreline(known ? describe(cp) : std::string());
        return true;
    }
}
