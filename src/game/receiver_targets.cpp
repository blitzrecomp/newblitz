// Receiver Buttons (Settings → Rules → Receiver Buttons; the team, 2026-10-23): Madden-style targeted passes. While a
// human's quarterback holds the ball, each of his three receivers wears a button icon that follows him (△, R1, ○ on a
// PlayStation pad: left to right as they lined up), and that button throws to him. ✕ still passes the game's way.
//
// How Blitz passes (found 2026-10-23 with save states): the Pass button throws to the receiver the stick points at,
// as the screen shows them. So a receiver button aims the stick from the quarterback at that receiver on screen and
// presses Pass with it; the game's own throw does the rest. The C buttons, which those face buttons are by default
// (△ C-left, R1 C-down, ○ C-right; the right stick too), do nothing in Blitz while the quarterback has the ball, so
// nothing is lost. On a phone, small ← ↓ → buttons around A press them (touch_controls.cpp), and the icons show arrows.
// Tested with live.sh's scripted presses, which go through the filter like a controller's (dev_input.cpp get_input):
// added after it, a scripted C button reached the game too and pulled the throw to the wrong man.
//
// What the game keeps (ovl_a, in both apps):
//   0x802D1590 + i * 0xB98   the 14 players (offense 0-6 or 7-13: three linemen, the QB, three receivers)
//     +0x0, +0x8             field position: downfield, across (floats)
//     +0x750, +0x754, +0x758 screen position the game projects every frame (its own name labels use them)
//     +0xB14                 the player's index; +0xB1C his team; +0xB20 the player slot controlling him, or -1
//   0x80364D20               the ball's owner (a player index; the QB before the throw)
//   0x802EA4A8               the play's phase: 2 while it's live (3 after the whistle); 0x802EA510 bit 0x400: whistled
//   0x8002AD94               [controller] the player slot it plays as
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "SDL2/SDL.h"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "recompinput/promptfont.h"
#include "ultramodern/ultramodern.hpp"

namespace blitz {
    uint8_t* monitor_rdram();             // dev_monitor.cpp
    int port_glyph_family(int port);      // controllers.cpp: 0 PlayStation, 1 Xbox, 2 Switch, 3 N64
    bool touch_controls_showing();        // touch_controls.cpp
}

extern "C" int blitz_tall_2d(void);
extern "C" int blitz_tall_carry(float nx, float ny, float height, float lift, float* tx, float* ty);
extern "C" float blitz_tall_window_y(float picture_y, int w, int h);
extern "C" int blitz_ds_first_part(float* top, float* bottom);  // ds_view.cpp
extern "C" void blitz_ds_badge(int k, int on, int screen, float x, float y, const char* glyph);
extern "C" int blitz_ds_active(void);          // ds_view.cpp
extern "C" int blitz_ds_icons_on_second(int* w, int* h);
extern "C" volatile int blitz_ds_screen_mode;  // tall_view.cpp: 0 both screens, 1 game + companion, 2 stylus

namespace {
    constexpr uint32_t players = 0x802D1590, stride = 0xB98, ball_owner = 0x80364D20, slot_of_pad = 0x8002AD94;
    constexpr uint32_t play_phase = 0x802EA4A8, play_flags = 0x802EA510, offense = 0x802EA47C, passer = 0x802EA4C0;
    constexpr uint32_t f_index = 0xB14, f_team = 0xB1C, f_slot = 0xB20;
    constexpr uint16_t A = 0x8000, Z = 0x2000, C_LEFT = 0x0002, C_RIGHT = 0x0001, C_DOWN = 0x0004, C_ALL = 0x000F;
    // The receiver buttons, left to right: C-left (△), C-down (R1), C-right (○).
    constexpr std::array<uint16_t, 3> buttons{C_LEFT, C_DOWN, C_RIGHT};

    bool setting_on() {
        return std::get<bool>(recompui::config::get_config("rules").get_option_value("receiver_buttons"));
    }
    // BLITZ DS's Stylus has them on whatever the setting says (team): its tiles throw to a receiver with these buttons, and the
    // icons say who's who. Only player 1's: the tiles and taps are his.
    bool stylus_on() { return blitz_ds_active() && blitz_ds_screen_mode == 2; }

