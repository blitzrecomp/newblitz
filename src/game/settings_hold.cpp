// The game held under Settings (1.0.5; Settings → General → Freeze Game in Settings). Before, the game ran on under the
// menu with no input: a play went on with the CPU, play select's clock ran down. Now opening Settings with the game
// started holds it at its dispatcher, the same place and the same way as the app going to the background
// (replay.cpp background_hold) and Instant Replay: every process asleep, the VI's messages taken one by one. RT64 presents
// every VI meanwhile (blitz_replay_keep_presenting), so the menu and the held picture keep drawing.
//
// Live redraw: a picture or two-screen change while Settings is up (a Graphics row's Apply, a Two Screens dial) is shown
// on the held moment, as the dev kit's HOLD does (dev_input.cpp): the first change saves that moment as the mark
// "settings" (in memory only, savestate.cpp memory_mark), and each change loads it, lets the game run a few frames with
// the new settings and holds again. A mark is a save state of the game's memory: like any load it goes through
// restore_snapshot, so the Controller Pak is never written and the game's view of it is brought up to date
// (pak_state_restored), and Redo Play's plays stay. A mark that can't be saved or loaded within a moment (a Season
// screen, a load still waiting for the game's processes) is dropped, and the game simply stays held: no redraw, never a
// late load in the middle of play.
//
// Let go: Settings closing, the setting off, the app going to the background (its Auto slot needs the game), any save or
// load asked for (Save & Quit, Main Menu: the game runs until it's done, then holds again if Settings is still up), and
// the UI thread not looking for 0.75 s (it's stuck or quitting). Potato devices with little memory have no live redraw
// (the mark is 8 MB), and the hold is off there unless turned on.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "SDL2/SDL.h"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "ultramodern/config.hpp"
#include "ultramodern/ultramodern.hpp"
#include "blitz_soft_reset.h"

extern "C" int blitz_savestate_busy(void);                       // savestate.cpp
extern "C" void blitz_savestate_mark(int op, const char* name);
extern "C" int blitz_savestate_mark_status(const char* name);
extern "C" void blitz_savestate_mark_cancel(const char* name);
extern "C" void blitz_savestate_quiet(int on);
extern "C" int blitz_app_in_background(void);
extern "C" { extern volatile int blitz_replay_keep_presenting; }  // lib/rt64 rt64_blitz_replay.cpp

namespace blitz {
    bool replay_is_open();  // replay_screen.cpp
    uint64_t game_frames();  // fps_counter.cpp: the game's swaps this launch
}

namespace {
    using Clock = std::chrono::steady_clock;
    int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

    constexpr const char* mark = "settings";
    // Dispatcher passes a redraw runs (about 60 a second): the frames the dev kit's HOLD runs (6 of the game's), and some.
    int redraw_passes() {
        static const int n = [] { const char* v = getenv("BLITZ_SETTINGS_HOLD_PASSES"); const int i = v ? atoi(v) : 0; return i > 0 ? i : 14; }();
        return n;
    }
    constexpr int64_t ui_gone_ms = 750;      // the UI thread not looking this long: let the game go
    constexpr int64_t mark_wait_ms = 800;    // a mark's save or load not done by then: dropped (the game runs meanwhile)

    std::atomic<bool> want{false};           // UI: hold the game
    std::atomic<int> run_passes{0};          // dispatcher passes to let run before holding again
    std::atomic<bool> holding{false};        // the dispatcher is held now
    std::atomic<int64_t> ui_seen_ms{0};      // the UI tick's last look
    std::atomic<int64_t> let_go_ms{0};       // when a hold last ended

    bool ui_gone() { return now_ms() - ui_seen_ms.load() > ui_gone_ms; }

    // Restart in place (soft_restart.cpp): the held dispatcher went with the game's threads.
    // Restart is pressed in Settings, so the game is usually held then, and the dispatcher that would let go (settings_hold_held)
    // is gone: what the hold turned on goes off here. RT64 left presenting every VI as well as the game's frames made the Surface
    // Duo's two screens stutter after one restart (build 960: frames over 1/2/3/4+ VIs instead of 2).
    [[maybe_unused]] const bool hold_soft_reset = blitz::on_soft_reset("settings hold", [] {
        if (holding.load() && !blitz::replay_is_open()) blitz_replay_keep_presenting = 0;
        holding = false;
        run_passes = 0;
    });

    // UI thread only.
    enum class Mark { none, saving, ready, loading, failed };
    Mark mark_state = Mark::none;
    int64_t mark_asked_ms = 0;
    bool redraw_pending = false;
    bool have_graphics = false;
    ultramodern::renderer::GraphicsConfig last_graphics{};
}

namespace blitz {
    // Settings → General → Freeze Game in Settings (config.cpp keeps it current).
    std::atomic<bool> settings_freeze_on{true};

    // The live redraw needs a mark (8 MB): not on a potato with little memory.
    bool settings_redraw_possible() {
        static const bool ok = !(getenv("BLITZ_LEAN") != nullptr && SDL_GetSystemRAM() < 800);
        return ok;
    }

    // A change Settings can show on the held moment (Two Screens' dials, the screens' modes; Graphics is seen below).
    void settings_hold_redraw() { redraw_pending = true; }

