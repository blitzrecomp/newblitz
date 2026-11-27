// First run and Blitz Builder (docs/design/FIRST_RUN_V2.md; the team's answers in its §12 lead). The first start's setup,
// then out of the way: once it's done the app starts straight into the game, and Settings → General → Main Menu or
// Blitz Builder bring it back. firstrun.cpp was builder.cpp.
//
// The screens, in order, each skipped once it's answered (so a setup that's resumed never asks the same thing twice):
//   A. Where do you want to play? (computers only): This computer, Phone or handheld, PS5, N64 or emulator.
//   Phone or handheld (no QR code since 2026-11-15): B, C, Setup, then Android or iPhone: the file made here and the
//      steps to install it (Sideloadly for an iPhone; the APK for Android). The Qr screen below is no longer reached.
//   B. The ROM: Load ROM (the system's file picker), or "Using your saved ROM" for one stored before setup began.
//   C. Add the 2026 season? (the public app, without the season): Add It downloads the season pak (season_pak.cpp);
//      Just the 1999 Game plays Original 1999.
//   Setup: Recommended, or Customize (Settings opens on Rules; setup goes on when it closes).
//   D. Updates and reports (a device with internet: this computer's own play, a phone): two check boxes, roster and
//      season updates (ticked) and crash reports (not ticked; crash_report.cpp).
//   Then the path's last step: Play (the game starts); the Android or iPhone file (device_send.cpp) and how to install
//   it; the PS5's address, the send and "Sent"; or the Export screen (export_screen.cpp) and "Your ROM is saved".
// Phones never see A: B, C, Setup, D, Play. A phone set up from a computer (its builder_done) asks D only.
// The hub model (team, 2026-11-15): the computer app pushes the game, the ROM and the setup to a device, so a phone,
// handheld or PS5 never shows Blitz Builder, and a phone with no ROM shows "Set up New Blitz from your computer" for B,
// with its own way round (Pick a File Instead; on an iPhone also Share from the Files app).
// Every screen has Exit (back to the launcher; during the first run its Finish Setup row resumes) and, after the
// first, Back (the screen before, with the earlier answers still chosen).
//
// How it starts (§6.1): on Windows 0.9.2 it never did, and nothing in the log said so. Whether setup is needed is
// worked out from the state every time it's asked (no latch); the launcher's own frame hook starts it at its 30th
// frame; a watchdog starts it 3 s after the launcher showed if it still isn't up (and says so); and the launcher's
// Finish Setup row, there while setup isn't done, starts it by hand. Every step prints a [firstrun] line (§7), so a
// run where it doesn't open shows where it stopped. Not when the app was started straight into the game (`--game`:
// dev runs and the live harness) or on the PS5 (always set up from a computer).
// About 1 GB of RAM (BLITZ_LEAN, Potato Mode): B is BlitzRomFirst.java, in the same look, before the game loads (the
// system picker in front of a running game got it killed for its memory). The rest runs here as everywhere, before the
// game starts (config.cpp skip_launcher waits for it), and a ROM stored at this start counts as B answered.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "SDL2/SDL.h"
#include "blitz_config.h"
#include "blitz_platform.h"
#include "blitz_host.h"
#include "../game/season_pak.h"
#include "librecomp/game.hpp"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "ultramodern/ultramodern.hpp"
#include "util/file.h"
#include "firstrun_screen.h"
#include "os_shell.h"

namespace blitz {
    void export_screen_open_for_setup(const std::string& lead, std::function<void()> back, std::function<void()> exported);  // export_screen.cpp
    void export_screen_close();
    bool device_kits_available();  // device_send.cpp
    bool iphone_kit_available();
    bool iphone_ipa_available();
    bool android_kit_available();
    void save_iphone_ipa();
    void save_android_apk();
    std::string ps5_last_address();
    void send_to_ps5(const std::string& ip, bool beta);
    std::string ps5_stable_label();  // device_send.cpp: "1.0.2 (build 789)", the build tested on a PS5
    bool ps5_beta_available();
    bool ps5_stable_tested();  // device_send.cpp: false when STABLE.txt says "tested: no"
    void send_to_iphone();
    void send_to_android();
    void send_to_android_replacing();      // the device's New Blitz removed first (another signature)
    std::string device_send_problem();     // the last Android send's problem ("downgrade", "no-device" ...), "" none
    int device_send_state();             // 0 idle, 1 sending, 2 done, 3 failed
    std::string device_send_message();
    std::string device_send_progress();  // the PS5 send's progress line
    std::filesystem::path device_send_folder();
    void rosters_check_again();  // rosters_live.cpp
    void original_settings_visibility();  // adv_mode.cpp: the settings that need the 2026 Season show once it's in
    bool store_rom(const std::filesystem::path& path, int& kind, std::string& message);  // main.cpp
    void show_launcher();          // main.cpp
    void post_status(const std::string& msg);  // notices.cpp
    std::vector<std::string> crash_report_preview(bool& sent);  // crash_report.cpp
    bool savestate_save_then(std::function<void(bool)> done);  // savestate.cpp
    void checkpoint_discard_on_quit();  // checkpoint.cpp
    void leave_to_main_menu();  // main.cpp
    void experience_switch();   // experience.cpp
    bool crash_reports_on();
    void set_crash_reports(bool on);
    void reset_data_open(bool restore);  // reset_data.cpp: Settings → General → Reset Data, Restore a Backup
    void reset_data_update();
    void pak_backups_open();    // pak_backups.cpp: Settings → General → Controller Pak Backups
    void pak_backups_update();
    extern std::atomic<int> stored_rom_changes;  // known_roms.cpp
    std::string stored_rom_display_name();
}

namespace {
    namespace fs = std::filesystem;
    using blitz::firstrun_ui::Screen;
    using blitz::firstrun_ui::Action;
    using blitz::firstrun_ui::Check;

