// The PSP's input (docs/PSP_PORT.md §4.4): sceCtrl in analog mode, read when the game polls its controller, merged with
// the scripted presses (an <app>/input_script.txt and the "live" command; backends_common.cpp).
//   PSP      N64                          PSP      N64
//   cross    A                            L        Z  (turbo, as the RG DS's L1)
//   square   B                            R        R  (turbo)
//   triangle C-up                         START    START
//   circle   C-right                      d-pad    d-pad
//   nub      stick (deadzone 0.15, curve 1.5: the nub is weak)
//   SELECT   held: the hotkey modifier; nothing reaches the game while it is held. A hotkey fires only for a press made
//            after SELECT was already down (the Nov 2 controls rule), never for a button that was held when SELECT went down:
//              SELECT + triangle   overlay off -> hud -> log
//              SELECT + circle     present 30 -> auto -> 20
// The `pad` command (psp_main.cpp) adds synthetic PSP button presses before the map ("pad SELECT 1", "pad +0.3 TRIANGLE"),
// so the map and the hotkey rule can be exercised in PPSSPP, where no real keys are pressed.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <pspctrl.h>

#include "psp_log.h"
#include "psp_sys.h"
#include "host/host_backends.h"

namespace {
    constexpr uint16_t N64_A = 0x8000, N64_B = 0x4000, N64_Z = 0x2000, N64_START = 0x1000, N64_UP = 0x0800, N64_DOWN = 0x0400,
                       N64_LEFT = 0x0200, N64_RIGHT = 0x0100, N64_R = 0x0010, N64_CUP = 0x0008,
                       N64_CRIGHT = 0x0001;

    struct PadEvent { uint64_t start, end; unsigned buttons; };
    pspsys::Mutex* g_pad_mutex = nullptr;
    std::vector<PadEvent> g_pad;
    unsigned g_prev = 0;   // the PSP buttons at the last poll (real + synthetic)
    bool g_input_log = false;   // the inputlog switch file: every raw change, for "down goes up" (PSP-1001, 2026-11-04)

    unsigned pad_mask(const std::string& name) {
        static const std::pair<const char*, unsigned> table[] = {
            {"CROSS", PSP_CTRL_CROSS}, {"SQUARE", PSP_CTRL_SQUARE}, {"TRIANGLE", PSP_CTRL_TRIANGLE}, {"CIRCLE", PSP_CTRL_CIRCLE},
            {"L", PSP_CTRL_LTRIGGER}, {"R", PSP_CTRL_RTRIGGER}, {"START", PSP_CTRL_START}, {"SELECT", PSP_CTRL_SELECT},
            {"UP", PSP_CTRL_UP}, {"DOWN", PSP_CTRL_DOWN}, {"LEFT", PSP_CTRL_LEFT}, {"RIGHT", PSP_CTRL_RIGHT},
        };
        for (auto& [n, m] : table)
            if (name == n) return m;
        return 0;
    }

    unsigned synthetic(uint64_t now) {
        if (g_pad_mutex == nullptr) return 0;
        unsigned b = 0;
        pspsys::Lock l(*g_pad_mutex);
        for (size_t i = 0; i < g_pad.size();) {
            if (now >= g_pad[i].end) { g_pad.erase(g_pad.begin() + (ptrdiff_t)i); continue; }
            if (now >= g_pad[i].start) b |= g_pad[i].buttons;
            i++;
        }
        return b;
    }

    void hotkey(unsigned pressed) {
        if (pressed & PSP_CTRL_TRIANGLE) {
            int m = (host::overlay_mode() + 1) % 3;
            host::set_overlay_mode(m);
            PLOG("input", "SELECT + triangle: overlay %s", host::overlay_mode_name(m));
        }
        if (pressed & PSP_CTRL_CIRCLE) {
            int m = (host::present_mode() + 1) % 3;
            host::set_present_mode(m);
            PLOG("input", "SELECT + circle: present %s", host::present_mode_name(m));
        }
    }
}

namespace host {
    void init_input() {
        g_pad_mutex = new pspsys::Mutex();
        sceCtrlSetSamplingCycle(0);
        sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
        g_input_log = pspsys::exists(psplog::app_path("inputlog"));
        if (g_input_log) PLOG("input", "inputlog switch: every raw button change and nub move goes to the log");
        PLOG("input", "sceCtrl analog; cross A, square B, triangle C-up, circle C-right, L Z, R R, nub stick (deadzone 0.15, curve 1.5); "
             "SELECT + triangle overlay, SELECT + circle present");
    }

