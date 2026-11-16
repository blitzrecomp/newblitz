// The launcher screen: the Blitz logo instead of a text title, and the options that matter here
// (no Mods: Blitz has none). The 2026 app also opens its content-pack and art drop-in folders,
// which otherwise live out of sight in Application Support.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <SDL.h>

#include "recompui/recompui.h"  // first: it includes base/ui_launcher.h itself
#include "elements/ui_image.h"
#include "elements/ui_label.h"
#include "recompui/config.h"
#include "librecomp/game.hpp"
#include "blitz_config.h"
#include "logo_bytes.h"
#include "ui_kit.h"
#include "os_shell.h"
#include "install_expiry.h"
#include <array>
#include <atomic>

namespace blitz {
#ifdef BLITZ_IOS
    bool is_ipad();  // platform_ios.mm
#else
    inline bool is_ipad() { return false; }
#endif
}

namespace {
    // The launcher's "RECOMPILED FOR ..." line: this platform's name (include/blitz_platform.h), as people say it.
    const char* platform_caps() {
        std::string_view n = blitz::platform::name;
        return n == "macOS" ? "MAC" : n == "Windows" ? "WINDOWS" : n == "Linux" ? "LINUX" : n == "iOS" ? (blitz::is_ipad() ? "IPAD" : "IPHONE")
             : n == "Android" ? "ANDROID" : "PLAYSTATION 5";
    }

    void open_folder(const std::filesystem::path& dir) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        blitz::open_in_file_browser(dir);
    }
}

namespace blitz {
    void start_blitz_party();  // main.cpp
    std::array<std::string, 4> port_descriptions();  // controllers.cpp
}

namespace blitz {
    std::string stored_rom_display_name();  // known_roms.cpp
    bool other_engine_available();          // experience.cpp
    bool other_engine_has_rom();
    bool engines_share_rom();
    const char* other_engine_title();
    void experience_switch();
    void builder_open();  // firstrun.cpp: Blitz Builder, the setup questions again
    bool firstrun_unfinished();          // firstrun.cpp: setup isn't done (welcomed is false)
    std::string firstrun_rom_line();     // firstrun.cpp: "Using your saved ROM · ..."
    void firstrun_finish_setup_row();
    extern std::atomic<int> stored_rom_changes;
}

namespace {
    using namespace recompui;

    // One line under the title: which ROM to load until one is (with no CHANGE ROM: nothing to change yet), then
    // which one is in use (docs/design/FIRST_RUN_V2.md §3, "the player always knows which ROM is in use"): "Using
    // your saved ROM · NFL Blitz 2000 (USA v1.1) · Change ROM", or for one stored before this install's setup began,
    // "Using a ROM saved earlier (3 Nov 2026) · Change ROM". Follows Load ROM straight away (stored_rom_changes) and
    // is re-checked now and then. (The "New here? Play to start" tip it showed went with the launcher at every start:
    // first run does that job now.)
    class RomNeededNote : public Element {
        Label* label = nullptr;
        std::vector<Element*> needs_rom;  // shown only once a ROM is loaded (Change ROM, Blitz Builder)
        Element* switch_item = nullptr;   // shown once the other game has a ROM
        Element* finish_item = nullptr;   // Finish setup: shown while setup isn't done
        int shown_switch = -1;
        int shown_finish = -1;
        int shown_changes = -1;
        int ticks = 0;
        int state = -1;  // 0 no ROM, 1 a ROM
        std::string shown_line;
    protected:
        std::string_view get_type_name() override { return "BlitzRomNeeded"; }
        void process_event(const Event& e) override {
            if (e.type != EventType::Update) return;
            int changes = blitz::stored_rom_changes.load();
            if (changes != shown_changes || ++ticks >= 60) {
                shown_changes = changes;
                ticks = 0;
                std::u8string id = blitz::game_id;
                bool ready = recomp::is_rom_valid(id);
                int now = ready ? 1 : 0;
                std::string line = ready ? blitz::firstrun_rom_line()
                                         : blitz::engines_share_rom() ? shared_text : blitz::other_engine_available() ? both_text : need_text;
                if (now != state || line != shown_line) {
                    state = now;
                    shown_line = line;
                    label->set_text(line);
                    for (Element* e : needs_rom) e->set_display(ready ? Display::Flex : Display::None);  // menu items are flex
                }
                int fin = blitz::firstrun_unfinished() ? 1 : 0;
                if (finish_item != nullptr && fin != shown_finish) {
                    shown_finish = fin;
                    finish_item->set_display(fin ? Display::Flex : Display::None);
                }
                int sw = switch_item != nullptr && blitz::other_engine_has_rom() ? 1 : 0;
                if (switch_item != nullptr && sw != shown_switch) {
                    shown_switch = sw;
                    switch_item->set_display(sw ? Display::Flex : Display::None);
                }
            }
            queue_update();
        }
#if BLITZ_TEAMPLUS
        static constexpr const char* need_text = "Load NFL Blitz 2000 with Zoinkity's Team Extension, or a community update built on it";
#else
        static constexpr const char* need_text = "Load your NFL Blitz 2000 ROM: the USA cartridge, v1.1 (.z64, .n64, .v64 or a .zip)";
#endif
        // One app with both games: either ROM is fine, and the right game takes it.
        static constexpr const char* both_text = "Load your NFL Blitz 2000 ROM: the USA v1.1 plays Classic Blitz, a Team Extension ROM plays Modern";
        // BRANDING: app name in the Load ROM prompt
        static constexpr const char* shared_text = "Load your NFL Blitz 2000 ROM: the USA cartridge, v1.1. It plays New Blitz and the original";

