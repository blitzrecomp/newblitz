// Tall view experiment (branch tall-view): a 2:3 picture for two stacked 4:3 screens, Sonic Rush style.
// BLITZ_TALL=<settings file> turns it on; BLITZ_WINDOW_SIZE=720x1080 gives the window that shape. A window taller than
// 2:3 (640x1032: two 640x480 and 72 between) gets a hinge: RT64 parts the picture's two halves around it (platform/
// rt64-patches/0009), so it hides nothing. Off, every hook here returns what it was given.
//
// RT64 draws a 2:3 frame (platform/rt64-patches/0008, only while blitz_tall_aspect_on is set) and narrows every
// full-width projection by the same factor, which would crop the
// sides. Widening the game's vertical field of view by that factor keeps the sides where they were and adds the
// extra picture above and below. Only what's drawn changes (gfx_load_camera's local copy of the camera), so game
// logic keeps its own camera.
//
// The settings file is optional and reread while the game runs, so the camera can be moved without a rebuild:
//   k 2        vertical view multiplier against the game's 4:3
//   raise 40   lift the eye
//   back 0     pull the eye back along the view, on the ground plane
//   look 70    push the point looked at forward along the view, on the ground plane
//   far 6000   far plane, when above the game's own
//   auto 1     the following camera below (0: only raise, back and look)
//   pline 35   its tilt down from level, degrees, at the line
//   pread 45   while the quarterback looks for a receiver
//   pcarry 35  with the ball in the air and on a carrier
//   near 165   its distance at the line
//   wide 320   its farthest distance, sideline to sideline
//   hold 170   its distance with the ball in the air and on a carrier
//   shift 0.4  at the line and on a carrier, how far down the picture the play sits (0 the middle, 1 the bottom edge),
//              to keep it off the hinge
//   score 0    which screen the scoreboard is on: 0 top, 1 bottom
//   board 1.35 the scoreboard's size against the rest of the 2D, from its screen's top left corner
//   mark 18    height above the ground of the markers over the players' heads
//   banner 0.12 how far down its screen the 2D on the upper screen moves (the share of the screen)
//   tilt 0.65  how far down the picture the game's own aim sits (0 the middle, 1 the bottom edge)
//   sky 28     how far above level the picture's top edge may look (degrees)
//   zoom 1.15  the game's camera, this much further back, always
//   across 1  two screens of different widths (a wide upper screen over a 4:3 lower one): 1, the narrower screen spans
//             the game's own view across, as one screen would, and the wider one sees more at its sides; 0, the wider
//             spans it and the narrower shows only its middle (cutting off wide receivers at the line)
//   skyfit 0  with across, a view taller than two 4:3 screens' gives its extra height to field, not sky (1; see the camera)
//   wide_out 0, wide_in 0.025  per frame, how quickly the throw's wide view comes and goes (0: it doesn't; the team, trying
//              the game's own camera only)
// The live command "TALL <name> <value>" sets these too (dev_input.cpp).
//
// The camera (team, 2026-11-01): the game's own, tilted up, so the action's on the bottom screen and the top one previews
// the field ahead. Only while the quarterback reads, or the ball's in the air, does it open over both screens to hold his
// receivers (looking downfield, so the field stays upright), easing back slowly after a catch.
//
// 2D (the game's sprite space: x 0-512, y 0-384 upward): each sprite the game draws during tall play is halved for
// this frame's draw, so the 4:3 layout fits one screen. The player bars go to the bottom screen, the scoreboard to the
// one chosen, the rest (banners, panels) to the top. The markers over the players (ARROW_*) are placed by the game
// through its own camera, so they're carried over: back through its camera to their height, forward through ours.
// Game code never sees any of it: blitz_tall_sprite_end restores the object before sprite_draw returns.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

#include "recomp.h"
#include "recompui/recompui.h"
#include "SDL2/SDL.h"

extern "C" volatile float blitz_tall_aspect_on;  // rt64_workload_queue.cpp (platform/rt64-patches/0008)
extern "C" int blitz_play_select_on_screen(void);  // gfx_ext.cpp
extern "C" int blitz_ds_active(void);  // ds_view.cpp
extern "C" volatile int blitz_ds_panel_first;  // ui_state.cpp
extern "C" volatile int blitz_ds_clip_hold;    // rt64_present_queue.cpp
extern "C" { volatile int blitz_ds_screen_mode = 0; }  // Settings → General → DS Screens: 0 both, 1 game + companion, 2 stylus, 3 controller
extern "C" { volatile int blitz_field_up = 0; }  // the field's on screen, nothing full width over it (ds_view.cpp's recipe_update)
extern "C" int blitz_sprite_full_width(uint8_t* rdram, int32_t node);  // gfx_ext.cpp
extern "C" float blitz_ds_k(void);
extern "C" void blitz_ds_bracket(float x, float y);
extern "C" volatile int blitz_ds_second_on_top;
extern "C" int blitz_ds_first_part(float* top, float* bottom);
extern "C" float blitz_ds_width_share(int first_window);  // ds_view.cpp
extern "C" volatile int blitz_ds_mode;  // rt64_present_queue.cpp

namespace {
    constexpr uint32_t gameplay_camera = 0x80318CB0;  // ovl_a camera callback while a game is on
    constexpr int32_t camera_callback = (int32_t)0x8002B6A0;
    constexpr float pi = 3.14159265f;

    // Sprite list node and object fields (gfx_ext.cpp).
    constexpr int node_obj = 0x08, node_group = 0x14;
    constexpr int obj_x = 0x24, obj_y = 0x28, obj_scale_x = 0x54, obj_scale_y = 0x58;
    constexpr uint32_t group_player_bars = 0x708;
    constexpr uint32_t groups_scoreboard[] = {0x702, 4, 5, 6};
    constexpr uint32_t group_punt_meter = 0x32B;
    constexpr uint32_t group_score_panel = 0x701, group_score_words = 9;  // SP_* plates; team names and scores

    struct Vec { float x, y, z; };
    Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    Vec operator*(Vec a, float b) { return {a.x * b, a.y * b, a.z * b}; }
    float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    Vec unit(Vec a) { return a * (1.0f / std::sqrt(dot(a, a))); }

