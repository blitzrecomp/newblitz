// Settings tabs. M2 uses the runtime's stock General/Graphics/Controls/Sound tabs;
// Blitz-specific options (M4) get added here.
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "blitz_config.h"
#include "recompui/config.h"
#include "SDL2/SDL.h"
#include "recompui/recompui.h"
#include "ultramodern/config.hpp"
#include "recompinput/recompinput.h"
#include "util/file.h"
#include "blitz_host.h"
#include "blitz_platform.h"
#include "../main/os_shell.h"
#include "miniz.h"
#include <ctime>

extern "C" volatile int blitz_ds_screen_mode;  // tall_view.cpp

extern "C" void blitz_rt64_render_to_ram(int mode);  // rt64_raster_shader_cache.cpp

#if defined(__ANDROID__)
extern double blitz_2x_scale;  // rt64_render_context.cpp
extern "C" void plume_narrow_barriers(int on);  // Plume
#endif
namespace blitz {
    // Configs live in a growing list: look them up when read, never keep a reference.
    bool player1_rumble() {
        return std::get<bool>(recompui::config::get_general_config().get_option_value("p1_rumble"));
    }

    // Settings → General → Redo Play and Save State Buttons (savestate.cpp, controllers.cpp).
    // Settings → General → Button Icons: 0 each player's own controller, 1 PlayStation, 2 Xbox, 3 Switch,
    // 4 N64 (the game's own). Stored as "button_icon_mode": the earlier "button_icons" (PlayStation / Xbox /
    // Match Player 1) saved its default, which would otherwise hide the new one.
    int button_icons() {
        return (int)std::get<uint32_t>(recompui::config::get_general_config().get_option_value("button_icon_mode"));
    }

    bool redo_play_enabled() {
        // Potato Mode keeps the last play only and no menu step-back (savestate.cpp): on an RG DS the step-back's snapshots
        // (8 MB at every confirm, 11 of them) pushed the game out to swap (team, 2026-11-02). Below 800 MB, none at all.
        if (getenv("BLITZ_LEAN") != nullptr && SDL_GetSystemRAM() < 800) return false;
        return std::get<bool>(recompui::config::get_general_config().get_option_value("redo_play"));
    }

    bool savestate_buttons() {
        return std::get<bool>(recompui::config::get_general_config().get_option_value("savestate_buttons"));
    }

    // Settings → General → Fullscreen Button: 0 off, 1 L3 + R3, 2 Home (PS) button.
    int fullscreen_button() {
        return (int)std::get<uint32_t>(recompui::config::get_general_config().get_option_value("fullscreen_button"));
    }

    bool skip_launcher() {
        if (getenv("BLITZ_LEAN") != nullptr) return true;  // about 1 GB of RAM: straight into the game (BlitzActivity)
        // Launcher at Start off, once Blitz Builder's first setup is done (it runs in the launcher).
        recomp::config::Config& g = recompui::config::get_general_config();
        return !std::get<bool>(g.get_option_value("launcher_at_start")) && std::get<bool>(g.get_option_value("welcomed"));
    }
}

extern "C" int blitz_skip_rumble_screen(void) {
    if (getenv("BLITZ_LEAN") != nullptr) return 1;
    return std::get<bool>(recompui::config::get_general_config().get_option_value("skip_rumble_screen")) ? 1 : 0;
}

extern "C" int blitz_skip_intros(void) {
    if (getenv("BLITZ_LEAN") != nullptr) return 1;
    return std::get<bool>(recompui::config::get_general_config().get_option_value("skip_intros")) ? 1 : 0;
}

namespace {
    // Settings → General → TV Mode: full screen, widescreen and the 16:9 HUD in one switch, for playing
    // on a TV (an HDMI cable, or AirPlay to an Apple TV). Written into the Graphics settings, so it
    // lasts; switching it off goes back to a window and leaves the rest.
    void apply_tv_mode(bool on) {
        using namespace ultramodern::renderer;
        recomp::config::Config& g = recompui::config::get_config(recompui::config::graphics::id);
        namespace opt = recompui::config::graphics::options;
        g.set_option_value(opt::wm_option, (uint32_t)(on ? WindowMode::Fullscreen : WindowMode::Windowed));
        if (on) {
            g.set_option_value(opt::ar_option, (uint32_t)AspectRatio::Expand);
            g.set_option_value(opt::hr_option, (uint32_t)HUDRatioMode::Clamp16x9);
        }
        g.save_config();
        GraphicsConfig c = get_graphics_config();
        c.wm_option = on ? WindowMode::Fullscreen : WindowMode::Windowed;
        if (on) {
            c.ar_option = AspectRatio::Expand;
            c.hr_option = HUDRatioMode::Clamp16x9;
        }
        set_graphics_config(c);
    }
}

