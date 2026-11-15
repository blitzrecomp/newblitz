// BLITZ DS's answer to a finger, on the lower screen (ds_view.cpp decides what a tap means):
//   - A ring where it came down, on every tap, so one that does nothing still shows it was felt: white for felt, yellow for did
//     something, orange for refused. It reaches the screen with the next present, and its time runs from there: a stall of
//     seconds holds it back, and doesn't use it up. Off since taps work (team, 2026-11-02): BLITZ_TAP_RINGS=1 brings it back.
//   - Page, Flip, Replay and Settings at play select, on a device whose upper screen has no touch (an RG DS's: BLITZ_ONE_TOUCH,
//     BlitzActivity.java): Replay and Settings are on the drive panel up there, out of reach. One framed bar at the top left
//     (team: all of them together, on the left), clear of the right edge, where a finger rests. Replay only with one to show.
//   - The BLITZ DS title on this screen while it would be black: the game's picture on the other one alone (the loading
//     screens and menus, with the second window on top), and nothing else of ours here (team). Behind every other context.
// Its own UI context that takes no input, like the toasts'. Marks come from any thread; the drawing is the UI tick's.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

#include "SDL2/SDL.h"
#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_image.h"
#include "elements/ui_label.h"
#include "ultramodern/ultramodern.hpp"

#ifdef __ANDROID__
namespace blitz { uint64_t first_presents(); }  // fps_counter.cpp
#endif

namespace {
    using namespace recompui;
    using Clock = std::chrono::steady_clock;

    constexpr float mark_ms = 350.0f;
    constexpr float mark_size = 120.0f;  // dp: 1080 to the screen's height
    constexpr float mark_wait_most_ms = 15000.0f;  // for its first present: past this the app's away, and it isn't coming

    // The buttons, in 0-1 of the lower screen, in a bar: drawn this tall; a finger counts down to route_hit_h.
    constexpr float route_w = 0.106f, route_gap = 0.006f, route_top = 0.022f, route_h = 0.085f, route_hit_h = 0.15f;
    constexpr float bar_left = 0.012f, bar_pad = 0.006f;
    constexpr int routes_n = 4;  // in place order: Page (as Turbo), Flip (the play, as Jump does here), Replay, Settings
    constexpr int route_id[routes_n] = {4, 3, 1, 2};  // ds_touch_route_at's answers

    // Its place along the bar, Replay's left out when it's down.
    float route_left(int i, bool replay) {
        int slot = (!replay && i > 2) ? i - 1 : i;
        return bar_left + bar_pad + (float)slot * (route_w + route_gap);
    }
    int routes_up(bool replay) { return replay ? routes_n : routes_n - 1; }

    std::atomic<float> mark_x{0}, mark_y{0};
    std::atomic<int> mark_kind{0};  // ds_touch_result
    std::atomic<int> mark_seq{0};   // one more for each tap: 0, none yet

    ContextId context = ContextId::null();
    Element* ring = nullptr;
    Element* route_box[routes_n] = {nullptr, nullptr, nullptr, nullptr};
    Element* route_bar = nullptr;
    std::atomic<bool> routes_drawn{false};
    bool shown = false, drawn_mark = false, drawn_routes = false, drawn_replay = false;
    int drawn_seq = 0, drawn_kind = -1, drawn_side = -2;
    float drawn_units = 0;
    Clock::time_point mark_started{};
#ifdef __ANDROID__
    Clock::time_point mark_came{};
    uint64_t mark_presents = 0;  // how many there had been when it came
    bool mark_waiting = false;
#endif

    Element* make_route(Element* root, const char* word, Color ink, Color edge) {
        Element* box = context.create_element<Element>(root);
        box->set_position(Position::Absolute);
        box->set_height(route_h * 1080.0f);
        box->set_top(route_top * 1080.0f);
        box->set_background_color(Color{13, 26, 58, 225});
        box->set_border_width(3.0f);
        box->set_border_color(edge);
        box->set_border_radius(8.0f);
        box->set_pointer_events(PointerEvents::None);
        box->set_display(Display::None);
        Label* label = context.create_element<Label>(box, word, theme::Typography::LabelSM);
        label->set_position(Position::Absolute);
        label->set_left(0);
        label->set_right(0);
        label->set_top(0);
        label->set_bottom(0);
        label->set_text_align(TextAlign::Center);
        label->set_font_size(24.0f);  // SETTINGS fits its box (32 ran into REPLAY, 26 still touched its edges: RG DS 2026-11-02)
        label->set_line_height(route_h * 1080.0f - 6.0f);
        label->set_color(ink);
        return box;
    }