    struct View {
        Vec eye, f, r, u;
        void set(Vec e, Vec at) {
            eye = e;
            f = unit(at - e);
            r = unit(cross(f, Vec{0, 1, 0}));
            u = cross(r, f);
        }
    };
    View game_view, tall_view;
    bool have_views = false;
    float game_tan = 0.0f;  // the game's tan(fov / 2)
    constexpr float game_aspect = 4.0f / 3.0f;

    const char* settings_path = getenv("BLITZ_TALL");
    bool tall_on() { return settings_path != nullptr || blitz_ds_active(); }
    // A recipe's ram_panels (BlitzRecipe.java) needs the field told from a panel on one screen too.
    bool watching_field() {
        static const bool on = [] { const char* v = getenv("BLITZ_RECIPE_RAM_PANELS"); return v != nullptr && v[0] == '1'; }();
        return on;
    }
    float auto_on = 1.0f, pitch_line = 35.0f, pitch_read = 45.0f, pitch_carry = 35.0f, dist_near = 165.0f, dist_wide = 320.0f, dist_hold = 170.0f;
    float board_size = 1.35f;
    float k = 2.0f, shift = 0.4f, eye_lift = 0.0f, back = 0.0f, look = 0.0f, far_plane = 6000.0f, score_bottom = 0.0f, mark_height = 18.0f;
    // TALL across (see the top): how much of the narrower screen's cut the camera makes up, and the view's widening for it
    // (1 when the screens are the same width), set each frame from the screens' width shares.
    float across = 1.0f, widen = 1.0f, sky_fit = 0.0f;
    float tilt = 0.65f, wide_out = 0.0f, wide_in = 0.025f, banner_drop = 0.12f, zoom = 1.15f, sky_limit = 28.0f;
    float log_sprites = 0.0f;
    // Two-screen banner places, live with TALL result_dx / result_line / panel / msg_up (tuned on a Surface Duo, 2026-11-26).
    float panel_words = 0.0f, card = 0.0f;  // TALL panel_words: 1ST AND 30's words against its plate; card: the rules and tip cards
    float result_dx = 0.0f, result_line = 280.0f, panel_lift = -29.0f, msg_up = 10.0f, pause_up = -3.0f, panel_low = 0.0f, panel_low_y = -28.0f, meter_mode = 0.0f, score_lift = 8.0f;
    bool paused_this_frame = false, paused_last_frame = false;  // the pause menu drew (group 0x5F07)
    bool score_plates_this_frame = false, score_plates_last_frame = false;  // the score panel's plates drew (group 0x701)
    constexpr uint32_t call_frames = 0x800, tall_frames = 0x810, call_words = 0x34;  // 0x810: the returner's panel
    // FUMBLE's box (a built image, 186x34 at 256,344 under the word's 10jFUMBLE at 353) is group 0x802, not 0x800: taken for
    // other 2D, it went down with the banners while its word stayed put, so the word sat over an empty box (the team's Duo,
    // 2026-11-27). It's a call's box like the others: measured and moved with them.
    constexpr uint32_t fumble_frames = 0x802;
    float call_range = 70.0f, call_shift = 0.0f, call_y = 0.0f, msg_shift = 0.0f, msg_low = 1e9f, msg_high = -1e9f;
    bool call_up = false;  // a call's panel is up
    std::array<float, 16> frame_ys{};
    int frame_count = 0, frame_anchor = 0;

    // Every frame: where last frame's upper frames were, as one block around the middle of its screen.
    void measure_call() {
        float high = -1e9f, low = 1e9f;
        for (int i = 0; i < frame_count; i++) high = std::max(high, frame_ys[(size_t)i]);
        for (int i = 0; i < frame_count; i++) {
            if (high - frame_ys[(size_t)i] <= 110.0f) low = std::min(low, frame_ys[(size_t)i]);
        }
        call_up = frame_count > 0 && high > 192.0f;
        if (call_up) {
            call_y = (high + low) * 0.5f;
            call_shift = call_y - 192.0f;
            call_range = std::max({70.0f, (high - low) * 0.5f + 40.0f, (float)frame_anchor * 2.0f});
        }
        frame_count = 0;
        frame_anchor = 0;
    }
    bool directions_up = false;
    float directions_x = 0.0f, directions_half = 0.0f, directions_y = 0.0f, directions_range = 0.0f;
    int calls = 0;
    int full_art_frames = 0, full_art_run = 0;
    bool full_art_seen = false;
    bool active = false;  // this camera load

    void set_setting(const char* name, float value) {
        if (!strcmp(name, "k")) k = value;
        else if (!strcmp(name, "shift")) shift = value;
        else if (!strcmp(name, "board")) board_size = value;
        else if (!strcmp(name, "raise")) eye_lift = value;
        else if (!strcmp(name, "back")) back = value;
        else if (!strcmp(name, "look")) look = value;
        else if (!strcmp(name, "far")) far_plane = value;
        else if (!strcmp(name, "score")) score_bottom = value;
        else if (!strcmp(name, "auto")) auto_on = value;
        else if (!strcmp(name, "pline")) pitch_line = value;
        else if (!strcmp(name, "pread")) pitch_read = value;
        else if (!strcmp(name, "pcarry")) pitch_carry = value;
        else if (!strcmp(name, "near")) dist_near = value;
        else if (!strcmp(name, "wide")) dist_wide = value;
        else if (!strcmp(name, "hold")) dist_hold = value;
        else if (!strcmp(name, "mark")) mark_height = value;
        else if (!strcmp(name, "tilt")) tilt = value;
        else if (!strcmp(name, "banner")) banner_drop = value;
        else if (!strcmp(name, "zoom")) zoom = value;
        else if (!strcmp(name, "sky")) sky_limit = value;
        else if (!strcmp(name, "log")) log_sprites = value;
        else if (!strcmp(name, "wide_out")) wide_out = value;
        else if (!strcmp(name, "wide_in")) wide_in = value;
        else if (!strcmp(name, "result_dx")) result_dx = value;
        else if (!strcmp(name, "result_line")) result_line = value;
        else if (!strcmp(name, "panel")) panel_lift = value;
        else if (!strcmp(name, "msg_up")) msg_up = value;
        else if (!strcmp(name, "pause_up")) pause_up = value;
        else if (!strcmp(name, "panel_low")) panel_low = value;
        else if (!strcmp(name, "panel_low_y")) panel_low_y = value;
        else if (!strcmp(name, "meter")) meter_mode = value;
        else if (!strcmp(name, "score_lift")) score_lift = value;
        else if (!strcmp(name, "panel_words")) panel_words = value;
        else if (!strcmp(name, "card")) card = value;
        else if (!strcmp(name, "across")) across = value;
        else if (!strcmp(name, "skyfit")) sky_fit = value;
        else fprintf(stderr, "[tall] no setting %s\n", name);
    }

