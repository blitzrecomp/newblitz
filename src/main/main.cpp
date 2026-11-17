// NFL Blitz 2000: Recompiled - macOS entry point.
// Structure follows BanjoRecomp's src/main/main.cpp (GPL-3.0): SDL window with a Metal layer,
// SDL audio (audio.cpp), RecompFrontend launcher and settings, N64ModernRuntime + RT64.
#ifdef __ANDROID__
#include <sched.h>
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>
#include <atomic>
#include <cerrno>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

#include "blitz_platform.h"

#ifdef __PROSPERO__
extern "C" int sceSystemServiceHideSplashScreen(void);
extern "C" void blitz_ps5_crumb(const char* step);  // platform_ps5.cpp: /data/blitzrecomp/boot.log
#define BOOT_CRUMB(step) blitz_ps5_crumb(step)
#else
#define BOOT_CRUMB(step) ((void)0)
#endif
#ifdef BLITZ_IOS
namespace blitz { void ios_keep_landscape(); }  // platform_ios.mm
#endif
#if BLITZ_HAS_FILE_DIALOG_NFD
#include "nfd.h"  // the desktop file dialog (phones use the system's document picker, the PS5 none)
#endif

#include "ultramodern/ultra64.h"
#include "blitz_host.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#if !defined(BLITZ_PLATFORM_PHONE)
#define SDL_MAIN_HANDLED
#endif
// iOS: SDL's main() starts UIKit and runs this main() as SDL_main (SDL2main, platform/ios/ios_bundle.cmake).
// Android: SDLActivity calls SDL_main in libBlitzRecompiled.so (platform/android).
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"
#if defined(BLITZ_PLATFORM_LINUX)
// SDL_syswm.h brings in Xlib on Linux; its macros collide with names in RmlUi and the runtime (as
// ultramodern/renderer_context.hpp also undoes).
#undef None
#undef Bool
#undef Status
#undef Always
#undef Success
#undef True
#undef False
#undef LockMask
#undef KeyPress
#undef KeyRelease
#undef FocusIn
#undef FocusOut
#undef Above
#undef Below
#undef CurrentTime
#endif

#include "recompui/recompui.h"
#include "ui_kit.h"
#include "install_expiry.h"
#include "recompui/program_config.h"
#include "recompui/renderer.h"
#include "recompui/config.h"
#include "util/file.h"
#include "recompinput/input_events.h"
#include "recompinput/recompinput.h"
#include "recompinput/profiles.h"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/helpers.hpp"

#include "blitz_audio.h"
#include "blitz_config.h"
#include "icon_bytes.h"
#include "miniz.h"

const std::string version_string = BLITZ_VERSION;  // CMakeLists.txt

namespace blitz {
    void touch_controls_init();  // touch_controls.cpp (phones)
    void touch_controls_update(SDL_Window* window);
    void fps_vi_tick();     // fps_counter.cpp
    void fps_update();
    void receiver_targets_update(SDL_Window* window);  // receiver_targets.cpp
    void tall_view_update(SDL_Window* window);  // tall_view.cpp
    void ds_update(SDL_Window* window);  // ds_view.cpp
    void stylus_update(SDL_Window* window);  // ds_stylus.cpp
    void ds_window_created(SDL_Window* window);
    void window_size_update(SDL_Window* window);
    void register_overlays();
    void on_init(uint8_t* rdram, recomp_context* ctx);
    void dev_input_game_started();
    void monitor_register_thread(const OSThread* t);
    bool get_input(int controller_num, uint16_t* buttons, float* x, float* y);
    // controllers.cpp: physical controllers -> N64 ports (port 1 is also the keyboard).
    void controllers_init();
    void capture_event(const SDL_Event& e);  // capture.cpp: F12 screenshots, Shift+F12 clips
    void controllers_update();
    bool port_connected(int port);
    void port_set_rumble(int port, bool on);
    void ports_update_rumble();
}

template<typename... Ts>
void exit_error(const char* str, Ts ...args) {
    ((void)fprintf(stderr, str, args), ...);
    assert(false);
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
}

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    // PlayStation pads in enhanced mode: light bar, player LEDs and rumble (controllers.cpp sets them).
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH_PLAYER_LED, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) < 0) {
        exit_error("Failed to initialize SDL2: %s\n", SDL_GetError());
    }

    fprintf(stdout, "SDL Video Driver: %s\n", SDL_GetCurrentVideoDriver());
    blitz::controllers_init();
    SDL_AddEventWatch([](void*, SDL_Event* e) { blitz::capture_event(*e); return 0; }, nullptr);  // F12: screenshot
    // Window changes go in the log: an open bug (team, 2026-10-18) has the window alternate between a frame and solid
    // magenta, which looks like the Metal drawable or a render target being recreated. The log should say whether
    // the window was moved, resized, hidden or changed screens when it starts.
    SDL_AddEventWatch([](void*, SDL_Event* e) {
        if (e->type != SDL_WINDOWEVENT) return 0;
        const char* what = nullptr;
        switch (e->window.event) {
            case SDL_WINDOWEVENT_SHOWN: what = "shown"; break;
            case SDL_WINDOWEVENT_HIDDEN: what = "hidden"; break;
            case SDL_WINDOWEVENT_EXPOSED: return 0;  // every redraw request: too many
            case SDL_WINDOWEVENT_MOVED: what = "moved"; break;
            case SDL_WINDOWEVENT_SIZE_CHANGED: what = "size changed"; break;
            case SDL_WINDOWEVENT_MINIMIZED: what = "minimized"; break;
            case SDL_WINDOWEVENT_MAXIMIZED: what = "maximized"; break;
            case SDL_WINDOWEVENT_RESTORED: what = "restored"; break;
            case SDL_WINDOWEVENT_FOCUS_GAINED: what = "focused"; break;
            case SDL_WINDOWEVENT_FOCUS_LOST: what = "unfocused"; break;
            case SDL_WINDOWEVENT_DISPLAY_CHANGED: what = "display changed"; break;
            default: return 0;
        }
        fprintf(stderr, "[window] %s (%d, %d) at %u ms\n", what, e->window.data1, e->window.data2, SDL_GetTicks());
#ifdef BLITZ_IOS
        // Back from a portrait system screen (screenshot sheet, app switcher): rotate the scene back (platform_ios.mm).
        if (e->window.event == SDL_WINDOWEVENT_RESTORED || e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
            (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED && e->window.data2 > e->window.data1))
            blitz::ios_keep_landscape();
#endif
        return 0;
    }, nullptr);
    return {};
}

namespace blitz {
    bool controller_connected(int controller_num) {
        return port_connected(controller_num);
    }
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (blitz::controller_connected(controller_num)) {
        // The runtime only uses connected_pak for the "accessory inserted" bit in osContGetQuery's status.
        // Which accessory it is (a Controller Pak for player 1) is answered by src/game/controller_pak.cpp,
        // which replaces the game's pak and rumble probes (osMotorInit, __osPfsGetStatus).
        return ultramodern::input::connected_device_info_t{
            .connected_device = ultramodern::input::Device::Controller,
            .connected_pak = ultramodern::input::Pak::RumblePak,
        };
    }

    return ultramodern::input::connected_device_info_t{
        .connected_device = ultramodern::input::Device::None,
        .connected_pak = ultramodern::input::Pak::None,
    };
}

