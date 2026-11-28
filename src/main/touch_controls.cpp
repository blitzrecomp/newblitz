// On-screen touch controls for phones (include/blitz_platform.h: BLITZ_HAS_TOUCH_CONTROLS; iOS and Android share
// this file). Player 1's N64 controller, drawn over the game: a floating stick on the left half, A, B and Turbo on the
// right, a small Start (⏵) at the bottom and a menu button (Settings, ☰) at the top left.
// Only what means something now is shown, labelled for now (game_moment.h, the same table as the phone controllers'
// page): OK and BACK in the menus, PICK, FLIP and PAGE at play select, PASS or SWITCH in a play. On offense, three
// small receiver buttons wrap around A (← ↓ →, the left, middle and right receiver, as the icons
// over their heads show; Receiver Buttons). Save states: small L, + (next slot) and S circles at the top right, always,
// with REDO beside them (UNDO in the menus: back a step); at team select UNIFORM and PRO BOWL stack below them.
//
// Input: SDL's finger events (the same on both phones), turned into an N64 button mask and stick that
// controllers.cpp's port_input merges into player 1, like the keyboard. The whole state is recomputed from the fingers
// down on every event, so a lost or cancelled touch can't leave a button held; a tap that starts and ends between two
// of the game's reads is held until the next read, so quick taps always count.
// Shown while touch is what's being played with: hidden once a controller is used (back at the next touch), while a
// menu is open (touches then drive the menu, as a mouse) and before the game starts.
// Drawing: an RmlUi context like the toasts' (toast.cpp), taking no input; 1 dp = screen height / 1080.
#include "blitz_platform.h"
#if BLITZ_HAS_TOUCH_CONTROLS
extern "C" int blitz_ds_active(void);
extern "C" volatile int blitz_ds_screen_mode;  // tall_view.cpp: 3 is DS Screens → Controller
extern "C" volatile int blitz_ds_mode;          // rt64_present_queue.cpp: 3, the picture on the second (upper) window alone
extern "C" volatile int blitz_ds_second_on_top;
extern "C" volatile int blitz_ds_panel_first;   // ui_state.cpp: the first window has BLITZ DS's board
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <cstdio>
#include <string>

#include "SDL2/SDL.h"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_image.h"
#include "elements/ui_label.h"
#include "ultramodern/ultramodern.hpp"
#include "game_moment.h"

extern "C" void blitz_savestate_request(int op);  // savestate.cpp: 1 save, 2 load (the current slot)
extern "C" void blitz_redo_request();             // Redo Play
namespace blitz {
    bool toast_tap(float x, float y);  // toast.cpp: a tap on a toast puts it away
    bool hints_tap(float x, float y);  // hints.cpp: a tap on a hint card puts it away
    bool uniform_picker_listening();   // uniform_picker.cpp: team select, picker on
    bool uniform_picker_season();      // uniform_picker.cpp: that team select is Season's (uniforms, no Pro Bowl)
    bool receiver_buttons_on(int port);  // receiver_targets.cpp: the setting, or BLITZ DS's Stylus for player 1
    bool ds_touch_routes_up();           // ds_touch.cpp: play select's bar of buttons on the lower screen
}
extern "C" void blitz_savestate_slot_step(int delta);  // savestate.cpp: the next slot

namespace {
    // N64 buttons (controllers.cpp's n64_button_values)
    constexpr uint16_t A = 0x8000, B = 0x4000, Z = 0x2000, START = 0x1000, R = 0x0010;
    constexpr uint16_t C_LEFT = 0x0002, C_DOWN = 0x0004, C_RIGHT = 0x0001;

    // A button that isn't the N64's: it acts when the finger lifts on it.
    enum class Action { None, Menu, Save, Load, Slot, Redo };
    enum class Anchor { Left, Right, Middle };
    // When a control is on screen.
    enum class When { Always, NotMenus, Offense, TeamSelect, SaveStates, Redo, PageOnly };
    // Its label from the moment's table (game_moment.h), or its own.
    enum class Text { Own, Pass, Jump, Turbo };

    // One control, in units of the screen's height from its anchor: the left edge, the right edge (x negative), or
    // the middle. radius is the drawn size; touches count within radius * hit.
    struct Button {
        const char* label;
        uint16_t mask;
        Anchor anchor;
        float x, y, radius;
        bool pill = false;  // a wide pill instead of a circle
        Action action = Action::None;
        When when = When::Always;
        Text text = Text::Own;
    };
    constexpr float hit = 1.35f;
    // The receiver buttons sit around A at 6:00, 4:30 and 3:00, close enough to roll a thumb onto.
    constexpr float a_x = -0.27f, a_y = 0.68f, ring = 0.155f, ring45 = ring * 0.7071f;
    constexpr std::array<Button, 15> buttons{{
        {"A", A, Anchor::Right, a_x, a_y, 0.085f, false, Action::None, When::Always, Text::Pass},
        {"B", B, Anchor::Right, -0.45f, 0.84f, 0.070f, false, Action::None, When::Always, Text::Jump},
        {"TURBO", Z, Anchor::Right, -0.42f, 0.50f, 0.070f, false, Action::None, When::NotMenus, Text::Turbo},
        {"\xE2\x86\x90", C_LEFT, Anchor::Right, a_x, a_y + ring, 0.050f, false, Action::None, When::Offense},           // ← 6:00
        {"\xE2\x86\x93", C_DOWN, Anchor::Right, a_x + ring45, a_y + ring45, 0.050f, false, Action::None, When::Offense}, // ↓ 4:30
        {"\xE2\x86\x92", C_RIGHT, Anchor::Right, a_x + ring, a_y, 0.050f, false, Action::None, When::Offense},          // → 3:00
        {"", START, Anchor::Middle, 0.0f, 0.93f, 0.034f},  // a drawn right-facing triangle (start_pixels)
        {"", 0, Anchor::Left, 0.075f, 0.09f, 0.036f, false, Action::Menu},  // a hamburger, left of the scoreboard
        // Save states and Redo Play (savestate.cpp), for a phone with no controller: what F7, F6, F5 and F8 do on a
        // computer. Small, at the top right, away from the play; L, + and S always (a state can be loaded from the
        // menus, to jump straight back into a game).
        {"L", 0, Anchor::Right, -0.31f, 0.09f, 0.036f, false, Action::Load, When::SaveStates},
        {"+", 0, Anchor::Right, -0.22f, 0.09f, 0.036f, false, Action::Slot, When::SaveStates},
        {"S", 0, Anchor::Right, -0.13f, 0.09f, 0.036f, false, Action::Save, When::SaveStates},
        {"REDO", 0, Anchor::Right, -0.47f, 0.09f, 0.036f, true, Action::Redo, When::Redo},
        // Team select (uniform_picker.cpp), stacked under the save state buttons: △ steps through uniforms (N64
        // C-left), R picks Pro Bowl players.
        {"UNIFORM", C_LEFT, Anchor::Right, -0.22f, 0.19f, 0.036f, true, Action::None, When::TeamSelect},
        {"PRO BOWL", R, Anchor::Right, -0.22f, 0.29f, 0.036f, true, Action::None, When::TeamSelect},
        // Play select with a controller in hand, when nothing else shows: a thumb's way to the next page, as Turbo.
        {"PAGE", Z, Anchor::Left, 0.13f, 0.05f, 0.03f, true, Action::None, When::PageOnly},
    }};
    constexpr float stick_x = 0.24f, stick_y = 0.70f, stick_radius = 0.15f;  // the stick's resting place and reach
    constexpr float stick_zone = 0.45f;  // the left 45% of the screen (below the menu button) is the stick's