    int32_t rd(uint8_t* rdram, uint32_t addr) {
        int32_t v;
        std::memcpy(&v, rdram + (addr - 0x80000000u), 4);
        return v;
    }
    float rdf(uint8_t* rdram, uint32_t addr) {
        float v;
        std::memcpy(&v, rdram + (addr - 0x80000000u), 4);
        return v;
    }
    uint32_t rec(int i) { return players + (uint32_t)i * stride; }

    // The players' table is there (a game is on): every record carries its own index.
    bool in_game(uint8_t* rdram) {
        for (int i = 0; i < 14; i++) {
            if (rd(rdram, rec(i) + f_index) != i) return false;
        }
        return true;
    }

    // The player who may throw, holding the ball behind the line, and the player slot controlling him, or -1s.
    // Usually the quarterback; after a pass caught behind the line, the catcher, who can throw again (a double pass;
    // The team, 2026-10-23). The game keeps who may throw at 0x802EA4C0 (its pass code checks it; -1 while the ball's in
    // the air or after a handoff).
    struct Snap {
        int qb = -1;
        int slot = -1;
    };
    // The line of scrimmage, seen at the line before the snap: the center's spot (who may throw then: he snaps it) and
    // which way is downfield (away from his quarterback). The game marks a catcher as able to throw wherever he is
    // and checks the line itself, so this does too. Unknown (a save state loaded mid-play): only the quarterback.
    std::atomic<float> line_x{0}, downfield{0};  // downfield 0: unknown

    void watch_line(uint8_t* rdram) {
        if (rd(rdram, play_phase) != 1) return;
        int center = rd(rdram, passer);
        if (center < 0 || center > 13) return;
        int qb = center < 7 ? 3 : 10;
        float cx = rdf(rdram, rec(center)), qx = rdf(rdram, rec(qb));
        if (!std::isfinite(cx) || !std::isfinite(qx) || cx == qx) return;
        line_x = cx;
        downfield = cx > qx ? 1.0f : -1.0f;
    }

    bool behind_line(uint8_t* rdram, int who) {
        float dir = downfield.load();
        if (dir == 0) return who == 3 || who == 10;
        return (rdf(rdram, rec(who)) - line_x.load()) * dir < 0;
    }

    Snap current(uint8_t* rdram) {
        Snap s;
        if (rdram == nullptr || !in_game(rdram)) return s;
        watch_line(rdram);
        if (rd(rdram, play_phase) != 2 || (rd(rdram, play_flags) & 0x400)) return s;  // no live play
        int who = rd(rdram, passer);
        if (who < 0 || who > 13 || rd(rdram, ball_owner) != who) return s;  // nobody may throw
        if (!behind_line(rdram, who)) return s;  // past the line: no more passes
        int slot = rd(rdram, rec(who) + f_slot);
        if (slot < 0 || slot > 3) return s;      // the CPU's
        s.qb = who;
        s.slot = slot;
        return s;
    }

    // Shared between the game thread (input) and the main thread (icons).
    std::mutex mutex;
    int play_qb = -1;                 // the passer the order below is for (-1 while nobody may throw)
    std::array<int, 3> order{};       // the receivers, left to right as they stood when the icons came up
    struct Pending {
        int target = -1;
        int frames = 0;
    };
    std::array<Pending, 4> pending{};
    std::array<int, 4> forced{-1, -1, -1, -1};  // per player slot: the receiver his pass goes to (blitz_pass_target)
    std::array<uint16_t, 4> held{};
    constexpr int press_frames = 24;  // reads of the aimed stick, the first pass_frames of them with Pass
    constexpr int pass_frames = 9;

    // The targets, fixed when a passer first has the ball (mutex held).
    // A passer's three targets, left to right on screen now: the other three of his team's quarterback and
    // receivers (records 3-6 or 10-13), so on a double pass the quarterback is one.
    std::array<int, 3> left_to_right(uint8_t* rdram, int qb) {
        std::array<int, 3> r{};
        int base = qb < 7 ? 3 : 10;
        for (int i = base, k = 0; i < base + 4 && k < 3; i++) {
            if (i != qb) r[(size_t)k++] = i;
        }
        std::sort(r.begin(), r.end(), [&](int a, int b) { return rdf(rdram, rec(a) + 0x750) < rdf(rdram, rec(b) + 0x750); });
        return r;
    }