    // [firstrun] lines (§7): one per event, on stderr, which app_log.cpp copies into blitz.log.
    void log(const char* fmt, ...) {
        char line[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        fprintf(stderr, "[firstrun] %s\n", line);
    }

    enum Step {
        Idle, Where, Qr, Rom, RomBad, Season, Downloading, SeasonFailed, Setup, CustomizeWait, Updates, ReportView,
        Device, Android, Iphone, Sending, SendFailed, InstallSteps, Ps5, Sent, Export, ExportWait, ExportDone,
        LeaveGame, StepCount
    };
    const char* step_names[StepCount] = {
        "idle", "where", "qr", "rom", "rom-bad", "season", "season-download", "season-failed", "setup", "customize",
        "updates", "report-view", "device", "android", "iphone", "sending", "send-failed", "install-steps", "ps5",
        "sent", "export", "export-wait", "export-done", "leave-game"};

    // Where the player wants to play (A), and on the phone path, which phone and how.
    enum Where { Here, Phone, Ps5Target, ExportTarget };
    enum PhoneKind { NoPhone, AndroidUsb, AndroidFile, IphoneIpa, IphoneXcode };

    struct State {
        int step = Idle;
        std::vector<int> history;  // the screens before this one (Back)
        bool builder = false;      // run again from Blitz Builder (welcomed already): Exit changes nothing
        int where = Here;
        bool no_camera = false;    // the phone path makes the file here
        int phone = NoPhone;
        bool customize = false;
        int season_answer = -1;    // C: 0 Add It, 1 Just the 1999 Game (focus on Back)
        int rom_tries = 0;         // wrong ROMs in a row
        std::string rom_problem;
        bool rom_confirmed = false;  // "Use It" on the saved ROM
        bool rom_shown = false, season_shown = false, updates_shown = false;
        bool rosters = true, reports = false;  // D's boxes, kept while going back and forth
        bool updates_read = false;             // ... read from the settings the first time D shows
        std::string ps5_ip;
        bool ps5_beta = false;  // Send Beta: this version's PS5 build, untested on a PS5
        std::string send_result;
        std::string send_progress;  // the progress line on the Sending screen now
        std::string send_problem;   // an Android send's problem (device_send_problem), for its way forward
        bool android_replace = false;  // Remove and Install: the next USB send removes the device's copy first
        bool offline = false;      // the season download couldn't reach the server
        std::string export_path, export_detail;
        bool export_ok = false;
        bool export_started_game = false;  // the export started the game to build the ROM (not one already being played)
    } st;

    std::atomic<bool> dirty{false};       // the screen needs building again (the main thread's tick does it)
    std::atomic<int> download_result{0};  // the season download: 0 running, 1 installed, 2 failed
    std::mutex mutex;
    std::string download_message;
    Uint32 download_started = 0;
    // A picked ROM, from the file dialog's callback: 0 none, 1 stored, 2 refused (rom_pick_kind and message).
    std::atomic<int> rom_pick{0};
    int rom_pick_kind = 0;
    std::string rom_pick_message;
    std::atomic<int> export_result{0};    // 0 waiting, 1 saved, 2 failed
    std::atomic<int> settings_request{0}; // Settings rows: 1 Blitz Builder, 2 Main Menu, 3 the crash report view
    std::atomic<bool> leave_requested{false};

    bool autostarted = false;
    bool builder_done_at_start = false;
    Uint32 launcher_ready_at = 0;
    bool abandoned = false;   // Exit this run: only Finish Setup brings it back (no automatic start)
    int watchdog_fires = 0;
    Uint32 watchdog_next = 0;
    std::string last_decision;

    recomp::config::Config& general() { return recompui::config::get_general_config(); }
    bool flag(const char* id) { return std::get<bool>(general().get_option_value(id)); }
    void set_flag(const char* id, bool v) {
        general().update_option_value(id, v);
        general().save_config();
    }
    bool welcomed() { return flag("welcomed"); }

    bool modern() {
#if BLITZ_MODERN
        return true;
#else
        return false;
#endif
    }

    bool original_chosen() {
#if BLITZ_MODERN
        return std::get<uint32_t>(recompui::config::get_config("season").get_option_value("game")) == 1;
#else
        return true;
#endif
    }

    void set_game(uint32_t v) {
#if BLITZ_MODERN
        recomp::config::Config& season = recompui::config::get_config("season");
        season.update_option_value("game", v);
        season.save_config();
        blitz::original_settings_visibility();
#else
        (void)v;
#endif
    }

    fs::path stored_rom_path() { return recomp::get_config_path() / (std::u8string(blitz::game_id) + u8".z64"); }

    bool rom_valid() {
        std::u8string id = blitz::game_id;
        return recomp::is_rom_valid(id);
    }

    std::time_t rom_time() {
        std::error_code ec;
        auto t = fs::last_write_time(stored_rom_path(), ec);
        if (ec) return 0;
        // (No clock_cast in every standard library yet: the file clock's distance from now, on the system clock.)
        return std::chrono::system_clock::to_time_t(std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            t - fs::file_time_type::clock::now() + std::chrono::system_clock::now()));
    }

    std::string date_text(std::time_t t, const char* fmt) {
        char out[32];
        std::strftime(out, sizeof(out), fmt, std::localtime(&t));
        return out;
    }

    // "Nov 3, 2026" (American order, no leading zero).
    std::string human_date(std::time_t t) {
        std::string day = date_text(t, "%d");
        if (!day.empty() && day[0] == '0') day.erase(0, 1);
        return date_text(t, "%b") + " " + day + ", " + date_text(t, "%Y");
    }

    // When setup first began on this install (hidden setup_started, seconds), set the first time it's read.
    std::time_t setup_started() {
        std::string s = std::get<std::string>(general().get_option_value("setup_started"));
        if (s.empty()) {
            s = std::to_string((long long)std::time(nullptr));
            general().update_option_value("setup_started", s);
            general().save_config();
        }
        return (std::time_t)std::atoll(s.c_str());
    }

    // A ROM stored before this install's setup began (another install's, or 0.9.2's on Windows): setup shows it
    // and asks before using it.
    bool rom_leftover() {
        std::time_t t = rom_time();
        return t != 0 && t < setup_started();
    }

    const char* rom_state(bool checked) {
        std::error_code ec;
        if (!fs::is_regular_file(stored_rom_path(), ec)) return "none";
        if (!checked) return "stored";
        return rom_valid() ? "valid" : "bad";
    }

    void log_state(bool checked) {
        std::time_t t = rom_time();
        log("state welcomed=%d rom=%s romsaved=%s pak=%d season_asked=%d updates_asked=%d builder_done=%d",
            welcomed() ? 1 : 0, rom_state(checked), t ? date_text(t, "%Y-%m-%d").c_str() : "", blitz::season_content() ? 1 : 0,
            flag("setup_season_asked") ? 1 : 0, flag("setup_updates_asked") ? 1 : 0, builder_done_at_start ? 1 : 0);
    }

    bool is_ps5() { return blitz::platform::name == std::string_view("PS5"); }

    // A phone set up from a computer (builder_done) still asks D once: the computer never answers it for a device.
    bool device_asks_updates() { return flag("setup_ask_updates") && !flag("setup_updates_asked"); }

    // Whether setup should be up, from the state alone (no latch: a missed tick or a ROM that arrives late can't lose
    // it). reason: why not, or why.
    bool needed(const char*& reason) {
        if (is_ps5()) { reason = "ps5"; return false; }
        if (autostarted) { reason = "autostart"; return false; }
        if (ultramodern::is_game_started()) { reason = "game"; return false; }
        if (welcomed() && !device_asks_updates()) { reason = builder_done_at_start ? "builder_done" : "welcomed"; return false; }
        reason = welcomed() ? "updates-unasked" : "not-welcomed";
        return true;
    }

    bool decide() {
        const char* reason = "";
        bool yes = needed(reason);
        std::string d = std::string(yes ? "yes" : "no") + " reason=" + reason;
        if (d != last_decision) {
            last_decision = d;
            log("needed=%s", d.c_str());
        }
        return yes;
    }

    bool running() { return st.step != Idle; }

    // The launcher steps aside while setup is up: under a screen it takes the D-pad from it (2026-10-25). Not over a
    // running game (Blitz Builder from Settings).
    void launcher_visible(bool on) {
        if (ultramodern::is_game_started()) return;
        recompui::ContextId launcher = recompui::get_launcher_context_id();
        if (launcher == recompui::ContextId::null()) return;
        if (on) blitz::show_launcher();
        else if (recompui::is_context_shown(launcher)) recompui::hide_context(launcher);
    }

    // ---- which screens this path has -------------------------------------------------------------------------------

    // Potato Mode: a stored ROM is never asked about again. It's BlitzRomFirst's (picked before the game loaded), or one
    // kept from earlier; Change ROM there would open the system picker inside the app. Settings → Change ROM still can.
    bool lean() { return getenv("BLITZ_LEAN") != nullptr; }
    bool rom_needed() { return !rom_valid() || (!lean() && !welcomed() && rom_leftover() && !st.rom_confirmed); }
    bool season_needed() { return modern() && !blitz::season_content() && !flag("setup_season_asked"); }
    // D: devices that play here with internet (this computer's own play, a phone). Not for a device made here (it asks
    // its own), the PS5 or an N64.
    bool updates_on_path() { return st.where == Here && !st.no_camera; }
    bool updates_needed() { return !flag("setup_updates_asked"); }

    // The screens of the current path, in order (the step count, and what comes next).
    std::vector<int> plan() {
        std::vector<int> p;
        if (welcomed() && device_asks_updates() && !st.builder) {  // set up from a computer: D only
            p.push_back(Updates);
            return p;
        }
        if (blitz::platform::desktop) p.push_back(Where);
        if (rom_needed() || st.rom_shown) p.push_back(Rom);
        if (season_needed() || st.season_shown) p.push_back(Season);
        p.push_back(Setup);
        if (updates_on_path() && (updates_needed() || st.updates_shown)) p.push_back(Updates);
        if (st.where == Phone) {
            p.push_back(Device);
            p.push_back(st.phone == AndroidUsb || st.phone == AndroidFile ? Android : Iphone);
            p.push_back(InstallSteps);
        }
        else if (st.where == Ps5Target) {
            p.push_back(Ps5);
            p.push_back(Sent);
        }
        else if (st.where == ExportTarget) {
            p.push_back(Export);
            p.push_back(ExportDone);
        }
        return p;
    }