namespace blitz {
    void post_status(const std::string& msg);  // notices.cpp: a toast
    void show_development(bool on);            // below
    // A second screen has been seen this run (ds_view.cpp, from the host's surface, whatever Two Screens says).
    std::atomic<bool> second_screen_seen{false};
    void ds_set_swapped(bool on);  // ds_view.cpp
    void host_send_logs(const std::filesystem::path& zip);  // platform_android.cpp
    void set_clipboard_later(const std::string& text);      // controllers.cpp: on the main thread
    void host_set_swap_screens(bool swap);
    void host_set_potato(bool on);
    void host_set_potato_two_screens(bool on);
    void tall_set(const std::string& name, float value);  // tall_view.cpp
    std::atomic<bool> ds_screens_step{false};
    void ds_screens_next() { ds_screens_step = true; }  // controllers.cpp, the game thread: done in ds_screens_update
    // Every UI tick (ds_view.cpp).
    void ds_screens_update() {
        if (!ds_screens_step.exchange(false)) return;
        static const char* names[3] = {"Both Screens", "Game + Companion", "Stylus"};
        recomp::config::Config& g = recompui::config::get_general_config();
        uint32_t next = (std::get<uint32_t>(g.get_option_value("ds_screens")) + 1) % 3;
        g.update_option_value("ds_screens", next);
        g.save_config();
        blitz_ds_screen_mode = (int)next;
        blitz::post_status(names[next]);
    }
    // Camera Zoom Out's choices, in the order of its options (ds_zoom): the factor on the game's own distance.
    float ds_zoom_for(uint32_t choice) {
        static const float zoom[] = {1.0f, 1.05f, 1.10f, 1.125f, 1.15f, 1.20f, 1.25f, 1.30f};
        return zoom[std::min<uint32_t>(choice, uint32_t(sizeof(zoom) / sizeof(zoom[0])) - 1)];
    }
    // The host has a second screen (ds_view.cpp; called again for each frame it's there): Two Screens and Swap Screens
    // show, even while Two Screens is off, so it can be turned back on.
    void ds_second_seen() {
        if (second_screen_seen.exchange(true)) return;
        show_development(std::get<bool>(recompui::config::get_general_config().get_option_value(recompui::config::general::options::debug_mode)));
    }
    // Two screens found (ds_view.cpp): their settings show, and take hold.
    void ds_settings_found() {
        recomp::config::Config& g = recompui::config::get_general_config();
        g.update_option_hidden("ds_tilt", false);
        g.update_option_hidden("ds_zoom", false);
        g.update_option_hidden("ds_screens", false);
        blitz_ds_screen_mode = (int)std::get<uint32_t>(g.get_option_value("ds_screens"));
        tall_set("tilt", 0.40f + 0.05f * (float)std::get<uint32_t>(g.get_option_value("ds_tilt")));
        tall_set("zoom", ds_zoom_for(std::get<uint32_t>(g.get_option_value("ds_zoom"))));
    }
    bool potato_two_screens() {
#if defined(__ANDROID__)
        // A run recipe (BlitzRecipe.java) says first in all of these: for this run, and nothing of it is saved.
        if (const char* recipe = getenv("BLITZ_RECIPE_TWO")) return recipe[0] == '1';
        return std::get<bool>(recompui::config::get_general_config().get_option_value("potato_two_screens"));
#else
        return true;
#endif
    }

#if defined(__ANDROID__)
    static int frame_copy_for(uint32_t choice) {
        const char* recipe = getenv("BLITZ_RECIPE_FRAME_COPY");
        return recipe != nullptr ? atoi(recipe) : (int)choice;
    }
    // Two-Screen Sharpness: 1 is the original resolution (hold_caps, ds_view.cpp), and the rest 2x at that scale (blitz_2x_scale).
    static double sharpness_scale_for(uint32_t choice) {
        const char* recipe = getenv("BLITZ_RECIPE_SCALE");
        return recipe != nullptr ? atof(recipe) : 1.0 + 0.25 * (double)choice;
    }
    // What 2x draws at. Sharpness 1x is the original resolution, which hold_caps chooses by itself: taken as a scale, it would have
    // a one-screen run in Potato Mode logging 2x and drawing 1x.
    static double two_x_scale(double sharpness) { return sharpness > 1.0 ? sharpness : 2.0; }
    static bool narrow_waits_for(uint32_t choice) {
        if (getenv("BLITZ_SAFE_START") != nullptr) return false;  // whatever's saved or posted (BlitzActivity.safeStartCheck)
        const char* recipe = getenv("BLITZ_RECIPE_BARRIERS");
        return recipe != nullptr ? strcmp(recipe, "narrow") == 0 : choice == 0;
    }
    double potato_scale() {
        return sharpness_scale_for(std::get<uint32_t>(recompui::config::get_general_config().get_option_value("potato_two_sharpness")));
    }
    bool potato_narrow_waits() {
        return narrow_waits_for(std::get<uint32_t>(recompui::config::get_general_config().get_option_value("potato_waits")));
    }
#endif

    int potato_frame_copy() {
#if defined(__ANDROID__)
        return frame_copy_for(std::get<uint32_t>(recompui::config::get_general_config().get_option_value("potato_frame_copy")));
#else
        return 1;
#endif
    }

    bool two_screens_setting() {
        if (getenv("BLITZ_LEAN") != nullptr) return potato_two_screens();
        return std::get<bool>(recompui::config::get_general_config().get_option_value("two_screens"));
    }
    void hints_reset();                       // hints.cpp
    std::string stored_rom_display_name();     // known_roms.cpp
#if BLITZ_MODERN
    std::string active_pack_id();              // content_pack_tp.cpp
    std::string rosters_status();              // rosters_live.cpp: which roster day this device has
#endif

