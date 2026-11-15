// Controllers: which physical controller is which N64 port, and its input and rumble.
//
// RecompFrontend stays in single-player mode (its menus, bindings and Controls tab work as they are),
// and Blitz routes ports itself:
//   - every game controller gets the lowest free port (1-4) when it's connected; one that was
//     connected before gets its old port back if that port is free, so a reconnect or a re-pair
//     doesn't reshuffle players;
//   - port 1 is also the keyboard; it's always connected, so the keyboard plays when no controller is;
//   - all controllers use the Controls tab's controller bindings, the keyboard its keyboard bindings;
//   - the player LEDs (DualSense, Switch, Xbox) and the DualSense / DualShock light bar show the port;
//   - rumble goes to the port's controller (ports 2-4 have Rumble Paks; port 1 has the Controller Pak).
// Blitz's own Controller Select screen then puts each port on a team.
//
// Dev: BLITZ_VIRTUAL_PADS=N attaches N virtual SDL controllers, driven by dev_input.cpp's "P2:A"-style
// presses, so multiplayer routing can be tested without hands on real controllers.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <SDL.h>

#include "recompinput/recompinput.h"
#include "recompinput/input_state.h"
#include "recompinput/profiles.h"
#include "recompinput/input_types.h"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "ultramodern/ultramodern.hpp"

extern "C" void blitz_savestate_request(int op);  // savestate.cpp
extern "C" void blitz_savestate_slot_step(int delta);
extern "C" void blitz_redo_request();

extern "C" int blitz_ds_active(void);  // ds_view.cpp
extern "C" int blitz_play_mode(void);  // savestate.cpp: 0 menus, 1 play select, 2 a play
extern "C" int blitz_touch_port(void);  // savestate.cpp: the player BLITZ DS's taps and tiles play for

#include <atomic>
namespace blitz { std::atomic<bool> digital_trigger[2]{}; }
namespace blitz { extern std::atomic<bool> hotkey_held; }
// ui_state.cpp: Select with the hotkey is a screenshot, so it mustn't open the menu too.
extern "C" int blitz_hotkey_held(void) { return blitz::hotkey_held.load() ? 1 : 0; }

namespace blitz {
    void phone_pads_update();  // phone_pads.cpp
    void ds_input_read(uint16_t* buttons);  // ds_view.cpp
    void stylus_read(uint16_t* buttons);    // ds_stylus.cpp
    void ds_screens_next();                 // config.cpp: the hotkey + Start, on two screens
    void replay_open();                     // replay_screen.cpp
    bool replay_is_open();
    void capture_screenshot();              // capture.cpp
    void toggle_development();              // config.cpp: the Development settings rows
    std::atomic<bool> hotkey_held{false};   // L3 or R3 held on any controller (the main thread's last look)
    void touch_controls_read(uint16_t* buttons, float* x, float* y);  // touch_controls.cpp
    bool player1_rumble();  // config.cpp: Settings → General → Player 1 Rumble
    int fullscreen_button();  // config.cpp: Settings → General → Fullscreen Button
    bool savestate_buttons();  // config.cpp: Settings → General → Save State Buttons
    int button_icons();        // config.cpp: Settings → General → Button Icons
    // dev_input.cpp: scripted presses for a port (port > 0, for virtual controllers).
    void dev_scripted_state(int port, uint16_t* buttons, float* x, float* y, bool* any);
}

namespace {
    constexpr int ports = 4;

    struct Pad {
        SDL_GameController* controller = nullptr;
        SDL_JoystickID id = -1;
        std::string key;   // serial, or vendor:product:name when the controller has none
        std::string name;
        int port = -1;
        bool rumble_failed = false;
        uint32_t pulse_until = 0;  // SDL ticks: the connect pulse plays until then
        uint32_t flash_until = 0;  // SDL ticks: Identify's white light bar until then
    };

    // Button icons (button_glyphs.cpp, read by the game thread): each port's controller family (-1 none or
    // unknown, 0 PlayStation, 1 Xbox, 2 Switch), Settings → General → Button Icons, and the menu owner.
    std::atomic<int> port_family[4] = {-1, -1, -1, -1};
    std::atomic<int> icon_mode{0};
    std::atomic<int> menu_owner{0};
    std::array<uint16_t, 4> last_buttons{};  // game thread (port_input)
    std::mutex pads_mutex;          // pads is read by the game thread (input, rumble)
    std::vector<Pad> pads;
    std::array<SDL_JoystickID, ports> port_pad{-1, -1, -1, -1};
    std::map<std::string, int> last_port;   // key -> port it had, for reconnects
    std::array<bool, ports> rumble_on{};
    std::array<float, ports> rumble_level{};
    std::vector<SDL_Joystick*> virtual_pads;
    std::mutex requests_mutex;
    std::vector<std::pair<bool, int>> requests;  // dev: (attach, virtual pad number) from any thread
    std::string menu_request;                    // dev: open Settings on this tab
    std::vector<std::array<std::string, 3>> config_requests;  // dev: tab, option, value

    // libultra button bits in GameInput order (recompinput's own table is internal).
#define DEFINE_INPUT(name, value, readable) value,
    constexpr std::array<uint16_t, 14> n64_button_values = {DEFINE_N64_BUTTON_INPUTS()};
#undef DEFINE_INPUT