SDL_Window* window;

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    BOOT_CRUMB("create_window");
    // 4:3 window at 3x the game's 320x240 output. BLITZ_WINDOW_SIZE=WxH overrides it (dev: widescreen tests).
    int width = 960, height = 720;
    if (const char* size = getenv("BLITZ_WINDOW_SIZE")) {
        sscanf(size, "%dx%d", &width, &height);
    }
#ifdef _WIN32
    // Windows: a plain window; RT64 makes its own Vulkan (or D3D12) surface on the HWND (docs/WINDOWS_VULKAN.md).
    window = SDL_CreateWindow(blitz::program_name.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
#elif defined(__PROSPERO__)
    // PS5: an SDL window (SDL's dummy video driver) for events and the UI's size, the size of the 4K display plane
    // RT64 draws to (docs/PS5_HOMEBREW.md).
    width = 3840;
    height = 2160;
    window = SDL_CreateWindow(blitz::program_name.c_str(), 0, 0, width, height, 0);
#elif defined(BLITZ_IOS)
    // iPhone/iPad: the whole screen, in the app's window scene (SDL's UIKit patch), drawn with Metal; landscape only.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, getenv("BLITZ_DS_STACK") ? "Portrait" : "LandscapeLeft LandscapeRight");
    window = SDL_CreateWindow(blitz::program_name.c_str(), 0, 0, width, height,
        SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI);
#elif defined(__ANDROID__)
    // Android: the whole screen (SDL's surface view), drawn with Vulkan on its ANativeWindow; landscape only. The
    // Vulkan flag matters: without it SDL makes the window OpenGL and takes the surface for EGL, and Vulkan's surface
    // then fails (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR).
    SDL_SetHint(SDL_HINT_ORIENTATIONS, getenv("BLITZ_DS_STACK") ? "Portrait" : "LandscapeLeft LandscapeRight");
    window = SDL_CreateWindow(blitz::program_name.c_str(), 0, 0, width, height,
        SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_VULKAN);
#elif defined(BLITZ_PLATFORM_LINUX)
    // Linux: a Vulkan window; RT64 makes its surface through SDL (X11 or Wayland).
    window = SDL_CreateWindow(blitz::program_name.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_ALLOW_HIGHDPI);
#else
    window = SDL_CreateWindow(blitz::program_name.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI);
#endif

    if (window == nullptr) {
        exit_error("Failed to create window: %s\n", SDL_GetError());
    }
    blitz::touch_controls_init();  // phones: on-screen controls for player 1 (nothing elsewhere)
    blitz::window_created(window);
    blitz::ds_window_created(window);
    // BRANDING: app menu name, from program_name
    {   // Cmd+Q, Cmd+H, Cmd+M (macos_support.mm): the name after "BlitzRecomp: " if there's one ("Modern Blitz"), else the
        // whole name (New Blitz has none: find gave npos, and npos + 2 cut the first letter, "Quit ew Blitz").
        const size_t colon = blitz::program_name.find(": ");
        blitz::install_app_menu(colon == std::string::npos ? std::string("New Blitz") : blitz::program_name.substr(colon + 2));
    }

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);

#ifdef _WIN32
    return ultramodern::renderer::WindowHandle{ wmInfo.info.win.window, GetCurrentThreadId() };
#elif defined(__PROSPERO__)
    return ultramodern::renderer::WindowHandle{window};  // RT64 takes this one instead of making its own (frontend patch)
#elif defined(BLITZ_IOS)
    SDL_MetalView view = SDL_Metal_CreateView(window);
    return ultramodern::renderer::WindowHandle{ wmInfo.info.uikit.window, SDL_Metal_GetLayer(view) };
#elif defined(__ANDROID__)
    return ultramodern::renderer::WindowHandle{ wmInfo.info.android.window };
#elif defined(BLITZ_PLATFORM_LINUX)
    return window;  // RT64_SDL_WINDOW_VULKAN: the SDL window itself
#else
    SDL_MetalView view = SDL_Metal_CreateView(window);
    return ultramodern::renderer::WindowHandle{ wmInfo.info.cocoa.window, SDL_Metal_GetLayer(view) };
#endif
}

namespace blitz {
    std::string take_notice();  // notices.cpp
    std::string take_status();
    void toast_show(const std::string& message);  // toast.cpp
    void toast_hold(bool on);
    int take_status_hold();                       // notices.cpp
    void toast_update();
    void replay_update();              // replay_screen.cpp
    void checkpoint_startup_prompt();  // checkpoint.cpp
    void savestate_startup_prompt();   // savestate.cpp
    bool savestate_quit_prompt();
    void capture_shutdown();  // capture.cpp
    void party_update();               // below
    void hints_update();               // hints.cpp
    void builder_update();             // firstrun.cpp: the first start's setup
    void builder_autostarted();
    void firstrun_launcher_frame(int frame);
    void export_update();              // export_rom.cpp
    void rosters_check_start();        // rosters_live.cpp (Modern)
    void paks_at_launch();             // season_pak.cpp
    void rosters_update();
    void crew_update();                // crew.cpp (Modern)
    void season_pregame_update();      // season_pregame.cpp (Modern)
    void party_tab_refresh();  // tournament.cpp
    bool checkpoint_quit_prompt();
    void ui_tick();                    // fps_counter.cpp
    void ui_step(const char* step);    //   ... and where in the tick the UI thread is (the PS5's stall line names it)
}

// Main thread, about every millisecond (recomp.cpp's loop).
void update_gfx(void*) {
    blitz::ui_tick();
    blitz::ui_step("events (SDL pump)");
    recompinput::handle_events();
    blitz::ui_step("controllers");
    blitz::controllers_update();
    // Problems found on the game thread (Controller Pak, content packs) are queued and shown here.
    blitz::ui_step("checkpoint prompt");
    blitz::checkpoint_startup_prompt();
    blitz::ui_step("savestate prompt");
    blitz::savestate_startup_prompt();
    blitz::ui_step("party");
    blitz::party_update();
    // First-time hints (hints.cpp), a few times a second.
    {
        static Uint32 last = 0;
        Uint32 now = SDL_GetTicks();
        if (now - last >= 50) {
            last = now;
            blitz::ui_step("hints, builder, export, rosters");
            blitz::hints_update();
            blitz::install_expiry_update();
            blitz::builder_update();
            blitz::export_update();
#if BLITZ_MODERN
            blitz::rosters_update();
            blitz::crew_update();
            blitz::season_pregame_update();
#endif
        }
    }
    blitz::ui_step("toasts, replay, screens");
    blitz::party_tab_refresh();
    // Statuses (save states, Redo Play) show as a toast at the bottom right for a moment.
    std::string status = blitz::take_status();
    if (!status.empty() && ultramodern::is_game_started()) blitz::toast_show(status);
    if (int h = blitz::take_status_hold(); h != 0) blitz::toast_hold(h == 1);  // a load's notice stays up (notices.cpp)
    blitz::toast_update();
    blitz::replay_update();
    blitz::touch_controls_update(window);
    blitz::fps_update();
    blitz::ds_update(window);
    blitz::stylus_update(window);
    blitz::tall_view_update(window);
    blitz::receiver_targets_update(window);
    blitz::window_size_update(window);
    blitz::ui_step("notice");
    std::string notice = blitz::take_notice();
    // BLITZ_QUIET_NOTICES=1 (dev/test runs): log only, no dialog on the desktop.
    static const bool quiet = getenv("BLITZ_QUIET_NOTICES") != nullptr;
    if (!notice.empty() && !quiet) {
        recompui::message_box(notice.c_str());
    }
    blitz::ui_step("between ticks (the runtime's loop)");
}

