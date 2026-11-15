// Scripted controller input for development: BLITZ_INPUT presses buttons on controller 1 at
// set times, on top of real input. Lets runs be reproduced without anyone at the keyboard.
//
//   BLITZ_INPUT="12 START 0.15; 15 A 0.15; 18 DOWN 0.3; 20 STICK 0,-1 0.5"
//
// Each event: <seconds since the game started> <what> [hold seconds, default 0.15].
// <what> is one or more of A B Z START L R UP DOWN LEFT RIGHT CUP CDOWN CLEFT CRIGHT joined with
// '+', or STICK x,y (each -1..1). Events may overlap. A "P2:" .. "P4:" prefix (P2:A, P3:STICK 1,0)
// presses on that player's controller instead: a virtual one (BLITZ_VIRTUAL_PADS, controllers.cpp).
//
// Live control: with BLITZ_INPUT_LIVE=<file>, lines appended to that file while the game runs are
// pressed as soon as they are read: "<what> [hold seconds]" (no time). tools/live.sh drives it.
// More live commands: "GFX key=value ..." changes a graphics option for this session only
// (ar=Original|Expand, hr=Original|Clamp16x9|Full, rr=Original|Display|Manual, rr_manual=N),
// "DUMP" writes an RDRAM dump (see BLITZ_DUMP_RDRAM_AT), "PAD -2" / "PAD +2" unplugs / plugs in
// virtual controller 2 (BLITZ_VIRTUAL_PADS), and "RESTART" does what Settings → Restart does.
#include <chrono>
#include <filesystem>
#ifdef __ANDROID__
#include <sched.h>
#endif
#include "SDL2/SDL.h"

#include "ultramodern/ultramodern.hpp"
#include <fstream>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "recomp.h"
#include "recompinput/profiles.h"
#include "recompui/recompui.h"
#include "ultramodern/config.hpp"

extern "C" void blitz_savestate_request(int op);  // savestate.cpp
extern "C" void blitz_menu_step_input(int port, uint16_t* buttons);
extern "C" void blitz_replay_input(int port, uint16_t* buttons, float* x, float* y);  // replay.cpp
extern "C" void blitz_rt64_render_to_ram(int mode);  // rt64_raster_shader_cache.cpp
extern "C" void blitz_rt64_fast_filter(int mode);     // rt64_framebuffer_renderer.cpp (BLITZ_FAST_FILTER)
extern "C" void blitz_rt64_present_skip(int rule);    // rt64_present_queue.cpp (BLITZ_PRESENT_SKIP)
extern "C" void blitz_rt64_picture_first(int on);     // rt64_present_queue.cpp (BLITZ_DS_PICTURE_FIRST)
extern "C" void blitz_rt64_ui_every(int every);       // rt64_present_queue.cpp (BLITZ_DS_UI_EVERY)

namespace blitz {
    void ds_tap(int screen, float x, float y);  // ds_view.cpp
    void replay_open();                         // replay_screen.cpp (the live command REPLAY)
    void replay_close();
    void replay_save();
    void send_to_ps5(const std::string& ip);  // device_send.cpp (the live command SEND)
    void send_to_iphone();
    void send_to_android();
    void save_iphone_ipa();
}  // savestate.cpp: menu steps (Redo goes back)

namespace blitz {
    uint8_t* monitor_rdram();  // dev_monitor.cpp
    void dev_dump_rdram();  // dev_monitor.cpp
    void request_restart();  // main.cpp
    void request_blitz_party();
    void hints_preview(const std::string& id);  // hints.cpp
    void export_rom_now_dev();                   // export_rom.cpp
    void export_screen_dev();                    // export_rom.cpp
    void request_window_size(int w, int h, bool centre = false);  // main.cpp (the window is the main thread's)
    void dev_set_option(const std::string& tab, const std::string& option, const std::string& value);  // controllers.cpp
    void dev_virtual_pad(bool attach, int number);  // controllers.cpp
    void dev_open_menu(const std::string& tab);
    void roster_room_open();  // roster_room.cpp (Modern; Classic has a stub)
    void tall_set(const std::string& name, float value);  // tall_view.cpp
}

namespace {
    struct InputEvent {
        double start, end;
        int port = 0;
        uint16_t buttons = 0;
        std::optional<std::pair<float, float>> stick;
    };

