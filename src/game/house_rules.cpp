// Settings → Rules: Blitz's VS-screen codes as standing house rules (both apps).
//
// Each code is a 32-bit flag word in main bss (bit n = player slot n; team A = slots 0-1, team B =
// slots 2-3, so 0xF = both teams). The VS screen's code process (main func_8024B784) clears them all
// and then reads the players' Turbo/Jump/Pass + direction entries. A hook right after the clear
// (0x8024B880, blitz2000.toml) sets the chosen rules there, so the game's own pipeline treats them as
// entered: its agreement pass, the weather/stadium resolution and every gameplay reader apply as
// usual, and players can still enter more codes. Like the game, the codes it refuses in Season
// (gameplay-changing ones) are not applied there. Addresses are the same in vanilla and team+.
#include "../main/platform_compat.h"
#include "../main/os_shell.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "json/json.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "recomp.h"
#if BLITZ_MODERN
#include "team_store.h"
#endif
#include "recompui/config.h"
#include "util/file.h"

namespace {
    constexpr uint32_t game_mode = 0x8028B998;  // 2 = Season
    constexpr const char* tab = "rules";

    // Home field: the right-hand team at the VS screen is the home team (its end zone is painted in the
    // stadium; the season builder lists home second). Found by its scoreboard abbreviation, so packs and
    // community ROMs work as long as they use the usual abbreviations.
#if BLITZ_TEAMPLUS
    constexpr uint32_t home_abbr = 0x802E94AC;
#else
    constexpr uint32_t home_abbr = 0x802E94BC;
#endif
    constexpr uint32_t field_grass = 0x80282F60, field_turf = 0x80282F64, weather_clear = 0x80282F30;
    struct Venue {
        const char* abbr;
        bool turf;   // artificial turf (else grass)
        bool roof;   // a dome or a roof that's usually closed: no rain or snow
    };
#if BLITZ_MODERN
    // Today's stadiums (2026 season).
    constexpr Venue venues[] = {
        {"ARI", false, true},  {"ATL", true, true},   {"BAL", false, false}, {"BUF", false, false}, {"CAR", true, false},
        {"CHI", false, false}, {"CIN", true, false},  {"CLE", false, false}, {"DAL", true, true},   {"DEN", false, false},
        {"DET", true, true},   {"GB", false, false},  {"HOU", true, true},   {"IND", true, true},   {"JAX", false, false},
        {"KC", false, false},  {"LV", false, true},   {"LAC", true, true},   {"LAR", true, true},   {"MIA", false, false},
        {"MIN", true, true},   {"NE", true, false},   {"NO", true, true},    {"NYG", true, false},  {"NYJ", true, false},
        {"PHI", false, false}, {"PIT", false, false}, {"SF", false, false},  {"SEA", true, false},  {"TB", false, false},
        {"TEN", true, false},  {"WAS", false, false}, {"WSH", false, false},
    };
#else
    // The 1999 season's stadiums, for Classic's teams.
    constexpr Venue venues[] = {
        {"ARZ", false, false}, {"ATL", true, true},   {"BAL", false, false}, {"BUF", true, false},  {"CAR", false, false},
        {"CHI", false, false}, {"CIN", true, false},  {"CLE", false, false}, {"DAL", true, false},  {"DEN", false, false},
        {"DET", true, true},   {"GB", false, false},  {"IND", true, true},   {"JAC", false, false}, {"KC", false, false},
        {"MIA", false, false}, {"MIN", true, true},   {"NE", false, false},  {"NO", true, true},    {"NYG", true, false},
        {"NYJ", true, false},  {"OAK", false, false}, {"PHI", true, false},  {"PIT", true, false},  {"SD", false, false},
        {"SF", false, false},  {"SEA", true, true},   {"STL", true, true},   {"TB", false, false},  {"TEN", false, false},
        {"WAS", false, false},
    };
#endif

    // The home team's scoreboard abbreviation. At the VS screen the scoreboard copy isn't written yet for a
    // new game, so it's read from the home team's own record (ovl_b, still loaded) by its chosen slot.
    uint32_t home_abbr_addr(uint8_t* rdram) {
        int32_t team = MEM_W(4, (gpr)(int32_t)0x802EA434);  // chosen[1]
#if !BLITZ_MODERN  // Modern reads the team store's live calls (home_venue)
        if (team >= 0 && team < 31) return 0x8033800C + (uint32_t)team * 0x64 + 0x50;    // team table +0x50 (content_pack.cpp)
#endif
        return home_abbr;
    }