extern RspUcodeFunc n_aspMain;

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_AUDTASK:
        return n_aspMain;
    default:
        fprintf(stderr, "Unknown task: %" PRIu32 "\n", task->t.type);
        return nullptr;
    }
}

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

#if BLITZ_TEAMPLUS
#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

namespace blitz {
    // The 2026 build runs any ROM with team+'s code, whatever its data (rosters, art, sound): Zoinkity's
    // Team Extension itself, or a community update built on it. The code regions' XXH3-64 is pinned
    // in config/teamplus.json; ovl_b's data (0x11EC8C-0x12357C) and the asset file system may differ.
    bool teamplus_rom_check(const std::vector<uint8_t>& rom) {
        constexpr size_t rom_size = 0x1000000;
        constexpr std::pair<size_t, size_t> code_regions[] = {{0x0, 0x11EC8C}, {0x12357C, 0x150C40}};
        // 16 MB, or more for a ROM exported by this app (Export ROM): its added files come after the original data.
        if (rom.size() < rom_size || rom.size() > 4 * rom_size) {
            return false;
        }
        XXH3_state_t* state = XXH3_createState();
        XXH3_64bits_reset(state);
        for (auto [start, end] : code_regions) {
            XXH3_64bits_update(state, rom.data() + start, end - start);
        }
        uint64_t hash = XXH3_64bits_digest(state);
        XXH3_freeState(state);
        return hash == 0xB1A70371349CB6C5ULL;
    }
}
#endif

extern "C" void blitz_thread_created(uint8_t* rdram, recomp_context* ctx);  // savestate.cpp

std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash = blitz::rom_hash,
        .internal_name = "blitz2k",
        .display_name = blitz::display_name,
        .game_id = blitz::game_id,
        .mod_game_id = blitz::mod_game_id,
        .save_type = recomp::SaveType::None,
        .thumbnail_bytes = std::span<const char>(icon_bytes),
        .is_enabled = false,
        .has_compressed_code = false,
        .entrypoint_address = (gpr)(int32_t)0x80000400,
        .entrypoint = recomp_entrypoint,
        .thread_create_callback = blitz_thread_created,  // savestate.cpp
        .on_init_callback = blitz::on_init,
#if BLITZ_TEAMPLUS
        .rom_check = blitz::teamplus_rom_check,
#endif
    },
};

namespace blitz {
    // Thread ids from boot_main/idle_thread/sched_init (see NOTES.md "Boot flow and threads").
    std::string get_game_thread_name(const OSThread* t) {
        monitor_register_thread(t);  // called once per created thread; feeds BLITZ_MONITOR
        std::string name = "[Game] ";
        switch (t->id) {
            case 1: name += "IDLE"; break;
            case 6: name += "SCHED_INIT"; break;
            case 7: name += "GAME"; break;
            default: name += std::to_string(t->id); break;
        }
        return name;
    }
}

// `--game blitz2000` skips the launcher and starts the stored ROM (dev runs, tools/run_dev.sh).
static bool autostart_game = false;
// Settings → Restart: quit normally, then main replaces the process with a fresh copy that starts the
// game straight away (same PID, so tools watching it keep working).
static std::atomic<bool> restart_requested{false};
// Settings → Main Menu during a game: the same relaunch, into the launcher instead of the game.
static std::atomic<bool> restart_to_launcher{false};

namespace blitz {
    void log_shutdown();  // app_log.cpp

    void request_restart() {
        if (!ultramodern::is_game_started()) return;  // quitting while the game starts up isn't safe
        restart_requested = true;
        ultramodern::quit();
    }

    // Settings → Main Menu from a game (firstrun.cpp, after its "Leave this game?"): a game can't be stopped and left
    // running underneath, so the app ends and starts again on the launcher. The mark (config.cpp skip_launcher) makes
    // that next start open on the launcher; a phone, which can't start itself again, opens there when it's next opened.
    void leave_to_main_menu() {
        std::error_code ec;
        std::filesystem::create_directories(recompui::file::get_app_folder_path(), ec);
        std::ofstream(recompui::file::get_app_folder_path() / "open_launcher") << "1\n";
        if (blitz::restart_available) {
            restart_requested = true;
            restart_to_launcher = true;
        }
        ultramodern::quit();
    }

    // Reset Data and Restore a Backup (reset_data.cpp): the app ends, its folder is cleared (or restored) on the way
    // out, and on a computer it starts again, on the launcher, where the first run comes up. A phone or console, which
    // can't start itself again, starts over when it's next opened.
    void restart_fresh() {
        fprintf(stderr, "[reset] the app %s\n", blitz::restart_available ? "restarts" : "closes");
        if (blitz::restart_available) {
            restart_requested = true;
            restart_to_launcher = true;
        }
        ultramodern::quit();
    }

    // The Windows resize crash (docs/WINDOWS_VULKAN.md): the live command RESIZE sets a size; BLITZ_RESIZE_STRESS=<s>
    // flips the window between two sizes every <s> seconds, so a run reaches it without anyone at the window.
    static std::atomic<uint32_t> wanted_size{0};  // w << 16 | h, 0: none
    static std::atomic<bool> wanted_centre{false};

    // centre: also centre the window on the display it's mostly on (tools/capture: the whole picture on one screen).
    void request_window_size(int w, int h, bool centre) {
        wanted_centre = centre;
        wanted_size = (uint32_t)std::clamp(w, 1, 0xFFFF) << 16 | (uint32_t)std::clamp(h, 1, 0xFFFF);
    }

    void window_size_update(SDL_Window* window) {
        static const double stress = [] { const char* s = getenv("BLITZ_RESIZE_STRESS"); return s ? atof(s) : 0.0; }();
        if (stress > 0) {
            static uint32_t next = 0;
            static bool big = false;
            uint32_t now = SDL_GetTicks();
            if (now >= next) {
                next = now + (uint32_t)(stress * 1000);
                big = !big;
                request_window_size(big ? 1600 : 960, big ? 900 : 540, false);
            }
        }
        uint32_t want = wanted_size.exchange(0);
        if (want == 0 || window == nullptr) return;
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(window);
        SDL_SetWindowSize(window, (int)(want >> 16), (int)(want & 0xFFFF));
        if (wanted_centre.exchange(false)) {
            int d = SDL_GetWindowDisplayIndex(window);
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(d < 0 ? 0 : d), SDL_WINDOWPOS_CENTERED_DISPLAY(d < 0 ? 0 : d));
        }
    }
}

static void restart_app(char** argv) {
#ifdef _WIN32
    // Windows: a new process with the same arguments (and --game), then this one ends as on a Mac.
    wchar_t exe_w[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe_w, MAX_PATH) == 0) return;
    std::wstring cmd = L"\"" + std::wstring(exe_w) + L"\"";
    for (int i = 1; argv[i] != nullptr; i++) {
        if (strcmp(argv[i], "--game") == 0 && argv[i + 1] != nullptr) { i++; continue; }
        cmd += L" \"" + std::filesystem::path(argv[i]).wstring() + L"\"";
    }
    if (!restart_to_launcher) cmd += L" --game " + std::filesystem::path(blitz::mod_game_id).wstring();
    blitz::log_shutdown();
    _wputenv_s(L"BLITZ_INPUT_LIVE_FROM_END", L"1");
    _wputenv_s(L"BLITZ_WAIT_PID", std::to_wstring(GetCurrentProcessId()).c_str());  // it starts once this one has ended
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe_w, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::exit(0);
    }
    fprintf(stderr, "[restart] CreateProcess failed: %lu\n", GetLastError());
