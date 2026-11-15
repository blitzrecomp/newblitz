// Blitz Builder (team, 2026-10-25): the first start's setup, in the launcher, then out of the way. Once a ROM is stored
// (Load ROM, or a phone's or the PS5's drop folder) and setup has never run, three questions, one at a time:
//   1. Which Blitz: New Blitz (today's teams) or Original 1999 (Settings → Season → Game; adv_mode.cpp). Where the
//      2026 Season isn't here yet (the public app: no licensed content inside, 2026-11-13), the question is "Add the 2026
//      season?" instead: Add It downloads and installs the season pak (season_pak.cpp) and plays New Blitz, then asks
//      whether to keep rosters up to date (Settings → Teams → Roster Updates); Just the 1999 Game plays Original 1999.
//      A download that fails (offline, the server gone, a bad checksum) says so and plays the 1999 game; the season
//      can be added later from Add-Ons;
//   2. the recommended setup, or Customize (Settings opens on the Rules tab);
//   3. where to play (a computer only): here; on an N64 or an emulator (Export ROM); or, when the app carries the
//      device kits, sent to a PS5, an iPhone or an Android device with the ROM and this setup (device_send.cpp). The
//      public app can't send to an iPhone (Apple installs only what the player signs): Save the iPhone app (.ipa) puts
//      the unsigned app, the ROM and the setup in Downloads, and says how to install it with the player's Apple ID.
// Recommended and here starts the game straight away. The launcher's Blitz Builder runs it again (another device, an
// export). Not when the app was started straight into the game (`--game`: dev runs and the live harness).
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <string>

#include "blitz_config.h"
#include "blitz_platform.h"
#include "../game/season_pak.h"
#include "librecomp/game.hpp"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "ultramodern/ultramodern.hpp"

namespace blitz {
    void export_rom_dialog();  // export_rom.cpp
    bool device_kits_available();  // device_send.cpp
    bool iphone_kit_available();
    bool iphone_ipa_available();
    void save_iphone_ipa();
    std::string ps5_last_address();
    void send_to_ps5(const std::string& ip);
    void send_to_iphone();
    void send_to_android();
    void rosters_check_again();  // rosters_live.cpp
    void original_settings_visibility();  // adv_mode.cpp: the settings that need the 2026 Season show once it's in
}

namespace {
    enum Step { Idle, AskGame, Downloading, DownloadFailed, AskLive, AskSetup, AskWhere, AskElsewhere, AskDevice, AskPhone, AskPs5,
                IpaSaved, Finish, Done };
    std::atomic<int> step{Idle};
    int shown = Idle;       // the step whose prompt is open (the UI thread only)
    bool customize = false;
    enum Where { Here, Export, Ps5, IPhone, Android, IPhoneIpa };
    int where = Here;
    std::string ps5_ip;
    bool autostarted = false;
    // The season download (Add It): 0 running, 1 installed, 2 failed, with what to say.
    std::atomic<int> download_result{0};
    std::mutex download_mutex;
    std::string download_message;

    recomp::config::Config& general() { return recompui::config::get_general_config(); }

    void open_settings(const char* tab) {
        recompui::update_game_mod_id(blitz::mod_game_id);
        recompui::config::set_tab(tab);
        recompui::hide_all_contexts();
        recompui::config::open();
    }

    void start_game() {
        std::u8string id = blitz::game_id;
        if (!recomp::is_rom_valid(id)) return;
        recompui::update_game_mod_id(blitz::mod_game_id);
        recomp::start_game(id, {});
        recompui::hide_all_contexts();
    }

    // A value set here isn't written until saved (Settings saves on its own changes).
    void set_game(uint32_t v) {
#if BLITZ_MODERN
        recomp::config::Config& season = recompui::config::get_config("season");
        season.update_option_value("game", v);
        season.save_config();
#else
        (void)v;
#endif
    }

    bool original_chosen() {
#if BLITZ_MODERN
        return std::get<uint32_t>(recompui::config::get_config("season").get_option_value("game")) == 1;
#else
        return true;
#endif
    }