    // The home team's scoreboard code as the game has it now.
    std::string home_abbr_text(uint8_t* rdram) {
        std::string raw;
#if BLITZ_MODERN
        int32_t team = MEM_W(4, (gpr)(int32_t)0x802EA434);  // chosen[1]
        if (team >= 0 && team < 60) raw = blitz::teamdata::live::abbr(rdram, team);
        else
#endif
        {
            const uint32_t at = home_abbr_addr(rdram);
            for (int i = 0; i < 4; i++) {
                char ch = (char)MEM_B(i, (gpr)(int32_t)at);
                if (ch == 0) break;
                raw += ch;
            }
        }
        return raw;
    }

    const Venue* home_venue(uint8_t* rdram) {
        std::string abbr;
        for (char ch : home_abbr_text(rdram))
            if (ch != '.' && ch != ' ') abbr += (char)std::toupper((unsigned char)ch);
        if (abbr == "ARI" || abbr == "ARZ") abbr = venues[0].abbr;  // either spelling
        for (const Venue& v : venues) {
            if (abbr == v.abbr) return &v;
        }
        return nullptr;
    }

    struct Toggle {
        const char* id;
        const char* name;
        const char* help;
        uint32_t flag;
        bool not_in_season;
    };
    const std::vector<Toggle> toggles = {
        {"tournament", "Tournament Mode", "No codes or catch-up help in a game between two humans (the game turns the fun codes off at kickoff). Only for human vs human.", 0x80282EB8, true},
        {"no_cpu_assist", "No CPU Assistance", "Turns off the game's catch-up help for the team that's behind: the better catch and interception odds and field goal luck in every game, and the skill boost in a game between two humans. In a 1-player game the CPU's skill still follows the score.", 0x80282EA8, true},
        {"no_fumbles", "No Random Fumbles", "", 0x80282EAC, true},
        {"infinite_turbo", "Infinite Turbo", "", 0x80282EE4, true},
        {"fast_passes", "Fast Passes", "", 0x80282F10, true},
        {"fast_turbo", "Fast Turbo Running", "", 0x80282F14, true},
        {"no_punting", "No Punting", "", 0x80282EE8, true},
        {"no_first_downs", "No First Downs", "", 0x80282EF0, true},
        {"no_interceptions", "No Interceptions", "", 0x80282EEC, true},
        {"late_hits", "Late Hits", "", 0x80282EFC, false},
        {"super_fg", "Super Field Goals", "", 0x80282F28, true},
        {"show_fg", "Show Field Goal %", "", 0x80282EB4, false},
        {"more_field", "Show More Field", "A wider camera.", 0x80282E74, false},
        {"hide_names", "Hide Receiver Names", "", 0x80282E8C, false},
        {"tiny_players", "Tiny Players", "", 0x80282E84, false},
        {"big_football", "Big Football", "", 0x80282E88, false},
        {"night", "Night Game", "", 0x80282E9C, false},
    };

    struct Choice {
        const char* id;
        const char* name;
        const char* help;
        std::vector<std::pair<const char*, uint32_t>> options;  // label, flag word (0 = none)
    };
    const std::vector<Choice> choices = {
        {"heads", "Heads", "", {{"Normal", 0}, {"Big", 0x80282E78}, {"Huge", 0x80282EF4}, {"Headless", 0x80282EA0}}},
        {"weather", "Weather", "\"Game decides\" keeps the game's own random weather.",
         {{"Game decides", 0}, {"Clear", 0x80282F30}, {"Rain", 0x80282E90}, {"Snow", 0x80282ECC}}},
        {"fog", "Fog", "", {{"Game decides", 0}, {"Fog", 0x80282EA4}, {"Thick fog", 0x80282EBC}, {"Lights out", 0x80282ED0}}},
        {"stadium", "Stadium", "",
         {{"Game decides", 0}, {"Day", 0x80282F38}, {"Night", 0x80282F3C}, {"Snow", 0x80282F40}, {"Old day", 0x80282F44},
          {"Old night", 0x80282F48}, {"Old snow", 0x80282F4C}, {"City", 0x80282F50}, {"Future", 0x80282F54},
          {"Roman", 0x80282F58}, {"None", 0x80282F08}}},
        {"field", "Field", "Some stadiums (City, Future, Roman) bring their own field.",
         {{"Game decides", 0}, {"Grass", 0x80282F60}, {"Turf", 0x80282F64}, {"Dirt", 0x80282F5C}, {"Asphalt", 0x80282F6C},
          {"Snow", 0x80282F68}}},
    };