    // Screens that are steps of their own; the rest (a failure, a wait) count as the step they belong to.
    int step_of(int s) {
        switch (s) {
            case RomBad: return Rom;
            case Downloading: case SeasonFailed: return Season;
            case CustomizeWait: return Setup;
            case ReportView: return Updates;
            case Sending: case SendFailed: return st.where == Ps5Target ? Ps5 : st.phone == AndroidUsb || st.phone == AndroidFile ? Android : Iphone;
            case ExportWait: return Export;
            default: return s;
        }
    }

    std::string eyebrow() {
        std::vector<int> p = plan();
        auto it = std::find(p.begin(), p.end(), step_of(st.step));
        std::string head = st.builder ? "BLITZ BUILDER" : "SETUP";
        // A, the first screen: how many steps follow depends on its answer (a PS5 has its own), so no count there.
        if (it == p.end() || step_of(st.step) == Where) return head;
        return head + " \xC2\xB7 STEP " + std::to_string(it - p.begin() + 1) + " OF " + std::to_string(p.size());
    }

    void show();
    void finish(const char* target);

    // Opens a screen (from a button or the tick: it's built on the next tick).
    void go(int next, bool remember = true) {
        if (remember && st.step != Idle && st.step != next && (st.history.empty() || st.history.back() != st.step))
            st.history.push_back(st.step);
        st.step = next;
        dirty = true;
    }

    // The next screen of the path after this one; the end of the path finishes setup.
    void next() {
        std::vector<int> p = plan();
        int here = step_of(st.step);
        auto it = std::find(p.begin(), p.end(), here);
        // A screen that's no longer in the plan (the ROM just loaded, so B drops out): on to whatever follows the
        // first screen not yet behind us.
        if (it == p.end() || it + 1 == p.end()) {
            if (it == p.end()) {
                for (int s : p) {
                    if (std::find(st.history.begin(), st.history.end(), s) == st.history.end() && s != here) {
                        go(s);
                        return;
                    }
                }
            }
            finish(st.where == Phone ? "device" : st.where == Ps5Target ? "ps5" : st.where == ExportTarget ? "export" : "computer");
            return;
        }
        go(*(it + 1));
    }

    void back() {
        if (st.history.empty()) return;
        int prev = st.history.back();
        st.history.pop_back();
        log("back to %s", step_names[prev]);
        go(prev, false);
    }

    // Exit: setup leaves for the launcher. During the first run its answers so far stay (the setup_* marks), welcomed
    // stays false and Finish Setup shows; from Blitz Builder nothing has changed. A season download carries on in the
    // background and finishes cleanly (or doesn't, and nothing changed).
    // Exit and the end of a path are often pressed from a screen's own button, with its context open: the launcher
    // can't be opened from there (one context at a time). Both happen on the next tick.
    std::atomic<int> pending{0};  // 1 exit, 2 finish
    std::string pending_target;

    void exit_setup() {
        pending = 1;
        dirty = true;
    }

    void finish(const char* target) {
        pending_target = target;
        pending = 2;
        dirty = true;
    }

    void do_exit() {
        log("abandoned at=%s", step_names[st.step]);
        blitz::export_screen_close();
        blitz::firstrun_ui::hide();
        bool leave = st.export_started_game && ultramodern::is_game_started();  // as do_finish: not into the export's game
        st = State{};
        abandoned = true;
        if (leave) {
            blitz::checkpoint_discard_on_quit();
            blitz::leave_to_main_menu();
            return;
        }
        launcher_visible(true);
    }

    void do_finish(const char* target) {
        set_flag("welcomed", true);
        set_flag("setup_ask_updates", false);
        log("done target=%s welcomed=1 game=%s setup=%s", target, original_chosen() ? "original" : "new",
            st.customize ? "customize" : "recommended");
        blitz::firstrun_ui::hide();
        bool play = std::string(target) == "computer";
        // Export ROM started the game only to build the ROM: back to the launcher, not into that game (a game that was
        // already being played when Blitz Builder opened carries on).
        bool leave = std::string(target) == "export" && st.export_started_game && ultramodern::is_game_started();
        st = State{};
        if (leave) {
            log("export done: back to the launcher");
            blitz::checkpoint_discard_on_quit();
            blitz::leave_to_main_menu();
            return;
        }
        if (play && !ultramodern::is_game_started()) {
            std::u8string id = blitz::game_id;
            if (recomp::is_rom_valid(id)) {
                recompui::update_game_mod_id(blitz::mod_game_id);
                recomp::start_game(id, {});
                recompui::hide_all_contexts();
                return;
            }
        }
        launcher_visible(true);
    }

    void open_settings(const char* tab) {
        recompui::update_game_mod_id(blitz::mod_game_id);
        recompui::config::set_tab(tab);
        recompui::config::open();
    }

    bool settings_open() {
        try {
            return recompui::is_context_shown(recompui::config::get_config_context_id());
        }
        catch (const std::exception&) {
            return false;
        }
    }

    // The ROM picker (B): the system's file dialog (a sheet on a computer, the Files or Android picker on a phone).
    // BLITZ_TEST_PICK=<file> stands in for the dialog (rehearsals; a .pick file holds the path to pick).
    // A picked ROM stored (or refused) for B to show: Load ROM's file, or one opened into the app on a phone.
    void took_rom(const fs::path& path) {
        int kind = 0;
        std::string message;
        bool ok = blitz::store_rom(path, kind, message);
        {
            std::lock_guard lock{mutex};
            rom_pick_kind = kind;
            rom_pick_message = message;
        }
        rom_pick = ok ? 1 : 2;
    }

    void pick_rom() {
        auto took = [](const fs::path& path) { took_rom(path); };
        if (const char* test = getenv("BLITZ_TEST_PICK"); test != nullptr && *test) {
            fs::path pick = test;
            if (pick.extension() == ".pick") {  // a file naming the pick, changed between presses
                std::string line;
                std::getline(std::ifstream(pick), line);
                pick = line;
            }
            took(pick);
            return;
        }
        recompui::file::open_file_dialog([took](bool success, const fs::path& path) {
            if (success) took(path);
            else log("rom picker closed without a file");
        });
    }

    std::string send_target() {
        return st.where == Ps5Target ? "ps5" : st.phone == AndroidUsb ? "android-usb" : st.phone == AndroidFile ? "android-file"
             : st.phone == IphoneXcode ? "iphone" : "ipa";
    }

    void start_send() {
        log("send start target=%s%s", send_target().c_str(), st.phone == AndroidUsb && st.android_replace ? " replace=1" : "");
        if (st.where == Ps5Target) blitz::send_to_ps5(st.ps5_ip, st.ps5_beta);
        else if (st.phone == AndroidUsb && st.android_replace) blitz::send_to_android_replacing();
        else if (st.phone == AndroidUsb) blitz::send_to_android();
        else if (st.phone == AndroidFile) blitz::save_android_apk();
        else if (st.phone == IphoneXcode) blitz::send_to_iphone();
        else blitz::save_iphone_ipa();
        st.android_replace = false;
        go(Sending);
    }

    // ---- the screens ----------------------------------------------------------------------------------------------

    Screen base(const std::string& title, const std::string& body) {
        Screen s;
        s.eyebrow = eyebrow();
        s.title = title;
        s.body = body;
        if (!st.history.empty()) s.back = [] { back(); };
        s.exit = [] { exit_setup(); };
        return s;
    }

    Action act(const std::string& label, const char* answer, std::function<void()> then) {
        std::string screen = step_names[st.step];
        return Action{label, [screen, answer, then] {
            log("answered %s %s", screen.c_str(), answer);
            then();
        }};
    }

    // iPhone wording in one place (§12.6: the free Apple ID's limits are still being tested; easy to change).
    // BRANDING: the iPhone folder's name in Downloads
    const char* iphone_steps =
        "Saved in Downloads \xE2\x86\x92 New Blitz for iPhone. This file holds your ROM. Don't share it.\n"
        "\n"
        "1. Install NewBlitz.ipa with Sideloadly (sideloadly.io) and your own Apple ID. A free one works. Turn on its "
        "AutoRefresh so it renews itself over Wi-Fi.\n"
        "2. On the iPhone: trust your Apple ID in Settings \xE2\x86\x92 General \xE2\x86\x92 VPN & Device Management.\n"
        "3. Open New Blitz. It starts ready to play. Every step: newblitz.com/start";

