// On-screen touch controls for phones (include/blitz_platform.h: BLITZ_HAS_TOUCH_CONTROLS; iOS and Android share
// this file). Player 1's N64 controller, drawn over the game: a floating stick on the left half, A, B and Turbo on the
// right, a small Start (⏵) at the bottom and a menu button (Settings, ☰) at the top left.
// Only what means something now is shown, labelled for now (game_moment.h, the same table as the phone controllers'
// page): OK and BACK in the menus, PICK, FLIP and PAGE at play select, PASS or SWITCH in a play. On offense, three
// small receiver buttons wrap around A (← ↓ →, the left, middle and right receiver, as the icons
// over their heads show; Receiver Buttons). Save states: small L, + (next slot) and S circles at the top right, always,
// with REDO beside them (UNDO in the menus: back a step); at team select UNIFORM and PRO BOWL stack below them.
//
// Input: SDL's finger events (the same on both phones), turned into an N64 button mask and stick that
// controllers.cpp's port_input merges into player 1, like the keyboard. The whole state is recomputed from the fingers
// down on every event, so a lost or cancelled touch can't leave a button held; a tap that starts and ends between two
// of the game's reads is held until the next read, so quick taps always count.
// Shown while touch is what's being played with: hidden once a controller is used (back at the next touch), while a
// menu is open (touches then drive the menu, as a mouse) and before the game starts.
// Drawing: an RmlUi context like the toasts' (toast.cpp), taking no input; 1 dp = screen height / 1080.
#include "blitz_platform.h"
#if BLITZ_HAS_TOUCH_CONTROLS
extern "C" int blitz_ds_active(void);
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <cstdio>
#include <string>

#include "SDL2/SDL.h"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "ultramodern/ultramodern.hpp"
#include "game_moment.h"

extern "C" void blitz_savestate_request(int op);  // savestate.cpp: 1 save, 2 load (the current slot)
extern "C" void blitz_redo_request();             // Redo Play
namespace blitz {
    bool toast_tap(float x, float y);  // toast.cpp: a tap on a toast puts it away
    bool hints_tap(float x, float y);  // hints.cpp: a tap on a hint card puts it away
    bool uniform_picker_listening();   // uniform_picker.cpp: team select, picker on
    bool uniform_picker_season();      // uniform_picker.cpp: that team select is Season's (uniforms, no Pro Bowl)
    bool receiver_buttons_on(int port);  // receiver_targets.cpp: the setting, or BLITZ DS's Stylus for player 1
    bool ds_touch_routes_up();           // ds_touch.cpp: play select's bar of buttons on the lower screen
}
extern "C" void blitz_savestate_slot_step(int delta);  // savestate.cpp: the next slot

namespace {
    // N64 buttons (controllers.cpp's n64_button_values)
    constexpr uint16_t A = 0x8000, B = 0x4000, Z = 0x2000, START = 0x1000, R = 0x0010;
    constexpr uint16_t C_LEFT = 0x0002, C_DOWN = 0x0004, C_RIGHT = 0x0001;

    // A button that isn't the N64's: it acts when the finger lifts on it.
    enum class Action { None, Menu, Save, Load, Slot, Redo };
    enum class Anchor { Left, Right, Middle };
    // When a control is on screen.
    enum class When { Always, NotMenus, Offense, TeamSelect, SaveStates, Redo, PageOnly };
    // Its label from the moment's table (game_moment.h), or its own.
    enum class Text { Own, Pass, Jump, Turbo };