    // Settings → Rules → Quick Setup: sets the toggles and choices below in one pick (the setup itself
    // then reads "Custom" once anything is changed by hand).
    // Shareable presets: every <app folder>/rules/<name>.json is listed too (read at start). Such a file
    // is just {"option id": value}, the same as rules.json: a rules.json someone sends you works as is.
    struct Preset {
        std::string key;
        std::string name;
        std::vector<std::string> on;           // built in: toggles on; everything else off / first choice
        std::map<std::string, nlohmann::json> values;  // from a file: option values as rules.json stores them
        bool from_file = false;
    };
    std::vector<Preset> presets = {
        {"custom", "Custom", {}, {}},
        {"original", "Original", {}, {}},
        {"party", "Party", {"infinite_turbo", "fast_turbo", "late_hits", "big_football", "show_fg"}, {}},
        {"tournament", "Tournament", {"tournament", "no_cpu_assist", "no_fumbles"}, {}},
    };

    std::filesystem::path preset_folder() { return recompui::file::get_app_folder_path() / "rules"; }

    void load_preset_files() {
        std::error_code ec;
        std::filesystem::create_directories(preset_folder(), ec);
        std::vector<std::filesystem::path> files;
        for (const auto& e : std::filesystem::directory_iterator(preset_folder(), ec)) {
            if (e.path().extension() == ".json") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& path : files) {
            try {
                std::ifstream in{path};
                nlohmann::json j = nlohmann::json::parse(in);
                if (!j.is_object()) continue;
                Preset p;
                p.name = path.stem().string();
                p.key = "file:" + p.name;
                p.from_file = true;
                for (auto& [k, v] : j.items()) {
                    if (k != "preset") p.values[k] = v;
                }
                presets.push_back(std::move(p));
            }
            catch (...) {
                fprintf(stderr, "[rules] %s isn't a rules file; skipped\n", path.string().c_str());
            }
        }
    }
    bool applying_preset = false;

    recomp::config::Config& config() {
        return recompui::config::get_config(tab);  // look it up each time; configs live in a growing list
    }

    // Realism (build/analysis/rules_realism.md): NFL-style rules the game keeps as plain numbers. Each
    // option's first value is the game as it shipped. The values are fixed at the VS screen for the game
    // that follows, and not applied in Season (as the game does with its gameplay codes).
    struct Realism {
        int first_down = 30;   // yards for a first down (arcade FDYARDS)
        int touchback = 20;    // kickoff touchback spot
        int two_point = 90;    // spot of the try after a touchdown (90 = the opponent's 10)
        bool pat_miss = false; // the free extra point misses about 6% of the time
        bool one_ot = false;   // after one overtime a tied game ends tied
    };
    Realism realism;  // the current game's (set at the VS screen)

    struct RealismChoice {
        const char* id;
        const char* name;
        const char* help;
        std::vector<std::pair<const char*, int>> options;  // label, value; the first is Blitz's
    };
    const char* not_in_season_note = " Not in Season, like the game's own gameplay codes. Set before a game; it applies from the next one.";
    const std::vector<RealismChoice> realism_choices = {
        {"first_down", "First Down",
         "Yards needed for a first down. Blitz uses 30; the NFL uses 10. The down and distance, the first-down "
         "line and the rules help box all follow. The No First Downs code still wins.",
         {{"30 yards (Blitz)", 30}, {"20 yards", 20}, {"10 yards", 10}}},
        {"kick_touchback", "Kickoff Touchback",
         "Where the ball goes after a kickoff touchback (the returner downed in his own end zone). Blitz uses the "
         "20; the NFL has used the 25 and now the 35. Punts and interceptions in the end zone stay at the 20.",
         {{"The 20 (Blitz)", 20}, {"The 25", 25}, {"The 35", 35}}},
        {"two_point", "Two-Point Try",
         "Where a two-point try starts after a touchdown. Blitz uses the 10; the NFL uses the 2. The free extra "
         "point is unchanged.",
         {{"From the 10 (Blitz)", 90}, {"From the 2", 98}}},
        {"overtime", "Overtime",
         "Blitz plays up to three full overtimes before a tie. <b>One, then a tie</b>: a game still tied after one "
         "overtime ends tied, as in the NFL's regular season.",
         {{"Up to 3 (Blitz)", 3}, {"One, then a tie", 1}}},
    };
    constexpr const char* pat_miss_id = "pat_miss";