    public:
        RomNeededNote(ResourceId rid, Element* parent, std::vector<Element*> rom_items, Element* switch_to, Element* finish)
            : Element(rid, parent, Events(EventType::Update), "div", false), needs_rom(std::move(rom_items)), switch_item(switch_to),
              finish_item(finish) {
            set_position(Position::Absolute);
            set_top(blitz::ui::px(244));
            set_left(blitz::ui::px(0));
            set_right(blitz::ui::px(0));
            set_display(Display::Flex);
            set_justify_content(JustifyContent::Center);
            label = get_current_context().create_element<Label>(this, "", theme::Typography::Body);
            label->set_font_size(blitz::ui::px(17));
            label->set_font_weight(600);
            label->set_color(Color{143, 239, 245, 255});
            queue_update();
        }
    };

    // Under the ROM line, a sideloaded iPhone or iPad whose signature runs out in 3 days or fewer (install_expiry.cpp):
    // "This install expires in 2 days (your saves stay). Keep your computer on for Sideloadly's refresh, or re-sign it:
    // newblitz.com/start", in the warning colour. One line on a phone; it wraps on a narrower screen (an iPad, a Mac
    // window), and the menu moves down by as much as it takes. Nothing for a paid account's year, or anywhere else.
    // Re-checked now and then: a launcher left up overnight counts down.
    class InstallExpiryNote : public Element {
        Label* label = nullptr;
        Element* menu_items = nullptr;  // the launcher's items: moved down while the line shows
        std::string shown = "-";
        float shown_top = -1.0f;
        int ticks = 0;
    protected:
        std::string_view get_type_name() override { return "BlitzInstallExpiry"; }
        void process_event(const Event& e) override {
            if (e.type != EventType::Update) return;
            if (ticks-- <= 0) {
                ticks = 600;
                std::string line = blitz::install_expiry_line();
                if (line != shown) {
                    shown = line;
                    label->set_text(line);
                    set_display(line.empty() ? Display::None : Display::Flex);
                }
            }
            // The menu's top: under the line as laid out (its height in design px), never above where it was.
            float top = menu_top;
            if (!shown.empty()) {
                float ratio = label->get_dp_to_pixel_ratio();
                float h = ratio > 0.0f ? label->get_client_height() / ratio / blitz::ui::k : 0.0f;
                top = std::max(menu_top, line_top + h + 6.0f);
            }
            if (std::abs(top - shown_top) > 0.5f) {
                shown_top = top;
                menu_items->set_top(blitz::ui::px(top));
            }
            queue_update();
        }
    public:
        static constexpr float menu_top = 286.0f;  // style_launcher's set_top_anchor
        static constexpr float line_top = 266.0f;
        InstallExpiryNote(ResourceId rid, Element* parent, Element* items)
            : Element(rid, parent, Events(EventType::Update), "div", false), menu_items(items) {
            set_position(Position::Absolute);
            set_top(blitz::ui::px(line_top));
            set_left(blitz::ui::px(24));
            set_right(blitz::ui::px(24));
            set_display(Display::None);
            set_justify_content(JustifyContent::Center);
            label = get_current_context().create_element<Label>(this, "", theme::Typography::Body);
            label->set_font_size(blitz::ui::px(16));
            label->set_font_weight(600);
            label->set_text_align(TextAlign::Center);
            label->set_color(theme::color::Warning);
            queue_update();
        }
    };

