// League selection's prompts (1.0.6, branch league-select; docs/SETTINGS_LAYOUT.md "League"). Every one is our own modal
// (recompui's prompt: controller, touch and keyboard, on the game's screen in two-screen modes), shown from the main
// thread's tick (league_select_update, main.cpp), one at a time, and never over a running play:
//
//   - League browses before it chooses: < > (the D-pad, the arrow keys, a tap on an arrow) only show another league,
//     dimmed with "Press × to choose"; ×, Enter or a tap on the name chooses it. Leaving the row, the tab or Settings with
//     one browsed asks "CFL 2026 isn't chosen yet." Restart to Switch / Keep NFL 2026.
//   - "Switch to CFL 2026?" a setting the game reads only as it starts (Game, League, Rosters, Season Mode) changed while
//     the game runs. Restart is the app's own (in place on a phone, a relaunch on a computer). League's second button is
//     Later (kept for the next start, the Restart button's dot lit); the others' is Cancel, which puts the setting back. Lineups needs none: each game reads it. While something waits for a restart anyway (a
//     device that can't restart itself, a pack removed under the game), the Settings header's Restart shows a dot and its
//     prompt names what waits.
//   - "Play CFL 2026 now?" Yes / Not Now: a league pak just went in (Add-Ons' Add…, Check, the automatic downloads).
//     Shown on the launcher or over Settings, never over a game; queued until then.
//   - "CFL 2026 plays Arcade and Tournament": the main menu with a league pack (it used to be a system dialog), with a
//     button that switches League back to NFL 2026 (then the restart prompt above).
//   - "Standard or Advanced season?": when a new season begins (the game's season record reset). It starts from the
//     running Season Mode and saves the answer to it; the other mode needs a restart, since a season's saves and team
//     order follow the mode, so the game restarts for it and the next new season isn't asked again. Not asked mid-season:
//     the reset only happens for a new season.
//
// Why a restart and not live: a league pack replaces the team blocks, registers its own files and changes the menu's modes
// and the saves folder as the game boots (content_pack_tp.cpp, game_init.cpp); doing that between two games would need
// all of it undone and redone in a running game. A restart in place on a phone takes about as long as a menu.
#if BLITZ_MODERN
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "blitz_config.h"
#include "blitz_soft_reset.h"
#include "season_pak.h"
#include "librecomp/game.hpp"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "ultramodern/ultramodern.hpp"

namespace blitz {
    // season_settings.cpp
    extern std::function<void(const char*, recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant)> season_change_hook;
    extern std::function<void(const std::string&, const std::string&)> league_menu_hook;
    extern std::function<void()> new_season_hook;
    extern std::function<void(const std::string&, const std::string&)> league_notice_hook;
    std::string league_title(const std::string& id);
    std::string league_id();
    std::string league_check_installed(bool now);
    bool original_game();            // adv_mode.cpp
    bool advanced_mode();
    void original_settings_visibility();
    bool league_active();            // content_pack_tp.cpp
    struct SeasonData;
    const SeasonData* pack_season();
    void request_restart();          // main.cpp
    std::string app_restart_setting();  // main.cpp: a setting read as the app launches changed ("" none)
    void post_status(const std::string& msg);  // notices.cpp
}

namespace {
    namespace fs = std::filesystem;
    using Value = recomp::config::ConfigValueVariant;

    recomp::config::Config& season() { return recompui::config::get_config("season"); }

    // The settings the game reads as it starts, with their names in the restart prompt.
    struct Watched {
        const char* id;
        const char* name;
    };
    constexpr Watched watched[] = {{"game", "Game"}, {"league", "League"}, {"rosters", "Rosters"}, {"season_mode", "Season Mode"}};

    std::mutex mutex;                    // the queue and the snapshot
    std::map<std::string, Value> started;  // each watched setting as the running game read it
    bool snapped = false;
    bool resnap = false;
    bool reverting = false;              // a Cancel or a fallback puts a value back: no prompt for that
    int seen_generation = -1;
    std::string fallback_toast;          // League fell back to NFL 2026 (its pack is gone): told once the game is up

    // A choice's name ("Original 1999", "CFL 2026", "Advanced").
    std::string value_name(const char* id, const Value& v) {
        if (std::holds_alternative<std::string>(v)) return blitz::league_title(std::get<std::string>(v));
        if (!std::holds_alternative<uint32_t>(v)) return "?";
        recomp::config::Config& c = season();
        const auto& by_id = c.get_config_schema().options_by_id;
        auto it = by_id.find(id);
        if (it == by_id.end()) return "?";
        const auto& e = c.get_option_config<recomp::config::ConfigOptionEnum>(it->second);
        auto found = e.find_option_from_value(std::get<uint32_t>(v));
        return found == e.options.end() ? "?" : found->name;
    }