#elif !defined(BLITZ_MACOS) && !defined(BLITZ_PLATFORM_LINUX)
    (void)argv;
    fprintf(stderr, "[restart] not on this platform\n");
#else
    char exe[4096];
#if defined(BLITZ_PLATFORM_LINUX)
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) return;
    exe[len] = '\0';
#else
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0) return;
#endif
    std::vector<std::string> args{exe};
    for (int i = 1; argv[i] != nullptr; i++) {
        if (strcmp(argv[i], "--game") == 0 && argv[i + 1] != nullptr) { i++; continue; }
        args.emplace_back(argv[i]);
    }
    if (!restart_to_launcher) {
        args.emplace_back("--game");
        args.emplace_back(blitz::mod_game_id);
    }
    std::vector<char*> cargs;
    for (std::string& a : args) cargs.push_back(a.data());
    cargs.push_back(nullptr);
    blitz::log_shutdown();  // stdout/stderr back to the terminal; the new copy starts its own log
    setenv("BLITZ_INPUT_LIVE_FROM_END", "1", 1);  // dev: don't replay the live commands that asked for this
    fprintf(stderr, "[restart] relaunching %s\n", exe);
    execv(exe, cargs.data());
    fprintf(stderr, "[restart] execv failed: %s\n", strerror(errno));
#endif
}
// `--import-rom <file>` checks and stores a ROM exactly like the launcher's Load ROM button.
static const char* import_rom_path = nullptr;
static bool exit_after_import = false;  // `--exit-after-import`: the other game handing a ROM over (experience.cpp)
static bool rom_handed_over = false;    // the last pick went to the other game, which already said so

namespace blitz {
    bool experience_hand_over(const std::filesystem::path& rom);  // experience.cpp
    void experience_started();
    void experience_exec(char** argv);
}

// What a player reads when a ROM isn't accepted (the launcher's message box and --import-rom).
static std::string rom_error_text(recomp::RomValidationError error) {
    if (rom_handed_over) {  // the other game took it and asked about switching
        rom_handed_over = false;
        return "";
    }
    switch (error) {
        case recomp::RomValidationError::Good: return "ok, stored";
        case recomp::RomValidationError::FailedToOpen: return "Couldn't read that file. Check it isn't empty and that the app may open it.";
        case recomp::RomValidationError::NotARom:
            return "That isn't an N64 ROM. Pick the game's .z64, .n64 or .v64 file, or a .zip holding one.";
        case recomp::RomValidationError::IncorrectRom: return "That's an N64 ROM, but not NFL Blitz 2000.";
        case recomp::RomValidationError::NotYet: return "That version of the game isn't supported yet.";
#if BLITZ_TEAMPLUS
        case recomp::RomValidationError::IncorrectVersion:
            // BRANDING: ROM error text names the app
            return "That's NFL Blitz 2000, but not a ROM with the Team Extension (32 teams). Modern Blitz needs the Team "
                   "Extension ROM, or a roster update built on it. The plain USA ROM plays in Classic Blitz.";
#elif BLITZ_MODERN
        case recomp::RomValidationError::IncorrectVersion:
            // BRANDING: ROM error text names the app
            return "That's NFL Blitz 2000, but not the USA v1.1 copy New Blitz plays: it may be another version or "
                   "region, damaged, or modified.";
#else
        case recomp::RomValidationError::IncorrectVersion:
            // BRANDING: ROM error text names the app
            return "That's NFL Blitz 2000, but not the USA v1.1 copy Classic Blitz needs: it may be another version or "
                   "region, damaged, or modified.";
#endif
        case recomp::RomValidationError::OtherError:
            return "The ROM is fine, but it couldn't be saved in the app folder (" + recompui::file::get_app_folder_path().string() +
                   "). Check there's free space.";
        default: return "Something went wrong reading that file.";
    }
}

// Load ROM and --import-rom: a .z64/.n64/.v64 file, or a .zip holding one (unpacked to a temporary file in the
// app folder, then checked and stored exactly like a loose ROM).
static recomp::RomValidationError import_rom(const std::filesystem::path& path, std::u8string& game_id) {
    std::error_code ec;
    std::filesystem::path folder = recompui::file::get_app_folder_path();
    std::filesystem::create_directories(folder, ec);  // a fresh install has no app folder yet
    char magic[4] = {};
    {
        std::ifstream f(path, std::ios::binary);
        f.read(magic, sizeof magic);
    }
    if (std::memcmp(magic, "PK\x03\x04", 4) != 0) {
        return recomp::select_rom(path, game_id);
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.string().c_str(), 0)) {
        return recomp::RomValidationError::NotARom;
    }
    // The biggest .z64/.n64/.v64 inside (any folder), up to 64 MB.
    int pick = -1;
    mz_uint64 pick_size = 0;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        std::string name = st.m_filename;
        std::string ext = name.size() >= 4 ? name.substr(name.size() - 4) : "";
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".z64" && ext != ".n64" && ext != ".v64") continue;
        if (st.m_uncomp_size > 64ull * 1024 * 1024 || st.m_uncomp_size <= pick_size) continue;
        pick = (int)i;
        pick_size = st.m_uncomp_size;
    }
    if (pick < 0) {
        mz_zip_reader_end(&zip);
        return recomp::RomValidationError::NotARom;
    }
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)pick, &size, 0);
    mz_zip_reader_end(&zip);
    if (data == nullptr) {
        return recomp::RomValidationError::FailedToOpen;
    }
    std::filesystem::path tmp = folder / ".import-rom.tmp";
    bool written = false;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), (std::streamsize)size);
        written = (bool)out;
    }
    mz_free(data);
    recomp::RomValidationError result = written ? recomp::select_rom(tmp, game_id) : recomp::RomValidationError::OtherError;
    std::filesystem::remove(tmp, ec);
    return result;
}

#if BLITZ_TEAMPLUS
namespace blitz {
    bool rom_data_rebranded(std::span<const uint8_t> rom);  // art_dropin.cpp
    std::string rom_display_name(std::span<const uint8_t> rom);  // known_roms.cpp
    extern std::atomic<int> stored_rom_changes;
    void set_rosters(uint32_t value);                       // season_settings.cpp: 0 Auto, 1 Blitz 2026, 2 the ROM's own
}

// The launcher's Load ROM / Change ROM: a community release with its own branding (Resurrection's Gold or Silver
// Edition) asks whose rosters to play; the Team Extension itself goes back to Auto (Blitz 2026).
static recomp::RomValidationError launcher_import_rom(const std::filesystem::path& path, std::u8string& game_id) {
    recomp::RomValidationError result = import_rom(path, game_id);
    if (result != recomp::RomValidationError::Good) return result;
    blitz::stored_rom_changes++;
    std::ifstream f(recomp::get_config_path() / (game_id + u8".z64"), std::ios::binary);
    std::vector<uint8_t> rom{std::istreambuf_iterator<char>(f), {}};
    if (!blitz::rom_data_rebranded(rom)) {
        blitz::set_rosters(0);
        return result;
    }
    std::string name = blitz::rom_display_name(rom);
    if (name.empty()) name = path.stem().string();
    recompui::open_choice_prompt(
        "Pick your rosters",
        name + " has its own art and teams. Play its rosters, or the 2026 Season's "
               "with its art? You can change this any time in Settings → Teams → Rosters.",
        "2026 Season", "Its Own",
        [] { blitz::set_rosters(1); },
        [] { blitz::set_rosters(2); },
        recompui::ButtonStyle::Primary, recompui::ButtonStyle::Secondary, false);
    return result;
}
#endif

