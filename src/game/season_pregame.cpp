#include <SDL.h>
// Season's pre-game screen (New Blitz with the 2026 Season pak; Standard and Advanced seasons). Season reaches team select
// only at a new season, so a week's game had no place to pick uniforms or a crew: the team, 2026-11-16, "add a uniform picker
// before a game starts on Season mode... it can just lock in both teams and you can pick the unis there... square button
// would bring you back to start game".
//
// SEASON → PLAY WEEK → PLAY runs main func_802636AC as the dialog's handler (a process of its own; the dialog waits on
// 0x8028BD40 until the handler clears it). Hooked at its first instruction (tools/engine32_toml.py), before its frame:
//   - the week's games come from the game's own week builder (ovl_b 802FDEF4; Advanced's is host code behind it). A week
//     with no game for the player (a bye, out of the playoffs, the season over) plays as before, no screen.
//   - otherwise the handler ends at once (the dialog is back on PLAY / SIMULATE, as if nothing was chosen) and the screen
//     comes up: both teams locked in (the player's is always the game's left side, 802FD094), a row of uniform sets for
//     each, Pick Your Crew (crew.cpp's panel, for the player's team and then the CPU's), Back and Play.
//   - Back (□, or ○) closes it: the dialog is still on PLAY. Play (or Start) closes it and presses A for the player, so the
//     dialog runs the handler again; this time the hook lets it through and puts the picks in (uniform_picker.cpp, as △
//     at team select would; the kickoff applies them).
// Buttons still held when it closes are kept from the game until they're let go, so the □ that closed it isn't the
// dialog's Back too.
//
// No game state is touched while the screen is up: nothing to save or restore beyond what the picker already keeps.
#if BLITZ_MODERN
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "recomp.h"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "librecomp/game.hpp"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_button.h"
#include "ultramodern/ultramodern.hpp"
#include "../main/ui_kit.h"

extern "C" {
    void ovl_b_func_802FDEF4(uint8_t* rdram, recomp_context* ctx);  // the week builder: (week, out)
    extern int blitz_adv;  // adv_mode.cpp: an Advanced season
    extern int blitz_s32;  // season32.cpp: a 32-team Standard season
}

