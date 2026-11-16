// BLITZ DS (docs/BLITZ_DS.md): the game on two screens. One build for every Android device: a second screen is found at
// startup (BlitzActivity.java) and used if Settings allows it; BLITZ_DS=1 on a Mac opens a second window to stand in.
//   - During play the tall picture (tall_view.cpp) spans both, and nothing is hidden by the hinge: each screen draws its
//     share (RT64: platform/rt64-patches/0010, drawn in turn on its present thread).
//   - Menus and play select stay whole on the lower screen; the upper one shows the drive and the score.
//   - Receiver icons sit on whichever screen their receiver is on, and taps work on both.
// The second screen's content is RML the UI tick writes here (recompui::ds_panel_set), only when it changes.
// Logging: [ds] lines for every change of state and a line of timings every 5 s, in blitz.log.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <sys/sysinfo.h>
#endif
#include "blitz_platform.h"
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"
#if defined(BLITZ_PLATFORM_LINUX)
// SDL_syswm.h brings in Xlib on Linux; its macros collide with names in the runtime (as in main.cpp).
#undef None
#undef Bool
#undef Status
#undef Always
#undef Success
#undef True
#undef False
#undef LockMask
#undef KeyPress
#undef KeyRelease
#undef FocusIn
#undef FocusOut
#endif
#include "recomp.h"
#include "librecomp/game.hpp"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "ultramodern/ultramodern.hpp"

extern "C" {
    extern volatile int blitz_ds_mode;
    extern volatile int blitz_edge_trim_px;       // rt64_vi_renderer.cpp
    extern volatile int blitz_ds_clip_right;      // rt64_present_queue.cpp: the last play's clip at the second screen's right
    extern volatile float blitz_ds_split;
    extern volatile int blitz_ds_second_on_top;
    extern volatile float blitz_ds_second_ratio;  // rt64_workload_queue.cpp
    extern volatile int blitz_ds_panel_first;     // ui_state.cpp
    extern volatile int blitz_ds_second_icons;    // rt64_present_queue.cpp
    extern volatile int blitz_ds_screen_mode;     // tall_view.cpp: 0 both screens, 1 game + companion, 2 stylus
    extern volatile uint64_t blitz_ds_second_frames;
    extern volatile uint64_t blitz_ds_second_failures;
    extern volatile float blitz_ds_second_ms;
    extern volatile int blitz_ds_clip_enabled;
    extern volatile int blitz_ds_clip_restart;
    extern volatile int blitz_ds_clip_show;
    extern volatile int blitz_ds_clip_frames;
    extern volatile float blitz_tall_ratio;
    extern volatile float blitz_tall_aspect_on;
    void blitz_ds_set_window(void* a, void* b);
    void blitz_ds_read_state(uint8_t* rdram, void* out);  // play_log.cpp
    int blitz_play_select_on_screen(void);
}

extern "C" void blitz_rt64_render_to_ram(int mode);  // rt64_raster_shader_cache.cpp
extern "C" volatile int blitz_field_up;  // tall_view.cpp
namespace blitz { int potato_frame_copy(); void host_hide_splash(); void host_first_frame(); uint64_t first_presents(); }  // config.cpp, platform_android.cpp, fps_counter.cpp
namespace blitz::firstrun_ui { bool shown(); }  // firstrun_screen.cpp
#ifdef __ANDROID__
namespace blitz { double potato_scale(); bool potato_narrow_waits(); }  // config.cpp
#endif

namespace blitz {
    uint8_t* monitor_rdram();
    bool original_game();
    std::optional<std::vector<uint8_t>> rom_file_at(int32_t index);  // art_dropin.cpp
}

namespace {
    std::atomic<bool> have_second{false};
    std::atomic<int> second_w{0}, second_h{0}, first_w{0}, first_h{0};
    std::atomic<bool> swapped{false};  // the first window is the upper screen (Android: BlitzLauncher.firstIsUpper; a Mac test: Swap Screens)
    std::atomic<bool> shapes_changed{false};

    float shape(int w, int h) { return w > 0 ? (float)h / (float)w : 0.75f; }

    // The picture's shape and the upper screen's share of it follow the two screens' own shapes.
    void apply_shapes() {
        float a = shape(first_w, first_h), b = shape(second_w, second_h);
        blitz_tall_ratio = 1.0f / (a + b);
        blitz_ds_second_on_top = swapped.load() ? 0 : 1;
        if (second_h.load() > 0) blitz_ds_second_ratio = (float)second_w.load() / (float)second_h.load();
        blitz_ds_split = (swapped.load() ? a : b) / (a + b);
    }

    struct Badge {
        bool on = false;
        int screen = 0;      // 0 the first window, 1 the second
        float x = 0, y = 0;  // on that screen: 0-1
        std::string glyph;
    };
    std::array<Badge, 3> badges;
    std::mutex badges_mutex;
}

// The game's picture is on two screens right now.
extern "C" int blitz_ds_active(void) { return have_second.load() ? 1 : 0; }

// The UI's window (the first) is this share of the picture, from this height (0-1), during play.
extern "C" int blitz_ds_first_part(float* top, float* bottom) {
    if (!have_second.load()) return 0;
    float split = blitz_ds_split;
    *top = blitz_ds_second_on_top ? split : 0.0f;
    *bottom = blitz_ds_second_on_top ? 1.0f : split;
    return 1;
}

// The vertical view multiplier the two screens call for, against the game's 4:3.
extern "C" float blitz_ds_k(void) { return (4.0f / 3.0f) / blitz_tall_ratio; }

// A receiver's icon: where it is, on either screen, for taps; the second screen's are also drawn here. k = 0-2; on false
// clears it.
extern "C" void blitz_ds_badge(int k, int on, int screen, float x, float y, const char* glyph) {
    std::lock_guard lock{badges_mutex};
    if (k < 0 || k >= (int)badges.size()) return;
    badges[(size_t)k].on = on != 0;
    badges[(size_t)k].screen = screen;
    badges[(size_t)k].x = x;
    badges[(size_t)k].y = y;
    if (glyph != nullptr) badges[(size_t)k].glyph = glyph;
}

// Stylus in play: the picture's on the second window and nothing of it on the first (tall_view.cpp), so a receiver's icon goes
// over there. 1 and that window's size, or 0.
extern "C" int blitz_ds_icons_on_second(int* w, int* h) {
    if (!have_second.load() || blitz_ds_screen_mode != 2 || blitz_ds_mode != 3 || blitz_ds_panel_first) return 0;
    *w = second_w.load();
    *h = second_h.load();
    return *w > 0 && *h > 0;
}

// ---- The drive -------------------------------------------------------------------------------------------------------

namespace {
    // Every finished play of the current drive, and how the last drive ended. Fed once per play by play_log.cpp; the whole
    // game goes to blitz.log as [drive] lines too. The score and the next snap are read live (blitz_ds_read_state), so
    // they're right at the game's first play select and after a save state.
    struct PlayRow {
        std::string before;  // "2nd & 7", as it was at the snap
        std::string word;    // "pass", "run", "incomplete", ...
        std::string gain;    // "+12", "-3", "0", or empty when yards don't apply
        const char* ink;     // the gain's colour
    };
    struct Drive {
        std::string team, result;
        int plays = 0, yards = 0, seconds = 0;
    };
    std::mutex drive_mutex;
    Drive drive, last_drive;
    int drive_side = -1, drive_start = 0, drive_clock_start = 0, last_clock = -1, last_quarter = -1;
    std::vector<PlayRow> rows;
    std::string snap_situation;  // the next snap, as play select showed it
    std::atomic<int> drive_scroll{0};  // plays back from the newest, on the second screen's list

    std::string escape_rml(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '<') o += "&lt;";
            else if (c == '>') o += "&gt;";
            else if (c == '&') o += "&amp;";
            else o += (char)toupper((unsigned char)c);
        }
        return o;
    }

    void end_drive(const std::string& how) {
        if (drive.team.empty() || drive.plays == 0) return;
        drive.result = how;
        fprintf(stderr, "[drive] %s: %d plays, %d yds, %d:%02d, %s\n", drive.team.c_str(), drive.plays, drive.yards, drive.seconds / 60, drive.seconds % 60, how.c_str());
        last_drive = drive;
    }

    // plays.log's words, for a person.
    std::string shown_result(const std::string& result) {
        return result.rfind("score or special", 0) == 0 ? "Score" : result;
    }

    // "2nd & 7 at own 35": the first part.
    std::string down_part(const std::string& situation) {
        size_t at = situation.find(" at ");
        return at == std::string::npos ? situation : situation.substr(0, at);
    }
    std::string where_part(const std::string& situation) {
        size_t at = situation.find(" at ");
        return at == std::string::npos ? std::string() : situation.substr(at + 4);
    }

    PlayRow make_row(const std::string& before, const std::string& result, bool has_gain, int gain, int kind) {
        PlayRow row{down_part(before), shown_result(result), "", "#b8c4da"};
        if (result.find("pass") != std::string::npos) row.word = "Pass";
        else if (result.find("run") != std::string::npos) row.word = "Run";
        else if (result == "no gain") row.word = "No gain";
        if (kind == 4) {
            row.word = "Touchdown";
            row.gain = "TD";
            row.ink = "#f6d400";
        }
        else if (has_gain && gain != 0) {
            row.gain = (gain > 0 ? "+" : "") + std::to_string(gain);
            row.ink = gain > 0 ? "#45e04a" : "#ff6a3d";
        }
        else if (has_gain) row.gain = "0";
        return row;
    }

    std::string how_it_ended(const std::string& result, int kind, bool turnover) {
        if (turnover) return "Change of possession";  // a punt and downs look the same here as a pick
        if (kind == 4) return "Touchdown";            // the next snap is an extra point
        if (kind == 1) return "Score";                // the next snap is a kickoff
        return shown_result(result);
    }

    // The board after each play, so a rewind (Redo Play, a save state) takes off only what it undid.
    struct BoardStep {
        int at;  // game_seconds
        std::vector<PlayRow> rows;
        Drive drive, last_drive;
        int drive_side, drive_start, drive_clock_start;
        std::string snap_situation;
    };
    std::vector<BoardStep> board_steps;

    void forget_drives(const char* why) {
        fprintf(stderr, "[drive] %s: starting over\n", why);
        rows.clear();
        drive = Drive{};
        last_drive = Drive{};
        drive_side = -1;
        snap_situation.clear();
        board_steps.clear();
        last_quarter = -1;
        last_clock = -1;
        blitz_ds_clip_restart = 1;
    }

    int game_seconds(int quarter, int clock_seconds) { return quarter * 120 + (120 - clock_seconds); }  // 2:00 quarters

    void rewind_drives(int quarter, int clock_seconds) {
        const int now = game_seconds(quarter, clock_seconds);
        while (!board_steps.empty() && board_steps.back().at > now) board_steps.pop_back();
        if (board_steps.empty()) {  // a new game, or further back than this remembers
            forget_drives("the clock went back");
            return;
        }
        const BoardStep& b = board_steps.back();
        rows = b.rows;
        drive = b.drive;
        last_drive = b.last_drive;
        drive_side = b.drive_side;
        drive_start = b.drive_start;
        drive_clock_start = b.drive_clock_start;
        snap_situation = b.snap_situation;
        blitz_ds_clip_restart = 1;
        fprintf(stderr, "[drive] rewound: %d plays on the board\n", (int)rows.size());
    }

    // Is this moment earlier than the last we saw? A new game, a save state or Redo Play.
    // Not a quarter's end: the clock runs out and is set again a moment before the quarter's number changes.
    bool time_went_back(int quarter, int clock_seconds) {
        if (last_quarter < 0) return false;
        if (quarter < last_quarter) return true;
        return quarter == last_quarter && clock_seconds > last_clock && last_clock != 0;
    }
}