namespace blitz {
    // First run's Load ROM and Settings → Change ROM (firstrun.cpp): the launcher's own import (zips, the other game's
    // hand-over, Modern's roster question), and what to tell the player when it's refused. kind: 1 not the game (or
    // not a ROM), 2 the game but not the v1.1 copy (another version, region, or a bad dump), 3 couldn't read or keep it.
    bool store_rom(const std::filesystem::path& path, int& kind, std::string& message) {
        std::u8string gid = blitz::game_id;
#if BLITZ_TEAMPLUS
        recomp::RomValidationError r = launcher_import_rom(path, gid);
#else
        recomp::RomValidationError r = import_rom(path, gid);
#endif
        kind = 0;
        if (r == recomp::RomValidationError::Good) {
            recomp::check_all_stored_roms();  // so recomp::is_rom_valid agrees for the rest of this session
            fprintf(stderr, "[rom] stored from %s\n", path.filename().string().c_str());
            return true;
        }
        if ((r == recomp::RomValidationError::IncorrectVersion || r == recomp::RomValidationError::IncorrectRom) &&
            blitz::experience_hand_over(path)) {
            rom_handed_over = true;
        }
        kind = r == recomp::RomValidationError::IncorrectVersion ? 2
             : r == recomp::RomValidationError::FailedToOpen || r == recomp::RomValidationError::OtherError ? 3 : 1;
        // BRANDING: first run's wrong-ROM text names the app
        message = kind == 1 ? "That's not the right game. New Blitz needs NFL Blitz 2000 for the N64, USA, v1.1."
                : kind == 2 ? "That's NFL Blitz 2000, but not the USA v1.1 copy New Blitz needs. A different region or "
                              "version won't work, and a damaged file looks the same: try dumping it again."
                            : rom_error_text(r);
        rom_handed_over = false;
        fprintf(stderr, "[rom] %s: not stored (%d)\n", path.filename().string().c_str(), (int)r);
        return false;
    }
}

namespace blitz {
    void crash_report_at_start();  // crash_report.cpp
    void dev_input_early();        // dev_input.cpp
    void log_init(const std::filesystem::path& app_folder, const std::string& version);  // app_log.cpp
    void firstrun_app_up(const std::filesystem::path& app_folder, const std::string& version);  // firstrun.cpp
    void reset_data_pending(const std::filesystem::path& app_folder, const char* when);  // reset_data.cpp
    void firstrun_settings_loaded();
    void style_launcher(recompui::LauncherMenu* menu, recompui::GameOptionsMenu* options);
    bool skip_launcher();  // config.cpp: Settings → General → Launcher at Start off (phones' default)
}

// Going straight into the game (--game, or Launcher at Start off once Blitz Builder has run), decided once as the
// launcher is made: the settings are loaded by then. Blitz Builder's Customize sets welcomed mid-launcher, and a
// decision taken later started the game from under the launcher the player had just gone back to.
static bool launcher_autostart = false;

static recompui::LauncherMenu* launcher_menu = nullptr;

namespace blitz {
    // The launcher, back in view (setup's Exit and Done, Settings → Main Menu outside a game): it may have been kept
    // out of sight for a start straight into the game (on_launcher_init).
    void show_launcher() {
        recompui::ContextId launcher = recompui::get_launcher_context_id();
        if (launcher == recompui::ContextId::null()) return;
        if (launcher_menu != nullptr) {
            bool opened = launcher.open_if_not_already();
            launcher_menu->set_display(recompui::Display::Block);
            if (opened) launcher.close();
        }
        if (!recompui::is_context_shown(launcher)) recompui::show_context(launcher, "");
    }
}

void on_launcher_init(recompui::LauncherMenu* menu) {
    launcher_menu = menu;
    auto game_options_menu = menu->init_game_options_menu(
        supported_games[0].game_id,
        supported_games[0].mod_game_id,
        supported_games[0].display_name,
        supported_games[0].thumbnail_bytes
    );
    blitz::style_launcher(menu, game_options_menu);  // launcher.cpp: logo and options
    launcher_autostart = autostart_game || blitz::skip_launcher();
    // The launcher has to be up for its first 30 frames (on_launcher_update) but isn't for seeing then: it flashed at
    // every start on a phone (logo, PLAY ... BLITZ BUILDER) before the game. Shown if the game doesn't start after all.
    if (launcher_autostart) menu->set_display(recompui::Display::None);
}

// Autostart from the launcher's per-frame update, a few frames in, like a quick click on Start.
// Starting from the init callback is too early: the VI thread only installs its placeholder
// VI mode while no game is running, so a start before its first tick leaves it with a null mode.

namespace blitz {
    // Launcher → Blitz Party: a party set up in one click. TV Mode, Phone Controllers, the Party
    // rules, no intro or Rumble Pak screens; then the game starts and the Players tab opens on the QR
    // code so everyone can join. All of these stay as settings (turn any off in Settings).
    static std::atomic<uint32_t> party_started_at{0};
    static std::atomic<bool> party_requested{false};
    void request_blitz_party() { party_requested = true; }  // dev: the live command "PARTY"
    static bool is_launcher_ready_for_party() {
        static int frames = 0;
        return ++frames > 30;
    }

    void start_blitz_party() {
        std::u8string game_id = blitz::game_id;
        if (!recomp::is_rom_valid(game_id)) {
            recompui::message_box("Load the game's ROM first (Start Game shows how).");
            return;
        }
        recomp::config::Config& g = recompui::config::get_general_config();
        for (const char* id : {"tv_mode", "phone_controllers", "skip_intros", "skip_rumble_screen"}) {
            g.update_option_value(id, true);
        }
        g.save_config();
        recomp::config::Config& rules = recompui::config::get_config("rules");
        rules.update_option_value("preset", (uint32_t)2);  // Party
        rules.save_config();
        recompui::update_game_mod_id(blitz::mod_game_id);
        recomp::start_game(game_id, {});
        recompui::hide_all_contexts();
        party_started_at = SDL_GetTicks();
        fprintf(stderr, "[party] Blitz Party started\n");
    }

    // The first-start welcome modal is gone: the hints (hints.cpp) cover the same ground without stopping play.

    // update_gfx: a couple of seconds into a party, the Players tab with the QR code.
    void party_update() {
        uint32_t t = party_started_at;
        if (t == 0 || SDL_GetTicks() - t < 2500) return;
        party_started_at = 0;
        recompui::config::open();
        recompui::config::set_tab("players");
    }
}

void on_launcher_update(recompui::LauncherMenu* menu) {
    static bool dev_party = getenv("BLITZ_DEV_PARTY") != nullptr;  // tests: as if Blitz Party was clicked
    if (dev_party && blitz::is_launcher_ready_for_party()) {
        dev_party = false;
        blitz::party_requested = true;
    }
    if (blitz::party_requested.exchange(false)) {
        blitz::start_blitz_party();
        return;
    }
    // First run's start: the launcher's own frames, not a timer (firstrun.cpp).
    static int launcher_frames = 0;
    if (launcher_frames < 1000) blitz::firstrun_launcher_frame(++launcher_frames);
    static int frames = 0;
    if (++frames != 30) {
        return;
    }
    // --game, or Launcher at Start off unless Shift is held (to reach the launcher anyway).
    if (!launcher_autostart) {
        return;
    }
    std::u8string game_id = blitz::game_id;  // is_rom_valid takes a non-const reference
    if ((autostart_game || !(SDL_GetModState() & KMOD_SHIFT)) && recomp::is_rom_valid(game_id)) {
        // Same as the launcher's Start Game option (RecompFrontend ui_launcher.cpp).
        recompui::update_game_mod_id(blitz::mod_game_id);
        recomp::start_game(game_id, {});
        recompui::hide_all_contexts();
        return;
    }
    if (!recomp::is_rom_valid(game_id)) {
        fprintf(stderr, "[launcher] no valid stored ROM to start; the launcher stays (Load ROM).\n");
    }
    menu->set_display(recompui::Display::Block);  // kept out of sight for the start that didn't happen (on_launcher_init)
}