    // Under the version, top right: which known ROM is loaded ("NFL Blitz 2026 Gold Edition (Resurrection)"),
    // re-read after Load ROM / Change ROM. Nothing for a ROM we don't know by name.
    class RomNameLabel : public Element {
        Label* label = nullptr;
        int shown = -1;
    protected:
        std::string_view get_type_name() override { return "BlitzRomName"; }
        void process_event(const Event& e) override {
            if (e.type != EventType::Update) return;
            int now = blitz::stored_rom_changes.load();
            if (now != shown) {
                shown = now;
                label->set_text(blitz::stored_rom_display_name());
            }
            queue_update();
        }
    public:
        RomNameLabel(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Update), "div", false) {
            set_position(Position::Absolute);
            set_top(blitz::ui::px(38));
            set_right(blitz::ui::px(20));
            label = get_current_context().create_element<Label>(this, "", theme::Typography::Body);
            label->set_font_size(blitz::ui::px(14));
            label->set_font_weight(600);
            label->set_color(theme::color::TextDim);
            queue_update();
        }
    };
    // The launcher's footer: one chip per player slot (number in the player colour, device name), kept
    // current while the launcher is up; the confirm glyph and "SELECT" on the right.
    class PlayerFooter : public Element {
        std::array<Element*, 4> chips{};
        std::array<Label*, 4> names{};
        std::array<Element*, 4> numbers{};
        std::array<std::string, 4> shown{"-", "-", "-", "-"};
        Label* select_glyph = nullptr;
        Label* select_label = nullptr;
        std::string shown_glyph;
        int shown_touch = -1;
        static constexpr Color player[4] = {{58, 123, 255, 255}, {255, 77, 77, 255}, {47, 211, 107, 255}, {255, 210, 63, 255}};

        static std::string short_name(const std::string& d) {
            std::string s = d.substr(0, d.find(" \xC2\xB7 "));  // drop " · battery OK"
            size_t k = s.find(" + Keyboard");
            return k == std::string::npos ? s : s.substr(0, k);
        }

    protected:
        std::string_view get_type_name() override { return "BlitzPlayerFooter"; }
        void process_event(const Event& e) override {
            if (e.type != EventType::Update) return;
            // The confirm button as the current glyph style draws it (Button Icons can change while up).
            std::string glyph = recompinput::InputField::controller_digital(SDL_CONTROLLER_BUTTON_A).to_string();
            if (glyph != shown_glyph) {
                shown_glyph = glyph;
                select_glyph->set_text(glyph);
            }
            // Touch alone (a phone with no controller): the items themselves are tapped, so no button to name.
            int touch = blitz::ui::touch_only() ? 1 : 0;
            if (touch != shown_touch) {
                shown_touch = touch;
                select_glyph->set_display(touch ? Display::None : Display::Block);
                select_label->set_display(touch ? Display::None : Display::Block);
            }
            auto now = blitz::port_descriptions();
            for (int i = 0; i < 4; i++) {
                if (now[i] == shown[i]) continue;
                shown[i] = now[i];
                bool open = now[i].empty();
                names[i]->set_text(open ? "Open" : short_name(now[i]));
                chips[i]->set_border_color(open ? Color{143, 239, 245, 61} : player[i]);
                // An open slot is quiet: outlined number, steel at 80%; a connected one is in the player's colour.
                numbers[i]->set_background_color(open ? Color{0, 0, 0, 0} : player[i]);
                numbers[i]->set_border_color(open ? Color{184, 196, 218, 115} : player[i]);
                numbers[i]->set_color(open ? Color{184, 196, 218, 204} : Color{2, 7, 18, 255});
                names[i]->set_color(open ? Color{184, 196, 218, 204} : Color{242, 242, 242, 255});
            }
            queue_update();
        }

    public:
        PlayerFooter(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Update), "div", false) {
            ContextId context = get_current_context();
            set_position(Position::Absolute);
            set_left(blitz::ui::px(24));
            set_right(blitz::ui::px(24));
            set_bottom(blitz::ui::px(20));
            set_display(Display::Flex);
            set_align_items(AlignItems::Center);
            set_gap(blitz::ui::px(10));
            for (int i = 0; i < 4; i++) {
                Element* chip = context.create_element<Element>(this, 0, "div", false);
                chip->set_display(Display::Flex);
                chip->set_align_items(AlignItems::Center);
                chip->set_gap(blitz::ui::px(8));
                chip->set_height(blitz::ui::px(36));
                chip->set_padding_left(blitz::ui::px(4));
                chip->set_padding_right(blitz::ui::px(10));
                chip->set_border_width(blitz::ui::px(2));
                chip->set_border_radius(blitz::ui::px(4));
                chip->set_border_color(player[i]);
                chip->set_background_color(Color{7, 15, 34, 235});
                Element* num = context.create_element<Element>(chip, 0, "div", false);
                num->set_width(blitz::ui::px(28));
                num->set_height(blitz::ui::px(28));
                num->set_border_radius(blitz::ui::px(3));
                num->set_border_width(blitz::ui::px(2));
                num->set_display(Display::Flex);
                num->set_align_items(AlignItems::Center);
                num->set_justify_content(JustifyContent::Center);
                context.create_element<Label>(num, std::to_string(i + 1), theme::Typography::Header3)->set_font_size(blitz::ui::px(22));
                Label* name = context.create_element<Label>(chip, "", theme::Typography::Body);
                name->set_font_size(blitz::ui::px(18));
                name->set_font_weight(600);
                name->set_white_space(WhiteSpace::Nowrap);
                name->set_overflow(Overflow::Hidden);
                chip->set_max_width(blitz::ui::px(180));
                chips[i] = chip;
                numbers[i] = num;
                names[i] = name;
            }
            Element* spacer = context.create_element<Element>(this, 0, "div", false);
            spacer->set_flex_grow(1.0f);
            select_glyph = context.create_element<Label>(this, "", theme::Typography::LabelSM);
            select_glyph->set_font_family("promptfont");
            select_glyph->set_font_style(FontStyle::Normal);
            select_glyph->set_font_weight(400);
            select_glyph->set_font_size(42.0f);
            select_glyph->set_letter_spacing(0.0f);
            select_glyph->set_color(theme::color::Text);
            Label* select = context.create_element<Label>(this, "SELECT", theme::Typography::LabelSM);
            select->set_color(theme::color::Text);
            select_label = select;
            queue_update();
        }
    };
}