    // Light bar colour per port: blue, red, green, yellow (the game's on-screen badges follow the slot
    // each controller picks on Controller Select, so they can differ).
    constexpr uint8_t port_colour[ports][3] = {{0, 64, 255}, {255, 24, 24}, {0, 200, 40}, {255, 190, 0}};

    std::string key_of(SDL_GameController* c) {
        const char* serial = SDL_GameControllerGetSerial(c);
        if (serial != nullptr && *serial != '\0') {
            return serial;
        }
        const char* name = SDL_GameControllerName(c);
        return std::to_string(SDL_GameControllerGetVendor(c)) + ":" + std::to_string(SDL_GameControllerGetProduct(c)) + ":" +
               (name ? name : "");
    }

    const char* type_name(SDL_GameController* c) {
        switch (SDL_GameControllerGetType(c)) {
            case SDL_CONTROLLER_TYPE_PS5: return "DualSense";
            case SDL_CONTROLLER_TYPE_PS4: return "DualShock 4";
            case SDL_CONTROLLER_TYPE_PS3: return "DualShock 3";
            case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox";
            case SDL_CONTROLLER_TYPE_XBOX360: return "Xbox 360";
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
            case SDL_CONTROLLER_TYPE_VIRTUAL: return "Virtual pad";
            default: return "Controller";
        }
    }

    // Player LEDs and light bar show the port; a short pulse says "this one is connected".
    void show_port(Pad& p) {
        SDL_GameControllerSetPlayerIndex(p.controller, p.port);
        if (p.port >= 0) {
            const uint8_t* c = port_colour[p.port];
            SDL_GameControllerSetLED(p.controller, c[0], c[1], c[2]);
            if (SDL_GameControllerRumble(p.controller, 0x5000, 0x5000, 250) == 0) p.pulse_until = SDL_GetTicks() + 250;
        }
    }

    void assign(Pad& p) {
        int want = -1;
        if (auto it = last_port.find(p.key); it != last_port.end() && port_pad[it->second] < 0) {
            want = it->second;
        }
        for (int i = 0; i < ports && want < 0; i++) {
            if (port_pad[i] < 0) want = i;
        }
        p.port = want;
        if (want >= 0) {
            port_pad[want] = p.id;
            last_port[p.key] = want;
        }
        show_port(p);
        fprintf(stderr, "[pads] %s \"%s\" connected: %s\n", type_name(p.controller), p.name.c_str(),
                want >= 0 ? ("player " + std::to_string(want + 1)).c_str() : "no free port");
    }