void enable_texture_pack(recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
    recompui::renderer::enable_texture_pack(context, mod);
}

void disable_texture_pack(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    recompui::renderer::disable_texture_pack(mod);
}

void reorder_texture_pack(recomp::mods::ModContext&) {
    recompui::renderer::trigger_texture_pack_update();
}

int main(int argc, char** argv) {
    blitz::early_init();  // the platform's first step (blitz_host.h): Linux finds its resources
    BOOT_CRUMB("main");
#ifdef _WIN32
    // Windows' C runtime ends the app on any invalid argument to a library call (no message, exit 0xC0000409): log
    // it instead, and let the call fail (the Parallels test, docs/hardware/windows-parallels-2026-10-21).
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        fprintf(stderr, "[crt] a C library call got an invalid argument (ignored)\n");
    });
    // Unbuffered: Windows has no line buffering, and its buffered modes need a real size (size 0 was that crash).
    setvbuf(stdout, nullptr, _IONBF, 0);
#else
    // Line-buffer stdout so diagnostics (e.g. RSPRecomp's "Unhandled jump target") survive the
    // runtime's quick exit when output goes to a file.
    setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
#ifdef _WIN32
    // Restart and Switch start the new process before this one ends (there's no exec): it waits for the old one,
    // which still holds blitz.log and the phone port.
    if (const char* wait = getenv("BLITZ_WAIT_PID"); wait != nullptr && *wait != '\0') {
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)strtoul(wait, nullptr, 10))) {
            WaitForSingleObject(old, 10000);
            CloseHandle(old);
        }
        _putenv_s("BLITZ_WAIT_PID", "");
    }
    // Sharp at 125-200% display scaling: SDL makes the app DPI aware and sizes the window in scaled units.
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
    // The app's resources sit next to the .exe; the frontend finds them through BLITZ_RESOURCES (it has no Windows
    // lookup of its own), so a shortcut or another working folder still finds the fonts, assets and packs.
    if (getenv("BLITZ_RESOURCES") == nullptr) {
        wchar_t exe_w[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe_w, MAX_PATH) != 0)
            _wputenv_s(L"BLITZ_RESOURCES", std::filesystem::path(exe_w).parent_path().wstring().c_str());
    }
    // A virtual machine without a Vulkan driver (Parallels on a Mac): a Mesa driver folder beside the .exe is used
    // instead, "dozen" (Vulkan on Direct3D 12: the VM's GPU, or Windows' WARP with DZN_DEBUG=warp) before "lavapipe"
    // (software Vulkan); BLITZ_VULKAN_DRIVER=<folder> picks one. Each holds the driver DLL and its .json.
    if (getenv("VK_DRIVER_FILES") == nullptr) {
        wchar_t exe_w[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe_w, MAX_PATH) != 0) {
            std::filesystem::path dir = std::filesystem::path(exe_w).parent_path();
            std::vector<std::string> folders{"dozen", "lavapipe"};
            if (const char* pick = getenv("BLITZ_VULKAN_DRIVER")) folders.insert(folders.begin(), pick);
            std::error_code ec;
            for (const std::string& folder : folders) {
                std::filesystem::path icd;
                for (const auto& f : std::filesystem::directory_iterator(dir / folder, ec))
                    if (f.path().extension() == ".json") icd = f.path();
                if (icd.empty()) continue;
                _wputenv_s(L"VK_DRIVER_FILES", icd.wstring().c_str());
                _wputenv_s(L"VK_ICD_FILENAMES", icd.wstring().c_str());  // older Vulkan loaders
                fprintf(stderr, "[vulkan] using the %s driver beside the app\n", folder.c_str());
                break;
            }
        }
    }
#endif
#ifdef __PROSPERO__
    // The system's launch splash goes first (as UnleashedRecomp-Prospero): an early failure must not leave it up.
    sceSystemServiceHideSplashScreen();
    // SDL's PS5 video driver would take the TV output that RT64 draws to (Plume's display surface): SDL keeps a
    // window-less dummy video driver for events and pads, as UnleashedRecomp-Prospero does.
    setenv("SDL_VIDEODRIVER", "dummy", 1);
#endif
#ifdef BLITZ_IOS
    // Landscape only, always. Without this SDL derives the allowed orientations from the window's last size, so one
    // portrait layout (the screenshot sheet, the app switcher) locked the game into portrait: a magenta screen.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, getenv("BLITZ_DS_STACK") ? "Portrait" : "LandscapeLeft LandscapeRight");
#endif
#ifdef __ANDROID__
    // Potato Mode on a strong device: threads started from here on inherit this.
    if (getenv("BLITZ_POTATO_CORES") != nullptr) {
        cpu_set_t cores;
        CPU_ZERO(&cores);
        for (int c = 0; c < 4; c++) CPU_SET(c, &cores);
        int failed = sched_setaffinity(0, sizeof(cores), &cores);
        fprintf(stderr, "[device] potato mode: four cores%s\n", failed != 0 ? " FAILED" : "");
    }
#endif
    // Before SDL/AppKit start: no accent picker when a key is held (see macos_support.mm).
    blitz::disable_press_and_hold();

    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--game") == 0 && blitz::mod_game_id == argv[i + 1]) {
            autostart_game = true;
            blitz::builder_autostarted();  // no setup questions over a game started from the command line
        }
        if (strcmp(argv[i], "--import-rom") == 0) {
            import_rom_path = argv[i + 1];
        }
        if (strcmp(argv[i + 1], "--exit-after-import") == 0) {
            exit_after_import = true;
        }
    }

    recomp::Version project_version{};
    if (!recomp::Version::from_string(version_string, project_version)) {
        ultramodern::error_handling::message_box(("Invalid version string: " + version_string).c_str());
        return EXIT_FAILURE;
    }

#if BLITZ_HAS_FILE_DIALOG_NFD
    NFD_Init();
#endif

    recompui::programconfig::set_program_name(blitz::program_name);
    recompui::programconfig::set_program_id(blitz::program_id);
    // From here on, output also goes to <app folder>/blitz.log (app_log.cpp), with crashes noted.
    BOOT_CRUMB("before log_init");
    blitz::log_init(recompui::file::get_app_folder_path(), version_string);
    BOOT_CRUMB("after log_init");
    // A Reset Data or Restore that didn't finish on the way out last time (reset_data.cpp): before anything reads the folder.
    blitz::reset_data_pending(recompui::file::get_app_folder_path(), "start");
    blitz::firstrun_app_up(recompui::file::get_app_folder_path(), version_string);  // [firstrun] line 1

    BOOT_CRUMB("SDL audio");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL audio init failed: %s\n", SDL_GetError());
    }
    if (!blitz::audio::reset(48000)) {
#if !defined(BLITZ_PLATFORM_DESKTOP)
        // A console or phone with no audio output (an emulator started without sound): play on, silently.
        fprintf(stderr, "[audio] no audio output; playing without sound\n");
#else
        return EXIT_FAILURE;
#endif
    }

    std::u8string controller_db_path = (recompui::file::get_program_path() / "recompcontrollerdb.txt").u8string();