    std::vector<InputEvent> events;
    std::mutex events_mutex;
    std::once_flag parsed;
    std::chrono::steady_clock::time_point game_start = std::chrono::steady_clock::now();
    bool started = false;
    // live.sh (BLITZ_INPUT_LIVE): presses as they're typed, from launch, so the virtual pads can drive the launcher
    // and its prompts (Blitz Builder) too; a script's times stay relative to the game's start.
    const bool live_mode = getenv("BLITZ_INPUT_LIVE") != nullptr;

    uint16_t button_mask(const std::string& name) {
        static const std::pair<const char*, uint16_t> table[] = {
            {"A", 0x8000}, {"B", 0x4000}, {"Z", 0x2000}, {"START", 0x1000},
            {"UP", 0x0800}, {"DOWN", 0x0400}, {"LEFT", 0x0200}, {"RIGHT", 0x0100},
            {"L", 0x0020}, {"R", 0x0010},
            {"R3", 0x0080}, {"R1", 0x0040},  // unused N64 bits: virtual pads only (right stick click, right shoulder)
            {"CUP", 0x0008}, {"CDOWN", 0x0004}, {"CLEFT", 0x0002}, {"CRIGHT", 0x0001},
        };
        for (auto& [n, m] : table) {
            if (name == n) {
                return m;
            }
        }
        fprintf(stderr, "[input] unknown button '%s'\n", name.c_str());
        return 0;
    }