    // "[+delay] <BUTTON[+BUTTON...]> [hold secs]": PSP buttons (CROSS, SQUARE, TRIANGLE, CIRCLE, L, R, START, SELECT, UP, ...)
    bool add_pad_line(const std::string& line, std::string& err) {
        std::stringstream ss(line);
        std::string first, what;
        if (!(ss >> first)) { err = "pad: pad [+secs] <BUTTON[+BUTTON]> [hold secs]\n"; return false; }
        double delay = 0;
        if (first[0] == '+') {
            delay = atof(first.c_str() + 1);
            if (!(ss >> what)) { err = "pad: no buttons\n"; return false; }
        } else {
            what = first;
        }
        unsigned b = 0;
        std::stringstream parts(what);
        std::string n;
        while (std::getline(parts, n, '+')) {
            unsigned m = pad_mask(n);
            if (!m) { err = "pad: unknown button " + n + "\n"; return false; }
            b |= m;
        }
        double hold = 0.15;
        ss >> hold;
        uint64_t now = pspsys::now_us();
        PadEvent e{ now + (uint64_t)(delay * 1e6), now + (uint64_t)((delay + hold) * 1e6), b };
        pspsys::Lock l(*g_pad_mutex);
        g_pad.push_back(e);
        return true;
    }

    bool get_input(int controller, uint16_t* buttons, float* x, float* y) {
        if (controller != 0) return false;
        uint16_t b = 0;
        float sx = 0, sy = 0;
        script_input(0, &b, &sx, &sy);

        SceCtrlData pad;
        memset(&pad, 0, sizeof pad);
        pad.Lx = pad.Ly = 128;
        if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) pad.Buttons = 0;
        unsigned p = pad.Buttons | synthetic(pspsys::now_us());
        if (g_input_log) {
            static unsigned last_raw = 0;
            static int last_lx = -1, last_ly = -1;
            bool nub_moved = std::abs((int)pad.Lx - last_lx) > 24 || std::abs((int)pad.Ly - last_ly) > 24;
            if (pad.Buttons != last_raw || nub_moved) {
                PLOG("input", "raw buttons 0x%06X (was 0x%06X) nub %d,%d", pad.Buttons, last_raw, pad.Lx, pad.Ly);
                last_raw = pad.Buttons;
                last_lx = pad.Lx;
                last_ly = pad.Ly;
            }
        }
        unsigned pressed = p & ~g_prev;
        bool select_before = (g_prev & PSP_CTRL_SELECT) != 0;
        g_prev = p;
        if (p & PSP_CTRL_SELECT) {
            // only presses made while SELECT was already held count; nothing reaches the game
            if (select_before && (pressed & ~PSP_CTRL_SELECT)) hotkey(pressed & ~PSP_CTRL_SELECT);
        } else {
            if (p & PSP_CTRL_CROSS) b |= N64_A;
            if (p & PSP_CTRL_SQUARE) b |= N64_B;
            if (p & PSP_CTRL_TRIANGLE) b |= N64_CUP;
            if (p & PSP_CTRL_CIRCLE) b |= N64_CRIGHT;
            if (p & PSP_CTRL_LTRIGGER) b |= N64_Z;
            if (p & PSP_CTRL_RTRIGGER) b |= N64_R;
            if (p & PSP_CTRL_START) b |= N64_START;
            if (p & PSP_CTRL_UP) b |= N64_UP;
            if (p & PSP_CTRL_DOWN) b |= N64_DOWN;
            if (p & PSP_CTRL_LEFT) b |= N64_LEFT;
            if (p & PSP_CTRL_RIGHT) b |= N64_RIGHT;
            if (sx == 0 && sy == 0) {
                float nx = ((int)pad.Lx - 128) / 127.0f, ny = -((int)pad.Ly - 128) / 127.0f;
                float mag = std::sqrt(nx * nx + ny * ny);
                constexpr float dead = 0.15f;
                if (mag > dead) {
                    float m = std::fmin(1.0f, (mag - dead) / (1.0f - dead));
                    m = std::pow(m, 1.5f);   // radial deadzone, then the curve on the magnitude
                    sx = nx / mag * m;
                    sy = ny / mag * m;
                }
            }
        }
        if (g_input_log) {
            static uint16_t last_b = 0;
            static int last_sy = 0;
            int qy = (int)(sy * 4);
            if (b != last_b || qy != last_sy) {
                PLOG("input", "  -> N64 buttons 0x%04X stick %.2f,%.2f", b, sx, sy);
                last_b = b;
                last_sy = qy;
            }
        }
        *buttons = b;
        *x = sx;
        *y = sy;
        return true;
    }
}
