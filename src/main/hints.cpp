// Hints: a short tip the first time a player meets each part of the game (docs/design/FIRST_RUN.md). A card at the
// bottom left in the Blitz '27 look (toasts are bottom right): a cyan TIP eyebrow, a yellow title, a line or two in
// which the player's own buttons are drawn as glyphs (player 1's bindings and controller style), and a quiet footer
// saying where to turn hints off. Its own UI context that takes no input, so the game keeps every press.
//
// Each hint shows once, the first time its moment comes (one card at a time; none in the attract demo or while a
// prompt is up), and is remembered in Settings → General (hidden "hints_seen"). It fades when its moment ends or
// after a while, or at any button press. Settings → General → Hints turns them off; Show Hints Again forgets the ones seen.
//
// Moments come from what the app already tracks: the launcher, the game's menus (0x802EA420 phase 1), team select
// (the uniform picker's per-frame hook), play select / a play / offense or defense (savestate.cpp's play mode),
// and Settings being open. Main thread only (update_gfx in main.cpp); the team-select mark comes from the game thread.
#include "game_moment.h"
#include "../game/season_pak.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "recompinput/profiles.h"
#include "recompinput/recompinput.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_modal.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "blitz_config.h"
#include "ui_kit.h"

namespace recompui {
    std::pair<recompinput::InputDevice, int> hint_input_info();  // ui_modal.cpp: player 1's device and profile
}

namespace blitz {
    uint8_t* monitor_rdram();  // dev_monitor.cpp
    bool touch_controls_showing();  // touch_controls.cpp: a phone's on-screen buttons are up
}

namespace {
    using Clock = std::chrono::steady_clock;
    constexpr auto fade = std::chrono::milliseconds(250);
    constexpr auto settle = std::chrono::milliseconds(1200);  // a moment has to last this long before its hint shows
    constexpr auto dismissable = std::chrono::milliseconds(700);  // the press that brought the screen up doesn't count
    constexpr auto seen_after = std::chrono::milliseconds(2500);  // on screen this long = seen (a flash doesn't count)

    std::atomic<int64_t> team_select_at{0};  // ms since start of the last team-select frame (game thread)