namespace blitz {
    bool original_game();                       // adv_mode.cpp: Original 1999
    std::string active_pack_id();               // content_pack_tp.cpp
    bool league_active();
    int team_of_slot(int slot);
    std::vector<std::string> team_franchises();
    std::string team_display_name(const std::string& franchise);
    void inject_press(int port, uint16_t buttons, int ms);  // dev_input.cpp
    SDL_GameController* pad_on_port(int port);  // controllers.cpp
    uint8_t* monitor_rdram();                   // dev_monitor.cpp
    std::vector<std::pair<int, std::string>> uniform_choices(int slot);  // uniform_picker.cpp
    int uniform_current(int side, int slot);
    bool uniform_picker_on();
    void uniform_season_pick(uint8_t* rdram, int side, int slot, int set);
    bool crew_pregame_start(const std::string& mine, const std::string& cpu);  // crew.cpp
    int crew_pregame_state();
}

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr uint32_t game_mode = 0x8028B998;      // 2 Season
    constexpr uint32_t rec_team = 0x802EF6F0;       // the season record: the player's team
    constexpr uint32_t rec_week = 0x802EF6F1;       //   and the week (0-based; 0xFF no season)
    constexpr uint32_t dialog_busy = 0x8028BD40;    // the dialog waits while its handler runs; the handler clears it
    constexpr uint32_t idle_left = 0x8028BB60;      // the menus' idle countdown (60, about a second a step)
    constexpr uint32_t builder_out = 0x802E9508;    // the week builder's buffer (the driver fills it again first)
    constexpr uint32_t div_map = 0x8032E898, grid = 0x8033BB00;  // the driver's grid build, for the builder
    // ovl_b is in RDRAM (as season32.cpp checks): the builder's code is there.
    constexpr uint32_t resident_a = 0x803015CC, resident_a_word = 0x080C057F;
    constexpr uint32_t resident_b = 0x802FBAAC, resident_b_word = 0x27BDFFE0;
    constexpr uint16_t n64_a = 0x8000;

    enum State : int { Idle, Requested, Open, Go };
    std::atomic<int> state{Idle};
    std::atomic<int64_t> state_ms{0};  // when the state last changed (or the screen was last seen up)
    int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
    int64_t state_age_ms() { return now_ms() - state_ms.load(); }

    struct Game {
        int slot[2] = {-1, -1};   // side 0 the player's team, 1 the CPU's (game slots)
        std::string f[2];         // their franchises
        int week = 0;
        int port = 0;             // the controller the season dialog reads
        int pick[2] = {-1, -1};   // the sets chosen here (uniform_picker.cpp's numbers)
    } game;
    std::mutex game_mutex;

    // Buttons held when the screen closes, kept from the game until let go (game thread reads, main thread arms).
    std::atomic<uint16_t> latch[4] = {0, 0, 0, 0};

    bool uniforms_on() { return blitz::uniform_picker_on(); }
    bool crew_on() {
        return std::get<bool>(recompui::config::get_config("rules").get_option_value("pick_crew"));
    }

    gpr guest(uint32_t a) { return (gpr)(int32_t)a; }

    // The week's game for the player: false for none (a bye, out of the playoffs, season over).
    bool find_game(uint8_t* rdram, recomp_context* ctx, int user, int week, Game& out) {
        if ((uint32_t)MEM_W(0, guest(resident_a)) != resident_a_word || (uint32_t)MEM_W(0, guest(resident_b)) != resident_b_word) {
            fprintf(stderr, "[pregame] ovl_b isn't loaded: no screen this time\n");
            return false;
        }
        if (!blitz_adv) {  // the driver's grid (team by division and slot), which the game's builder reads
            int teams = blitz_s32 ? 32 : 31;
            for (int t = 0; t < teams; t++) {
                int d = MEM_B(0, guest(div_map + 2 * (uint32_t)t)), s = MEM_B(0, guest(div_map + 2 * (uint32_t)t + 1));
                if (d < 0 || d > 5 || s < 0 || s > 5) continue;
                MEM_W(0, guest(grid + (uint32_t)(d * 24 + s * 4))) = t;
            }
        }
        recomp_context saved = *ctx;
        ctx->r4 = week;
        ctx->r5 = (int32_t)builder_out;
        ovl_b_func_802FDEF4(rdram, ctx);
        *ctx = saved;
        for (uint32_t i = 0; i < 18; i++) {
            int a = MEM_B(0, guest(builder_out + 6 * i)), b = MEM_B(0, guest(builder_out + 6 * i + 1));
            uint8_t played = (uint8_t)MEM_B(0, guest(builder_out + 6 * i + 2));
            if (a < 0 || b < 0 || played != 0xFF) continue;
            if (a != user && b != user) continue;
            out.slot[0] = user;
            out.slot[1] = a == user ? b : a;
            return true;
        }
        return false;
    }

    std::string franchise(int slot) {
        const auto& f = blitz::team_franchises();
        int t = slot >= 0 ? blitz::team_of_slot(slot) : -1;
        return t >= 0 && (size_t)t < f.size() ? f[(size_t)t] : "";
    }

    std::string upper(std::string s) {
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }
}

namespace blitz {
    // Game thread (dev_input.cpp's get_input, before injected presses): the latch above.
    void pregame_input_filter(int port, uint16_t* buttons) {
        if (port < 0 || port > 3) return;
        // While the screen is up the menus see no buttons, and after 60 seconds of none the game's idle_proc (main
        // 0x8025A344) would leave for the attract demo behind it: each read is activity, as a menu move is (0x8025A454).
        int st = state.load();
        if (port == 0 && (st == Requested || st == Open)) {
            if (uint8_t* rdram = monitor_rdram()) MEM_W(0, guest(idle_left)) = 0x3C;
        }
        uint16_t m = latch[port].load();
        if (m == 0) return;
        uint16_t held = *buttons & m;
        *buttons &= (uint16_t)~m;
        latch[port] = held;
    }
}