    void show() {
        Screen s;
        switch (st.step) {
            case Where: {
                s = base("Where do you want to play?",
                         "Pick one. You can run this again any time from Blitz Builder (Settings \xE2\x86\x92 General).");
                s.actions = {act("This Computer", "here", [] { st.where = Here; next(); })};
                // Phone or Handheld makes the iPhone or Android file here and walks through installing it (team,
                // 2026-11-15: no QR code; an iPhone needs Sideloadly anyway, an Android device takes the APK), so only
                // where this app carries a phone's app (device_send.cpp's kits).
                if (blitz::android_kit_available() || blitz::iphone_ipa_available() || blitz::iphone_kit_available())
                    s.actions.push_back(act("Phone or Handheld", "phone", [] { st.where = Phone; st.no_camera = true; next(); }));
                // PS5 and N64 only where this app can make them: the PS5 kit (device_send.cpp), Export ROM.
                std::error_code ec;
                if (fs::is_directory(recompui::file::get_program_path() / "devices" / "ps5", ec))
                    s.actions.push_back(act("PS5", "ps5", [] { st.where = Ps5Target; next(); }));
                if (blitz::export_available)
                    s.actions.push_back(act("N64 or Emulator", "export", [] { st.where = ExportTarget; next(); }));
                {   // the earlier answer, by its label (the row's place depends on which kits this app carries)
                    const char* want = st.where == Phone ? "Phone or Handheld" : st.where == Ps5Target ? "PS5"
                                     : st.where == ExportTarget ? "N64 or Emulator" : "This Computer";
                    s.focus = 0;
                    for (size_t i = 0; i < s.actions.size(); i++) if (s.actions[i].label == want) s.focus = (int)i;
                }
                break;
            }
            case Qr:
                s = base("Get it on your phone",
                         "Scan this with your phone's camera. The page gives you the Android or iPhone app, and the app asks "
                         "for your ROM itself.");
                // BRANDING: the download page's address
                s.qr_url = "https://newblitz.com/download";
                s.qr_caption = "newblitz.com/download";
                s.actions = {act("Done", "done", [] { finish("qr"); })};
                // Only where this app carries a phone's app to make the file from (device_send.cpp's kits).
                if (blitz::android_kit_available() || blitz::iphone_ipa_available() || blitz::iphone_kit_available())
                    s.actions.push_back(act("No Camera? Make the File on This Computer", "no-camera", [] { st.no_camera = true; next(); }));
                s.focus = st.no_camera ? 1 : 0;
                break;
            case Rom:
                st.rom_shown = true;
                // A ROM stored before setup began, or the one picked a moment ago (Back): shown, with Use It.
                if (rom_valid() && (st.rom_confirmed || (rom_leftover() && !welcomed()))) {
                    std::string name = blitz::stored_rom_display_name();
                    s = base(st.rom_confirmed ? "Your ROM" : "Using your saved ROM", (name.empty() ? std::string("NFL Blitz 2000") : name) + " \xC2\xB7 saved " +
                                                         human_date(rom_time()) + "\n\nNew Blitz keeps its own copy. It never leaves this device.");
                    s.actions = {
                        act("Use It", "use-it", [] {
                            st.rom_confirmed = true;
                            log("rom ready source=stored-earlier name=%s", blitz::stored_rom_display_name().c_str());
                            next();
                        }),
                        act("Change ROM", "change-rom", [] { pick_rom(); }),
                    };
                    break;
                }
                if (!blitz::platform::desktop) {
                    // A phone or handheld with no ROM (an APK or IPA installed by hand): the computer is the hub, so
                    // that's the way it asks for; picking the file here is the way round.
                    log("rom screen: set up from the computer, or pick a file");
                    // BRANDING: app name, the setup tool's name
                    s = base("Set up New Blitz from your computer",
                             std::string("Open New Blitz on your Mac, Windows or Linux computer \xE2\x86\x92 Blitz Builder \xE2\x86\x92 Phone or "
                                         "Handheld. ") +
#if defined(BLITZ_IOS)
                                 "It makes this app again with your ROM and setup inside: install that one, and New Blitz "
                                 "starts ready to play.\n\n"
                                 "No computer? Pick a File Instead: your NFL Blitz 2000 ROM, the USA cartridge, v1.1 (.z64, "
                                 ".n64, .v64 or a .zip). Or in the Files app, share the ROM to New Blitz (Share \xE2\x86\x92 New Blitz)."
#else
                                 "It sends the game, your ROM and your setup here over USB, and New Blitz starts ready to play.\n\n"
                                 "No computer? Pick a File Instead: your NFL Blitz 2000 ROM, the USA cartridge, v1.1 (.z64, "
                                 ".n64, .v64 or a .zip)."
#endif
                    );
                    s.actions = {act("Pick a File Instead", "pick-a-file", [] { pick_rom(); })};
                    break;
                }
                s = base("Load your NFL Blitz 2000 ROM",
                         std::string("The USA cartridge, v1.1 (.z64, .n64, .v64 or a .zip).\n") +
                             (blitz::platform::desktop ? "New Blitz keeps its own copy. It never leaves this computer."
                                                       : "New Blitz keeps its own copy. It never leaves this device."));
                s.actions = {act("Load ROM", "load-rom", [] { pick_rom(); })};
                break;
            case RomBad:
                s = base("That ROM didn't work", st.rom_problem);
                if (st.rom_tries >= 4) s.note = "The right file is NFL Blitz 2000 for the N64, USA, version 1.1.";
                s.actions = {act("Try Another", "try-another", [] { pick_rom(); })};
                break;
            case Season:
                st.season_shown = true;
                // BRANDING: Blitz Builder's season question
                s = base("Add the 2026 season?",
                         "Today's 32 teams, rosters, uniforms and team art, as a small download from the New Blitz server.\n\n"
                         "Or play NFL Blitz 2000 exactly as it shipped in 1999.\n\n"
                         "Either way you get widescreen, save states and the rest. Add the season any time from Add-Ons.");
                s.actions = {
                    act("Add It", "add-it", [] {
                        st.season_answer = 0;
                        download_result = 0;
                        download_started = SDL_GetTicks();
                        log("season download start");
                        bool started = blitz::pak_download_start([](bool ok, const std::string& message) {
                            {
                                std::lock_guard lock{mutex};
                                download_message = message;
                            }
                            download_result = ok ? 1 : 2;
                        });
                        if (!started) {
                            std::lock_guard lock{mutex};
                            download_message = "A download is already running.";
                            download_result = 2;
                        }
                        go(Downloading);
                    }),
                    act("Just the 1999 Game", "1999", [] {
                        st.season_answer = 1;
                        set_game(1u);
                        set_flag("setup_season_asked", true);
                        next();
                    }),
                };
                s.focus = st.season_answer == 1 ? 1 : 0;
                break;
            case Downloading:
                s = base("Getting the 2026 Season\xE2\x80\xA6", "One moment: the season is downloading and going in.");
                s.back = nullptr;  // it carries on in the background; Exit leaves it to finish
                if (SDL_GetTicks() - download_started >= 10000) s.actions = {act("Cancel", "cancel", [] { exit_setup(); })};
                break;
            case SeasonFailed: {
                std::string why;
                {
                    std::lock_guard lock{mutex};
                    why = download_message;
                }
                s = base("No 2026 Season for now", why.empty() ? std::string("The season couldn't be downloaded.") : why);
                s.actions = {
                    act("Try Again", "try-again", [] { go(Season, false); }),
                    act("Play the 1999 Game for Now", "1999-for-now", [] {
                        set_game(1u);
                        set_flag("setup_season_asked", true);
                        st.season_answer = 1;
                        st.step = Season;  // what follows Season
                        next();
                    }),
                };
                break;
            }
            case Setup:
                s = base("Set it up for you?",
                         std::string(original_chosen() ? "The recommended setup plays the original game as it was, with hints "
                                                         "the first time you reach each screen."
                                                       : "The recommended setup plays great out of the box: the game's own "
                                                         "controls, and hints the first time you reach each screen.") +
                             "\n\nCustomize opens the house rules (Madden-style Receiver Buttons, the play clock, realism ...) "
                             "and every other setting. Setup goes on when you close them.");
                s.actions = {
                    act("Recommended", "recommended", [] { st.customize = false; next(); }),
                    act("Customize", "customize", [] {
                        st.customize = true;
                        go(CustomizeWait);
                    }),
                };
                s.focus = st.customize ? 1 : 0;
                break;
            case Updates: {
                st.updates_shown = true;
                if (!st.updates_read) {
                    // A first run's defaults: rosters ticked, reports not (§12.1, §12.3). Run again (Blitz Builder), what's set now.
                    st.updates_read = true;
                    st.reports = blitz::crash_reports_on();
                    if (flag("setup_updates_asked")) st.rosters = blitz::live_rosters_on();
                }
                bool rosters_row = modern() && !original_chosen() && blitz::season_content();
                s = base("Updates and reports", rosters_row ? "Two choices. Change them any time in Settings."
                                                            : "Change it any time in Settings.");
                if (rosters_row) {
                    s.checks.push_back(Check{
                        "Keep rosters and uniforms up to date",
                        "New lineups download in the background when the app starts, on Thursdays and Saturdays in season, "
                        "and a newer 2026 Season (uniforms, art) goes in at the next start.",
                        [] { return st.rosters; }, [](bool v) { st.rosters = v; }});
                }
                s.checks.push_back(Check{
                    "Send crash reports to the team",
                    "If the app crashes, a small report goes the next time you open it. No ROM, no files, no name.",
                    [] { return st.reports; }, [](bool v) { st.reports = v; }});
                if (st.offline && rosters_row) s.note = "You're offline. Rosters will update when you're connected.";
                s.actions = {
                    act("Continue", "continue", [rosters_row] {
                        if (rosters_row) {
                            blitz::set_live_rosters(st.rosters);
#if BLITZ_MODERN
                            if (st.rosters) blitz::rosters_check_again();
#endif
                        }
                        blitz::set_crash_reports(st.reports);
                        set_flag("setup_updates_asked", true);
                        set_flag("setup_ask_updates", false);
                        log("updates rosters=%s reports=%s", rosters_row ? (st.rosters ? "on" : "off") : "n/a", st.reports ? "on" : "off");
                        next();
                    }),
                    act("See What a Report Sends", "see-report", [] { go(ReportView); }),
                };
                break;
            }
            case ReportView: {
                bool sent = false;
                std::vector<std::string> blocks = blitz::crash_report_preview(sent);
                s = base(sent ? "The last crash report sent" : "What a crash report sends",
                         sent ? "Exactly what went, word for word."
                              : "Nothing has been sent from here. This is what a report would hold, made from this run's log: "
                                "the app version, the device, the crash and the log's last lines, with your folders' names taken out.");
                s.long_text = std::move(blocks);  // Back (the button under it, or the key) returns to the choices
                break;
            }
            case Device:
                s = base("Which phone or handheld?", "");
                if (blitz::android_kit_available()) s.actions.push_back(act("Android", "android", [] { st.phone = AndroidUsb; next(); }));
                if (blitz::iphone_ipa_available() || blitz::iphone_kit_available())
                    s.actions.push_back(act("iPhone", "iphone", [] { st.phone = IphoneIpa; next(); }));
                s.focus = (st.phone == IphoneIpa || st.phone == IphoneXcode) && s.actions.size() > 1 ? 1 : 0;
                break;
            case Android:
                s = base("Send to your Android device",
                         "Connect it with USB, with USB debugging on (Settings \xE2\x86\x92 About \xE2\x86\x92 tap Build number 7 times, "
                         "then Developer options \xE2\x86\x92 USB debugging).\n\n"
                         "Or save the app with your ROM and setup beside it, to copy over yourself.");
                s.actions = {
                    act("Send over USB", "usb", [] { st.phone = AndroidUsb; start_send(); }),
                    act("Save the APK", "save-apk", [] { st.phone = AndroidFile; start_send(); }),
                };
                s.focus = st.phone == AndroidFile ? 1 : 0;
                break;
            case Iphone:
                if (blitz::iphone_kit_available()) {
                    s = base("Send to your iPhone", "This Mac has the iPhone kit and Xcode: New Blitz installs on a connected "
                                                    "iPhone with your ROM and this setup.");
                    s.actions = {act("Send to the iPhone", "xcode", [] { st.phone = IphoneXcode; start_send(); })};
                    break;
                }
                s = base("Make the iPhone app",
                         "Apple only installs apps you sign yourself. New Blitz makes the app with your ROM and setup already "
                         "inside. You install it with Sideloadly and your own Apple ID. Steps: newblitz.com/start");
                s.actions = {act("Save the iPhone App (.ipa)", "save-ipa", [] { st.phone = IphoneIpa; start_send(); })};
                break;
            case Sending: {
                // The PS5's send takes minutes (etaHEN's FTP): what it's doing, refreshed by the tick.
                std::string progress = st.where == Ps5Target ? blitz::device_send_progress() : std::string();
                st.send_progress = progress;
                s = base(st.where == Ps5Target ? "Sending to your PS5\xE2\x80\xA6" : st.phone == AndroidUsb ? "Sending to your Android device\xE2\x80\xA6"
                                                                                  : "Saving\xE2\x80\xA6",
                         progress.empty() ? std::string("One moment.") : progress);
                s.back = nullptr;
                break;
            }
            case SendFailed: {
                std::string body = st.send_result.empty() ? std::string("The send stopped.") : st.send_result;
                auto try_again = [] { return act("Try Again", "try-again", [] { start_send(); }); };
                auto save_apk = [] { return act("Save the APK Instead", "save-apk", [] { st.phone = AndroidFile; start_send(); }); };
                if (st.phone == AndroidUsb) {
                    // Each of adb's usual failures in its own words, with its way forward (device_send.cpp words it).
                    const std::string& p = st.send_problem;
                    if (p == "incompatible") {
                        // BRANDING: app name
                        s = base("Replace New Blitz on the device?", body);
                        s.actions = {
                            act("Remove and Install", "remove-and-install", [] {
                                st.android_replace = true;
                                start_send();
                            }),
                            act("Cancel", "cancel", [] { go(Android, false); }),
                        };
                        s.focus = 1;  // the saves on the device go: not the default
                        break;
                    }
                    s = base(p == "downgrade" ? "The device has a newer New Blitz"  // BRANDING: app name
                             : p == "no-adb" ? "This computer needs adb"
                             : p == "no-device" ? "Connect your Android device"
                             : p == "unauthorized" ? "Allow this computer on the device"
                             : p == "offline" ? "The device isn't answering"
                             : p == "more-than-one" ? "More than one device"
                             : p == "storage" ? "The device is full"
                                              : "That didn't work",
                             body);
                    // No forced downgrade, and no adb to try with: saving the APK is the way on.
                    if (p == "downgrade" || p == "no-adb") s.actions = {save_apk(), try_again()};
                    else s.actions = {try_again(), save_apk()};
                    break;
                }
                s = base("That didn't work", body);
                s.actions = {try_again()};
                if (st.where == Ps5Target) s.actions.push_back(act("Change Address", "change-address", [] { go(Ps5, false); }));
                break;
            }
            case InstallSteps: {
                fs::path folder = blitz::device_send_folder();
                if (st.phone == AndroidUsb) s = base("It's on your device", "Open New Blitz on it: it starts ready to play.");
                else if (st.phone == IphoneXcode) s = base("It's on your iPhone", "Open New Blitz on it: it starts ready to play.");
                else if (st.phone == AndroidFile)
                    s = base("Saved in Downloads: New Blitz for Android",  // (no arrow in the title face)
                             "Copy NewBlitz.apk to the device (USB, a cloud folder, or a card) and open it. Android will ask you to "
                             "allow installs from that source.\n\nThen copy what's in the \"Copy to the device\" folder into New "
                             "Blitz's folder (Android/data/com.blitzrecomp.newblitz/files). The folder holds your ROM. Don't share it.");
                else s = base("Your iPhone app is saved", iphone_steps);
                s.actions = {act("Done", "done", [] { finish("device"); })};
                if (!folder.empty() && blitz::platform::file_browser)
                    s.actions.push_back(act("Open Folder", "open-folder", [folder] { blitz::open_in_file_browser(folder); }));
                break;
            }
            case Ps5: {
                if (st.ps5_ip.empty()) st.ps5_ip = blitz::ps5_last_address();
                {
                    std::string stable = blitz::ps5_stable_label();
                    std::string which = stable.empty() ? ""
                        : "\n\nSends New Blitz " + stable + (blitz::ps5_stable_tested() ? ", the PS5 build tested on a PS5." : ".");
                    if (blitz::ps5_beta_available())
                        which += " Send Beta sends this version's PS5 build instead: newer, but never run on a PS5.";
                    s = base("Send to your PS5", "On the PS5: etaHEN running, same network. Settings \xE2\x86\x92 Network shows the "
                                                 "address.\n\nThe PS5's address: " + st.ps5_ip + which);
                }
                // Send and Start can't restart a copy that's already open (the launch payload leaves it running).
                s.note = "If New Blitz is already open on the PS5, close it first.";
                s.actions = {
                    act("Send and Start", "send", [] {
                        // An address like 192.168.1.20
                        int a, b, c, d;
                        char extra;
                        if (sscanf(st.ps5_ip.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
                            blitz::post_status("That doesn't look like an address (like 192.168.1.20)");
                            return;
                        }
                        st.ps5_beta = false;
                        start_send();
                    }),
                    act("Change Address", "change-address", [] {
                        recompui::open_keyboard("The PS5's address", st.ps5_ip, 15, false, [](const std::string& ip) {
                            st.ps5_ip = ip;
                            dirty = true;
                        });
                    }),
                };
                if (blitz::ps5_beta_available())
                    s.actions.push_back(act("Send Beta (untested)", "send-beta", [] {
                        int a, b, c, d;
                        char extra;
                        if (sscanf(st.ps5_ip.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
                            blitz::post_status("That doesn't look like an address (like 192.168.1.20)");
                            return;
                        }
                        st.ps5_beta = true;
                        start_send();
                    }));
                break;
            }
            case Sent:
                s = base("Sent", st.send_result);  // device_send.cpp says whether it's starting
                s.actions = {act("Done", "done", [] { finish("ps5"); })};
                break;
            case Export:
                // The Export screen as it is, with a lead-in (§3 K). Its own Back is setup's Back.
                blitz::firstrun_ui::hide();
                log("asked export");
                blitz::export_screen_open_for_setup(
                    blitz::season_content() && !original_chosen()
                        ? "Full needs an Expansion Pak (a flash cart can't add memory). Lite fits a plain N64." : "",
                    [] { back(); },
                    [] {
                        log("export start");
                        st.export_started_game = !ultramodern::is_game_started();
                        export_result = 0;
                        go(ExportWait);
                    });
                return;
            case ExportWait:
                s = base("Making your ROM\xE2\x80\xA6", "The game starts for a moment to build it.");
                s.back = nullptr;
                break;
            case ExportDone:
                if (!st.export_ok) {
                    s = base("Couldn't export", st.export_detail);
                    s.actions = {act("Try Again", "try-again", [] { go(Export, false); })};
                    break;
                }
                s = base("Your ROM is saved", (fs::path(st.export_path).parent_path().filename() / fs::path(st.export_path).filename()).string() + "\n\n"
                         "N64 with a flash cart: copy it to the card and load it.\n"
                         "Emulator: open it like any game. RetroArch and other mupen64plus emulators freeze at team select with "
                         "Choose CPU Opponent: use the Lite.");
                s.actions = {act("Done", "done", [] { finish("export"); })};
                if (blitz::platform::file_browser)
                    s.actions.push_back(act("Open Folder", "open-folder", [] { blitz::reveal_in_file_browser(st.export_path); }));
                s.back = nullptr;
                break;
            default:
                return;
        }
        static int last_asked = Idle;
        if (st.step != Downloading && !(st.step == Sending && last_asked == Sending)) log("asked %s", step_names[st.step]);  // not each refresh
        last_asked = st.step;
        launcher_visible(false);
        blitz::firstrun_ui::show(s);
    }

    // ---- Main Menu from Settings (team, 2026-11-15) ------------------------------------------------------------------

    std::atomic<bool> leave_saving{false};

    void show_leave_game() {
        Screen s;
        s.title = "Leave this game?";
        s.body = "Going to the main menu ends this game.";
        s.actions = {
            {"Save and Leave", [] {
                 blitz::firstrun_ui::hide();
                 leave_saving = true;
                 if (!blitz::savestate_save_then([](bool ok) {
                         if (ok) leave_requested = true;
                         else blitz::post_status("Couldn't save the game here; leave again without saving");
                         leave_saving = false;
                     })) {
                     leave_saving = false;
                     leave_requested = true;  // the menus, not a game: nothing to save
                 }
             }},
            {"Leave without Saving", [] {
                 blitz::firstrun_ui::hide();
                 blitz::checkpoint_discard_on_quit();
                 leave_requested = true;
             }},
            {"Cancel", [] { blitz::firstrun_ui::hide(); }},
        };
        s.back = [] { blitz::firstrun_ui::hide(); };
        s.back_button = false;  // Cancel is the on-screen way back
        blitz::firstrun_ui::show(s);
    }

#if defined(BLITZ_PLATFORM_PS5)
    // The PS5 with no ROM (2026-11-26, for ProsperoStore: a store-installed copy arrives without one, and the PS5 never
    // shows setup). One screen over the launcher says how to add it: Blitz Builder from a computer, or the ROM copied
    // to /data/blitzrecomp over FTP. Every 2 s (and on Check Again) that folder is looked at again, as main.cpp does
    // at startup, so a ROM copied in is taken without a restart; then the screen goes and the launcher (Play) is back.
    // Untested on a PS5 (none to run it on): it only reads the folder and calls store_rom, as the startup import does.
    namespace ps5_no_rom {
        int state = 0;                   // 0 not shown yet, 1 up, 2 done (a ROM is stored)
        Uint32 next_look = 0;
        std::atomic<bool> look_now{false};
        std::vector<std::string> tried;  // "name|size|time" of files that weren't the ROM, so each is tried once
        std::string problem;             // the last refused file, for the note

        void show() {
            blitz::firstrun_ui::Screen s;
            // BRANDING: app name, the setup tool's name, PS5 app folder /data/blitzrecomp
            s.title = "Add your NFL Blitz 2000 ROM";
            s.body = "New Blitz needs your own copy of NFL Blitz 2000 for the N64: the USA cartridge, v1.1. No game files "
                     "come with it.\n\n"
                     "From a computer: open New Blitz on your Mac, Windows or Linux computer \xE2\x86\x92 Blitz Builder "
                     "\xE2\x86\x92 PS5. It sends your ROM and your setup. Close New Blitz here first: it starts again "
                     "when the send is done.\n\n"
                     "Or copy the ROM (.z64, .n64, .v64 or a .zip) to /data/blitzrecomp on this PS5 over FTP. New Blitz "
                     "finds it in a few seconds. If it doesn't, start New Blitz again.";
            s.note = problem;
            s.actions = {{"Check Again", [] { look_now = true; }}};
            s.back_button = false;
            blitz::firstrun_ui::show(s);
        }

        // A file in the drop folder that could be the ROM, not tried yet and not still being copied: stored if it's
        // the one. True once a ROM is stored.
        bool look() {
            const fs::path folder = blitz::rom_drop_folder();
            const fs::path stored = stored_rom_path();
            std::vector<fs::path> roms, zips;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(folder, ec)) {
                std::string name = entry.path().filename().string();
                if (name.rfind("._", 0) == 0 || !entry.is_regular_file(ec) || entry.path() == stored) continue;
                std::string ext = entry.path().extension().string();
                for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                if (ext == ".z64" || ext == ".n64" || ext == ".v64") roms.push_back(entry.path());
                else if (ext == ".zip") zips.push_back(entry.path());
            }
            std::sort(roms.begin(), roms.end());
            std::sort(zips.begin(), zips.end());
            for (const auto* list : {&roms, &zips}) {
                for (const fs::path& rom : *list) {
                    std::error_code fe;
                    auto size = fs::file_size(rom, fe);
                    auto when = fs::last_write_time(rom, fe);
                    if (fe) continue;
                    // Changed in the last 3 s: an FTP copy still going. The next look takes it.
                    if (fs::file_time_type::clock::now() - when < std::chrono::seconds(3)) continue;
                    std::string key = rom.filename().string() + "|" + std::to_string((unsigned long long)size) + "|" +
                                      std::to_string((long long)when.time_since_epoch().count());
                    if (std::find(tried.begin(), tried.end(), key) != tried.end()) continue;
                    tried.push_back(key);
                    int kind = 0;
                    std::string message;
                    bool ok = blitz::store_rom(rom, kind, message);
                    log("ps5 no-rom: %s %s", rom.filename().string().c_str(), ok ? "stored" : "not the ROM");
                    if (ok) return true;
                    problem = rom.filename().string() + ": " + message;
                    if (state == 1) show();  // the note changes
                }
            }
            return false;
        }

        // Every UI tick (builder_update), once the launcher is up (the startup import has had its go by then).
        void tick() {
            if (state == 2 || launcher_ready_at == 0 || ultramodern::is_game_started()) return;
            Uint32 now = SDL_GetTicks();
            if (now - launcher_ready_at < 1000) return;
            if (!look_now.exchange(false) && now < next_look) return;
            next_look = now + 2000;
            bool ready = rom_valid();
            if (!ready && look()) {
                ready = true;
                blitz::stored_rom_changes++;  // the launcher's ROM line
            }
            if (ready) {
                if (state == 1) {
                    blitz::firstrun_ui::hide();
                    launcher_visible(true);
                    std::string name = blitz::stored_rom_display_name();
                    blitz::post_status(name.empty() ? std::string("ROM saved") : name + " saved");
                    log("ps5 no-rom: ROM ready, screen closed");
                }
                state = 2;
                return;
            }
            if (state == 0) {
                if (blitz::firstrun_ui::shown()) return;  // another screen is up: after it
                log("ps5 no-rom: no ROM in %s, showing how to add one", blitz::rom_drop_folder().string().c_str());
                launcher_visible(false);  // it takes the D-pad otherwise
                state = 1;
                show();
            }
        }
    }
#endif
}

namespace blitz {