    // Runs a menu button's work on its own thread. Menu callbacks run holding the UI lock, and debug_info asks SDL
    // for the controllers (its joystick lock), while SDL's event thread holds that lock and waits on the UI one
    // (tap_watch): on the RG DS, Save Logs froze the game that way. One at a time; a second press while one runs
    // is dropped.
    void off_ui_thread(std::function<void()> work) {
        static std::atomic<bool> busy{false};
        if (busy.exchange(true)) return;
        std::thread([work = std::move(work)] {
            work();
            busy = false;
        }).detach();
    }

    // The Development rows (the team's settings pass): for chasing problems, not for playing. Shown by seven presses of About or
    // L2 + R2 held 3 s in Settings, and they stay shown (debug_mode, general.json) until hidden the same way.
    void show_development(bool on) {
        recomp::config::Config& g = recompui::config::get_general_config();
        const bool second = second_screen_seen.load();
        const bool lean = getenv("BLITZ_LEAN") != nullptr;
#if defined(__ANDROID__)
        const bool android = true;
#else
        const bool android = false;
#endif
        for (const char* id : {"fps_counter", "copy_debug_info", "experimental_resume"}) g.update_option_hidden(id, !on);
        for (const char* id : {"potato_two_sharpness", "potato_waits", "potato_frame_copy"}) g.update_option_hidden(id, !(on && lean));
        // Two Screens and Swap Screens: an Android device's with a second screen (a phone with one has no use for them;
        // Potato Mode has its own Two Screens row); the Mac's test windows.
        g.update_option_hidden("two_screens", !((android && !lean && second) || on));
        g.update_option_hidden("swap_screens", !((android && second) || on));
        recomp::config::Config& gfx = recompui::config::get_graphics_config();
        gfx.update_option_hidden(recompui::config::graphics::options::api_option, !on);
        gfx.update_option_hidden(recompui::config::graphics::options::hpfb_option, !on);
    }

    void toggle_development() {
        recomp::config::Config& g = recompui::config::get_general_config();
        const bool on = !std::get<bool>(g.get_option_value(recompui::config::general::options::debug_mode));
        g.update_option_value(recompui::config::general::options::debug_mode, on);
        g.save_config();
        show_development(on);
        post_status(on ? "Development settings shown" : "Development settings hidden");
        fprintf(stderr, "[settings] development %s\n", on ? "shown" : "hidden");
    }

    // Settings → General → Copy Debug Info: what a bug report needs, on the clipboard.
    std::string debug_info() {
        auto sys = [](const char* name) {
#if !defined(__APPLE__)
            (void)name;
            return std::string("?");  // sysctl is the Mac's (and iPhone's); elsewhere the OS and CPU lines come later
#else
            char buf[256] = {};
            size_t len = sizeof(buf) - 1;
            return sysctlbyname(name, buf, &len, nullptr, 0) == 0 ? std::string(buf) : std::string("?");
#endif
        };
        std::string out;
#if BLITZ_MODERN
        // BRANDING: Copy Debug Info's first line
        out += "New Blitz";
#else
        out += "Classic Blitz";
#endif
        out += " " BLITZ_VERSION " (build " BLITZ_BUILD ", " BLITZ_COMMIT ")\n";
        out += "macOS " + sys("kern.osproductversion") + " (" + sys("kern.osversion") + "), " + sys("hw.model") + ", " + sys("machdep.cpu.brand_string") + "\n";
        std::string rom = stored_rom_display_name();
        out += "ROM: " + (rom.empty() ? std::string("not a known release (or none loaded)") : rom) + "\n";
#if BLITZ_MODERN
        std::string pack = active_pack_id();
        out += "Rosters: " + (pack.empty() ? std::string("the ROM's own") : pack) + "\n";
        out += "Roster updates: " + blitz::rosters_status() + "\n";
#endif
        out += "Controllers:";
        int pads = 0;
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (!SDL_IsGameController(i)) continue;
            const char* name = SDL_GameControllerNameForIndex(i);
            out += std::string(pads++ ? ", " : " ") + (name ? name : "unknown");
        }
        out += pads ? "\n" : " none\n";
        // The home folder as ~: a bug report is pasted in public, and the path would carry the player's account name.
        std::string folder = recompui::file::get_app_folder_path().string();
        for (const char* var : {"HOME", "USERPROFILE"}) {
            const char* home = getenv(var);
            size_t n = home ? strlen(home) : 0;
            if (n > 1 && folder.compare(0, n, home) == 0) {
                folder = "~" + folder.substr(n);
                break;
            }
        }
        out += "App folder: " + folder + "\nLog: " + folder + "/blitz.log (attach it to the report)\n";
        return out;
    }

    // Settings → General → Save Logs: this run's log and the last, any crash dumps, the debug info and the settings,
    // in one zip where the player can reach it: Downloads on a computer; on a phone or console the folder the Files
    // app, a file manager or FTP opens (blitz_host.h rom_drop_folder). The zip's path, or empty if it failed.
    std::filesystem::path save_logs() {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path app = recompui::file::get_app_folder_path();
        fs::path dir = blitz::rom_drop_folder();
        if (dir.empty()) dir = downloads_folder();
        fs::create_directories(dir, ec);
        char stamp[32] = {};
        std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", std::localtime(&now));
        fs::path zip_path = dir / (std::string("Blitz logs ") + stamp + ".zip");
        mz_zip_archive zip{};
        if (!mz_zip_writer_init_file(&zip, zip_path.string().c_str(), 0)) return {};
        bool ok = true;
        auto add = [&](const fs::path& file, const std::string& name) {
            if (fs::is_regular_file(file, ec) && !mz_zip_writer_add_file(&zip, name.c_str(), file.string().c_str(), nullptr, 0, MZ_DEFAULT_COMPRESSION))
                ok = false;
        };
        std::string info = debug_info();
        ok = mz_zip_writer_add_mem(&zip, "debug info.txt", info.data(), info.size(), MZ_DEFAULT_COMPRESSION) && ok;
        for (const char* f : {"blitz.log", "blitz.prev.log", "blitz.early.log", "plays.log"}) add(app / f, f);
        for (fs::directory_iterator it{app, ec}, end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() == ".json") add(it->path(), "settings/" + it->path().filename().string());
        }
        for (fs::directory_iterator it{app / "dumps", ec}, end; !ec && it != end; it.increment(ec)) {
            std::string name = it->path().filename().string();
            if (name.rfind("crash-", 0) == 0) add(it->path(), "dumps/" + name);  // not dev runs' memory dumps
        }
        ok = mz_zip_writer_finalize_archive(&zip) && ok;
        mz_zip_writer_end(&zip);
        if (!ok) {
            fs::remove(zip_path, ec);
            return {};
        }
        return zip_path;
    }

    void phone_pads_enable(bool on);  // phone_pads.cpp
    void create_party_tab();          // tournament.cpp
    void create_addons_tab();  // addons.cpp
    void create_export_tab();  // export_rom.cpp

    void create_season_tab();       // season_settings.cpp
    void create_controllers_tab();  // controllers_tab.cpp
    void create_rules_tab();        // house_rules.cpp
    void original_settings_visibility();  // adv_mode.cpp
    void apply_ui_theme();          // ui_theme.cpp
}