    void reload() {
        if (settings_path == nullptr) return;
        FILE* f = fopen(settings_path, "r");
        if (f == nullptr) return;
        char name[32];
        float value;
        while (fscanf(f, "%31s %f", name, &value) == 2) set_setting(name, value);
        fclose(f);
    }

    // Upper case, as want is: the game's names come in both (ARROW_YELLOW.WMs).
    int memcmp_game(uint8_t* rdram, int32_t addr, const char* want, int n) {
        for (int i = 0; i < n; i++) {
            char c = (char)MEM_B(i, addr);
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (c != want[i]) return 1;
        }
        return 0;
    }

    float get_float(uint8_t* rdram, int32_t addr) {
        uint32_t bits = (uint32_t)MEM_W(0, addr);
        float out;
        memcpy(&out, &bits, 4);
        return out;
    }

    void put_float(uint8_t* rdram, int32_t addr, float value) {
        uint32_t bits;
        memcpy(&bits, &value, 4);
        MEM_W(0, addr) = (int32_t)bits;
    }
}

namespace { void follow_frame_begin(); }

extern "C" int blitz_tall_2d(void) { return blitz_tall_aspect_on != 0.0f; }
namespace blitz {
    void tall_set(const std::string& name, float value) {  // the live command TALL
        set_setting(name.c_str(), value);
        fprintf(stderr, "[tall] %s = %.3f\n", name.c_str(), value);
    }
}

// A point on the game's own 4:3 picture (nx, ny -1 to 1, up positive): back through its camera to the given height
// above the ground, up by lift, forward through the tall camera. Out: the tall picture, -1 to 1. 0 if it's behind.
extern "C" int blitz_tall_carry(float nx, float ny, float height, float lift, float* tx, float* ty) {
    if (!have_views || game_tan <= 0.0f) return 0;
    Vec dir = game_view.f + game_view.r * (nx * game_tan * game_aspect) + game_view.u * (ny * game_tan);
    if (dir.y >= -0.0001f) return 0;
    Vec point = game_view.eye + dir * ((height - game_view.eye.y) / dir.y);
    point.y += lift;
    Vec v = point - tall_view.eye;
    float z = dot(v, tall_view.f);
    if (z <= 1.0f) return 0;
    *tx = dot(v, tall_view.r) / (z * game_tan * widen * game_aspect);
    *ty = dot(v, tall_view.u) / (z * game_tan * widen * k);
    return 1;
}

// Where a height on the 2:3 picture (0 top, 1 bottom) lands in a window that may part its halves around a hinge.
extern "C" float blitz_tall_window_y(float picture_y, int w, int h) {
    float picture = w * 1.5f;
    if (h <= picture) return (h - picture) * 0.5f + picture * picture_y;
    return picture * picture_y + (picture_y < 0.5f ? 0.0f : h - picture);
}

// Every frame (gfx_build_frame): the tall shape only while the gameplay camera is up and play select isn't.
extern "C" void blitz_tall_frame_begin(uint8_t* rdram) {
    if (!tall_on()) {
        blitz_tall_aspect_on = 0.0f;
        if (watching_field()) {  // one screen: the same reading as below, with nothing else that depends on it
            full_art_run = full_art_seen ? full_art_run + 1 : 0;
            full_art_seen = false;
            blitz_field_up = (uint32_t)MEM_W(0, camera_callback) == gameplay_camera && !blitz_play_select_on_screen() && full_art_run < 4 ? 1 : 0;
        }
        return;
    }
    bool gameplay = (uint32_t)MEM_W(0, camera_callback) == gameplay_camera;
    // Full-width art up (halftime, stats, a door closing): that screen as it is, not halved and stretched into the tall view.
    // A few frames in a row, so nothing passing through flips the whole view.
    full_art_run = full_art_seen ? full_art_run + 1 : 0;
    full_art_seen = false;
    if (full_art_run >= 4) full_art_frames = 3;
    if (full_art_frames > 0) full_art_frames--;
    blitz_field_up = gameplay && !blitz_play_select_on_screen() && full_art_frames == 0 ? 1 : 0;
    blitz_ds_clip_hold = MEM_W(0, (gpr)(int32_t)0x8028BFC0) != 0 ? 1 : 0;  // the pause menu's up: not part of the last play
    msg_shift = msg_high >= msg_low ? (msg_low + msg_high) * 0.5f - 192.0f : 0.0f;
    msg_low = 1e9f;
    msg_high = -1e9f;
    measure_call();
    directions_up = false;
    // DS Screens: the tall view is only for "both screens".
    const int screens = blitz_ds_active() ? blitz_ds_screen_mode : 0;
    blitz_tall_aspect_on = gameplay && !blitz_play_select_on_screen() && full_art_frames == 0 && screens == 0 ? 1.0f : 0.0f;
    if (blitz_ds_active()) {
        k = blitz_ds_k();
        // A screen narrower than the picture shows its middle: the view widens so that screen still spans the game's own
        // view across (TALL across), the same in both directions, so each screen keeps one scale and the halves still meet.
        // With a wide upper screen over a 4:3 lower one the lower then frames the play as one 4:3 screen does (and as two
        // screens of one width do), and the upper adds field at its sides. Same widths: 1, as before.
        const float narrow = std::min(blitz_ds_width_share(1), blitz_ds_width_share(0));
        widen = (narrow > 0.05f && narrow < 0.999f) ? 1.0f + std::max(across, 0.0f) * (1.0f / narrow - 1.0f) : 1.0f;
        // Over both in play. Otherwise the game on the upper screen, but play select on the lower, where a thumb picks.
        bool want_upper = !blitz_play_select_on_screen();
        blitz_ds_mode = blitz_tall_aspect_on != 0.0f ? 1 : (want_upper == (blitz_ds_second_on_top != 0)) ? 3 : 2;
        // Stylus with the first window on top: the wheel only draws on the first window, so the game takes the other.
        if (screens == 2 && blitz_ds_second_on_top == 0) blitz_ds_mode = 3;
        // Game + companion: with the game on the second window, the first has the drive board. So does play select there,
        // which is only with the first window on top.
        blitz_ds_panel_first = blitz_ds_mode == 3 && (screens == 1 || blitz_play_select_on_screen()) ? 1 : 0;
        // Controller (team, 2026-11-26): the game always on the upper screen, the lower one the touch controller alone.
        if (screens == 3) {
            blitz_ds_mode = blitz_ds_second_on_top != 0 ? 3 : 2;
            blitz_ds_panel_first = 0;
        }
    }
    paused_last_frame = paused_this_frame;
    paused_this_frame = false;
    score_plates_last_frame = score_plates_this_frame;
    score_plates_this_frame = false;
    follow_frame_begin();
}