// play_log.cpp, once per play: the state describes the next snap. result: what just happened.
extern "C" void blitz_ds_play(const char* result, const char* situation, int offense_side, const char* team, int spot, int kind, int quarter, int clock_seconds, int turnover, int has_gain, int gain) {
    std::lock_guard lock{drive_mutex};
    if (time_went_back(quarter, clock_seconds)) rewind_drives(quarter, clock_seconds);
    last_quarter = quarter;
    last_clock = clock_seconds;

    bool same_drive = offense_side == drive_side && !turnover && kind == 0;
    if (drive_side >= 0) {
        rows.push_back(make_row(snap_situation, result, has_gain != 0 && !turnover, gain, kind));
        if (rows.size() > 40) rows.erase(rows.begin());
        drive.plays++;
        drive.seconds = std::max(0, game_seconds(quarter, clock_seconds) - drive_clock_start);
        if (same_drive) drive.yards = spot - drive_start;
        else if (kind == 4) drive.yards = 100 - drive_start;  // the rest of the way
        fprintf(stderr, "[drive] play %d: %s -> %s\n", drive.plays, snap_situation.c_str(), result);
    }
    if (!same_drive) {
        end_drive(how_it_ended(result, kind, turnover));
        drive = Drive{};
        drive.team = team;
        drive_side = kind == 0 ? offense_side : -1;  // a kickoff or extra point isn't a drive's first snap
        drive_start = spot;
        drive_clock_start = game_seconds(quarter, clock_seconds);
        rows.clear();
    }
    snap_situation = situation;
    drive_scroll = 0;
    board_steps.push_back({game_seconds(quarter, clock_seconds), rows, drive, last_drive, drive_side, drive_start, drive_clock_start, snap_situation});
    if (board_steps.size() > 64) board_steps.erase(board_steps.begin());
}

// ---- Taps ------------------------------------------------------------------------------------------------------------

namespace blitz {
    uint8_t* monitor_rdram();    // dev_monitor.cpp
    float fps_last_second();     // fps_counter.cpp: the game's latest frame rate, 0 if not known
}
extern "C" int blitz_play_mode(void);     // savestate.cpp: 1 play select, 2 a play
extern "C" int blitz_port_side(int port);
extern "C" int blitz_touch_port(void);  // savestate.cpp: the player a touch plays for
extern "C" int blitz_replay_count(void);  // rt64_blitz_replay.cpp

namespace {
    constexpr uint16_t btn_a = 0x8000, btn_b = 0x4000, btn_z = 0x2000, btn_up = 0x0800, btn_down = 0x0400, btn_left = 0x0200, btn_right = 0x0100;
    constexpr uint16_t receiver_buttons[3] = {0x0002, 0x0004, 0x0001};  // C-left, C-down, C-right
    constexpr int tile_most_steps = 8;  // the farthest tile is 4 moves away: past this the game isn't moving (a wrong address)

    std::atomic<int> tile_goal{-1};           // walking the cursor there, then pressing A
    std::atomic<int> tile_wait{0};            // reads to hold off between presses
    std::atomic<int> tile_steps{0};
    std::atomic<int> tile_last_at{-1};
    std::atomic<int> tile_stuck{0};
    std::atomic<uint32_t> injected{0};        // buttons to press for the next few reads
    std::atomic<int> injected_reads{0};

    void press(uint16_t buttons, int reads = 4) {
        injected = buttons;
        injected_reads = reads;
    }
}
namespace blitz {
    void ds_press(uint16_t buttons, int reads) { press(buttons, reads); }  // savestate.cpp: Start as the app goes away
}
namespace {

    // Play select's cursor, from its brackets as they're drawn (tall_view.cpp). In the game's 2D space: tiles at x 6, 89,
    // 172 and 263, 346, 429, 83 wide; y 294, 198, 102 at their tops, 96 tall; a bracket sits 3 left of its tile.
    using Stamp = std::chrono::steady_clock;
    struct Bracket {
        std::atomic<float> x{0}, y{0};
        std::atomic<int64_t> when{0};
    };
    Bracket brackets[2];  // the grid's left and right halves
    std::atomic<int64_t> picking_since{0};  // when this play select came up

    int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Stamp::now().time_since_epoch()).count(); }

    // The tile a side's cursor is on, 0-8 across then down, or -1 with no bracket up: the help page, or already picked.
    int read_cursor(int side) {
        const Bracket& b = brackets[side];
        if (now_ms() - b.when.load() > 250) return -1;
        int col = (int)((b.x.load() + 3.0f - (side == 0 ? 6.0f : 263.0f) + 41.5f) / 83.0f);
        int row = (int)((294.0f - b.y.load() + 48.0f) / 96.0f);
        return col >= 0 && col <= 2 && row >= 0 && row <= 2 ? row * 3 + col : -1;
    }
}

extern "C" void blitz_ds_bracket(float x, float y) {
    Bracket& b = brackets[x >= 250.0f ? 1 : 0];
    b.x = x;
    b.y = y;
    b.when = now_ms();
}

// Which tile, on the game's 4:3 picture (x, y 0-1, from the top): that half's 0-8, or -1. side: which half it's in.
static int tile_at(float x, float y, int* side) {
    float gx = x * 512.0f, gy = 384.0f - y * 384.0f;
    *side = gx >= 256.0f ? 1 : 0;
    int col = (int)((gx - (*side ? 263.0f : 6.0f)) / 83.0f);
    int row = (int)((294.0f - gy) / 96.0f);
    if (gx < (*side ? 263.0f : 6.0f) || gy > 294.0f || col > 2 || row > 2) return -1;
    return row * 3 + col;
}

namespace blitz {
    void dev_open_menu(const std::string& tab);  // controllers.cpp: opens Settings, from any thread
    void replay_open();                          // replay_screen.cpp
    bool replay_is_open();
    bool touch_controls_showing();               // touch_controls.cpp
    bool stylus_showing();                       // ds_stylus.cpp
    void post_status(const std::string& msg);    // notices.cpp: a toast
    void ds_touch_mark(float x, float y);        // ds_touch.cpp
    void ds_touch_result(int kind);
    int ds_touch_route_at(float x, float y, bool replay, int my_side);
    void ds_touch_update(SDL_Window* window, bool routes, bool replay, int my_side);
    void ds_backdrop_update(SDL_Window* window, bool want, const char* line);  // ds_touch.cpp: line under the title, "" none

    constexpr int tap_did_something = 1, tap_refused = 2;  // ds_touch_result: yellow, orange
    static std::atomic<bool> upper_touched{false};  // a tap from up there: BLITZ_ONE_TOUCH was wrong about this device
    static std::atomic<float> second_mark_x{0}, second_mark_y{0};
    static std::atomic<int64_t> second_mark_until{0};

    // The ring where a finger came down, before anything can refuse it (ds_touch.cpp; the second window's is in its panel).
    // Not over the stylus tiles or the on-screen controls, which light up for themselves. Potato Mode's one screen has it
    // too: that's where a touch that seems to do nothing needs to show it arrived (DualStack's one surface, GammaOS's menus).
    static void tap_mark(int screen, float x, float y) {
        static const bool lean = getenv("BLITZ_LEAN") != nullptr;
        if ((!have_second.load() && !lean) || stylus_showing() || touch_controls_showing()) return;
        if (screen == 0) {
            ds_touch_mark(x, y);
            return;
        }
        second_mark_x = x;
        second_mark_y = y;
        second_mark_until = now_ms() + 350;
    }

    // Nobody's playing the other half of the game: player 1 can be told where his plays are, and its header is free.
    static bool alone_in_game() {
        const int my_side = blitz_port_side(blitz_touch_port());
        if (my_side != 0 && my_side != 1) return false;
        for (int port = 1; port < 4; port++) {
            if (blitz_port_side(port) == 1 - my_side) return false;
        }
        return true;
    }

    // Settings and Replay, on the screen with touch, at play select: an upper screen with no touch of its own (an RG DS's:
    // BLITZ_ONE_TOUCH, BlitzActivity.java) can't reach the board's. Drawn on the first window, so only when that's the lower,
    // and in the other half's header (ds_touch.cpp), so only when nobody's using that.
    static bool lower_routes() {
        static const bool no_upper_touch = getenv("BLITZ_ONE_TOUCH") != nullptr;
        return no_upper_touch && !upper_touched.load() && have_second.load() && !swapped.load() && blitz_play_mode() == 1 && !replay_is_open() &&
               !recompui::is_context_capturing_input() && !touch_controls_showing() && alone_in_game();
    }

    // Replay's button only when it has something to open (replay_screen.cpp's own test): under 800 MB of RAM never.
    static bool replay_route() { return blitz_replay_count() >= 8; }