    void ask(int s) {
        using recompui::ButtonStyle;
        switch (s) {
            case AskGame:
#if BLITZ_MODERN
                if (!blitz::season_content()) {
                    recompui::open_choice_prompt(
                        // BRANDING: Blitz Builder's season question
                        "Add the 2026 season?",
                        "Today's 32 teams, rosters, uniforms and team art, as a small download from the New Blitz "
                        "server.\n\n"
                        "Or play NFL Blitz 2000 exactly as it shipped in 1999.\n\n"
                        "Either way you get widescreen, save states and the rest. Add the season any time from Add-Ons.",
                        "Add It", "Just the 1999 Game",
                        [] {
                            set_game(0u);
                            download_result = 0;
                            bool started = blitz::pak_download_start([](bool ok, const std::string& message) {
                                {
                                    std::lock_guard lock{download_mutex};
                                    download_message = message;
                                }
                                download_result = ok ? 1 : 2;
                            });
                            if (!started) download_result = 2;
                            step = Downloading;
                        },
                        [] {
                            set_game(1u);
                            step = AskSetup;
                        },
                        ButtonStyle::Primary, ButtonStyle::Secondary, false);
                    break;
                }
#endif
                recompui::open_choice_prompt(
                    // BRANDING: Blitz Builder's mode question names the app
                    "Which Blitz?",
                    "New Blitz: today's 32 teams, rosters and uniforms, kept up to date.\n\n"
                    "Original 1999: NFL Blitz 2000 exactly as it shipped.\n\n"
                    "Either way you get widescreen, save states and the rest. Switch any time in Settings → Teams.",
                    "New Blitz", "Original 1999",
                    [] {
                        set_game(0u);
                        step = AskSetup;
                    },
                    [] {
                        set_game(1u);
                        step = AskSetup;
                    },
                    ButtonStyle::Primary, ButtonStyle::Secondary, false);
                break;
            case Downloading:
                recompui::open_notification("Getting the 2026 Season…", "One moment: the season is downloading and going in.");
                break;
            case DownloadFailed: {
                std::string why;
                {
                    std::lock_guard lock{download_mutex};
                    why = download_message;
                }
                recompui::open_info_prompt(
                    "No 2026 Season for now",
                    (why.empty() ? std::string("The season couldn't be downloaded.") : why) +
                        "\n\nYou'll play the 1999 game for now. Add the season any time from Add-Ons.",
                    "OK", [] { step = AskSetup; }, ButtonStyle::Secondary);
                break;
            }
            case AskLive:
                recompui::open_choice_prompt(
                    "Keep rosters up to date?",
                    "The 2026 Season is in. New lineups (injuries, signings, depth charts) can download when the app "
                    "starts, on Thursdays and Saturdays during the season.\n\n"
                    "Change it any time in Settings → Teams → Roster Updates.",
                    "Keep Them Updated", "No Thanks",
                    [] {
                        blitz::set_live_rosters(true);
#if BLITZ_MODERN
                        blitz::rosters_check_again();
#endif
                        step = AskSetup;
                    },
                    [] {
                        blitz::set_live_rosters(false);
                        step = AskSetup;
                    },
                    ButtonStyle::Primary, ButtonStyle::Secondary, false);
                break;
            case AskSetup:
                recompui::open_choice_prompt(
                    "Set it up for you?",
                    std::string(original_chosen() ? "The recommended setup plays the original game as it was, with hints the first "
                                                    "time you reach each screen."
                                                  : !blitz::season_content()
                                                  ? "The recommended setup plays great out of the box: the game's own controls, and "
                                                    "hints the first time you reach each screen."
                                                  : "The recommended setup plays great out of the box: this week's rosters, the "
                                                    "game's own controls, and hints the first time you reach each screen.") +
                        "\n\nCustomize opens the house rules (Madden-style Receiver Buttons, the play clock, realism ...) "
                        "and every other setting.",
                    "Recommended", "Customize",
                    [] {
                        customize = false;
                        step = blitz::export_available ? AskWhere : Finish;
                    },
                    [] {
                        customize = true;
                        step = blitz::export_available ? AskWhere : Finish;
                    },
                    ButtonStyle::Primary, ButtonStyle::Secondary, false);
                break;
            case AskWhere:
                if (blitz::device_kits_available()) {
                    recompui::open_choice_prompt(
                        "Where do you want to play?",
                        "Right here, or somewhere else: a PS5, a phone, or a Nintendo 64 or an emulator.",
                        "Here", "Somewhere else",
                        [] { where = Here; step = Finish; },
                        [] { step = AskElsewhere; },
                        ButtonStyle::Primary, ButtonStyle::Secondary, false);
                    break;
                }
                recompui::open_choice_prompt(
                    "Where do you want to play?",
                    "Right here, or on a Nintendo 64 (with a flash cart) or an emulator: a ROM with your setup built in.",
                    "Here", "N64 or Emulator",
                    [] { where = Here; step = Finish; },
                    [] { where = Export; step = Finish; },
                    ButtonStyle::Primary, ButtonStyle::Secondary, false);
                break;
            case AskElsewhere:
                recompui::open_choice_prompt(
                    "Somewhere else",
                    // BRANDING: app name in Builder's send text
                    "Send New Blitz to a console or a phone, with your ROM and this setup; or make a ROM for a Nintendo 64 "
                    "(with a flash cart) or an emulator.",
                    "A console or phone", "N64 or Emulator",
                    [] { step = AskDevice; },
                    [] { where = Export; step = Finish; },
                    ButtonStyle::Primary, ButtonStyle::Secondary, false);
                break;
            case AskDevice:
                recompui::open_choice_prompt(
                    "Which one?",
                    "A PS5 needs etaHEN running and its address. A phone connects to this computer (USB, or paired).",
                    "PS5", "Phone",
                    [] { step = AskPs5; },
                    [] { step = AskPhone; },
                    ButtonStyle::Primary, ButtonStyle::Secondary, false);
                break;
            case AskPhone:
                if (!blitz::iphone_kit_available() && blitz::iphone_ipa_available()) {
                    // The public app: the iPhone app is saved for the player to sign, on any computer.
                    recompui::open_choice_prompt(
                        "Which phone?",
                        "An Android device installs through adb (USB debugging on).\n\n"
                        "An iPhone or iPad installs apps you sign with your own Apple ID: New Blitz saves the iPhone app "
                        "(.ipa), your ROM and this setup to Downloads, and says how to install them with AltStore, "
                        "SideStore or Sideloadly.",
                        "Save the iPhone app (.ipa)", "Android",
                        [] { where = IPhoneIpa; step = Finish; },
                        [] { where = Android; step = Finish; },
                        ButtonStyle::Primary, ButtonStyle::Secondary, false);
                    break;
                }
                recompui::open_choice_prompt(
                    "Which phone?",
                    blitz::iphone_kit_available()
                        ? "An iPhone installs through Xcode on this Mac; an Android device through adb (USB debugging on)."
                        : "An Android device installs through adb (USB debugging on). The iPhone app isn't included: build "
                          "and install it with Xcode on your Mac (make ios-modern, then tools/ios/install.sh modern).",
                    "iPhone", "Android",
                    [] { where = IPhone; step = Finish; },
                    [] { where = Android; step = Finish; },
                    ButtonStyle::Primary, ButtonStyle::Secondary, false);
                break;
            case IpaSaved:
                recompui::open_info_prompt(
                    // BRANDING: the iPhone folder's name in Downloads
                    "Saved in Downloads",
                    "Downloads → New Blitz for iPhone holds NewBlitz.ipa, a README, and a folder to copy to the phone.\n\n"
                    "1. Install NewBlitz.ipa with AltStore, SideStore or Sideloadly, signed with your own Apple ID (a "
                    "free one works for 7 days at a time; re-sign to keep playing).\n"
                    "2. On the iPhone, turn on Developer Mode and trust your Apple ID (Settings → General → VPN & "
                    "Device Management).\n"
                    "3. Copy what's in \"Copy to the iPhone\" into New Blitz's folder: Files → On My iPhone → New "
                    "Blitz, or your iPhone's Files in Finder or the Apple Devices app. New Blitz starts with your ROM "
                    "and this setup.",
                    "OK", [] { step = Done; }, ButtonStyle::Primary);
                break;
            case AskPs5:
                recompui::open_keyboard("The PS5's address", blitz::ps5_last_address(), 15, false, [](const std::string& ip) {
                    ps5_ip = ip;
                    where = Ps5;
                    step = Finish;
                });
                break;
            default:
                break;
        }
    }