    // Dispatcher, every pass (replay.cpp blitz_replay_gate): whether to hold here now.
    bool settings_hold_pass() {
        if (!want.load()) return false;
        if (blitz_savestate_busy()) return false;  // a save or load first: it's done in this pass's tick
        if (run_passes.load() > 0) {
            run_passes--;
            return false;
        }
        return !ui_gone() && !blitz_app_in_background();
    }
    // Held, each VI: still?
    bool settings_hold_keep() {
        return want.load() && !blitz_savestate_busy() && run_passes.load() == 0 && !ui_gone() && !blitz_app_in_background();
    }
    void settings_hold_held(bool on) {
        static int64_t since = 0;
        if (on) {
            holding = true;
            blitz_replay_keep_presenting = 1;  // nothing presents with the game stopped: the menu would freeze too
            since = now_ms();
        }
        else {
            if (!replay_is_open()) blitz_replay_keep_presenting = 0;
            holding = false;
            let_go_ms = now_ms();
            fprintf(stderr, "[settings] hold: the game ran again after %lld ms held%s\n", (long long)(now_ms() - since),
                    want.load() ? (blitz_savestate_busy() ? " (a save or load)" : run_passes.load() > 0 ? " (a redraw)" : " (the UI or the background)") : "");
        }
    }

    // Main thread, every UI tick (main.cpp update_gfx).
    void settings_hold_update() {
        ui_seen_ms = now_ms();
        bool open = false;
        try {
            open = recompui::is_context_shown(recompui::config::get_config_context_id());
        }
        catch (const std::exception&) {
        }
        // A game that has drawn: not one still booting under a Settings opened from the launcher or setup.
        const bool eligible = settings_freeze_on.load() && ultramodern::is_game_started() && game_frames() > 0 && !replay_is_open() &&
                              !blitz_app_in_background();
        // Graphics: what the renderer uses now (Apply, Fullscreen, TV Mode), compared each tick.
        {
            const ultramodern::renderer::GraphicsConfig& g = ultramodern::renderer::get_graphics_config();
            if (have_graphics && !(g == last_graphics) && want.load()) redraw_pending = true;
            last_graphics = g;
            have_graphics = true;
        }
        if (open && eligible && !want.load()) {
            want = true;
            run_passes = 0;
            mark_state = Mark::none;
            redraw_pending = false;
            blitz_savestate_quiet(1);  // the mark's own notices stay off the screen
            fprintf(stderr, "[settings] hold: Settings up, the game holds%s\n", settings_redraw_possible() ? "" : " (no live redraw here)");
            return;
        }
        if ((!open || !eligible) && want.load()) {
            // A mark's save or load not done yet never lands after this: in the middle of play it would go back in time.
            if (mark_state == Mark::saving || mark_state == Mark::loading) blitz_savestate_mark_cancel(mark);
            want = false;
            run_passes = 0;
            mark_state = Mark::none;
            redraw_pending = false;
            blitz_savestate_quiet(0);
            fprintf(stderr, "[settings] hold: %s, the game carries on\n", !open ? "Settings closed" : "no longer wanted");
            return;
        }
        if (!want.load()) return;
        // Live redraw.
        const int64_t t = now_ms();
        switch (mark_state) {
            case Mark::none:
                if (!redraw_pending) break;
                if (!settings_redraw_possible()) {
                    redraw_pending = false;
                    break;
                }
                if (blitz_savestate_busy()) break;  // someone else's first
                blitz_savestate_mark(1, mark);
                mark_state = Mark::saving;
                mark_asked_ms = t;
                break;
            case Mark::saving: {
                const int s = blitz_savestate_mark_status(mark);
                if (s == 1) {
                    mark_state = Mark::ready;
                    fprintf(stderr, "[settings] hold: this moment kept for redraws (%lld ms)\n", (long long)(t - mark_asked_ms));
                }
                else if (s < 0 || t - mark_asked_ms > mark_wait_ms) {
                    blitz_savestate_mark_cancel(mark);
                    mark_state = Mark::failed;
                    fprintf(stderr, "[settings] hold: this moment can't be kept (%s): changes show once Settings closes\n", s < 0 ? "refused" : "too slow");
                }
                break;
            }
            case Mark::ready:
                // One redraw at a time, each from the held moment: a slider's run of changes is one more redraw.
                if (!redraw_pending || !holding.load() || blitz_savestate_busy()) break;
                redraw_pending = false;
                run_passes = redraw_passes();
                blitz_savestate_mark(2, mark);
                mark_state = Mark::loading;
                mark_asked_ms = t;
                break;
            case Mark::loading: {
                const int s = blitz_savestate_mark_status(mark);
                if (s == 2) mark_state = Mark::ready;
                else if (s < 0 || t - mark_asked_ms > mark_wait_ms) {
                    blitz_savestate_mark_cancel(mark);
                    run_passes = 0;
                    mark_state = Mark::failed;
                    fprintf(stderr, "[settings] hold: the moment didn't load (%s): no more redraws this time\n", s < 0 ? "refused" : "too slow");
                }
                break;
            }
            case Mark::failed:
                redraw_pending = false;
                break;
        }
    }
}

// savestate.cpp: the freeze rescue doesn't count a held game as stuck, nor the frame-rate guards a slow second.
extern "C" int blitz_settings_holding(void) { return holding.load() || want.load() ? 1 : 0; }
extern "C" int blitz_settings_hold_settling(void) {
    const int64_t t = let_go_ms.load();
    return want.load() || (t != 0 && now_ms() - t < 3000) ? 1 : 0;
}