    void ensure_order(uint8_t* rdram, int qb) {
        if (play_qb == qb) return;
        play_qb = qb;
        order = left_to_right(rdram, qb);
    }

    // At the line before the snap (the play's phase 1): the offense's quarterback, when a human plays him (Madden's
    // look at who's where: the icons show while Turbo, L2 on a PlayStation pad, is held). The offense from the game's
    // side with the ball (0x802EA47C bit 0: 0 the team in records 0-6), not the ball's owner, which can still be
    // whoever had it last play (PS5, 2026-10-23: the icons came and went).
    Snap presnap(uint8_t* rdram) {
        Snap s;
        if (rdram == nullptr || !in_game(rdram)) return s;
        if (rd(rdram, play_phase) != 1) return s;
        int qb = (rd(rdram, offense) & 1) ? 10 : 3;
        int slot = rd(rdram, rec(qb) + f_slot);
        if (slot < 0 || slot > 3) return s;
        s.qb = qb;
        s.slot = slot;
        return s;
    }

    // The stick, aimed from the quarterback at a receiver as the screen shows them: Blitz throws to the receiver
    // nearest the stick's direction on screen (with field positions, a far receiver out wide lost to a nearer one
    // in front: play testing, 2026-10-23). The game's screen x and y units are nearly square (376 across a 4:3
    // picture, 384/1.44 down it); up is downfield.
    void aim(uint8_t* rdram, int qb, int target, float* x, float* y) {
        float dx = (rdf(rdram, rec(target) + 0x750) - rdf(rdram, rec(qb) + 0x750)) * (384.0f / 1.44f) / (376.0f * 0.75f);
        float dy = rdf(rdram, rec(qb) + 0x754) - rdf(rdram, rec(target) + 0x754);
        float len = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(len) || len < 0.001f) return;
        *x = dx / len;
        *y = dy / len;
    }
}

namespace blitz {
    // Do this controller's receiver buttons throw? Any thread (touch_controls.cpp has player 1's).
    bool receiver_buttons_on(int port) { return setting_on() || (port == 0 && stylus_on()); }

    // Game thread (controllers.cpp, each player's read): a receiver button becomes an aimed pass.
    void receiver_targets_filter(int port, uint16_t* out_buttons, float* x, float* y) {
        if (port < 0 || port > 3) return;
        uint8_t* rdram = monitor_rdram();
        Snap s = current(rdram);
        std::lock_guard lock{mutex};
        if (s.qb < 0) {
            if (play_qb >= 0 && rdram != nullptr && in_game(rdram)) {
                fprintf(stderr, "[receivers] the ball went to player %d\n", rd(rdram, ball_owner));  // play testing
            }
            play_qb = -1;
            pending[port] = {};
            forced = {-1, -1, -1, -1};
            held[port] = *out_buttons;
            return;
        }
        if (!receiver_buttons_on(port) || rd(rdram, slot_of_pad + 4 * (uint32_t)port) != s.slot) {
            held[port] = *out_buttons;
            return;
        }
        ensure_order(rdram, s.qb);
        uint16_t now = *out_buttons;
        for (size_t k = 0; k < buttons.size(); k++) {
            if ((now & buttons[k]) && !(held[port] & buttons[k]) && pending[port].frames == 0) {
                pending[port] = {order[k], press_frames};
                forced[(size_t)s.slot] = order[k];
                float ax = 0, ay = 0;
                aim(rdram, s.qb, order[k], &ax, &ay);
                fprintf(stderr, "[receivers] port %d: pass to player %d (stick %.2f,%.2f; qb %.0f,%.0f; him %.0f,%.0f)\n", port + 1,
                        order[k], ax, ay, rdf(rdram, rec(s.qb) + 0x750), rdf(rdram, rec(s.qb) + 0x754),
                        rdf(rdram, rec(order[k]) + 0x750), rdf(rdram, rec(order[k]) + 0x754));
            }
        }
        held[port] = now;
        *out_buttons &= (uint16_t)~C_ALL;  // the receiver buttons are spoken for while the quarterback has the ball
        Pending& p = pending[port];
        if (p.frames > 0) {
            static const bool no_aim = getenv("BLITZ_RECEIVER_NO_AIM") != nullptr;  // dev: the pass hook alone
            if (!no_aim) aim(rdram, s.qb, p.target, x, y);
            // The stick and Pass together, the stick held well past the press: Blitz turns the quarterback toward
            // the stick and picks the receiver as the ball leaves (play testing, 2026-10-23).
            if (p.frames > press_frames - pass_frames) *out_buttons |= A;
            *out_buttons &= (uint16_t)~0x4000;      // and not a jump
            if (--p.frames == 0) forced[(size_t)s.slot] = -1;  // a ✕ pass later goes where the stick says
        }
    }
}