namespace {
    int32_t saved_obj = 0;
    float saved[4];
}

// sprite_draw 0x8001337C, a1 = the sprite list node.
extern "C" void blitz_tall_sprite_begin(uint8_t* rdram, int32_t node) {
    saved_obj = 0;
    if ((tall_on() || watching_field()) && blitz_sprite_full_width(rdram, node)) full_art_seen = true;
    // The doors closing at a quarter's end: a little short of a screen's art, and a whole screen's business all the same.
    if (tall_on()) {
        int32_t door_img = MEM_W(0x0C, MEM_W(node_obj, node));
        if (door_img != 0 && !memcmp_game(rdram, door_img + 0x0E, "DOOR", 4)) full_art_frames = 3;
    }
    // Play select's cursor is a pair of brackets drawn over the tile: the one place its position is sure in every game.
    if (blitz_ds_active() && blitz_play_select_on_screen()) {
        int32_t bracket_obj = MEM_W(node_obj, node);
        int32_t bracket_img = MEM_W(0x0C, bracket_obj);
        if (bracket_img != 0 && !memcmp_game(rdram, bracket_img + 0x0E, "BRCKTTOP", 8)) {
            blitz_ds_bracket(get_float(rdram, bracket_obj + obj_x), get_float(rdram, bracket_obj + obj_y));
        }
    }
    if (!tall_on() || !blitz_tall_2d()) return;
    int32_t obj = MEM_W(node_obj, node);
    uint32_t group = (uint32_t)MEM_W(node_group, node);
    bool scoreboard = false;
    for (uint32_t g : groups_scoreboard) scoreboard = scoreboard || group == g;
    if (group == 0x5F07) paused_this_frame = true;
    // The down and distance (1ST AND 30 between plays) and the score panel after a score (groups 9 and 0x701): the lower
    // screen's bottom, where one screen has the down and distance (team, 2026-11-26); TALL panel_low 0 puts them back on the
    // upper screen's bottom.
    // The score panel after a score (its SP_ plates, group 0x701) shares group 9 with the down and distance: when the plates
    // drew last frame, group 9 is the panel's names and scores and goes up with it, out from under the thumbs (team,
    // 2026-11-27); otherwise it's the down and distance, at the lower screen's bottom as one screen has it.
    // Only the score panel's own plates (its logo, quarter and clock) say it's up: the down and distance's plate is SP_ALL too,
    // and counting that sent 1ST AND 30 up with the rules board (team, 2026-11-27).
    if (group == group_score_panel) {
        const int32_t sp_img = MEM_W(0x0C, obj);
        if (sp_img != 0 && (!memcmp_game(rdram, sp_img + 0x0E, "SP_LOGO", 7) || !memcmp_game(rdram, sp_img + 0x0E, "SP_QTR", 6) ||
                            !memcmp_game(rdram, sp_img + 0x0E, "SP_CLOCK", 8))) score_plates_this_frame = true;
    }
    const bool score_panel = group == group_score_panel || group == group_score_words;
    // Which of the two it is, from the piece itself where it can be: the down and distance's words are the 10j font (10j...1ST
    // AND 30), the score panel's names and scores 8j and 9j; its plates other than SP_ALL are its own. Only the plate the two
    // share (SP_ALL) goes by whether the panel's own plates drew last frame.
    bool down_banner = false;
    if (score_panel) {
        const int32_t piece = MEM_W(0x0C, obj);
        if (piece == 0) down_banner = !score_plates_last_frame;
        // A text piece's name starts with an escape (1B, then 10j-256c1ST AND 30): read past it. Looked for at +0x0E, "10j"
        // never matched, so the words went with the score panel's dial, apart from their plate (team, 2026-11-27).
        else if (group == group_score_words) {
            const int32_t name = piece + 0x0E + ((uint8_t)MEM_B(0x0E, piece) == 0x1B ? 1 : 0);
            down_banner = !memcmp_game(rdram, name, "10J", 3);
        }
        else down_banner = !memcmp_game(rdram, piece + 0x0E, "SP_ALL", 6) && !score_plates_last_frame;
    }
    bool bottom = group == group_player_bars || (scoreboard && score_bottom != 0.0f) || (down_banner && panel_low != 0.0f);
    // The punt meter (group 0x32B: PUNTGRID, PUNTSTEM, PUNTARROW and the WIND plate, seen 2026-11-04): the upper screen's lower
    // left corner, clear of the touch controls on the lower one (team, 2026-11-26; it was the lower screen's). The kick meter's
    // names are a guess, kept in case they're right.
    int32_t meter_img = MEM_W(0x0C, obj);
    // The big messages (TOUCHDOWN!, 1ST DOWN, SAFETY: MSG_ images) and their sparks share the panels' group (0x800). They're
    // placed from the game's own layout, not measured with the panels: measured, a message showed for a frame where the game
    // put it and then jumped (team, 2026-11-27).
    const bool big_message = meter_img != 0 && (!memcmp_game(rdram, meter_img + 0x0E, "MSG_", 4) || !memcmp_game(rdram, meter_img + 0x0E, "SPARK", 5));
    const bool meter_seen = group == group_punt_meter ||
                       (meter_img != 0 && (!memcmp_game(rdram, meter_img + 0x0E, "K_METER", 7) || !memcmp_game(rdram, meter_img + 0x0E, "K_BALL", 6) || !memcmp_game(rdram, meter_img + 0x0E, "METER", 5)));
    // TALL meter 0: the upper screen's lower left; 1: as any other 2D; 2: the lower screen's lower left (as before 11-26).
    const bool meter = meter_seen && meter_mode != 1.0f;
    if (meter && meter_mode == 2.0f) bottom = true;
    float obj_y_now = get_float(rdram, obj + obj_y);
    int anchor_y = (int)MEM_H(0xAA, obj);
    // The narrow panel a kick returner gets (PLAYER 1 HAS BALL: tall, not wide like the rules): the bottom screen, with its
    // words.
    float obj_x_now = get_float(rdram, obj + obj_x);
    int anchor_x = (int)MEM_H(0xA8, obj);
    if ((group == call_frames || group == tall_frames) && anchor_y >= 60 && anchor_x < 130) {
        directions_x = obj_x_now;
        directions_half = (float)anchor_x + 20.0f;
        directions_y = obj_y_now;
        directions_range = (float)anchor_y * 2.0f;
        directions_up = true;
    }
    bool directions = directions_up && (group == call_frames || group == tall_frames || group == call_words) && std::fabs(obj_y_now - directions_y) <= directions_range &&
                      std::fabs(obj_x_now - directions_x) <= directions_half + (group == call_words ? 60.0f : 0.0f);
    // Frames of a call, measured this frame and moved next: one panel, or several images (END OF over 1ST QUARTER).
    const bool call_box = group == call_frames || group == tall_frames || group == fumble_frames;
    if (call_box && !directions && !big_message && frame_count < (int)frame_ys.size()) {
        frame_ys[(size_t)frame_count++] = obj_y_now;
        frame_anchor = std::max(frame_anchor, anchor_y);
    }
    if (directions) bottom = true;
    saved[0] = get_float(rdram, obj + obj_x);
    saved[1] = get_float(rdram, obj + obj_y);
    saved[2] = get_float(rdram, obj + obj_scale_x);
    saved[3] = get_float(rdram, obj + obj_scale_y);
    if (log_sprites != 0.0f || getenv("BLITZ_TALL_LOG")) {
        static uint32_t seen[512];
        int32_t img = MEM_W(0x0C, obj);
        uint32_t id = group ^ ((uint32_t)img << 8);
        bool known = false;
        for (uint32_t v : seen) known = known || v == id;
        static int count = 0;
        if (!known && count < 512) {
            seen[count++] = id;
            char name[17] = {};
            for (int i = 0; img && i < 16; i++) name[i] = (char)MEM_B(0x0E + i, img);
            fprintf(stderr, "[tall2d] group %X %s x %.1f y %.1f sx %.2f sy %.2f anchor %d %d\n", group, name, saved[0], saved[1], saved[2], saved[3], (int)MEM_H(0xA8, obj), (int)MEM_H(0xAA, obj));
        }
    }
    // One screen's share of the picture: as wide as it, and no taller than its screen (they can differ: Retroid Pocket Duo).
    float scale = 1.0f / k;
    float part_top, part_bottom;
    if (blitz_ds_first_part(&part_top, &part_bottom)) {
        float upper_share = blitz_ds_second_on_top ? part_top : part_bottom;
        scale = std::min(scale, bottom ? 1.0f - upper_share : upper_share);
        // A screen narrower than the picture (a 4:3 one under a wide one) shows only its middle: its 2D shrinks to fit it.
        const bool first_window = bottom == (blitz_ds_second_on_top != 0);
        scale *= blitz_ds_width_share(first_window ? 1 : 0);
    }
    // Everything shrinks about the middle: the shrunken 512-wide space shows only its middle half across the picture, so a
    // place measured from the left edge (the kick meter's was) lands off the screen. The meter never showed for that.
    float placed_x = 256.0f + (saved[0] - 256.0f) * scale;
    float placed_y = saved[1] * scale + (bottom ? 0.0f : 384.0f * (1.0f - scale));
    if (down_banner && bottom) placed_y += panel_low_y;  // almost on the lower screen's bottom edge (team): TALL panel_low_y
    float size = scale;
    // A call's panel (INCOMPLETE, NO GAIN: frames group 0x800, words 0x34) in the upper half of the game's picture: the
    // middle of its screen, all of it by the same amount. So too the one-word images (1ST DOWN, 1ST QUARTER: MSG_*), the
    // first of them each frame setting it for the others. Other 2D: down a little, clear of the scoreboard.
    if (directions) placed_y -= (directions_y - 192.0f) * scale;
    else if (!scoreboard && !bottom && !meter) {
        int32_t call_img = MEM_W(0x0C, obj);
        if (big_message && !(call_up && std::fabs(saved[1] - call_y) <= call_range)) {
            // On its own: where the game puts it, moved up the same from the first frame (the messages sit around y 262), a
            // little high on the upper screen so the score panel under it stays clear (TALL msg_up). In a panel's box (FUMBLE
            // over its plate, perhaps): with the box, below.
            placed_y -= (262.0f - 192.0f) * scale;
            placed_y += msg_up;
        }
        else if (call_box || group == call_words) {
            if (call_up && std::fabs(saved[1] - call_y) <= call_range) {
                // A play's result (16 YARD PASS over 1ST DOWN AND 30) is high on the game's screen: the upper screen's top right,
                // where one screen has it, out of the field's way (team, 2026-11-26). Lower panels (the rules) still centre.
                if (call_y >= result_line) placed_x += result_dx;
                else placed_y += -call_shift * scale + card;  // the rules and tip cards: centred, then TALL card (team, 2026-11-27)
            }
        }
        else if (call_img != 0 && !memcmp_game(rdram, call_img + 0x0E, "MSG_", 4)) {
            // Together, by last frame's extent: END OF over 1ST QUARTER are three images on two lines.
            msg_low = std::min(msg_low, saved[1]);
            msg_high = std::max(msg_high, saved[1]);
            // From the game's own layout, not last frame's: measured, a message showed in one place for a frame and then
            // jumped (TOUCHDOWN!, the team, 2026-11-27). They sit around y 262 (247-271); END OF over 1ST QUARTER keep their lines.
            placed_y -= (262.0f - 192.0f) * scale;
            placed_y += msg_up;  // a little high on the upper screen, clear of the score panel under it (TALL msg_up)
        }
        // The score panel after a score and the down-and-distance one (groups 0x701 and 9): at the bottom of the upper screen,
        // centred, just clear of the hinge (team, 2026-11-26; it was lifted 16 for the hinge, 2026-11-04).
        // Two offsets: the down and distance (1ST AND 30) sits low (the team's pick, panel), the score panel after a score is taller
        // and keeps clear of the hinge higher up (score_lift; it shared panel's and split at the hinge, 2026-11-27).
        // The down and distance's words have their own nudge against its plate (TALL panel_words, the team 2026-11-27).
        else if (group == group_score_panel || group == group_score_words)
            placed_y += (down_banner ? panel_lift + (group == group_score_words ? panel_words : 0.0f) : score_lift) * scale;
        // While the pause menu's up (TIME OUT, group 0x5F07, seen last frame), all of it goes up together: its labels are
        // another group than its plates, so lifting one group alone parted them (team, 2026-11-26).
        else placed_y += paused_last_frame ? pause_up : -banner_drop * 384.0f * scale;
    }
    if (meter_seen) {  // where a kick meter lands, now and then (it hasn't shown on two screens: the team's Duo, 2026-11-27)
        static int said = 0;
        if (said++ % 120 == 0) fprintf(stderr, "[tall] meter group %X at %.0f,%.0f -> %.0f,%.0f scale %.2f (mode %.0f, bottom %d)\n", group, saved[0], saved[1], placed_x, placed_y, size, meter_mode, bottom ? 1 : 0);
    }
    put_float(rdram, obj + obj_x, placed_x);
    put_float(rdram, obj + obj_y, placed_y);
    put_float(rdram, obj + obj_scale_x, saved[2] * size);
    put_float(rdram, obj + obj_scale_y, saved[3] * size);
    saved_obj = obj;

    int32_t img = MEM_W(0x0C, obj);
    bool marker = img != 0 && !memcmp_game(rdram, img + 0x0E, "ARROW_", 6);
    float tx, ty;
    if (marker && blitz_tall_carry((saved[0] - 256.0f) / 256.0f, (saved[1] - 192.0f) / 192.0f, mark_height, 0.0f, &tx, &ty)) {
        put_float(rdram, obj + obj_x, 256.0f + tx * 256.0f * scale);
        put_float(rdram, obj + obj_y, 192.0f + ty * 192.0f);
    }
}

