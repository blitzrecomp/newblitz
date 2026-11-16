// The Export ROM screen (the launcher's Export ROM, Settings → Add-Ons): only what a ROM can carry. Shortcuts first
// (Modern: Full, Lite, Custom; Classic and the Team Extension build: everything, or Custom), then the extras as
// toggles (changing one makes it Custom), what to know on real hardware and in emulators, and Export. The values are
// Settings → Export's (export_rom.cpp), so the two always agree; the shortcut is its export_preset.
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "elements/ui_modal.h"
#include "../main/ui_kit.h"

namespace blitz {
#if BLITZ_ENGINE_ORIGINAL
    bool original_game();  // adv_mode.cpp
#endif
    bool season_content();  // content_dirs.cpp: the 2026 Season is here
    void export_rom_go();  // export_rom.cpp: Export now, or once the game has started
    void roster_room_open();  // roster_room.cpp (Modern; Classic has a stub)
    bool export_applying = false;  // a shortcut is setting the toggles (export_rom.cpp's callbacks wait)
}

namespace {
    using namespace recompui;

    struct Extra {
        const char* id;
        const char* name;
        const char* about;
    };
    // Settings → Export's toggles, in the screen's order.
    const std::vector<Extra> extras = {
#if BLITZ_ENGINE_ORIGINAL
        {"export_teams32", "32 Teams", "Houston joins as the 32nd team, in every mode. Off: the game's own 31 teams."},
        {"export_season", "2026 Season",
         "Season plays the 2026 league: 8 divisions, 18 weeks, 7-team playoffs. With every uniform set it needs an "
         "Expansion Pak on a real N64; with the 2026 home and away only, any N64. Off: the game's own 1999 Season."},
        {"export_uniforms", "Every Uniform",
         "Every uniform set the app has (alternates, throwbacks) for the Uniform Picker. Off: each team's 2026 and 1999 "
         "home and away, which leaves a plain N64 room for the 2026 season."},
#endif
#if BLITZ_MODERN
        {"export_picker", "Uniform Picker", "C-left at team select steps through the team's uniforms: the game's choice, the 1999 home and away, then its extra sets."},
#endif
        {"export_opponent", "Choose CPU Opponent",
         "In a 1-player game, the other side's list opens for you after your pick: you choose who the CPU plays as. "
         "Emulators built on mupen64plus (RetroArch's usual N64 core) freeze at team select with it."},
        {"export_rumble", "Skip Rumble Screen", "Controller Select goes straight to team select."},
        {"export_clock", "Play Clock", "Rules → Play Clock's seconds at play select (15 or 20)."},
        {"export_codes", "Blitz Codes", "Rules' codes and choices (heads, weather, stadium, turbo …) on in every game. The gameplay ones stay off in Season."},
        {"export_home", "Home Field", "Rules → Home Field: the home team's grass or turf, and no rain or snow under a roof."},
        {"export_realism", "Realism", "Rules → Realism: first-down yards, touchback spot, two-point spot, missable extra points, one overtime. Season too."},
    };

    struct Shortcut {
        uint32_t value;       // export_preset
        const char* name;
        const char* about;
        std::vector<const char*> off;  // everything else on; Custom (empty name list unused) keeps the toggles
        bool custom = false;
    };
    const std::vector<Shortcut> shortcuts = {
#if BLITZ_ENGINE_ORIGINAL
        {3, "LITE: ARCADE", "Every uniform. Arcade, Tournament, the game's own Season. Any N64 or emulator.",
         {"export_season", "export_opponent"}},
        {1, "LITE: SEASON", "The 2026 season, with 2026 home and away uniforms. Any N64.",
         {"export_uniforms", "export_opponent"}},
        {0, "FULL", "Everything. On a real N64 the 2026 season needs an Expansion Pak.", {}},
        // Game → Original 1999 (adv_mode.cpp): the only shortcut then, with Custom.
        {4, "ORIGINAL 1999", "The game as it shipped: its 31 teams and its own Season, with your extras. Any N64 or emulator.",
         {"export_teams32", "export_season", "export_uniforms", "export_picker"}},
#elif BLITZ_MODERN
        {0, "TEAM EXTENSION", "Your Team Extension ROM's 32 teams and every extra.", {}},
#else
        {0, "CLASSIC", "Your rosters and art, and every extra.", {}},
#endif
        {2, "CUSTOM", "Pick the extras yourself below.", {}, true},
    };