    // A tap, from a finger or a mouse. screen 0: the first window; 1: the second. x, y 0-1 on that screen. Any thread.
    void ds_tap(int screen, float x, float y) {
        if ((screen == 0) == swapped.load()) upper_touched = true;
        tap_mark(screen, x, y);
        if (!have_second.load() || recompui::is_context_capturing_input()) return;
        int mode = blitz_play_mode();
        // The tiles have the first window. In a live play a touch on the other is for the icons over the picture there.
        if (stylus_showing() && (screen == 0 || mode != 2)) return;
        bool lower = (screen == 0) != swapped.load();
        fprintf(stderr, "[ds] tap screen %d (%s) at %.3f,%.3f, moment %d\n", screen, lower ? "lower" : "upper", x, y, mode);
        // REPLAY, beside SETTINGS (panel_drive), or a tap on the last play itself: Instant Replay.
        const int board = blitz_ds_panel_first ? 0 : 1;  // the screen the drive board's on (tall_view.cpp)
        const float full_units = 1080.0f * (float)(board == 0 ? first_w.load() : second_w.load()) / (float)std::max(board == 0 ? first_h.load() : second_h.load(), 1);
        // The clip (panel_drive): the top right corner, or the left third.
        const bool clip_on = blitz_ds_clip_show != 0, corner = clip_on && blitz_ds_clip_right != 0;
        const float edge = full_units, ux = x * full_units;
        const bool on_clip = clip_on && (corner ? ux >= full_units - 436.0f * 4.0f / 3.0f && y < 436.0f / 1080.0f : ux < full_units / 3.0f);
        if (screen == board && (mode != 2 || blitz_ds_panel_first) && ((y > 0.91f && ux >= edge - 460.0f && ux < edge - 248.0f) || on_clip)) {
            fprintf(stderr, "[ds] tap: replay\n");
            ds_touch_result(tap_did_something);
            replay_open();
            return;
        }
        // The SETTINGS button (panel_settings): a device may have no key that opens it.
        if (screen == board && (mode != 2 || blitz_ds_panel_first) && ux >= edge - 236.0f && ux <= edge && y > 0.91f) {
            fprintf(stderr, "[ds] tap: settings\n");
            ds_touch_result(tap_did_something);
            dev_open_menu("general");
            return;
        }
        // The drive's list, on the board: its top half goes back, its bottom half forward.
        if (screen == board && blitz_play_select_on_screen() && y > 0.42f && y < 0.92f) {
            bool up = y < 0.67f;
            drive_scroll = up ? drive_scroll.load() + 1 : std::max(0, drive_scroll.load() - 1);
            fprintf(stderr, "[ds] tap: drive list %s\n", up ? "back" : "forward");
            ds_touch_result(tap_did_something);
            return;
        }
        if (mode == 1 && lower) {  // play select's on the lower screen, whichever window that is (tall_view.cpp)
            if (touch_controls_showing()) return;  // no controller: the on-screen ones are playing
            // The upper screen's buttons, down here where they can be reached.
            if (lower_routes()) {
                const int route = ds_touch_route_at(x, y, replay_route(), blitz_port_side(blitz_touch_port()));
                if (route != 0) {
                    fprintf(stderr, "[ds] tap: %s, from the lower screen\n", route == 1 ? "replay" : route == 3 ? "flip" : route == 4 ? "page" : "settings");
                    ds_touch_result(tap_did_something);
                    if (route == 1) replay_open();
                    else if (route == 3) press(btn_b, 6);  // Jump flips the plays here (game_moment.cpp)
                    else if (route == 4) press(btn_z, 6);  // the next page, as Turbo
                    else dev_open_menu("general");
                    return;
                }
            }
            // The game's 4:3 picture, centred on this screen whatever its shape.
            float w = (float)(screen == 0 ? first_w.load() : second_w.load()), h = (float)(screen == 0 ? first_h.load() : second_h.load());
            float picture_w = std::min(w, h * 4.0f / 3.0f), picture_h = std::min(h, w * 3.0f / 4.0f);
            float gx = picture_w > 0.0f ? (x * w - (w - picture_w) * 0.5f) / picture_w : x;
            float gy = picture_h > 0.0f ? (y * h - (h - picture_h) * 0.5f) / picture_h : y;
            if (gx < 0.0f || gx > 1.0f || gy < 0.0f || gy > 1.0f) return;
            int side = 0, my_side = blitz_port_side(blitz_touch_port());
            int tile = tile_at(gx, gy, &side);
            // Player 1's half only: only his buttons are pressed here, and the other half's plays are named differently, so
            // a pick from there would lock in one he never saw. Alone in the game he's told where his are. Not for a finger
            // resting on the right edge (team): that's only when the right half is the opponent's, so a player on that side
            // has its whole third column, which gets here as his own half.
            if (side != my_side) {
                fprintf(stderr, "[ds] tap: the other side's half (player 1 is on side %d)\n", my_side);
                if (alone_in_game() && !(side == 1 && x >= 0.86f)) {
                    ds_touch_result(tap_refused);
                    post_status(my_side == 0 ? "Your plays are on the left" : "Your plays are on the right");
                }
                return;
            }
            if (tile < 0) {
                if (gy < 0.225f) {  // the header: next page, as Turbo
                    press(btn_z, 6);
                    fprintf(stderr, "[ds] tap: next page\n");
                    ds_touch_result(tap_did_something);
                }
                return;
            }
            tile_steps = 0;
            tile_stuck = 0;
            tile_last_at = -1;
            tile_goal = tile;
            ds_touch_result(tap_did_something);
            fprintf(stderr, "[ds] tap: tile %d, cursor at %d (bracket %.0f,%.0f, %lld ms ago)\n", tile, read_cursor(side), brackets[side].x.load(), brackets[side].y.load(), (long long)(now_ms() - brackets[side].when.load()));
            return;
        }
        if (mode == 2) {
            if (screen == 0 && touch_controls_showing()) return;  // the on-screen stick and buttons have this screen
            if (lower && x >= 0.86f) return;  // the right edge is for resting a finger on (team)
            // Whoever's icon is nearest, on the screen tapped, within about its own size.
            std::lock_guard lock{badges_mutex};
            int best = -1;
            float best_d = 0.012f;
            float aspect = 0.75f;
            for (int k = 0; k < 3; k++) {
                const Badge& b = badges[(size_t)k];
                if (!b.on || b.screen != screen) continue;
                float dx = b.x - x, dy = (b.y - y) * aspect;
                float d = dx * dx + dy * dy;
                if (d < best_d) {
                    best_d = d;
                    best = k;
                }
            }
            if (best >= 0) {
                press(receiver_buttons[best]);
                fprintf(stderr, "[ds] tap: receiver %d\n", best);
                ds_touch_result(tap_did_something);
            }
            else {
                for (const Badge& b : badges) fprintf(stderr, "[ds] tap missed: icon %s screen %d at %.3f,%.3f\n", b.on ? "on" : "off", b.screen, b.x, b.y);
            }
        }
    }

    // Game thread, player 1's read (controllers.cpp): what taps are pressing.
    void ds_input_read(uint16_t* buttons) {
        // Play select: L has no job in this game, nor C-Up and C-Down there, so they go through the drive on the second
        // screen. On a PlayStation-style pad, L1 back and R1 (C-Down) forward.
        constexpr uint16_t back_buttons = 0x0020 | 0x0008, forward_buttons = 0x0004;  // L, C-Up; C-Down
        static uint16_t scroll_held = 0;
        if (have_second.load() && blitz_play_mode() == 1) {
            uint16_t now = *buttons & (back_buttons | forward_buttons);
            if ((now & back_buttons) && !(scroll_held & back_buttons)) drive_scroll = drive_scroll.load() + 1;
            if ((now & forward_buttons) && !(scroll_held & forward_buttons) && drive_scroll.load() > 0) drive_scroll = drive_scroll.load() - 1;
            scroll_held = now;
            *buttons &= (uint16_t)~(back_buttons | forward_buttons);
        }
        else {
            scroll_held = 0;
        }
        int reads = injected_reads.load();
        if (reads > 0) {
            *buttons |= (uint16_t)injected.load();
            injected_reads = reads - 1;
            return;
        }
        int goal = tile_goal.load();
        if (goal < 0) return;
        if (blitz_play_mode() != 1) {
            fprintf(stderr, "[ds] tile tap dropped: play select has gone\n");
            tile_goal = -1;
            return;
        }
        // A press, then a release, a step at a time.
        int wait = tile_wait.load();
        if (wait > 0) {
            tile_wait = wait - 1;
            return;
        }
        int side = blitz_port_side(blitz_touch_port());
        int at = side == 0 || side == 1 ? read_cursor(side) : -1;
        tile_wait = 8;
        if (at < 0) {
            // No bracket: the help page, which A closes, once: the grid takes a moment to come up, and a second A then
            // would pick whatever tile the cursor starts on. If it never comes, the tap's dropped.
            int stuck = tile_stuck.load();
            if (stuck >= 20) {
                fprintf(stderr, "[ds] tile tap dropped: no cursor on screen\n");
                tile_goal = -1;
                return;
            }
            // Only if this play select's grid hasn't been up: once it has, no brackets means the player hid his cursor (Up
            // from the top row), and A would lock in a play he can't see. Down brings it back, then it walks as ever.
            bool grid_seen = side >= 0 && side <= 1 && brackets[side].when.load() >= picking_since.load();
            if (grid_seen) {
                if (stuck % 4 == 0) {
                    fprintf(stderr, "[ds] cursor hidden: Down, to bring it back\n");
                    press(btn_down);
                }
            }
            else if (stuck == 0) {
                fprintf(stderr, "[ds] no cursor yet: A, for the help page\n");
                press(btn_a);
            }
            tile_stuck = stuck + 1;
            return;
        }
        // A step that didn't move it: the game wasn't listening (a slow device). Longer presses, then give up, and never A:
        // that would lock in whichever tile it's on.
        int steps = tile_steps.load();
        if (at != goal && at == tile_last_at.load() && steps >= tile_most_steps) {
            fprintf(stderr, "[ds] tile tap dropped: the cursor won't reach tile %d\n", goal);
            tile_goal = -1;
            return;
        }
        bool repeat = at == tile_last_at.load();
        tile_steps = steps + 1;
        tile_last_at = at;
        if (at == goal) {
            fprintf(stderr, "[ds] tile %d picked\n", goal);
            tile_goal = -1;
            press(btn_a);
            return;
        }
        int ar = at / 3, ac = at % 3, gr = goal / 3, gc = goal % 3;
        press(ar < gr ? btn_down : ar > gr ? btn_up : ac < gc ? btn_right : btn_left, repeat ? 8 : 4);
    }
}

// The second screen's look: the Blitz '27 house style (ui_theme.cpp, docs/design/redesign): midnight ground with the teal
// grid, dark glass panels with a cyan rim, Barlow Condensed italic in Blitz yellow, the red focus bar for what's now.
// Sizes in dp, 1080 to the screen's height, 1440 across on a 4:3 screen.
namespace {
    constexpr const char* ground = "position:absolute;left:0;right:0;top:0;bottom:0;background-color:#020712;";
    constexpr const char* display = "font-family:Barlow Condensed;font-style:italic;font-weight:800;";

    std::string ui_art(const char* name) { return std::string("?/blitz/ui/") + name; }  // ui_kit.cpp loads them

    std::string grid() {
        return "<div style=\"position:absolute;left:0;right:0;top:0;bottom:0;opacity:0.22;decorator:image(" + ui_art("grid_tile") + " repeat);\"></div>";
    }

