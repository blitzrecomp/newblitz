// Instant Replay, the game's side: stopping the game, and the REPLAY item in its pause menu. The screen itself is
// src/main/replay_screen.cpp; the frames are kept by RT64 (docs/REPLAY.md).
//
// Stopping: the process dispatcher's loop head (0x80273D7C, the hook blitz_savestate_tick shares) is the one place every
// frame passes with all the game's processes stopped in process_sleep. There it stays, taking the same retrace messages
// from the same queue it would anyway, in a real osRecvMesg so the cooperative scheduler keeps running the other game
// threads and external messages keep being delivered (threads.cpp: they're only delivered when a game thread asks). No
// process wakes, so game time stands still, and nothing is drawn. Nothing is torn down or restored, as a save state would,
// so coming back is just leaving the loop: every register and every byte of RDRAM is where it was. The audio thread keeps
// running (the host output is muted, blitz::audio::set_mute): stopping it would need quiescence the game doesn't offer
// mid-sound, and it can't change anything but where the music has got to. Save states wait: their tick is after this.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#include "blitz_replay.h"

namespace blitz {
    std::atomic_bool replay_gate_request{false};
    std::atomic_bool replay_gated{false};
    std::atomic_int replay_phase{0};
    std::atomic_bool replay_pause_menu{false};
    std::atomic_bool replay_input_held{false};
    std::atomic_bool replay_play_live{false};
    std::atomic_uint replay_snaps{0};
    // The app in the background (a clamshell closed, an app left on a phone; savestate.cpp sets it once the Auto slot is
    // saved): the game holds at the dispatcher, as for a replay, until it's back. It played on with the lid shut before.
    std::atomic_bool background_hold{false};
    bool settings_hold_pass();          // settings_hold.cpp: the game held under Settings
    bool settings_hold_keep();
    void settings_hold_held(bool on);
}

namespace {
    constexpr int32_t phase_addr = (int32_t)0x802EA420;       // 2: a game (savestate.cpp)
    constexpr int32_t pause_up_addr = (int32_t)0x8028BFC0;    // func_80262D90 sets it on the way in, clears it on the way out
    // The play's phase (receiver_targets.cpp): 1 at the line (a kickoff's run-up too), 2 live, 5 a kickoff in the air, 3 after
    // the whistle, 6 the coin toss.
    constexpr int32_t play_phase_addr = (int32_t)0x802EA4A8;
    constexpr int32_t retrace_queue = (int32_t)0x8005DF20;    // what the dispatcher's own osRecvMesg reads
}

// The dispatcher's loop head, every time round (blitz2000.toml).
extern "C" void blitz_replay_gate(uint8_t* rdram) {
    blitz::replay_phase = MEM_W(0, (gpr)phase_addr);
    blitz::replay_pause_menu = MEM_W(0, (gpr)pause_up_addr) != 0;
    // The play's phase, and each time it goes live: the snap, or a kickoff's kick, where the replay's next play starts. Read
    // on every frame here, so the main thread's 50 ms look (replay_screen.cpp) can't miss one. Outside a game it's 0.
    {
        static int last = -1;
        static bool was_live = false;
        static const bool trace = getenv("BLITZ_REPLAY_TRACE") != nullptr;
        const int play = blitz::replay_phase.load() == 2 ? (int)MEM_W(0, (gpr)play_phase_addr) : 0;
        const bool live = play == 2 || play == 5;
        if (live && !was_live) blitz::replay_snaps++;
        static int last_game = -1;
        if (trace && (play != last || blitz::replay_phase.load() != last_game)) {
            static const auto t0 = std::chrono::steady_clock::now();
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            fprintf(stderr, "[replay] play phase %d -> %d (%lld ms, kind %d, game phase %d)\n", last, play, ms, (int)MEM_W(0, (gpr)(int32_t)0x802EA508), blitz::replay_phase.load());
        }
        last_game = blitz::replay_phase.load();
        last = play;
        was_live = live;
        blitz::replay_play_live = live;
    }
    // Settings up: the game holds under it (settings_hold.cpp), let go for a save or load, a redraw's few frames and close.
    if (blitz::settings_hold_pass()) {
        blitz::settings_hold_held(true);
        while (blitz::settings_hold_keep()) {
            osRecvMesg(rdram, retrace_queue, NULLPTR, OS_MESG_BLOCK);
        }
        blitz::settings_hold_held(false);
    }
    if (blitz::background_hold.load()) {
        fprintf(stderr, "[background] the game holds\n");
        while (blitz::background_hold.load()) {
            osRecvMesg(rdram, retrace_queue, NULLPTR, OS_MESG_BLOCK);
        }
        fprintf(stderr, "[background] the game carries on\n");
    }
    if (!blitz::replay_gate_request.load()) return;
    fprintf(stderr, "[replay] game stopped\n");
    blitz::replay_gated = true;
    while (blitz::replay_gate_request.load()) {
        osRecvMesg(rdram, retrace_queue, NULLPTR, OS_MESG_BLOCK);
    }
    blitz::replay_gated = false;
    fprintf(stderr, "[replay] game running\n");
}