    double seconds_since_start() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - game_start).count();
    }

    // "<what> [hold]" read from ss, starting at time t.
    bool parse_event(std::stringstream& ss, double t, InputEvent& ev) {
        std::string what;
        if (!(ss >> what)) {
            return false;
        }
        ev = InputEvent{t, t + 0.15};
        if (what.size() > 3 && what[0] == 'P' && what[1] >= '1' && what[1] <= '4' && what[2] == ':') {
            ev.port = what[1] - '1';
            what = what.substr(3);
        }
        if (what == "STICK") {
            std::string xy;
            ss >> xy;
            float x = 0, y = 0;
            sscanf(xy.c_str(), "%f,%f", &x, &y);
            ev.stick = std::make_pair(x, y);
        }
        else {
            std::stringstream names{what};
            std::string b;
            while (std::getline(names, b, '+')) {
                ev.buttons |= button_mask(b);
            }
        }
        double hold;
        if (ss >> hold) {
            ev.end = t + hold;
        }
        return true;
    }

    // Live "GFX" command: change a graphics option in this session (graphics.json is not touched).
    void set_graphics_option(const std::string& key, const std::string& value) {
        using namespace ultramodern::renderer;
        GraphicsConfig cfg = get_graphics_config();
        if (key == "ar") {
            cfg.ar_option = value == "Original" ? AspectRatio::Original : AspectRatio::Expand;
        }
        else if (key == "hr") {
            cfg.hr_option = value == "Original" ? HUDRatioMode::Original : value == "Full" ? HUDRatioMode::Full : HUDRatioMode::Clamp16x9;
        }
        else if (key == "rr") {
            cfg.rr_option = value == "Original" ? RefreshRate::Original : value == "Manual" ? RefreshRate::Manual : RefreshRate::Display;
        }
        else if (key == "rr_manual") {
            cfg.rr_manual_value = atoi(value.c_str());
        }
        else {
            fprintf(stderr, "[dev] unknown gfx key '%s'\n", key.c_str());
            return;
        }
        set_graphics_config(cfg);
        fprintf(stderr, "[dev] gfx %s=%s\n", key.c_str(), value.c_str());
    }

    // Commands other than button presses; true if the line was one.
    bool live_command(const std::string& line) {
        std::stringstream ss{line};
        std::string word;
        ss >> word;
        if (word == "ROSTERROOM") {  // as Settings → Season → Roster Room
            blitz::roster_room_open();
            return true;
        }
        if (word == "DUMP") {
            blitz::dev_dump_rdram();
            return true;
        }
        if (word == "KEY") {  // "KEY Return", "KEY Down", "KEY Backspace": an app menu's key (SDL's names), as a remote can't touch
            std::string name;
            ss >> name;
            SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
            if (sc == SDL_SCANCODE_UNKNOWN) {
                fprintf(stderr, "[input] unknown key '%s'\n", name.c_str());
                return true;
            }
            for (Uint32 type : {SDL_KEYDOWN, SDL_KEYUP}) {
                SDL_Event e{};
                e.type = type;
                e.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
                e.key.keysym.scancode = sc;
                e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
                SDL_PushEvent(&e);
            }
            return true;
        }
#ifdef __ANDROID__
        if (word == "CPUS") {  // "CPUS 0x0F": the game on those cores only, standing in for a weaker chip; "CPUS 0xFF": all again
            std::string m;
            ss >> m;
            unsigned long mask = std::stoul(m, nullptr, 0);
            cpu_set_t set;
            CPU_ZERO(&set);
            for (int c = 0; c < 32; c++) if (mask & (1ul << c)) CPU_SET(c, &set);
            int done = 0, failed = 0;
            std::error_code ec;
            for (const auto& t : std::filesystem::directory_iterator("/proc/self/task", ec)) {
                if (sched_setaffinity((pid_t)atoi(t.path().filename().c_str()), sizeof(set), &set) == 0) done++;
                else failed++;
            }
            fprintf(stderr, "[dev] cpus %lx: %d threads, %d failed; max MHz:", mask, done, failed);
            for (int c = 0; c < 8; c++) {
                std::ifstream f{"/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq/cpuinfo_max_freq"};
                long khz = 0;
                f >> khz;
                fprintf(stderr, " %ld", khz / 1000);
            }
            fprintf(stderr, "\n");
            return true;
        }
#endif
        if (word == "RT64") {  // "RT64 ram 0": the second drawing of each frame, into the N64's RAM (0 when read, 1 always, 2 never)
            std::string what;  // "RT64 filter 1": BLITZ_FAST_FILTER for this run (0 exact, 1 one bilinear fetch, 2 not on rectangles)
            int on = 1;
            ss >> what >> on;
            if (what == "ram") blitz_rt64_render_to_ram(on);
            if (what == "filter") blitz_rt64_fast_filter(on);
            if (what == "skip") blitz_rt64_present_skip(on);
            if (what == "pfirst") blitz_rt64_picture_first(on);  // "RT64 pfirst 1": Stylus draws the picture's screen first
            if (what == "uievery") blitz_rt64_ui_every(on);      // "RT64 uievery 3": and the UI's screen every 3rd frame, or when it changes
            fprintf(stderr, "[input] RT64 %s %d\n", what.c_str(), on);
            return true;
        }
        if (word == "TALL") {  // "TALL tilt 0.5": BLITZ DS's camera, for this run (tall_view.cpp)
            std::string name;
            float value = 0;
            ss >> name >> value;
            blitz::tall_set(name, value);
            return true;
        }
        if (word == "PEEK") {  // "PEEK 0x80720E80 10": that many words, to the log
            std::string a;
            int n = 1;
            ss >> a >> n;
            uint8_t* rdram = blitz::monitor_rdram();
            uint32_t addr = (uint32_t)std::stoul(a, nullptr, 0);
            if (rdram != nullptr && addr >= 0x80000000u && addr + (uint32_t)n * 4 < 0x80800000u) {
                for (int i = 0; i < n; i += 8) {
                    fprintf(stderr, "[peek] %08X:", addr + (uint32_t)i * 4);
                    for (int k = i; k < n && k < i + 8; k++) fprintf(stderr, " %08X", (uint32_t)MEM_W(0, (gpr)(int32_t)(addr + (uint32_t)k * 4)));
                    fprintf(stderr, "\n");
                }
                fflush(stderr);
            }
            return true;
        }
        if (word == "POKE8" || word == "POKE16" || word == "POKE32") {  // "POKE32 0x80123456 0x10": tests only
            std::string a, v;
            ss >> a >> v;
            uint8_t* rdram = blitz::monitor_rdram();
            uint32_t addr = (uint32_t)std::stoul(a, nullptr, 0), val = (uint32_t)std::stoul(v, nullptr, 0);
            if (rdram != nullptr && addr >= 0x80000000u && addr < 0x80800000u) {
                if (word == "POKE8") MEM_B(0, (gpr)(int32_t)addr) = (int8_t)val;
                else if (word == "POKE16") MEM_H(0, (gpr)(int32_t)addr) = (int16_t)val;
                else MEM_W(0, (gpr)(int32_t)addr) = (int32_t)val;
                fprintf(stderr, "[dev] %s %08X = %X\n", word.c_str(), addr, val);
            }
            return true;
        }
        if (word == "CONFIG") {  // "CONFIG general tv_mode true"
            std::string tab, option, value;
            ss >> tab >> option >> value;
            blitz::dev_set_option(tab, option, value);
            return true;
        }
        if (word == "QUITPROMPT") {  // as Cmd+Q does: the quit prompt (with Save & Quit during a game)
            recompui::open_quit_game_prompt();
            return true;
        }
        if (word == "QUIT") {  // as confirming the quit prompt does
            ultramodern::quit();
            return true;
        }
        if (word == "PARTY") {
            blitz::request_blitz_party();
            return true;
        }
        if (word == "SAVESTATE" || word == "LOADSTATE") {  // as F5 / F7, in the current slot
            blitz_savestate_request(word == "SAVESTATE" ? 1 : 2);
            return true;
        }
        if (word == "RESTART") {
            blitz::request_restart();
            return true;
        }
        if (word == "RESIZE") {  // "RESIZE 1280 720 [centre]": as dragging the window's corner (the Windows resize crash)
            int w = 0, h = 0;
            std::string centre;
            ss >> w >> h >> centre;
            if (w > 0 && h > 0) blitz::request_window_size(w, h, centre == "centre");
            return true;
        }
        if (word == "REPLAY") {  // "REPLAY open", "REPLAY save", "REPLAY close": Instant Replay (replay_screen.cpp)
            std::string what;
            ss >> what;
            if (what == "save") blitz::replay_save();
            else if (what == "close") blitz::replay_close();
            else blitz::replay_open();
            return true;
        }
        if (word == "DSTAP") {  // "DSTAP 0 0.1 0.5": BLITZ DS's tap: screen 0 or 1, x, y 0-1
            int screen = 0;
            float x = 0, y = 0;
            ss >> screen >> x >> y;
            blitz::ds_tap(screen, x, y);
            return true;
        }
        if (word == "EXPORTROM") {  // Export ROM without the prompt (export_rom.cpp)
            blitz::export_rom_now_dev();
            return true;
        }
        if (word == "EXPORTSCREEN") {  // the Export ROM screen, as the launcher's item (export_screen.cpp)
            blitz::export_screen_dev();
            return true;
        }
        if (word == "HINT") {  // "HINT team" shows that first-time hint now (hints.cpp)
            std::string id;
            ss >> id;
            blitz::hints_preview(id);
            return true;
        }
        if (word == "SEND") {  // "SEND ps5 192.168.0.90", "SEND iphone", "SEND android", "SEND ipa": Blitz Builder's device send
            std::string what, arg;
            ss >> what >> arg;
            if (what == "ps5") blitz::send_to_ps5(arg);
            else if (what == "iphone") blitz::send_to_iphone();
            else if (what == "android") blitz::send_to_android();
            else if (what == "ipa") blitz::save_iphone_ipa();  // Save the iPhone app (.ipa)
            return true;
        }
        if (word == "MENU") {  // "MENU controllers" opens Settings on that tab
            std::string tab;
            ss >> tab;
            blitz::dev_open_menu(tab);
            return true;
        }
        if (word == "PAD") {  // "PAD -2" unplugs virtual pad 2, "PAD +2" plugs it back in
            std::string arg;
            ss >> arg;
            if (arg.size() >= 2 && (arg[0] == '+' || arg[0] == '-')) blitz::dev_virtual_pad(arg[0] == '+', atoi(arg.c_str() + 1));
            return true;
        }
        if (word == "GFX") {
            while (ss >> word) {
                size_t eq = word.find('=');
                if (eq != std::string::npos) {
                    set_graphics_option(word.substr(0, eq), word.substr(eq + 1));
                }
            }
            return true;
        }
        return false;
    }

    // Poll the live input file for appended lines and press each one now.
    void watch_live_file(std::string path) {
        std::streamoff offset = 0;
        if (getenv("BLITZ_INPUT_LIVE_FROM_END") != nullptr) {  // a restarted copy: skip what the old one read
            std::ifstream f{path, std::ios::binary | std::ios::ate};
            if (f) offset = f.tellg();
        }
        std::string pending;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::ifstream f{path, std::ios::binary};
            if (!f) {
                continue;
            }
            f.seekg(0, std::ios::end);
            std::streamoff size = f.tellg();
            if (size < offset) {
                offset = 0;  // file was recreated
            }
            if (size == offset) {
                continue;
            }
            f.seekg(offset);
            std::string chunk(size - offset, '\0');
            f.read(chunk.data(), chunk.size());
            offset = size;
            pending += chunk;
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (live_command(line)) {
                    continue;
                }
                std::stringstream ss{line};
                InputEvent ev;
                if ((started || live_mode) && parse_event(ss, seconds_since_start(), ev)) {
                    std::lock_guard lock{events_mutex};
                    events.push_back(ev);
                }
            }
        }
    }

    void parse() {
        if (const char* live = getenv("BLITZ_INPUT_LIVE")) {
            std::thread(watch_live_file, std::string{live}).detach();
            fprintf(stderr, "[input] live input from %s\n", live);
        }
        const char* spec = getenv("BLITZ_INPUT");
        if (spec == nullptr) {
            return;
        }
        std::stringstream all{spec};
        std::string item;
        while (std::getline(all, item, ';')) {
            std::stringstream ss{item};
            double t;
            InputEvent ev;
            if (ss >> t && parse_event(ss, t, ev)) {
                events.push_back(ev);
            }
        }
        fprintf(stderr, "[input] %zu scripted input event(s)\n", events.size());
    }
}