    std::string text_at(int left, int top, int width, const char* align, int size, const char* color, const char* extra, const std::string& text) {
        char style[260];
        snprintf(style, sizeof(style), "position:absolute;left:%ddp;top:%ddp;width:%ddp;text-align:%s;font-size:%ddp;line-height:%ddp;color:%s;%s", left, top, width, align, size, size, color, extra);
        return std::string("<div style=\"") + style + "\">" + escape_rml(text) + "</div>";
    }

    // One line, whatever the screen: the size at which this text fits, by the font's average letter width (in font sizes).
    int fit_size(const std::string& text, int width, int most, float letter) {
        const int fits = text.empty() ? most : (int)((float)width / ((float)text.size() * letter));
        return std::clamp(fits, 20, most);
    }

#if BLITZ_MODERN
    // A .wms as RGBA, the right way up: types 8 and 19 (RGBA5551, 19 with a 4-bit alpha after), 16 (4-bit index), 0 (8-bit).
    bool wms_rgba(const std::vector<uint8_t>& d, std::vector<char>& out, uint32_t& w, uint32_t& h) {
        if (d.size() < 32) return false;
        auto be32 = [&](size_t o) { return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3]; };
        const uint32_t type = be32(4);
        w = be32(8);
        h = be32(12);
        if (w == 0 || h == 0 || w > 1024 || h > 1024) return false;
        const size_t n = (size_t)w * h;
        const uint8_t* p = d.data() + 32;
        const size_t room = d.size() - 32;
        auto c5551 = [&](size_t o) { return (uint16_t)(p[o] << 8 | p[o + 1]); };
        const size_t pad = (n + 7) & ~(size_t)7;
        if ((type == 8 && room < 2 * n) || (type == 19 && room < 2 * n) || (type == 16 && room < n / 2 + 32) || (type == 0 && room < pad + 512)) return false;
        if (type != 8 && type != 19 && type != 16 && type != 0) return false;
        out.resize(n * 4);
        for (size_t i = 0; i < n; i++) {
            uint16_t v = type == 16 ? c5551(n / 2 + 2 * ((p[i / 2] >> (i % 2 ? 0 : 4)) & 15)) : type == 0 ? c5551(pad + 2 * (size_t)p[i]) : c5551(2 * i);
            size_t row = i / w, col = i % w, at = (d[3] & 2 ? (h - 1 - row) * w + col : i) * 4;  // flag 2: stored upside down
            out[at] = (char)(((v >> 11) & 31) * 255 / 31);
            out[at + 1] = (char)(((v >> 6) & 31) * 255 / 31);
            out[at + 2] = (char)(((v >> 1) & 31) * 255 / 31);
            out[at + 3] = (char)(v & 1 ? 255 : 0);
        }
        return true;
    }

    // This game's logo for a side, as the play select's: the copy of the team's record (Pro Bowl art included). "" if none.
    std::string team_logo(int side) {
        static std::set<uint32_t> ready, failed;
        uint8_t* rdram = blitz::monitor_rdram();
        if (rdram == nullptr || blitz::original_game()) return "";
        uint32_t id = (uint32_t)MEM_W(0, (gpr)(int32_t)(0x802E9408u + (uint32_t)side * 0x64 + 0x5C));
        if ((id >> 24) != 0x0A || (id & 0xFFFFFF) == 0 || failed.count(id)) return "";
        char src[32];
        snprintf(src, sizeof(src), "?/dslogo%06X.png", id & 0xFFFFFF);
        if (!ready.count(id)) {
            std::vector<char> rgba;
            uint32_t w = 0, h = 0;
            auto file = blitz::rom_file_at((int32_t)(id & 0xFFFFFF) - 1);
            if (!file || !wms_rgba(*file, rgba, w, h)) {
                fprintf(stderr, "[ds] no logo for file %06X\n", id & 0xFFFFFF);
                failed.insert(id);
                return "";
            }
            recompui::queue_image_from_bytes_rgba32(src, rgba, w, h);
            ready.insert(id);
        }
        return src;
    }
#else
    std::string team_logo(int) { return ""; }