extern "C" int blitz_season_pregame(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_W(0, guest(game_mode)) != 2 || blitz::original_game() || blitz::league_active() ||
        blitz::active_pack_id() != "blitz2026" || (!uniforms_on() && !crew_on())) {
        return 0;
    }
    int user = (uint8_t)MEM_B(0, guest(rec_team)), week = (uint8_t)MEM_B(0, guest(rec_week));
    if (user >= 32 || week >= 22) return 0;
    int st = state.load();
    if (st == Go) {  // Play on the screen: this is its press
        state = Idle;
        std::lock_guard lock{game_mutex};
        if (game.slot[0] == user && game.week == week && state_age_ms() < 5000) {
            if (uniforms_on()) {
                for (int s = 0; s < 2; s++) blitz::uniform_season_pick(rdram, s, game.slot[s], game.pick[s]);
            }
            fprintf(stderr, "[pregame] play: %s v %s, week %d\n", game.f[0].c_str(), game.f[1].c_str(), week + 1);
            return 0;
        }
    }
    else if (st == Requested || st == Open) {  // the screen is up (its input isn't the game's): carry on
        if (state_age_ms() < 10000) {
            MEM_W(0, guest(dialog_busy)) = 0;
            return 1;
        }
        state = Idle;
        return 0;
    }
    Game g;
    if (!find_game(rdram, ctx, user, week, g)) return 0;
    g.week = week;
    int32_t arg = (int32_t)ctx->r4;
    g.port = (uint32_t)arg >= 0x80000000u && (uint32_t)arg < 0x80800000u ? MEM_W(0, guest((uint32_t)arg)) : 0;
    if (g.port < 0 || g.port > 3) g.port = 0;
    for (int s = 0; s < 2; s++) {
        g.f[s] = franchise(g.slot[s]);
        g.pick[s] = blitz::uniform_current(s, g.slot[s]);
    }
    if (g.f[0].empty() || g.f[1].empty()) return 0;
    {
        std::lock_guard lock{game_mutex};
        if (game.week == week && game.slot[0] == g.slot[0] && game.slot[1] == g.slot[1]) {  // back after Back: as it was left
            g.pick[0] = game.pick[0];
            g.pick[1] = game.pick[1];
        }
        game = g;
    }
    state_ms = now_ms();
    state = Requested;
    MEM_W(0, guest(dialog_busy)) = 0;  // the dialog takes input again (its PLAY / SIMULATE)
    fprintf(stderr, "[pregame] week %d: %s v %s (pad %d): the pre-game screen\n", week + 1, g.f[0].c_str(), g.f[1].c_str(),
            g.port);
    return 1;
}

// ---- The screen (main thread) ----------------------------------------------------------------------------------------

namespace {
    struct Choice {
        int side, set;
        recompui::Button* button;
    };
    struct Ui {
        recompui::ContextId context = recompui::ContextId::null();
        recompui::Element* card = nullptr;
        recompui::Label* eyebrow = nullptr;
        recompui::Label* title = nullptr;
        recompui::Element* body = nullptr;
        recompui::Element* bar = nullptr;
        std::vector<Choice> choices;
        std::function<void()> play;
        bool shown = false;
        bool crew_up = false;  // Pick Your Crew's panel is over it
        bool rebuild = false;  // a pick: the page again, focus on refocus (side, set)
        std::pair<int, int> refocus{-9, -9};
    } pg;

    void back();
    void play();

    class Root : public recompui::Element {
    protected:
        std::string_view get_type_name() override { return "PregameRoot"; }
        void process_event(const recompui::Event& e) override {
            if (e.type == recompui::EventType::MenuAction &&
                std::get<recompui::EventMenuAction>(e.variant).action == recompui::MenuAction::Back) {
                back();
            }
        }
    public:
        Root(recompui::ResourceId rid, recompui::Element* parent)
            : Element(rid, parent, recompui::Events(recompui::EventType::MenuAction), "div", false) {}
    };