void blitz::init_config() {
    std::filesystem::path app_dir = recompui::file::get_app_folder_path();
    if (!app_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(app_dir, ec);
        if (ec) {  // say why instead of ending with an uncaught exception
            std::string msg = "The app folder " + app_dir.string() + " can't be created (" + ec.message() +
                              "). Settings and saves won't be kept.";
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Blitz", msg.c_str(), nullptr);
        }
    }

    blitz::apply_ui_theme();

    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = true;
    general_options.has_gyro_sensitivity = false;
    general_options.has_mouse_sensitivity = false;
    recomp::config::Config& general = recompui::config::create_general_tab(general_options);
    general.add_bool_option(
        "p1_rumble",
        "Player 1 Rumble",
        "Player 1's controller rumbles on hits, like players 2-4. Saving to the Controller Pak is unaffected.",
        true);
    general.add_bool_option("welcomed", "Welcomed", "The first-start welcome was shown.", false, true);
    // Sound folded in here (team): the Sound tab held this one slider. It stays the real setting (sound.json, what the
    // audio reads); this row writes it, and is set from it at start.
    general.add_percent_number_option("volume", "Volume", "The game's music, crowd and effects.", 100.0);
    general.add_option_change_callback("volume", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                    recomp::config::OptionChangeContext) {
        recomp::config::Config& sound = recompui::config::get_sound_config();
        if (std::get<double>(sound.get_option_value(recompui::config::sound::options::main_volume)) != std::get<double>(cur)) {
            sound.update_option_value(recompui::config::sound::options::main_volume, cur);
            sound.save_config();
        }
    });
    // About: the build, and seven presses of it show (or hide) the Development rows, as holding L2 + R2 for 3 s in Settings
    // does (controllers.cpp). For touch, with no triggers.
    // BRANDING: About text (app name + version)
    general.add_bool_option("about", "About", std::string("New Blitz ") + BLITZ_VERSION + ", build " + BLITZ_BUILD + " (" + BLITZ_COMMIT + ").", false);
    recompui::config::set_option_action("general", "about", "Build " BLITZ_BUILD);
    general.add_option_change_callback("about", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                   recomp::config::OptionChangeContext ctx) {
        if (ctx != recomp::config::OptionChangeContext::Permanent || !std::get<bool>(cur)) return;
        recompui::config::get_config("general").update_option_value("about", false);
        static int presses = 0;
        if (++presses >= 7) {
            presses = 0;
            blitz::toggle_development();
        }
    });
    // Hints (hints.cpp, docs/design/FIRST_RUN.md): a short tip the first time each part of the game comes up.
    general.add_bool_option(
        "hints",
        "Hints",
        "A short tip the first time you meet each part of the game: the menus, team select, play select, offense and "
        "defense, do-overs. Each shows once, at the bottom left, and takes no button presses.",
        true);
    general.add_bool_option(
        "hints_reset",
        "Show Hints Again",
        "Brings back the hints you've already seen.",
        false);
    general.add_string_option("hints_seen", "Hints Seen", "The hints already shown.", "", true);
    // The frame counter (src/main/fps_counter.cpp): for chasing stutter, on any device.
    general.add_bool_option(
        "fps_counter",
        "Frame Counter",
        "The game's frames per second and the longest gap between two frames in the last second, at the top right. "
        "A gap well over the usual one is a visible hitch.",
        false);
    if (blitz::platform::touch_controls) {
        // Phones (src/main/touch_controls.cpp): on-screen controls for player 1 while you play by touch.
        general.add_bool_option(
            "touch_controls",
            "Touch Controls",
            "On-screen controls for player 1. They hide while a controller is in use and come back at your next touch.",
            true);
    }
    recompui::config::set_option_action("general", "hints_reset", "Show");
    general.add_option_change_callback("hints_reset", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                         recomp::config::OptionChangeContext ctx) {
        if (ctx != recomp::config::OptionChangeContext::Permanent || !std::get<bool>(cur)) return;
        recompui::config::get_general_config().update_option_value("hints_reset", false);
        blitz::hints_reset();
        blitz::post_status("Hints: they'll show again");
    });
    general.add_bool_option("phone_controllers", "Phone Controllers", "Settings → Players.", false, true);
    general.add_option_change_callback("phone_controllers", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                               recomp::config::OptionChangeContext) {
        blitz::phone_pads_enable(std::get<bool>(cur));
    });
    general.add_bool_option(
        "tv_mode",
        "TV Mode",
        blitz::platform::desktop
            ? "For playing on a TV (HDMI, or AirPlay to an Apple TV): full screen, widescreen picture and the 16:9 "
              "HUD at once. Off goes back to a window. Pair the controllers first (Players tab)."
            : "For playing on a TV: the widescreen picture and the 16:9 HUD at once. Pair the controllers first "
              "(Players tab).",
        false);
    general.add_option_change_callback("tv_mode", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                     recomp::config::OptionChangeContext ctx) {
        if (ctx == recomp::config::OptionChangeContext::Permanent) apply_tv_mode(std::get<bool>(cur));
    });
    general.add_enum_option(
        "button_icon_mode",
        "Button Icons",
        "Which buttons the game's screens and these menus show. <b>Each Player's Controller</b>: in the game, every "
        "player sees their own controller's buttons (PlayStation, Xbox or Switch; any other controller shows "
        "PlayStation), and these menus follow player 1. Or one style for everyone: <b>PlayStation</b>, <b>Xbox</b>, "
        "<b>Switch</b> (by position: B passes, Y tackles) or <b>N64</b> (the game's own icons). These menus have no "
        "Switch or N64 style: they show Xbox letters for Switch and PlayStation for N64.",
        std::vector<recomp::config::ConfigOptionEnumOption>{
            {0, "Each", "Each Player's Controller"}, {1, "PlayStation", "PlayStation"}, {2, "Xbox", "Xbox"}, {3, "Switch", "Switch"}, {4, "N64", "N64"}},
        0u);
    general.add_enum_option(
        "fullscreen_button",
        "Fullscreen Button",
        "Switch between window and full screen from the controller. <b>L3 + R3</b>: press both sticks in together. "
        "<b>Home</b>: the PS / Xbox / Home button (macOS may also open its game overlay on that button). "
        "On the keyboard, F11 or Alt+Return.",
        std::vector<recomp::config::ConfigOptionEnumOption>{{0, "Off", "Off"}, {1, "L3R3", "L3 + R3"}, {2, "Home", "Home"}},
        1u, !blitz::platform::desktop);  // a console or phone is always full screen
    general.add_bool_option(
        "savestate_buttons",
        "Save State Buttons",
        std::string("Save and load a state from any controller: hold <b>L3 or R3</b> (press a stick in), then press ") +
            (getenv("BLITZ_SHOULDERS_ARE_TRIGGERS") ? "<b>R2</b> to save, or <b>L2</b> to load" : "<b>R1</b> to save, or <b>L1</b> to load") +
            " (only a press after the stick is in counts, so holding turbo can't load a state by accident); <b>□ / ○</b> (or <b>D-pad left / right</b>) pick one of three slots, or Auto, which saves itself when the app goes to the background mid-game." +
            (blitz::platform::desktop ? " On the keyboard, F5 saves, F7 loads and F6 picks the slot."
             : blitz::platform::touch_controls ? " Playing by touch: the SAVE and LOAD buttons at the top." : "") +
            " (Experimental)",
        true);
    general.add_bool_option(
        "redo_play",
        "Redo Play",
        std::string("Run the last play again: hold <b>L3 or R3</b> and press <b>△</b> (or <b>D-pad up</b>)") +
            (blitz::platform::desktop ? " (or <b>F8</b>)" : blitz::platform::touch_controls ? " (or REDO when playing by touch)" : "") +
            ". In the middle of a play, "
        "or while its result shows, it goes back to that play's play select; at play select it goes back one play "
        "more, and pressing again keeps going back (up to 8 plays; in Potato Mode the last one only).",
        true);
    general.add_bool_option(
        "instant_replay",
        "Instant Replay",
        std::string("Keeps the last few seconds of play, to watch again from the pause menu's <b>REPLAY</b>") +
            (blitz::platform::desktop ? " (or <b>F9</b>)" : "") +
            ": full screen, with the game stopped, a scrubber and <b>Save</b>. Takes some memory (under 100 MB on a small "
            "handheld, up to about 600 MB on a computer) and a little time each frame, and turns itself off if play can't "
            "hold its frame rate.",
        true);
    // Was Skip Launcher (off by default everywhere); a phone opened on the launcher every time (team, 2026-10-29).
    general.add_bool_option(
        "launcher_at_start",
        "Launcher at Start",
        std::string("Open on the launcher (Change ROM, Add-Ons, Blitz Builder) instead of starting the game.") +
            (blitz::platform::desktop ? " Hold <b>Shift</b> while the app opens to see the launcher anyway." : ""),
        blitz::platform::desktop);
    general.add_bool_option(
        "skip_intros",
        "Skip Intro Screens",
        "Cut the legal screen and the title short, so the game is ready for Start in a few seconds.",
        false);
    general.add_bool_option(
        "skip_rumble_screen",
        "Skip Rumble Pak Screen",
        "Go straight from Controller Select to team select, without the \"please insert Rumble Paks\" screen. "
        "Rumble works the same either way.",
        true);  // on for a new install (a saved setting stays): the screen means nothing without a Rumble Pak
    general.add_bool_option(
        "experimental_resume",
        "Resume Interrupted Games",
        "If the app closes in the middle of a game (a crash, or quitting), the next game between the same two "
        "teams picks up where it left off: score, quarter, clock, possession and field position. "
        "<b>Experimental:</b> the game's tutorial tips cover the first play back, and nothing says a game is "
        "being resumed until it starts.",
        false);
    general.add_bool_option(
        "copy_debug_info",
        "Copy Debug Info",
        "For a bug report: copies the app version, your device and its system, which ROM is loaded, your controllers and "
        "where the log file is. Paste it into the report and attach blitz.log from the app folder.",
        false);
    recompui::config::set_option_action("general", "copy_debug_info", "Copy");
    general.add_option_change_callback("copy_debug_info", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                             recomp::config::OptionChangeContext ctx) {
        if (ctx != recomp::config::OptionChangeContext::Permanent || !std::get<bool>(cur)) return;
        recompui::config::get_config("general").update_option_value("copy_debug_info", false);
        blitz::off_ui_thread([] {
            std::string info = blitz::debug_info();
            blitz::set_clipboard_later(info);  // controllers.cpp: SDL's clipboard is the main thread's
            fprintf(stderr, "[debug] copied:\n%s", info.c_str());
            blitz::post_status("Debug info copied");
        });
    });
    // BLITZ DS (ds_view.cpp): a second screen, where the device has one.
    general.add_bool_option("two_screens", "Two Screens",
        "On a device with two screens: the field across both, menus and play select on the lower one. Turning it off puts "
        "the game on one screen.", true);
    general.add_bool_option("swap_screens", "Swap Screens",
        "Which screen is the lower one, where play select goes. If play select comes up on the top screen, turn this on and "
        "restart the app.", false);