    void snapshot() {
        std::lock_guard lock{mutex};
        started.clear();
        for (const Watched& w : watched) started[w.id] = season().get_option_value(w.id);
        snapped = true;
        resnap = false;
    }

    // Restart in place: the next boot reads the settings again.
    [[maybe_unused]] const bool league_soft_reset = blitz::on_soft_reset("league select", [] {
        std::lock_guard lock{mutex};
        resnap = true;
    });

    // What differs from what the running game read: "League: CFL 2026, Game: Original 1999" ("" nothing). skip: one id
    // left out (the one a prompt is about).
    std::string differing(const char* skip = nullptr) {
        if (!ultramodern::is_game_started()) return {};
        std::map<std::string, Value> was;
        {
            std::lock_guard lock{mutex};
            if (!snapped || resnap) return {};
            was = started;
        }
        std::string out;
        for (const Watched& w : watched) {
            if (skip != nullptr && std::string(skip) == w.id) continue;
            Value now = season().get_option_value(w.id);
            if (now == was[w.id]) continue;
            out += (out.empty() ? "" : ", ") + std::string(w.name) + ": " + value_name(w.id, now);
        }
        return out;
    }

    // ---- The queue -------------------------------------------------------------------------------------------------

    struct Ask {
        std::string key;  // one of each at a time: a newer one replaces a waiting one
        std::string header, body, yes, no;
        std::function<void()> on_yes, on_no;
        bool needs_quiet = false;  // only on the launcher or over Settings (not over a game)
        recompui::ButtonStyle yes_style = recompui::ButtonStyle::Primary;
        bool focus_no = true;
        bool single = false;  // one button (yes): a notice
        std::function<bool()> still_wanted;  // asked just before it shows
    };
    std::deque<Ask> queue;
    // The one of ours that's up: B (Back) closes a prompt without either button, which counts as its second one here
    // (Cancel, Not Now, OK, the running Season Mode), so a Back never leaves a change half made.
    bool showing = false, answered = false;
    std::function<void()> showing_no;
    // What's up, and whether the game had started when it opened: the launcher giving way to the game (Skip Launcher)
    // closes a prompt nobody answered, and that one is asked again rather than taken as its second button.
    std::optional<Ask> shown_ask;
    bool shown_in_game = false;

    void enqueue(Ask a) {
        std::lock_guard lock{mutex};
        for (auto it = queue.begin(); it != queue.end();) {
            if (it->key == a.key) it = queue.erase(it);
            else ++it;
        }
        fprintf(stderr, "[league] prompt queued: %s\n", a.header.c_str());
        queue.push_back(std::move(a));
    }

    bool settings_shown() {
        try {
            recompui::ContextId s = recompui::config::get_config_context_id();
            return s != recompui::ContextId::null() && recompui::is_context_shown(s);
        }
        catch (...) {
            return false;
        }
    }

    // Settings has been up a moment: a prompt opened in the frame Settings opens is drawn under it.
    bool settings_settled() {
        static std::chrono::steady_clock::time_point since{};
        static bool was = false;
        bool now = settings_shown();
        if (now && !was) since = std::chrono::steady_clock::now();
        was = now;
        return now && std::chrono::steady_clock::now() - since > std::chrono::milliseconds(500);
    }

    bool launcher_shown() {
        recompui::ContextId l = recompui::get_launcher_context_id();
        return l != recompui::ContextId::null() && recompui::is_context_shown(l);
    }

    // ---- "Switch to CFL 2026?" ---------------------------------------------------------------------------------------

    void put_back(const std::string& id, const Value& v) {
        reverting = true;
        season().update_option_value(id, v);
        season().save_config();
        reverting = false;
        blitz::original_settings_visibility();
    }