    // Open new controllers, drop removed ones. Main thread.
    void reconcile() {
        std::lock_guard lock{pads_mutex};
        for (auto it = pads.begin(); it != pads.end();) {
            if (!SDL_GameControllerGetAttached(it->controller)) {
                fprintf(stderr, "[pads] \"%s\" disconnected (player %d)\n", it->name.c_str(), it->port + 1);
                if (it->port >= 0) {
                    port_pad[it->port] = -1;
                    rumble_level[it->port] = 0;
                }
                SDL_GameControllerClose(it->controller);
                it = pads.erase(it);
            }
            else {
                ++it;
            }
        }
        // Dev: BLITZ_VIRTUAL_ONLY=1 leaves real controllers alone, so a test copy doesn't take the ones
        // someone is playing with (only the virtual pads and the keyboard play).
        static const bool virtual_only = getenv("BLITZ_VIRTUAL_ONLY") != nullptr;
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (!SDL_IsGameController(i)) continue;
            if (virtual_only && !SDL_JoystickIsVirtual(i)) continue;
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
            if (std::any_of(pads.begin(), pads.end(), [&](const Pad& p) { return p.id == id; })) continue;
            SDL_GameController* c = SDL_GameControllerOpen(i);
            if (c == nullptr) continue;
            Pad p;
            p.controller = c;
            p.id = id;
            p.key = key_of(c);
            const char* name = SDL_GameControllerName(c);
            p.name = name ? name : "controller";
            if (char* mapping = SDL_GameControllerMapping(c)) {
                fprintf(stderr, "[pad] %s: %s\n", p.name.c_str(), mapping);
                SDL_free(mapping);
            }
            assign(p);
            pads.push_back(std::move(p));
        }
        // A port freed by a disconnect goes to a waiting controller (a fifth one).
        for (Pad& p : pads) {
            if (p.port < 0 && std::find(port_pad.begin(), port_pad.end(), -1) != port_pad.end()) assign(p);
        }
        // Each port's controller family for the game's button icons. BLITZ_PAD_FAMILY=1:ps,2:xb,3:sw,4:n64
        // (dev) overrides ports, e.g. for virtual pads, which have no family of their own.
        static const std::array<int, ports> forced = [] {
            std::array<int, ports> f{-2, -2, -2, -2};
            if (const char* env = getenv("BLITZ_PAD_FAMILY")) {
                std::string v = env;
                size_t i = 0;
                while (i <= v.size()) {
                    size_t comma = std::min(v.find(',', i), v.size());
                    std::string item = v.substr(i, comma - i);
                    if (item.size() >= 3 && item[1] == ':' && item[0] >= '1' && item[0] <= '4') {
                        std::string fam = item.substr(2);
                        f[item[0] - '1'] = fam == "ps" ? 0 : fam == "xb" ? 1 : fam == "sw" ? 2 : fam == "n64" ? 3 : -1;
                    }
                    i = comma + 1;
                }
            }
            return f;
        }();
        for (int port = 0; port < ports; port++) {
            const Pad* first = nullptr;
            for (const Pad& p : pads) if (p.port == port) first = &p;
            int fam = -1;
            if (first != nullptr) {
                switch (SDL_GameControllerGetType(first->controller)) {
                    case SDL_CONTROLLER_TYPE_PS3: case SDL_CONTROLLER_TYPE_PS4: case SDL_CONTROLLER_TYPE_PS5: fam = 0; break;
                    case SDL_CONTROLLER_TYPE_XBOX360: case SDL_CONTROLLER_TYPE_XBOXONE: fam = 1; break;
                    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
                    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
                        fam = 2; break;
                    default: break;
                }
            }
            if (forced[port] != -2) fam = forced[port];
            port_family[port] = fam;
        }
        // Settings → General → Button Icons. The app's own prompts (the Controls tab, hint row, launcher) have
        // two styles, PlayStation (1) and Xbox (0): "Each Player's Controller" follows player 1.
        int mode = blitz::button_icons();
        icon_mode = mode;
        int fam = mode == 0 ? port_family[0].load() : mode - 1;
        int style = (fam == 1 || fam == 2) ? 0 : 1;
        recompinput::set_controller_glyph_style(style);
    }

    SDL_GameController* controller_on(int port) {
        for (const Pad& p : pads) {
            if (p.port == port) return p.controller;
        }
        return nullptr;
    }

    // TEST ONLY, comes out with the stacked BLITZ DS test (BlitzActivity.showStacked). Held upright, the right stick and the
    // face buttons are below the screen, turned a quarter: they act as they now look. R1 and R2 stand in for L1 and L2.
    // For a Retroid in its default Retro style, whose east button is SDL's A (Nintendo's layout): bottom is the east button.
    const bool held_upright = getenv("BLITZ_DS_STACK") != nullptr;

    SDL_GameControllerButton upright_button(SDL_GameControllerButton b) {
        switch (b) {
            case SDL_CONTROLLER_BUTTON_B: return SDL_CONTROLLER_BUTTON_X;  // right on screen: its north button
            case SDL_CONTROLLER_BUTTON_X: return SDL_CONTROLLER_BUTTON_B;  // left on screen: its south button
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
            default: return b;  // A is bottom on screen, Y top
        }
    }

    float axis(SDL_GameController* c, int32_t id) {
        if (id == 0 || std::abs(id) - 1 >= SDL_CONTROLLER_AXIS_MAX) return 0.0f;
        // A handheld whose triggers are plain buttons in line with its shoulders (an RG DS): L1 holds turbo, the game's Z,
        // and R1 is the game's R. Its L2 and R2 are only the hotkey's load and save (team: not turbo too).
        static const bool shoulders = getenv("BLITZ_SHOULDERS_ARE_TRIGGERS") != nullptr;
        // Triggers that are plain buttons, which SDL's controller layer ignores on Android (BlitzActivity.dispatchKeyEvent).
        if (!shoulders && id > 0 && std::abs(id) - 1 == SDL_CONTROLLER_AXIS_TRIGGERLEFT && blitz::digital_trigger[0].load()) return 1.0f;
        if (!shoulders && id > 0 && std::abs(id) - 1 == SDL_CONTROLLER_AXIS_TRIGGERRIGHT && blitz::digital_trigger[1].load()) return 1.0f;
        // Only the handheld's own pad: a phone controller (phone_pads.cpp) sends its turbo on the trigger itself.
        if (shoulders && id > 0 && (std::abs(id) - 1 == SDL_CONTROLLER_AXIS_TRIGGERLEFT || std::abs(id) - 1 == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
            const char* name = SDL_JoystickName(SDL_GameControllerGetJoystick(c));  // the virtual pad's own name
            if (name != nullptr && !strncmp(name, "Phone ", 6)) {
                const float v = SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)(std::abs(id) - 1)) / 32768.0f;
                return std::clamp(v, 0.0f, 1.0f);
            }
        }
        if (shoulders && id > 0 && std::abs(id) - 1 == SDL_CONTROLLER_AXIS_TRIGGERLEFT) return SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 1.0f : 0.0f;
        if (shoulders && id > 0 && std::abs(id) - 1 == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) return SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 1.0f : 0.0f;
        if (held_upright) {
            int a = std::abs(id) - 1;
            if (a == SDL_CONTROLLER_AXIS_RIGHTX || a == SDL_CONTROLLER_AXIS_RIGHTY) return 0.0f;  // it moves the player
            if (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT) id = id < 0 ? -(SDL_CONTROLLER_AXIS_TRIGGERRIGHT + 1) : SDL_CONTROLLER_AXIS_TRIGGERRIGHT + 1;
        }
        float v = SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)(std::abs(id) - 1)) / 32768.0f;
        return std::clamp(id < 0 ? -v : v, 0.0f, 1.0f);
    }

    // A press too quick for the game to see: buttons are looked at once per game read, under 20-30 reads a second on a
    // slow handheld, so a tap that went down and up between two reads was lost and needed pressing again (team, RG DS).
    // Each press is kept for the next read of its controller (if within 150 ms: a press made while the menu had the
    // pad isn't handed to the game later). Presses with L3 or R3 held are the hotkey's, not kept. Only SDL's own events
    // are used here, no SDL calls (its joystick lock is held while events are sent).
    struct Latch { uint32_t held = 0; uint32_t kept = 0; uint64_t at[SDL_CONTROLLER_BUTTON_MAX] = {}; };
    std::mutex latch_mutex;
    std::map<SDL_JoystickID, Latch> latches;
    uint32_t reading_kept = 0;  // the presses kept for the controller being read now (the game's input thread only)

    int latch_watch(void*, SDL_Event* e) {
        if (e->type != SDL_CONTROLLERBUTTONDOWN && e->type != SDL_CONTROLLERBUTTONUP) return 0;
        if (e->cbutton.button >= SDL_CONTROLLER_BUTTON_MAX || e->cbutton.button >= 32) return 0;
        const uint32_t bit = 1u << e->cbutton.button;
        const uint32_t sticks = (1u << SDL_CONTROLLER_BUTTON_LEFTSTICK) | (1u << SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        std::lock_guard lock{latch_mutex};
        Latch& l = latches[e->cbutton.which];
        if (e->type == SDL_CONTROLLERBUTTONUP) { l.held &= ~bit; return 0; }
        l.held |= bit;
        if (!(l.held & sticks & ~bit)) { l.kept |= bit; l.at[e->cbutton.button] = SDL_GetTicks64(); }
        return 0;
    }

    uint32_t take_kept(SDL_GameController* c) {
        static std::once_flag once;
        std::call_once(once, [] { SDL_AddEventWatch(latch_watch, nullptr); });
        const SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
        const uint64_t now = SDL_GetTicks64();
        std::lock_guard lock{latch_mutex};
        auto it = latches.find(id);
        if (it == latches.end()) return 0;
        uint32_t kept = 0;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && b < 32; b++) {
            if ((it->second.kept & (1u << b)) && now - it->second.at[b] <= 150) kept |= 1u << b;
        }
        it->second.kept = 0;
        return kept;
    }

    float field_value(SDL_GameController* c, const recompinput::InputField& f) {
        using recompinput::InputType;
        if (f.input_type == InputType::ControllerDigital) {
            if (f.input_id < 0 || f.input_id >= SDL_CONTROLLER_BUTTON_MAX) return 0.0f;
            SDL_GameControllerButton b = (SDL_GameControllerButton)f.input_id;
            if (held_upright) b = upright_button(b);
            return (SDL_GameControllerGetButton(c, b) || (b < 32 && (reading_kept & (1u << b)))) ? 1.0f : 0.0f;
        }
        if (f.input_type == InputType::ControllerAnalog) return axis(c, f.input_id);
        return 0.0f;
    }

    float binding_value(SDL_GameController* c, int profile, recompinput::GameInput input) {
        float v = 0;
        for (size_t k = 0; k < recompinput::num_bindings_per_input; k++) {
            v += field_value(c, recompinput::profiles::get_input_binding(profile, input, k));
        }
        return std::clamp(v, 0.0f, 1.0f);
    }

    // The N64 state of one controller under the Controls tab's controller bindings.
    void read_controller(SDL_GameController* c, uint16_t* buttons, float* x, float* y) {
        using recompinput::GameInput;
        int profile = recompinput::profiles::get_sp_controller_profile_index();
        // L3 or R3 held is the hotkey (below): with one of its buttons down too, the press is the combo's, not the game's,
        // so Redo in a menu doesn't also move the cursor. A stick clicked in alone (pushed hard on a handheld) keeps them.
        bool combo = false;
        static const bool shoulders_turbo = getenv("BLITZ_SHOULDERS_ARE_TRIGGERS") != nullptr;
        reading_kept = take_kept(c);
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK) || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK)) {
            // Turbo held (an RG DS's L1/R1) while a stick is pushed hard isn't a combo: the save/load buttons there are L2/R2.
            if (!shoulders_turbo && (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))) combo = true;
            for (SDL_GameControllerButton b : {SDL_CONTROLLER_BUTTON_X,
                                               SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                                               SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK}) {
                if (SDL_GameControllerGetButton(c, b)) combo = true;
            }
        }
        for (size_t i = 0; i < n64_button_values.size() && !combo; i++) {
            if (binding_value(c, profile, (GameInput)((size_t)GameInput::N64_BUTTON_START + i)) > 0.5f) {
                *buttons |= n64_button_values[i];
            }
        }
        // At play select R1 opens Instant Replay (below), so it isn't also C-down there.
        if (blitz_play_mode() == 1 && (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) || (reading_kept & (1u << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)))) *buttons &= (uint16_t)~0x0004;
        float sx = binding_value(c, profile, GameInput::X_AXIS_POS) - binding_value(c, profile, GameInput::X_AXIS_NEG);
        float sy = binding_value(c, profile, GameInput::Y_AXIS_POS) - binding_value(c, profile, GameInput::Y_AXIS_NEG);
        if (held_upright) {  // the top of the stick now points right, its right points down
            sx += -SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY) / 32768.0f;
            sy += -SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX) / 32768.0f;
        }
        recompinput::apply_joystick_deadzone(sx, sy, &sx, &sy);
        *x += sx;
        *y += sy;
        reading_kept = 0;
    }

    // The keyboard's N64 state (port 1).
    void read_keyboard(uint16_t* buttons, float* x, float* y) {
        using recompinput::GameInput;
        using recompinput::InputType;
        int profile = recompinput::profiles::get_sp_keyboard_profile_index();
        auto value = [&](GameInput input) {
            float v = 0;
            for (size_t k = 0; k < recompinput::num_bindings_per_input; k++) {
                const recompinput::InputField& f = recompinput::profiles::get_input_binding(profile, input, k);
                if (f.input_type == InputType::Keyboard) v += recompinput::get_input_analog(0, f);
            }
            return std::clamp(v, 0.0f, 1.0f);
        };
        for (size_t i = 0; i < n64_button_values.size(); i++) {
            if (value((GameInput)((size_t)GameInput::N64_BUTTON_START + i)) > 0.5f) *buttons |= n64_button_values[i];
        }
        *x += value(GameInput::X_AXIS_POS) - value(GameInput::X_AXIS_NEG);
        *y += value(GameInput::Y_AXIS_POS) - value(GameInput::Y_AXIS_NEG);
    }

    // Virtual controllers follow the scripted presses for their port (dev only). Main thread.
    void drive_virtual_pads() {
        if (virtual_pads.empty()) return;
        std::lock_guard lock{pads_mutex};
        for (int port = 0; port < ports; port++) {
            SDL_JoystickID id = port_pad[port];
            SDL_Joystick* j = nullptr;
            for (SDL_Joystick* v : virtual_pads) {
                if (v != nullptr && SDL_JoystickInstanceID(v) == id) j = v;
            }
            if (j == nullptr) continue;
            uint16_t b = 0;
            float x = 0, y = 0;
            bool any = false;
            blitz::dev_scripted_state(port, &b, &x, &y, &any);
            // The default controller bindings: A = south, B = west, Z = left trigger, L/R = shoulders
            // and right trigger, Start, D-pad, C buttons = right stick.
            auto btn = [&](SDL_GameControllerButton sb, uint16_t mask) { SDL_JoystickSetVirtualButton(j, sb, (b & mask) ? 1 : 0); };
            btn(SDL_CONTROLLER_BUTTON_A, 0x8000);
            btn(SDL_CONTROLLER_BUTTON_X, 0x4000);
            btn(SDL_CONTROLLER_BUTTON_START, 0x1000);
            btn(SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0800);
            btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0400);
            btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0200);
            btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0100);
            btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0020);
            btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x0080);     // dev names R3 / R1 (dev_input.cpp)
            btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0040);
            SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_TRIGGERLEFT, (b & 0x2000) ? 32767 : 0);
            SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, (b & 0x0010) ? 32767 : 0);
            SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTX, (Sint16)std::clamp(x * 32767.0f, -32768.0f, 32767.0f));
            SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTY, (Sint16)std::clamp(-y * 32767.0f, -32768.0f, 32767.0f));
            int cx = (b & 0x0001) ? 1 : (b & 0x0002) ? -1 : 0, cy = (b & 0x0004) ? 1 : (b & 0x0008) ? -1 : 0;
            SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_RIGHTX, (Sint16)(cx * 32767));
            SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_RIGHTY, (Sint16)(cy * 32767));
        }
    }
}