    int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    }

    // What the game is showing, as far as the hints care.
    enum class Moment { None, LauncherRom, LauncherReady, Intro, Menus, TeamSelect, PlaySelect, Offense, Defense, Settings };

    enum class Corner { BottomLeft, BottomRight, TopRight };

    struct Hint {
        const char* id;
        Moment moment;
        Corner corner;        // where the card sits: no one spot is free on every screen
        const char* title;
        const char* body;     // {A} {B} {Z} {START} {CLEFT} {NR} {MENU} {BACK} {TABL} {TABR} {R3} {L1} {R1} {DUP} {MOVE}
        int max_seconds;      // 0: as long as the moment lasts
        int nth = 1;          // the moment's nth occurrence (Redo Play: the third play select)
        Moment also = Moment::None;  // a second moment it may show in
    };

    // In the order they're offered: the first unseen one whose moment is now is shown.
    const std::vector<Hint>& hints() {
        static const std::vector<Hint> list = {
            {"settings", Moment::Intro, Corner::BottomRight, "Settings, any time",
             "Press {MENU} for Settings: rules, players, controllers and more. {BACK} backs out. Hints like this one can be turned off there (General).", 12, 1, Moment::Menus},
            {"side", Moment::Menus, Corner::BottomRight, "Choosing your side",
             "Arcade, then Play. At Controller Select, push {MOVE} left or right to pick a team's side, then {A}.", 14},
            {"team", Moment::TeamSelect, Corner::TopRight, "Pick your team",
             "Push {MOVE} up or down to choose, {A} to pick. {CLEFT} changes the uniform. {NR} makes a Pro Bowl crew of the league's best.", 16},
            {"play", Moment::PlaySelect, Corner::TopRight, "Pick a play",
             "Move to a play and press {A}. {B} flips it to the other side; {Z} turns the page.", 14},
            {"offense", Moment::Offense, Corner::BottomRight, "On offense",
             "Hold {Z} for turbo. {A} passes. {B} jumps, or spins with turbo. Turbo + {A} stiff-arms.", 10},
            {"defense", Moment::Defense, Corner::BottomRight, "On defense",
             "{A} switches to the player nearest the ball. {B} tackles, or dives with turbo. Hold {Z} for turbo.", 10},
            {"redo", Moment::PlaySelect, Corner::TopRight, "Do-overs",
             "Hold {R3}: + {R1} saves the game right here, + {L1} loads it back, + {DUP} replays the last play. L3 works as R3 does.", 14, 3},
            {"settings_tour", Moment::Settings, Corner::BottomRight, "Finding your way",
             "{TABL} {TABR} change tabs. Rules turns on the Blitz codes. Players shows who's who and lets phones join.", 12},
        };
        return list;
    }

    // Played by touch alone (ui_kit's touch_only: a phone with no controller): the same hints in the on-screen buttons'
    // words (touch_controls.cpp, labelled from game_moment.cpp's table), as there are no keys or pads to draw.
    const char* touch_body(const std::string& id, bool season) {
        if (id == "settings")
            return "Tap the menu button at the top left for Settings: rules, players, controllers and more. Its X closes it. "
                   "Hints like this one can be turned off there (General).";
        if (id == "side")
            return "Arcade, then Play. At Controller Select, drag the stick (the left of the screen) left or right to pick a "
                   "team's side, then tap OK.";
        if (id == "team")
            return season ? "Drag the stick up or down to choose, tap OK to pick. UNIFORM changes the uniform. PRO BOWL makes a "
                            "Pro Bowl crew of the league's best."
                          : "Drag the stick up or down to choose, tap OK to pick.";
        if (id == "play") return "Drag the stick to a play and tap PICK. FLIP flips it to the other side; PAGE turns the page.";
        if (id == "offense") return "Hold TURBO to run faster. PASS passes. JUMP jumps, or spins with turbo. TURBO + PASS stiff-arms.";
        if (id == "defense") return "SWITCH switches to the player nearest the ball. TACKLE tackles, or dives with turbo. Hold TURBO to run faster.";
        if (id == "redo")
            return "At the top right: S saves the game right here, L loads it back, + picks the next slot. REDO replays the last play.";
        if (id == "settings_tour") return "Tap a tab along the top to change tabs. Rules turns on the Blitz codes. Players shows who's who and lets phones join.";
        return nullptr;
    }

    bool enabled() {
        return std::get<bool>(recompui::config::get_general_config().get_option_value("hints"));
    }

    std::set<std::string> seen() {
        std::set<std::string> out;
        std::stringstream ss{std::get<std::string>(recompui::config::get_general_config().get_option_value("hints_seen"))};
        std::string id;
        while (std::getline(ss, id, ',')) if (!id.empty()) out.insert(id);
        return out;
    }

    void mark_seen(const std::string& id) {
        auto s = seen();
        if (!s.insert(id).second) return;
        std::string all;
        for (const auto& x : s) all += (all.empty() ? "" : ",") + x;
        recomp::config::Config& g = recompui::config::get_general_config();
        g.update_option_value("hints_seen", all);
        g.save_config();
    }

    bool rom_ready() {
        std::u8string id = blitz::game_id;
        return recomp::is_rom_valid(id);
    }

    bool settings_open() {
        try {
            recompui::TabbedModal* m = recompui::config::get_config_modal();
            return m != nullptr && m->is_open_now();
        }
        catch (...) {
            return false;
        }
    }

    Moment current_moment() {
        if (!ultramodern::is_game_started()) {
            return rom_ready() ? Moment::LauncherReady : Moment::LauncherRom;
        }
        if (settings_open()) return Moment::Settings;
        uint8_t* rdram = blitz::monitor_rdram();
        if (rdram == nullptr) return Moment::None;
        int32_t phase = *reinterpret_cast<int32_t*>(rdram + 0x2EA420);  // 0 intro and title, 1 menus, 7 attract demo
        if (phase == 7) return Moment::None;
        if (now_ms() - team_select_at.load() < 400) return Moment::TeamSelect;
        switch (blitz::moment_for(0)) {  // game_moment.cpp, as the touch controls and phones see it
            case blitz::GameMoment::PlaySelect: return Moment::PlaySelect;
            case blitz::GameMoment::Offense: return Moment::Offense;
            case blitz::GameMoment::Defense: return Moment::Defense;
            case blitz::GameMoment::Play: return Moment::None;
            case blitz::GameMoment::Menus: break;
        }
        return phase == 1 ? Moment::Menus : phase == 0 ? Moment::Intro : Moment::None;
    }

    // ---- glyphs ----

    struct Piece { std::string text; bool glyph; bool small = false; bool wide = false; };  // wide: draws past its advance (the D-pad)  // small: PromptFont draws it small (shoulders, triggers)

    bool small_glyph(const recompinput::InputField& f) {
        if (f.input_type == recompinput::InputType::ControllerDigital) {
            return f.input_id == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || f.input_id == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
        }
        if (f.input_type == recompinput::InputType::ControllerAnalog) {
            int axis = f.input_id - 1;
            return axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        }
        return false;
    }

    Piece glyph(const recompinput::InputField& f) {
        bool dpad = f.input_type == recompinput::InputType::ControllerDigital && f.input_id >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
                    f.input_id <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        return Piece{f.to_string(), true, small_glyph(f), dpad};
    }

    // Player 1's binding for a game input (empty if none): a button before a stick direction (C-left is both △ and
    // the right stick's left by default).
    recompinput::InputField binding_field(recompinput::GameInput input, recompinput::InputDevice device, int profile) {
        if (profile < 0) return {};
        recompinput::InputField fallback{};
        for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
            recompinput::InputField b = recompinput::profiles::get_input_binding(profile, input, i);
            if (b.is_empty()) continue;
            if (b.input_type == recompinput::InputType::Keyboard && recompui::menu_action_mapping::is_sdl_input_fake_mapped(b.input_id)) continue;
            bool kb = b.input_type == recompinput::InputType::Keyboard;
            if (kb != (device == recompinput::InputDevice::Keyboard)) continue;
            if (b.input_type == recompinput::InputType::ControllerAnalog) {
                if (fallback.is_empty()) fallback = b;
                continue;
            }
            return b;
        }
        return fallback;
    }

    std::vector<Piece> resolve(const std::string& token) {
        using GI = recompinput::GameInput;
        auto [device, profile] = recompui::hint_input_info();
        if (device == recompinput::InputDevice::COUNT) device = recompinput::InputDevice::Controller;
        bool kb = device == recompinput::InputDevice::Keyboard;
        auto key = [](SDL_Scancode s) { return glyph(recompinput::InputField::keyboard(s)); };
        auto pad = [](SDL_GameControllerButton b) { return glyph(recompinput::InputField::controller_digital(b)); };
        auto bound = [&](GI in, const char* fallback) {
            recompinput::InputField f = binding_field(in, device, profile);
            return f.is_empty() ? Piece{fallback, false} : glyph(f);
        };
        if (token == "A") return {bound(GI::A, "Pass")};
        if (token == "B") return {bound(GI::B, "Jump")};
        if (token == "Z") return {bound(GI::Z, "Turbo")};
        if (token == "START") return {bound(GI::START, "Start")};
        if (token == "CLEFT") return {bound(GI::C_LEFT, "C-Left")};
        if (token == "NR") return {bound(GI::R, "R")};
        if (token == "MENU") return kb ? std::vector<Piece>{key(SDL_SCANCODE_ESCAPE)} : std::vector<Piece>{bound(GI::TOGGLE_MENU, "Menu")};
        if (token == "BACK") return {kb ? key(SDL_SCANCODE_BACKSPACE) : bound(GI::BACK_MENU, "Back")};
        if (token == "TABL") return {kb ? key(SDL_SCANCODE_PAGEUP) : pad(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)};
        if (token == "TABR") return {kb ? key(SDL_SCANCODE_PAGEDOWN) : pad(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)};
        if (token == "R3") return {kb ? Piece{"F5 / F7 / F8, or", false} : pad(SDL_CONTROLLER_BUTTON_RIGHTSTICK)};
        if (token == "L1") return {kb ? key(SDL_SCANCODE_F7) : pad(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)};  // loads
        if (token == "R1") return {kb ? key(SDL_SCANCODE_F5) : pad(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)};  // saves
        if (token == "DUP") return {kb ? key(SDL_SCANCODE_F8) : pad(SDL_CONTROLLER_BUTTON_DPAD_UP)};
        if (token == "MOVE") return {Piece{kb ? "W A S D" : "the stick", false}};
        return {Piece{token, false}};
    }

    // "Press {MENU} for Settings" -> words and glyphs, each its own piece so the line wraps between them.
    std::vector<Piece> pieces(const std::string& body) {
        std::vector<Piece> out;
        std::string word;
        auto flush = [&] { if (!word.empty()) out.push_back({word, false}); word.clear(); };
        for (size_t i = 0; i < body.size(); i++) {
            char c = body[i];
            if (c == '{') {
                size_t end = body.find('}', i);
                if (end != std::string::npos) {
                    flush();
                    for (Piece& p : resolve(body.substr(i + 1, end - i - 1))) {
                        if (p.glyph) out.push_back(p);
                        else {  // fallback words wrap like any others
                            std::stringstream ws{p.text};
                            std::string w;
                            while (ws >> w) out.push_back({w, false});
                        }
                    }
                    i = end;
                    // Punctuation straight after a glyph stays with it ("{A}." doesn't start a line with ".").
                    while (i + 1 < body.size() && std::string(".,;:)").find(body[i + 1]) != std::string::npos) {
                        out.push_back({std::string(1, body[++i]), false});
                    }
                    continue;
                }
            }
            if (c == ' ') flush();
            else word += c;
        }
        flush();
        return out;
    }

    // ---- the card ----

    recompui::ContextId context = recompui::ContextId::null();
    recompui::Element* card = nullptr;
    recompui::Element* content_box = nullptr;  // the panel itself, for taps (hints_tap)
    recompui::Label* title = nullptr;
    recompui::Element* body = nullptr;
    recompui::Label* footer = nullptr;
    float opacity = -1.0f, shift = -1.0f;

    void build() {
        using namespace recompui;
        context = create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        Element* root = context.create_element<Element>(context.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(blitz::ui::px(0));
        root->set_left(blitz::ui::px(0));
        root->set_right(blitz::ui::px(0));
        root->set_bottom(blitz::ui::px(0));
        root->set_background_color(theme::color::Transparent);
        root->set_pointer_events(PointerEvents::None);
        card = context.create_element<Element>(root);
        card->set_position(Position::Absolute);
        card->set_max_width(blitz::ui::px(400));
        card->set_pointer_events(PointerEvents::None);
        blitz::ui::PanelOptions o;
        o.absolute = true;
        o.corners_tl_br = true;
        o.corner_size = 14;
        Element* content = blitz::ui::panel(context, card, o);
        content_box = content;
        content->set_display(Display::Flex);
        content->set_flex_direction(FlexDirection::Column);
        content->set_gap(blitz::ui::px(6));
        content->set_padding_top(blitz::ui::px(14));
        content->set_padding_left(blitz::ui::px(20));
        content->set_padding_right(blitz::ui::px(20));
        content->set_padding_bottom(blitz::ui::px(12));
        Label* eyebrow = context.create_element<Label>(content, "TIP", theme::Typography::LabelXS);
        eyebrow->set_color(Color{143, 239, 245, 255});
        title = context.create_element<Label>(content, "", theme::Typography::Header3);
        title->set_color(theme::color::SecondaryL);
        Element* rule = context.create_element<Element>(content, 0, "div", false);
        rule->set_height(blitz::ui::px(4));
        rule->set_position(Position::Relative);
        blitz::ui::stretch(context, rule, "rule_flame");
        body = context.create_element<Element>(content, 0, "div", false);
        body->set_display(Display::Flex);
        body->set_flex_direction(FlexDirection::Row);
        body->set_flex_wrap(FlexWrap::Wrap);
        body->set_align_items(AlignItems::Center);
        body->set_column_gap(blitz::ui::px(6));
        body->set_row_gap(blitz::ui::px(2));
        body->set_margin_top(blitz::ui::px(4));
        footer = context.create_element<Label>(content, "HINTS: SETTINGS → GENERAL", theme::Typography::LabelXS);
        footer->set_color(Color{184, 196, 218, 170});
        footer->set_margin_top(blitz::ui::px(6));
        context.close();
    }

    void fill(const Hint& h) {
        using namespace recompui;
        context.open();
        // Bottom: above the game's turbo bars and menu prompts along the bottom edge.
        bool right = h.corner != Corner::BottomLeft, top = h.corner == Corner::TopRight;
        // On a phone the touch buttons own the bottom right (OK, A, B, Turbo) and the top right (save states, REDO):
        // the card goes to the top left, beside the menu button, so it never sits on a button (2026-11-13: it covered
        // OK at the game-mode picker).
        bool touch = blitz::touch_controls_showing();
        if (touch) {
            right = false;
            top = true;
        }
        if (right) {
            card->unset_left();
            card->set_right(blitz::ui::px(24));
        }
        else {
            card->unset_right();
            card->set_left(blitz::ui::px(touch ? 150 : 24));
        }
        if (top) {
            card->unset_bottom();
            card->set_top(blitz::ui::px(24));
        }
        else {
            card->unset_top();
            card->set_bottom(blitz::ui::px(86));
        }
        std::string t = h.title;
        for (char& c : t) c = (char)std::toupper((unsigned char)c);
        title->set_text(t);
        body->clear_children();
        bool after_glyph = false;
        std::string text = h.body;
        bool season = true;
#if BLITZ_MODERN
        // Team select's extras (uniform sets, Pro Bowl) come with the 2026 Season (season_pak.h): without it, only how
        // to pick (2026-11-13).
        season = blitz::season_content();
        if (std::string(h.id) == "team" && !season) text = "Push {MOVE} up or down to choose, {A} to pick.";
#endif
        if (blitz::ui::touch_only()) {
            if (const char* t = touch_body(h.id, season)) text = t;
        }
        for (const Piece& p : pieces(text)) {
            Label* l = context.create_element<Label>(body, p.text, theme::Typography::Body);
            if (after_glyph && !p.glyph && p.text.size() == 1 && std::string(".,;:)").find(p.text[0]) != std::string::npos) {
                l->set_margin_left(blitz::ui::px(-5));  // "✕." not "✕ ."
            }
            after_glyph = p.glyph;
            if (p.glyph) {
                l->set_font_family("promptfont");
                l->set_font_style(FontStyle::Normal);
                l->set_font_weight(400);
                l->set_font_size(blitz::ui::px(30));  // one size for every button: a trigger drawn bigger crowded the line (team)
                l->set_margin_left(blitz::ui::px(2));
                l->set_margin_right(blitz::ui::px(p.wide ? 10 : 2));
            }
            else {
                l->set_font_size(blitz::ui::px(19));
                l->set_color(Color{242, 242, 242, 255});
            }
        }
        context.close();
    }

    void set_look(float o, float dx) {
        if (o == opacity && dx == shift) return;
        opacity = o;
        shift = dx;
        context.open();
        card->set_opacity(o);
        card->set_translate_2D(dx, 0.0f);
        context.close();
    }

    // Showing state.
    const Hint* showing = nullptr;
    bool up = false, leaving = false, preview = false;
    Clock::time_point shown_at{}, leave_at{};
    Moment last_moment = Moment::None;
    Clock::time_point moment_since{};
    int occurrences[16] = {};  // per Moment: how many times it has started this session
    bool scrimmage = false;    // a play select came before this play: not a kickoff (the sides aren't set then)

    void show(const Hint& h, bool is_preview = false) {
        if (context == recompui::ContextId::null()) build();
        fill(h);
        showing = &h;
        preview = is_preview;
        leaving = false;
        shown_at = Clock::now();
        set_look(0.0f, h.corner == Corner::BottomLeft ? -12.0f : 12.0f);
        if (up) recompui::hide_context(context);  // shown again below: in front of anything opened since
        recompui::show_context(context, "");
        up = true;
    }

    void begin_leave() {
        if (!up || leaving) return;
        leaving = true;
        leave_at = Clock::now();
    }
}