    recomp::config::Config& cfg() { return recompui::config::get_config("export"); }
    bool on(const char* id) { return std::get<bool>(cfg().get_option_value(id)); }
    uint32_t preset() { return std::get<uint32_t>(cfg().get_option_value("export_preset")); }

    void mark(Button* b, bool chosen) {  // the Roster Room's look: navy, a yellow rim on what's chosen (colours only:
                                         // apply_button_style would drop the focus look)
        b->set_background_color(chosen ? Color{58, 50, 8, 235} : Color{7, 15, 34, 235});
        b->set_border_color(chosen ? Color{246, 212, 0, 255} : Color{143, 239, 245, 70});
    }

#if BLITZ_MODERN
    struct RosterChoice {
        uint32_t value;  // export_rosters
        const char* name;
        const char* about;
    };
    const std::vector<RosterChoice> roster_choices = {
        {0, "Current", "This week's lineups, injuries and all."},
        {1, "2026 Season", "Every team at full strength, as if everyone were healthy."},
        {2, "Custom", "This week's lineups with your Roster Room crews. Opens the Roster Room in your browser."},
    };
#endif
    struct Screen {
        ContextId context = ContextId::null();
        std::vector<std::pair<Button*, uint32_t>> rosters;
        std::vector<std::pair<Button*, const Shortcut*>> cuts;
        Element* rosters_row = nullptr;
        struct Row { Button* button; Label* state; const Extra* extra; };
        std::vector<Row> rows;
        Label* about = nullptr;
        Label* intro = nullptr;
        Button* first = nullptr;
    } sc;

    // Opened by Blitz Builder's N64 or emulator path (firstrun.cpp): its Back goes back a step there, and Export tells
    // it the export is under way. Both empty when opened any other way.
    std::function<void()> setup_back, setup_exported;
    std::string default_intro;

    void close_back() {
        recompui::hide_context(sc.context);
        if (auto f = std::exchange(setup_back, nullptr)) {
            setup_exported = nullptr;
            f();
        }
    }

    void restyle() {
        uint32_t p = preset();
        for (auto& [b, s] : sc.cuts) mark(b, s->value == p);
#if BLITZ_MODERN
        uint32_t ro = std::get<uint32_t>(cfg().get_option_value("export_rosters"));
        for (auto& [b, v] : sc.rosters) mark(b, v == ro);
#endif
        for (auto& r : sc.rows) {
            bool v = on(r.extra->id);
            mark(r.button, v);
            r.state->set_text(v ? "ON" : "OFF");
            r.state->set_color(v ? Color{246, 212, 0, 255} : Color{184, 196, 218, 255});
        }
    }

    // A shortcut's values into the toggles.
    void set_values(uint32_t value) {
        for (const Shortcut& s : shortcuts) {
            if (s.value != value || s.custom) continue;
            for (const Extra& e : extras) {
                bool v = true;
                for (const char* off : s.off) if (std::string(off) == e.id) v = false;
                if (on(e.id) != v) cfg().update_option_value(e.id, v);
            }
        }
    }

    void apply(const Shortcut& s) {
        blitz::export_applying = true;  // export_rom.cpp's change callbacks stand aside
        cfg().update_option_value("export_preset", s.value);
        set_values(s.value);
        blitz::export_applying = false;
        cfg().save_config();
        restyle();
    }

