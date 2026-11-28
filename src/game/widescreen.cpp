// Widescreen support for the game side. RT64 widens Blitz's 3D by itself in Aspect Ratio "Expand"
// (platform/rt64-patches/0001). The game never clears its colour buffer in most modes, because its
// own backdrop or 3D scene covers the 4:3 screen every frame; the widened sides of 2D-only screens
// would keep whatever an earlier frame left there. While Expand is on, gfx_build_frame uses the
// game's own clear mode (a full-screen FILLRECT, which RT64 stretches over the widened buffer).
//
// Players at the sides (docs/notes/WIDESCREEN_CULLING.md). Every frame ovl_a 0x802FB6A8 (a process) puts each
// player into the game's own camera (eye 0x803668B0, unit rows 0x80366904) and sets bit 0x2 of his flags (+0x1A4)
// when he's in front, |x| <= z * 0.565 and |y| <= z * 0.373: the 4:3 picture's edges (tan 0.378 and 0.283) with a
// margin. The draw callback (func_80255180) and the shadow (func_80255E58) skip a player without it, so in a wider
// picture players vanish near the left and right sides (on a 19.5:9 phone well inside the picture).
// The bit is gameplay too: an off-screen defender runs 2.5x as fast (ovl_a 0x802F6BA4), after the whistle it picks
// who to go for (0x802FB1DC), and 0x803061A8 rolls rand() only for players on screen. So the game's bit stays
// exactly the 4:3 one; the hooks below work out a second, wider answer on the side and give it to the two draws
// only. With a picture no wider than 4:3 the draws get the game's own flags back untouched.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "recomp.h"
#include "SDL2/SDL.h"
#include "ultramodern/config.hpp"

extern "C" volatile float blitz_tall_aspect_on;   // rt64_workload_queue.cpp (platform/rt64-patches/0008)
extern "C" volatile float blitz_ds_second_ratio;  // rt64_workload_queue.cpp
extern "C" volatile int blitz_ds_mode;            // rt64_present_queue.cpp

extern "C" int blitz_force_color_clear(void) {
    return ultramodern::renderer::get_graphics_config().ar_option == ultramodern::renderer::AspectRatio::Expand;
}

namespace {
    using ultramodern::renderer::AspectRatio;

    // How much wider than 4:3 the 3D picture is (1: not wider), from the UI thread's widescreen_update. Builds that
    // never call it (the PSP) keep 1.
    volatile float wide_now = 1.0f;
    float wide_scale() { return wide_now; }

    // RT64's rule (rt64_workload_queue.cpp, threadConfigurationUpdate): Expand takes the window's shape, or in BLITZ
    // DS's mode 3 the second screen's; modes 2 and 3 keep at least 4:3; the tall view is narrower than 4:3. Manual is
    // RT64's default 16:9.
    float picture_scale(float window_ratio) {
        const AspectRatio ar = ultramodern::renderer::get_graphics_config().ar_option;
        if (blitz_tall_aspect_on != 0.0f) return 1.0f;
        float target = 0.0f;
        if (ar == AspectRatio::Expand) {
            target = window_ratio;
            if (blitz_ds_mode == 3 && blitz_ds_second_ratio > 0.0f) target = blitz_ds_second_ratio;
        }
        else if (ar == AspectRatio::Manual) {
            target = 16.0f / 9.0f;
        }
        if (!std::isfinite(target) || !(target > 4.0f / 3.0f)) return 1.0f;
        return std::min(target * 0.75f, 4.0f);  // 4:1 at most, so a broken size can't draw everything
    }

    // The players (0x802D1590 + i * 0xB98) the cull process walks, *(0x80283BE4) of them.
    constexpr uint32_t players = 0x802D1590, player_size = 0xB98;
    constexpr int max_players = 32;
    constexpr uint32_t on_screen = 0x2;  // flags (+0x1A4)

    struct Cull {
        bool valid;      // the cull process has seen him in this game
        bool pass_wide;  // in front and inside the wide picture's edges (with the game's margins)
        bool hidden;     // the game's second rule hid him (0x802FB918), whatever the camera says
        bool game_on;    // the game's own bit when it was set
        bool wide_on;    // what the draws get
    };
    Cull cull[max_players];

    int index_of(uint32_t player) {
        uint32_t d = player - players;
        if (d % player_size != 0) return -1;
        d /= player_size;
        return d < (uint32_t)max_players ? (int)d : -1;
    }
}

namespace blitz {
    // Every UI tick (main.cpp).
    void widescreen_update(SDL_Window* window) {
        if (window == nullptr) return;
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w > 0 && h > 0) wide_now = picture_scale((float)w / (float)h);
    }
}

// ovl_a 0x802FB6A8, the cull process's entry (once a game; it loops inside): nothing from an earlier game.
extern "C" void blitz_ws_cull_begin(void) {
    for (Cull& c : cull) c = Cull{};
}

// 0x802FB860, the depth test: s3 = the player, f18 / f10 / f8 = his x / y / depth in the game's camera, f2 = the
// game's |x| limit (depth * 0.769 * (120 / 512 + 0.5)), f6 = the |y| limit over the depth. The game's own tests follow
// unchanged.
extern "C" void blitz_ws_cull_test(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    int i = index_of((uint32_t)ctx->r19);
    if (i < 0) return;
    const float x = ctx->f18.fl, y = ctx->f10.fl, z = ctx->f8.fl;
    const float x_limit = ctx->f2.fl * wide_scale(), y_limit = z * ctx->f6.fl;
    cull[i].pass_wide = !(z < 28.0f) && !(std::fabs(x) > x_limit) && !(std::fabs(y) > y_limit);
    cull[i].hidden = false;
}

// 0x802FB918: the game is clearing the bit for a reason of its own (a rule on +0x3BC / +0x3C8 / 0x802EA418), not the
// camera's. The wide answer follows it.
extern "C" void blitz_ws_cull_hidden(recomp_context* ctx) {
    int i = index_of((uint32_t)ctx->r19);
    if (i >= 0) cull[i].hidden = true;
}

// 0x802FB928, where both ways join: s0 = the player + 0x758, so his flags are at s0 - 0x5B4. The bit there is final.
extern "C" void blitz_ws_cull_done(uint8_t* rdram, recomp_context* ctx) {
    int i = index_of((uint32_t)ctx->r19);
    if (i < 0) return;
    const bool on = (MEM_W(-0x5B4, ctx->r16) & on_screen) != 0;
    cull[i].game_on = on;
    cull[i].wide_on = on || (cull[i].pass_wide && !cull[i].hidden);
    cull[i].valid = true;
}

// func_80255180 0x802551B4 (the player's draw) and func_80255E58 0x80255E88 (his shadow): flags = the player's +0x1A4,
// about to be tested for the bit. Returned with the bit set when only the 4:3 edges hid him. Nothing is written back.
extern "C" uint32_t blitz_ws_draw_flags(uint8_t* rdram, int32_t player, uint32_t flags) {
    (void)rdram;
    if ((flags & on_screen) != 0 || wide_scale() <= 1.0f) return flags;
    int i = index_of((uint32_t)player);
    if (i < 0 || !cull[i].valid) return flags;
    // The game had him on screen and something since took him off (a cut scene, a substitution): leave him off.
    if (cull[i].game_on) return flags;
    return cull[i].wide_on ? (flags | on_screen) : flags;
}