#if BLITZ_MODERN
namespace {
    // Settings: the BLITZ '27 lockup, small at the bottom left, only on wide windows (at 4:3 the centred hint
    // row would run into it). Review P4.
    class SettingsBrandMark : public recompui::Element {
        int shown = -1;
    protected:
        std::string_view get_type_name() override { return "BlitzSettingsBrand"; }
        void process_event(const recompui::Event& e) override {
            if (e.type != recompui::EventType::Update) return;
            recompui::Element* root = recompui::get_current_context().get_root_element();
            float w = root->get_client_width(), h = root->get_client_height();
            int wide = h > 0.0f && w / h >= 1.5f ? 1 : 0;
            if (wide != shown) {
                shown = wide;
                set_display(wide ? recompui::Display::Block : recompui::Display::None);
            }
            queue_update();
        }
    public:
        SettingsBrandMark(recompui::ResourceId rid, recompui::Element* parent)
            : Element(rid, parent, recompui::Events(recompui::EventType::Update), "div", false) {
            using namespace recompui;
            set_position(Position::Absolute);
            set_left(blitz::ui::px(30));
            set_bottom(blitz::ui::px(8));  // in the hint band, clear of the panel's chrome corner
            set_height(blitz::ui::px(22));
            set_pointer_events(PointerEvents::None);
            set_display(Display::None);
            // BRANDING: BLITZ '27 lockup in Settings (assets/ui/logo27_lockup.png)
            Image* img = get_current_context().create_element<Image>(this, blitz::ui::src("logo27_lockup"));
            img->set_height(100.0f, Unit::Percent);
            queue_update();
        }
    };