#endif

    std::string image_at(const std::string& name, int left, int top, int width, int height) {
        return "<img src=\"" + ui_art(name.c_str()) + "\" style=\"position:absolute;left:" + std::to_string(left) + "dp;top:" + std::to_string(top) + "dp;width:" + std::to_string(width) + "dp;height:" + std::to_string(height) + "dp;\"/>";
    }

    std::string box_at(int left, int top, int width, int height, const std::string& rest) {
        return "<div style=\"position:absolute;left:" + std::to_string(left) + "dp;top:" + std::to_string(top) + "dp;width:" + std::to_string(width) + "dp;height:" + std::to_string(height) + "dp;" + rest + "\"></div>";
    }

    // The house's nine-piece art (ui_kit.cpp's panels), corners cw by ch units.
    std::string frame_at(const std::string& art, int left, int top, int width, int height, int cw, int ch) {
        std::string o;
        const int mid_w = width - 2 * cw, mid_h = height - 2 * ch;
        o += image_at(art + ".tl", left, top, cw, ch) + image_at(art + ".t", left + cw, top, mid_w, ch) + image_at(art + ".tr", left + cw + mid_w, top, cw, ch);
        o += image_at(art + ".l", left, top + ch, cw, mid_h) + image_at(art + ".c", left + cw, top + ch, mid_w, mid_h) + image_at(art + ".r", left + cw + mid_w, top + ch, cw, mid_h);
        o += image_at(art + ".bl", left, top + ch + mid_h, cw, ch) + image_at(art + ".b", left + cw, top + ch + mid_h, mid_w, ch) + image_at(art + ".br", left + cw + mid_w, top + ch + mid_h, cw, ch);
        return o;
    }

    // Framed glass with the teal grid in it, as every panel in the house style.
    std::string glass_at(int left, int top, int width, int height) {
        return frame_at("panel_frame", left, top, width, height, 24, 24) +
               "<div style=\"position:absolute;left:" + std::to_string(left + 6) + "dp;top:" + std::to_string(top + 6) + "dp;width:" + std::to_string(width - 12) + "dp;height:" + std::to_string(height - 12) + "dp;opacity:0.5;decorator:image(" + ui_art("grid_tile") + " repeat);\"></div>";
    }

    // The second screen's way into Settings, bottom right: a handheld may have no key for it (ds_tap).
    std::string panel_settings(int full) {
        return box_at(full - 236, 1010, 212, 56, "background-color:#0d1a3a;border-width:2dp;border-color:#8feff561;border-radius:4dp;") +
               text_at(full - 236, 1038 - 17, 212, "center", 34, "#b8c4da", display, "Settings");
    }

    std::string panel_title(int full) {
        const int logo_w = std::min(1100, full * 85 / 100);
        return std::string("<div style=\"") + ground + "\">" + grid() +
               // BRANDING: BLITZ '27 lockup on the second screen
               "<img src=\"" + ui_art("logo27_lockup") + "\" style=\"position:absolute;left:" + std::to_string((full - logo_w) / 2) + "dp;top:" + std::to_string(473 - logo_w * 96 / 738) + "dp;width:" + std::to_string(logo_w) + "dp;height:" + std::to_string(logo_w * 96 / 738) + "dp;\"/>"
               "<div style=\"position:absolute;left:0;right:0;top:530dp;text-align:center;font-size:" + std::to_string(std::min(190, full / 5)) + "dp;color:#f6d400;" + display + "\">BLITZ DS</div>" + panel_settings(full) + "</div>";
    }

    struct LiveState {
        char team[2][8];
        int points[2];
        int quarter, clock_seconds, offense;
        char clock[24], situation[48];
        int spot, marker, kind;
        int valid;
    };

    // The field, as a strip: where this drive began, where the ball is, and the line to gain. Offense always going right.
    // left and width in dp; goes from yard 0 to 100 between two end zones.
    std::string field_strip(int left, int top, int width, int height, const LiveState& live, bool in_drive, int start) {
        const int zone = width * 8 / 100, field_left = left + zone, field_w = width - 2 * zone;
        auto x_of = [&](int yard) { return field_left + field_w * std::clamp(yard, 0, 100) / 100; };
        std::string o;
        o += box_at(left - 3, top - 3, width + 6, height + 6, "background-color:#8feff5b2;");
        o += box_at(field_left, top, field_w, height, "background-color:#14682c;");
        o += box_at(left, top, zone, height, "background-color:#3f0a5f;");
        o += box_at(left + width - zone, top, zone, height, "background-color:#b3101e;");
        for (int yard = 10; yard <= 90; yard += 10) {
            o += box_at(x_of(yard) - 1, top, yard == 50 ? 4 : 2, height, "background-color:#ffffff8c;");
        }
        // The ground this drive has covered.
        const int ball = x_of(live.spot);
        if (in_drive && live.spot != start) {
            const int from = std::min(x_of(start), ball), span = std::abs(ball - x_of(start));
            o += box_at(from, top + height / 2 - 11, span, 22, live.spot > start ? "background-color:#8feff5d0;" : "background-color:#ff6a3dd0;");
        }
        if (live.kind == 0 && live.marker < 100) o += box_at(x_of(live.marker) - 3, top - 8, 6, height + 16, "background-color:#ffa21f;");
        o += box_at(ball - 3, top - 8, 6, height + 16, "background-color:#ffffff;");
        return o;
    }

    // with_clip: the last play runs on the left, 720 of the 1080-high units wide (RT64 draws it under this), and all of
    // this fits beside it. full: this screen's width in those units. Type is placed by its centre line: every text box is
    // exactly its font size tall, so top = centre - size / 2.
    constexpr int ds_corner_h = 436;  // the corner clip's height, in the panel's 1080: down to the field strip (rt64: 436/1080)
    std::string panel_drive(bool with_clip, int full, const LiveState& live) {
        std::lock_guard lock{drive_mutex};
        // Where RT64 draws the last play (rt64_present_queue.cpp), as the team laid it out (2026-11-02): in Both Screens the tall
        // clip in the left third, the board in the rest; in Game + Companion and Stylus (blitz_ds_clip_right 1) a 4:3 clip in
        // the top right corner, the board the whole screen with only its top rows (score, down, field) stopping short of it.
        const bool corner = with_clip && blitz_ds_clip_right != 0;
        const int clip_w = full / 3, corner_w = ds_corner_h * 4 / 3;
        const int left = with_clip && !corner ? clip_w : 0, width = full - left;
        const int pad = with_clip ? 24 : 44, inner = width - 2 * pad, x0 = left + pad, edge = left + width;
        const int top_w = corner ? width - corner_w - 12 : width, top_inner = top_w - 2 * pad, top_edge = left + top_w;
        const bool wide = inner > 1000;  // room for the numbers beside the plays
        std::string o;
        if (corner) {  // the ground everywhere but the corner, which RT64 has drawn
            const int cx = full - corner_w;
            o += box_at(0, ds_corner_h, full, 1080 - ds_corner_h, "background-color:#020712;") + box_at(0, 0, cx, ds_corner_h, "background-color:#020712;");
            o += "<div style=\"position:absolute;left:0;right:0;top:" + std::to_string(ds_corner_h) + "dp;bottom:0;\">" + grid() + "</div>";
            o += "<div style=\"position:absolute;left:0;width:" + std::to_string(cx) + "dp;top:0;height:" + std::to_string(ds_corner_h) + "dp;\">" + grid() + "</div>";
            o += box_at(cx - 4, 0, 4, ds_corner_h + 4, "background-color:#8feff5b2;") + box_at(cx - 4, ds_corner_h, corner_w + 4, 4, "background-color:#8feff5b2;");
            o += box_at(cx + (corner_w - 240) / 2, ds_corner_h - 50, 240, 46, "background-color:#b3101e;");
            o += text_at(cx + (corner_w - 240) / 2, ds_corner_h - 50 + 6, 240, "center", 34, "#ffe14a", display, "Last play");
        }
        else if (with_clip) {
            o += box_at(left, 0, width, 1080, "background-color:#020712;");
            o += "<div style=\"position:absolute;left:" + std::to_string(left) + "dp;width:" + std::to_string(width) + "dp;top:0;bottom:0;\">" + grid() + "</div>";
            o += box_at(left - 4, 0, 4, 1080, "background-color:#8feff5b2;");
            const int tag_w = std::min(300, clip_w), tag_x = (clip_w - tag_w) / 2;
            o += box_at(tag_x, 1020, tag_w, 60, "background-color:#b3101e;");
            o += text_at(tag_x, 1029, tag_w, "center", 42, "#ffe14a", display, "Last play");
        }
        else {
            o += std::string("<div style=\"") + ground + "\">" + grid() + "</div>";
        }

        // The score, on the bevelled plate; the team with the ball in yellow.
        constexpr int plate_h = 152, plate_mid = 78;
        const int name_w = with_clip ? 110 : 330, points_w = with_clip ? 72 : 170, off = live.offense & 1;  // room for the clock, on one line
        o += frame_at("header_bevel", left, 0, top_w, plate_h, 44, 37);
        const int name_size = with_clip ? 56 : 88, points_size = with_clip ? 70 : 104, clock_size = with_clip ? 34 : 56;
        // Each team's logo, over a bar on the one with the ball; its letters if there's no logo.
        for (int side = 0; side < 2; side++) {
            const std::string logo = team_logo(side);
            const int slot_x = side == 0 ? x0 + 8 : top_edge - pad - 8 - name_w;
            if (logo.empty()) {
                o += text_at(slot_x, plate_mid - name_size / 2, name_w, side == 0 ? "left" : "right", name_size, off == side ? "#ffe14a" : "#ffffff", display, live.team[side]);
                continue;
            }
            const int logo_h = plate_h - 52, logo_w = std::min(name_w, logo_h * 2);
            o += "<img src=\"" + logo + "\" style=\"position:absolute;left:" + std::to_string(slot_x + (name_w - logo_w) / 2) + "dp;top:18dp;width:" + std::to_string(logo_w) + "dp;height:" + std::to_string(logo_h) + "dp;\"/>";
            if (off == side) o += box_at(slot_x + name_w / 4, plate_h - 30, name_w / 2, 6, "background-color:#ffe14a;");
        }
        o += text_at(x0 + name_w, plate_mid - points_size / 2, points_w, "center", points_size, "#ffffff", display, std::to_string(live.points[0]));
        o += text_at(top_edge - pad - name_w - points_w, plate_mid - points_size / 2, points_w, "center", points_size, "#ffffff", display, std::to_string(live.points[1]));
        o += text_at(x0 + name_w + points_w, plate_mid - clock_size / 2, top_inner - 2 * name_w - 2 * points_w, "center", clock_size, "#8feff5", (std::string(display) + "white-space:nowrap;").c_str(), live.clock);

        // What's next, big, between the game's own cursor brackets.
        const std::string down = down_part(live.situation), where = where_part(live.situation);
        const int hero_size = fit_size(down, top_inner - 90, with_clip ? 112 : 136, 0.46f);  // EXTRA POINT, on one line
        o += image_at("selector_cap_l.t", x0, 172, 36, 42) + image_at("selector_cap_l.c", x0, 214, 36, 76) + image_at("selector_cap_l.b", x0, 290, 36, 42);
        o += image_at("selector_cap_r.t", top_edge - pad - 36, 172, 36, 42) + image_at("selector_cap_r.c", top_edge - pad - 36, 214, 36, 76) + image_at("selector_cap_r.b", top_edge - pad - 36, 290, 36, 42);
        o += text_at(x0, 226 - hero_size / 2, top_inner, "center", hero_size, "#f6d400", (std::string(display) + "white-space:nowrap;").c_str(), down);
        o += text_at(x0, 304 - 21, top_inner, "center", 42, "#ffffff", display, where.empty() ? std::string(live.team[off]) + " ball" : std::string(live.team[off]) + " ball, " + where);

        // The field.
        const bool in_drive = drive.plays > 0;
        o += field_strip(x0, 356, top_inner, 66, live, in_drive, drive_start);

        // The drive's numbers: across, or down the side on a wide screen.
        char clock_text[16];
        snprintf(clock_text, sizeof(clock_text), "%d:%02d", drive.seconds / 60, drive.seconds % 60);
        const struct { const char* name; std::string value; } stats[3] = {{"Plays", std::to_string(drive.plays)}, {"Yards", std::to_string(drive.yards)}, {"Time", clock_text}};
        const int top_y = 446, bottom_y = 992, side_w = wide ? 340 : 0;
        for (int i = 0; i < 3; i++) {
            const int stat_w = wide ? side_w : (inner - 24) / 3, stat_h = wide ? (bottom_y - top_y - 24) / 3 : 136;
            const int x = wide ? x0 : x0 + i * (stat_w + 12), y = wide ? top_y + i * (stat_h + 12) : top_y;
            o += glass_at(x, y, stat_w, stat_h);
            const int label_mid = y + 26, value_mid = y + stat_h / 2 + 16;
            o += text_at(x, label_mid - 13, stat_w, "center", 26, "#8feff5", "font-weight:700;", stats[i].name);
            o += text_at(x, value_mid - 37, stat_w, "center", 74, "#ffffff", display, stats[i].value);
        }

        // Every play of it, newest at the bottom; C-Up and C-Down, or a tap, go through a long one.
        const int list_x = wide ? x0 + side_w + 12 : x0, list_w = wide ? inner - side_w - 12 : inner;
        const int list_y = wide ? top_y : 596, list_h = bottom_y - list_y;
        constexpr int head_h = 66, row_h = 78;
        const int fits = (list_h - head_h - 8) / row_h;
        const int total = (int)rows.size(), shown = std::min(total, fits);
        const int most_back = std::max(0, total - shown);
        const int back = std::clamp(drive_scroll.load(), 0, most_back);
        drive_scroll = back;
        const int first = total - shown - back;
        o += glass_at(list_x, list_y, list_w, list_h);
        o += text_at(list_x + 28, list_y + 36 - 15, list_w / 2, "left", 30, "#8feff5", "font-weight:700;", "This drive");
        if (most_back > 0) {
            char where_in[48];
            snprintf(where_in, sizeof(where_in), "%s%d-%d of %d%s", first > 0 ? "\xE2\x96\xB2 " : "", first + 1, first + shown, total, back > 0 ? " \xE2\x96\xBC" : "");
            o += text_at(list_x + list_w / 2, list_y + 36 - 15, list_w / 2 - 28, "right", 30, "#ffe14a", "font-weight:700;", where_in);
        }
        o += image_at("rule_chrome", list_x + 28, list_y + 56, list_w - 56, 6);
        int y = list_y + head_h;
        const int chip_w = with_clip ? 204 : 260, gain_w = 130, row_x = list_x + 24, row_w = list_w - 48;
        for (int i = first; i < first + shown; i++) {
            const PlayRow& row = rows[(size_t)i];
            const int mid = y + row_h / 2;
            if (i != first) o += box_at(row_x, y - 1, row_w, 2, "background-color:#8feff53d;");
            o += box_at(row_x, mid - 26, chip_w, 52, "background-color:#0d1a3a;border-width:2dp;border-color:#8feff53d;border-radius:4dp;");
            const int chip_size = fit_size(row.before, chip_w - 12, 34, 0.56f);
            o += text_at(row_x, mid - chip_size / 2, chip_w, "center", chip_size, "#b8c4da", "font-weight:700;white-space:nowrap;", row.before);
            const int word_w = row_w - chip_w - gain_w - 24, word_size = fit_size(row.word, word_w, 54, 0.48f);
            o += text_at(row_x + chip_w + 20, mid - word_size / 2, word_w, "left", word_size, "#ffffff", (std::string(display) + "white-space:nowrap;").c_str(), row.word);
            o += text_at(row_x + row_w - gain_w, mid - 30, gain_w, "right", 60, row.ink, display, row.gain);
            y += row_h;
        }
        if (rows.empty()) {
            o += text_at(list_x + 28, list_y + head_h + (list_h - head_h) / 2 - 28, list_w - 56, "center", with_clip ? 44 : 56, "#b8c4da", display, "First play of the drive");
        }

        // How the last one went, while this one's young.
        if (!last_drive.team.empty() && rows.size() <= 2) {
            char line[80];
            snprintf(line, sizeof(line), with_clip ? "Last: %s, %d plays, %d yds" : "Last drive: %s, %d plays, %d yds, ", last_drive.team.c_str(), last_drive.plays, last_drive.yards);
            const std::string last_text = with_clip ? std::string(line) : line + last_drive.result;
            const int last_size = fit_size(last_text, inner - 454, with_clip ? 24 : 30, 0.56f);  // clear of the buttons
            if (inner - 454 >= 300) o += text_at(x0, 1038 - last_size / 2, inner - 454, "left", last_size, "#b8c4da", "font-weight:700;white-space:nowrap;", last_text);  // no room beside the clip
        }
        // Instant Replay, full screen: ds_tap. At the board's own right edge, clear of a clip beside it.
        o += box_at(edge - 460, 1010, 212, 56, "background-color:#0d1a3a;border-width:2dp;border-color:#f6d40080;border-radius:4dp;") +
             text_at(edge - 460, 1038 - 17, 212, "center", 34, "#f6d400", display, "Replay");
        return o + panel_settings(edge);
    }

    // During a play: three icons that only ever move (ds_view moves them by id), so it's parsed once.
    std::string panel_badges() {
        std::lock_guard lock{badges_mutex};
        std::string out;
        constexpr float size = 94.0f;  // as on the first screen (receiver_targets.cpp), in 1080-high units
        for (int k = 0; k < (int)badges.size(); k++) {
            char style[360];
            snprintf(style, sizeof(style), "display:none;position:absolute;width:%.0fdp;height:%.0fdp;border-width:3dp;border-color:#ffffffdc;border-radius:%.0fdp;background-color:#080c1cc8;text-align:center;line-height:%.0fdp;font-family:promptfont;font-size:%.0fdp;color:#ffffff;margin-left:-%.0fdp;margin-top:-%.0fdp;",
                     size, size, size * 0.5f, size - 6.0f, size * 0.62f, size * 0.5f, size);
            out += "<div id=\"b" + std::to_string(k) + "\" style=\"";
            out += style;
            out += "\">" + badges[(size_t)k].glyph + "</div>";
        }
        return out;
    }

    // The ring for a tap on the second window (ds_touch.cpp has the first's), in its panel. Not the picture's three icons,
    // which are parsed once.
    std::string second_mark(int full) {
        if (blitz_ds_panel_first || now_ms() >= blitz::second_mark_until.load()) return "";
        constexpr int size = 120;
        return box_at((int)(blitz::second_mark_x.load() * (float)full) - size / 2, (int)(blitz::second_mark_y.load() * 1080.0f) - size / 2, size, size, "border-width:8dp;border-color:#ffffffdc;border-radius:60dp;");
    }

    void log_state(const char* what) { fprintf(stderr, "[ds] %s\n", what); }
}