// The controller read (dev_input.cpp get_input): while the replay's up the game gets nothing, and a button still held
// when it closes stays away until it's let go. Otherwise closing with ✕ would press RESUME GAME.
extern "C" void blitz_replay_input(int port, uint16_t* buttons, float* x, float* y) {
    static std::atomic_bool wait_release[4];
    if (port < 0 || port > 3) return;
    if (blitz::replay_input_held.load()) {
        wait_release[port] = true;
    }
    else if (*buttons == 0) {
        wait_release[port] = false;
    }
    if (!wait_release[port].load()) return;
    *buttons = 0;
    *x = 0.0f;
    *y = 0.0f;
}

// The pause menu (main func_80262D90, the same in vanilla and team+). It has three rows, in a table of {x, y, z} floats
// at 0x8028BF90, then their three labels at 0x8028BFB4, then other things. Four rows run the table into where the labels
// were, which only this function reads, so the labels come from here instead. The row numbers are the code's: 0 RESUME
// GAME, 1 OPTIONS, 3 QUIT GAME (patched in the toml, was 2), and the 2 the code now ignores is REPLAY.
//   RESUME GAME 229, OPTIONS 179, REPLAY 129, QUIT GAME 79.
extern "C" {
    void func_8027A808(uint8_t* rdram, recomp_context* ctx);  // a text's colour

    // Before the rows are made.
    void blitz_replay_pause_setup(uint8_t* rdram) {
        constexpr int32_t row3 = (int32_t)0x8028BFB4;
        MEM_W(0, (gpr)row3) = 0x42E60000;      // 115.0
        MEM_W(4, (gpr)row3) = 0x429E0000;      // 79.0
        MEM_W(8, (gpr)row3) = 0x42A00000;      // 80.0
    }

    namespace {
        constexpr int32_t text_resume = (int32_t)0x802AC4E0, text_options = (int32_t)0x802AC4D8, text_quit = (int32_t)0x802AC4CC;
        constexpr int text_quit_length = 10;   // with its zero: "REPLAY" fits
        char quit_saved[text_quit_length];
        bool quit_swapped = false;
    }

    // Before func_8027A374 makes a row's text; s6 is the row's number + 1. There's no room for a new string in game
    // memory, so for REPLAY's call QUIT GAME's bytes hold it (the checkpoint's "SAVE THIS GAME?" does the same); the text
    // is built during the call.
    void blitz_replay_pause_label(uint8_t* rdram, recomp_context* ctx) {
        int row = (int)ctx->r22 - 1;
        ctx->r7 = (gpr)(row == 0 ? text_resume : row == 1 ? text_options : text_quit);
        if (row != 2) return;
        static const char replay[text_quit_length] = "REPLAY";
        for (int i = 0; i < text_quit_length; i++) {
            quit_saved[i] = (char)MEM_B(i, (gpr)text_quit);
            MEM_B(i, (gpr)text_quit) = (int8_t)replay[i];
        }
        quit_swapped = true;
    }

    // ... and after it.
    void blitz_replay_pause_label_done(uint8_t* rdram) {
        if (!quit_swapped) return;
        quit_swapped = false;
        for (int i = 0; i < text_quit_length; i++) MEM_B(i, (gpr)text_quit) = (int8_t)quit_saved[i];
    }

    // The cursor moved: the code greys its three labels (sp+0x20, +0x24, +0x28) and lights the current one. The fourth.
    void blitz_replay_pause_grey(uint8_t* rdram, recomp_context* ctx) {
        ctx->r4 = (gpr)MEM_W(0x2C, ctx->r29);
        ctx->r5 = (gpr)(int32_t)0xFFA3A300;
        func_8027A808(rdram, ctx);
    }

    // A press on row 2: what used to be nowhere the code goes. The menu stays as it is under the replay.
    void blitz_replay_pause_item(void) {
        blitz::replay_open();
    }
}