#ifdef __ANDROID__
    // Two screens: the pad's events can arrive through the other screen's window, with this one unfocused.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // An RG DS on GammaOS says it's an Xbox pad, but its A is on the right, as on a Switch. Buttons go by position.
    if (getenv("BLITZ_NINTENDO_PAD") != nullptr) {
        SDL_GameControllerAddMapping("05007ad15e040000130b0000ffff3f00,Xbox Series X Controller,a:b1,b:b0,back:b4,dpdown:b12,dpleft:b13,"
                                     "dpright:b14,dpup:b11,guide:b5,leftshoulder:b9,leftstick:b7,lefttrigger:a4,leftx:a0,lefty:a1,"
                                     "rightshoulder:b10,rightstick:b8,righttrigger:a5,rightx:a2,righty:a3,start:b6,x:b3,y:b2,platform:Android");
    }
#endif
    if (SDL_GameControllerAddMappingsFromFile(reinterpret_cast<const char*>(controller_db_path.c_str())) < 0) {
        fprintf(stderr, "Failed to load controller mappings: %s\n", SDL_GetError());
    }

    recompui::register_primary_font("InterVariable.ttf", "Inter Variable");
    recompui::register_extra_font("NotoEmoji-Regular.ttf");
    recompui::register_extra_font("BarlowCondensed-ExtraBoldItalic.ttf");  // the Blitz '27 display face (OFL)
    recompui::register_extra_font("BarlowCondensed-BoldItalic.ttf");
    blitz::ui::register_decorators();  // ui_kit.cpp: the Blitz '27 look for framework panels (prompts)

    recomp::register_config_path(recompui::file::get_app_folder_path());

    // supported_games is filled during static initialization, and it copies blitz_config.h's inline strings. Their
    // order across translation units isn't guaranteed: on Windows (MSVC's 15-character small-string limit) a
    // 20-character game_id needs a heap copy at run time and was still empty when copied, so 0.9.2 stored the ROM as
    // "<app folder>/.z64" under the id "" (the setup questions never opened: they ask for the real id). Set them
    // again here, after every static initializer, and move a ROM stored by 0.9.2 to its proper name.
    supported_games[0].game_id = blitz::game_id;
    supported_games[0].mod_game_id = blitz::mod_game_id;
    supported_games[0].display_name = blitz::display_name;
    {
        std::error_code ec;
        const std::filesystem::path app = recompui::file::get_app_folder_path();
        const std::filesystem::path nameless = app / ".z64", proper = app / (blitz::game_id + u8".z64");
        if (std::filesystem::is_regular_file(nameless, ec) && !std::filesystem::exists(proper, ec)) {
            std::filesystem::rename(nameless, proper, ec);
            fprintf(stderr, "[rom] the ROM stored without a name (0.9.2 on Windows) moved to %s: %s\n",
                    reinterpret_cast<const char*>(proper.filename().u8string().c_str()), ec ? ec.message().c_str() : "ok");
        }
    }

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

#if defined(BLITZ_IOS)
    // An iPhone app made on a computer (Blitz Builder → Phone → iPhone, device_send.cpp save_ipa) carries the ROM and
    // that computer's setup inside the app, in setup/ (team, 2026-11-15). Taken in once per .ipa made (its builder_done
    // says when it was made; re-signing the same one every 7 days doesn't take it in again): the setup files go to the
    // drop folder, which the step below moves into the app folder as it does a computer's send; the ROM is stored
    // straight from the app, when none is stored yet. An .ipa without setup/ (the one a phone downloads itself) asks
    // for the ROM with the Files picker, as before.
    {
        const std::filesystem::path setup = recompui::file::get_program_path() / "setup", drop = blitz::rom_drop_folder();
        const std::filesystem::path seen = recompui::file::get_app_folder_path() / "setup_imported";
        std::error_code ec;
        std::string mark, last;
        std::getline(std::ifstream(setup / "builder_done"), mark);
        std::getline(std::ifstream(seen), last);
        if (!mark.empty() && mark != last && !drop.empty()) {
            std::filesystem::create_directories(drop, ec);
            for (const auto& e : std::filesystem::directory_iterator(setup, ec)) {
                if (e.path().extension() == ".z64") continue;
                std::error_code cec;
                std::filesystem::copy_file(e.path(), drop / e.path().filename(), std::filesystem::copy_options::overwrite_existing, cec);
                fprintf(stderr, "[setup] %s from the app: %s\n", e.path().filename().string().c_str(), cec ? cec.message().c_str() : "ok");
            }
            std::error_code sec;
            if (!std::filesystem::exists(recomp::get_config_path() / (std::u8string(blitz::game_id) + u8".z64"), sec) &&
                std::filesystem::is_regular_file(setup / "blitz.z64", sec)) {
                std::u8string candidate = blitz::game_id;
                recomp::RomValidationError result = import_rom(setup / "blitz.z64", candidate);
                fprintf(stderr, "[rom] the ROM inside the app: %s\n", rom_error_text(result).c_str());
            }
            std::filesystem::create_directories(seen.parent_path(), ec);
            std::ofstream(seen) << mark << "\n";
        }
    }
#endif
#if BLITZ_HAS_ROM_DROP_FOLDER
    // A computer's Blitz Builder sends its setup (Season, Rules) and the mark that its questions were answered
    // (builder_done, read by config.cpp) here too: a fresh install has no app folder until it first runs, so they're
    // moved into it now, before the settings load.
    {
        const std::filesystem::path drop = blitz::rom_drop_folder(), app = recompui::file::get_app_folder_path();
        std::error_code ec;
        for (const char* f : {"season.json", "rules.json", "builder_done"}) {
            if (drop.empty() || drop == app || !std::filesystem::is_regular_file(drop / f, ec)) continue;
            // One the game couldn't remove last time (another owner: the PS5's FTP server writes as root) is taken in
            // once, not at every start over the player's own changes: its size and time are noted beside the setup.
            const std::filesystem::path taken = app / (std::string(".taken-") + f);
            std::string stamp = std::to_string((unsigned long long)std::filesystem::file_size(drop / f, ec)) + " " +
                                std::to_string((long long)std::filesystem::last_write_time(drop / f, ec).time_since_epoch().count());
            std::string seen;
            std::getline(std::ifstream(taken), seen);
            if (seen == stamp) continue;
            std::filesystem::create_directories(app, ec);
            ec.clear();
            std::filesystem::copy_file(drop / f, app / f, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {  // a copy there that can't be written over (another owner): this app's own copy in its place
                std::error_code ec2;
                std::filesystem::remove(app / f, ec2);
                ec.clear();
                std::filesystem::copy_file(drop / f, app / f, std::filesystem::copy_options::overwrite_existing, ec);
            }
            fprintf(stderr, "[setup] %s from %s: %s\n", f, drop.string().c_str(), ec ? ec.message().c_str() : "moved in");
            if (ec) continue;
            std::filesystem::remove(drop / f, ec);
            if (ec) {
                fprintf(stderr, "[setup] couldn't remove %s from %s (%s): noted, so it isn't taken in again\n", f,
                        drop.string().c_str(), ec.message().c_str());
                std::ofstream(taken) << stamp << "\n";
            }
        }
    }
    // A ROM (or a zip holding one) put in the device's drop folder (blitz::rom_drop_folder: the PS5's
    // /data/blitzrecomp over FTP, iOS's Documents from Finder or the Files app, Android's app files folder) is imported
    // at startup while no ROM is stored yet, beside Load ROM's document picker on a phone. Loose ROMs before zips,
    // macOS's ._ copies skipped, each tried until one is the right game. (Not is_rom_valid: the stored ROMs are only
    // checked once the runtime starts, so it said no every time and the phone re-imported the ROM at each launch.
    // A stored ROM that turns out bad is removed by that check, and the next start imports again.)
    std::error_code stored_ec;
    if (import_rom_path == nullptr &&
        !std::filesystem::exists(recomp::get_config_path() / (std::u8string(blitz::game_id) + u8".z64"), stored_ec)) {
        const std::filesystem::path rom_folder = blitz::rom_drop_folder();
        std::vector<std::filesystem::path> roms, zips;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(rom_folder, ec)) {
            std::string name = entry.path().filename().string();
            if (name.rfind("._", 0) == 0 || !entry.is_regular_file(ec)) continue;
            std::string ext = entry.path().extension().string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext == ".z64" || ext == ".n64" || ext == ".v64") roms.push_back(entry.path());
            else if (ext == ".zip") zips.push_back(entry.path());
        }
        std::sort(roms.begin(), roms.end());
        std::sort(zips.begin(), zips.end());
        bool stored = false;
        for (const auto* list : {&roms, &zips}) {
            for (const std::filesystem::path& rom : *list) {
                if (stored) break;
                std::u8string candidate = blitz::game_id;
                recomp::RomValidationError result = import_rom(rom, candidate);
                fprintf(stderr, "[rom] %s: %s\n", rom.string().c_str(), rom_error_text(result).c_str());
                stored = result == recomp::RomValidationError::Good;
            }
        }
        if (roms.empty() && zips.empty()) fprintf(stderr, "[rom] no ROM in %s yet\n", rom_folder.string().c_str());
    }