    // One control, in units of the screen's height from its anchor: the left edge, the right edge (x negative), or
    // the middle. radius is the drawn size; touches count within radius * hit.
    struct Button {
        const char* label;
        uint16_t mask;
        Anchor anchor;
        float x, y, radius;
        bool pill = false;  // a wide pill instead of a circle
        Action action = Action::None;
        When when = When::Always;
        Text text = Text::Own;
    };
    constexpr float hit = 1.35f;
    // The receiver buttons sit around A at 6:00, 4:30 and 3:00, close enough to roll a thumb onto.
    constexpr float a_x = -0.27f, a_y = 0.68f, ring = 0.155f, ring45 = ring * 0.7071f;
    constexpr std::array<Button, 15> buttons{{
        {"A", A, Anchor::Right, a_x, a_y, 0.085f, false, Action::None, When::Always, Text::Pass},
        {"B", B, Anchor::Right, -0.45f, 0.84f, 0.070f, false, Action::None, When::Always, Text::Jump},
        {"TURBO", Z, Anchor::Right, -0.42f, 0.50f, 0.070f, false, Action::None, When::NotMenus, Text::Turbo},
        {"\xE2\x86\x90", C_LEFT, Anchor::Right, a_x, a_y + ring, 0.050f, false, Action::None, When::Offense},           // ← 6:00
        {"\xE2\x86\x93", C_DOWN, Anchor::Right, a_x + ring45, a_y + ring45, 0.050f, false, Action::None, When::Offense}, // ↓ 4:30
        {"\xE2\x86\x92", C_RIGHT, Anchor::Right, a_x + ring, a_y, 0.050f, false, Action::None, When::Offense},          // → 3:00
        {"\xE2\x8F\xB5", START, Anchor::Middle, 0.0f, 0.93f, 0.034f},  // ⏵ (PromptFont)
        {"", 0, Anchor::Left, 0.075f, 0.09f, 0.036f, false, Action::Menu},  // a hamburger, left of the scoreboard
        // Save states and Redo Play (savestate.cpp), for a phone with no controller: what F7, F6, F5 and F8 do on a
        // computer. Small, at the top right, away from the play; L, + and S always (a state can be loaded from the
        // menus, to jump straight back into a game).
        {"L", 0, Anchor::Right, -0.31f, 0.09f, 0.036f, false, Action::Load, When::SaveStates},
        {"+", 0, Anchor::Right, -0.22f, 0.09f, 0.036f, false, Action::Slot, When::SaveStates},
        {"S", 0, Anchor::Right, -0.13f, 0.09f, 0.036f, false, Action::Save, When::SaveStates},
        {"REDO", 0, Anchor::Right, -0.47f, 0.09f, 0.036f, true, Action::Redo, When::Redo},
        // Team select (uniform_picker.cpp), stacked under the save state buttons: △ steps through uniforms (N64
        // C-left), R picks Pro Bowl players.
        {"UNIFORM", C_LEFT, Anchor::Right, -0.22f, 0.19f, 0.036f, true, Action::None, When::TeamSelect},
        {"PRO BOWL", R, Anchor::Right, -0.22f, 0.29f, 0.036f, true, Action::None, When::TeamSelect},
        // Play select with a controller in hand, when nothing else shows: a thumb's way to the next page, as Turbo.
        {"PAGE", Z, Anchor::Left, 0.13f, 0.05f, 0.03f, true, Action::None, When::PageOnly},
    }};
    constexpr float stick_x = 0.24f, stick_y = 0.70f, stick_radius = 0.15f;  // the stick's resting place and reach
    constexpr float stick_zone = 0.45f;  // the left 45% of the screen (below the menu button) is the stick's

    struct Finger {
        float x = 0, y = 0;           // pixels
        int control = -1;             // the button it went down on (-1 none), or stick_control
        float origin_x = 0, origin_y = 0;
    };
    constexpr int stick_control = 100;
    constexpr int swallowed = 101;  // a finger that put a toast or hint away: it presses nothing until it lifts
    SDL_Window* ui_window = nullptr;  // the game's window (touch_controls_update)

    std::mutex mutex;
    std::map<SDL_FingerID, Finger> fingers;
    uint16_t held = 0, latched = 0;   // held now; pressed since the game's last read
    float stick_dx = 0, stick_dy = 0; // -1..1, N64 axes (y up)
    float base_x = -1, base_y = -1;   // where the floating stick sits (pixels), -1: its resting place
    float screen_w = 1, screen_h = 1;
    std::atomic<uint32_t> last_touch{0}, last_pad{0};
    std::atomic<bool> menu_open{true}, visible{false};
    std::atomic<bool> page_only{false};  // only the PAGE pill is up

    // A control's centre in pixels.
    void centre(const Button& b, float& cx, float& cy) {
        cx = b.anchor == Anchor::Left ? b.x * screen_h : b.anchor == Anchor::Right ? screen_w + b.x * screen_h
                                                                                   : screen_w * 0.5f + b.x * screen_h;
        cy = b.y * screen_h;
    }