// sprite_draw 0x80013DB8, its only way out.
extern "C" void blitz_tall_sprite_end(uint8_t* rdram) {
    if (saved_obj == 0) return;
    put_float(rdram, saved_obj + obj_x, saved[0]);
    put_float(rdram, saved_obj + obj_y, saved[1]);
    put_float(rdram, saved_obj + obj_scale_x, saved[2]);
    put_float(rdram, saved_obj + obj_scale_y, saved[3]);
    saved_obj = 0;
}

namespace {
    // Game state (receiver_targets.cpp has the notes).
    constexpr int32_t players = (int32_t)0x802D1590;
    constexpr int player_size = 0xB98;
    constexpr int32_t play_phase = (int32_t)0x802EA4A8;  // 1 at the line, 2 live, 3 after the whistle
    constexpr int32_t passer = (int32_t)0x802EA4C0;      // who may throw; -1 with the ball in the air
    constexpr int32_t ball_owner = (int32_t)0x80364D20;
    constexpr int32_t offense = (int32_t)0x802EA47C;     // bit 0: the team in records 7-13

    // One step of the following camera per frame.
    bool stepped = false;
    void follow_frame_begin() { stepped = false; }
    float follow_mix = 0.0f;    // 0 the game's camera, 1 the wide view
    bool open_wide = false;     // the quarterback reading, or his throw in the air
    bool read_this_play = false;
    float follow_dist = 165.0f;
    float follow_shift = 0.0f;
    float follow_pitch = 35.0f;
    float scrimmage = 0.0f;     // along x, set at the line
    Vec follow_center{0, 0, 0};
    bool follow_valid = false;
    float downfield = 1.0f;     // along x