#if defined(__ANDROID__)
    general.add_bool_option("potato_mode", "Potato Mode",
        "For weak handhelds: lower resolution, no antialiasing, no Redo Play, and straight into the game with no launcher. "
        "Always on with about 1 GB of RAM. On a stronger device it also keeps the game on four cores, to stand in for a weak "
        "one. Restart the app after changing it.", false);
    general.add_option_change_callback("potato_mode", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                         recomp::config::OptionChangeContext) {
        blitz::host_set_potato(std::get<bool>(cur));
    });
#endif
#if defined(__ANDROID__)
    general.add_bool_option("potato_two_screens", "Two Screens (Potato)",
        "Potato Mode: one screen by default, where it runs best, and where the system's own dual-screen setting can take over. "
        "On: the field across both screens, slower. Restart the app after changing it.",
        false, getenv("BLITZ_LEAN") == nullptr);
    general.add_option_change_callback("potato_two_screens", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                                recomp::config::OptionChangeContext) {
        blitz::host_set_potato_two_screens(std::get<bool>(cur));
    });
    // Both under new names, as a saved value outlives a changed default: 1x (the setup the team called almost there; 1.5x has never
    // run) and Full (a crash at launch can't be switched off from Settings) start out true on a device that's already run these.
    general.add_enum_option("potato_two_sharpness", "Two-Screen Sharpness",
        "Potato Mode with two screens: how much detail each screen gets. 1x is the game's original resolution. More is sharper and slower. "
        "Restart the app after changing it.",
        std::vector<recomp::config::ConfigOptionEnumOption>{{0, "S100", "1x"}, {1, "S125", "1.25x"}, {2, "S150", "1.5x"}, {3, "S175", "1.75x"}, {4, "S200", "2x"}},
        0u, getenv("BLITZ_LEAN") == nullptr);
    general.add_option_change_callback("potato_two_sharpness", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                                  recomp::config::OptionChangeContext) {
        if (getenv("BLITZ_LEAN") != nullptr) blitz_2x_scale = blitz::two_x_scale(blitz::sharpness_scale_for(std::get<uint32_t>(cur)));
    });
    general.add_enum_option("potato_waits", "GPU Waits",
        "Potato Mode: <b>Narrow</b> lets the GPU overlap its work, which should be faster. <b>Full</b> is the safest if the picture glitches.",
        std::vector<recomp::config::ConfigOptionEnumOption>{{0, "Narrow", "Narrow"}, {1, "Full", "Full"}},
        1u, getenv("BLITZ_LEAN") == nullptr);
    general.add_option_change_callback("potato_waits", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                          recomp::config::OptionChangeContext) {
        // A safe start (BlitzActivity.safeStartCheck) is Full, whatever's saved, without changing what's saved.
        if (getenv("BLITZ_LEAN") != nullptr) plume_narrow_barriers(blitz::narrow_waits_for(std::get<uint32_t>(cur)) ? 1 : 0);
    });
    general.add_enum_option("potato_frame_copy", "Frame Copy",
        "Potato Mode: how often each frame is drawn a second time, into the game's own memory. <b>When Needed</b> is fast. "
        "<b>Always</b> is slowest and the most correct. <b>Never</b> is fastest and can garble panels and the field.",
        std::vector<recomp::config::ConfigOptionEnumOption>{{0, "WhenNeeded", "When Needed"}, {1, "Always", "Always"}, {2, "Never", "Never"}},
        0u, getenv("BLITZ_LEAN") == nullptr);
    general.add_option_change_callback("potato_frame_copy", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                               recomp::config::OptionChangeContext) {
        if (getenv("BLITZ_LEAN") != nullptr) blitz_rt64_render_to_ram(blitz::frame_copy_for(std::get<uint32_t>(cur)));
    });