    struct Finger {
        float x = 0, y = 0;           // pixels
        int control = -1;             // the button it went down on (-1 none), or stick_control
        float origin_x = 0, origin_y = 0;
    };
    constexpr int stick_control = 100;
    constexpr int swallowed = 101;  // a finger that put a toast or hint away: it presses nothing until it lifts
    SDL_Window* ui_window = nullptr;  // the game's window (touch_controls_update)

    std::mutex mutex;
    std::map<SDL_FingerID, Finger> fingers;
    uint16_t held = 0, latched = 0;   // held now; pressed since the game's last read
    float stick_dx = 0, stick_dy = 0; // -1..1, N64 axes (y up)
    float base_x = -1, base_y = -1;   // where the floating stick sits (pixels), -1: its resting place
    float screen_w = 1, screen_h = 1;
    std::atomic<uint32_t> last_touch{0}, last_pad{0};
    std::atomic<bool> menu_open{true}, visible{false};
    std::atomic<bool> page_only{false};  // only the PAGE pill is up

    // The thumbs' buttons are placed for a 16:9 phone, in units of its height: on a narrower screen (a Surface Duo's 4:3 panel)
    // that puts them further in than a thumb reaches. They come out toward the edge, 0.07 of the height at 4:3. The small
    // buttons along the top stay, and so does the stick (any nearer the edge, it set off the system's edge swipe).
    float edge_pull() {
        const float aspect = screen_w / std::max(screen_h, 1.0f);
        const float t = std::clamp((16.0f / 9.0f - aspect) / (16.0f / 9.0f - 4.0f / 3.0f), 0.0f, 1.0f);
        // At most as far as keeps the receiver ring's → (x -0.115, radius 0.05) clear of the edge (it touched at 0.07: the team's Duo).
        return std::min(0.07f * t, 0.045f);
    }

    // A control's centre in pixels.
    bool pad_place(const Button& b, float& cx, float& cy, float& radius);
    void centre(const Button& b, float& cx, float& cy) {
        float r;
        if (pad_place(b, cx, cy, r)) return;
        const float pull = b.y >= 0.4f ? edge_pull() : 0.0f;
        cx = b.anchor == Anchor::Left ? (b.x - pull) * screen_h : b.anchor == Anchor::Right ? screen_w + (b.x + pull) * screen_h
                                                                                   : screen_w * 0.5f + b.x * screen_h;
        cy = b.y * screen_h;
    }

    std::array<bool, buttons.size()> on_screen{};  // which controls show now (touch_controls_update; mutex held)