    int realism_value(const RealismChoice& ch) {
        uint32_t i = std::get<uint32_t>(config().get_option_value(ch.id));
        return ch.options[i < ch.options.size() ? i : 0].second;
    }

    void lock_realism(bool season) {
        realism = Realism{};
        if (!season) {
            for (const RealismChoice& ch : realism_choices) {
                int v = realism_value(ch);
                if (std::string(ch.id) == "first_down") realism.first_down = v;
                else if (std::string(ch.id) == "kick_touchback") realism.touchback = v;
                else if (std::string(ch.id) == "two_point") realism.two_point = v;
                else if (std::string(ch.id) == "overtime") realism.one_ot = v == 1;
            }
            realism.pat_miss = std::get<bool>(config().get_option_value(pat_miss_id));
        }
        static std::string logged;
        char line[160];
        snprintf(line, sizeof(line), "first down %d, kickoff touchback %d, two-point try at %d, extra points %s, overtime %s%s",
                 realism.first_down, realism.touchback, 100 - realism.two_point, realism.pat_miss ? "can miss" : "free",
                 realism.one_ot ? "one" : "up to 3", season ? " (Season: as shipped)" : "");
        if (logged != line) {
            logged = line;
            fprintf(stderr, "[rules] realism: %s\n", line);
        }
    }
}

namespace {
    // Each code's description, then "On now: …" (the codes that are on), so the panel is the summary.
    void refresh_code_descriptions() {
        recomp::config::Config& c = config();
        std::string on;
        for (const Toggle& t : toggles) {
            if (!std::get<bool>(c.get_option_value(t.id))) continue;
            on += (on.empty() ? "" : ", ") + std::string(t.name);
        }
        for (const Toggle& t : toggles) {
            std::string help = t.help;
            if (t.not_in_season) help += std::string(help.empty() ? "" : " ") + "Not in Season (the game doesn't allow it there).";
            if (help.empty()) help = "A Blitz code, on for every game.";
            c.update_option_description(t.id, help + "<br /><br /><b>On now:</b> " + (on.empty() ? std::string("none") : on) + ".");
        }
    }
}

namespace blitz {
    void add_cpu_rules(recomp::config::Config& c);                // cpu_smart.cpp: the CPU section
    void add_receiver_option(recomp::config::Config& c);          // receiver_targets.cpp: Receiver Buttons
#if BLITZ_MODERN
    void add_crew_option(recomp::config::Config& c);              // crew.cpp: Pick Your Crew
#endif
    void cpu_rules_vs_screen(uint8_t* rdram, bool season);

