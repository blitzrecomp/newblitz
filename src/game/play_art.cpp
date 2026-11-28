// Play art (Settings → Rules → Play Art; the team, 2026-11-26): Madden-style route ribbons painted on the turf before the
// snap. At the line (play phase 1), the human offense's three receivers each get a ribbon with an arrowhead along the
// route the play gives them, in the Receiver Buttons order and colours (left to right as they line up: △ green, R1
// blue, ○ red; Y yellow on an Xbox pad); at the snap (phase 2) it's gone. Research: docs/notes/PLAY_ART.md.
//
// Drawn by the game's own 3D pass, so the depth buffer does the work: the triangles go into the frame's display list
// at the second camera load (gfx_ext.cpp blitz_camera_tag), which comes after the field, the line markers and the
// shadows and before the players. Depth test on (against the turf), no depth write, blended; the players drawn next
// stand on top. World space, so the tall view (BLITZ DS) and widescreen need nothing of their own.
//
// The routes: 0x802EA4AC[side] is the chosen play's record (+0 formation, +8 seven route-script pointers), the same
// record ovl_a 0x8030EE90 hands out before the lineup, re-read every frame (audibles, flips). Walked from each
// receiver's actual spot; the play's world axes come from the players' spots against the formation's numbers, so
// side, direction and a flipped play need no flags (play_art_walk.h).
//
// Live (dev_input.cpp): "PLAYART off|first|full|auto" (auto: the setting), "PLAYART width 2.2", "lift 0.8",
// "alpha 208", "ztest 0|1", "h2h 0|1" (show it when the defense is human too), "turbo 0|1" (only while Turbo is held, on by
// default). Log: [playart].
#include <cstring>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "recomp.h"
#include "recompui/config.h"
#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

#include "play_art_walk.h"

#ifndef G_MTX_MODELVIEW
#define G_MTX_MODELVIEW 0x00
#endif

namespace blitz {
    int port_glyph_family(int port);  // controllers.cpp
    bool turbo_held(int port);        // receiver_targets.cpp
}
extern "C" int blitz_play_select_on_screen(void);  // gfx_ext.cpp

namespace {
    constexpr uint32_t players = 0x802D1590, stride = 0xB98;
    constexpr uint32_t f_index = 0xB14, f_slot = 0xB20;
    constexpr uint32_t play_phase = 0x802EA4A8, offense = 0x802EA47C;  // the play record: play_art_walk.h
    constexpr uint32_t slot_of_pad = 0x8002AD94;
    constexpr uint32_t offense_plays = 0x80336B24, offense_names = 0x80336BD8;  // 45 each (ovl_a), for the log
    constexpr uint32_t gameplay_camera = 0x80318CB0, camera_callback = 0x8002B6A0;
    constexpr uint32_t list_capacity = 0x5DC0;   // the frame's list (gfx_ext.cpp, NOTES.md 14)
    constexpr uint32_t list_room = 0x2000;       // drawn only while the list so far plus this stays under 8 KB
    constexpr uint32_t max_bytes = 0xC00;
    constexpr int fade_frames = 6;

    // Session settings: the live command's. mode -1: the setting.
    std::atomic<int> live_mode{-1};
    std::atomic<float> half_width{3.5f}, lift{0.8f};
    std::atomic<int> alpha{208}, ztest{1}, h2h{0};
    std::atomic<bool> turbo_gate{true};  // only while Turbo is held (PLAYART turbo 0|1)

    // The frame lists the game builds in (two, alternating), from blitz_gfx_prologue.
    std::array<std::atomic<uint32_t>, 4> lists{};

    bool readable(uint32_t a) { return a >= 0x80000000u && a < 0x80800000u && (a & 3) == 0; }
    int32_t rd(uint8_t* rdram, uint32_t a) { return MEM_W(0, (gpr)(int32_t)a); }
    uint32_t rec(int i) { return players + (uint32_t)i * stride; }