namespace blitz {
    bool two_screens_setting();  // config.cpp
    void ds_settings_found();    // config.cpp
    void ds_second_seen();       // config.cpp
    void ds_screens_update();    // config.cpp

    namespace {
        std::mutex second_mutex;
        void* second_a = nullptr;
        void* second_b = nullptr;
    }

    // The second screen came or went (Android's surface; the Mac's stand-in). w, h in pixels. Any thread. Used now if
    // the setting allows it (on by default), else when it's turned on.
    void ds_second_screen(void* a, void* b, int w, int h) {
        fprintf(stderr, "[ds] second screen: %s %dx%d\n", a != nullptr ? "on" : "off", w, h);
        std::lock_guard lock{second_mutex};
        second_a = a;
        second_b = b;
        if (a == nullptr) {
            have_second = false;
            blitz_ds_mode = 0;
            blitz_ds_set_window(nullptr, nullptr);
            return;
        }
        second_w = w;
        second_h = h;
        shapes_changed = true;
    }

    void ds_set_swapped(bool on) {
        swapped = on;
        apply_shapes();
        fprintf(stderr, "[ds] upper screen is the %s window\n", on ? "first" : "second");
    }

    // Tapping the first window. On a Mac, the mouse on either.
    namespace {
        Uint32 second_window = 0;

#if !defined(__APPLE__)
        // Every finger, before anything can refuse it, where touch is in question: what the panel reports out to its edges,
        // and whether a stretch with no tap lines had any touch at all. Down, then how far it went when it lifts. [ds] tap
        // lines are what became of them.
        void touch_raw(const SDL_Event* e) {
            if (e->type != SDL_FINGERDOWN && e->type != SDL_FINGERMOTION && e->type != SDL_FINGERUP) return;
            static const bool lean = getenv("BLITZ_LEAN") != nullptr;
            if (!have_second.load() && !lean) return;
            static bool down = false;
            static SDL_FingerID finger = 0;
            static float low_x = 0, high_x = 0, low_y = 0, high_y = 0;
            static Uint32 since = 0;
            const SDL_TouchFingerEvent& f = e->tfinger;
            if (e->type == SDL_FINGERDOWN) {
                fprintf(stderr, "[touch raw] down %.3f,%.3f finger %lld\n", f.x, f.y, (long long)f.fingerId);
                if (down && f.fingerId != finger) return;  // the first finger's the one followed
                down = true;
                finger = f.fingerId;
                low_x = high_x = f.x;
                low_y = high_y = f.y;
                since = f.timestamp;
                return;
            }
            if (!down || f.fingerId != finger) return;
            low_x = std::min(low_x, f.x);
            high_x = std::max(high_x, f.x);
            low_y = std::min(low_y, f.y);
            high_y = std::max(high_y, f.y);
            if (e->type == SDL_FINGERUP) {
                fprintf(stderr, "[touch raw] up: x %.3f-%.3f, y %.3f-%.3f, %u ms\n", low_x, high_x, low_y, high_y, (unsigned)(f.timestamp - since));
                down = false;
            }
        }
#endif

        // Taps the event watch saw, handled on the main thread (ds_update). An SDL event watch runs with SDL's own locks held
        // (its watcher lock; the joystick lock too, for a pad's event from Android's UI thread), so it must never wait on the
        // UI lock: the draw holds that while it asks SDL about controllers (touch_only's SDL_NumJoysticks, a pad's profile).
        // On an RG DS with two screens, a pad press in Settings froze the game that way (2026-11-15): the present thread in
        // the draw waited for the joystick lock, Android's UI thread (a pad release) held it and waited for the watcher lock,
        // and SDL's thread held that here, waiting for the UI lock.
        struct PendingTap { int screen; float x, y; bool mouse; };
        std::mutex pending_taps_mutex;  // held only to push or swap
        std::vector<PendingTap> pending_taps;

        int tap_watch(void*, SDL_Event* e) {
#if !defined(__APPLE__)
            touch_raw(e);
            if (e->type == SDL_FINGERDOWN) {
                std::lock_guard lock{pending_taps_mutex};
                pending_taps.push_back({0, e->tfinger.x, e->tfinger.y, false});
            }
#else
            if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button == SDL_BUTTON_LEFT && e->button.which != SDL_TOUCH_MOUSEID) {
                SDL_Window* win = SDL_GetWindowFromID(e->button.windowID);
                if (win == nullptr) return 1;
                int w = 0, h = 0;
                SDL_GetWindowSize(win, &w, &h);
                if (w > 0 && h > 0) {
                    std::lock_guard lock{pending_taps_mutex};
                    pending_taps.push_back({e->button.windowID == second_window ? 1 : 0, (float)e->button.x / w, (float)e->button.y / h, true});
                }
            }
#endif
            return 1;
        }

        // Main thread (ds_update): what tap_watch used to do on the spot.
        void handle_pending_taps() {
            std::vector<PendingTap> taps;
            {
                std::lock_guard lock{pending_taps_mutex};
                if (pending_taps.empty()) return;
                taps.swap(pending_taps);
            }
            for (const PendingTap& t : taps) {
                if (!have_second.load() || !ultramodern::is_game_started() || recompui::is_context_capturing_input()) {
                    if (!t.mouse) tap_mark(0, t.x, t.y);  // one screen, or a menu's: still felt
                    continue;
                }
                ds_tap(t.screen, t.x, t.y);
            }
        }
    }

    // A Mac test: a second window above the game's (BLITZ_DS=1). Screens are as big as the first window.
    void ds_window_created(SDL_Window* window) {
        SDL_AddEventWatch(tap_watch, nullptr);
#if defined(__APPLE__) && !defined(BLITZ_IOS)
        if (getenv("BLITZ_DS") == nullptr) return;
        int w = 0, h = 0, x = 0, y = 0;
        SDL_GetWindowSize(window, &w, &h);
        SDL_GetWindowPosition(window, &x, &y);
        int gap = 70;
        y = std::max(40, y);
        int second_width = w, second_height = h;  // BLITZ_DS_SECOND=WxH: a differently sized screen
        if (const char* size = getenv("BLITZ_DS_SECOND")) sscanf(size, "%dx%d", &second_width, &second_height);
        SDL_Window* second = SDL_CreateWindow("BLITZ DS: upper screen", x, y, second_width, second_height, SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI);
        SDL_SetWindowPosition(window, x, y + second_height + gap);
        if (second == nullptr) {
            fprintf(stderr, "[ds] no second window: %s\n", SDL_GetError());
            return;
        }
        SDL_MetalView view = SDL_Metal_CreateView(second);
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        SDL_GetWindowWMInfo(second, &info);
        int pw = w, ph = h;
        SDL_GetWindowSizeInPixels(second, &pw, &ph);
        second_window = SDL_GetWindowID(second);
        ds_second_screen(info.info.cocoa.window, SDL_Metal_GetLayer(view), pw, ph);
#else
        (void)window;
#endif
    }

    namespace { int first_w_now = 0, first_h_now = 0; }

    long host_total_memory_mb() {
#if defined(__ANDROID__)
        struct sysinfo info;
        if (sysinfo(&info) == 0) return (long)(((uint64_t)info.totalram * info.mem_unit) >> 20);
#endif
        return 0;
    }

    // What this run's been limited to: the Graphics tab's Apply brings the saved choices back, so they're held every tick.
    namespace {
        ultramodern::renderer::Resolution res_cap = ultramodern::renderer::Resolution::Auto;
        bool no_msaa = false, capped_rr = false;
        // What ds_set_quality chose, before any saved limits: a change of screen count starts again from these.
        ultramodern::renderer::Resolution quality_cap = ultramodern::renderer::Resolution::Auto;
        bool quality_no_msaa = false;

        int res_rank(ultramodern::renderer::Resolution r) {
            using ultramodern::renderer::Resolution;
            return r == Resolution::Original ? 0 : r == Resolution::Original2x ? 1 : 2;
        }

        void hold_caps() {
            using namespace ultramodern::renderer;
            GraphicsConfig c = get_graphics_config();
            bool change = false;
            bool exact = false;
#ifdef __ANDROID__
            // Potato Mode's two screens, or a recipe's scale (BlitzRecipe.java): Two-Screen Sharpness says exactly, and nothing steps it.
            // 1x is the original resolution, the setup the team called almost there; the others are 2x at that scale (blitz_2x_scale).
            static const bool lean = getenv("BLITZ_LEAN") != nullptr, recipe_scale = getenv("BLITZ_RECIPE_SCALE") != nullptr;
            if (lean && (have_second.load() || recipe_scale)) {
                exact = true;
                const Resolution want = potato_scale() <= 1.0 ? Resolution::Original : Resolution::Original2x;
                if (c.res_option != want) { c.res_option = want; change = true; }
            }
#endif
            if (!exact) {
                // A cap saved at the original resolution still leaves two screens free to choose more.
                const Resolution cap = have_second.load() && res_cap == Resolution::Original ? Resolution::Original2x : res_cap;
                if (res_rank(c.res_option) > res_rank(cap)) { c.res_option = cap; change = true; }
            }
            if (no_msaa && c.msaa_option != Antialiasing::None) { c.msaa_option = Antialiasing::None; change = true; }
            if (capped_rr && c.rr_option != RefreshRate::Original) { c.rr_option = RefreshRate::Original; change = true; }
            if (change) {
                fprintf(stderr, "[ds] quality limits held\n");
                set_graphics_config(c);
            }
        }
    }

    // BLITZ DS protects smoothness (team). The first guess comes from memory; if play still can't hold its frame rate it
    // steps down by itself (ds_update). All for this run: the Graphics tab's saved choices aren't touched.
    // BLITZ_DS_PROFILE=low|mid|high overrides the guess, BLITZ_DS_FPS_FLOOR=<fps> the rate it holds to.
    // 2x, not original: the two screens share the game's lines, so original is half of them per screen, and the
    // scoreboard can't be read (Mac, 2026-10-31).
    void ds_set_quality() {
        using namespace ultramodern::renderer;
        static bool done = false;  // comes back with the screen after the app's been away
        if (done) return;
        done = true;
        long mb = host_total_memory_mb();
        const char* env = getenv("BLITZ_DS_PROFILE");
        const bool lean = getenv("BLITZ_LEAN") != nullptr;
        std::string profile = env != nullptr ? env : lean ? "low" : mb <= 0 ? "high" : mb < 2500 ? "low" : mb < 5000 ? "mid" : "high";
        fprintf(stderr, "[ds] quality profile %s (%ld MB)\n", profile.c_str(), mb);
        // The first thing to go when a frame rate won't hold (hold_frame_rate), but on from 800 MB: an RG DS (950 MB) has
        // ~290 MB free in a game and the clip takes ~47 MB (the team wants it there, 2026-11-02). It was off in the low profile.
        blitz_ds_clip_enabled = (mb > 0 && mb < 800) ? 0 : 1;
        if (profile == "high") return;
        // About 1 GB of RAM: original resolution on its one screen (2x was too slow). Two screens go by Sharpness (hold_caps).
        res_cap = lean && mb > 0 && mb < 1500 ? Resolution::Original : Resolution::Original2x;  // 1 GB: every texture counts
        if (getenv("BLITZ_SAFE_START") != nullptr) res_cap = Resolution::Original;  // BlitzActivity.safeStartCheck: for this launch only
        no_msaa = true;
        capped_rr = true;
        quality_cap = res_cap;
        quality_no_msaa = no_msaa;
    }