    class Root : public Element {
    protected:
        std::string_view get_type_name() override { return "ExportRoot"; }
        void process_event(const Event& e) override {
            if (e.type == EventType::MenuAction && std::get<EventMenuAction>(e.variant).action == MenuAction::Back) {
                close_back();
            }
        }
    public:
        Root(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::MenuAction), "div", false) {}
    };

    class Hints : public Element {
        recompinput::InputDevice shown_device = recompinput::InputDevice::COUNT;
        int shown_profile = -2;
    protected:
        std::string_view get_type_name() override { return "ExportHints"; }
        void process_event(const Event& e) override {
            if (e.type != EventType::Update) return;
            auto [device, profile] = hint_input_info();
            if (device != shown_device || profile != shown_profile) {
                shown_device = device;
                shown_profile = profile;
                build_hint_row(get_current_context(), this, {{{MenuAction::Accept}, "Choose"}, {{MenuAction::Back}, "Back"}}, device, profile);
            }
            queue_update();
        }
    public:
        Hints(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Update), "div", false) {
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

    Label* text(Element* parent, const std::string& s, theme::Typography t, float size, Color color) {
        Label* l = sc.context.create_element<Label>(parent, s, t);
        l->set_font_size(blitz::ui::px(size));
        l->set_color(color);
        return l;
    }

    Element* row(Element* parent, float gap) {
        Element* r = sc.context.create_element<Element>(parent);
        r->set_display(Display::Flex);
        r->set_flex_direction(FlexDirection::Row);
        r->set_gap(blitz::ui::px(gap));
        r->set_as_navigation_container(NavigationType::GridRow);
        return r;
    }

    void build() {
        blitz::ui::load();
        sc.context = create_context();
        sc.context.open();
        Element* root = sc.context.create_element<Root>(sc.context.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(0);
        root->set_left(0);
        root->set_right(0);
        root->set_bottom(0);
        root->set_display(Display::Flex);
        root->set_align_items(AlignItems::Center);
        root->set_justify_content(JustifyContent::Center);
        root->set_background_color(Color{4, 10, 26, 235});
        Element* card = sc.context.create_element<Element>(root);
        card->set_position(Position::Relative);
        card->set_width(blitz::ui::px(860));
        blitz::ui::PanelOptions o;
        o.corners = true;
        Element* content = blitz::ui::panel(sc.context, card, o);
        content->set_display(Display::Flex);
        content->set_flex_direction(FlexDirection::Column);
        content->set_gap(blitz::ui::px(6));
        content->set_padding(blitz::ui::px(16));
        content->set_as_navigation_container(NavigationType::GridCol);

        text(content, "FOR A REAL N64, A FLASH CART OR AN EMULATOR", theme::Typography::LabelXS, 11, Color{143, 239, 245, 255});
        Label* title = sc.context.create_element<Label>(content, "EXPORT ROM", theme::Typography::Header2);
        title->set_color(theme::color::SecondaryL);
        Element* rule = sc.context.create_element<Element>(content, 0, "div", false);
        rule->set_height(blitz::ui::px(4));
        rule->set_position(Position::Relative);
        blitz::ui::stretch(sc.context, rule, "rule_flame");
#if BLITZ_MODERN
        const char* intro = "A .z64 with your rosters, art and uniforms built in, saved to Downloads. Its name says which kind it is.";
#else
        const char* intro = "A .z64 with your rosters and art built in, saved to your Downloads folder. The name says "
                            "when you made it.";
#endif
        default_intro = intro;
        sc.intro = text(content, intro, theme::Typography::Body, 15, Color{242, 242, 242, 255});
        sc.intro->set_white_space(WhiteSpace::Normal);

        Element* cuts = row(content, 8);
        for (const Shortcut& s : shortcuts) {
            Button* b = sc.context.create_element<Button>(cuts, "", ButtonStyle::Secondary, ButtonSize::Small);
            b->hide_focus_ring();
            b->set_flex(1.0f, 1.0f, 0.0f, Unit::Dp);
            b->set_height(blitz::ui::px(78));
            b->set_min_height(blitz::ui::px(78));
            b->set_max_height(blitz::ui::px(78));
            b->set_padding(blitz::ui::px(8));
            b->set_display(Display::Flex);
            b->set_flex_direction(FlexDirection::Column);
            b->set_align_items(AlignItems::FlexStart);
            b->set_justify_content(JustifyContent::FlexStart);
            b->set_gap(blitz::ui::px(3));
            text(b, s.name, theme::Typography::Header3, 18, Color{242, 242, 242, 255});
            Label* a = text(b, s.about, theme::Typography::Body, 11, Color{184, 196, 218, 255});
            a->set_white_space(WhiteSpace::Normal);
            a->set_line_height(blitz::ui::px(14));
            a->set_text_align(TextAlign::Left);
            const Shortcut* sp = &s;
            b->add_pressed_callback([sp] { apply(*sp); });
            b->set_focus_callback([sp](bool f) { if (f) sc.about->set_text(sp->about); });
            sc.cuts.push_back({b, sp});
            if (s.value == 0) sc.first = b;
        }

#if BLITZ_MODERN
        Element* ros = row(content, 6);
        sc.rosters_row = ros;
        ros->set_align_items(AlignItems::Center);
        Label* rl = text(ros, "ROSTERS", theme::Typography::LabelXS, 11, Color{143, 239, 245, 255});
        rl->set_width(blitz::ui::px(110));
        for (const RosterChoice& rc : roster_choices) {
            Button* b = sc.context.create_element<Button>(ros, rc.name, ButtonStyle::Secondary, ButtonSize::Small);
            b->hide_focus_ring();
            b->set_flex(1.0f, 1.0f, 0.0f, Unit::Dp);
            const RosterChoice* rp = &rc;
            b->add_pressed_callback([rp] {
                cfg().update_option_value("export_rosters", rp->value);
                cfg().save_config();
                restyle();
                if (rp->value == 2) blitz::roster_room_open();  // Custom: your crews, picked in the Roster Room
            });
            b->set_focus_callback([rp](bool f) { if (f) sc.about->set_text(rp->about); });
            sc.rosters.push_back({b, rp->value});
        }
#endif

        text(content, "WHAT GOES IN", theme::Typography::LabelXS, 11, Color{143, 239, 245, 255});
        Element* line = nullptr;
        for (size_t i = 0; i < extras.size(); i++) {
            if (i % 4 == 0) line = row(content, 6);
            const Extra* e = &extras[i];
            Button* b = sc.context.create_element<Button>(line, "", ButtonStyle::Secondary, ButtonSize::Small);
            b->hide_focus_ring();
            b->set_flex(1.0f, 1.0f, 0.0f, Unit::Dp);
            b->set_height(blitz::ui::px(38));
            b->set_min_height(blitz::ui::px(38));
            b->set_max_height(blitz::ui::px(38));
            b->set_padding(blitz::ui::px(6));
            b->set_display(Display::Flex);
            b->set_flex_direction(FlexDirection::Row);
            b->set_align_items(AlignItems::Center);
            b->set_justify_content(JustifyContent::SpaceBetween);
            Label* n = text(b, e->name, theme::Typography::Body, 12, Color{242, 242, 242, 255});
            n->set_text_align(TextAlign::Left);
            Label* st = text(b, "", theme::Typography::LabelXS, 11, Color{246, 212, 0, 255});
            b->add_pressed_callback([e] {
                cfg().update_option_value(e->id, !on(e->id));  // export_rom.cpp's callback makes it Custom
                cfg().save_config();
                restyle();
            });
            b->set_focus_callback([e](bool f) { if (f) sc.about->set_text(e->about); });
            sc.rows.push_back({b, st, e});
        }
        if (extras.size() % 4) for (size_t i = extras.size() % 4; i < 4; i++) {  // keep the last row's widths
            Element* gap = sc.context.create_element<Element>(line);
            gap->set_flex(1.0f, 1.0f, 0.0f, Unit::Dp);
        }
        sc.about = text(content, "", theme::Typography::Body, 12, Color{246, 212, 0, 255});
        sc.about->set_height(blitz::ui::px(30));

        text(content, "ON A REAL N64 AND IN EMULATORS", theme::Typography::LabelXS, 11, Color{143, 239, 245, 255});
        for (const char* note : {
#if BLITZ_ENGINE_ORIGINAL
                 "Full's 2026 season needs an Expansion Pak (a flash cart can't add memory); without one it plays the 1999 "
                 "Season. Lite: Season carries fewer uniforms to fit the 2026 season in a plain N64.",
                 "RetroArch and other mupen64plus emulators freeze at team select with Choose CPU Opponent: the Lites leave "
                 "it off. Skin tones come out as the game's two shades.",
#else
                 "RetroArch and other mupen64plus emulators freeze at team select with Choose CPU Opponent: use Ares, or "
                 "turn it off. Skin tones come out as the game's two shades.",
#endif
             }) {
            Label* l = text(content, std::string("•  ") + note, theme::Typography::Body, 12, Color{220, 226, 238, 255});
            l->set_white_space(WhiteSpace::Normal);
        }

        Element* bar = sc.context.create_element<Element>(content);
        bar->set_display(Display::Flex);
        bar->set_flex_direction(FlexDirection::Row);
        bar->set_justify_content(JustifyContent::FlexEnd);
        bar->set_flex_wrap(FlexWrap::Wrap);
        bar->set_gap(blitz::ui::px(8));
        bar->set_margin_top(blitz::ui::px(6));
        bar->set_as_navigation_container(NavigationType::Horizontal);
        Button* back = sc.context.create_element<Button>(bar, "Back", ButtonStyle::Tertiary, ButtonSize::Card);
        back->hide_focus_ring();
        back->set_min_width(blitz::ui::px(110));
        back->add_pressed_callback([] { close_back(); });
        back->set_focus_callback([](bool f) { if (f) sc.about->set_text("Back to the menu, nothing exported."); });
        Button* go = sc.context.create_element<Button>(bar, "Export", ButtonStyle::Primary, ButtonSize::Card);
        go->hide_focus_ring();
        go->set_min_width(blitz::ui::px(140));
        go->add_pressed_callback([] {
            recompui::hide_context(sc.context);
            setup_back = nullptr;
            if (auto f = std::exchange(setup_exported, nullptr)) f();
            blitz::export_rom_go();
        });
        go->set_focus_callback([](bool f) { if (f) sc.about->set_text("Saves the ROM to your Downloads folder."); });
        sc.context.create_element<Hints>(root);
        sc.context.close();
    }
}

namespace blitz {
    // Settings → Export's Kind of ROM: its shortcut's values (with export_applying set by the caller).
    void export_preset_values(uint32_t value) { set_values(value); }

    // Blitz Builder → N64 or emulator (firstrun.cpp): the screen as it is, with a line of its own first and Lite
    // chosen (it fits a plain N64 and the emulators that freeze with Choose CPU Opponent).
    void export_screen_open_for_setup(const std::string& lead, std::function<void()> back, std::function<void()> exported) {
        void export_screen_open_with(const std::string& lead);
        setup_back = std::move(back);
        setup_exported = std::move(exported);
#if BLITZ_ENGINE_ORIGINAL
        if (blitz::season_content() && !blitz::original_game() && preset() != 1 && preset() != 3)
            cfg().update_option_value("export_preset", (uint32_t)1);  // Lite: Season
#endif
        export_screen_open_with(lead);
    }

    void export_screen_close() {
        setup_back = nullptr;
        setup_exported = nullptr;
        if (sc.context != ContextId::null() && recompui::is_context_shown(sc.context)) recompui::hide_context(sc.context);
    }

    void export_screen_open() {
        void export_screen_open_with(const std::string& lead);
        setup_back = nullptr;
        setup_exported = nullptr;
        export_screen_open_with("");
    }

    void export_screen_open_with(const std::string& lead) {
        if (sc.context == ContextId::null()) build();
        if (recompui::is_context_shown(sc.context)) return;
        sc.context.open();
        sc.intro->set_text(lead.empty() ? default_intro : lead + " " + default_intro);
#if BLITZ_ENGINE_ORIGINAL
        // Original 1999 shows its own shortcut (and Custom) and no roster choice: the ROM's own teams go in. So does New
        // Blitz without the 2026 Season (2026-11-13): no 2026 rosters, season or uniforms to put on a cart.
        bool orig = blitz::original_game() || !blitz::season_content();
        for (auto& [b, s] : sc.cuts) b->set_display(orig == (s->value == 4) || s->custom ? Display::Flex : Display::None);
        if (sc.rosters_row) sc.rosters_row->set_display(orig ? Display::None : Display::Flex);
        if (orig && preset() != 4 && preset() != 2) cfg().update_option_value("export_preset", (uint32_t)4);
        if (!orig && preset() == 4) cfg().update_option_value("export_preset", (uint32_t)0);
#endif
        // A shortcut's values again (a toggle changed in Settings makes it Custom; an older app folder may differ).
        for (const Shortcut& s : shortcuts)
            if (s.value == preset() && !s.custom) apply(s);
        restyle();
        Button* focus = sc.first;
        for (auto& [b, s] : sc.cuts) if (s->value == preset()) focus = b;
        sc.about->set_text("");
        sc.context.set_autofocus_element(focus);
        sc.context.close();
        recompui::show_context(sc.context, "");
    }
}