#endif
    // Only for those with two screens: ds_update shows them when it finds the second.
    general.add_enum_option("ds_tilt", "Field Ahead",
        "How far the camera looks up the field across both screens. Higher keeps the play low on the bottom screen and shows "
        "more of what's ahead on the top one.",
        std::vector<recomp::config::ConfigOptionEnumOption>{{0, "T40", "0.40"}, {1, "T45", "0.45"}, {2, "T50", "0.50"}, {3, "T55", "0.55"}, {4, "T60", "0.60"}, {5, "T65", "0.65"}},
        2u, true);  // 0.50: the team's pick on an RG DS, 2026-11-02 (0.65 looked too far up the field)
    general.add_enum_option("ds_zoom", "Camera Zoom Out",
        "How much further back than the game's own camera, to keep the receivers on the ends in view.",
        std::vector<recomp::config::ConfigOptionEnumOption>{{0, "Z0", "0%"}, {1, "Z5", "5%"}, {2, "Z10", "10%"}, {3, "Z125", "12.5%"}, {4, "Z15", "15%"}, {5, "Z20", "20%"}, {6, "Z25", "25%"}, {7, "Z30", "30%"}},
        3u, true);  // 12.5%: the team's pick on an RG DS. Saved by name, so adding it moved no one's setting
    general.add_enum_option("ds_screens", "DS Screens",
        "<b>Both Screens</b>: the field across the two. <b>Game + Companion</b>: the game on the lower screen and the drive "
        "board above it. <b>Stylus</b>: the game above and a wheel below, to throw to a receiver by touch, with receiver icons on. L3 steps through them.",
        std::vector<recomp::config::ConfigOptionEnumOption>{{0, "Both", "Both Screens"}, {1, "Companion", "Game + Companion"}, {2, "Stylus", "Stylus"}},
        0u, true);
    general.add_option_change_callback("ds_screens", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                        recomp::config::OptionChangeContext) {
        blitz_ds_screen_mode = (int)std::get<uint32_t>(cur);
    });
    general.add_option_change_callback("ds_tilt", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                     recomp::config::OptionChangeContext) {
        blitz::tall_set("tilt", 0.40f + 0.05f * (float)std::get<uint32_t>(cur));
    });
    general.add_option_change_callback("ds_zoom", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                     recomp::config::OptionChangeContext) {
        blitz::tall_set("zoom", blitz::ds_zoom_for(std::get<uint32_t>(cur)));
    });
    general.add_option_change_callback("swap_screens", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                          recomp::config::OptionChangeContext) {
#if defined(__ANDROID__)
        blitz::host_set_swap_screens(std::get<bool>(cur));
#else
        blitz::ds_set_swapped(std::get<bool>(cur));  // the Mac's test windows
#endif
    });