    void create_rules_tab() {
        recomp::config::Config& c = recompui::config::create_config_tab("Rules", tab, false);
        load_preset_files();
        std::vector<recomp::config::ConfigOptionEnumOption> preset_opts;
        for (uint32_t i = 0; i < presets.size(); i++) preset_opts.emplace_back(i, presets[i].key, presets[i].name);
        c.add_enum_option("preset", "Quick Setup",
                          "<b>Original</b>: every code off, the game as it shipped. <b>Party</b>: infinite and faster turbo, "
                          "late hits, a big football and field goal odds on screen. <b>Tournament</b>: Tournament Mode, no "
                          "catch-up help, no random fumbles. Change anything below afterwards and this reads Custom.<br /><br />"
                          "<b>Sharing:</b> a setup saved as a file in the app folder's <b>rules</b> folder is listed here by its "
                          "file name (at the next start). Turn on Save Setup to File below to save the current one; send that "
                          "file to a friend to give them your rules.",
                          preset_opts, 0u);
        c.add_bool_option("choose_opponent", "Choose CPU Opponent",
                          "In a 1-player Arcade game, pick the team you play against: after you choose your team, "
                          "the other side's list opens for you. It starts on the team the game would have picked, "
                          "so confirming straight away plays like the original.",
                          true);
        c.add_bool_option("uniform_picker", "Uniform Picker",
                          "At team select, press <b>△</b> (Y on Xbox; UNIFORM on a phone's touch controls) to pick your uniform for the game: <b>Home</b>, "
                          "<b>Away</b>, or back to the game's choice (it gives one team its away set when the jerseys "
                          "clash). Your team re-dresses on the spot.",
                          true);
        blitz::add_receiver_option(c);
#if BLITZ_MODERN
        c.add_enum_option("cpu_uniforms", "Unpicked Uniforms",
                          "What a team wears when nobody picks with △ (the CPU's team, Season games, or yours if you "
                          "skip it). <b>2026</b>: the team's current home and away. <b>1999</b>: the game's own. "
                          "<b>Random</b>: any of the team's sets, a new one each game (the other team switches to white "
                          "if both would wear colour).",
                          std::vector<recomp::config::ConfigOptionEnumOption>{{1, "current", "2026"}, {0, "default", "1999"}, {2, "surprise", "Random"}},
                          1u);
        blitz::add_crew_option(c);
#endif
        c.add_bool_option("save_setup", "Save Setup to File",
                          "Saves the codes below as a file in the app folder's rules folder (\"My Rules 1.json\", ...) and "
                          "opens the folder. Rename it to name the preset.", false);
        for (const Toggle& t : toggles) {
            std::string help = t.help;
            if (t.not_in_season) help += std::string(help.empty() ? "" : " ") + "Not in Season (the game doesn't allow it there).";
            c.add_bool_option(t.id, t.name, help.empty() ? "A Blitz code, on for every game." : help, false);
        }
        for (const Choice& ch : choices) {
            std::vector<recomp::config::ConfigOptionEnumOption> opts;
            for (uint32_t i = 0; i < ch.options.size(); i++) opts.emplace_back(i, ch.options[i].first, ch.options[i].first);
            c.add_enum_option(ch.id, ch.name, *ch.help ? ch.help : "Applied at every VS screen.", opts, 0u);
        }
        c.add_bool_option("home_field", "Home Field",
                          "The home team (on the right at the VS screen) plays on its real stadium's surface, grass "
                          "or turf, and a stadium with a roof never gets rain or snow. Only when Field and Weather "
                          "are on \"Game decides\".",
#if BLITZ_MODERN
                          true
#else
                          false
#endif
        );
        c.add_enum_option("play_clock", "Play Clock",
                          "How long you have to call a play when the game's Play Timer is on. <b>Normal</b>: 9 seconds "
                          "in the 1st quarter, 5 after, as it shipped. <b>15 s</b> and <b>20 s</b>: the same every quarter "
                          "(the Blitz community's Extended Playbook patch uses 20). Turn the clock off altogether with "
                          "Play Timer in the game's own Options.",
                          {{0u, "normal", "Normal"}, {1u, "15", "15 s"}, {2u, "20", "20 s"}}, 0u);
        for (const RealismChoice& ch : realism_choices) {
            if (std::string(ch.id) == "overtime") {
                c.add_bool_option(pat_miss_id, "Extra Points Can Miss",
                                  std::string("The free extra point is good about 94% of the time, like an NFL kick, "
                                              "for both teams (as shipped it only misses now and then in a blowout).") +
                                      not_in_season_note,
                                  false);
            }
            std::vector<recomp::config::ConfigOptionEnumOption> opts;
            for (uint32_t i = 0; i < ch.options.size(); i++) {
                opts.emplace_back(i, std::to_string(ch.options[i].second), ch.options[i].first);
            }
            c.add_enum_option(ch.id, ch.name, std::string(ch.help) + not_in_season_note, opts, 0u);
        }
        add_cpu_rules(c);
        c.add_option_change_callback("preset", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                  recomp::config::OptionChangeContext ctx) {
            uint32_t i = std::get<uint32_t>(cur);
            if (ctx != recomp::config::OptionChangeContext::Permanent || i == 0 || i >= presets.size()) return;
            recomp::config::Config& c = config();
            applying_preset = true;
            const Preset& p = presets[i];
            for (const Toggle& t : toggles) {
                bool on = std::find(p.on.begin(), p.on.end(), t.id) != p.on.end();
                if (p.from_file) on = p.values.count(t.id) && p.values.at(t.id).is_boolean() && p.values.at(t.id).get<bool>();
                c.update_option_value(t.id, on);  // update_: the open Rules page redraws the control
            }
            for (const Choice& ch : choices) {
                uint32_t idx = 0;
                if (p.from_file && p.values.count(ch.id) && p.values.at(ch.id).is_string()) {
                    std::string want = p.values.at(ch.id).get<std::string>();
                    for (uint32_t k = 0; k < ch.options.size(); k++) if (want == ch.options[k].first) idx = k;
                }
                c.update_option_value(ch.id, idx);
            }
            if (p.from_file && p.values.count("choose_opponent") && p.values.at("choose_opponent").is_boolean()) {
                c.update_option_value("choose_opponent", p.values.at("choose_opponent").get<bool>());
            }
            if (p.from_file && p.values.count("play_clock") && p.values.at("play_clock").is_number_unsigned()) {
                c.update_option_value("play_clock", std::min<uint32_t>(p.values.at("play_clock").get<uint32_t>(), 2u));
            }
            for (const RealismChoice& ch : realism_choices) {
                if (p.from_file && p.values.count(ch.id) && p.values.at(ch.id).is_number_unsigned()) {
                    uint32_t last = (uint32_t)ch.options.size() - 1;
                    c.update_option_value(ch.id, std::min(p.values.at(ch.id).get<uint32_t>(), last));
                }
            }
            if (p.from_file && p.values.count(pat_miss_id) && p.values.at(pat_miss_id).is_boolean()) {
                c.update_option_value(pat_miss_id, p.values.at(pat_miss_id).get<bool>());
            }
            applying_preset = false;
        });
        auto to_custom = [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant,
                            recomp::config::OptionChangeContext ctx) {
            if (ctx == recomp::config::OptionChangeContext::Permanent && !applying_preset) config().update_option_value("preset", 0u);
        };
        c.add_option_change_callback("save_setup", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                      recomp::config::OptionChangeContext ctx) {
            if (ctx != recomp::config::OptionChangeContext::Permanent || !std::get<bool>(cur)) return;
            recomp::config::Config& c = config();
            nlohmann::json j = nlohmann::json::object();
            for (const Toggle& t : toggles) j[t.id] = std::get<bool>(c.get_option_value(t.id));
            for (const Choice& ch : choices) j[ch.id] = ch.options[std::get<uint32_t>(c.get_option_value(ch.id))].first;
            j["choose_opponent"] = std::get<bool>(c.get_option_value("choose_opponent"));
            j["play_clock"] = std::get<uint32_t>(c.get_option_value("play_clock"));
            for (const RealismChoice& ch : realism_choices) j[ch.id] = std::get<uint32_t>(c.get_option_value(ch.id));
            j[pat_miss_id] = std::get<bool>(c.get_option_value(pat_miss_id));
            std::filesystem::path path;
            for (int n = 1; n < 1000; n++) {
                path = preset_folder() / ("My Rules " + std::to_string(n) + ".json");
                if (!blitz::file_exists(path)) break;
            }
            std::ofstream(path) << j.dump(4) << "\n";
            fprintf(stderr, "[rules] saved %s\n", path.string().c_str());
            c.update_option_value("save_setup", false);
            if (!getenv("BLITZ_QUIET_NOTICES")) blitz::open_in_file_browser(preset_folder());
        });
        for (const Toggle& t : toggles) c.add_option_change_callback(t.id, to_custom);
        for (const Choice& ch : choices) c.add_option_change_callback(ch.id, to_custom);

        // Blitz '27 layout (docs/design/redesign/REVIEW.md S5): SETUP, CODES with a live count, LOOK & FEEL;
        // Save Setup to File is a SAVE button; each code's description ends with which codes are on now.
        recompui::config::set_option_section(tab, "preset", "SETUP");
        recompui::config::set_option_section(tab, toggles.front().id, "CODES \xC2\xB7 ON FOR EVERY GAME", []() {
            int on = 0;
            for (const Toggle& t : toggles) on += std::get<bool>(config().get_option_value(t.id)) ? 1 : 0;
            return on == 0 ? std::string("NONE ON") : std::to_string(on) + " ON";
        });
        recompui::config::set_option_section(tab, choices.front().id, "LOOK & FEEL");
        recompui::config::set_option_section(tab, realism_choices.front().id, "REALISM");
        recompui::config::set_option_action(tab, "save_setup", "Save");
        refresh_code_descriptions();
        for (const Toggle& t : toggles) {
            c.add_option_change_callback(t.id, [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant,
                                                  recomp::config::OptionChangeContext) { refresh_code_descriptions(); });
        }
    }
}