    std::array<bool, buttons.size()> on_screen{};  // which controls show now (touch_controls_update; mutex held)

    int button_at(float x, float y) {
        int best = -1;
        float best_d = 1e9f;
        for (size_t i = 0; i < buttons.size(); i++) {
            if (!on_screen[i]) continue;
            float cx, cy;
            centre(buttons[i], cx, cy);
            float r = buttons[i].radius * screen_h * hit * (buttons[i].pill ? 2.0f : 1.0f);
            float d = std::hypot((x - cx) / (buttons[i].pill ? 2.0f : 1.0f), y - cy);
            if (d <= r && d < best_d) {
                best = (int)i;
                best_d = d;
            }
        }
        return best;
    }

    // The state from the fingers down now (mutex held).
    void recompute() {
        uint16_t now = 0;
        float dx = 0, dy = 0;
        bool stick = false;
        for (auto& [id, f] : fingers) {
            if (f.control == swallowed) continue;
            if (f.control == stick_control) {
                stick = true;
                float r = stick_radius * screen_h;
                dx = (f.x - f.origin_x) / r;
                dy = (f.y - f.origin_y) / r;
                float len = std::hypot(dx, dy);
                if (len > 1.0f) {  // past the edge, the base follows the thumb
                    f.origin_x = f.x - dx / len * r;
                    f.origin_y = f.y - dy / len * r;
                    dx /= len;
                    dy /= len;
                }
                base_x = f.origin_x;
                base_y = f.origin_y;
            }
            else {
                // A finger may slide from one button to another (B to A for a jump then a pass).
                int b = button_at(f.x, f.y);
                // On BLITZ DS a tap on the header already turns the page (ds_view.cpp): the pill just shows that it can.
                if (b >= 0 && !(page_only && blitz_ds_active())) now |= buttons[(size_t)b].mask;
            }
        }
        if (!stick) {
            base_x = base_y = -1;
            dx = dy = 0;
        }
        latched |= now & ~held;
        if (now != held) fprintf(stderr, "[touch] buttons %04X\n", now);  // logcat / blitz.log: what the screen pressed
        held = now;
        stick_dx = dx;
        stick_dy = -dy;
    }

    void open_settings() {
        SDL_Event e{};
        e.type = SDL_KEYDOWN;
        e.key.state = SDL_PRESSED;
        e.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
        e.key.keysym.sym = SDLK_ESCAPE;
        SDL_PushEvent(&e);
        e.type = SDL_KEYUP;
        e.key.state = SDL_RELEASED;
        SDL_PushEvent(&e);
    }