    void ask_restart(const char* id, const Value& cur, const Value& prev) {
        std::string what = value_name(id, cur);
        std::string key = id;
        std::string header, loads;
        if (key == "league") header = "Switch to " + what + "?", loads = "its teams";
        else if (key == "rosters") header = "Switch Rosters to " + what + "?", loads = "them";
        else if (key == "season_mode") header = std::string("Switch to ") + (what == "Advanced" ? "an Advanced" : "a Standard") + " season?", loads = "it";
        else header = "Switch to " + what + "?", loads = "it";
        std::string body;
        std::string others = differing(id);
        if (blitz::restart_available) {
            body = "New Blitz restarts to load " + loads + ". Anything not saved to the Controller Pak is lost.";
            if (key == "season_mode") body = "New Blitz restarts to load it: then start your new season from Season. Each mode keeps its own saves.";
        }
        else {
            body = "It takes effect the next time New Blitz starts.";
        }
        if (!others.empty()) body += "\n\nAlso waiting for the restart: " + others + ".";
        Ask a;
        a.key = "restart";
        a.header = header;
        a.body = body;
        a.yes = blitz::restart_available ? "Restart" : "OK";
        a.no = "Cancel";
        // League is chosen on purpose (browse, then ×): Later keeps it for the next start, with the Restart button's dot.
        const bool later = key == "league";
        if (later) {
            a.yes = blitz::restart_available ? "Restart Now" : "OK";
            a.no = "Later";
            if (!blitz::restart_available) a.no = "Cancel";
        }
        a.yes_style = blitz::restart_available ? recompui::ButtonStyle::Warning : recompui::ButtonStyle::Primary;
        a.focus_no = false;
        std::string id_s = id;
        a.on_yes = [] {
            if (blitz::restart_available) {
                fprintf(stderr, "[league] restart for %s\n", differing().c_str());
                blitz::request_restart();
            }
        };
        a.on_no = [id_s, prev, later] {
            if (later && blitz::restart_available) {
                fprintf(stderr, "[league] %s: Later (at the next start)\n", id_s.c_str());
                return;
            }
            fprintf(stderr, "[league] %s put back\n", id_s.c_str());
            if (id_s == "season_mode") season().update_option_value("season_ask_skip", false);
            put_back(id_s, prev);
        };
        // A change undone before it showed (cycled back): nothing to ask.
        a.still_wanted = [id_s] {
            std::lock_guard lock{mutex};
            return snapped && season().get_option_value(id_s) != started[id_s];
        };
        enqueue(std::move(a));
    }

    // ---- League browsed but not chosen ("CFL 2026 isn't chosen yet.") ------------------------------------------------

    std::optional<std::string> league_browse() { return recompui::config::get_option_browse("season", "league"); }

    void open_now(Ask a);  // below

    // The question, with what each answer does. modal_close: Settings was closing (it closes after the answer).
    Ask browse_question(bool modal_close) {
        std::string to = league_browse().value_or(""), now = blitz::league_id();
        std::string to_name = blitz::league_title(to), now_name = blitz::league_title(now);
        bool game = ultramodern::is_game_started();
        Ask a;
        a.key = "browse";
        a.header = to_name + " isn't chosen yet.";
        a.body = "League still says " + now_name + ". " +
                 (game && blitz::restart_available ? "Restart to Switch chooses " + to_name + " and restarts New Blitz to load its teams."
                                                   : "Choose it, or keep " + now_name + ".");
        a.yes = game && blitz::restart_available ? "Restart to Switch" : "Choose " + to_name;
        a.no = "Keep " + now_name;
        a.focus_no = true;
        a.on_yes = [to, game, modal_close] {
            recompui::config::set_option_browse("season", "league", std::nullopt);
            fprintf(stderr, "[league] browse chosen: %s\n", to.c_str());
            reverting = true;  // this answer is the restart question's too: no "Switch to ...?" after it
            season().update_option_value("league", to);
            season().save_config();
            reverting = false;
            blitz::original_settings_visibility();
            if (game && blitz::restart_available) blitz::request_restart();
            else if (modal_close) recompui::config::close();
        };
        a.on_no = [modal_close] {
            recompui::config::set_option_browse("season", "league", std::nullopt);
            fprintf(stderr, "[league] browse dropped: League stays %s\n", blitz::league_id().c_str());
            if (modal_close) recompui::config::close();
        };
        a.still_wanted = [] { return league_browse().has_value() && settings_shown(); };
        return a;
    }

    // Focus left the League row with a browse (moving off it): asked once the focus has settled.
    void on_browse_left(const std::string&, const std::string&) {
        if (recompui::is_prompt_open()) return;  // our own question (or the restart one) took the focus
        enqueue(browse_question(false));
    }

    // Settings closing or changing tab with a browse: asked now, and it stays open until the answer.
    bool close_guard(bool modal_close) {
        if (!league_browse()) return true;
        if (!recompui::is_prompt_open()) open_now(browse_question(modal_close));
        return false;
    }

    void on_season_change(const char* id, Value cur, Value prev) {
        if (reverting || !ultramodern::is_game_started()) return;  // before the game: read as it starts
        {
            std::lock_guard lock{mutex};
            if (!snapped || resnap) return;
            if (started[id] == cur) return;  // back to what the game runs
        }
        ask_restart(id, cur, prev);
    }