#if defined(__ANDROID__) && !defined(BLITZ_PUBLIC_BUILD)  // the public app has no developer's address to send to
    general.add_bool_option("send_logs", "Send Logs",
        "Test builds: sends the app's logs, crash reports, debug info and settings straight to the developer.", false);
    recompui::config::set_option_action("general", "send_logs", "Send");
    general.add_option_change_callback("send_logs", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                       recomp::config::OptionChangeContext ctx) {
        if (ctx != recomp::config::OptionChangeContext::Permanent || !std::get<bool>(cur)) return;
        recompui::config::get_config("general").update_option_value("send_logs", false);
        blitz::off_ui_thread([] {
            std::filesystem::path zip = blitz::save_logs();
            if (zip.empty()) {
                blitz::post_status("Couldn't collect the logs");
                return;
            }
            blitz::host_send_logs(zip);
            blitz::post_status("Sending logs...");
        });
    });
#endif
    general.add_bool_option(
        "save_logs",
        "Save Logs",
        std::string("For a bug report: saves the app's logs, any crash reports, the debug info and your settings in "
                    "one zip, ") +
            (blitz::platform::desktop ? "in your Downloads folder." :
             blitz::platform::phone   ? "in the app's folder (the Files app on an iPhone, a file manager on Android)." :
                                        // BRANDING: PS5 app folder name shown to the user
                                        "in /data/blitzrecomp (FTP).") +
            " Send that file along with the report.",
        false);
    recompui::config::set_option_action("general", "save_logs", "Save");
    general.add_option_change_callback("save_logs", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                       recomp::config::OptionChangeContext ctx) {
        if (ctx != recomp::config::OptionChangeContext::Permanent || !std::get<bool>(cur)) return;
        recompui::config::get_config("general").update_option_value("save_logs", false);
        blitz::off_ui_thread([] {
            std::filesystem::path zip = blitz::save_logs();
            fprintf(stderr, "[logs] saved: %s\n", zip.empty() ? "(failed)" : zip.string().c_str());
            if (zip.empty()) blitz::post_status("Couldn't save the logs");
            else if (blitz::platform::desktop) {
                blitz::post_status("Logs saved in Downloads");
                blitz::reveal_in_file_browser(zip);
            }
            else blitz::post_status("Logs saved: " + zip.filename().string());
        });
    });

    recompui::config::create_graphics_tab();
    // What each N64 button does in Blitz, shown on the Controls tab (docs/PLAYING.md). L and the C buttons
    // have no known job, so they stay bare.
    {
        using GI = recompinput::GameInput;
        for (GI move : {GI::Y_AXIS_POS, GI::Y_AXIS_NEG, GI::X_AXIS_NEG, GI::X_AXIS_POS,
                        GI::DPAD_UP, GI::DPAD_DOWN, GI::DPAD_LEFT, GI::DPAD_RIGHT}) {
            recompui::set_game_input_meaning(move, "MOVE");
        }
        recompui::set_game_input_meaning(GI::A, "PASS / SWITCH");
        recompui::set_game_input_meaning(GI::B, "JUMP / TACKLE");
        recompui::set_game_input_meaning(GI::Z, "TURBO");
        recompui::set_game_input_meaning(GI::R, "TURBO");
        recompui::set_game_input_meaning(GI::START, "PAUSE");
    }
    recompui::config::create_controls_tab();
    blitz::create_controllers_tab();
    recompui::config::create_sound_tab();
    // No Mods tab: Blitz has no mod support (content packs and the art drop-in are folders instead).
    blitz::create_season_tab();
    blitz::create_rules_tab();