    constexpr int player_slot = 0xB20;                   // who controls him (receiver_targets.cpp): 0-3 a controller's slot
    constexpr int32_t slot_of_pad = (int32_t)0x8002AD94;

    // Anyone a controller is on, so the camera never leaves the player you're playing as (on defense, above all).
    int humans_at(uint8_t* rdram, Vec* out, int room) {
        int n = 0;
        for (int i = 0; i < 14 && n < room; i++) {
            int slot = MEM_W(player_slot, players + i * player_size);
            if (slot < 0 || slot > 3) continue;
            for (int port = 0; port < 4; port++) {
                if (MEM_W(0, slot_of_pad + 4 * port) == slot) {
                    out[n++] = Vec{get_float(rdram, players + i * player_size), 0.0f, get_float(rdram, players + i * player_size + 8)};
                    break;
                }
            }
        }
        return n;
    }

    Vec f_preview_shift(float amount, float dist) {
        return Vec{downfield, 0, 0} * (amount * dist * game_tan * widen * k / std::sin(follow_pitch * pi / 180.0f));
    }

    Vec player_at(uint8_t* rdram, int i) {
        int32_t r = players + i * player_size;
        return Vec{get_float(rdram, r), 0.0f, get_float(rdram, r + 8)};
    }

    // The nearest distance that keeps these points, feet and heads, inside the picture around center.
    float fit_distance(const Vec* points, int count, Vec center, Vec h, Vec r, float at_pitch, float at_shift, float from) {
        float cp = std::cos(at_pitch * pi / 180.0f), sp = std::sin(at_pitch * pi / 180.0f);
        Vec f = unit(h * cp + Vec{0, -sp, 0});
        Vec u = cross(r, f);
        float tan_h = game_tan * widen * game_aspect * 0.92f, tan_v = game_tan * widen * k * 0.92f;
        for (float d = from; d < dist_wide; d += 8.0f) {
            Vec aim = center + h * (at_shift * d * game_tan * widen * k / sp);
            Vec eye = aim + f * -d;
            bool fits = true;
            for (int i = 0; i < count && fits; i++) {
                for (float y : {0.0f, 22.0f}) {
                    Vec v = points[i] + Vec{0, y, 0} - eye;
                    float z = dot(v, f);
                    if (z <= 1.0f || std::fabs(dot(v, r)) > z * tan_h || std::fabs(dot(v, u)) > z * tan_v) fits = false;
                }
            }
            if (fits) return d;
        }
        return dist_wide;
    }