// main 0x8024B880: the VS screen's code process has just cleared every code.
extern "C" void blitz_house_rules(uint8_t* rdram) {
    bool season = MEM_W(0, (gpr)(int32_t)game_mode) == 2;
    auto set = [&](uint32_t flag) { MEM_W(0, (gpr)(int32_t)flag) = 0xF; };  // both teams
    recomp::config::Config& c = config();
    lock_realism(season);
    for (const Toggle& t : toggles) {
        if (std::get<bool>(c.get_option_value(t.id)) && !(season && t.not_in_season)) set(t.flag);
    }
    for (const Choice& ch : choices) {
        uint32_t i = std::get<uint32_t>(c.get_option_value(ch.id));
        if (i < ch.options.size() && ch.options[i].second != 0) set(ch.options[i].second);
    }
    blitz::cpu_rules_vs_screen(rdram, season);
    if (std::get<bool>(c.get_option_value("home_field"))) {
        const Venue* v = home_venue(rdram);
        bool field_open = std::get<uint32_t>(c.get_option_value("field")) == 0;
        bool weather_open = std::get<uint32_t>(c.get_option_value("weather")) == 0;
        if (v != nullptr) {
            if (field_open) set(v->turf ? field_turf : field_grass);
            if (weather_open && v->roof) set(weather_clear);
        }
        static std::string logged;
        std::string raw;
        for (char ch : home_abbr_text(rdram)) {
            raw += std::isprint((unsigned char)ch) ? ch : '?';
        }
        std::string line = v ? std::string(v->abbr) + (v->turf ? " turf" : " grass") + (v->roof ? ", roof" : "") : "unknown team (\"" + raw + "\")";
        if (line != logged) {
            logged = line;
            fprintf(stderr, "[rules] home field: %s\n", line.c_str());
        }
    }
}