    // SDL's event watch (the main thread, as events are pumped).
    int on_event(void*, SDL_Event* e) {
        switch (e->type) {
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
            case SDL_FINGERUP: {
                // With only the PAGE pill up (a controller in hand, at play select), a touch is for it: it doesn't bring the
                // rest of the touch controls up (team: that made PAGE useless on a phone).
                if (!page_only) last_touch = SDL_GetTicks();
                if (e->type == SDL_FINGERDOWN) {
                    // A toast or hint under the finger is put away, and the touch goes no further (in a menu too).
                    int pw = 0, ph = 0;
                    if (ui_window) SDL_GetWindowSizeInPixels(ui_window, &pw, &ph);
                    float ux = e->tfinger.x * pw, uy = e->tfinger.y * ph;
                    if (blitz::toast_tap(ux, uy) || blitz::hints_tap(ux, uy)) {
                        std::lock_guard lock{mutex};
                        Finger f;
                        f.control = swallowed;
                        fingers[e->tfinger.fingerId] = f;
                        return 1;
                    }
                }
                if (menu_open || !ultramodern::is_game_started()) {
                    std::lock_guard lock{mutex};
                    fingers.clear();
                    recompute();
                    return 1;
                }
                std::lock_guard lock{mutex};
                float x = e->tfinger.x * screen_w, y = e->tfinger.y * screen_h;
                if (e->type == SDL_FINGERUP) {
                    auto it = fingers.find(e->tfinger.fingerId);
                    if (it != fingers.end() && it->second.control >= 0 && it->second.control < (int)buttons.size() &&
                        button_at(x, y) == it->second.control) {
                        switch (buttons[(size_t)it->second.control].action) {
                            case Action::Menu: open_settings(); break;
                            case Action::Save: blitz_savestate_request(1); break;  // as F5
                            case Action::Load: blitz_savestate_request(2); break;  // as F7
                            case Action::Slot: blitz_savestate_slot_step(1); break;  // as F6
                            case Action::Redo: blitz_redo_request(); break;        // as F8
                            case Action::None: break;
                        }
                    }
                    fingers.erase(e->tfinger.fingerId);
                }
                else if (e->type == SDL_FINGERDOWN) {
                    Finger f;
                    f.x = x;
                    f.y = y;
                    f.control = button_at(x, y);
                    if (f.control < 0 && x < screen_w * stick_zone && !page_only) {
                        f.control = stick_control;
                        f.origin_x = x;
                        f.origin_y = y;
                    }
                    fingers[e->tfinger.fingerId] = f;
                }
                else if (auto it = fingers.find(e->tfinger.fingerId); it != fingers.end()) {
                    it->second.x = x;
                    it->second.y = y;
                }
                recompute();
                return 1;
            }
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_KEYDOWN:
                last_pad = SDL_GetTicks();
                return 1;
            case SDL_CONTROLLERAXISMOTION:
                if (std::abs(e->caxis.value) > 12000) last_pad = SDL_GetTicks();
                return 1;
            case SDL_WINDOWEVENT:
                if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    std::lock_guard lock{mutex};
                    screen_w = (float)std::max(1, e->window.data1);
                    screen_h = (float)std::max(1, e->window.data2);
                }
                return 1;
            default:
                return 1;
        }
    }

    // --- Drawing ----------------------------------------------------------------------------------------------------

    recompui::ContextId context = recompui::ContextId::null();
    recompui::Element* stick_base = nullptr;
    recompui::Element* stick_knob = nullptr;
    std::array<recompui::Element*, buttons.size()> button_el{};
    std::array<recompui::Label*, buttons.size()> button_label{};
    std::array<std::string, buttons.size()> drawn_label{};
    bool shown = false;
    uint16_t drawn_held = 0xFFFF;
    std::array<bool, buttons.size()> drawn_on{};  // on_screen as last drawn
    bool drawn_once = false;

    // Characters, not bytes (a label may be an arrow).
    size_t chars(const std::string& s) {
        return (size_t)std::count_if(s.begin(), s.end(), [](char c) { return ((unsigned char)c & 0xC0) != 0x80; });
    }

    void set_label(size_t i, const std::string& text) {
        float d = buttons[i].radius * 2 * 1080.0f;
        size_t n = chars(text);
        button_label[i]->set_text(text);
        button_label[i]->set_font_size(n > 2 ? std::min(d * 0.30f, d * 1.2f / (float)n) : d * 0.42f);
        drawn_label[i] = text;
    }
    float drawn_bx = -2, drawn_by = -2, drawn_dx = -2, drawn_dy = -2, drawn_w = 0, drawn_h = 0;

    float dp(float pixels) { return pixels * 1080.0f / screen_h; }

    recompui::Element* circle(recompui::Element* parent, float d, recompui::Color fill, recompui::Color edge) {
        using namespace recompui;
        Element* e = context.create_element<Element>(parent);
        e->set_position(Position::Absolute);
        e->set_width(d);
        e->set_height(d);
        e->set_border_radius(d * 0.5f);
        e->set_background_color(fill);
        e->set_border_width(3.0f);
        e->set_border_color(edge);
        e->set_pointer_events(PointerEvents::None);
        return e;
    }

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
        const Color fill{10, 12, 24, 110}, edge{255, 255, 255, 140};  // dark glass: reads over any picture
        stick_base = circle(root, stick_radius * 2 * 1080.0f, fill, edge);
        stick_knob = circle(root, stick_radius * 0.9f * 1080.0f, Color{255, 255, 255, 90}, Color{255, 255, 255, 170});
        for (size_t i = 0; i < buttons.size(); i++) {
            const Button& b = buttons[i];
            float d = b.radius * 2 * 1080.0f;
            Element* e = circle(root, d, fill, edge);
            if (b.pill) {
                e->set_width(d * 2.4f);
                e->set_border_radius(d * 0.5f);
            }
            Label* l = context.create_element<Label>(e, b.label, theme::Typography::LabelSM);
            l->set_position(Position::Absolute);
            l->set_left(0);
            l->set_right(0);
            l->set_top(0);
            l->set_bottom(0);
            l->set_text_align(TextAlign::Center);
            l->set_line_height(d - 6.0f);
            l->set_color(Color{255, 255, 255, 220});
            if (std::string(b.label) == "\xE2\x8F\xB5") l->set_font_family("promptfont");
            if (b.action == Action::Menu) {  // three bars: no font here has ☰
                for (int k = 0; k < 3; k++) {
                    Element* bar = context.create_element<Element>(e);
                    bar->set_position(Position::Absolute);
                    bar->set_left(d * 0.27f);
                    bar->set_width(d * 0.46f - 6.0f);
                    bar->set_top(d * (0.30f + 0.17f * (float)k) - 4.0f);
                    bar->set_height(d * 0.07f);
                    bar->set_border_radius(d * 0.035f);
                    bar->set_background_color(Color{255, 255, 255, 220});
                    bar->set_pointer_events(PointerEvents::None);
                }
            }
            button_el[i] = e;
            button_label[i] = l;
            set_label(i, b.label);
        }
        context.close();
    }

    void place(recompui::Element* e, float cx_px, float cy_px, float w_dp, float h_dp) {
        e->set_left(dp(cx_px) - w_dp * 0.5f);
        e->set_top(dp(cy_px) - h_dp * 0.5f);
    }
}

