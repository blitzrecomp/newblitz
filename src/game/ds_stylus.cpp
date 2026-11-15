// BLITZ DS's stylus mode (Settings → General → DS Screens): the game on the upper screen, and on the lower big tiles for the
// right hand, in the house look (ui_kit), that change with the moment as the touch and phone controllers do (game_moment.h).
// The left hand stays on the controller: it moves and holds turbo. Before the throw: the three receivers as wedges of a fan,
// each wearing its button's icon (the one over the man, receiver_targets.cpp), and Jump and Pass under it. With the ball after
// that: Jump and Stiff Arm. On defense: Tackle and Switch. Two tiles go one above the other, narrow, with room on the right
// (team). Play select has the game's own tiles, tapped
// (ds_view.cpp). Without a controller the on-screen touch controls stay instead (touch_controls.cpp).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "SDL2/SDL.h"
#include "recomp.h"
#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_image.h"
#include "elements/ui_label.h"
#include "../main/game_moment.h"
#include "../main/ui_kit.h"
#include "ultramodern/ultramodern.hpp"

extern "C" int blitz_ds_active(void);          // ds_view.cpp
extern "C" volatile int blitz_ds_screen_mode;  // tall_view.cpp: 0 both screens, 1 game + companion, 2 stylus

namespace blitz {
    uint8_t* monitor_rdram();
    std::array<std::string, 3> receiver_glyphs();  // receiver_targets.cpp
}

namespace {
    using namespace recompui;
    constexpr uint16_t btn_a = 0x8000, btn_b = 0x4000, c_left = 0x0002, c_down = 0x0004, c_right = 0x0001;
    constexpr uint32_t play_phase = 0x802EA4A8, passer = 0x802EA4C0, ball_owner = 0x80364D20;  // tall_view.cpp, receiver_targets.cpp

    struct Tile {
        Element* box = nullptr;
        Label* word = nullptr;
        Label* small = nullptr;
        uint16_t mask = 0;
        float x = 0, y = 0, w = 0, h = 0;
        bool on = false, hot = false;
    };
    std::array<Tile, 5> tiles;
    ContextId context = ContextId::null();
    bool shown = false, built = false;
    std::atomic<bool> showing{false};
    Label* heading = nullptr;
    float width_units = 1284.0f;
    int drawn_layout = -1;
    float drawn_width = 0;

    std::mutex mutex;
    std::map<SDL_FingerID, uint16_t> fingers;
    std::atomic<uint16_t> held{0}, latched{0};
    std::array<Tile, 5> hit;  // the places to touch, under the mutex (the event thread reads them)

    // The fan: a half wheel, hub at the bottom middle, three wedges of 60 degrees: the left receiver from 9 to 11 o'clock, the
    // middle 11 to 1, the right 1 to 3. Drawn here, once dim and once with each wedge lit.
    constexpr int fan_w = 512, fan_h = 256;
    std::array<Image*, 4> fan_art{};  // none lit, then each wedge
    std::array<Label*, 3> fan_glyph{};
    int fan_lit = -2;
    struct Fan {
        bool on = false;
        float cx = 0, cy = 0, r = 0;  // the hub and the radius, in UI units
    };
    Fan fan, fan_hit;  // drawn, and the event thread's copy (under the mutex)
    constexpr std::array<uint16_t, 3> fan_masks{c_left, c_down, c_right};

    float mix(float a, float b, float t) { return a + (b - a) * t; }

    // Which wedge a point in the half wheel is in (0 left, 1 middle, 2 right), from its angle around the hub; -1 outside.
    int wedge_at(float dx, float dy, float r) {
        float d = std::sqrt(dx * dx + dy * dy);
        if (dy > 0.0f || d > r || d < r * 0.30f) return -1;
        float deg = std::atan2(dx, -dy) * 57.29578f;  // 0 straight up, -90 left, 90 right
        return deg < -30.0f ? 0 : deg <= 30.0f ? 1 : 2;
    }