    // Moves the camera. The game's own eye and at come in, the following camera's go out, mixed.
    void follow(uint8_t* rdram, Vec& eye, Vec& at) {
        int phase = MEM_W(0, play_phase);
        bool sane = true;
        for (int i = 0; i < 14; i++) sane = sane && MEM_W(0xB14, players + i * player_size) == i;
        bool wanted = auto_on != 0.0f && sane && (phase == 1 || phase == 2) && game_tan > 0.0f;

        if (!stepped) {
            stepped = true;
            if (!wanted) open_wide = false;
            follow_mix += ((open_wide ? 1.0f : 0.0f) - follow_mix) * (open_wide ? wide_out : wide_in);
            if (wanted) {
                int base = (MEM_W(0, offense) & 1) ? 7 : 0, qb = base + 3;
                float line = (player_at(rdram, base).x + player_at(rdram, base + 1).x + player_at(rdram, base + 2).x) / 3.0f;
                Vec q = player_at(rdram, qb);
                if (phase == 1 && line != q.x) downfield = line > q.x ? 1.0f : -1.0f;
                Vec h{downfield, 0, 0};
                Vec r = unit(cross(h, Vec{0, 1, 0}));

                Vec points[8];
                int count = 0, who = MEM_W(0, passer), owner = MEM_W(0, ball_owner);
                float goal_dist, goal_shift = 0.0f, goal_pitch = pitch_carry;
                bool opening = false;
                if (phase == 1) {
                    scrimmage = line;
                    read_this_play = false;
                }
                bool scrambling = phase == 2 && owner == who && (q.x - scrimmage) * downfield > 0.0f;
                Vec goal_center;
                // The game hands the ball to the target as it's thrown, so the owner is who to look at from then on.
                bool in_air = phase == 2 && who == -1;
                if (phase == 1) {
                    goal_center = q * 0.5f + Vec{line, 0, q.z} * 0.5f;
                    goal_dist = dist_near;
                    goal_shift = shift;
                    goal_pitch = pitch_line;
                }
                else if (who >= 0 && owner == who && !scrambling) {
                    opening = true;
                    read_this_play = true;
                    goal_pitch = pitch_read;
                    int receivers = owner < 7 ? 3 : 10;
                    for (int i = 0; i < 4; i++) points[count++] = player_at(rdram, receivers + i);
                    float fmin = 1e9f, fmax = -1e9f, rmin = 1e9f, rmax = -1e9f;
                    for (int i = 0; i < count; i++) {
                        fmin = std::min(fmin, dot(points[i], h));
                        fmax = std::max(fmax, dot(points[i], h));
                        rmin = std::min(rmin, dot(points[i], r));
                        rmax = std::max(rmax, dot(points[i], r));
                    }
                    goal_center = h * ((fmin + fmax) * 0.5f) + r * ((rmin + rmax) * 0.5f);
                    count += humans_at(rdram, points + count, 4);
                    goal_dist = fit_distance(points, count, goal_center, h, r, pitch_read, 0.0f, dist_near);
                }
                else {
                    if (owner >= 0 && owner <= 13) goal_center = player_at(rdram, owner);
                    else goal_center = Vec{at.x, 0, at.z};  // a loose ball: the game's own idea
                    goal_shift = shift;
                    points[count++] = goal_center;
                    count += humans_at(rdram, points + count, 4);
                    opening = in_air && read_this_play;  // a kick's in the air too: that one stays the game's to follow
                    goal_dist = in_air ? fit_distance(points, count, goal_center, h, r, pitch_carry, shift, dist_hold) : follow_dist;
                }
                open_wide = opening;
                static const bool log_camera = getenv("BLITZ_TALL_LOG") != nullptr;
                static int log_tick = 0;
                if (log_camera && ++log_tick % 30 == 0) {
                    Vec humans[4];
                    int n = humans_at(rdram, humans, 4);
                    float worst = 0.0f;
                    for (int i = 0; i < n; i++) {
                        Vec v = humans[i] - (follow_center + f_preview_shift(follow_shift, follow_dist));
                        worst = std::max(worst, std::fabs(dot(v, r)));
                    }
                    fprintf(stderr, "[camera] phase %d who %d owner %d offense %d humans %d (%.0f,%.0f) dist %.0f goal %.0f pitch %.0f across %.0f\n", phase, who, owner, MEM_W(0, offense) & 1, n, n > 0 ? humans[0].x : 0.0f, n > 0 ? humans[0].z : 0.0f, follow_dist, goal_dist, follow_pitch, worst);
                }
                if (!follow_valid) {
                    follow_center = goal_center;
                    follow_dist = goal_dist;
                    follow_shift = goal_shift;
                    follow_pitch = goal_pitch;
                    follow_valid = true;
                }
                // Quicker with the ball in the air, so a long throw doesn't leave the picture behind it.
                follow_center = follow_center + (goal_center - follow_center) * (in_air ? 0.22f : 0.10f);
                follow_shift += (goal_shift - follow_shift) * 0.08f;
                follow_pitch += (goal_pitch - follow_pitch) * 0.08f;
                // Out quickly, so a receiver isn't lost; back in slowly.
                follow_dist += (goal_dist - follow_dist) * (goal_dist > follow_dist ? 0.12f : 0.05f);
            }
            else if (follow_mix < 0.01f) {
                follow_valid = false;
            }
        }
        if (follow_mix < 0.01f || !follow_valid) return;
        float cp = std::cos(follow_pitch * pi / 180.0f), sp = std::sin(follow_pitch * pi / 180.0f);
        Vec f = unit(Vec{downfield, 0, 0} * cp + Vec{0, -sp, 0});
        // The ground a screen's height covers, near enough: moving the aim forward moves the play down the picture.
        Vec aim = follow_center + Vec{downfield, 0, 0} * (follow_shift * follow_dist * game_tan * widen * k / sp);
        Vec my_eye = aim + f * -follow_dist;
        eye = eye + (my_eye - eye) * follow_mix;
        at = at + (aim - at) * follow_mix;
    }
}