// ---- The icons ------------------------------------------------------------------------------------------------------

namespace {
    recompui::ContextId context = recompui::ContextId::null();
    std::array<recompui::Element*, 3> badge{};
    std::array<recompui::Label*, 3> glyph{};
    bool shown = false;
    int shown_family = -1;
    constexpr float badge_dp = 94.0f;  // 54 at first; the team: 75% bigger

    void build() {
        using namespace recompui;
        context = create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        Element* root = context.create_element<Element>(context.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(0);
        root->set_left(0);
        root->set_right(0);
        root->set_bottom(0);
        root->set_pointer_events(PointerEvents::None);
        for (size_t k = 0; k < badge.size(); k++) {
            Element* b = context.create_element<Element>(root);
            b->set_position(Position::Absolute);
            b->set_width(badge_dp);
            b->set_height(badge_dp);
            b->set_border_radius(badge_dp * 0.5f);
            b->set_background_color(Color{8, 12, 28, 200});
            b->set_border_width(3.0f);
            b->set_border_color(Color{255, 255, 255, 220});
            b->set_pointer_events(PointerEvents::None);
            Label* l = context.create_element<Label>(b, "", theme::Typography::LabelSM);
            l->set_font_family("promptfont");
            l->set_font_size(badge_dp * 0.62f);
            l->set_line_height(badge_dp - 6.0f);
            l->set_text_align(TextAlign::Center);
            l->set_position(Position::Absolute);
            l->set_left(0);
            l->set_right(0);
            l->set_top(0);
            l->set_color(Color{255, 255, 255, 255});
            badge[k] = b;
            glyph[k] = l;
        }
        context.close();
    }

    // The three buttons' glyphs for a controller family (left to right: C-left, C-down, C-right by default).
    constexpr int touch_family = 4;  // a phone played by touch: gold rings with the arrows of the buttons around A

    std::array<const char*, 3> glyphs_for(int family) {
        switch (family) {
            case touch_family: return {"\xE2\x86\x90", "\xE2\x86\x93", "\xE2\x86\x92"};  // ← ↓ →, as the buttons around A
            case 1: return {PF_XBOX_Y, PF_XBOX_RIGHT_SHOULDER, PF_XBOX_B};
            case 2: return {"X", "R", "A"};
            case 3: return {"C\xE2\x86\x90", "C\xE2\x86\x93", "C\xE2\x86\x92"};  // C← C↓ C→ (PromptFont has no ◀ ▼ ▶)
            default: return {PF_SONY_Y, PF_SONY_RIGHT_SHOULDER, PF_SONY_B};
        }
    }

    void hide() {
        for (int k = 0; k < 3; k++) blitz_ds_badge(k, 0, 0, 0.0f, 0.0f, nullptr);
        if (shown) {
            recompui::hide_context(context);
            shown = false;
        }
    }
}

namespace blitz {
    // The three receivers' button glyphs (PromptFont), left to right, for player 1's controller: the stylus tiles wear them
    // too (ds_stylus.cpp).
    std::array<std::string, 3> receiver_glyphs() {
        std::array<const char*, 3> g = glyphs_for(port_glyph_family(0));
        return {g[0], g[1], g[2]};
    }