    int setting_mode() {
        int m = live_mode.load();
        if (m >= 0) return m;
        try {
            return (int)std::get<uint32_t>(recompui::config::get_config("rules").get_option_value("play_art"));
        }
        catch (...) {
            return 0;
        }
    }
    const char* mode_name(int m) { return m == 1 ? "first" : m == 2 ? "full" : "off"; }

    bool in_game(uint8_t* rdram) {
        for (int i = 0; i < 14; i++) {
            if (rd(rdram, rec(i) + f_index) != i) return false;
        }
        return true;
    }

    // A side with a human on it: any of its seven players controlled from a player slot.
    bool human_side(uint8_t* rdram, int side) {
        for (int s = 0; s < 7; s++) {
            int slot = rd(rdram, rec(side * 7 + s) + f_slot);
            if (slot >= 0 && slot <= 3) return true;
        }
        return false;
    }

    std::string play_name(uint8_t* rdram, uint32_t record) {
        for (uint32_t i = 0; i < 45; i++) {
            if ((uint32_t)rd(rdram, offense_plays + 4 * i) != record) continue;
            uint32_t s = (uint32_t)rd(rdram, offense_names + 4 * i);
            std::string n;
            for (uint32_t k = 0; readable(s & ~3u) && k < 24; k++) {
                char c = (char)MEM_B(k, (gpr)(int32_t)s);
                if (c == 0) break;
                n += c;
            }
            return std::to_string(i) + " '" + n + "'";
        }
        return "custom";  // a Play Editor play, or another build's tables
    }

    // The log, on a change only (the game thread's; the live command asks for the next line again).
    std::string last_log;
    std::atomic<bool> relog{false};
    void log_once(const std::string& s) {
        if (relog.exchange(false)) last_log.clear();
        if (s == last_log) return;
        last_log = s;
        fprintf(stderr, "[playart] %s\n", s.c_str());
    }

    int frames_shown = 0;

    // The camera, frame to frame (gfx_ext.cpp passes the one this frame is drawn with: eye then look-at, on its stack): the
    // routes wait until it has stopped. On a game's first play it flies around the field while the rules boards are up, and
    // routes drawn from there read as stray lines across the screen (team, 2026-11-27). Behind the quarterback at the line it
    // sits still. [playart] camera lines give the movement while it waits.
    int32_t camera_now = 0;
    float last_cam[6] = {0, 0, 0, 0, 0, 0};
    int still_frames = 0;
    constexpr int still_needed = 8;           // frames, about a quarter second at 30
    constexpr float moving_per_frame = 0.3f;  // units the eye and look-at move together
    bool camera_settled(uint8_t* rdram) {
        if (camera_now == 0) return true;
        float c[6];
        for (int i = 0; i < 6; i++) {
            const int32_t w = rd(rdram, (uint32_t)camera_now + 4u * (uint32_t)i);
            std::memcpy(&c[i], &w, sizeof(float));
        }
        float d = 0;
        for (int i = 0; i < 6; i++) d += (c[i] - last_cam[i]) * (c[i] - last_cam[i]);
        d = std::sqrt(d);
        std::memcpy(last_cam, c, sizeof(c));
        const bool moving = d > moving_per_frame;
        if (moving && still_frames > 0) {
            static int said = 0;
            if (said++ < 20) fprintf(stderr, "[playart] camera moving %.2f a frame: routes wait\n", d);
        }
        still_frames = moving ? 0 : std::min(still_frames + 1, 1000);
        return still_frames >= still_needed;
    }

    // The frame list the cursor is in, or 0.
    uint32_t list_of(uint32_t cursor) {
        for (auto& l : lists) {
            uint32_t b = l.load();
            if (b != 0 && cursor >= b && cursor < b + list_capacity) return b;
        }
        return 0;
    }