// gfx_load_camera 0x8000F9E0: the camera callback has filled cam = {eye[3], at[3], up[3]} on the stack.
extern "C" void blitz_tall_camera(uint8_t* rdram, int32_t cam) {
    if (!tall_on()) return;
    if (calls++ % 30 == 0) reload();
    active = blitz_tall_2d() != 0;
    if (!active) return;

    float eye[3], at[3];
    for (int i = 0; i < 3; i++) {
        eye[i] = get_float(rdram, cam + 4 * i);
        at[i] = get_float(rdram, cam + 12 + 4 * i);
    }
    if (getenv("BLITZ_TALL_LOG") && calls % 90 == 1) {
        fprintf(stderr, "[tall] eye %.1f %.1f %.1f  at %.1f %.1f %.1f\n", eye[0], eye[1], eye[2], at[0], at[1], at[2]);
    }
    game_view.set(Vec{eye[0], eye[1], eye[2]}, Vec{at[0], at[1], at[2]});
    float fx = at[0] - eye[0], fz = at[2] - eye[2];
    float len = std::sqrt(fx * fx + fz * fz);
    if (len > 0.001f) {
        fx /= len;
        fz /= len;
        eye[0] -= fx * back;
        eye[2] -= fz * back;
        at[0] += fx * look;
        at[2] += fz * look;
    }
    eye[1] += eye_lift;
    Vec eye_v{eye[0], eye[1], eye[2]}, at_v{at[0], at[1], at[2]};
    if (blitz_ds_active() && tilt != 0.0f && game_tan > 0.0f) {
        // Up by the angle that puts the old aim that far down the tall picture; the same distance, so nothing else moves.
        View base;
        base.set(eye_v, at_v);
        // Back along the view first: the old aim stays where it was on the picture.
        eye_v = at_v + base.f * -(std::sqrt(dot(at_v - eye_v, at_v - eye_v)) * zoom);
        float slope = tilt * game_tan * widen * k;
        // A low camera (a kickoff's) tilted that far would look past the top of the sky's art: only as far as sky_limit.
        // A view widened for a narrower screen (TALL across) and taller than two 4:3 screens' (k 2: an AYN Thor's square-ish
        // lower screen under its wide upper one) spends its extra height on sky. TALL skyfit 1 brings the top edge down by
        // that much (the picture's bottom edge where two 4:3 screens have it): more field on the upper screen and the line
        // near the hinge, but a low camera's play (a fumble's pile) then sits on the hinge or the upper screen. Off by
        // default (Mac test at Thor proportions, 2026-11-27): the play stays on the lower screen, the top has more sky.
        float sky = sky_limit;
        if (widen > 1.001f) sky -= sky_fit * 2.0f * (std::atan(game_tan * widen * k) - std::atan(game_tan * 2.0f)) * 180.0f / pi;
        float room = sky * pi / 180.0f - std::asin(std::clamp(base.f.y, -1.0f, 1.0f)) - std::atan(game_tan * widen * k);
        slope = std::min(slope, std::tan(std::max(room, 0.0f)));
        at_v = eye_v + unit(base.f + base.u * slope) * std::sqrt(dot(at_v - eye_v, at_v - eye_v));
    }
    follow(rdram, eye_v, at_v);
    eye[0] = eye_v.x; eye[1] = eye_v.y; eye[2] = eye_v.z;
    at[0] = at_v.x; at[1] = at_v.y; at[2] = at_v.z;
    tall_view.set(Vec{eye[0], eye[1], eye[2]}, Vec{at[0], at[1], at[2]});
    have_views = true;
    for (int i = 0; i < 3; i++) {
        put_float(rdram, cam + 4 * i, eye[i]);
        put_float(rdram, cam + 12 + 4 * i, at[i]);
    }
}

// gfx_load_camera 0x8000FA10: a2 = guPerspective's field of view (degrees, float bits).
extern "C" uint32_t blitz_tall_fov(uint32_t fov_bits) {
    if (!tall_on() || !active) return fov_bits;
    float fov;
    memcpy(&fov, &fov_bits, 4);
    game_tan = std::tan(fov * pi / 360.0f);
    fov = 2.0f * std::atan(k * widen * std::tan(fov * pi / 360.0f)) * 180.0f / pi;
    memcpy(&fov_bits, &fov, 4);
    return fov_bits;
}

extern "C" int32_t blitz_tall_far(int32_t far_units) {
    if (!tall_on() || !active || far_plane <= (float)far_units) return far_units;
    return (int32_t)far_plane;
}

// ---- The hinge ------------------------------------------------------------------------------------------------------

namespace {
    recompui::ContextId hinge_context = recompui::ContextId::null();
    recompui::Element* hinge_bar = nullptr;
    bool hinge_shown = false;
    constexpr recompui::Color hinge_color{26, 27, 30, 255};

    void hinge_build() {
        using namespace recompui;
        hinge_context = create_context();
        hinge_context.open();
        hinge_context.set_captures_input(false);
        hinge_context.set_captures_mouse(false);
        Element* root = hinge_context.create_element<Element>(hinge_context.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(0);
        root->set_left(0);
        root->set_right(0);
        root->set_bottom(0);
        root->set_pointer_events(PointerEvents::None);
        hinge_bar = hinge_context.create_element<Element>(root);
        hinge_bar->set_position(Position::Absolute);
        hinge_bar->set_left(0);
        hinge_bar->set_right(0);
        hinge_bar->set_background_color(hinge_color);
        hinge_bar->set_pointer_events(PointerEvents::None);
        hinge_context.close();
    }
}

namespace blitz {
    // Every UI tick (main.cpp): whatever the window has beyond 2:3 is the hinge.
    void tall_view_update(SDL_Window* window) {
        if (settings_path == nullptr || blitz_ds_active() || window == nullptr) {
            if (hinge_shown) recompui::hide_context(hinge_context);
            hinge_shown = false;
            return;
        }
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w <= 0 || h <= 0) return;
        float hinge = h - w * 1.5f;
        bool show = blitz_tall_2d() && hinge >= 2.0f && !recompui::is_context_capturing_input();
        if (!show) {
            if (hinge_shown) recompui::hide_context(hinge_context);
            hinge_shown = false;
            return;
        }
        if (hinge_context == recompui::ContextId::null()) hinge_build();
        float dp = 1080.0f / h;  // UI units per pixel
        hinge_context.open();
        hinge_bar->set_top((h - hinge) * 0.5f * dp);
        hinge_bar->set_height(hinge * dp);
        hinge_context.close();
        if (!hinge_shown) {
            recompui::show_context(hinge_context, "");
            hinge_shown = true;
        }
    }
}