namespace blitz {
    SDL_GameController* pad_on_port(int port) { return controller_on(port); }  // crew.cpp
    // Dev (any thread): plug a virtual pad in or out (1-based), e.g. from the live command "PAD -2".
    void dev_virtual_pad(bool attach, int number) {
        std::lock_guard lock{requests_mutex};
        requests.emplace_back(attach, number);
    }

    // Dev (any thread): open Settings on a tab ("MENU controllers").
    void dev_open_menu(const std::string& tab) {
        std::lock_guard lock{requests_mutex};
        menu_request = tab.empty() ? "general" : tab;
    }

    // Any thread: text for the clipboard, set on the main thread (SDL's clipboard on a Mac, an iPhone, Windows or X11 isn't
    // for other threads; Copy Debug Info builds its text on one of its own, config.cpp).
    std::string clipboard_request;
    bool clipboard_pending = false;
    void set_clipboard_later(const std::string& text) {
        std::lock_guard lock{requests_mutex};
        clipboard_request = text;
        clipboard_pending = true;
    }

    // Any thread: open Settings, or close it if it's open (an Android device's Back button, BlitzActivity.deviceBack).
    void toggle_menu() {
        std::lock_guard lock{requests_mutex};
        menu_request = "\ttoggle";
    }

    // Dev (any thread): set a settings option as the Settings page would ("CONFIG general tv_mode true").
    void dev_set_option(const std::string& tab, const std::string& option, const std::string& value) {
        std::lock_guard lock{requests_mutex};
        config_requests.push_back({tab, option, value});
    }