    void build() {
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
        route_bar = context.create_element<Element>(root);
        route_bar->set_position(Position::Absolute);
        route_bar->set_top((route_top - bar_pad) * 1080.0f);
        route_bar->set_height((route_h + 2.0f * bar_pad) * 1080.0f);
        route_bar->set_background_color(Color{2, 7, 18, 215});
        route_bar->set_border_width(3.0f);
        route_bar->set_border_color(Color{143, 239, 245, 140});
        route_bar->set_border_radius(10.0f);
        route_bar->set_pointer_events(PointerEvents::None);
        route_bar->set_display(Display::None);
        route_box[0] = make_route(root, "PAGE", Color{255, 255, 255, 255}, Color{143, 239, 245, 160});
        route_box[1] = make_route(root, "FLIP", Color{255, 255, 255, 255}, Color{143, 239, 245, 160});
        route_box[2] = make_route(root, "REPLAY", Color{246, 212, 0, 255}, Color{246, 212, 0, 128});
        route_box[3] = make_route(root, "SETTINGS", Color{184, 196, 218, 255}, Color{143, 239, 245, 97});
        ring = context.create_element<Element>(root);
        ring->set_position(Position::Absolute);
        ring->set_width(mark_size);
        ring->set_height(mark_size);
        ring->set_border_radius(mark_size * 0.5f);
        ring->set_border_width(8.0f);
        ring->set_pointer_events(PointerEvents::None);
        ring->set_display(Display::None);
        context.close();
    }

    // Context open.
    void draw(bool mark_on, float age, float units, bool routes, bool replay, bool place_routes, int my_side) {
        (void)my_side;
        if (mark_on != drawn_mark) {
            ring->set_display(mark_on ? Display::Block : Display::None);
            drawn_mark = mark_on;
        }
        if (mark_on) {
            const int kind = mark_kind.load();
            if (kind != drawn_kind) {
                ring->set_border_color(kind == 1 ? Color{255, 225, 74, 255} : kind == 2 ? Color{255, 106, 61, 255} : Color{255, 255, 255, 220});
                drawn_kind = kind;
            }
            ring->set_left(mark_x.load() * units - mark_size * 0.5f);
            ring->set_top(mark_y.load() * 1080.0f - mark_size * 0.5f);
            ring->set_opacity(1.0f - age / mark_ms);
        }
        if (routes != drawn_routes || replay != drawn_replay) {
            for (int i = 0; i < routes_n; i++) route_box[i]->set_display(routes && (i != 2 || replay) ? Display::Block : Display::None);
            route_bar->set_display(routes ? Display::Block : Display::None);
            drawn_routes = routes;
            drawn_replay = replay;
            routes_drawn = routes;
            place_routes = true;
        }
        if (place_routes) {
            for (int i = 0; i < routes_n; i++) {
                route_box[i]->set_left(route_left(i, replay) * units);
                route_box[i]->set_width(route_w * units);
            }
            const int n = routes_up(replay);
            route_bar->set_left(bar_left * units);
            route_bar->set_width(((float)n * route_w + (float)(n - 1) * route_gap + 2.0f * bar_pad) * units);
            drawn_side = my_side;
            drawn_units = units;
        }
    }
}

namespace {
    ContextId backdrop = ContextId::null();
    bool backdrop_shown = false;
    float backdrop_units = 0;
    Element* backdrop_logo = nullptr;
    Label* backdrop_word = nullptr;
    Label* backdrop_status = nullptr;  // LOADING / PRESS START under the title
    std::string backdrop_line;

    void build_backdrop() {
        backdrop = create_context();
        backdrop.open();
        backdrop.set_captures_input(false);
        backdrop.set_captures_mouse(false);
        Element* root = backdrop.create_element<Element>(backdrop.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(0);
        root->set_left(0);
        root->set_right(0);
        root->set_bottom(0);
        root->set_background_color(Color{2, 7, 18, 255});
        root->set_pointer_events(PointerEvents::None);
        Element* grid = backdrop.create_element<Element>(root);
        grid->set_position(Position::Absolute);
        grid->set_top(0);
        grid->set_left(0);
        grid->set_right(0);
        grid->set_bottom(0);
        grid->set_opacity(0.22f);
        grid->set_decorator("image(?/blitz/ui/grid_tile repeat)");
        backdrop_logo = backdrop.create_element<Image>(root, "?/blitz/ui/logo27_lockup");
        backdrop_logo->set_position(Position::Absolute);
        backdrop_word = backdrop.create_element<Label>(root, "BLITZ DS", theme::Typography::Header1);
        backdrop_word->set_position(Position::Absolute);
        backdrop_word->set_left(0);
        backdrop_word->set_right(0);
        backdrop_word->set_top(530.0f);
        backdrop_word->set_text_align(TextAlign::Center);
        backdrop_word->set_color(Color{246, 212, 0, 255});
        backdrop_status = backdrop.create_element<Label>(root, "", theme::Typography::Header3);
        backdrop_status->set_position(Position::Absolute);
        backdrop_status->set_left(0);
        backdrop_status->set_right(0);
        backdrop_status->set_top(800.0f);
        backdrop_status->set_text_align(TextAlign::Center);
        backdrop_status->set_font_size(64.0f);
        backdrop_status->set_color(Color{143, 239, 245, 255});
        backdrop.close();
    }
}

namespace blitz {
    // Every UI tick (ds_update). want: the game's picture is only on the other screen and nothing of ours is on this one.
    void ds_backdrop_update(SDL_Window* window, bool want, const char* line) {
        if (backdrop_shown && !recompui::is_context_shown(backdrop)) backdrop_shown = false;
        if (!want) {
            if (backdrop_shown) recompui::hide_context(backdrop);
            backdrop_shown = false;
            return;
        }
        if (backdrop == ContextId::null()) build_backdrop();
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        const float units = h > 0 ? 1080.0f * (float)w / (float)h : 1440.0f;
        if (units != backdrop_units) {
            // As the second screen's title (ds_view.cpp panel_title): the lockup over BLITZ DS.
            backdrop_units = units;
            const float logo_w = std::min(1100.0f, units * 0.85f), logo_h = logo_w * 96.0f / 738.0f;
            backdrop.open();
            backdrop_logo->set_left((units - logo_w) * 0.5f);
            backdrop_logo->set_top(473.0f - logo_h);
            backdrop_logo->set_width(logo_w);
            backdrop_logo->set_height(logo_h);
            backdrop_word->set_font_size(std::min(190.0f, units / 5.0f));
            backdrop.close();
        }
        if (line != nullptr && backdrop_line != line) {
            backdrop_line = line;
            backdrop.open();
            backdrop_status->set_text(backdrop_line);
            backdrop.close();
        }
        if (!backdrop_shown) {
            recompui::show_context(backdrop, "");
            backdrop.get_document()->PushToBack();  // the toasts, the FPS counter and the menu stay over it
            backdrop_shown = true;
        }
    }