namespace blitz {
    // A tap at (x, y), in the UI's pixels (touch_controls.cpp, phones): on the card, the hint goes away now (it stays
    // seen). True if it was on it.
    bool hints_tap(float x, float y) {
        if (!up || leaving || content_box == nullptr) return false;
        float margin = 16.0f * content_box->get_dp_to_pixel_ratio();
        float l = content_box->get_absolute_left() - margin, t = content_box->get_absolute_top() - margin;
        float r = content_box->get_absolute_left() + content_box->get_client_width() + margin;
        float b = content_box->get_absolute_top() + content_box->get_client_height() + margin;
        if (x < l || x > r || y < t || y > b) return false;
        if (showing != nullptr && !preview) mark_seen(showing->id);  // read and put away: it doesn't come back
        begin_leave();
        return true;
    }

    // Team select's selection loop, each frame (uniform_picker.cpp, game thread).
    void hints_mark_team_select() {
        team_select_at = now_ms();
    }

    // Dev: "HINT <id>" shows that hint now, whatever the moment (and doesn't mark it seen).
    void hints_preview(const std::string& id) {
        for (const Hint& h : hints()) {
            if (id == h.id) {
                show(h, true);
                return;
            }
        }
        fprintf(stderr, "[hints] no hint '%s'\n", id.c_str());
    }