    // ---- The main menu with a league pack ---------------------------------------------------------------------------

    void on_league_menu(const std::string& title, const std::string& modes) {
        Ask a;
        a.key = "league_menu";
        a.header = title;
        // BRANDING: the NFL season's name in the league pack's main menu note
        a.body = title + " plays " + modes + ". To play a Season, switch League to NFL 2026 in Settings → Teams.";
        a.yes = "Switch to NFL 2026";
        a.no = "OK";
        a.on_yes = [] {
            season().update_option_value("league", std::string{});  // then "Switch to NFL 2026?" Restart / Cancel
            season().save_config();
            blitz::original_settings_visibility();
        };
        a.on_no = [] {};
        enqueue(std::move(a));
    }

    // ---- The league and pak code's messages (a pack that didn't load ...): were system dialogs ------------------------

    void on_league_notice(const std::string& header, const std::string& msg) {
        Ask a;
        a.key = "notice:" + msg;
        a.header = header;
        a.body = msg;
        a.yes = "OK";
        a.single = true;
        a.on_yes = [] {};
        enqueue(std::move(a));
    }

    // ---- "Standard or Advanced season?" -----------------------------------------------------------------------------

    void on_new_season() {
        Ask a;
        a.key = "new_season";
        a.still_wanted = [] {
            if (blitz::original_game() || blitz::league_active() || blitz::pack_season() == nullptr) return false;
            if (std::get<bool>(season().get_option_value("season_ask_skip"))) {
                season().update_option_value("season_ask_skip", false);  // the season the restart was for
                season().save_config();
                fprintf(stderr, "[league] new season: chosen before the restart, not asked\n");
                return false;
            }
            return true;
        };
        // The running mode is the one a Back keeps (the prompt's Cancel); the other one is the first button.
        bool adv = blitz::advanced_mode();
        a.header = "Standard or Advanced season?";
        a.body = "Standard: the classic Blitz season, all 32 teams in the six 1999 divisions.\n\n"
                 "Advanced: the real 2026 schedule in today's eight divisions, weekly lineups that follow the real season, "
                 "NFL tiebreakers and 7-team playoffs.\n\n"
                 "Each keeps its own saves. You can change it later in Settings → Teams → Season Mode.";
        a.yes = adv ? "Standard" : "Advanced";
        a.no = adv ? "Advanced" : "Standard";
        a.focus_no = true;
        auto choose = [](bool want_adv) {
            fprintf(stderr, "[league] new season: %s\n", want_adv ? "Advanced" : "Standard");
            bool other = want_adv != blitz::advanced_mode();
            if (other) season().update_option_value("season_ask_skip", true);
            season().update_option_value("season_mode", (uint32_t)(want_adv ? 1 : 0));  // other: "Switch to ...?" follows
            season().save_config();
        };
        a.on_yes = [choose, adv] { choose(!adv); };
        a.on_no = [choose, adv] { choose(adv); };
        enqueue(std::move(a));
    }

    // ---- "Play CFL 2026 now?" ----------------------------------------------------------------------------------------

    struct LeaguePack {
        std::string id, title, modes;  // modes: "Arcade and Tournament"
    };
    // The league packs inside an installed pak (paks/<id>/packs/<pack>/pack.json with "league").
    std::vector<LeaguePack> league_packs_in(const std::string& pak_id) {
        std::vector<LeaguePack> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(blitz::paks_dir() / pak_id / "packs", ec)) {
            std::ifstream in{e.path() / "pack.json"};
            if (!in) continue;
            try {
                nlohmann::json j = nlohmann::json::parse(in);
                if (j.value("format", "") != "blitz-pack/2" || !j.contains("league")) continue;
                std::string id = e.path().filename().string();
                std::vector<std::string> names{"Arcade"};
                if (j["league"].is_object() && j["league"]["modes"].is_array()) {
                    for (const auto& m : j["league"]["modes"]) {
                        if (m == "tournament") names.push_back("Tournament");
                        else if (m == "play_editor") names.push_back("Play Editor");
                    }
                }
                std::string modes;
                for (size_t i = 0; i < names.size(); i++) modes += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
                out.push_back({id, j.value("title", id), modes});
            }
            catch (...) {
            }
        }
        return out;
    }
}