    // A finger came down at x, y (0-1) on the lower screen. Any thread.
    void ds_touch_mark(float x, float y) {
        mark_kind = 0;
        mark_x = x;
        mark_y = y;
        mark_seq.fetch_add(1);
    }

    // What the latest tap came to: 0 only felt, 1 did something, 2 refused. Any thread.
    void ds_touch_result(int kind) { mark_kind = kind; }

    // Which button is at x, y (0-1): 1 Replay, 2 Settings, 3 Flip, 4 Page, 0 none. replay: Replay's is up.
    int ds_touch_route_at(float x, float y, bool replay, int my_side) {
        (void)my_side;
        if (y > route_hit_h) return 0;
        for (int i = 0; i < routes_n; i++) {
            if (i == 2 && !replay) continue;
            const float left = route_left(i, replay);
            if (x >= left - route_gap * 0.5f && x <= left + route_w + route_gap * 0.5f) return route_id[i];
        }
        return 0;
    }

    // The bar's up (touch_controls.cpp leaves out its own PAGE pill then). Any thread.
    bool ds_touch_routes_up() { return routes_drawn.load(); }

    // Every UI tick (ds_update). routes: Settings is up now, and replay: Replay is too. my_side: the half that's player 1's.
    void ds_touch_update(SDL_Window* window, bool routes, bool replay, int my_side) {
        const int seq = mark_seq.load();
        const Clock::time_point now = Clock::now();
        if (seq != drawn_seq) {
            drawn_seq = seq;
            mark_started = now;
#ifdef __ANDROID__
            mark_came = now;
            mark_presents = blitz::first_presents();
            mark_waiting = true;
#endif
        }
#ifdef __ANDROID__
        // The UI's only drawn in a present, so nothing's been seen until one starts after this tap: the second from now. A stall
        // on the renderer stops those, and the UI's tick goes on, so the clock waits.
        if (mark_waiting) {
            mark_started = now;
            if (blitz::first_presents() >= mark_presents + 2 || std::chrono::duration<float, std::milli>(now - mark_came).count() >= mark_wait_most_ms) mark_waiting = false;
        }
#endif
        const float age = std::chrono::duration<float, std::milli>(now - mark_started).count();
        static const bool rings = getenv("BLITZ_TAP_RINGS") != nullptr && atoi(getenv("BLITZ_TAP_RINGS")) != 0;
        const bool mark_on = rings && seq != 0 && age < mark_ms;
        // Hiding every context (the launcher's buttons, Blitz Builder's) takes this one too, and hiding it again is an error dialog.
        if (shown && !recompui::is_context_shown(context)) shown = false;
        // A touch on the splash, before there's any UI to draw with, is only logged (tap_watch).
        if ((!mark_on && !routes) || !(ultramodern::is_game_started() || recompui::is_context_capturing_input())) {
            if (shown) recompui::hide_context(context);
            shown = false;
            routes_drawn = false;
            return;
        }
        if (context == ContextId::null()) build();
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        const float units = h > 0 ? 1080.0f * (float)w / (float)h : 1440.0f;
        const bool place_routes = routes && (my_side != drawn_side || units != drawn_units);
        // The buttons sit still for a whole play select: no taking the UI's lock each tick for nothing.
        if (mark_on || mark_on != drawn_mark || routes != drawn_routes || replay != drawn_replay || place_routes) {
            context.open();
            draw(mark_on, age, units, routes, replay, place_routes, my_side);
            context.close();
        }
        if (!shown && !recompui::is_context_shown(context)) recompui::show_context(context, "");
        shown = true;
    }
}
