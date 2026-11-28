// Settings for 1.0.5 (docs/SETTINGS_LAYOUT.md): the layout that keeps General to app-wide things (settings_layout.h),
// the Saves tab, and the Two Screens tab with its built-in modes and Custom Layout (two_screens.h).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "recompui/config.h"
#include "settings_layout.h"
#include "two_screens.h"

namespace blitz {
    void tall_set(const std::string& name, float value);  // tall_view.cpp
    void settings_hold_redraw();                         // settings_hold.cpp
}

namespace {
    constexpr const char* screens_id = "screens";
    constexpr const char* custom_id = "custom";
    constexpr const char* reset_id = "reset";
    bool screens_found = false;  // two screens in use this run (ds_settings_found)
    bool screens_tab_shown = false;

    recomp::config::Config* screens_config() {
        try {
            return &recompui::config::get_config(screens_id);
        }
        catch (const std::exception&) {
            return nullptr;  // not created yet
        }
    }

    double dial_value(recomp::config::Config& c, const blitz::two_screens::Dial& d) {
        const recomp::config::ConfigValueVariant v = c.get_option_value(d.id);
        if (const double* n = std::get_if<double>(&v)) return *n;
        if (const uint32_t* e = std::get_if<uint32_t>(&v)) return (double)*e;
        return d.builtin;
    }

    void set_dial(recomp::config::Config& c, const blitz::two_screens::Dial& d, double v) {
        if (d.choices != nullptr) c.update_option_value(d.id, (uint32_t)v);
        else c.update_option_value(d.id, v);
    }

    bool custom_on() {
        recomp::config::Config* c = screens_config();
        return c != nullptr && std::get<bool>(c->get_option_value(custom_id));
    }

    uint32_t general_choice(const char* id) {
        return std::get<uint32_t>(recompui::config::get_general_config().get_option_value(id));
    }

    // Custom Layout's rows show with it on; the Field Ahead and Camera Zoom Out presets stay in sight, greyed, as it sets
    // the camera then.
    void screens_visibility() {
        recomp::config::Config* c = screens_config();
        if (c == nullptr) return;
        const bool on = custom_on();
        for (const auto& d : blitz::two_screens::dials()) c->update_option_hidden(d.id, !on);
        c->update_option_hidden(reset_id, !on);
        recomp::config::Config& g = recompui::config::get_general_config();
        g.update_option_disabled("ds_tilt", on);
        g.update_option_disabled("ds_zoom", on);
    }
}

namespace blitz {
    // ---- Saves ----------------------------------------------------------------------------------------------------------
    // Save Sync (branch cloud-sync) and anything else with saves to offer adds its rows here: a function that adds them to
    // the Saves tab's own config (saves.json), registered before Settings is built (a static initializer, as
    // savestate_register does). They're drawn after the save state and Controller Pak rows, before Reset Data.
    std::vector<std::function<void(recomp::config::Config&)>>& saves_entry_adders() {
        static std::vector<std::function<void(recomp::config::Config&)>> adders;
        return adders;
    }
    void saves_tab_add_entries(std::function<void(recomp::config::Config&)> add) { saves_entry_adders().push_back(std::move(add)); }
    void saves_tab_entries(recomp::config::Config& saves) {
        for (auto& add : saves_entry_adders()) add(saves);
    }

    void create_saves_tab() {
        recomp::config::Config& saves = recompui::config::create_config_tab("Saves", "saves", false);
        saves_tab_entries(saves);
    }

    // ---- Two Screens ----------------------------------------------------------------------------------------------------
    // Sets tall_view.cpp's dials from the presets or Custom Layout (two_screens.h resolve). `all`: every dial, as when
    // Custom Layout goes on or off; otherwise with it off only the camera's two, as before 1.0.5 (the rest stay at
    // tall_view's own values, or a dev's TALL file).
    void two_screens_apply(bool all) {
        recomp::config::Config* c = screens_config();
        const bool on = custom_on();
        std::map<std::string, double> custom;
        if (c != nullptr && on) {
            for (const auto& d : two_screens::dials()) custom[d.id] = dial_value(*c, d);
        }
        for (const auto& [name, v] : two_screens::resolve(on, general_choice("ds_tilt"), general_choice("ds_zoom"), custom)) {
            if (all || on || name == "tilt" || name == "zoom") tall_set(name, v);
        }
        settings_hold_redraw();  // shown on the held moment if Settings is up
    }