    // The launcher's line for a new player (launcher.cpp): until a game has been started once, with hints on.
    bool hints_launcher_tip() {
        return enabled() && !seen().count("launcher");
    }

    // Settings → General → Show Hints Again.
    void hints_reset() {
        recomp::config::Config& g = recompui::config::get_general_config();
        g.update_option_value("hints_seen", std::string{});
        g.save_config();
        for (int& n : occurrences) n = 0;
    }

    // Every UI tick (update_gfx).
    void hints_update() {
        auto now = Clock::now();
        auto ms = [](auto d) { return std::chrono::duration<float, std::milli>(d).count(); };
        static bool started = false;
        if (!started && ultramodern::is_game_started()) {  // the launcher's line has done its job
            started = true;
            if (enabled()) mark_seen("launcher");
        }
        Moment m = current_moment();
        if (m != last_moment) {
            if (m == Moment::PlaySelect) scrimmage = true;
            else if (m == Moment::Menus || m == Moment::TeamSelect || (last_moment != Moment::PlaySelect &&
                     (m == Moment::Offense || m == Moment::Defense) && last_moment != Moment::None)) scrimmage = false;
            last_moment = m;
            moment_since = now;
            if (m != Moment::None) occurrences[(int)m]++;
        }
        if ((m == Moment::Offense || m == Moment::Defense) && !scrimmage) m = Moment::None;
        bool blocked = recompui::is_prompt_open() || (settings_open() && m != Moment::Settings);

        if (up && showing != nullptr) {
            if (!leaving && now - shown_at > dismissable) {  // any button sends it away; the game still gets the press
                uint16_t buttons = 0;
                float x = 0, y = 0;
                if (recompinput::profiles::get_n64_input(0, &buttons, &x, &y) && buttons != 0) {
                    if (!preview) mark_seen(showing->id);
                    begin_leave();
                }
            }
            if (!preview) {
                bool ended = m != showing->moment && m != showing->also;
                bool timed = showing->max_seconds > 0 && now - shown_at > std::chrono::seconds(showing->max_seconds);
                if (ended || timed || blocked || !enabled()) begin_leave();
                if (now - shown_at > seen_after) mark_seen(showing->id);
            }
            else if (now - shown_at > std::chrono::seconds(showing->max_seconds > 0 ? showing->max_seconds : 12)) {
                begin_leave();
            }
            if (leaving) {
                float k = ms(now - leave_at) / ms(fade);
                if (k >= 1.0f) {
                    recompui::hide_context(context);
                    up = false;
                    showing = nullptr;
                }
                else set_look(1.0f - k, 0.0f);
            }
            else {
                float k = std::min(1.0f, ms(now - shown_at) / ms(fade));
                set_look(k, (showing->corner == Corner::BottomLeft ? -12.0f : 12.0f) * (1.0f - k));
            }
            return;
        }
        if (blocked || !enabled() || m == Moment::None || now - moment_since < settle) return;
        auto done = seen();
        for (const Hint& h : hints()) {
            if ((h.moment != m && h.also != m) || done.count(h.id)) continue;
            if (occurrences[(int)m] < h.nth) continue;
            show(h);
            fprintf(stderr, "[hints] %s\n", h.id);
            return;
        }
    }
}