    // main.cpp: the app was started straight into the game (`--game`).
    void builder_autostarted() { autostarted = true; }

    // main.cpp, right after the log opens (line 1); before the settings load, which take builder_done in.
    void firstrun_app_up(const std::filesystem::path& app_folder, const std::string& version) {
        std::error_code ec;
        builder_done_at_start = fs::is_regular_file(app_folder / "builder_done", ec);
#if BLITZ_HAS_ROM_DROP_FOLDER
        if (!builder_done_at_start) {
            fs::path drop = blitz::rom_drop_folder();
            builder_done_at_start = !drop.empty() && fs::is_regular_file(drop / "builder_done", ec);
        }
#endif
        std::string platform = blitz::platform::name;
        for (char& c : platform) c = (char)std::tolower((unsigned char)c);
        log("app up platform=%s build=%s appfolder=%s", platform.c_str(), version.c_str(), app_folder.string().c_str());
    }

    // main.cpp, once the settings are loaded (lines 2 and 3).
    void firstrun_settings_loaded() {
        log_state(false);
        log("waiting for launcher");
    }

    // The one way setup begins (idempotent). trigger: launcher-ready, watchdog, finish-setup-row, builder-row,
    // settings-row, export-row.
    void firstrun_start(const char* trigger, int where = -1) {
        if (running() || blitz::firstrun_ui::shown()) {
            log("trigger=%s: already running", trigger);
            return;
        }
        log("trigger=%s", trigger);
        abandoned = false;
        st = State{};
        st.builder = welcomed() && !device_asks_updates();
        setup_started();
        if (where >= 0) st.where = where;
        // Potato Mode with its ROM (BlitzRomFirst's screen was step 1): still a step of the plan, so the count goes on from
        // it, but already behind.
        if (lean() && !st.builder && rom_valid()) {
            st.rom_shown = st.rom_confirmed = true;
            log("rom ready source=potato-before-setup name=%s", blitz::stored_rom_display_name().c_str());  // BlitzRomFirst's, or kept
        }
        std::vector<int> p = plan();
        // Blitz Builder's N64 row (Add-Ons) goes straight to its path.
        int first = p.empty() ? Idle : p.front();
        if (where >= 0 && first == Where && p.size() > 1) first = p[1];
        if (first == Rom && st.rom_confirmed && p.size() > 1) first = p[1];  // Potato Mode: picked before the game loaded
        if (first == Idle) {
            finish("computer");
            return;
        }
        if (first == Rom && rom_valid() && !rom_needed()) log("skipped rom: stored");
        go(first, false);
    }