    void draw(uint8_t* rdram, int32_t gp) {
        const int mode = setting_mode();
        if (mode <= 0) {
            frames_shown = 0;
            log_once("off");
            return;
        }
        if (!in_game(rdram) || (uint32_t)rd(rdram, camera_callback) != gameplay_camera) {
            frames_shown = 0;
            return;
        }
        const int phase = rd(rdram, play_phase);
        if (phase != 1) {
            if (frames_shown > 0) log_once("gone (phase " + std::to_string(phase) + ")");
            frames_shown = 0;
            return;
        }
        if (blitz_play_select_on_screen()) return;
        if (!camera_settled(rdram)) {
            if (frames_shown > 0) log_once("waiting: the camera's moving");
            frames_shown = 0;
            return;
        }
        const int side = rd(rdram, offense) & 1;
        if (!human_side(rdram, side)) {
            log_once("none: the CPU's offense");
            return;
        }
        if (human_side(rdram, 1 - side) && !h2h.load()) {
            log_once("none: a human on defense too (PLAYART h2h 1 to show it)");
            return;
        }
        // The colours of the buttons of the controller that plays the quarterback.
        int port = 0;
        for (int p = 0; p < 4; p++) {
            int slot = rd(rdram, slot_of_pad + 4u * (uint32_t)p);
            if (slot >= 0 && slot == rd(rdram, rec(side * 7 + 3) + f_slot)) {
                port = p;
                break;
            }
        }
        // Only while Turbo is held, as the icons over the receivers (team, 2026-11-27): a look at the routes, not always on.
        // "PLAYART turbo 0" shows them all the time at the line.
        if (turbo_gate.load() && !blitz::turbo_held(port)) {
            if (frames_shown > 0) log_once("hidden: Turbo let go");
            frames_shown = 0;
            return;
        }
        std::array<uint32_t, 3> colour = playart::colours(blitz::port_glyph_family(port));
        frames_shown++;
        const float fade = std::min(1.0f, (float)frames_shown / (float)fade_frames);
        const uint32_t a = (uint32_t)std::clamp((int)std::lround((float)alpha.load() * fade), 0, 255);
        for (auto& c : colour) c = (c & 0xFFFFFF00u) | a;

        playart::Style style;
        style.half_width = half_width.load();
        style.lift = lift.load();
        const playart::Cut cut = mode == 2 ? playart::Cut::Full : playart::Cut::FirstBreak;
        playart::Read32 read = [&](uint32_t addr, int32_t& out) {
            if (!readable(addr)) return false;
            out = rd(rdram, addr);
            return true;
        };
        std::vector<playart::Prim> prims;
        const playart::Art art = playart::build_art(read, side, cut, style, colour, prims);
        const uint32_t record = art.record;
        if (art.why != nullptr) {
            log_once(std::string("none: ") + art.why + " (play " + play_name(rdram, record) + ")");
            return;
        }
        std::string legs = std::to_string(art.legs[0]) + "/" + std::to_string(art.legs[1]) + "/" + std::to_string(art.legs[2]);

        const uint32_t cursor = (uint32_t)MEM_W(0, (gpr)gp);
        const uint32_t base = list_of(cursor);
        playart::ListOptions o;
        o.ztest = ztest.load() != 0;
        o.use_ex = true;
        GfxCommand c[2];
        // Its own interpolation group, pushed and popped, so the scene's matrices keep their order whether or not
        // the art is drawn this frame.
        gEXMatrixGroup(c, 0xB4000000u, G_EX_INTERPOLATE_DECOMPOSE, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_COMPONENT_AUTO,
                       G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO,
                       G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_ORDER_AUTO, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
                       G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
        o.ex_group_push = {c[0].values.word0, c[0].values.word1, c[1].values.word0, c[1].values.word1};
        gEXPopMatrixGroup(c, G_MTX_MODELVIEW);
        o.ex_group_pop = {c[0].values.word0, c[0].values.word1};
        const std::vector<uint32_t> words = playart::build_list(cursor, prims, o);
        const uint32_t bytes = (uint32_t)words.size() * 4;
        std::string what = "play " + play_name(rdram, record) + " side " + std::to_string(side) + " " + mode_name(mode) +
                           ": routes " + legs + " legs, " + std::to_string(prims.size()) + " prims, " + std::to_string(bytes) +
                           " bytes";
        if (words.empty()) {
            log_once(what + " (nothing to draw)");
            return;
        }
        if (base == 0 || bytes > max_bytes || cursor - base + bytes > list_room) {
            log_once(what + ": no room in the frame's list (at +" + std::to_string(base ? cursor - base : 0) + ")");
            return;
        }
        for (size_t i = 0; i < words.size(); i++) MEM_W((int32_t)(i * 4), (gpr)(int32_t)cursor) = (int32_t)words[i];
        MEM_W(0, (gpr)gp) = (int32_t)(cursor + bytes);  // the game carries on after it
        if (frames_shown >= fade_frames) log_once(what + ", list at +" + std::to_string(cursor - base));
    }
}

// gfx_ext.cpp blitz_camera_tag, each camera load of the frame (call 0 the field's, 1 the players').
extern "C" void blitz_play_art_camera(uint8_t* rdram, int32_t gp, int32_t cam, int call) {
    if (call == 1) {
        camera_now = cam;
        draw(rdram, gp);
    }
}

// gfx_ext.cpp blitz_gfx_prologue: a graphics task's list, to know which buffer the cursor is in.
extern "C" void blitz_play_art_list(int32_t dl) {
    const uint32_t d = (uint32_t)dl;
    for (auto& l : lists) {
        if (l.load() == d) return;
    }
    for (size_t i = lists.size() - 1; i > 0; i--) lists[i] = lists[i - 1].load();
    lists[0] = d;
}

namespace blitz {
    // Settings → Rules (house_rules.cpp).
    void add_play_art_option(recomp::config::Config& c) {
        c.add_enum_option("play_art", "Play Art",
                          "At the line before the snap, hold <b>Turbo</b> to see your receivers' routes painted on the field, Madden style, "
                          "in the colours of their Receiver Buttons (left to right: <b>△</b> green, <b>R1</b> blue, "
                          "<b>○</b> red), with the icons over their heads. Gone at the snap. <b>Off</b> never shows them. <b>First Break</b>: each route to its first cut. "
                          "<b>Full</b>: the whole route. Only your own plays; not when a human is on defense too.",
                          {{0u, "off", "Off"}, {1u, "first", "First Break"}, {2u, "full", "Full"}}, 1u);
    }