    class Hints : public recompui::Element {
        recompinput::InputDevice shown_device = recompinput::InputDevice::COUNT;
        int shown_profile = -2;
    protected:
        std::string_view get_type_name() override { return "PregameHints"; }
        void process_event(const recompui::Event& e) override {
            if (e.type != recompui::EventType::Update) return;
            auto [device, profile] = recompui::hint_input_info();
            if (device != shown_device || profile != shown_profile) {
                shown_device = device;
                shown_profile = profile;
                recompui::build_hint_row(recompui::get_current_context(), this, {
                    {{recompui::MenuAction::Accept}, "Pick"},
                    {{recompui::MenuAction::Back}, "Back"},
                }, device, profile);
            }
            queue_update();
        }
    public:
        Hints(recompui::ResourceId rid, recompui::Element* parent)
            : Element(rid, parent, recompui::Events(recompui::EventType::Update), "div", false) {
            using namespace recompui;
            set_position(Position::Absolute);
            set_left(0);
            set_right(0);
            set_bottom(0);
            set_height(64.0f);
            set_display(Display::Flex);
            set_justify_content(JustifyContent::Center);
            set_align_items(AlignItems::Center);
            set_gap(45.0f);
            queue_update();
        }
    };

    // The Roster Room's and Pick Your Crew's look: navy cards, a yellow rim on what's chosen.
    // Colours only: apply_button_style would also drop the focused button's focus look (it's restyled as it's pressed).
    void mark_chosen(recompui::Button* b, bool chosen) {
        using namespace recompui;
        b->set_background_color(chosen ? Color{58, 50, 8, 235} : Color{7, 15, 34, 235});
        b->set_border_color(chosen ? Color{246, 212, 0, 255} : Color{143, 239, 245, 70});
    }

    void restyle() {
        for (const Choice& c : pg.choices) mark_chosen(c.button, game.pick[c.side] == c.set);
    }

