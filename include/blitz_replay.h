#pragma once
// Instant Replay (docs/REPLAY.md). The ring of frames is RT64's (rt64_blitz_replay.cpp); this is the way in from the rest
// of the app. The screen is src/main/replay_screen.cpp, stopping the game is src/game/replay.cpp.
#include <atomic>
#include <cstdint>

namespace blitz {
    void replay_open();     // any thread: takes over the screen at the next UI tick, if there's something to show
    void replay_close();    // any thread: back to the game
    void replay_save();     // any thread: the play on screen goes to <app folder>/replays as a GIF
    bool replay_is_open();  // any thread: the screen's up, or on its way

    // src/game/replay.cpp, for replay_screen.cpp.
    extern std::atomic_bool replay_gate_request;  // stop the game at its next frame
    extern std::atomic_bool replay_gated;         // ... and it has
    extern std::atomic_int replay_phase;          // the game's phase (2: a game), as of its last frame
    extern std::atomic_bool replay_pause_menu;    // the game's own pause menu is up
    extern std::atomic_bool replay_input_held;    // the game gets no buttons (until they've been let go)
}