namespace {
    void open_now(Ask a) {
        fprintf(stderr, "[league] prompt: %s\n", a.header.c_str());
        showing = true;
        answered = false;
        showing_no = a.on_no;
        shown_ask = a;
        shown_in_game = ultramodern::is_game_started();
        auto yes = [f = a.on_yes] { answered = true; if (f) f(); };
        auto no = [f = a.on_no] { answered = true; if (f) f(); };
        if (a.single) recompui::open_info_prompt(a.header, a.body, a.yes, yes, recompui::ButtonStyle::Primary);
        else recompui::open_choice_prompt(a.header, a.body, a.yes, a.no, yes, no, a.yes_style, recompui::ButtonStyle::Secondary,
                                          a.focus_no);
    }
}

namespace blitz {
    // season_pak.cpp, as a new pak goes in (any thread: a download's, Add-Ons', the launch's).
    void league_pak_arrived(const std::string& pak_id) {
        for (const auto& [id, title, modes] : league_packs_in(pak_id)) {
            Ask a;
            a.key = "play_now";
            a.header = "Play " + title + " now?";
            a.body = title + " is installed. Yes switches League to it (Settings → Teams): it plays " + modes +
                     " with its own teams. Not Now keeps your league; you can switch any time.";
            a.yes = "Yes";
            a.no = "Not Now";
            a.needs_quiet = true;
            a.focus_no = false;
            std::string pack = id;
            a.on_yes = [pack] {
                fprintf(stderr, "[league] Play now: %s\n", pack.c_str());
                season().update_option_value("league", pack);  // during a game: "Switch to ...?" follows
                season().save_config();
                original_settings_visibility();
            };
            a.on_no = [] {};
            a.still_wanted = [pack] { return league_id() != pack && !original_game(); };
            enqueue(std::move(a));
        }
    }

    // What waits for a restart, for the Settings header's Restart (its dot and its prompt): "" nothing.
    std::string restart_pending_text() {
        std::string out = differing();
        std::string app = app_restart_setting();
        if (!app.empty()) out += (out.empty() ? "" : ", ") + app;
        return out;
    }

    // config.cpp, once the settings are loaded.
    void league_select_init() {
        season_change_hook = on_season_change;
        league_menu_hook = on_league_menu;
        new_season_hook = on_new_season;
        league_notice_hook = on_league_notice;
        recompui::config::set_restart_pending(restart_pending_text);
        recompui::config::set_option_browse_left(on_browse_left);
        recompui::config::set_close_guard(close_guard);
        seen_generation = content_dirs_generation();
        std::string fell = league_check_installed(true);
        if (!fell.empty()) fallback_toast = fell;
    }

    // Every UI tick (main.cpp): the snapshot at the game's start, a pack that went away, and the next prompt.
    void league_select_update() {
        bool game = ultramodern::is_game_started();
        const bool settled = settings_settled();  // every tick, so it sees Settings open
        {
            bool take = false;
            {
                std::lock_guard lock{mutex};
                take = game && (!snapped || resnap);
            }
            if (take) snapshot();
        }
        if (int g = content_dirs_generation(); g != seen_generation) {
            seen_generation = g;
            reverting = true;  // not a choice: no "Switch to NFL 2026?" (the dot shows the game still has the old one)
            std::string fell = league_check_installed(true);
            reverting = false;
            if (!fell.empty()) fallback_toast = fell;
        }
        if (!fallback_toast.empty() && game) {
            post_status(fallback_toast);
            fallback_toast.clear();
        }
        if (showing && !recompui::is_prompt_open()) {
            showing = false;
            if (!answered && shown_ask && shown_in_game != game) {
                fprintf(stderr, "[league] prompt closed as the game started: asked again\n");
                std::lock_guard lock{mutex};
                queue.push_front(std::move(*shown_ask));
            }
            else if (!answered && showing_no) {
                fprintf(stderr, "[league] prompt closed with Back: its second button\n");
                showing_no();
            }
            showing_no = nullptr;
            shown_ask.reset();
        }
        if (recompui::is_prompt_open() || recompui::is_keyboard_open()) return;
        // Before the game, only over the launcher: a start straight into the game (--game, a phone) has no prompt to
        // open until the UI is up, and opening one then ends the app ("an invalid UI context").
        if (!game && !launcher_shown()) return;
        Ask a;
        {
            std::lock_guard lock{mutex};
            bool quiet = !game || settled;
            auto it = queue.begin();
            while (it != queue.end() && it->needs_quiet && !quiet) ++it;
            if (it == queue.end()) return;
            a = std::move(*it);
            queue.erase(it);
        }
        if (a.still_wanted && !a.still_wanted()) return;
        open_now(std::move(a));
    }
}
#endif