    // The launcher steps aside while the questions are up: shown under a prompt it takes the D-pad from it (focus
    // left the prompt's buttons on the first press, 2026-10-25).
    void launcher_visible(bool on) {
        recompui::ContextId launcher = recompui::get_launcher_context_id();
        if (launcher == recompui::ContextId::null()) return;
        if (on && !recompui::is_context_shown(launcher)) recompui::show_context(launcher, "");
        if (!on && recompui::is_context_shown(launcher)) recompui::hide_context(launcher);
    }

    void finish() {
        general().update_option_value("welcomed", true);
        general().save_config();
        if (!customize && where == Here) {
            start_game();
            fprintf(stderr, "[builder] done: %s, recommended, here\n", original_chosen() ? "Original 1999" : "New Blitz");
            return;
        }
        launcher_visible(true);
        static const char* names[] = {"here", "export", "PS5", "iPhone", "Android", "iPhone (.ipa)"};
        fprintf(stderr, "[builder] done: %s, %s, %s\n", original_chosen() ? "Original 1999" : "New Blitz",
                customize ? "customize" : "recommended", names[where]);
        // A device gets this computer's setup: Customize first means the settings, then send from Blitz Builder again.
        if (where == Ps5 && !ps5_ip.empty()) blitz::send_to_ps5(ps5_ip);
        else if (where == IPhone) blitz::send_to_iphone();
        else if (where == Android) blitz::send_to_android();
        else if (where == IPhoneIpa) {
            blitz::save_iphone_ipa();
            launcher_visible(false);
            step = IpaSaved;  // how to install it, over the launcher (builder_update)
            return;
        }
        if (where == Export) blitz::export_rom_dialog();
        else if (customize) open_settings("rules");
    }
}