    // Flame from the hub out, dim until its wedge is pressed, chrome between the wedges and round the rim.
    std::vector<char> fan_pixels(int lit) {
        std::vector<char> out((size_t)fan_w * fan_h * 4, 0);
        const float c = (fan_w - 1) * 0.5f, r = (float)fan_h - 1.0f;
        for (int y = 0; y < fan_h; y++) {
            for (int x = 0; x < fan_w; x++) {
                float dx = (x - c) / r, dy = (y - r) / r, d = std::sqrt(dx * dx + dy * dy);
                float R = 0, G = 0, B = 0, A = 0;
                if (d <= 1.0f && dy <= 0.0f) {
                    float deg = std::atan2(dx, -dy) * 57.29578f;
                    int wedge = deg < -30.0f ? 0 : deg <= 30.0f ? 1 : 2;
                    float to_edge = std::min(std::fabs(deg + 30.0f), std::fabs(deg - 30.0f)) / 57.29578f * d;
                    if (d > 0.93f) {  // the chrome rim, light from above
                        float v = mix(0.55f, 0.95f, -dy) * (1.0f - std::fabs((d - 0.965f) / 0.035f) * 0.4f);
                        R = G = v;
                        B = v * 1.06f;
                        A = 1.0f;
                    }
                    else if (d > 0.30f) {
                        if (to_edge < 0.018f) {
                            R = G = 0.62f;
                            B = 0.68f;
                        }
                        else {
                            float t = (d - 0.30f) / 0.63f;  // hot at the hub, red at the rim
                            float k = wedge == lit ? 1.0f : 0.42f;
                            R = mix(1.0f, 0.70f, t) * k;
                            G = mix(0.88f, 0.06f, t) * k;
                            B = mix(0.29f, 0.12f, t) * k;
                        }
                        A = 1.0f;
                    }
                    else if (d > 0.27f) {  // the hub's chrome edge
                        R = 0.75f;
                        G = 0.77f;
                        B = 0.82f;
                        A = 1.0f;
                    }
                }
                size_t o = (size_t)(y * fan_w + x) * 4;
                out[o] = (char)std::min(255.0f, R * 255.0f);
                out[o + 1] = (char)std::min(255.0f, G * 255.0f);
                out[o + 2] = (char)std::min(255.0f, B * 255.0f);
                out[o + 3] = (char)(A * 255.0f);
            }
        }
        return out;
    }

    enum Layout { Throw, Carry, Defend };

    void build() {
        for (int i = 0; i < 4; i++) {
            std::vector<char> art = fan_pixels(i - 1);
            recompui::queue_image_from_bytes_rgba32("?/blitz/stylus_fan" + std::to_string(i), art, fan_w, fan_h);
        }
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
        root->set_background_color(Color{2, 7, 18, 255});
        root->set_pointer_events(PointerEvents::None);
        heading = context.create_element<Label>(root, "", theme::Typography::Header1);
        heading->set_position(Position::Absolute);
        heading->set_left(0);
        heading->set_right(0);
        heading->set_top(26.0f);
        heading->set_text_align(TextAlign::Center);
        heading->set_font_size(84.0f);
        heading->set_color(Color{255, 225, 74, 255});
        Element* rule = context.create_element<Element>(root, 0, "div", false);
        rule->set_position(Position::Absolute);
        rule->set_left(40.0f);
        rule->set_right(40.0f);
        rule->set_top(122.0f);
        rule->set_height(6.0f);
        blitz::ui::stretch(context, rule, "rule_flame");
        for (Tile& t : tiles) {
            t.box = context.create_element<Element>(root);
            t.box->set_position(Position::Absolute);
            t.box->set_pointer_events(PointerEvents::None);
            blitz::ui::PanelOptions o;
            o.absolute = true;
            o.corners = true;
            o.corner_size = 26.0f;
            Element* content = blitz::ui::panel(context, t.box, o);
            content->set_height(100.0f, Unit::Percent);
            content->set_display(Display::Flex);
            content->set_flex_direction(FlexDirection::Column);
            content->set_justify_content(JustifyContent::Center);
            content->set_align_items(AlignItems::Center);
            t.word = context.create_element<Label>(content, "", theme::Typography::Header1);
            t.word->set_color(Color{255, 255, 255, 255});
            t.small = context.create_element<Label>(content, "", theme::Typography::LabelSM);
            t.small->set_color(Color{143, 239, 245, 255});
            t.small->set_font_size(34.0f);
            t.small->set_margin_top(14.0f);
            t.box->set_display(Display::None);
        }
        for (int i = 0; i < 4; i++) {
            Image* img = context.create_element<Image>(root, "?/blitz/stylus_fan" + std::to_string(i));
            img->set_position(Position::Absolute);
            img->set_pointer_events(PointerEvents::None);
            img->set_display(Display::None);
            fan_art[(size_t)i] = img;
        }
        for (Label*& g : fan_glyph) {
            g = context.create_element<Label>(root, "", theme::Typography::LabelSM);
            g->set_font_family("promptfont");
            g->set_position(Position::Absolute);
            g->set_text_align(TextAlign::Center);
            g->set_color(Color{255, 255, 255, 255});
            g->set_pointer_events(PointerEvents::None);
            g->set_display(Display::None);
        }
        built = true;
        context.close();
    }

