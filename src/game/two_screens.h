// Settings → Two Screens (settings_tabs.cpp): the built-in modes, then a Custom Layout whose dials are tall_view.cpp's
// own (the live command TALL <name> <value> sets the same ones). This header is the table and the rule that picks each
// dial's value, with nothing of the app in it, so tests/two_screens_test.cpp can check it.
//
// The rule: Custom Layout off, every dial is its built-in value (exactly tall_view.cpp's start values), and the camera's
// tilt and zoom come from the Field Ahead and Camera Zoom Out presets (General's ds_tilt and ds_zoom, saved as before).
// Custom Layout on, every dial is the player's, the camera's too. Turning it on starts the camera dials from the presets
// then in use (seed_from_presets), so nothing jumps.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace blitz::two_screens {
    // One dial. A choice dial (choices != nullptr) is an enum row whose values are 0, 1, 2...; the rest are sliders.
    struct Dial {
        const char* id;      // the option's id in screens.json
        const char* tall;    // tall_view.cpp's name for it
        const char* name;    // the row's name
        const char* desc;    // what it does, for players
        double min, max, step;
        int precision;
        double builtin;      // tall_view.cpp's start value, in the row's own units
        const char* const* choices = nullptr;  // a choice dial's names, null-terminated
        bool camera = false; // in the CAMERA part (else ON-SCREEN TEXT)
    };

    inline const char* const screen_choices[] = {"Upper Screen", "Lower Screen", nullptr};
    inline const char* const meter_choices[] = {"Upper Corner", "With the Rest", "Lower Corner", nullptr};

    // The two camera dials the presets also set. Field Ahead is tall_view's tilt as it is; Zoom Out is a percentage over the
    // game's own distance (tall_view's zoom is 1 + this / 100).
    inline constexpr const char* tilt_id = "tilt";
    inline constexpr const char* zoom_id = "zoom_out";

    inline const std::vector<Dial>& dials() {
        static const std::vector<Dial> all = {
            // Camera
            {tilt_id, "tilt", "Field Ahead",
             "How far the camera looks up the field. Higher keeps the play low on the lower screen and shows more of what's "
             "ahead on the upper one. The Field Ahead choices are 0.40 to 0.65.",
             0.20, 0.95, 0.01, 2, 0.50, nullptr, true},
            {zoom_id, "zoom", "Camera Zoom Out",
             "How much further back than the game's own camera, in percent, to keep the receivers on the ends in view.",
             0.0, 60.0, 0.5, 1, 12.5, nullptr, true},
            {"sky", "sky", "Sky Limit",
             "How far above level the top of the picture may look, in degrees. A low camera (a kickoff's) tilted further "
             "would look past the sky's art.", 10.0, 45.0, 1.0, 0, 28.0, nullptr, true},
            {"mark", "mark", "Marker Height",
             "How high over the players' heads their markers float.", 0.0, 40.0, 1.0, 0, 18.0, nullptr, true},
            {"wide_out", "wide_out", "Wide View on Throws",
             "0 keeps the game's own camera. Above 0, the camera opens over both screens while the quarterback reads and "
             "the ball's in the air, this quickly.", 0.0, 0.2, 0.005, 3, 0.0, nullptr, true},
            {"wide_in", "wide_in", "Wide View Ease Back",
             "How quickly the wide view goes back to the game's camera after a catch.", 0.005, 0.2, 0.005, 3, 0.025, nullptr, true},
            {"pread", "pread", "Wide View Angle",
             "The wide view's tilt down from level while the quarterback reads, in degrees.", 20.0, 70.0, 1.0, 0, 45.0, nullptr, true},
            {"wide", "wide", "Wide View Farthest",
             "How far back the wide view may go to keep every receiver in.", 200.0, 500.0, 5.0, 0, 320.0, nullptr, true},
            // On-screen text and panels (the game's 2D; up is higher on the screen)
            {"score", "score", "Scoreboard Screen", "Which screen the scoreboard is on.", 0, 1, 1, 0, 0.0, screen_choices},
            {"board", "board", "Scoreboard Size", "The scoreboard's size against the rest of the text and panels.",
             0.8, 2.0, 0.05, 2, 1.35},
            {"banner", "banner", "Banners Lower",
             "How far down its screen the upper screen's banners and panels move, as a share of the screen.", 0.0, 0.4, 0.01, 2, 0.12},
            {"panel_low", "panel_low", "Down & Distance Screen",
             "Which screen 1ST AND 10 goes on between plays.", 0, 1, 1, 0, 0.0, screen_choices},
            {"panel", "panel", "Down & Distance Height",
             "1ST AND 10's height at the bottom of the upper screen. Higher is up.", -80.0, 40.0, 1.0, 0, -29.0},
            {"panel_low_y", "panel_low_y", "Down & Distance Height (Lower)",
             "1ST AND 10's height when it's on the lower screen. Higher is up.", -80.0, 40.0, 1.0, 0, -28.0},
            {"panel_words", "panel_words", "Down & Distance Words",
             "Moves 1ST AND 10's words up or down against their plate.", -20.0, 20.0, 1.0, 0, 0.0},
            {"score_lift", "score_lift", "Score Panel Height",
             "The score panel after a score, at the bottom of the upper screen. Higher is up, clear of the hinge.", -40.0, 60.0, 1.0, 0, 8.0},
            {"msg_up", "msg_up", "Big Message Height",
             "TOUCHDOWN!, 1ST DOWN and the other big words. Higher is up.", -60.0, 60.0, 1.0, 0, 10.0},
            {"card", "card", "Rules Card Height",
             "The rules and tip cards in the middle of the upper screen. Higher is up.", -80.0, 80.0, 1.0, 0, 0.0},
            {"result_dx", "result_dx", "Play Result Sideways",
             "A play's result (16 YARD PASS) at the top of the upper screen: right is positive.", -200.0, 200.0, 2.0, 0, 0.0},
            {"pause_up", "pause_up", "Pause Menu Height", "The pause menu. Higher is up.", -60.0, 60.0, 1.0, 0, -3.0},
            {"meter", "meter", "Kick Meter",
             "Where the kick and punt meters go: the upper screen's lower left corner, with the rest of the text, or the "
             "lower screen's lower left corner.", 0, 2, 1, 0, 0.0, meter_choices},
        };
        return all;
    }

    inline const Dial* find(const std::string& id) {
        for (const Dial& d : dials()) {
            if (id == d.id) return &d;
        }
        return nullptr;
    }

    // The presets: General's Field Ahead (ds_tilt) and Camera Zoom Out (ds_zoom) choices, as saved since 1.0.
    inline double preset_tilt(uint32_t choice) { return 0.40 + 0.05 * (double)std::min<uint32_t>(choice, 5u); }
    inline double preset_zoom_percent(uint32_t choice) {
        static const double pct[] = {0.0, 5.0, 10.0, 12.5, 15.0, 20.0, 25.0, 30.0};
        return pct[std::min<uint32_t>(choice, 7u)];
    }

    // tall_view's value for a dial's row value.
    inline float to_tall(const Dial& d, double v) {
        if (std::string(d.id) == zoom_id) return (float)(1.0 + v / 100.0);
        return (float)v;
    }

    // What each dial is now, as tall_view's (name, value) pairs. `custom` is the player's saved values by dial id (any
    // missing one is its built-in value).
    inline std::vector<std::pair<std::string, float>> resolve(bool custom_on, uint32_t tilt_choice, uint32_t zoom_choice,
                                                              const std::map<std::string, double>& custom) {
        std::vector<std::pair<std::string, float>> out;
        for (const Dial& d : dials()) {
            double v = d.builtin;
            if (custom_on) {
                auto it = custom.find(d.id);
                if (it != custom.end()) v = std::clamp(it->second, d.min, d.max);
            }
            else if (std::string(d.id) == tilt_id) v = preset_tilt(tilt_choice);
            else if (std::string(d.id) == zoom_id) v = preset_zoom_percent(zoom_choice);
            out.emplace_back(d.tall, to_tall(d, v));
        }
        return out;
    }

    // Custom Layout turned on (or reset): the camera dials start from the presets in use, the rest from built-in.
    inline std::map<std::string, double> seed_from_presets(uint32_t tilt_choice, uint32_t zoom_choice) {
        std::map<std::string, double> out;
        for (const Dial& d : dials()) out[d.id] = d.builtin;
        out[tilt_id] = preset_tilt(tilt_choice);
        out[zoom_id] = preset_zoom_percent(zoom_choice);
        return out;
    }
}