    // Two screens in use (config.cpp ds_settings_found): the dials take hold.
    void two_screens_found() {
        screens_found = true;
        two_screens_apply(custom_on());
        screens_visibility();
    }

    // The tab is only for a device with a second screen (or the Development rows shown).
    void two_screens_tab_visible(bool visible) {
        if (visible == screens_tab_shown) return;
        screens_tab_shown = visible;
        recompui::config::set_tab_visible(screens_id, visible);
    }

    void create_two_screens_tab() {
        recomp::config::Config& c = recompui::config::create_config_tab("DS", screens_id, false);  // "DS" (team, 2026-11-27; was Two Screens)
        c.add_bool_option(custom_id, "Custom Layout",
                          "Your own camera and placement for everything on the two screens, past what the modes above "
                          "offer. It starts from the camera you have now; <b>Reset Custom Layout</b> puts every dial back. "
                          "Saved on this device only. Off: the built-in layout.",
                          false);
        for (const auto& d : two_screens::dials()) {
            if (d.choices != nullptr) {
                std::vector<recomp::config::ConfigOptionEnumOption> opts;
                for (uint32_t i = 0; d.choices[i] != nullptr; i++) opts.emplace_back(i, std::to_string(i), d.choices[i]);
                c.add_enum_option(d.id, d.name, d.desc, opts, (uint32_t)d.builtin);  // hidden by screens_visibility, not for good
            }
            else {
                c.add_number_option(d.id, d.name, d.desc, d.min, d.max, d.step, d.precision, false, d.builtin);
            }
            const two_screens::Dial* dial = &d;
            c.add_option_change_callback(d.id, [dial](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                      recomp::config::OptionChangeContext ctx) {
                if (ctx == recomp::config::OptionChangeContext::Load || !screens_found || !custom_on()) return;
                const double v = std::holds_alternative<double>(cur) ? std::get<double>(cur) : (double)std::get<uint32_t>(cur);
                tall_set(dial->tall, two_screens::to_tall(*dial, v));
                settings_hold_redraw();
            });
        }
        // Not added hidden: an option added hidden is hidden for good (librecomp update_option_hidden ignores it), which kept
        // Custom Layout's rows from ever showing (the team's Duo, 2026-11-27). screens_visibility hides them while it's off.
        c.add_bool_option(reset_id, "Reset Custom Layout", "Every dial back to the built-in layout, the camera to the presets above.",
                          false);
        recompui::config::set_option_action(screens_id, reset_id, "Reset");
        recompui::config::set_option_section(screens_id, custom_id, "CUSTOM LAYOUT");

        // Starts from the presets in use (seed_from_presets), so turning it on changes nothing until a dial moves.
        auto seed = [] {
            recomp::config::Config* sc = screens_config();
            if (sc == nullptr) return;
            const auto values = two_screens::seed_from_presets(general_choice("ds_tilt"), general_choice("ds_zoom"));
            for (const auto& d : two_screens::dials()) set_dial(*sc, d, values.at(d.id));
        };
        c.add_option_change_callback(custom_id, [seed](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                       recomp::config::OptionChangeContext ctx) {
            if (ctx == recomp::config::OptionChangeContext::Load) return;
            if (std::get<bool>(cur)) seed();
            screens_visibility();
            if (screens_found) two_screens_apply(true);
            fprintf(stderr, "[screens] custom layout %s\n", std::get<bool>(cur) ? "on" : "off");
        });
        c.add_option_change_callback(reset_id, [seed](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                      recomp::config::OptionChangeContext ctx) {
            if (ctx != recomp::config::OptionChangeContext::Permanent || !std::get<bool>(cur)) return;
            recompui::config::get_config(screens_id).update_option_value(reset_id, false);
            seed();
            if (screens_found) two_screens_apply(true);
            fprintf(stderr, "[screens] custom layout reset\n");
        });
        // Hidden until a second screen is seen (config.cpp ds_second_seen) or the Development rows are shown.
        recompui::config::set_tab_visible(screens_id, false);
    }

    // After the configs load (config.cpp, after finalize): Custom Layout's rows as saved.
    void two_screens_loaded() { screens_visibility(); }

    // ---- The layout -----------------------------------------------------------------------------------------------------
    void apply_settings_layout() {
        for (const auto& p : settings_layout::placements) {
            recompui::config::place_option(p.config, p.option, p.tab, p.first);
            if (p.section != nullptr) recompui::config::set_option_section(p.config, p.option, p.section);
        }
        // Bracket is on Teams now (its config, party.json, unchanged).
        recompui::config::set_tab_visible("party", false);
    }
}