    // After SDL_Init (main thread).
    void controllers_init() {
        int n = 0;
        if (const char* env = getenv("BLITZ_VIRTUAL_PADS")) n = std::clamp(atoi(env), 0, ports + 1);
        virtual_pads.assign(n, nullptr);
        for (int i = 0; i < n; i++) dev_virtual_pad(true, i + 1);
    }

    void ui_late_init();  // launcher.cpp

    // Main thread, from update_gfx (about every millisecond); the device scan runs every 100 ms.
    void controllers_update() {
        ui_late_init();
        std::vector<std::pair<bool, int>> todo;
        std::string menu;
        std::vector<std::array<std::string, 3>> configs;
        std::string clipboard;
        bool set_clipboard = false;
        {
            std::lock_guard lock{requests_mutex};
            todo.swap(requests);
            menu.swap(menu_request);
            configs.swap(config_requests);
            clipboard.swap(clipboard_request);
            set_clipboard = std::exchange(clipboard_pending, false);
        }
        if (set_clipboard) SDL_SetClipboardText(clipboard.c_str());
        for (const auto& [tab, option, value] : configs) {
            if (tab == "fullscreen") {  // dev: what the Fullscreen Button does
                recompui::config::graphics::toggle_fullscreen();
                fprintf(stderr, "[dev] fullscreen toggled\n");
                continue;
            }
            recomp::config::Config& c = recompui::config::get_config(tab);
            recomp::config::ConfigValueVariant v;
            // Stored as the option's own type (a double setting given "0" would throw in save_config).
            bool is_double = std::holds_alternative<double>(c.get_option_value(option));
            if (value == "true" || value == "false") v = value == "true";
            else if (is_double && !value.empty() && value.find_first_not_of("0123456789.-") == std::string::npos) v = std::stod(value);
            else if (!value.empty() && value.find_first_not_of("0123456789") == std::string::npos) v = (uint32_t)std::stoul(value);
            else v = value;
            c.update_option_value(option, v);  // runs the option's change callbacks, like the page does
            c.save_config();
            fprintf(stderr, "[dev] %s.%s = %s\n", tab.c_str(), option.c_str(), value.c_str());
        }
        if (menu == "\ttoggle") {
            // Settings up (it captures input), close it; else open it. Its context id throws before it's built.
            if (recompui::is_context_capturing_input()) recompui::config::close();
            else if (ultramodern::is_game_started()) recompui::config::open();
        }
        else if (!menu.empty()) {
            recompui::config::set_tab(menu);  // first: opening keeps the selected tab (the launcher's Controls does this)
            recompui::config::open();
        }
        for (auto [attach, number] : todo) {
            if (number < 1 || number > (int)virtual_pads.size()) continue;
            SDL_Joystick*& j = virtual_pads[number - 1];
            if (attach && j == nullptr) {
                SDL_VirtualJoystickDesc desc{};
                desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
                desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
                desc.naxes = SDL_CONTROLLER_AXIS_MAX;
                desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
                std::string name = "Blitz virtual pad " + std::to_string(number);
                desc.name = name.c_str();
                int index = SDL_JoystickAttachVirtualEx(&desc);
                j = index >= 0 ? SDL_JoystickOpen(index) : nullptr;
                if (j == nullptr) fprintf(stderr, "[pads] virtual pad failed: %s\n", SDL_GetError());
            }
            else if (!attach && j != nullptr) {
                SDL_JoystickID id = SDL_JoystickInstanceID(j);
                SDL_JoystickClose(j);
                j = nullptr;
                for (int i = 0; i < SDL_NumJoysticks(); i++) {
                    if (SDL_JoystickGetDeviceInstanceID(i) == id) SDL_JoystickDetachVirtual(i);
                }
            }
        }
        // Settings → General → Fullscreen Button, on any controller (on the press, not while held).
        {
            static bool was_down = false;
            int mode = fullscreen_button();
            bool down = false;
            if (mode != 0) {
                std::lock_guard lock{pads_mutex};
                for (const Pad& p : pads) {
                    auto pressed = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(p.controller, b) != 0; };
                    if (mode == 1 ? pressed(SDL_CONTROLLER_BUTTON_LEFTSTICK) && pressed(SDL_CONTROLLER_BUTTON_RIGHTSTICK)
                                  : pressed(SDL_CONTROLLER_BUTTON_GUIDE)) down = true;
                }
            }
            if (down && !was_down) recompui::config::graphics::toggle_fullscreen();
            was_down = down;
        }
        // The hotkey: L3 or R3 held, on any controller (team, 2026-11-02), the way handhelds' RetroArch hotkeys go. With it:
        //   R1 saves a state, L1 loads it (Settings → General → Save State Buttons);
        //   □ / ○ (or D-pad left / right) step the slot back / on, △ (or D-pad up) is Redo Play;
        //   Start steps through the DS screens, on two screens; Select takes a screenshot.
        // Each on the press. L3 + R3 alone is still the fullscreen button. While it's held the game gets no buttons (read_controller)
        // and Select doesn't open the menu (ui_state.cpp, blitz_hotkey_held).
        {
            // A combo goes off on its button's press while the hotkey was already held (the tick before): turbo held and then
            // a stick clicked in while running sets nothing off (team: L3 + L1, turbo on an RG DS, loaded a state by accident).
            // Save and load are R1 and L1, or on a pad whose shoulders are its turbo (BLITZ_SHOULDERS_ARE_TRIGGERS: an RG DS)
            // R2 and L2.
            static bool hotkey_before = false;
            static uint32_t down_before = 0;
            static const bool shoulders_turbo = getenv("BLITZ_SHOULDERS_ARE_TRIGGERS") != nullptr;
            enum : uint32_t { k_save = 1, k_load = 2, k_next = 4, k_prev = 8, k_redo = 16, k_screens = 32, k_shot = 64 };
            uint32_t down = 0;
            bool any_hotkey = false;
            const bool ds = blitz_ds_active();
            {
                std::lock_guard lock{pads_mutex};
                for (const Pad& p : pads) {
                    auto pressed = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(p.controller, b) != 0; };
                    auto trigger = [&](SDL_GameControllerAxis a, int side) {
                        return SDL_GameControllerGetAxis(p.controller, a) > 24000 || digital_trigger[side].load();
                    };
                    if (!pressed(SDL_CONTROLLER_BUTTON_RIGHTSTICK) && !pressed(SDL_CONTROLLER_BUTTON_LEFTSTICK)) continue;
                    any_hotkey = true;
                    const bool states = savestate_buttons();
                    if (states && (shoulders_turbo ? trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1) : pressed(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))) down |= k_save;
                    if (states && (shoulders_turbo ? trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 0) : pressed(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))) down |= k_load;
                    if (states && (pressed(SDL_CONTROLLER_BUTTON_B) || pressed(SDL_CONTROLLER_BUTTON_DPAD_RIGHT))) down |= k_next;
                    if (states && (pressed(SDL_CONTROLLER_BUTTON_X) || pressed(SDL_CONTROLLER_BUTTON_DPAD_LEFT))) down |= k_prev;
                    if (pressed(SDL_CONTROLLER_BUTTON_Y) || pressed(SDL_CONTROLLER_BUTTON_DPAD_UP)) down |= k_redo;  // not down any more (team)
                    if (ds && pressed(SDL_CONTROLLER_BUTTON_START)) down |= k_screens;
                    if (pressed(SDL_CONTROLLER_BUTTON_BACK)) down |= k_shot;
                }
            }
            const uint32_t fresh = (any_hotkey && hotkey_before) ? (down & ~down_before) : 0;
            hotkey_before = any_hotkey;
            down_before = any_hotkey ? down : 0;
            int now_op = 0;  // 1 save, 2 load, 3 next slot, 4 previous slot, 5 redo play, 6 next DS screen, 7 screenshot
            if (fresh & k_save) now_op = 1;
            else if (fresh & k_load) now_op = 2;
            else if (fresh & k_next) now_op = 3;
            else if (fresh & k_prev) now_op = 4;
            else if (fresh & k_redo) now_op = 5;
            else if (fresh & k_screens) now_op = 6;
            else if (fresh & k_shot) now_op = 7;
            hotkey_held = any_hotkey;
            // Settings up, L2 + R2 held 3 s on any controller: the Development rows shown or hidden (config.cpp).
            {
                static uint32_t both_since = 0;
                static bool fired = false;
                bool both = false;
                if (recompui::is_context_capturing_input()) {  // Settings (or another menu) up; its id throws before it's built
                    if (digital_trigger[0].load() && digital_trigger[1].load()) both = true;
                    std::lock_guard lock{pads_mutex};
                    for (const Pad& p : pads) {
                        if (SDL_GameControllerGetAxis(p.controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 24000 &&
                            SDL_GameControllerGetAxis(p.controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 24000) both = true;  // not 16000: an SDL virtual pad's idle trigger reads ~16384
                    }
                }
                const uint32_t now = SDL_GetTicks();
                if (!both) {
                    both_since = 0;
                    fired = false;
                }
                else if (both_since == 0) both_since = now;
                else if (!fired && now - both_since >= 3000) {
                    fired = true;
                    toggle_development();
                }
            }
            // R1 at play select, without the hotkey: Instant Replay of the last play (team). On a fresh press only: R1 is
            // watched every tick, so letting go of L3 before R1, or holding R1 as play select comes up, opens nothing.
            {
                static bool r1_was = false;
                bool r1_now = false;
                {
                    std::lock_guard lock{pads_mutex};
                    for (const Pad& p : pads) {
                        if (SDL_GameControllerGetButton(p.controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) r1_now = true;
                    }
                }
                if (r1_now && !r1_was && !any_hotkey && blitz_play_mode() == 1 && !recompui::is_context_capturing_input()) replay_open();
                r1_was = r1_now;
            }
            if (now_op != 0 && !(now_op <= 5 && replay_is_open())) {  // no states or Redo under a replay
                if (now_op <= 2) blitz_savestate_request(now_op);
                else if (now_op == 5) blitz_redo_request();
                else if (now_op == 6) blitz::ds_screens_next();
                else if (now_op == 7) blitz::capture_screenshot();
                else blitz_savestate_slot_step(now_op == 3 ? 1 : -1);
            }
        }
        phone_pads_update();
        static uint32_t last = 0;
        uint32_t now = SDL_GetTicks();
        if (now - last >= 100) {
            last = now;
            reconcile();
            std::lock_guard lock{pads_mutex};
            for (Pad& p : pads) {
                if (p.flash_until != 0 && (int32_t)(p.flash_until - now) <= 0 && p.port >= 0) {
                    p.flash_until = 0;
                    const uint8_t* c = port_colour[p.port];
                    SDL_GameControllerSetLED(p.controller, c[0], c[1], c[2]);
                }
            }
        }
        drive_virtual_pads();
    }

    // For the Controllers settings tab: what plays as each player ("" = nobody).
    std::array<std::string, ports> port_descriptions() {
        std::array<std::string, ports> out;
        std::lock_guard lock{pads_mutex};
        for (const Pad& p : pads) {
            if (p.port < 0) continue;
            std::string d = std::string(type_name(p.controller)) == "Controller" ? p.name : type_name(p.controller);
            // "DualSense · battery OK"; the launcher's chips keep only the part before " · " (launcher.cpp).
            switch (SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(p.controller))) {
                case SDL_JOYSTICK_POWER_WIRED: d += " \xC2\xB7 USB"; break;
                case SDL_JOYSTICK_POWER_FULL: d += " \xC2\xB7 battery full"; break;
                case SDL_JOYSTICK_POWER_MEDIUM: d += " \xC2\xB7 battery OK"; break;
                case SDL_JOYSTICK_POWER_LOW: case SDL_JOYSTICK_POWER_EMPTY: d += " \xC2\xB7 battery low"; break;
                default: break;
            }
            out[p.port] = d;
        }
        if (out[0].empty()) out[0] = "Keyboard";
        else out[0] += " + Keyboard";
        return out;
    }

    // Rumble and flash the light bar white on the player's controller. UI thread.
    void identify_port(int port) {
        std::lock_guard lock{pads_mutex};
        for (Pad& p : pads) {
            if (p.port != port) continue;
            SDL_GameControllerSetLED(p.controller, 255, 255, 255);
            if (SDL_GameControllerRumble(p.controller, 0x9000, 0x9000, 400) == 0) p.pulse_until = SDL_GetTicks() + 400;
            p.flash_until = SDL_GetTicks() + 600;
        }
    }

    // Swap the controllers of two players (either may be empty). UI thread.
    void swap_ports(int a, int b) {
        if (a < 0 || b < 0 || a >= ports || b >= ports || a == b) return;
        std::lock_guard lock{pads_mutex};
        std::swap(port_pad[a], port_pad[b]);
        for (Pad& p : pads) {
            if (p.port == a) p.port = b;
            else if (p.port == b) p.port = a;
            else continue;
            last_port[p.key] = p.port;
            show_port(p);
        }
        rumble_level[a] = rumble_level[b] = 0;
    }

    // The button icons a port's player sees in the game (button_glyphs.cpp, game thread): 0 PlayStation,
    // 1 Xbox, 2 Switch, 3 N64 (the game's own). Each player's controller, unless Button Icons picks one
    // style; a port with no controller, or one we don't know, shows PlayStation.
    int port_glyph_family(int port) {
        int mode = icon_mode;
        if (mode != 0) return mode - 1;
        int fam = port >= 0 && port < ports ? port_family[port].load() : -1;
        return fam < 0 ? 0 : fam;
    }

    // The port that last pressed a button: it drives the game's menus (their footer icons follow it).
    int menu_owner_port() {
        return menu_owner;
    }

    // Which player (0-3) an SDL joystick plays as, or -1 (phone_pads.cpp shows it on the phone).
    int port_of_joystick(SDL_JoystickID id) {
        std::lock_guard lock{pads_mutex};
        for (int port = 0; port < ports; port++) if (port_pad[port] == id) return port;
        return -1;
    }

    bool port_rumbling(int port) {
        return port >= 0 && port < ports && rumble_on[port];
    }

    bool port_connected(int port) {
        if (port == 0) return true;  // the keyboard, or the first controller
        std::lock_guard lock{pads_mutex};
        return port > 0 && port < ports && port_pad[port] >= 0;
    }

    // Game thread (osContGetReadData).
    bool port_input(int port, uint16_t* buttons, float* x, float* y) {
        *buttons = 0;
        *x = 0;
        *y = 0;
        if (!port_connected(port)) return false;
        if (recompinput::game_input_disabled()) return true;
        {
            std::lock_guard lock{pads_mutex};
            if (SDL_GameController* c = controller_on(port)) read_controller(c, buttons, x, y);
        }
        if (port == 0) {
            read_keyboard(buttons, x, y);
            touch_controls_read(buttons, x, y);  // a phone's on-screen controls (touch_controls.cpp)
        }
        if (port == blitz_touch_port()) {  // player 1, or whoever's in the game if he isn't
            ds_input_read(buttons);  // BLITZ DS's taps (ds_view.cpp)
            stylus_read(buttons);    // BLITZ DS's tiles (ds_stylus.cpp)
        }
        if (port >= 0 && port < ports) {
            if (*buttons & ~last_buttons[port]) menu_owner = port;  // a new press
            last_buttons[port] = *buttons;
        }
        *x = std::clamp(*x, -1.0f, 1.0f);
        *y = std::clamp(*y, -1.0f, 1.0f);
        return true;
    }

    // Game thread (osMotorStart/Stop). The game rumbles only during play (its flag 0x8002B068) and
    // never checks the pak type, so player 1 rumbles too while keeping the Controller Pak for saves;
    // Settings → General → Player 1 Rumble can turn that off. Note for runtime changes: this relies on
    // __osMotorAccess passing commands through without the ROM's "motor initialised" status check.
    void port_set_rumble(int port, bool on) {
        static const bool log = getenv("BLITZ_RUMBLE_LOG") != nullptr;
        if (log) fprintf(stderr, "[rumble] port %d %s\n", port + 1, on ? "on" : "off");
        if (port == 0 && on && !player1_rumble()) on = false;
        if (port >= 0 && port < ports) rumble_on[port] = on;
    }

    // Every VI: ramp each port's motor the way an N64 Rumble Pak spins up and down.
    void ports_update_rumble() {
        double strength = recompui::config::general::get_rumble_strength();
        std::lock_guard lock{pads_mutex};
        for (int port = 0; port < ports; port++) {
            float& level = rumble_level[port];
            if (rumble_on[port]) level = std::min(1.0f, level + 0.17f);
            else level = std::max(0.0f, level * 0.92f - 0.01f);
            float smooth = level * level * (3.0f - 2.0f * level);
            uint32_t now = SDL_GetTicks();
            for (Pad& p : pads) {
                if (p.port != port || p.rumble_failed || (int32_t)(p.pulse_until - now) > 0) continue;
                uint16_t s = (uint16_t)(smooth * strength * 0xFFFF / 100);
                if (SDL_GameControllerRumble(p.controller, 0, s, 1000000) != 0 && s != 0) p.rumble_failed = true;
            }
        }
    }
}