namespace blitz {
    // Export ROM (export_rom.cpp): the current Rules as a cart can carry them.
    struct ExportRules {
        std::vector<uint32_t> flags;       // codes on in any game (the choices: heads, weather, stadium ...)
        std::vector<uint32_t> flags_play;  // gameplay codes the game refuses in Season
        bool home_field = false, field_open = false, weather_open = false;
        int play_clock = 0;                // 0: the game's (9, then 5)
        int first_down = 30, touchback = 20, two_point = 90;
        bool pat_miss = false, one_ot = false;
    };
    // A stadium for a scoreboard abbreviation: bit 0 known, 1 turf, 2 roof.
    int export_venue(const std::string& abbr_in) {
        std::string abbr;
        for (char ch : abbr_in) if (ch != '.' && ch != ' ' && ch) abbr += (char)std::toupper((unsigned char)ch);
        if (abbr == "ARI" || abbr == "ARZ") abbr = venues[0].abbr;
        for (const Venue& v : venues) if (abbr == v.abbr) return 1 | (v.turf ? 2 : 0) | (v.roof ? 4 : 0);
        return 0;
    }
    ExportRules export_rules() {
        ExportRules e;
        recomp::config::Config& c = config();
        for (const Toggle& t : toggles)
            if (std::get<bool>(c.get_option_value(t.id))) (t.not_in_season ? e.flags_play : e.flags).push_back(t.flag);
        for (const Choice& ch : choices) {
            uint32_t i = std::get<uint32_t>(c.get_option_value(ch.id));
            if (i < ch.options.size() && ch.options[i].second != 0) e.flags.push_back(ch.options[i].second);
        }
        e.home_field = std::get<bool>(c.get_option_value("home_field"));
        e.field_open = std::get<uint32_t>(c.get_option_value("field")) == 0;
        e.weather_open = std::get<uint32_t>(c.get_option_value("weather")) == 0;
        switch (std::get<uint32_t>(c.get_option_value("play_clock"))) {
            case 1: e.play_clock = 15; break;
            case 2: e.play_clock = 20; break;
            default: break;
        }
        for (const RealismChoice& ch : realism_choices) {
            int v = realism_value(ch);
            if (std::string(ch.id) == "first_down") e.first_down = v;
            else if (std::string(ch.id) == "kick_touchback") e.touchback = v;
            else if (std::string(ch.id) == "two_point") e.two_point = v;
            else if (std::string(ch.id) == "overtime") e.one_ot = v == 1;
        }
        e.pat_miss = std::get<bool>(c.get_option_value(pat_miss_id));
        return e;
    }
}

// ovl_a 0x8032C4C0, play select: a3 is the play clock's start in seconds (9 in the 1st quarter, 5 after),
// just before the "timer" process is created with it. The digits are drawn from the same count.
extern "C" int32_t blitz_play_clock_seconds(uint8_t* rdram, int32_t game_seconds) {
    (void)rdram;
    switch (std::get<uint32_t>(config().get_option_value("play_clock"))) {
        case 1: return 15;
        case 2: return 20;
        default: return game_seconds;
    }
}