#ifdef __ANDROID__
    // What a handheld stepped down to, for the next run: it doesn't have to play slowly to find out again.
    // To try Auto again, <app folder>/gfx_limits.txt needs a line for the run's screen count: "1 0 0 2" or "2 0 0 2" (third
    // number 1: with antialiasing off). "0 2" only speaks for one screen. With no line, or no file, load_limits starts at 2x.
    // A line per screen count and Potato Mode or not, which never expires: "<screens> <lean> <antialiasing off> <resolution>".
    // Two numbers are from before those keys, on one screen and out of Potato Mode: a Retroid's pin stays, an RG DS's old one doesn't.
    struct SavedLimits { int screens, lean, msaa, res; };
    static std::filesystem::path limits_file() { return recomp::get_config_path() / "gfx_limits.txt"; }
    static std::vector<SavedLimits> read_limits() {
        std::vector<SavedLimits> all;
        std::ifstream f{limits_file()};
        std::string line;
        while (std::getline(f, line)) {
            int a = 0, b = 0, c = 0, d = 0;
            const int n = sscanf(line.c_str(), "%d %d %d %d", &a, &b, &c, &d);
            if (n == 2) all.push_back(SavedLimits{1, 0, a, b});
            else if (n == 4) all.push_back(SavedLimits{a, b, c, d});
        }
        return all;
    }
    static void save_limits() {
        const int screens = have_second.load() ? 2 : 1, lean = getenv("BLITZ_LEAN") != nullptr ? 1 : 0;
        const std::vector<SavedLimits> all = read_limits();
        std::ofstream f{limits_file()};
        for (const SavedLimits& l : all) {
            if (l.screens != screens || l.lean != lean) f << l.screens << ' ' << l.lean << ' ' << l.msaa << ' ' << l.res << '\n';
        }
        f << screens << ' ' << lean << ' ' << (no_msaa ? 1 : 0) << ' ' << res_rank(res_cap) << '\n';
    }
    static void load_limits() {
        using ultramodern::renderer::Resolution;
        const int screens = have_second.load() ? 2 : 1;
        const bool lean = getenv("BLITZ_LEAN") != nullptr;
        for (const SavedLimits& l : read_limits()) {
            if (l.screens != screens || l.lean != (lean ? 1 : 0)) continue;
            const Resolution saved = l.res == 0 ? Resolution::Original : l.res == 1 ? Resolution::Original2x : Resolution::Auto;
            // Potato Mode's own limits (ds_set_quality) are the ceiling: a saved line only goes lower.
            if (!lean || res_rank(saved) < res_rank(res_cap)) res_cap = saved;
            no_msaa = no_msaa || l.msaa != 0;
            fprintf(stderr, "[gfx] limits from earlier runs (%d screens, lean %d): antialiasing %s, resolution %d (0 original, 1 2x, 2 auto)\n", screens, lean ? 1 : 0, no_msaa ? "off" : "as saved", l.res);
            return;
        }
        // Nothing measured on this device yet. Auto would work out 11x or 12x over two unequal panels, so a strong one starts
        // at the RP5's measured best and moves up from real runs. Potato Mode has its own limits (ds_set_quality).
        if (!lean && getenv("BLITZ_DS_PROFILE") == nullptr) {
            no_msaa = true;
            res_cap = Resolution::Original2x;
            fprintf(stderr, "[gfx] first run here: 2x, antialiasing off\n");
        }
    }
#endif

    // Too slow during play: one step down, and a while to see if it helped. Never back up in a run.
    static void hold_frame_rate(bool playing) {
        using namespace ultramodern::renderer;
        using clock = std::chrono::steady_clock;
#ifdef __ANDROID__
        // Per screen count: a second screen can arrive after the first tick, and it mustn't play by the one-screen line.
        static int loaded_screens = 0;
        const int screens = have_second.load() ? 2 : 1;
        if (loaded_screens != screens) {
            if (loaded_screens != 0) {
                res_cap = quality_cap;
                no_msaa = quality_no_msaa;
            }
            loaded_screens = screens;
            load_limits();
        }
#endif
        hold_caps();
        static const float floor_fps = getenv("BLITZ_DS_FPS_FLOOR") ? (float)atof(getenv("BLITZ_DS_FPS_FLOOR")) : 26.0f;
        static clock::time_point next = clock::now() + std::chrono::seconds(30);
        static int slow = 0;
        if (!playing || clock::now() < next) return;
        next = clock::now() + std::chrono::seconds(5);
        float fps = fps_last_second();
        if (fps <= 0.0f) return;
        slow = fps < floor_fps ? slow + 1 : 0;
        if (slow < 3) return;
        slow = 0;
        next = clock::now() + std::chrono::seconds(30);
        GraphicsConfig c = get_graphics_config();
#ifdef __ANDROID__
        const bool was_no_msaa = no_msaa;
        const Resolution was_cap = res_cap;
#endif
        if (blitz_ds_clip_enabled) {
            blitz_ds_clip_enabled = 0;
            fprintf(stderr, "[ds] too slow (%.1f fps): last-play clip off\n", fps);
        }
        else if (c.msaa_option != Antialiasing::None) {
            no_msaa = true;
            fprintf(stderr, "[ds] too slow (%.1f fps): antialiasing off\n", fps);
        }
        else if (c.res_option == Resolution::Auto) {
            res_cap = Resolution::Original2x;
            fprintf(stderr, "[ds] too slow (%.1f fps): resolution 2x\n", fps);
        }
        else if (c.res_option == Resolution::Original2x) {
            res_cap = Resolution::Original;
            fprintf(stderr, "[ds] too slow (%.1f fps): original resolution, the last step\n", fps);
        }
        hold_caps();
#ifdef __ANDROID__
        // Only a step that held: one hold_caps refused, saved, would be back to fight it at every launch.
        const GraphicsConfig held = get_graphics_config();
        const bool took = res_rank(held.res_option) <= res_rank(res_cap) && (!no_msaa || held.msaa_option == Antialiasing::None);
        if (took && (no_msaa != was_no_msaa || res_cap != was_cap)) save_limits();
#endif
    }

#ifdef __ANDROID__
    // A run recipe (BlitzRecipe.java), every UI tick in Potato Mode. Its name for a moment, once a game's up to show it over (a
    // status before that is dropped) and past its first frames. ram_panels=1: the RAM pass on panels and menus, the field as chosen.
    static void recipe_update() {
        static bool shown = getenv("BLITZ_RECIPE_SHOW") == nullptr;
        static int wait_ticks = 90;
        if (!shown && ultramodern::is_game_started() && --wait_ticks <= 0) {
            shown = true;
            post_status(getenv("BLITZ_RECIPE_SHOW"));
        }
        static const bool ram_panels = [] { const char* v = getenv("BLITZ_RECIPE_RAM_PANELS"); return v != nullptr && v[0] == '1'; }();
        if (!ram_panels) return;
        static const int field_mode = potato_frame_copy();
        static int last_mode = -1;
        const int mode = blitz_field_up != 0 ? field_mode : 1;
        if (mode != last_mode) {
            last_mode = mode;
            blitz_rt64_render_to_ram(mode);
            fprintf(stderr, "[recipe] RAM pass mode %d (0 when read, 1 always, 2 never): %s\n", mode, blitz_field_up != 0 ? "the field" : "a panel or menu");
        }
    }