namespace blitz {
    // At startup (main.cpp), once SDL's video is up.
    void touch_controls_init() {
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");  // touches still drive the menus (as a mouse)
        SDL_AddEventWatch(on_event, nullptr);
    }

    // Whether the touch controls are on screen (receiver_targets.cpp draws tap targets then, not button icons).
    bool touch_controls_showing() { return visible && !page_only; }  // the PAGE pill alone leaves taps to BLITZ DS

    // Game thread (controllers.cpp, player 1's read): the touch state merged into the controller's.
    void touch_controls_read(uint16_t* out_buttons, float* x, float* y) {
        if (!visible) return;
        std::lock_guard lock{mutex};
        *out_buttons |= (held | latched);
        latched = 0;
        if (stick_dx != 0 || stick_dy != 0) {
            *x = stick_dx;
            *y = stick_dy;
        }
    }

    // Every UI tick (main.cpp's update_gfx): show, hide and redraw.
    void touch_controls_update(SDL_Window* window) {
        ui_window = window;
        menu_open = recompui::is_context_capturing_input();
        bool on = std::get<bool>(recompui::config::get_general_config().get_option_value("touch_controls"));
        bool controller = SDL_NumJoysticks() > 0;
        // BLITZ DS with a controller: real buttons, and taps have their own meaning (ds_view.cpp). Without one (a Surface
        // Duo) these are all there is.
        bool want = on && !(blitz_ds_active() && controller) && ultramodern::is_game_started() && !menu_open &&
                    (!controller || last_touch.load() > last_pad.load());  // a controller in hand: none until a touch
        // The PAGE pill alone (a controller in hand at play select) is gone (team, 2026-11-02): a controller has Z for it, and
        // the touch controls have their own Turbo. BLITZ DS's bar has a PAGE of its own (ds_touch.cpp).
        bool page_pill = false;
        page_only = page_pill;
        want = want || page_pill;
        if (window) {
            int w = 0, h = 0;
            SDL_GetWindowSize(window, &w, &h);
            if (w > 0 && h > 0) {
                std::lock_guard lock{mutex};
                screen_w = (float)w;
                screen_h = (float)h;
            }
        }
        visible = want;
        if (!want) {
            if (shown) {
                recompui::hide_context(context);
                shown = false;
                std::lock_guard lock{mutex};
                fingers.clear();
                recompute();
            }
            return;
        }
        if (context == recompui::ContextId::null()) build();
        uint16_t h;
        float bx, by, dx, dy, sw, sh;
        {
            std::lock_guard lock{mutex};
            h = held;
            bx = base_x < 0 ? stick_x * screen_h : base_x;
            by = base_y < 0 ? stick_y * screen_h : base_y;
            dx = stick_dx;
            dy = stick_dy;
            sw = screen_w;
            sh = screen_h;
        }
        // What shows: by the moment (game_moment.h) and the settings; labels by the moment.
        auto general = [](const char* key) { return std::get<bool>(recompui::config::get_general_config().get_option_value(key)); };
        blitz::GameMoment moment = blitz::moment_for(0);
        bool menus = moment == blitz::GameMoment::Menus;
        bool team_select = blitz::uniform_picker_listening();
        bool receivers_on = blitz::receiver_buttons_on(0);
        std::array<bool, buttons.size()> show{};
        for (size_t i = 0; i < buttons.size(); i++) {
            switch (buttons[i].when) {
                case When::Always: show[i] = true; break;
                case When::NotMenus: show[i] = !menus; break;
                case When::Offense: show[i] = moment == blitz::GameMoment::Offense && receivers_on; break;
                case When::TeamSelect: show[i] = team_select && (buttons[i].mask != R || !blitz::uniform_picker_season()); break;  // no Pro Bowl in Season
                case When::SaveStates: show[i] = general("savestate_buttons"); break;
                case When::Redo: show[i] = general("redo_play"); break;  // in the menus: back a step (UNDO)
                case When::PageOnly: show[i] = false; break;
            }
            if (page_pill) show[i] = buttons[i].when == When::PageOnly;
        }
        const blitz::MomentText& words = blitz::moment_text(moment);
        context.open();
        stick_base->set_display(page_pill ? recompui::Display::None : recompui::Display::Block);
        stick_knob->set_display(page_pill ? recompui::Display::None : recompui::Display::Block);
        for (size_t i = 0; i < buttons.size(); i++) {
            if (!drawn_once || show[i] != drawn_on[i]) button_el[i]->set_display(show[i] ? recompui::Display::Block : recompui::Display::None);
            const char* text = buttons[i].text == Text::Pass ? words.pass.label : buttons[i].text == Text::Jump ? words.jump.label
                             : buttons[i].text == Text::Turbo ? words.turbo.label
                             : buttons[i].action == Action::Redo && menus ? "UNDO" : buttons[i].label;
            if (drawn_label[i] != text) set_label(i, text);
        }
        context.close();
        drawn_on = show;
        drawn_once = true;
        {
            std::lock_guard lock{mutex};
            on_screen = show;
        }
        bool layout = sw != drawn_w || sh != drawn_h;
        if (layout || h != drawn_held || bx != drawn_bx || by != drawn_by || dx != drawn_dx || dy != drawn_dy) {
            context.open();
            float base_d = stick_radius * 2 * 1080.0f, knob_d = stick_radius * 0.9f * 1080.0f;
            place(stick_base, bx, by, base_d, base_d);
            place(stick_knob, bx + dx * stick_radius * sh, by - dy * stick_radius * sh, knob_d, knob_d);
            for (size_t i = 0; i < buttons.size(); i++) {
                const Button& b = buttons[i];
                float cx, cy;
                centre(b, cx, cy);
                float d = b.radius * 2 * 1080.0f;
                place(button_el[i], cx, cy, b.pill ? d * 2.4f : d, d);
                bool down = b.action == Action::None && (h & b.mask);
                button_el[i]->set_background_color(down ? recompui::Color{255, 196, 0, 170} : recompui::Color{10, 12, 24, 110});
            }
            context.close();
            drawn_w = sw;
            drawn_h = sh;
            drawn_held = h;
            drawn_bx = bx;
            drawn_by = by;
            drawn_dx = dx;
            drawn_dy = dy;
        }
        if (!shown) {
            recompui::show_context(context, "");
            shown = true;
        }
    }
}
#else
#include <cstdint>
struct SDL_Window;
namespace blitz {
    void touch_controls_init() {}
    bool touch_controls_showing() { return false; }
    void touch_controls_read(uint16_t*, float*, float*) {}
    void touch_controls_update(SDL_Window*) {}
}
#endif