    // ---- DS Screens → Controller: the lower screen as the phone controller (phone_page.h), drawn here as images ----
    std::atomic<bool> pad_look{false};  // that mode is on (touch_controls_update)
    struct Rgb { float r, g, b; };
    Rgb hex(uint32_t v) { return {(float)((v >> 16) & 0xFF), (float)((v >> 8) & 0xFF), (float)(v & 0xFF)}; }
    Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
    void put(std::vector<char>& px, int w, int x, int y, Rgb c, float a) {  // over what's there
        char* p = &px[((size_t)y * (size_t)w + (size_t)x) * 4];
        const float da = (uint8_t)p[3] / 255.0f, oa = a + da * (1.0f - a);
        if (oa <= 0.0f) return;
        auto ch = [&](int i, float v) { p[i] = (char)(uint8_t)std::clamp((v * a + (uint8_t)p[i] * da * (1.0f - a)) / oa, 0.0f, 255.0f); };
        ch(0, c.r); ch(1, c.g); ch(2, c.b);
        p[3] = (char)(uint8_t)std::clamp(oa * 255.0f, 0.0f, 255.0f);
    }
    // The ground: navy, a purple glow at the top right, the stick side's cyan grid and the rule between the halves.
    std::vector<char> ground_pixels(int w, int h) {
        std::vector<char> px((size_t)w * h * 4, 0);
        const Rgb navy = hex(0x020712), purple = hex(0x3F0A5F), cyan = hex(0x8FEFF5), grid = hex(0x2FB3B3);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            const float gx = (x - w) / (0.9f * w), gy = y / (0.7f * h);
            const float glow = std::clamp(1.0f - std::sqrt(gx * gx + gy * gy) / 0.62f, 0.0f, 1.0f) * 0.85f;
            const float lx = x / (0.7f * w), ly = (y - h) / (0.6f * h);
            const float tint = std::clamp(1.0f - std::sqrt(lx * lx + ly * ly) / 0.7f, 0.0f, 1.0f) * 0.12f;
            Rgb c = mix(mix(navy, purple, glow), hex(0x2F6BFF), tint);
            put(px, w, x, y, c, 1.0f);
            const int cell = h / 20;
            if (x < w * 40 / 100 && (x % cell == 0 || y % cell == 0)) put(px, w, x, y, grid, 0.14f);
            if (std::abs(x - w * 40 / 100) <= 1) put(px, w, x, y, cyan, 0.24f);
        }
        return px;
    }
    // An arcade cap in a chrome bezel, with its drop shadow; brighter when held.
    std::vector<char> cap_pixels(int n, uint32_t c0, uint32_t c1, uint32_t c2, bool on) {
        std::vector<char> px((size_t)n * n * 4, 0);
        const float r = n * 0.47f, cx = n * 0.5f, cy = n * 0.47f, ring = n * 0.045f, shadow = on ? n * 0.01f : n * 0.03f;
        const Rgb chrome[7] = {hex(0xF4F7FC), hex(0x8C98AE), hex(0xE6ECF5), hex(0x4C5770), hex(0xC9D3E3), hex(0x6E7A92), hex(0xF4F7FC)};
        const float stops[7] = {0.0f, 0.18f, 0.32f, 0.50f, 0.68f, 0.84f, 1.0f};
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
            const float sd = std::hypot(x + 0.5f - cx, y + 0.5f - cy - shadow);
            if (sd < r) put(px, n, x, y, hex(0x000000), 0.45f * std::clamp(r - sd, 0.0f, 1.0f));
            const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
            const float edge = std::clamp(r - d, 0.0f, 1.0f);
            if (edge <= 0.0f) continue;
            if (d > r - ring) {  // the bezel: a conic sweep from 210 degrees
                float t = std::fmod((std::atan2(x - cx, -(y - cy)) * 57.29578f - 210.0f + 720.0f), 360.0f) / 360.0f;
                int k = 0;
                while (k < 5 && t > stops[k + 1]) k++;
                put(px, n, x, y, mix(chrome[k], chrome[k + 1], (t - stops[k]) / (stops[k + 1] - stops[k])), edge);
                continue;
            }
            const float inner = r - ring;
            const float ux = (x + 0.5f - (cx - inner * 0.1f)) / inner, uy = (y + 0.5f - (cy - inner * 0.4f)) / inner;
            const float t = std::clamp(std::sqrt(ux * ux + uy * uy) / 1.4f, 0.0f, 1.0f);
            Rgb c = t < 0.58f ? mix(hex(c0), hex(c1), t / 0.58f) : mix(hex(c1), hex(c2), (t - 0.58f) / 0.42f);
            if (on) c = {std::min(255.0f, c.r * 1.35f), std::min(255.0f, c.g * 1.35f), std::min(255.0f, c.b * 1.35f)};
            const float fy = (y + 0.5f - cy) / inner;
            if (fy > 0.86f) c = mix(c, hex(0x000000), 0.25f);  // the lip, below
            if (fy < -0.88f) c = mix(c, hex(0xFFFFFF), 0.18f);  // and the shine, above
            put(px, n, x, y, c, std::clamp(inner - d, 0.0f, 1.0f));
        }
        return px;
    }
    // The stick's ring: a cyan rim, a faint fill, a dashed ring inside; and its knob, player 1's blue.
    std::vector<char> ring_pixels(int n) {
        std::vector<char> px((size_t)n * n * 4, 0);
        const float c = n * 0.5f, r = n * 0.49f;
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
            const float d = std::hypot(x + 0.5f - c, y + 0.5f - c);
            if (d > r) continue;
            put(px, n, x, y, hex(0x06122C), 0.2f + 0.25f * (d / r));
            if (d > r - n * 0.02f) put(px, n, x, y, hex(0x8FEFF5), 0.9f);
            const float inner = r - n * 0.12f;
            const float a = std::atan2(y - c, x - c) * 57.29578f + 180.0f;
            if (std::fabs(d - inner) < n * 0.007f && std::fmod(a, 12.0f) < 7.0f) put(px, n, x, y, hex(0x8FEFF5), 0.35f);
        }
        return px;
    }
    std::vector<char> knob_pixels(int n) {
        std::vector<char> px((size_t)n * n * 4, 0);
        const float c = n * 0.5f, r = n * 0.46f;
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
            const float d = std::hypot(x + 0.5f - c, y + 0.5f - c);
            if (d > r + 1.0f) continue;
            const float ux = (x - n * 0.4f) / r, uy = (y - n * 0.32f) / r, t = std::clamp(std::sqrt(ux * ux + uy * uy) / 1.4f, 0.0f, 1.0f);
            Rgb col = t < 0.55f ? mix(hex(0xA9C2FF), hex(0x2F6BFF), t / 0.55f) : mix(hex(0x2F6BFF), hex(0x13306E), (t - 0.55f) / 0.45f);
            if (d > r - n * 0.05f) col = hex(0x0A1022);  // its dark ring
            put(px, n, x, y, col, std::clamp(r + 1.0f - d, 0.0f, 1.0f));
        }
        return px;
    }
    // START's mark: a right-facing triangle (team, 2026-11-27), white, a little right of centre so it looks centred.
    std::vector<char> start_pixels(int n) {
        std::vector<char> px((size_t)n * n * 4, 0);
        const float x0 = n * 0.32f, x1 = n * 0.76f, cy = n * 0.5f, half = n * 0.25f;
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            if (fx < x0 - 1.0f || fx > x1 + 1.0f) continue;
            const float t = (fx - x0) / (x1 - x0), allowed = half * (1.0f - t);
            const float a = std::clamp(allowed - std::fabs(fy - cy) + 0.5f, 0.0f, 1.0f) * std::clamp(fx - x0 + 0.5f, 0.0f, 1.0f);
            if (a > 0.0f) put(px, n, x, y, hex(0xFFFFFF), a * 0.92f);
        }
        return px;
    }

    // Where PASS, JUMP and TURBO sit in that mode, as the phone page lays them out in its button area (54% wide, below the
    // top row): in units of the screen's height. False for the others, which keep their places.
    bool pad_place(const Button& b, float& cx, float& cy, float& radius) {
        if (!pad_look.load() || b.action != Action::None) return false;
        if (b.mask == START && b.anchor == Anchor::Middle) {  // up in the top row, as the phone page has it, clear of JUMP
            radius = b.radius;
            cx = screen_w * 0.5f;
            cy = 0.09f * screen_h;
            return true;
        }
        if (b.anchor != Anchor::Right) return false;
        const float W = screen_w / std::max(screen_h, 1.0f), u = std::min(0.54f * W, 0.86f);
        float d, right, bottom;
        if (b.mask == A && b.text == Text::Pass) { d = 0.52f * u; right = 0.04f * u; bottom = 0.06f * u; }
        else if (b.mask == B && b.text == Text::Jump) { d = 0.40f * u; right = 0.60f * u; bottom = 0.04f * u; }
        else if (b.mask == Z && b.text == Text::Turbo) { d = 0.36f * u; right = 0.38f * u; bottom = 0.58f * u; }
        else return false;
        radius = d * 0.5f;
        cx = (W - right - radius) * screen_h;
        cy = (1.0f - bottom - radius) * screen_h;
        return true;
    }
    float stick_reach() { return pad_look.load() ? 0.19f : stick_radius; }

    int button_at(float x, float y) {
        int best = -1;
        float best_d = 1e9f;
        for (size_t i = 0; i < buttons.size(); i++) {
            if (!on_screen[i]) continue;
            float cx, cy;
            centre(buttons[i], cx, cy);
            float pr = 0, px_ = 0, py_ = 0;
            const float radius = pad_place(buttons[i], px_, py_, pr) ? pr : buttons[i].radius;
            float r = radius * screen_h * hit * (buttons[i].pill ? 2.0f : 1.0f);
            float d = std::hypot((x - cx) / (buttons[i].pill ? 2.0f : 1.0f), y - cy);
            if (d <= r && d < best_d) {
                best = (int)i;
                best_d = d;
            }
        }
        return best;
    }

    // The state from the fingers down now (mutex held).
    void recompute() {
        uint16_t now = 0;
        float dx = 0, dy = 0;
        bool stick = false;
        for (auto& [id, f] : fingers) {
            if (f.control == swallowed) continue;
            if (f.control == stick_control) {
                stick = true;
                float r = stick_reach() * screen_h;
                dx = (f.x - f.origin_x) / r;
                dy = (f.y - f.origin_y) / r;
                float len = std::hypot(dx, dy);
                if (len > 1.0f) {  // past the edge, the base follows the thumb
                    f.origin_x = f.x - dx / len * r;
                    f.origin_y = f.y - dy / len * r;
                    dx /= len;
                    dy /= len;
                }
                base_x = f.origin_x;
                base_y = f.origin_y;
            }
            else {
                // A finger may slide from one button to another (B to A for a jump then a pass).
                int b = button_at(f.x, f.y);
                // On BLITZ DS a tap on the header already turns the page (ds_view.cpp): the pill just shows that it can.
                if (b >= 0 && !(page_only && blitz_ds_active())) now |= buttons[(size_t)b].mask;
            }
        }
        if (!stick) {
            base_x = base_y = -1;
            dx = dy = 0;
        }
        latched |= now & ~held;
        if (now != held) fprintf(stderr, "[touch] buttons %04X\n", now);  // logcat / blitz.log: what the screen pressed
        held = now;
        stick_dx = dx;
        stick_dy = -dy;
    }

    void open_settings() {
        SDL_Event e{};
        e.type = SDL_KEYDOWN;
        e.key.state = SDL_PRESSED;
        e.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
        e.key.keysym.sym = SDLK_ESCAPE;
        SDL_PushEvent(&e);
        e.type = SDL_KEYUP;
        e.key.state = SDL_RELEASED;
        SDL_PushEvent(&e);
    }

    // SDL's event watch (the main thread, as events are pumped).
    int on_event(void*, SDL_Event* e) {
        switch (e->type) {
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
            case SDL_FINGERUP: {
                // With only the PAGE pill up (a controller in hand, at play select), a touch is for it: it doesn't bring the
                // rest of the touch controls up (team: that made PAGE useless on a phone).
                if (!page_only) last_touch = SDL_GetTicks();
                if (e->type == SDL_FINGERDOWN) {
                    // A toast or hint under the finger is put away, and the touch goes no further (in a menu too).
                    int pw = 0, ph = 0;
                    if (ui_window) SDL_GetWindowSizeInPixels(ui_window, &pw, &ph);
                    float ux = e->tfinger.x * pw, uy = e->tfinger.y * ph;
                    if (blitz::toast_tap(ux, uy) || blitz::hints_tap(ux, uy)) {
                        std::lock_guard lock{mutex};
                        Finger f;
                        f.control = swallowed;
                        fingers[e->tfinger.fingerId] = f;
                        return 1;
                    }
                }
                if (menu_open || !ultramodern::is_game_started()) {
                    std::lock_guard lock{mutex};
                    fingers.clear();
                    recompute();
                    return 1;
                }
                std::lock_guard lock{mutex};
                float x = e->tfinger.x * screen_w, y = e->tfinger.y * screen_h;
                if (e->type == SDL_FINGERUP) {
                    auto it = fingers.find(e->tfinger.fingerId);
                    if (it != fingers.end() && it->second.control >= 0 && it->second.control < (int)buttons.size() &&
                        button_at(x, y) == it->second.control) {
                        switch (buttons[(size_t)it->second.control].action) {
                            case Action::Menu: open_settings(); break;
                            case Action::Save: blitz_savestate_request(1); break;  // as F5
                            case Action::Load: blitz_savestate_request(2); break;  // as F7
                            case Action::Slot: blitz_savestate_slot_step(1); break;  // as F6
                            case Action::Redo: blitz_redo_request(); break;        // as F8
                            case Action::None: break;
                        }
                    }
                    fingers.erase(e->tfinger.fingerId);
                }
                else if (e->type == SDL_FINGERDOWN) {
                    Finger f;
                    f.x = x;
                    f.y = y;
                    f.control = button_at(x, y);
                    if (f.control < 0 && x < screen_w * stick_zone && !page_only) {
                        f.control = stick_control;
                        f.origin_x = x;
                        f.origin_y = y;
                    }
                    fingers[e->tfinger.fingerId] = f;
                }
                else if (auto it = fingers.find(e->tfinger.fingerId); it != fingers.end()) {
                    it->second.x = x;
                    it->second.y = y;
                }
                recompute();
                return 1;
            }
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_KEYDOWN:
                last_pad = SDL_GetTicks();
                return 1;
            case SDL_CONTROLLERAXISMOTION:
                if (std::abs(e->caxis.value) > 12000) last_pad = SDL_GetTicks();
                return 1;
            case SDL_WINDOWEVENT:
                if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    std::lock_guard lock{mutex};
                    screen_w = (float)std::max(1, e->window.data1);
                    screen_h = (float)std::max(1, e->window.data2);
                }
                return 1;
            default:
                return 1;
        }
    }

    // --- Drawing ----------------------------------------------------------------------------------------------------

    recompui::ContextId context = recompui::ContextId::null();
    recompui::Element* stick_base = nullptr;
    recompui::Element* stick_knob = nullptr;
    std::array<recompui::Element*, buttons.size()> button_el{};
    recompui::Element* pad_ground = nullptr;  // DS Screens → Controller: the lower screen's navy, as the phone controller's page
    std::array<recompui::Element*, buttons.size()> cap_img{}, cap_img_on{};  // that mode's arcade caps (PASS, JUMP, TURBO)
    std::array<recompui::Label*, buttons.size()> pad_label{};                 // and their lettering
    recompui::Element* ring_img = nullptr;
    recompui::Element* knob_img = nullptr;
    bool drawn_pad_look = false;
    std::array<recompui::Label*, buttons.size()> button_label{};
    std::array<std::string, buttons.size()> drawn_label{};
    bool shown = false;
    uint16_t drawn_held = 0xFFFF;
    std::array<bool, buttons.size()> drawn_on{};  // on_screen as last drawn
    bool drawn_once = false;

    // Characters, not bytes (a label may be an arrow).
    size_t chars(const std::string& s) {
        return (size_t)std::count_if(s.begin(), s.end(), [](char c) { return ((unsigned char)c & 0xC0) != 0x80; });
    }

    void set_label(size_t i, const std::string& text) {
        float d = buttons[i].radius * 2 * 1080.0f;
        size_t n = chars(text);
        button_label[i]->set_text(text);
        float pcx, pcy, pr;
        if (pad_label[i] != nullptr) {
            pad_label[i]->set_text(text);
            if (pad_place(buttons[i], pcx, pcy, pr)) {  // the phone page's cap lettering: Barlow, italic, heavy
                const float pd = pr * 2 * 1080.0f;
                pad_label[i]->set_font_size(std::min(pd * 0.17f, pd * 0.9f / (float)std::max<size_t>(n, 1)));
                pad_label[i]->set_line_height(pd - 6.0f);
            }
        }
        button_label[i]->set_line_height(d - 6.0f);
        button_label[i]->set_font_size(n > 2 ? std::min(d * 0.30f, d * 1.2f / (float)n) : d * 0.42f);
        drawn_label[i] = text;
    }
    float drawn_bx = -2, drawn_by = -2, drawn_dx = -2, drawn_dy = -2, drawn_w = 0, drawn_h = 0;

    float dp(float pixels) { return pixels * 1080.0f / screen_h; }

    recompui::Element* circle(recompui::Element* parent, float d, recompui::Color fill, recompui::Color edge) {
        using namespace recompui;
        Element* e = context.create_element<Element>(parent);
        e->set_position(Position::Absolute);
        e->set_width(d);
        e->set_height(d);
        e->set_border_radius(d * 0.5f);
        e->set_background_color(fill);
        e->set_border_width(3.0f);
        e->set_border_color(edge);
        e->set_pointer_events(PointerEvents::None);
        return e;
    }

    void build() {
        using namespace recompui;
        context = create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        Element* root = context.create_element<Element>(context.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(0);
        root->set_left(0);
        root->set_right(0);
        root->set_bottom(0);
        root->set_pointer_events(PointerEvents::None);
        pad_ground = context.create_element<Element>(root);
        pad_ground->set_position(Position::Absolute);
        pad_ground->set_top(0);
        pad_ground->set_left(0);
        pad_ground->set_right(0);
        pad_ground->set_bottom(0);
        pad_ground->set_background_color(Color{2, 7, 18, 255});
        pad_ground->set_pointer_events(PointerEvents::None);
        pad_ground->set_display(Display::None);
        {   // The phone controller's art, drawn once (pad_look turns it on).
            const int gw = 900, gh = 675;
            queue_image_from_bytes_rgba32("?/blitz/pad_ground", ground_pixels(gw, gh), gw, gh);
            queue_image_from_bytes_rgba32("?/blitz/pad_ring", ring_pixels(256), 256, 256);
            queue_image_from_bytes_rgba32("?/blitz/pad_knob", knob_pixels(128), 128, 128);
            queue_image_from_bytes_rgba32("?/blitz/start_tri", start_pixels(96), 96, 96);
            const struct { const char* name; uint32_t c0, c1, c2; } caps[3] = {
                {"pass", 0xF0414F, 0xB3101E, 0x6E0812}, {"jump", 0x7A2BB0, 0x3F0A5F, 0x22053A}, {"turbo", 0xFFD84A, 0xFF9A00, 0xE05200}};
            for (const auto& c : caps) {
                queue_image_from_bytes_rgba32(std::string("?/blitz/pad_") + c.name, cap_pixels(256, c.c0, c.c1, c.c2, false), 256, 256);
                queue_image_from_bytes_rgba32(std::string("?/blitz/pad_") + c.name + "_on", cap_pixels(256, c.c0, c.c1, c.c2, true), 256, 256);
            }
            Element* g = context.create_element<Image>(pad_ground, "?/blitz/pad_ground");
            g->set_position(Position::Absolute);
            g->set_left(0);
            g->set_top(0);
            g->set_width(100.0f, Unit::Percent);
            g->set_height(100.0f, Unit::Percent);
            g->set_pointer_events(PointerEvents::None);
        }
        const Color fill{10, 12, 24, 110}, edge{255, 255, 255, 140};  // dark glass: reads over any picture
        stick_base = circle(root, stick_radius * 2 * 1080.0f, fill, edge);
        stick_knob = circle(root, stick_radius * 0.9f * 1080.0f, Color{255, 255, 255, 90}, Color{255, 255, 255, 170});
        auto fill_with = [&](Element* parent, const std::string& src) {
            Element* im = context.create_element<Image>(parent, src);
            im->set_position(Position::Absolute);
            im->set_left(0);
            im->set_top(0);
            im->set_width(100.0f, Unit::Percent);
            im->set_height(100.0f, Unit::Percent);
            im->set_pointer_events(PointerEvents::None);
            im->set_display(Display::None);
            return im;
        };
        ring_img = fill_with(stick_base, "?/blitz/pad_ring");
        knob_img = fill_with(stick_knob, "?/blitz/pad_knob");
        for (size_t i = 0; i < buttons.size(); i++) {
            const Button& b = buttons[i];
            float d = b.radius * 2 * 1080.0f;
            Element* e = circle(root, d, fill, edge);
            if (b.pill) {
                e->set_width(d * 2.4f);
                e->set_border_radius(d * 0.5f);
            }
            Label* l = context.create_element<Label>(e, b.label, theme::Typography::LabelSM);
            l->set_position(Position::Absolute);
            l->set_left(0);
            l->set_right(0);
            l->set_top(0);
            l->set_bottom(0);
            l->set_text_align(TextAlign::Center);
            l->set_line_height(d - 6.0f);
            l->set_color(Color{255, 255, 255, 220});
            if (b.mask == START && b.anchor == Anchor::Middle) {  // a drawn triangle, not a font's glyph
                l->set_display(Display::None);
                fill_with(e, "?/blitz/start_tri")->set_display(Display::Block);
            }
            if (b.action == Action::Menu) {  // three bars: no font here has ☰
                for (int k = 0; k < 3; k++) {
                    Element* bar = context.create_element<Element>(e);
                    bar->set_position(Position::Absolute);
                    bar->set_left(d * 0.27f);
                    bar->set_width(d * 0.46f - 6.0f);
                    bar->set_top(d * (0.30f + 0.17f * (float)k) - 4.0f);
                    bar->set_height(d * 0.07f);
                    bar->set_border_radius(d * 0.035f);
                    bar->set_background_color(Color{255, 255, 255, 220});
                    bar->set_pointer_events(PointerEvents::None);
                }
            }
            button_el[i] = e;
            button_label[i] = l;
            {
                const char* cap = b.text == Text::Pass && b.mask == A ? "pass" : b.text == Text::Jump && b.mask == B ? "jump"
                                : b.text == Text::Turbo && b.mask == Z ? "turbo" : nullptr;
                if (cap != nullptr && b.action == Action::None) {
                    cap_img[i] = fill_with(e, std::string("?/blitz/pad_") + cap);
                    cap_img_on[i] = fill_with(e, std::string("?/blitz/pad_") + cap + "_on");
                    // The cap's own lettering, over the art (made after it, so drawn after it); the plain label is for the
                    // overlay look, and the two take turns.
                    Label* pl = context.create_element<Label>(e, b.label, theme::Typography::LabelSM);
                    pl->set_position(Position::Absolute);
                    pl->set_left(0);
                    pl->set_right(0);
                    pl->set_top(0);
                    pl->set_bottom(0);
                    pl->set_text_align(TextAlign::Center);
                    pl->set_font_family("Barlow Condensed");
                    pl->set_font_style(FontStyle::Italic);
                    pl->set_font_weight(800);
                    pl->set_display(Display::None);
                    pad_label[i] = pl;
                }
            }
            set_label(i, b.label);
        }
        context.close();
    }

    void place(recompui::Element* e, float cx_px, float cy_px, float w_dp, float h_dp) {
        e->set_left(dp(cx_px) - w_dp * 0.5f);
        e->set_top(dp(cy_px) - h_dp * 0.5f);
    }
}