#if BLITZ_MODERN
    blitz::original_settings_visibility();  // adv_mode.cpp: Original 1999 hides the 2026-only settings
#endif
    blitz::create_party_tab();
    blitz::create_addons_tab();
    if (blitz::export_available) blitz::create_export_tab();
    // Folded away (the team's settings pass): Sound into General's Volume, and Export, whose options the Export screen has too.
    recompui::config::set_tab_visible(recompui::config::sound::id, false);
    if (blitz::export_available) recompui::config::set_tab_visible("export", false);

    recompui::config::finalize();
    {
        // Volume shows what the audio really uses (sound.json).
        recomp::config::Config& g = recompui::config::get_general_config();
        g.update_option_value("volume", recompui::config::get_sound_config().get_option_value(recompui::config::sound::options::main_volume));
        // Rows only a desktop has, and the Development rows unless they've been shown (debug_mode).
        g.update_option_hidden(recompui::config::general::options::background_input_mode, !blitz::platform::desktop);
        recompui::config::get_graphics_config().update_option_hidden(recompui::config::graphics::options::wm_option, !blitz::platform::desktop);
        blitz::show_development(std::get<bool>(g.get_option_value(recompui::config::general::options::debug_mode)));
    }
#if defined(__ANDROID__)
    // The callbacks don't run on a first launch, with no settings file to load, and a recipe has to hold there too.
    if (getenv("BLITZ_LEAN") != nullptr) {
        blitz_2x_scale = blitz::two_x_scale(blitz::potato_scale());
        plume_narrow_barriers(blitz::potato_narrow_waits() ? 1 : 0);
    }
    if (const char* recipe = getenv("BLITZ_RECIPE_LOG")) fprintf(stderr, "[recipe] %s\n", recipe);
#endif
    // The app always opens in a window (team): full screen from L3 + R3, the Graphics tab or TV Mode lasts for
    // the session; TV Mode's widescreen settings stay.
    {
        recomp::config::Config& g = recompui::config::get_config(recompui::config::graphics::id);
        namespace opt = recompui::config::graphics::options;
        if (std::get<uint32_t>(g.get_option_value(opt::wm_option)) == (uint32_t)ultramodern::renderer::WindowMode::Fullscreen) {
            g.set_option_value(opt::wm_option, (uint32_t)ultramodern::renderer::WindowMode::Windowed);
            g.save_config();
        }
    }
    if (std::get<bool>(recompui::config::get_general_config().get_option_value("phone_controllers"))) {
        blitz::phone_pads_enable(true);  // remembered from last time
    }
    // Set up from a computer's Blitz Builder (device_send.cpp: Send to iPhone, Android or PS5): its questions were
    // answered there, so they aren't asked again here.
    {
        std::error_code ec;
        const std::filesystem::path sent = recompui::file::get_app_folder_path() / "builder_done";
        if (std::filesystem::is_regular_file(sent, ec)) {
            recompui::config::get_general_config().update_option_value("welcomed", true);
            if (recompui::config::get_general_config().save_config()) std::filesystem::remove(sent, ec);
        }
    }
    // Skip Launcher, from before Launcher at Start: a computer that skipped it keeps skipping it.
    {
        std::ifstream in(recompui::file::get_app_folder_path() / "general.json");
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.find("\"launcher_at_start\"") == std::string::npos && text.find("\"skip_launcher\": true") != std::string::npos) {
            recompui::config::get_general_config().update_option_value("launcher_at_start", false);
            recompui::config::get_general_config().save_config();
        }
    }
}