namespace blitz {
    // Called when the game starts, so script times are relative to boot rather than app launch.
    void dev_input_game_started() {
        if (!live_mode) game_start = std::chrono::steady_clock::now();  // live presses keep their own clock
        started = true;
    }

    bool port_input(int port, uint16_t* buttons, float* x, float* y);  // controllers.cpp

    // Scripted presses for a port's virtual controller (controllers.cpp, main thread).
    void dev_scripted_state(int port, uint16_t* buttons, float* x, float* y, bool* any) {
        if (!started && !live_mode) return;
        if (live_mode) std::call_once(parsed, parse);  // before the game's first read, too: the launcher
        double now = seconds_since_start();
        std::lock_guard lock{events_mutex};
        for (const auto& ev : events) {
            if (ev.port == port && now >= ev.start && now < ev.end) {
                *any = true;
                *buttons |= ev.buttons;
                if (ev.stick) {
                    *x = ev.stick->first;
                    *y = ev.stick->second;
                }
            }
        }
    }

    // A press the app makes for a player (Pro Bowl's R confirms the pick with A): N64 buttons held for `ms`.
    std::mutex inject_mutex;
    struct Injected { uint16_t buttons = 0; std::chrono::steady_clock::time_point until{}; } injected[4];
    void inject_press(int port, uint16_t buttons, int ms) {
        if (port < 0 || port > 3) return;
        std::lock_guard lock{inject_mutex};
        injected[port] = {buttons, std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)};
    }

    // Returning false makes the runtime report "no response" for the port, as for an empty one.
    void receiver_targets_filter(int port, uint16_t* buttons, float* x, float* y);  // receiver_targets.cpp

    // Scripted presses for player 1 (BLITZ_INPUT, live.sh), added to the controller's own.
    void add_scripted(uint16_t* buttons, float* x, float* y) {
        static const bool active = getenv("BLITZ_INPUT") != nullptr || getenv("BLITZ_INPUT_LIVE") != nullptr;
        if (!active || !started) return;
        double now = seconds_since_start();
        std::lock_guard lock{events_mutex};
        for (const auto& ev : events) {
            if (ev.port == 0 && now >= ev.start && now < ev.end) {
                // R3 with others is a save state combo (controllers.cpp): like a controller's, it reaches the game
                // as nothing.
                if (!(ev.buttons & 0x0080)) *buttons |= ev.buttons;
                if (ev.stick) {
                    *x = ev.stick->first;
                    *y = ev.stick->second;
                }
            }
        }
    }

    bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
        if (!port_input(controller_num, buttons, x, y)) {
            return false;
        }
        std::call_once(parsed, parse);
        if (controller_num >= 0 && controller_num < 4) {
            std::lock_guard lock{inject_mutex};
            if (injected[controller_num].buttons && std::chrono::steady_clock::now() < injected[controller_num].until) {
                *buttons |= injected[controller_num].buttons;
            }
        }
        if (controller_num == 0) add_scripted(buttons, x, y);
        // Last, so a scripted press goes through Receiver Buttons and menu steps the way a controller's does.
        receiver_targets_filter(controller_num, buttons, x, y);
        blitz_menu_step_input(controller_num, buttons);
        blitz_replay_input(controller_num, buttons, x, y);
        return true;
    }
}