    // Blitz Builder is the computer's (the hub model): a phone, handheld or PS5 never runs it.
    void builder_open() {
        if (!blitz::platform::desktop) {
            log("trigger=builder-row: not on this device (the computer's Blitz Builder sets it up)");
            return;
        }
        firstrun_start("builder-row");
    }

    // config.cpp, as it takes in a computer's builder_done (on an iPhone it's inside the app, so it isn't seen sooner).
    void firstrun_builder_done_seen() {
        builder_done_at_start = true;
        log("builder_done: set up from a computer");
    }

    // The launcher's per-frame update (main.cpp on_launcher_update), with its frame count: the launcher's own ready
    // hook, not a timer. Line 4 on the first frame; at frame 30, setup starts when it's needed.
    void firstrun_launcher_frame(int frame) {
        if (frame == 1) {
            launcher_ready_at = SDL_GetTicks();
            log("launcher ready ms=%u", (unsigned)launcher_ready_at);
            log_state(true);
        }
        if (frame == 30 && !abandoned && decide() && !running()) firstrun_start("launcher-ready");
    }

    // The launcher's ROM line (launcher.cpp RomNeededNote), once a ROM is stored.
    std::string firstrun_rom_line() {
        std::time_t t = rom_time();
        std::string started = std::get<std::string>(general().get_option_value("setup_started"));
        bool earlier = t != 0 && !welcomed() && (started.empty() || t < (std::time_t)std::atoll(started.c_str()));
        if (earlier) return "Using a ROM saved earlier (" + human_date(t) + ") \xC2\xB7 Change ROM";
        std::string name = blitz::stored_rom_display_name();
        return "Using your saved ROM" + (name.empty() ? std::string() : " \xC2\xB7 " + name) + " \xC2\xB7 Change ROM";
    }