    void build_ui() {
        using namespace recompui;
        blitz::ui::load();
        pg.context = create_context();
        pg.context.open();
        Element* root = pg.context.create_element<Root>(pg.context.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(0);
        root->set_left(0);
        root->set_right(0);
        root->set_bottom(0);
        root->set_display(Display::Flex);
        root->set_align_items(AlignItems::Center);
        root->set_justify_content(JustifyContent::Center);
        root->set_background_color(Color{4, 10, 26, 150});
        root->set_padding_top(blitz::ui::px(12));
        root->set_padding_bottom(64.0f + blitz::ui::px(8));
        pg.card = pg.context.create_element<Element>(root);
        pg.card->set_position(Position::Relative);
        pg.card->set_width(blitz::ui::px(800));
        pg.card->set_max_width(96.0f, Unit::Percent);
        pg.card->set_max_height(100.0f, Unit::Percent);
        pg.card->set_display(Display::Flex);
        pg.card->set_flex_direction(FlexDirection::Column);
        blitz::ui::PanelOptions o;
        o.corners = true;
        Element* content = blitz::ui::panel(pg.context, pg.card, o);
        content->set_display(Display::Flex);
        content->set_flex_direction(FlexDirection::Column);
        content->set_flex(0.0f, 1.0f);
        content->set_min_height(0.0f);
        content->set_gap(blitz::ui::px(10));
        content->set_padding(blitz::ui::px(18));
        content->set_as_navigation_container(NavigationType::Vertical);
        pg.eyebrow = pg.context.create_element<Label>(content, "", theme::Typography::LabelXS);
        pg.eyebrow->set_color(Color{143, 239, 245, 255});
        pg.title = pg.context.create_element<Label>(content, "", theme::Typography::Header2);
        pg.title->set_color(theme::color::SecondaryL);
        Element* rule = pg.context.create_element<Element>(content, 0, "div", false);
        rule->set_height(blitz::ui::px(4));
        rule->set_flex_shrink(0);
        rule->set_position(Position::Relative);
        blitz::ui::stretch(pg.context, rule, "rule_flame");
        Element* scroll = pg.context.create_element<Element>(content);
        scroll->set_flex(0.0f, 1.0f);
        scroll->set_min_height(0.0f);
        scroll->set_overflow_x(Overflow::Hidden);
        scroll->set_overflow_y(Overflow::Auto);
        pg.body = pg.context.create_element<Element>(scroll);
        pg.body->set_display(Display::Flex);
        pg.body->set_flex_direction(FlexDirection::Column);
        pg.body->set_gap(blitz::ui::px(10));
        pg.body->set_as_navigation_container(NavigationType::GridCol);
        pg.bar = pg.context.create_element<Element>(content);
        pg.bar->set_display(Display::Flex);
        pg.bar->set_flex_direction(FlexDirection::Row);
        pg.bar->set_flex_wrap(FlexWrap::Wrap);
        pg.bar->set_justify_content(JustifyContent::FlexEnd);
        pg.bar->set_gap(blitz::ui::px(8));
        pg.bar->set_margin_top(blitz::ui::px(6));
        pg.bar->set_flex_shrink(0);
        pg.bar->set_as_navigation_container(NavigationType::Horizontal);
        pg.context.create_element<Hints>(root);
        pg.context.close();
    }

    recompui::Button* bar_button(const std::string& text, std::function<void()> fn, bool primary = false) {
        using namespace recompui;
        Button* b = pg.context.create_element<Button>(pg.bar, text, primary ? ButtonStyle::Primary : ButtonStyle::Tertiary, ButtonSize::Card);
        b->hide_focus_ring();
        b->set_min_width(blitz::ui::px(110));
        b->add_pressed_callback(std::move(fn));
        return b;
    }

    // The page, with the context open.
    void build_page() {
        using namespace recompui;
        pg.choices.clear();
        pg.body->clear_children();
        pg.bar->clear_children();
        bool adv = blitz_adv != 0;
        int regular = adv ? 18 : 17;
        std::string week = game.week < regular ? "WEEK " + std::to_string(game.week + 1) : "PLAYOFFS";
        pg.eyebrow->set_text("SEASON · " + week + " · BEFORE THE GAME");
        std::string mine = upper(blitz::team_display_name(game.f[0])), cpu = upper(blitz::team_display_name(game.f[1]));
        pg.title->set_text(mine + " VS " + cpu);  // as the VS screen has them: the player on the left
        if (uniforms_on()) {
            for (int s = 0; s < 2; s++) {
                Label* l = pg.context.create_element<Label>(pg.body, std::string(s == 0 ? "YOUR TEAM" : "CPU") + " · " +
                    (s == 0 ? mine : cpu) + " · UNIFORM", theme::Typography::LabelSM);
                l->set_color(Color{143, 239, 245, 255});
                l->set_letter_spacing(blitz::ui::px(1.0f));
                Element* row = pg.context.create_element<Element>(pg.body);
                row->set_display(Display::Flex);
                row->set_flex_direction(FlexDirection::Row);
                row->set_flex_wrap(FlexWrap::Wrap);
                row->set_gap(blitz::ui::px(8));
                row->set_as_navigation_container(NavigationType::GridRow);
                for (const auto& [set, name] : blitz::uniform_choices(game.slot[s])) {
                    std::string text = set == -1 ? "AUTO" : upper(name);
                    Button* b = pg.context.create_element<Button>(row, text, ButtonStyle::Secondary, ButtonSize::Card);
                    b->hide_focus_ring();
                    b->set_min_width(blitz::ui::px(96));
                    int side = s, which = set;
                    b->add_pressed_callback([side, which] {
                        {
                            std::lock_guard lock{game_mutex};
                            game.pick[side] = which;
                        }
                        // Built again on the next tick, focus back on this card: restyling the focused card in place
                        // hides its focus look.
                        pg.refocus = {side, which};
                        pg.rebuild = true;
                    });
                    pg.choices.push_back({s, set, b});
                }
            }
            Label* note = pg.context.create_element<Label>(pg.body, "AUTO: the game's choice (Settings → Rules → Unpicked Uniforms), "
                                                                     "so the two teams don't clash.", theme::Typography::Body);
            note->set_color(Color{184, 196, 218, 255});
        }
        else {
            Label* none = pg.context.create_element<Label>(pg.body, "The uniform picker is off (Settings → Rules → Uniform Picker).",
                                                           theme::Typography::Body);
            none->set_color(Color{184, 196, 218, 255});
        }
        restyle();
        bar_button("Back", [] { back(); });
        if (crew_on()) {
            bar_button("Pick Your Crew", [] {
                std::string mine, cpu;
                {
                    std::lock_guard lock{game_mutex};
                    mine = game.f[0];
                    cpu = game.f[1];
                }
                if (!pg.shown || !blitz::crew_pregame_start(mine, cpu)) return;
                recompui::hide_context(pg.context);
                pg.shown = false;
                pg.crew_up = true;
            });
        }
        Button* go = bar_button("Play", [] { play(); }, true);
        Button* focus = go;
        for (const Choice& c : pg.choices) {
            if (pg.refocus == std::make_pair(c.side, c.set)) focus = c.button;
        }
        pg.refocus = {-9, -9};
        pg.context.set_autofocus_element(focus);
        focus->focus();
    }

    // Each of these once: a held key repeats its action, and a context can't be hidden twice.
    void close_screen() {
        if (pg.shown) recompui::hide_context(pg.context);
        pg.shown = false;
        pg.crew_up = false;
        for (auto& l : latch) l = 0xFFFF;  // whatever's held as it closes stays the screen's
    }

    void back() {
        if (state != Open || !pg.shown) return;
        close_screen();
        state = Idle;
        fprintf(stderr, "[pregame] back to PLAY WEEK\n");
    }

    void play() {
        if (state != Open) return;
        int port;
        {
            std::lock_guard lock{game_mutex};
            port = game.port;
        }
        close_screen();
        state_ms = now_ms();
        state = Go;
        blitz::inject_press(port, n64_a, 200);  // the dialog's PLAY again, for real this time
    }
}

namespace blitz {
    // Main thread, with the other screens' updates (main.cpp).
    void season_pregame_update() {
        int st = state.load();
        if (st == Go && state_age_ms() > 5000) {
            state = Idle;  // the press didn't reach the dialog: the next PLAY asks again
            fprintf(stderr, "[pregame] PLAY never came: idle again\n");
        }
        if (st == Requested) {
            if (!ultramodern::is_game_started()) return;
            if (pg.context == recompui::ContextId::null()) build_ui();
            pg.context.open();
            build_page();
            pg.context.close();
            recompui::show_context(pg.context, "");
            pg.shown = true;
            state_ms = now_ms();
            state = Open;
            return;
        }
        if (st != Open) return;
        state_ms = now_ms();  // up and in use: the hook's 10 s are from the last tick
        if (pg.rebuild && pg.shown) {
            pg.rebuild = false;
            pg.context.open();
            build_page();
            pg.context.close();
            return;
        }
        if (pg.crew_up) {
            int c = crew_pregame_state();
            if (c == 1) return;
            pg.crew_up = false;
            if (c == 2) {  // Go Play on the crew panel: play
                play();
                return;
            }
            recompui::show_context(pg.context, "");
            pg.shown = true;
            return;
        }
        // Start plays, without hunting for the button: any player's (as on Pick Your Crew's panel).
        static bool start_was_down = false;
        bool start_down = false;
        for (int port = 0; port < 4 && !start_down; port++) {
            SDL_GameController* pad = pad_on_port(port);
            start_down = pad != nullptr && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START) != 0;
        }
        if (start_down && !start_was_down && pg.shown && !recompui::is_keyboard_open() && !recompui::is_prompt_open()) {
            start_was_down = true;
            play();
            return;
        }
        start_was_down = start_down;
    }
}
#else
#include <cstdint>
namespace blitz {
    void pregame_input_filter(int, uint16_t*) {}
}
#endif