namespace blitz {
    // At startup (main.cpp), once SDL's video is up.
    void touch_controls_init() {
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");  // touches still drive the menus (as a mouse)
        SDL_AddEventWatch(on_event, nullptr);
    }

    // Whether the touch controls are on screen (receiver_targets.cpp draws tap targets then, not button icons).
    bool touch_controls_showing() { return visible && !page_only; }  // the PAGE pill alone leaves taps to BLITZ DS

    // Game thread (controllers.cpp, player 1's read): the touch state merged into the controller's.
    void touch_controls_read(uint16_t* out_buttons, float* x, float* y) {
        if (!visible) return;
        std::lock_guard lock{mutex};
        *out_buttons |= (held | latched);
        latched = 0;
        if (stick_dx != 0 || stick_dy != 0) {
            *x = stick_dx;
            *y = stick_dy;
        }
    }

    // Every UI tick (main.cpp's update_gfx): show, hide and redraw.
    void touch_controls_update(SDL_Window* window) {
        ui_window = window;
        menu_open = recompui::is_context_capturing_input();
        bool on = std::get<bool>(recompui::config::get_general_config().get_option_value("touch_controls"));
        bool controller = SDL_NumJoysticks() > 0;
        // BLITZ DS with a controller: real buttons, and taps have their own meaning (ds_view.cpp). Without one (a Surface
        // Duo) these are all there is.
        // DS Screens → Controller: the lower screen is the controller, a pad in hand or not.
        const bool controller_screen = blitz_ds_active() && blitz_ds_screen_mode == 3;
        // The phone controller's look whenever the lower screen is only controls: DS Screens → Controller, and in any mode while
        // the game's picture is on the upper screen alone (menus, team select, cut scenes: the team, 2026-11-27). Live play over
        // both screens and play select on the lower keep the see-through look over the picture.
        pad_look = controller_screen || (blitz_ds_active() && blitz_ds_mode == 3 && blitz_ds_second_on_top != 0 && blitz_ds_panel_first == 0);
        bool want = on && ultramodern::is_game_started() && !menu_open &&
                    (controller_screen || (!(blitz_ds_active() && controller) && (!controller || last_touch.load() > last_pad.load())));
        // The PAGE pill alone (a controller in hand at play select) is gone (team, 2026-11-02): a controller has Z for it, and
        // the touch controls have their own Turbo. BLITZ DS's bar has a PAGE of its own (ds_touch.cpp).
        {   // Why they're up or not, each time that changes (a Surface Duo spanned had none, 2026-11-26).
            static int last = -1;
            const int why = (on ? 1 : 0) | (blitz_ds_active() ? 2 : 0) | (controller ? 4 : 0) | (ultramodern::is_game_started() ? 8 : 0) |
                            (menu_open ? 16 : 0) | (last_touch.load() > last_pad.load() ? 32 : 0) | (want ? 64 : 0);
            if (why != last) {
                last = why;
                fprintf(stderr, "[touch] controls %s: setting %d, two screens %d, controllers %d, game %d, menu %d, touched last %d\n", want ? "up" : "down",
                    on ? 1 : 0, blitz_ds_active() ? 1 : 0, SDL_NumJoysticks(), ultramodern::is_game_started() ? 1 : 0, menu_open ? 1 : 0, last_touch.load() > last_pad.load() ? 1 : 0);
                for (int i = 0; i < SDL_NumJoysticks(); i++) fprintf(stderr, "[touch]   joystick %d: %s\n", i, SDL_JoystickNameForIndex(i));
            }
        }
        bool page_pill = false;
        page_only = page_pill;
        want = want || page_pill;
        if (window) {
            int w = 0, h = 0;
            SDL_GetWindowSize(window, &w, &h);
            if (w > 0 && h > 0) {
                std::lock_guard lock{mutex};
                screen_w = (float)w;
                screen_h = (float)h;
            }
        }
        visible = want;
        if (!want) {
            if (shown) {
                recompui::hide_context(context);
                shown = false;
                std::lock_guard lock{mutex};
                fingers.clear();
                recompute();
            }
            return;
        }
        if (context == recompui::ContextId::null()) build();
        uint16_t h;
        float bx, by, dx, dy, sw, sh;
        {
            std::lock_guard lock{mutex};
            h = held;
            bx = base_x < 0 ? stick_x * screen_h : base_x;  // not pulled out: nearer the edge, a thumb set off the system's edge swipe (team, Surface Duo)
            by = base_y < 0 ? stick_y * screen_h : base_y;
            dx = stick_dx;
            dy = stick_dy;
            sw = screen_w;
            sh = screen_h;
        }
        // What shows: by the moment (game_moment.h) and the settings; labels by the moment.
        auto general = [](const char* key) { return std::get<bool>(recompui::config::get_general_config().get_option_value(key)); };
        blitz::GameMoment moment = blitz::moment_for(0);
        bool menus = moment == blitz::GameMoment::Menus;
        bool team_select = blitz::uniform_picker_listening();
        bool receivers_on = blitz::receiver_buttons_on(0);
        std::array<bool, buttons.size()> show{};
        for (size_t i = 0; i < buttons.size(); i++) {
            switch (buttons[i].when) {
                case When::Always: show[i] = true; break;
                case When::NotMenus: show[i] = !menus; break;
                case When::Offense: show[i] = moment == blitz::GameMoment::Offense && receivers_on && !pad_look.load(); break;
                case When::TeamSelect: show[i] = team_select && (buttons[i].mask != R || !blitz::uniform_picker_season()); break;  // no Pro Bowl in Season
                case When::SaveStates: show[i] = general("savestate_buttons"); break;
                case When::Redo: show[i] = general("redo_play"); break;  // in the menus: back a step (UNDO)
                case When::PageOnly: show[i] = false; break;
            }
            if (page_pill) show[i] = buttons[i].when == When::PageOnly;
        }
        const blitz::MomentText& words = blitz::moment_text(moment);
        context.open();
        stick_base->set_display(page_pill ? recompui::Display::None : recompui::Display::Block);
        stick_knob->set_display(page_pill ? recompui::Display::None : recompui::Display::Block);
        for (size_t i = 0; i < buttons.size(); i++) {
            if (!drawn_once || show[i] != drawn_on[i]) button_el[i]->set_display(show[i] ? recompui::Display::Block : recompui::Display::None);
            const char* text = buttons[i].text == Text::Pass ? words.pass.label : buttons[i].text == Text::Jump ? words.jump.label
                             : buttons[i].text == Text::Turbo ? words.turbo.label
                             : buttons[i].action == Action::Redo && menus ? "UNDO" : buttons[i].label;
            if (drawn_label[i] != text) set_label(i, text);
        }
        context.close();
        drawn_on = show;
        drawn_once = true;
        {
            std::lock_guard lock{mutex};
            on_screen = show;
        }
        const bool look = pad_look.load();
        bool layout = sw != drawn_w || sh != drawn_h || look != drawn_pad_look;
        if (layout || h != drawn_held || bx != drawn_bx || by != drawn_by || dx != drawn_dx || dy != drawn_dy) {
            context.open();
            const float reach = stick_reach();
            float base_d = reach * 2 * 1080.0f, knob_d = reach * (look ? 0.98f : 0.9f) * 1080.0f;  // the controller's knob about half its ring, as the phone page's (team)
            stick_base->set_width(base_d);
            stick_base->set_height(base_d);
            stick_base->set_border_radius(base_d * 0.5f);
            stick_knob->set_width(knob_d);
            stick_knob->set_height(knob_d);
            stick_knob->set_border_radius(knob_d * 0.5f);
            stick_base->set_border_width(look ? 0.0f : 3.0f);
            stick_knob->set_border_width(look ? 0.0f : 3.0f);
            ring_img->set_display(look ? recompui::Display::Block : recompui::Display::None);
            knob_img->set_display(look ? recompui::Display::Block : recompui::Display::None);
            place(stick_base, bx, by, base_d, base_d);
            place(stick_knob, bx + dx * reach * sh, by - dy * reach * sh, knob_d, knob_d);
            for (size_t i = 0; i < buttons.size(); i++) {
                const Button& b = buttons[i];
                float cx, cy;
                centre(b, cx, cy);
                float pcx, pcy, pr;
                const bool capped = look && cap_img[i] != nullptr && pad_place(b, pcx, pcy, pr);
                float d = (capped ? pr : b.radius) * 2 * 1080.0f;
                button_el[i]->set_width(b.pill ? d * 2.4f : d);
                button_el[i]->set_height(d);
                button_el[i]->set_border_radius(d * 0.5f);
                place(button_el[i], cx, cy, b.pill ? d * 2.4f : d, d);
                bool down = b.action == Action::None && (h & b.mask);
                if (cap_img[i] != nullptr) {
                    cap_img[i]->set_display(capped && !down ? recompui::Display::Block : recompui::Display::None);
                    cap_img_on[i]->set_display(capped && down ? recompui::Display::Block : recompui::Display::None);
                    button_el[i]->set_border_width(capped ? 0.0f : 3.0f);
                    pad_label[i]->set_display(capped ? recompui::Display::Block : recompui::Display::None);
                    button_label[i]->set_display(capped ? recompui::Display::None : recompui::Display::Block);
                    if (look != drawn_pad_look) set_label(i, drawn_label[i]);
                    if (capped) {
                        button_el[i]->set_background_color(recompui::Color{0, 0, 0, 0});
                        pad_label[i]->set_color(b.mask == A ? recompui::Color{255, 225, 74, 255} : b.mask == Z ? recompui::Color{26, 10, 0, 255} : recompui::Color{242, 242, 242, 255});
                        continue;
                    }
                }
                if (!look) {
                    button_el[i]->set_background_color(down ? recompui::Color{255, 196, 0, 170} : recompui::Color{10, 12, 24, 110});
                    button_el[i]->set_border_color(recompui::Color{255, 255, 255, 140});
                    button_label[i]->set_color(recompui::Color{255, 255, 255, 220});
                    continue;
                }
                // The phone controller's buttons (phone_page.h): PASS red with yellow letters, JUMP purple, TURBO orange,
                // the rest the house navy, all with the cyan rim; brighter while held.
                recompui::Color fill{13, 26, 58, 235}, text{242, 242, 242, 255};
                if (b.mask == A && b.action == Action::None) { fill = down ? recompui::Color{240, 65, 79, 255} : recompui::Color{179, 16, 30, 240}; text = {255, 225, 74, 255}; }
                else if (b.mask == B && b.action == Action::None) fill = down ? recompui::Color{122, 43, 176, 255} : recompui::Color{63, 10, 95, 240};
                else if (b.mask == Z && b.action == Action::None) { fill = down ? recompui::Color{255, 216, 74, 255} : recompui::Color{255, 154, 0, 240}; text = {26, 10, 0, 255}; }
                else if (down) fill = {246, 212, 0, 220};
                button_el[i]->set_background_color(fill);
                button_el[i]->set_border_color(recompui::Color{143, 239, 245, 200});
                button_label[i]->set_color(text);
            }
            pad_ground->set_display(look ? recompui::Display::Block : recompui::Display::None);
            stick_base->set_background_color(look ? recompui::Color{0, 0, 0, 0} : recompui::Color{10, 12, 24, 110});
            stick_base->set_border_color(recompui::Color{255, 255, 255, 140});
            stick_knob->set_background_color(look ? recompui::Color{0, 0, 0, 0} : recompui::Color{255, 255, 255, 90});
            context.close();
            drawn_w = sw;
            drawn_h = sh;
            drawn_pad_look = look;
            drawn_held = h;
            drawn_bx = bx;
            drawn_by = by;
            drawn_dx = dx;
            drawn_dy = dy;
        }
        if (!shown) {
            recompui::show_context(context, "");
            shown = true;
        }
    }
}
#else
#include <cstdint>
struct SDL_Window;
namespace blitz {
    void touch_controls_init() {}
    bool touch_controls_showing() { return false; }
    void touch_controls_read(uint16_t*, float*, float*) {}
    void touch_controls_update(SDL_Window*) {}
}
#endif