    void show_fan(bool on, float cx, float cy, float r) {
        fan.on = on;
        fan.cx = cx;
        fan.cy = cy;
        fan.r = r;
        fan_lit = -2;  // redrawn by the next tick
        for (Image* img : fan_art) {
            img->set_display(Display::None);
            img->set_left(cx - r);
            img->set_top(cy - r);
            img->set_width(2.0f * r);
            img->set_height(r);
        }
        std::array<std::string, 3> glyphs = blitz::receiver_glyphs();
        const float size = r * 0.36f, reach = r * 0.64f;
        for (int k = 0; k < 3; k++) {
            Label* g = fan_glyph[(size_t)k];
            g->set_display(on ? Display::Block : Display::None);
            if (!on) continue;
            float a = (-60.0f + 60.0f * (float)k) / 57.29578f;  // each wedge's middle: -60, 0, 60 degrees from straight up
            float x = cx + std::sin(a) * reach, y = cy - std::cos(a) * reach;
            g->set_text(glyphs[(size_t)k]);
            g->set_font_size(size);
            g->set_line_height(size * 1.2f);
            g->set_left(x - size);
            g->set_width(2.0f * size);
            g->set_top(y - size * 0.6f);
        }
    }

    void set_tile(Tile& t, const char* word, const char* small, uint16_t mask, float x, float y, float w, float h, float size) {
        t.on = true;
        t.mask = mask;
        t.x = x;
        t.y = y;
        t.w = w;
        t.h = h;
        t.box->set_display(Display::Block);
        t.box->set_left(x);
        t.box->set_top(y);
        t.box->set_width(w);
        t.box->set_height(h);
        t.word->set_text(word);
        t.word->set_font_size(size);
        t.small->set_text(small);
    }

    void lay_out(Layout layout, const blitz::MomentText& words) {
        for (Tile& t : tiles) {
            t.on = false;
            t.box->set_display(Display::None);
        }
        // A wide strip down the right with nothing in it, to rest a finger on, and a little room on the left (team).
        const float m = 28.0f, top = 150.0f, bottom = 1056.0f;
        const float x0 = width_units * 0.07f;
        if (layout == Throw) {
            heading->set_text("THROW IT");
            // The fan as wide as it can be under the heading, and Jump and Pass side by side below it.
            const float W = width_units * 0.66f, r = std::min(W * 0.5f, (bottom - top) * 0.58f);
            const float cx = x0 + W * 0.5f, cy = top + r;
            show_fan(true, cx, cy, r);
            const float half = (W - m) * 0.5f, split = cy + m;
            set_tile(tiles[3], words.jump.label, words.jump.small, btn_b, x0, split, half, bottom - split, 84.0f);
            set_tile(tiles[4], words.pass.label, "nearest", btn_a, x0 + half + m, split, half, bottom - split, 84.0f);
        }
        else {
            show_fan(false, 0, 0, 0);
            // One above the other, narrow.
            const float W = width_units * 0.52f, half = (bottom - top - m) * 0.5f;
            heading->set_text(layout == Carry ? "BREAK IT" : "STOP HIM");
            set_tile(tiles[3], words.jump.label, words.jump.small, btn_b, x0, top, W, half, 104.0f);
            if (layout == Carry) set_tile(tiles[4], "STIFF ARM", "with turbo", btn_a, x0, top + half + m, W, half, 92.0f);
            else set_tile(tiles[4], words.pass.label, words.pass.small, btn_a, x0, top + half + m, W, half, 104.0f);
        }
        std::lock_guard lock{mutex};
        hit = tiles;
        fan_hit = fan;
    }