    // Every UI tick (main.cpp): the icons over the receivers while a human's quarterback has the ball.
    void receiver_targets_update(SDL_Window* window) {
        uint8_t* rdram = monitor_rdram();
        if (!ultramodern::is_game_started() || window == nullptr || !(setting_on() || stylus_on()) || recompui::is_context_capturing_input()) {
            hide();
            return;
        }
        Snap s = current(rdram);
        bool live = s.qb >= 0;
        if (!live) s = presnap(rdram);
        if (s.qb < 0) {
            hide();
            return;
        }
        int port = -1;
        for (int p = 0; p < 4; p++) {
            if (rd(rdram, slot_of_pad + 4 * (uint32_t)p) == s.slot) port = p;
        }
        if (!receiver_buttons_on(port)) {  // Stylus alone, and someone else at quarterback
            hide();
            return;
        }
        std::array<int, 3> who;
        {
            std::lock_guard lock{mutex};
            bool look = live || (port >= 0 && (held[(size_t)port] & Z));  // before the snap: only while Turbo is held
            static bool was_looking = false;
            if (!live && look != was_looking) fprintf(stderr, "[receivers] before the snap: icons %s\n", look ? "on" : "off");
            was_looking = !live && look;
            if (!look) {
                hide();
                return;
            }
            if (live) ensure_order(rdram, s.qb);
            who = live ? order : left_to_right(rdram, s.qb);
        }
        if (context == recompui::ContextId::null()) build();
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w <= 0 || h <= 0) return;
        // The game's picture: 4:3 at the window's height, centred (with widescreen it extends past the sides, the
        // middle staying where it was).
        float game_w = h * 4.0f / 3.0f, left = (w - game_w) * 0.5f;
        float dp = 1080.0f / h;  // UI units per pixel
        int family = port == 0 && touch_controls_showing() ? touch_family : port >= 0 ? port_glyph_family(port) : 0;
        context.open();
        if (family != shown_family) {
            auto g = glyphs_for(family);
            bool touch = family == touch_family;
            for (size_t k = 0; k < glyph.size(); k++) {
                glyph[k]->set_text(g[k]);
                badge[k]->set_background_color(touch ? recompui::Color{255, 196, 40, 90} : recompui::Color{8, 12, 28, 200});
                badge[k]->set_border_color(touch ? recompui::Color{255, 196, 40, 240} : recompui::Color{255, 255, 255, 220});
                badge[k]->set_border_width(touch ? 5.0f : 3.0f);
            }
            shown_family = family;
        }
        for (size_t k = 0; k < who.size(); k++) {
            uint32_t r = rec(who[k]);
            float sx = rdf(rdram, r + 0x750), sy = rdf(rdram, r + 0x754), depth = rdf(rdram, r + 0x758);
            if (!std::isfinite(sx) || !std::isfinite(sy) || depth <= 1.0f) {
                blitz_ds_badge((int)k, 0, 0, 0.0f, 0.0f, nullptr);
                badge[k]->set_display(recompui::Display::None);
                continue;
            }
            // Stylus in play: the game's own camera on the second window, and nothing of it on this one, which has the tiles. Drawn
            // there by ds_view.cpp, from the same fit as below. 4:3 at that window's height, or its width if it's narrower.
            int second_w = 0, second_h = 0;
            if (blitz_ds_icons_on_second(&second_w, &second_h)) {
                const float picture_w = std::min(second_h * 4.0f / 3.0f, (float)second_w), picture_h = picture_w * 0.75f;
                const float x = ((second_w - picture_w) * 0.5f + picture_w * (0.5f + (sx - 256.0f) / 376.0f)) / second_w;
                const float y = ((second_h - picture_h) * 0.5f + picture_h * (sy * 1.44f / 384.0f - 34.0f / depth)) / second_h;
                blitz_ds_badge((int)k, 1, 1, std::clamp(x, 0.04f, 0.96f), std::clamp(y, 0.10f, 0.97f), glyphs_for(family)[k]);
                badge[k]->set_display(recompui::Display::None);
                continue;
            }
            // Fitted on screen (2026-10-23, Mac, 4:3): the picture's middle is x = 256, and 376 of the game's units
            // span its width; his feet are y * 1.44 / 384 of the way down; he stands 34 / depth of the picture tall.
            float px = left + game_w * (0.5f + (sx - 256.0f) / 376.0f);
            float py = h * (sy * 1.44f / 384.0f - 34.0f / depth);
            // Tall view (tall_view.cpp): the game's screen position is through its own camera; carried over to ours.
            float tx, ty;
            if (blitz_tall_2d() && blitz_tall_carry((sx - 256.0f) / 188.0f, 1.0f - sy * 1.44f / 192.0f, 0.0f, 22.0f, &tx, &ty)) {
                px = w * (0.5f + tx * 0.5f);
                py = blitz_tall_window_y(0.5f - ty * 0.5f, w, h);
                // BLITZ DS: on the screen he's on. This window shows only its part of the picture.
                float part_top, part_bottom;
                if (blitz_ds_first_part(&part_top, &part_bottom)) {
                    float y = 0.5f - ty * 0.5f;
                    bool here = y >= part_top && y < part_bottom;
                    float other_top = part_top > 0.0f ? 0.0f : part_bottom, other_bottom = part_top > 0.0f ? part_top : 1.0f;
                    float top_of = here ? part_top : other_top, bottom_of = here ? part_bottom : other_bottom;
                    // Kept just inside the screen, so the button's still there to press (as below).
                    blitz_ds_badge((int)k, 1, here ? 0 : 1, std::clamp(0.5f + tx * 0.5f, 0.04f, 0.96f), std::clamp((y - top_of) / (bottom_of - top_of), 0.10f, 0.97f), glyphs_for(family)[k]);
                    if (!here) {
                        badge[k]->set_display(recompui::Display::None);
                        continue;
                    }
                    py = h * (y - part_top) / (part_bottom - part_top);
                }
            }
            badge[k]->set_display(recompui::Display::Block);
            // A receiver off the edge keeps his icon just inside it, so the button is still there to press.
            float margin = 0.25f * badge_dp, right = w * dp - badge_dp - margin, bottom = h * dp - badge_dp - margin;
            float left_dp = std::clamp(px * dp - badge_dp * 0.5f, margin, right), top_dp = std::clamp(py * dp - badge_dp, margin, bottom);
            badge[k]->set_left(left_dp);
            badge[k]->set_top(top_dp);
        }
        context.close();
        if (!shown) {
            recompui::show_context(context, "");
            shown = true;
        }
    }