// ---- Realism (build/analysis/rules_realism.md). Same addresses in vanilla and team+. ----
namespace {
    constexpr uint32_t gs_phase = 0x802EA420, gs_los = 0x802EA48C, gs_down = 0x802EA498, gs_first_down = 0x802EA49C;
    constexpr uint32_t gs_play_type = 0x802EA508, gs_last_play_type = 0x802EA50C, gs_play_result = 0x802EA514;
    constexpr uint32_t code_no_first_downs = 0x80282EF0;
    constexpr uint32_t help_yards = 0x8033809C;  // ovl_a data: "30 YARDS " in the RULES help box
    constexpr uint32_t pat_label = 0x80337E04;   // ovl_a data: "FREE", the extra point's play-select label (both apps)
    int32_t& gw(uint8_t* rdram, uint32_t a) { return MEM_W(0, (gpr)(int32_t)a); }
}

// ovl_a 0x80301E0C, where every play-outcome path of the game loop joins (before plays.log): the game
// has just set down, spot, first-down marker, possession and the next play's kind. Changing them here
// is what the next play select, the HUD and the lineup read (the on-field lines are computed right after).
extern "C" void blitz_rules_play_ended(uint8_t* rdram) {
    if (gw(rdram, gs_phase) != 2) return;  // the attract demo runs the same loop
    const Realism& r = realism;
    bool no_first_downs = gw(rdram, code_no_first_downs) != 0;
    int kind = gw(rdram, gs_play_type);
    // Touchback (result 15) after a kickoff (last play kind 1): the game put it at the 20, first down at 50.
    if (r.touchback != 20 && gw(rdram, gs_play_result) == 15 && gw(rdram, gs_last_play_type) == 1) {
        gw(rdram, gs_los) = r.touchback;
        gw(rdram, gs_first_down) = no_first_downs ? 100 : std::min(r.touchback + 30, 100);
    }
    // Try after a touchdown (kind 4): the game spots it at 90 (the opponent's 10).
    if (r.two_point != 90 && kind == 4) gw(rdram, gs_los) = r.two_point;
    // Every result that starts a series sets down 1 and first down = spot + 30 (or 100 for goal to go).
    if (r.first_down != 30) {
        if (kind == 0 && gw(rdram, gs_down) == 1 && !no_first_downs) {
            gw(rdram, gs_first_down) = std::min<int32_t>(gw(rdram, gs_los) + r.first_down, 100);
        }
        MEM_B(0, (gpr)(int32_t)help_yards) = (int8_t)('0' + r.first_down / 10);  // same length: "10 YARDS "
    }
    // The extra point's label at play select is "FREE" (ovl_a data, like "3 PTS" for a field goal and "PUNT").
    // With Extra Points Can Miss it isn't free (team): "1 PT", the same length.
    {
        const char* label = r.pat_miss ? "1 PT" : "FREE";
        for (int i = 0; i < 4; i++) MEM_B(i, (gpr)(int32_t)pat_label) = (int8_t)label[i];
    }
}

// ovl_a 0x8032CF10: the free extra point's "KICK WAS GOOD!" path (the game's own rare miss jumps to
// 0x8032CEEC instead). Nonzero sends it down the miss path: "KICK WAS NO GOOD!" and no point.
// BLITZ_TEST_PAT_MISS=1 makes every kick miss while the option is on (for tests).
extern "C" int blitz_pat_missed(uint8_t* rdram) {
    (void)rdram;
    if (!realism.pat_miss) return 0;
    static const bool always = getenv("BLITZ_TEST_PAT_MISS") != nullptr;
    static std::mt19937 rng{std::random_device{}()};
    bool miss = always || std::uniform_int_distribution<int>(0, 99)(rng) < 6;
    if (miss) fprintf(stderr, "[rules] extra point no good\n");
    return miss ? 1 : 0;
}

// ovl_a 0x80302128, end of the 4th quarter or an overtime with nobody ahead: the game plays another
// overtime unless this was the 3rd (quarter word 6). Nonzero ends the game as a tie now.
extern "C" int blitz_rules_end_tied_game(uint8_t* rdram, int32_t quarter) {
    (void)rdram;
    return realism.one_ot && quarter >= 4 ? 1 : 0;
}