    // Whether the launcher's Finish Setup row shows (and the launcher shows at all at start: config.cpp).
    bool firstrun_unfinished() { return !welcomed() || device_asks_updates(); }

    void firstrun_finish_setup_row() { firstrun_start("finish-setup-row"); }

    // Settings rows (config.cpp): done on the next tick, with Settings closed first.
    void firstrun_settings_row(int which) { settings_request = which; }

    // export_rom.cpp: an export finished. True when setup was waiting for it (it says how it went, not the prompt).
    bool firstrun_export_finished(bool ok, const std::string& path, const std::string& detail) {
        if (st.step != ExportWait) return false;
        st.export_ok = ok;
        st.export_path = path;
        st.export_detail = detail;
        log("export %s kind=%s", ok ? "ok" : "failed", path.find("Lite") != std::string::npos ? "lite" : path.find("Full") != std::string::npos ? "full"
                                                       : path.find("Original") != std::string::npos ? "original" : "custom");
        export_result = ok ? 1 : 2;
        return true;
    }

#if defined(BLITZ_IOS)
    std::vector<std::filesystem::path> ios_take_opened_files();  // platform_ios.mm
#endif

    // Every UI tick (main.cpp).
    void builder_update() {
        blitz::reset_data_update();  // Reset Data and Restore a Backup's screens (reset_data.cpp)
        blitz::pak_backups_update();  // Controller Pak Backups' screens (pak_backups.cpp)
#if defined(BLITZ_PLATFORM_PS5)
        ps5_no_rom::tick();  // no ROM yet: how to add one
#endif
#if defined(BLITZ_IOS)
        // A ROM opened into the app from another (Files' Share → New Blitz): setup's ROM screen takes it as if Load ROM
        // had picked it and goes on; anywhere else it's stored as Settings → Change ROM would, with a toast.
        for (const fs::path& path : ios_take_opened_files()) {
            if (st.step == Rom || st.step == RomBad) {
                took_rom(path);
                fprintf(stderr, "[rom] opened file %s: %s (setup's ROM screen)\n", path.filename().string().c_str(),
                        rom_pick.load() == 1 ? "stored" : "not stored");
            }
            else {
                int kind = 0;
                std::string message;
                bool ok = blitz::store_rom(path, kind, message);
                fprintf(stderr, "[rom] opened file %s: %s%s%s\n", path.filename().string().c_str(), ok ? "stored" : "not stored",
                        message.empty() ? "" : ": ", message.c_str());
                if (ok) {
                    blitz::stored_rom_changes++;
                    std::string name = blitz::stored_rom_display_name();
                    blitz::post_status((name.empty() ? std::string("ROM saved") : name + " saved") +
                                       (ultramodern::is_game_started() ? ": it plays from the next start" : ""));
                }
                else if (!message.empty()) blitz::post_status(message);
            }
            std::error_code ec;
            fs::remove(path, ec);  // the tmp copy (a stored ROM is kept in the app folder)
        }
#endif
        // Settings rows.
        if (int r = settings_request.exchange(0); r == 4) {
            // Change ROM: the file dialog over Settings, a toast for how it went.
            recompui::file::open_file_dialog([](bool success, const fs::path& path) {
                if (!success) return;
                int kind = 0;
                std::string message;
                if (blitz::store_rom(path, kind, message)) {
                    blitz::stored_rom_changes++;
                    std::string name = blitz::stored_rom_display_name();
                    blitz::post_status((name.empty() ? std::string("ROM saved") : name + " saved") +
                                       (ultramodern::is_game_started() ? ": it plays from the next start" : ""));
                }
                else if (!message.empty()) blitz::post_status(message);
            });
        }
        else if (r == 5) {
            blitz::experience_switch();
        }
        else if (r != 0) {
            recompui::config::close();
            if (r == 1) {
                if (blitz::platform::desktop) firstrun_start("settings-row");
            }
            else if (r == 10) {
                if (running()) {
                    log("abandoned at=%s for %s", step_names[st.step], "pak-backups");
                    st = State{};
                    abandoned = true;
                }
                blitz::pak_backups_open();
            }
            else if (r == 8 || r == 9) {
                // Reset Data or Restore from Settings during setup (Customize has Settings open): setup steps aside, as
                // Exit does (Finish Setup brings it back if the reset is cancelled).
                if (running()) {
                    log("abandoned at=%s for %s", step_names[st.step], r == 8 ? "reset-data" : "restore-backup");
                    st = State{};
                    abandoned = true;
                }
                blitz::reset_data_open(r == 9);
            }
            else if (r == 6) firstrun_start("export-row", ExportTarget);
            else if (r == 7) open_settings("general");
            else if (r == 2) {
                if (ultramodern::is_game_started()) show_leave_game();
                else blitz::show_launcher();
            }
            else if (r == 3) {
                st = State{};
                st.builder = true;
                st.step = ReportView;
                st.history.push_back(Idle);  // Back closes it
                dirty = true;
            }
        }
        if (leave_requested.exchange(false)) {
            log("main menu: leaving the game");
            blitz::leave_to_main_menu();
            return;
        }
        // The watchdog: setup needed, nothing up, 3 s after the launcher showed. Every 3 s, at most 3 times.
        if (launcher_ready_at != 0 && !abandoned && watchdog_fires < 3) {
            Uint32 now = SDL_GetTicks();
            if (now - launcher_ready_at >= 3000 && now >= watchdog_next) {
                watchdog_next = now + 3000;
                if (!running() && decide()) {
                    watchdog_fires++;
                    log("WARNING needed but not started after %u ms welcomed=%d rom=%s", (unsigned)(now - launcher_ready_at),
                        welcomed() ? 1 : 0, rom_state(true));
                    firstrun_start("watchdog");
                }
            }
        }
        // The season download, whether setup is still up or was left (it finishes cleanly in the background).
        if (int d = download_result.load(); d != 0 && (st.step == Downloading || st.step == Idle)) {
            download_result = 0;
            std::string message;
            {
                std::lock_guard lock{mutex};
                message = download_message;
            }
            log("season download %s %s", d == 1 ? "ok" : "failed", d == 1 ? "" : message.c_str());
            if (d == 1) {
                set_game(0u);  // New Blitz, now that its season is in
                set_flag("setup_season_asked", true);
            }
#if BLITZ_MODERN
            blitz::original_settings_visibility();
#endif
            if (st.step == Downloading) {
                if (d == 1) {
                    st.step = Season;  // what follows Season
                    next();
                }
                else {
                    st.offline = message.find("reach") != std::string::npos;
                    go(SeasonFailed, false);
                }
            }
        }
        if (st.step == Downloading && SDL_GetTicks() - download_started >= 10000 && SDL_GetTicks() - download_started < 10100) dirty = true;  // Cancel shows
        // A picked ROM.
        if (int r = rom_pick.exchange(0); r != 0 && (st.step == Rom || st.step == RomBad)) {
            int kind;
            std::string message;
            {
                std::lock_guard lock{mutex};
                kind = rom_pick_kind;
                message = rom_pick_message;
            }
            if (r == 1) {
                st.rom_tries = 0;
                st.rom_confirmed = true;
                blitz::stored_rom_changes++;
                log("rom ready source=picked name=%s", blitz::stored_rom_display_name().c_str());
                st.step = Rom;  // what follows the ROM
                next();
            }
            else {
                st.rom_tries++;
                log("rom rejected reason=%s", kind == 2 ? "damaged" : kind == 3 ? "unreadable" : "wrong-game");
                st.rom_problem = message;
                go(RomBad, st.step == Rom);
            }
        }
        // Customize: Settings is open on Rules; setup goes on when it closes.
        if (st.step == CustomizeWait) {
            static bool seen_open = false;
            if (settings_open()) seen_open = true;
            else if (seen_open || dirty) {
                seen_open = false;
                if (!dirty) {
                    st.step = Setup;
                    next();
                }
            }
        }
        // A device send or save.
        if (st.step == Sending) {
            static Uint32 progress_checked = 0;
            if (st.where == Ps5Target && SDL_GetTicks() - progress_checked >= 1000) {  // the progress line, once a second
                progress_checked = SDL_GetTicks();
                if (blitz::device_send_progress() != st.send_progress) dirty = true;
            }
            int s = blitz::device_send_state();
            if (s == 2 || s == 3) {
                st.send_result = blitz::device_send_message();
                st.send_problem = st.phone == AndroidUsb && s == 3 ? blitz::device_send_problem() : std::string();
                std::string problem = st.send_problem.empty() ? "" : " problem=" + st.send_problem;
                log("send %s target=%s%s %s", s == 2 ? "ok" : "failed", send_target().c_str(), problem.c_str(), st.send_result.c_str());
                if (s == 2) {
                    st.step = step_of(Sending);
                    next();
                }
                else go(SendFailed, false);
            }
        }
        if (st.step == ExportWait && export_result.load() != 0 && !recompui::is_prompt_open()) {
            export_result = 0;
            st.step = Export;
            next();
        }
        if (!dirty.exchange(false)) return;
        if (int p = pending.exchange(0); p != 0) {
            if (p == 1) do_exit();
            else do_finish(pending_target.c_str());
            return;
        }
        if (st.step == CustomizeWait) {
            blitz::firstrun_ui::hide();
            log("asked customize");
            open_settings("rules");
            return;
        }
        if (st.step == Idle) {
            blitz::firstrun_ui::hide();
            return;
        }
        if (st.step == ReportView && st.history.size() == 1 && st.history.front() == Idle) {
            // From Settings: the view alone, Back to Settings.
            Screen s;
            bool sent = false;
            s.title = "What a crash report sends";
            s.long_text = blitz::crash_report_preview(sent);
            s.body = sent ? "The last report this device sent, word for word."
                          : "Nothing has been sent from here. This is what a report would hold, made from this run's log.";
            auto close = [] {
                st = State{};
                blitz::firstrun_ui::hide();
                settings_request = 7;  // Settings again, on the next tick (not from this screen's own button)
            };
            s.back = close;
            blitz::firstrun_ui::show(s);
            return;
        }
        show();
    }
}