    // Settings → Rules (house_rules.cpp).
    void add_receiver_option(recomp::config::Config& c) {
        c.add_bool_option("receiver_buttons", "Receiver Buttons",
                          "Madden-style passing: while your quarterback has the ball, each receiver wears a button "
                          "(<b>△</b>, <b>R1</b>, <b>○</b> on a PlayStation controller, left to right as they line up) "
                          "and that button throws to him. <b>✕</b> still passes the game's way, where the stick points. Before the "
                          "snap, hold <b>L2</b> (Turbo) to see whose button is whose. "
                          "On a phone, the small ← ↓ → buttons around A do the same. "
                          "BLITZ DS's Stylus has them on for player 1 whatever this says.",
                          false);  // off for a new install (team: game changers are opt-in)
    }
}

// The game's hook (blitz2000.toml, ovl_a 0x802F1B78; the game thread): the receiver a human quarterback's pass goes
// to. The stick alone can't single out a receiver standing near another in its direction (iPhone, 2026-10-23: three
// taps, three balls to the same man), so a receiver button's choice replaces the game's pick outright.
extern "C" int32_t blitz_pass_target(uint8_t* rdram, int32_t qb, int32_t picked) {
    if (rdram == nullptr) return picked;
    int slot = rd(rdram, (uint32_t)qb + f_slot);
    if (slot < 0 || slot > 3) return picked;
    std::lock_guard lock{mutex};
    int target = forced[(size_t)slot];
    forced[(size_t)slot] = -1;
    if (target < 0) return picked;
    if (target != picked) fprintf(stderr, "[receivers] the game picked player %d; throwing to %d\n", picked, target);
    return target;
}