#endif

    // Every UI tick (main.cpp).
    void ds_update(SDL_Window* window) {
        if (window == nullptr) return;
        handle_pending_taps();
#ifdef __ANDROID__
        static int splash_ticks = 0;
        if (splash_ticks < 45 && ++splash_ticks == 45) host_hide_splash();
        // The UI ticks whatever the renderer's doing. Frames over a whole 5 s window mean the game's running: a crash on the first
        // draw would have ended the process by now. Presents mean they're reaching the screen: a surface RT64 can't present to lets
        // the game swap at full rate behind a black one, and 30 is more than a stuck GPU's queues can pass off. Setup's screens
        // before the game (Potato Mode's first run too) count: they're the renderer's frames reaching the screen.
        static bool drew = false;
        if (!drew && (fps_last_second() >= 1.0f || firstrun_ui::shown()) && first_presents() >= 30) {
            drew = true;
            host_first_frame();
        }
#endif
        ds_screens_update();
        // The picture's edges (rt64_vi_renderer.cpp, platform/rt64-patches/0032): on two screens, and Potato Mode's one, the game's
        // border columns and a short scissor showed as a line down each side (team, RG DS). 6 px of 1024: about a 1% zoom.
        {
            static const bool lean_trim = getenv("BLITZ_LEAN") != nullptr;
            static const int trim_env = getenv("BLITZ_EDGE_TRIM") ? atoi(getenv("BLITZ_EDGE_TRIM")) : -1;
            blitz_edge_trim_px = trim_env >= 0 ? trim_env : (have_second.load() || lean_trim) ? 6 : 0;
        }
        const bool routes = lower_routes();
        ds_touch_update(window, routes, routes && replay_route(), blitz_port_side(blitz_touch_port()));
        // The loading screens and menus with the game on the other screen: this one has the BLITZ DS title, not black. Under it,
        // while the game boots (started, nothing presented yet): LOADING; on the title and the attract (phase 0 or 7, as
        // hints.cpp reads it) once it's drawing: PRESS START (team: a slow handheld looked hung).
        {
            const char* line = "";
            bool booting = false;
#ifdef __ANDROID__
            const bool started = ultramodern::is_game_started();
            booting = started && first_presents() == 0;
            if (booting) line = "LOADING THE GAME... A FEW SECONDS";
            else if (started && drew && !recompui::is_prompt_open()) {
                if (uint8_t* rdram = monitor_rdram()) {
                    const int32_t ph = MEM_W(0, (gpr)(int32_t)0x802EA420);
                    if (ph == 0 || ph == 7) line = "PRESS START";
                }
            }
#endif
            ds_backdrop_update(window, have_second.load() && (booting || (blitz_ds_mode == 3 && !blitz_ds_panel_first && !stylus_showing() &&
                                                                          !replay_is_open() && !touch_controls_showing())), line);
        }
        // Potato mode's one screen gets the limits too (BlitzLauncher.lean).
        static const bool lean = getenv("BLITZ_LEAN") != nullptr;
        static const bool recipe_run = getenv("BLITZ_RECIPE_NAME") != nullptr;
        if (lean) {
            ds_set_quality();
            hold_caps();
            static bool copy_set = false;
            if (!copy_set) {
                copy_set = true;
                blitz_rt64_render_to_ram(potato_frame_copy());
#ifdef __ANDROID__
                // What this run does: a safe start is one screen, whatever Settings saved (BlitzLauncher.launchDisplay).
                fprintf(stderr, "[potato] two screens %d, sharpness %.2fx, frame copy %d, GPU waits %s\n", two_screens_setting() && getenv("BLITZ_SAFE_START") == nullptr ? 1 : 0, potato_scale(), potato_frame_copy(), potato_narrow_waits() ? "narrow" : "full");
#endif
            }
#ifdef __ANDROID__
            recipe_update();
#endif
        }
        {
            std::lock_guard lock{second_mutex};
            if (second_a != nullptr) ds_second_seen();
            bool want = second_a != nullptr && two_screens_setting();
            if (want && !have_second.load()) {
                SDL_GetWindowSizeInPixels(window, &first_w_now, &first_h_now);
                first_w = first_w_now;
                first_h = first_h_now;
                apply_shapes();
                blitz_ds_set_window(second_a, second_b);
                ds_settings_found();
                have_second = true;
                fprintf(stderr, "[ds] two screens on\n");
                if (const char* panels = getenv("BLITZ_ONE_TOUCH")) fprintf(stderr, "[ds] touch panels: %s; Settings and Replay on the lower screen at play select\n", panels);
                ds_set_quality();
            }
            else if (!want && have_second.load()) {
                have_second = false;
                blitz_ds_mode = 0;
                blitz_ds_set_window(nullptr, nullptr);
                fprintf(stderr, "[ds] two screens off\n");
            }
        }
#ifdef __ANDROID__
        static bool second_seen = false;
        if (have_second.load()) second_seen = true;
        // One screen on a handheld holds its frame rate the same way, starting from the saved settings.
        if (!have_second.load()) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                blitz_ds_clip_enabled = 0;
                auto c = ultramodern::renderer::get_graphics_config();
                fprintf(stderr, "[gfx] saved: resolution %d (0 original, 1 2x, 2 auto), antialiasing %d, refresh %d\n", (int)c.res_option, (int)c.msaa_option, (int)c.rr_option);
            }
            // A Potato Mode run that's had two screens (one gone for a moment is no reason to step down) and a recipe's run don't
            // step at all: Sharpness or the recipe chose, and a slow experiment mustn't be saved for the runs after it.
            hold_frame_rate(blitz_play_mode() == 2 && !replay_is_open() && !recipe_run && !(lean && second_seen));
        }
#endif
        if (!have_second.load()) return;
        if (replay_is_open()) {  // over both screens, with nothing of the panel on it
            blitz_ds_clip_show = 0;
            recompui::ds_panel_set("<div></div>");
            return;
        }
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w != first_w.load() || h != first_h.load() || shapes_changed.exchange(false)) {
            first_w = w;
            first_h = h;
            apply_shapes();
            fprintf(stderr, "[ds] first screen %dx%d, second %dx%d, picture ratio %.3f, upper share %.3f\n", w, h, second_w.load(), second_h.load(), (float)blitz_tall_ratio, (float)blitz_ds_split);
        }

        enum class Show { Title, Drive, Play } show = Show::Title;
        if (ultramodern::is_game_started()) {
            if (blitz_tall_aspect_on != 0.0f) show = Show::Play;
            else if (blitz_play_select_on_screen()) show = Show::Drive;
        }
        static Show last = Show::Title;
        static bool first = true;
        if (first || show != last) {
            log_state(show == Show::Play ? "second screen: the picture" : show == Show::Drive ? "second screen: the drive" : "second screen: title");
            // A cut scene mid-play leaves the picture and comes back: only the end of play select starts a new clip.
            if (last == Show::Drive && show != Show::Drive) blitz_ds_clip_restart = 1;
            if (show == Show::Drive) picking_since = now_ms();
            first = false;
            last = show;
        }

        // Between play select and the snap (the camera's fly-in), a cut scene, the kickoff: a game's still on, so the
        // score and the drive stay up. The title's for when there's no game.
        const bool picking = show == Show::Drive;
        LiveState live{};
        if (show != Show::Play && ultramodern::is_game_started()) {
            uint8_t* rdram = monitor_rdram();
            if (rdram != nullptr) {
                blitz_ds_read_state(rdram, &live);
                // BLITZ_DS_DEMO=1: a drive to look at, for working on this panel.
                static const bool demo = getenv("BLITZ_DS_DEMO") != nullptr;
                if (demo && live.valid) {
                    std::lock_guard lock{drive_mutex};
                    live.spot = 58;
                    live.marker = 68;
                    live.kind = 0;
                    live.points[0] = 14;
                    live.points[1] = 7;
                    snprintf(live.situation, sizeof(live.situation), "2nd & 10 at opp 42");
                    rows = {make_row("1st & 30 at own 25", "12 yd pass", true, 12, 0), make_row("2nd & 18 at own 37", "2 yd run for a loss", true, -2, 0),
                            make_row("3rd & 20 at own 35", "23 yd pass", true, 23, 0), make_row("1st & 30 at opp 42", "incomplete", false, 0, 0),
                            make_row("2nd & 30 at opp 42", "no gain", true, 0, 0), make_row("3rd & 30 at opp 42", "6 yd run", true, 6, 0),
                            make_row("4th & 24 at opp 36", "6 yd run for a loss", true, -6, 0)};
                    drive.team = "ARI";
                    drive.plays = 7;
                    drive.yards = 33;
                    drive.seconds = 38;
                    drive_start = 25;
                }
                if (live.valid) {
                    std::lock_guard lock{drive_mutex};
                    if (time_went_back(live.quarter, live.clock_seconds)) rewind_drives(live.quarter, live.clock_seconds);
                    last_quarter = live.quarter;
                    last_clock = live.clock_seconds;
                    if (picking) snap_situation = live.situation;
                }
            }
        }
        // This screen's width, in the panels' units.
        int full = second_h.load() > 0 ? 1080 * second_w.load() / second_h.load() : 1440;
        if (blitz_ds_panel_first && first_h.load() > 0) full = 1080 * first_w.load() / first_h.load();
        // A narrow or portrait screen has no room for the clip and the drive side by side, and RT64 only draws it on the second.
        bool clip = picking && live.valid && blitz_ds_clip_enabled && blitz_ds_clip_frames > 0 && full >= 1200 && !blitz_ds_panel_first;
        blitz_ds_clip_show = clip ? 1 : 0;
        // Game + Companion and Stylus: the clip's a 4:3 one, kept while the picture's on one screen, in the corner. Both Screens:
        // the tall one, in the left third. A switch starts it over, so neither shape shows in the other's place.
        {
            const int corner = blitz_ds_screen_mode != 0 ? 1 : 0;
            if (corner != blitz_ds_clip_right) {
                blitz_ds_clip_right = corner;
                blitz_ds_clip_restart = 1;
            }
        }
        // Stylus in play has the icons too, over the picture on this window. Nothing else of a panel shows there.
        int stylus_w = 0, stylus_h = 0;
        const bool stylus_icons = blitz_ds_icons_on_second(&stylus_w, &stylus_h) != 0;
        blitz_ds_second_icons = stylus_icons ? 1 : 0;
        if (show == Show::Play || stylus_icons) {
            recompui::ds_panel_set(panel_badges());
            std::lock_guard lock{badges_mutex};
            std::array<recompui::DsMover, 3> movers;
            for (size_t k = 0; k < badges.size(); k++) {
                movers[k] = {badges[k].on && badges[k].screen == 1, badges[k].x, badges[k].y};
            }
            recompui::ds_panel_move(movers.data(), (int)movers.size());
        }
        else if (live.valid) recompui::ds_panel_set(panel_drive(clip, full, live) + second_mark(full));
        else recompui::ds_panel_set(panel_title(full) + second_mark(full));

        // Still called with stepping off: it loads the limits and holds the caps. In Potato Mode Sharpness chose, as a recipe does.
        hold_frame_rate(show == Show::Play && !recipe_run && !lean);

        using clock = std::chrono::steady_clock;
        static clock::time_point next = clock::now() + std::chrono::seconds(5);
        static uint64_t frames_before = 0;
        if (clock::now() >= next) {
            uint64_t frames = blitz_ds_second_frames;
            fprintf(stderr, "[ds] second screen %.1f fps, last draw %.1f ms, %llu failures, mode %d, clip %d frames\n", (double)(frames - frames_before) / 5.0, (double)blitz_ds_second_ms, (unsigned long long)blitz_ds_second_failures, (int)blitz_ds_mode, (int)blitz_ds_clip_frames);
            frames_before = frames;
            next = clock::now() + std::chrono::seconds(5);
        }
    }
}