    bool add_settings_brand_mark() {
        recompui::ContextId config_ctx = recompui::ContextId::null();
        try {
            config_ctx = recompui::config::get_config_context_id();
        } catch (const std::exception&) {
            return false;  // the settings overlay isn't made yet
        }
        recompui::ContextId prev = recompui::try_close_current_context();
        config_ctx.open();
        config_ctx.create_element<SettingsBrandMark>(config_ctx.get_root_element());
        config_ctx.close();
        if (prev != recompui::ContextId::null()) prev.open();
        return true;
    }
}
#endif

namespace blitz {
    // Main thread, every update (controllers.cpp): things that need the settings overlay, once it exists.
    void ui_late_init() {
#if BLITZ_MODERN
        static bool done = false;
        if (!done) done = add_settings_brand_mark();
#endif
    }
}

namespace blitz {
    void style_launcher(recompui::LauncherMenu* menu, recompui::GameOptionsMenu* options) {
        using namespace recompui;
        ContextId context = get_current_context();
        menu->remove_default_title();
        blitz::ui::load();

        // Potato Mode goes straight into the game (skip_launcher), so the launcher's art would only hold GPU memory: 7 MB for the
        // background and logo. BLITZ_LAUNCHER_ART=1 keeps them.
        const bool bare = getenv("BLITZ_LEAN") != nullptr && getenv("BLITZ_LAUNCHER_ART") == nullptr;

        // The title-screen background: purple swoosh, stadium grid, flame floor; full height, centred.
        Element* bg = menu->get_background_wrapper();
        bg->set_overflow(Overflow::Hidden);
        if (!bare) {
            Image* back = context.create_element<Image>(bg, blitz::ui::src("bg_title"));
            back->set_position(Position::Absolute);
            back->set_top(blitz::ui::px(0));
            back->set_height(100.0f, Unit::Percent);
            back->set_left(50.0f, Unit::Percent);
            back->set_translate_2D(-50.0f, 0.0f, Unit::Percent);
            // BRANDING: launcher logo, icons/logo_2026.png (CMakeLists.txt BLITZ_LOGO)
            queue_image_from_bytes_file("?/blitz/logo", std::vector<char>(logo_bytes, logo_bytes + sizeof(logo_bytes)));
        }
        Element* title = context.create_element<Element>(menu, 0, "div", false);
        title->set_position(Position::Absolute);
        title->set_top(blitz::ui::px(36.0f));
        title->set_left(blitz::ui::px(0));
        title->set_right(blitz::ui::px(0));
        title->set_display(Display::Flex);
        title->set_flex_direction(FlexDirection::Column);
        title->set_align_items(AlignItems::Center);
        title->set_gap(blitz::ui::px(12.0f));
        Element* logo = bare ? context.create_element<Element>(title, 0, "div", false) : context.create_element<Image>(title, "?/blitz/logo");
#if BLITZ_MODERN
        logo->set_width(blitz::ui::px(560.0f));   // the NFL Blitz 2027 logo, 1400x434
        logo->set_height(blitz::ui::px(174.0f));
#else
        logo->set_width(blitz::ui::px(560.0f));
        logo->set_height(blitz::ui::px(176.0f));
#endif
        // Just the platform under the logo (team, 2026-11-15): the logo already names the game.
        const std::string app_name = std::string("RECOMPILED FOR ") + platform_caps();
        Element* subtitle = context.create_element<Element>(title, 0, "div", false);
        subtitle->set_display(Display::Flex);
        subtitle->set_align_items(AlignItems::Center);
        subtitle->set_gap(blitz::ui::px(14.0f));
        auto rule = [&](const char* name) {
            Element* r = context.create_element<Element>(subtitle, 0, "div", false);
            r->set_position(Position::Relative);
            r->set_width(blitz::ui::px(120));
            r->set_height(blitz::ui::px(4));
            blitz::ui::stretch(context, r, name);
        };
        rule("rule_chrome_rev");
        Label* sub = context.create_element<Label>(subtitle, app_name, theme::Typography::LabelXS);
        sub->set_letter_spacing(blitz::ui::px(4.3f));
        sub->set_color(theme::color::TextDim);
        rule("rule_chrome");

        // The version, top right.
        menu->get_version_label()->set_display(Display::None);
        Label* version = context.create_element<Label>(menu, "v" + recomp::get_project_version().to_string(), theme::Typography::Body);
        version->set_position(Position::Absolute);
        version->set_top(blitz::ui::px(14));
        version->set_right(blitz::ui::px(20));
        version->set_font_size(blitz::ui::px(18));  // (Body is already scaled; this sets the design size)
        version->set_font_weight(600);
        version->set_color(theme::color::TextDim);
        context.create_element<RomNameLabel>(menu);

        context.create_element<PlayerFooter>(menu);

        options->set_top_anchor(blitz::ui::px(InstallExpiryNote::menu_top), blitz::ui::px(420.0f));
        std::vector<GameOption*> items;
        // Finish setup, first, while setup isn't done (docs/design/FIRST_RUN_V2.md §6.1): the player's way back into
        // it after backing out, and a tester's way to tell "it didn't start by itself" from "it doesn't work".
        GameOption* finish = options->add_option("Finish Setup", [] { blitz::firstrun_finish_setup_row(); });
        items.push_back(finish);
        // A launcher about content (docs/design/ONE_APP.md): play, the ROM, add-ons, export, settings.
        items.push_back(options->add_start_game_or_load_rom_option("Load ROM", "Play"));
        // One app with both games: Switch to Classic / Modern Blitz, once that game has a ROM.
        GameOption* switch_item = nullptr;
        if (blitz::other_engine_available()) {
            switch_item = options->add_option("Switch to " + std::string(blitz::other_engine_title()), [] { blitz::experience_switch(); });
            items.push_back(switch_item);
        }
        // Switch ROMs: the Team Extension and community releases (Resurrection's Gold, Silver, ...) in Modern.
        GameOption* change_rom = options->add_change_rom_option("Change ROM");
        items.push_back(change_rom);
        items.push_back(options->add_option("Add-Ons", [] {  // as the Settings item does, on the Add-Ons tab
            recompui::update_game_mod_id(blitz::mod_game_id);
            recompui::config::set_tab("addons");
            recompui::hide_all_contexts();
            recompui::config::open();
        }));
        std::vector<Element*> rom_items{change_rom};
        // No Export ROM row (team, 2026-11-15): Blitz Builder → N64 or emulator makes the cart.
        // BRANDING: the setup tool's name, "Blitz Builder"
        // A computer's only (the hub model, the team 2026-11-15): it sets up phones, handhelds and the PS5, which have none.
        if (blitz::platform::desktop) {
            GameOption* builder = options->add_option("Blitz Builder", [] { blitz::builder_open(); });  // setup again
            items.push_back(builder);
            rom_items.push_back(builder);
        }
        items.push_back(options->add_settings_option());
        context.create_element<RomNeededNote>(menu, rom_items, switch_item, finish);
        context.create_element<InstallExpiryNote>(menu, options);
        if (!blitz::firstrun_unfinished()) finish->set_display(Display::None);
        else context.set_autofocus_element(finish);  // Play's own autofocus otherwise
        items.push_back(options->add_exit_option());
        // Chrome caps around the focused item, with the red bar (the theme's focus style). Every row gets a fixed
        // height: the caps span the row, and without one the bar drew past their bottoms. Six or more items
        // (Modern) sit a little tighter so EXIT stays clear of the player footer.
        for (GameOption* item : items) {
            if (item == nullptr) continue;
            item->set_height(blitz::ui::px(items.size() > 5 ? 46 : 52));
            auto [l, r] = blitz::ui::selector_caps(context, item);
            l->set_display(Display::None);
            r->set_display(Display::None);
            item->focus_adornments = {l, r};
        }
    }
}