    // Mutex held.
    uint16_t press_at(float ux, float uy) {
        if (fan_hit.on) {
            int w = wedge_at(ux - fan_hit.cx, uy - fan_hit.cy, fan_hit.r);
            if (w >= 0) return fan_masks[(size_t)w];
        }
        for (const Tile& t : hit) {
            if (t.on && ux >= t.x && ux <= t.x + t.w && uy >= t.y && uy <= t.y + t.h) return t.mask;
        }
        return 0;
    }

    int on_event(void*, SDL_Event* e) {
        if (e->type != SDL_FINGERDOWN && e->type != SDL_FINGERMOTION && e->type != SDL_FINGERUP) return 1;
        std::lock_guard lock{mutex};
        if (!showing.load()) {
            fingers.clear();
            held = 0;
            return 1;
        }
        if (e->type == SDL_FINGERUP) fingers.erase(e->tfinger.fingerId);
        else {
            uint16_t p = press_at(e->tfinger.x * width_units, e->tfinger.y * 1080.0f);
            auto it = fingers.find(e->tfinger.fingerId);
            if (it == fingers.end() || it->second != p) latched = (uint16_t)(latched.load() | p);
            fingers[e->tfinger.fingerId] = p;
        }
        uint16_t now = 0;
        for (const auto& [id, mask] : fingers) now |= mask;
        held = now;
        return 1;
    }
}

namespace blitz {
    bool stylus_showing() { return showing.load(); }

    // Game thread, player 1's read (controllers.cpp).
    void stylus_read(uint16_t* buttons) {
        if (!showing.load()) return;
        *buttons |= (uint16_t)(held.load() | latched.exchange(0));
    }

    // Every UI tick (main.cpp).
    void stylus_update(SDL_Window* window) {
        static bool watching = false;
        GameMoment moment = moment_for(0);
        bool play = moment == GameMoment::Offense || moment == GameMoment::Defense || moment == GameMoment::Play;
        bool want = blitz_ds_active() && blitz_ds_screen_mode == 2 && SDL_NumJoysticks() > 0 && ultramodern::is_game_started() &&
                    play && !recompui::is_context_capturing_input();
        showing = want;
        if (!want) {
            if (shown) recompui::hide_context(context);
            shown = false;
            return;
        }
        if (!watching) {
            watching = true;
            SDL_AddEventWatch(on_event, nullptr);
        }
        if (!built) build();
        int w = 0, h = 0;
        if (window) SDL_GetWindowSizeInPixels(window, &w, &h);
        if (h > 0) width_units = 1080.0f * (float)w / (float)h;
        // With the ball and able to throw: the receivers. A pass in the air or after it: as on the touch controls, what's next.
        Layout layout = Defend;
        if (moment != GameMoment::Defense) {
            layout = Carry;
            uint8_t* rdram = monitor_rdram();
            if (rdram != nullptr) {
                int phase = MEM_W(0, (gpr)(int32_t)play_phase), who = MEM_W(0, (gpr)(int32_t)passer), owner = MEM_W(0, (gpr)(int32_t)ball_owner);
                if (phase <= 1 || (who >= 0 && owner == who)) layout = Throw;
            }
        }
        uint16_t now = held.load();
        context.open();
        if ((int)layout != drawn_layout || width_units != drawn_width) {
            lay_out(layout, moment_text(moment));
            drawn_layout = (int)layout;
            drawn_width = width_units;
        }
        int lit = -1;
        for (int k = 0; k < 3; k++) {
            if (fan.on && (now & fan_masks[(size_t)k]) != 0) lit = k;
        }
        if (lit != fan_lit) {
            for (int i = 0; i < 4; i++) fan_art[(size_t)i]->set_display(fan.on && i == lit + 1 ? Display::Block : Display::None);
            for (int k = 0; k < 3; k++) fan_glyph[(size_t)k]->set_color(k == lit ? Color{255, 225, 74, 255} : Color{255, 255, 255, 255});
            fan_lit = lit;
        }
        for (Tile& t : tiles) {
            bool hot = t.on && (now & t.mask) != 0;
            if (hot != t.hot) {
                t.hot = hot;
                blitz::ui::set_panel_frame(context, t.box, hot ? "panel_frame_hot" : "panel_frame");
                t.word->set_color(hot ? Color{255, 225, 74, 255} : Color{255, 255, 255, 255});
            }
        }
        context.close();
        if (!shown) recompui::show_context(context, "");
        shown = true;
    }
}