namespace blitz {
    // main.cpp: the app was started straight into the game (`--game`).
    void builder_autostarted() { autostarted = true; }

    // The launcher's Blitz Builder.
    void builder_open() {
        customize = false;
        where = Here;
        step = AskGame;
    }

    // Every UI tick (main.cpp).
    void builder_update() {
        if (ultramodern::is_game_started()) return;
        // Only once the launcher has come up: before then the UI (its prompt among it) may not exist yet. While the
        // questions are up the launcher is hidden (launcher_visible).
        recompui::ContextId launcher = recompui::get_launcher_context_id();
        if (step.load() == Idle && (launcher == recompui::ContextId::null() || !recompui::is_context_shown(launcher))) return;
        static bool checked = false;
        // About 1 GB of RAM skips the launcher (BlitzActivity): its game starts underneath, and the questions would stay up over it.
        if (!checked && !autostarted && getenv("BLITZ_LEAN") == nullptr) {
            std::u8string id = blitz::game_id;
            if (recomp::is_rom_valid(id)) {  // a ROM is in (it may arrive while the launcher shows: Load ROM)
                checked = true;
                if (!std::get<bool>(general().get_option_value("welcomed"))) builder_open();
            }
        }
        int s = step.load();
        // The season download ended under its "Getting the 2026 Season…" notice: that closes, the next question opens.
        if (s == Downloading && shown == Downloading && download_result.load() != 0) {
            fprintf(stderr, "[builder] the 2026 Season: %s\n", download_result.load() == 1 ? "added" : "not added");
            recompui::close_prompt();
#if BLITZ_MODERN
            blitz::original_settings_visibility();
#endif
            if (download_result.load() == 1) step = AskLive;
            else {
                set_game(1u);  // the 1999 game for now
                step = DownloadFailed;
            }
            return;
        }
        if (s == AskPs5 && shown == AskPs5 && !recompui::is_keyboard_open() && !recompui::is_prompt_open()) {
            step = Idle;  // the address keyboard was cancelled: back to the launcher
            shown = Idle;
            launcher_visible(true);
            return;
        }
        if (s == Done && !recompui::is_prompt_open()) {  // the last prompt closed: back to the launcher
            step = Idle;
            shown = Idle;
            launcher_visible(true);
            return;
        }
        if (s == Idle || s == shown || recompui::is_prompt_open() || recompui::is_keyboard_open()) return;
        shown = s;
        if (s == Finish) {
            step = Idle;
            shown = Idle;
            finish();
            return;
        }
#if !BLITZ_MODERN
        if (s == AskGame) {  // one game here
            step = AskSetup;
            return;
        }
#endif
        launcher_visible(false);
        ask(s);
    }
}