#endif

    if (import_rom_path != nullptr) {
        // On a fresh install nothing has created the app folder yet, and select_rom doesn't.
        std::error_code ec;
        std::filesystem::create_directories(recompui::file::get_app_folder_path(), ec);
        if (ec) {
            fprintf(stderr, "--import-rom: can't create %s: %s\n", recompui::file::get_app_folder_path().string().c_str(), ec.message().c_str());
        }
        std::u8string game_id = blitz::game_id;
        recomp::RomValidationError result = import_rom(import_rom_path, game_id);
        fprintf(stderr, "--import-rom %s: %s\n", import_rom_path, rom_error_text(result).c_str());
        if (exit_after_import) return result == recomp::RomValidationError::Good ? 0 : 1;
    }
    blitz::experience_started();
#if BLITZ_MODERN
    blitz::paks_at_launch();       // a 2026 Season pak sent to this device; the season server's switches (season_pak.cpp)
    blitz::rosters_check_start();  // new weekly lineups, in the background
#endif

    // The launcher's Load ROM: zips, these messages (and rosters, in Modern). A ROM for the other game goes to it
    // when it's in the same app (experience.cpp).
    recompui::set_rom_import([](const std::filesystem::path& path, std::u8string& gid) {
#if BLITZ_TEAMPLUS
        recomp::RomValidationError r = launcher_import_rom(path, gid);
#else
        recomp::RomValidationError r = import_rom(path, gid);
#endif
        if ((r == recomp::RomValidationError::IncorrectVersion || r == recomp::RomValidationError::IncorrectRom) &&
            blitz::experience_hand_over(path)) {
            rom_handed_over = true;
        }
        return r;
    }, rom_error_text);
    recompui::register_ui_exports();
    blitz::register_overlays();

    recompinput::players::set_single_player_mode(true);
    blitz::install_expires();  // [install] signed until <date> (a sideloaded iPhone or iPad)
    blitz::init_config();
    blitz::firstrun_settings_loaded();  // [firstrun] lines 2 and 3
    blitz::dev_input_early();           // dev: live commands reach the launcher and setup (tools/live.sh)
    blitz::crash_report_at_start();     // the last run crashed and reports are on: one goes, in the background
    if (blitz::restart_available) recompui::config::set_restart_callback(blitz::request_restart);  // the header's Restart
    // A game in progress: offer to save it (save states), else Resume Interrupted Games (experimental).
    recompui::set_quit_prompt_override([] { return blitz::savestate_quit_prompt() || blitz::checkpoint_quit_prompt(); });
    recompui::register_launcher_init_callback(on_launcher_init);
    recompui::register_launcher_update_callback(on_launcher_update);

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = [](uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
#ifdef __ANDROID__
            // The surface as it is now, waited for if the app's in the background: create_window's may be gone by this time
            // (platform_android.cpp).
            window_handle.window = blitz::render_window(window_handle.window);
#endif
            auto presentation_mode = ultramodern::renderer::PresentationMode::PresentEarly;
            return recompui::renderer::create_render_context(rdram, window_handle, presentation_mode, developer_mode);
        },
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };

    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = blitz::audio::queue_samples,
        .get_frames_remaining = blitz::audio::get_frames_remaining,
        .set_frequency = blitz::audio::set_frequency,
    };

    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = recompinput::poll_inputs,
        .get_input = blitz::get_input,  // real input plus BLITZ_INPUT scripted presses (dev_input.cpp)
        .set_rumble = blitz::port_set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };

    ultramodern::events::callbacks_t thread_callbacks{
        .vi_callback = [] {
            blitz::ports_update_rumble();
            blitz::fps_vi_tick();
        },
        .gfx_init_callback = nullptr,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = recompui::message_box,
    };

    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = blitz::get_game_thread_name,
    };

    recomp::mods::ModContentType texture_pack_content_type{
        .content_filename = "rt64.json",
        .allow_runtime_toggle = true,
        .on_enabled = enable_texture_pack,
        .on_disabled = disable_texture_pack,
        .on_reordered = reorder_texture_pack,
    };
    auto texture_pack_content_type_id = recomp::mods::register_mod_content_type(texture_pack_content_type);
    recomp::mods::register_mod_container_type("rtz", std::vector{ texture_pack_content_type_id }, false);

    BOOT_CRUMB("recomp::start (launcher next)");
    recomp::start(
        project_version,
        {},
        rsp_callbacks,
        renderer_callbacks,
        audio_callbacks,
        input_callbacks,
        gfx_callbacks,
        thread_callbacks,
        error_handling_callbacks,
        threads_callbacks
    );

#if BLITZ_HAS_FILE_DIALOG_NFD
    NFD_Quit();
#endif
    blitz::capture_shutdown();  // finish a clip being recorded
    // Reset Data or Restore a Backup asked for this quit: the folder is cleared (or restored) now, with the game stopped.
    blitz::reset_data_pending(recompui::file::get_app_folder_path(), "quit");
    blitz::experience_exec(argv);  // Switch to Classic / Modern (one app, docs/design/ONE_APP.md)
    if (restart_requested) {
        restart_app(argv);
    }
#if defined(BLITZ_IOS)
    // iOS keeps an app running when SDL_main returns, so Quit left it on screen (the crash after it used to close it, and
    // went with n64modernruntime 0014). The game's other threads may still be running: end the process here, the log first.
    fprintf(stderr, "[quit] the process ends\n");
    blitz::log_shutdown();
    _exit(0);
#endif
    return EXIT_SUCCESS;
}