    // dev_input.cpp: "PLAYART off|first|full|auto", "PLAYART width 2.2", ...
    void play_art_live(const std::string& args) {
        std::stringstream ss{args};
        std::string what;
        ss >> what;
        float v = 0;
        bool has = (bool)(ss >> v);
        if (what == "off") live_mode = 0;
        else if (what == "first") live_mode = 1;
        else if (what == "full") live_mode = 2;
        else if (what == "auto") live_mode = -1;
        else if (what == "turbo" && has) turbo_gate = v != 0.0f;
        else if (what == "width" && has) half_width = v;
        else if (what == "lift" && has) lift = v;
        else if (what == "alpha" && has) alpha = (int)v;
        else if (what == "ztest" && has) ztest = (int)v;
        else if (what == "h2h" && has) h2h = (int)v;
        else {
            fprintf(stderr, "[playart] PLAYART off|first|full|auto, or width|lift|alpha|ztest|h2h <n>\n");
            return;
        }
        relog = true;  // the next frame logs what it draws
        fprintf(stderr, "[playart] live: mode %d (-1 the setting), half width %.2f, lift %.2f, alpha %d, ztest %d, h2h %d\n",
                live_mode.load(), half_width.load(), lift.load(), alpha.load(), ztest.load(), h2h.load());
    }
}
