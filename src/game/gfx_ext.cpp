// RT64 extended display-list commands for widescreen (M4). Everything here is emitted from host
// hooks into the game's own display lists; no game code is rewritten.
//
// Frame prologue (rsp_task_start hook): each frame's display list starts with `DB060000 00000000`
// (segment 0 = 0, from gfx_build_frame). That word pair is replaced with gEXEnable: RT64 zero-fills
// segments at RSP reset and Blitz only ever sets segment 0 to 0, so nothing is lost. Extended state
// resets at every G_RDPFULLSYNC, so this is needed every frame.
//
// Sprites (sprite_draw hooks): every 2D element goes through sprite_draw -> sprite_texrect. Just
// before sprite_texrect emits its rectangle, we may emit:
//   - gEXSetRectAspect(ADJUST) for full-screen artwork (512 wide), so RT64 keeps it 4:3 instead of
//     stretching it across a widened frame (the colour clear in widescreen.cpp fills the sides);
//   - gEXSetRectAlign for the gameplay HUD: the scoreboard anchored left, the four player bars spread
//     proportionally. The HUD placement setting scales this (Original keeps the 4:3 layout).
// State is tracked per frame so commands are only emitted on change.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "recomp.h"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

extern "C" int blitz_tall_2d(void);  // tall_view.cpp

namespace {
    constexpr uint32_t gameplay_camera = 0x80318CB0;  // ovl_a camera callback while a game is on
    constexpr int32_t camera_callback = (int32_t)0x8002B6A0;

    // Sprite list node and object fields (see NOTES "Enhancements (M4)").
    constexpr int node_obj = 0x08, node_group = 0x14;
    constexpr int obj_image = 0x0C, obj_x = 0x24, obj_scale_x = 0x54, obj_anchor_x = 0xA8;
    constexpr int image_width = 0x2C;

    constexpr uint32_t group_scoreboard_plate = 0x702, group_player_bars = 0x708;

    int32_t cur_node = 0;
    // Widescreen edge mirroring (blitz_gfx_prologue): display-list ranges of 2D art drawn at the 4:3
    // edges this frame, recorded around sprite_texrect.
    std::vector<std::pair<int32_t, int32_t>> mirror_ranges;
    int32_t mirror_gp = 0, mirror_start = 0;
    std::atomic<uint32_t> gfx_frames{0}, play_select_draws{0}, play_select_frame{0};
    uint16_t cur_origin = G_EX_ORIGIN_NONE;
    uint8_t cur_aspect = G_EX_ASPECT_AUTO;
    bool hud_scissor_widened = false;

    float read_float(uint8_t* rdram, int32_t addr) {
        return std::bit_cast<float>((uint32_t)MEM_W(0, addr));
    }

    // Append commands at the game's display-list cursor (gp points at it) and advance the cursor.
    void emit(uint8_t* rdram, int32_t gp, const GfxCommand* cmds, int count) {
        int32_t cursor = MEM_W(0, gp);
        for (int i = 0; i < count; i++) {
            MEM_W(i * 8 + 0, cursor) = cmds[i].values.word0;
            MEM_W(i * 8 + 4, cursor) = cmds[i].values.word1;
        }
        MEM_W(0, gp) = cursor + count * 8;
    }

    struct SpriteInfo {
        uint32_t group;
        float x;          // object position in the game's 512-wide space
        float left, right;  // screen extent
    };

    SpriteInfo sprite_info(uint8_t* rdram, int32_t node) {
        SpriteInfo info{};
        info.group = (uint32_t)MEM_W(node_group, node);
        int32_t obj = MEM_W(node_obj, node);
        info.x = read_float(rdram, obj + obj_x);
        float scale = read_float(rdram, obj + obj_scale_x);
        int16_t anchor = MEM_H(obj_anchor_x, obj);
        int32_t image = MEM_W(obj_image, obj);
        float width = image != 0 ? (float)(uint32_t)MEM_W(image_width, image) : 0.0f;
        info.left = info.x - anchor * scale;
        info.right = info.left + width * scale;
        return info;
    }

    // Widescreen menus: the game's abstract backdrops are stretched across the widened screen (the
    // swooshes behind every menu, team select's rings), drawn whole or in left/right halves. Other
    // full-screen art (title, play select, VS screen, New Features, stats, trivia, attract pages) keeps
    // its shape (ADJUST, pillarboxed). Play select's header (PLYTOP.WMS) was stretched too, but its
    // grid stays 4:3, so the header's art stopped in a line a third of the way down the sides (team,
    // 2026-10-29: all the way down, or 4:3); it's pillarboxed with the grid. BLITZ_WS_BACKDROPS=0
    // turns it off.
    constexpr int image_name = 0x0E;  // file name, e.g. "MENUBG.WMS"
    constexpr const char* stretch_backdrops[] = {"MENUBG.WMS", "TEAMSEL.WMS"};
    // Art mirrored past the 4:3 edges (mirror_edges). None at the moment: the title (BLITZTIT.WMS),
    // the VS screen (VSBG.WMS) and play select's grid ("mem") were tried and the team preferred bars
    // (repeated play boxes, rough arcs); the machinery stays for art that suits it.
    constexpr std::array<const char*, 0> mirror_backdrops{};

    bool image_is(uint8_t* rdram, int32_t img, const char* want) {
        if (want == nullptr || want[0] == 0) return false;  // never matches a nameless image
        for (int i = 0; i < 16; i++) {
            char c = (char)std::toupper((unsigned char)MEM_B(image_name + i, img));
            if (c != want[i]) return false;
            if (c == 0) return true;
        }
        return false;
    }

    bool stretch_backdrop(uint8_t* rdram, int32_t node, const SpriteInfo& s) {
        static const bool on = !getenv("BLITZ_WS_BACKDROPS") || getenv("BLITZ_WS_BACKDROPS")[0] != '0';
        if (!on) return false;
        auto near = [](float a, float b) { return std::fabs(a - b) < 1.0f; };
        bool piece = (near(s.left, 0) || near(s.left, 256)) && (near(s.right, 256) || near(s.right, 512));
        if (!piece) return false;
        int32_t img = MEM_W(obj_image, MEM_W(node_obj, node));
        if (img == 0) return false;
        for (const char* n : stretch_backdrops) if (image_is(rdram, img, n)) return true;
        return false;
    }

    uint16_t hud_origin(uint8_t* rdram, const SpriteInfo& s) {
        if ((uint32_t)MEM_W(0, camera_callback) != gameplay_camera) {
            return G_EX_ORIGIN_NONE;  // menus keep their 4:3 layout
        }
        if (blitz_tall_2d()) return G_EX_ORIGIN_NONE;  // tall view fits the HUD to a screen itself (tall_view.cpp)
        switch (s.group) {
            case group_scoreboard_plate:
            case 4:
            case 5:
            case 6:
                return G_EX_ORIGIN_LEFT;  // scoreboard plate and its text
            case group_player_bars:
                // Spread the bar row: each element keeps its distance from a point that sits at the
                // same fraction of the widened screen as the element does of the 4:3 one.
                return (uint16_t)std::clamp(std::lround(s.x * 2.0f), 0L, (long)G_EX_ORIGIN_RIGHT);
            default:
                return G_EX_ORIGIN_NONE;
        }
    }
}

// ---- Interpolation tags (docs/ENHANCEMENTS.md item 9) ----
// RT64 draws Blitz's 30 Hz frames at the display rate by interpolating matching matrices. Untagged, a
// camera cut is smeared across one displayed frame, and identical players can be matched to each
// other in a pileup. The camera gets an id that changes on every cut (eye or look-at jumping more than
// 50 units, the view turning more than 20 degrees, or another camera taking over), so RT64 snaps
// instead; each player's matrices get an id per player slot. Measured: normal camera motion stays
// under ~23 units a frame, cuts are 105+ (build/analysis/camcut). BLITZ_NO_INTERP_TAGS=1 turns it off;
// BLITZ_INTERP_LOG=1 logs each cut.
namespace {
#ifndef G_MTX_PROJECTION
    constexpr uint8_t G_MTX_PROJECTION = 0x04, G_MTX_MODELVIEW = 0x00;
#endif
    constexpr int32_t player_records = (int32_t)0x802EB2E0, player_record_size = 0x44;
    constexpr int32_t player_count = (int32_t)0x80283BE4;
    constexpr int32_t play_cursor = (int32_t)0x8005EA78;  // display-list cursor while 3D draws

    const bool tags_on = getenv("BLITZ_NO_INTERP_TAGS") == nullptr;
    uint32_t epoch = 0;
    int camera_calls = 0;
    bool have_last = false;
    float last_eye[3], last_at[3];
    uint32_t last_callback = 0;

    void group(uint8_t* rdram, int32_t gp, uint32_t id, bool projection) {
        GfxCommand cmd[2];
        if (projection) {
            gEXMatrixGroup(cmd, id, G_EX_INTERPOLATE_SIMPLE, G_EX_NOPUSH, G_MTX_PROJECTION,
                           G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE,
                           G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_INTERPOLATE, G_EX_COMPONENT_SKIP,
                           G_EX_COMPONENT_SKIP, G_EX_ORDER_LINEAR, G_EX_EDIT_NONE, G_EX_ASPECT_AUTO,
                           G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP);
        }
        else {
            gEXMatrixGroup(cmd, id, G_EX_INTERPOLATE_DECOMPOSE, G_EX_NOPUSH, G_MTX_MODELVIEW,
                           G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO,
                           G_EX_COMPONENT_AUTO, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO, G_EX_ORDER_AUTO,
                           G_EX_EDIT_NONE, G_EX_ASPECT_AUTO, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_AUTO);
        }
        emit(rdram, gp, cmd, 2);
    }

    uint32_t scene_id() { return 0xB3000000u | (epoch & 0xFFFFFF); }
}

// gfx_load_camera (0x8000FABC), after the projection and look-at loads: gp = the display-list cursor,
// cam = the camera {eye[3], at[3], up[3]} on its stack. Runs 2-3 times a frame with the same camera.
extern "C" void blitz_camera_tag(uint8_t* rdram, int32_t gp, int32_t cam) {
    if (!tags_on) return;
    if (camera_calls++ == 0) {
        float eye[3], at[3];
        for (int i = 0; i < 3; i++) {
            eye[i] = read_float(rdram, cam + 4 * i);
            at[i] = read_float(rdram, cam + 12 + 4 * i);
        }
        uint32_t callback = (uint32_t)MEM_W(0, camera_callback);
        bool cut = !have_last || callback != last_callback;
        if (!cut) {
            float de = 0, da = 0, d0[3], d1[3], n0 = 0, n1 = 0, dot = 0;
            for (int i = 0; i < 3; i++) {
                de += (eye[i] - last_eye[i]) * (eye[i] - last_eye[i]);
                da += (at[i] - last_at[i]) * (at[i] - last_at[i]);
                d0[i] = last_at[i] - last_eye[i];
                d1[i] = at[i] - eye[i];
                n0 += d0[i] * d0[i];
                n1 += d1[i] * d1[i];
                dot += d0[i] * d1[i];
            }
            float cosang = (n0 > 0 && n1 > 0) ? dot / std::sqrt(n0 * n1) : 1.0f;
            cut = de > 50.0f * 50.0f || da > 50.0f * 50.0f || cosang < 0.9397f;  // cos 20 degrees
        }
        if (cut) {
            epoch++;
            if (getenv("BLITZ_INTERP_LOG")) fprintf(stderr, "[interp] camera cut %u at %08X\n", epoch, gp);
        }
        std::copy(eye, eye + 3, last_eye);
        std::copy(at, at + 3, last_at);
        last_callback = callback;
        have_last = true;
    }
    group(rdram, gp, 0xB1000000u | ((epoch & 0xFFF) << 4) | (uint32_t)std::min(camera_calls - 1, 15), true);
    group(rdram, gp, scene_id(), false);  // everything that isn't a player
}

// func_8027139C / func_80273084, before each player's draw pass (s0 = the player's record).
extern "C" void blitz_player_tag(uint8_t* rdram, int32_t record) {
    if (!tags_on) return;
    int32_t slot = (record - player_records) / player_record_size;
    if (slot < 0 || slot >= 16 || slot >= MEM_W(0, player_count) || (record - player_records) % player_record_size != 0) return;
    group(rdram, play_cursor, 0xB2000000u | ((epoch & 0xFFFF) << 4) | (uint32_t)slot, false);
}

// After the player passes: back to the scene's group.
extern "C" void blitz_player_tags_end(uint8_t* rdram) {
    if (!tags_on) return;
    group(rdram, play_cursor, scene_id(), false);
}

extern "C" void blitz_gfx_frame_begin(void) {
    camera_calls = 0;
    cur_origin = G_EX_ORIGIN_NONE;
    cur_aspect = G_EX_ASPECT_AUTO;
    hud_scissor_widened = false;
}

// The foreground sprite pass sets the game's own scissor (sprites_draw_fg 0x80013E54), which drops
// any widened HUD scissor from the background pass; the next anchored sprite must widen it again.
extern "C" void blitz_gfx_scissor_reset(void) {
    hud_scissor_widened = false;
}

extern "C" void blitz_sprite_node(uint8_t* rdram, int32_t node) {
    cur_node = node;
}

extern "C" void blitz_sprite_before_texrect(uint8_t* rdram, int32_t gp) {
    if (cur_node == 0) {
        return;
    }
    SpriteInfo s = sprite_info(rdram, cur_node);
    GfxCommand cmds[6];
    int n = 0;

    uint8_t aspect = stretch_backdrop(rdram, cur_node, s) ? G_EX_ASPECT_STRETCH
                     : (s.left <= 1.0f && s.right >= 511.0f) ? G_EX_ASPECT_ADJUST  // +-1: backdrops mid-slide
                     : G_EX_ASPECT_AUTO;
    if (aspect != cur_aspect) {
        gEXSetRectAspect(&cmds[n], aspect);
        n += 1;
        cur_aspect = aspect;
    }

    uint16_t origin = hud_origin(rdram, s);
    if (origin != cur_origin && origin == G_EX_ORIGIN_NONE && hud_scissor_widened) {
        // Back to unanchored sprites (e.g. banners after the HUD): restore the game's own 4:3 clip,
        // ED000000 007FC3FC, so they don't spill into the widescreen sides.
        gEXSetScissor(&cmds[n], 0, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 511, 255);
        n += 2;
        hud_scissor_widened = false;
    }
    if (origin != cur_origin) {
        if (origin != G_EX_ORIGIN_NONE && !hud_scissor_widened) {
            // A rect's scissor is narrowed with it; widen the HUD scissor (same 0..511 in game
            // coordinates, but anchored to both edges) so anchored sprites aren't clipped.
            gEXSetScissor(&cmds[n], 0, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, -1, 255);
            n += 2;
            hud_scissor_widened = true;
        }
        // RDP::movedFromOrigin adds origin * 512 * 4 / 0x400 (10.2 fixed point); cancel it so the
        // sprite keeps its game position relative to the origin point.
        int16_t offset = origin == G_EX_ORIGIN_NONE ? 0 : (int16_t)(-2 * origin);
        gEXSetRectAlign(&cmds[n], origin, origin, offset, 0, offset, 0);
        n += 2;
        cur_origin = origin;
    }

    if (n > 0) {
        emit(rdram, gp, cmds, n);
    }
    // Listed art (mirror_backdrops) touching a 4:3 edge: its commands are noted, and the frame's
    // prologue adds mirrored copies past the edges.
    mirror_start = 0;
    bool gameplay = (uint32_t)MEM_W(0, camera_callback) == gameplay_camera;
    bool edge = std::fabs(s.left) < 1.0f || std::fabs(s.right - 512.0f) < 1.0f;
    static const bool log_all = getenv("BLITZ_WS_LOGALL") != nullptr;  // every sprite image, once
    if (log_all) {
        int32_t img = MEM_W(obj_image, MEM_W(node_obj, cur_node));
        char name[17] = {};
        for (int i = 0; img && i < 16; i++) name[i] = (char)MEM_B(image_name + i, img);
        static std::vector<std::string> seen;
        if (std::find(seen.begin(), seen.end(), name) == seen.end() && seen.size() < 4000) {
            seen.push_back(name);
            fprintf(stderr, "[ws] sprite %s %.0f-%.0f\n", name, s.left, s.right);
        }
    }
    static const bool log = getenv("BLITZ_WS_LOG") != nullptr;  // lists edge art, to find more to add
    if (log && edge && s.right - s.left >= 64.0f) {
        int32_t img = MEM_W(obj_image, MEM_W(node_obj, cur_node));
        char name[17] = {};
        for (int i = 0; img && i < 16; i++) name[i] = (char)MEM_B(image_name + i, img);
        static std::string last;
        if (last != name) {
            last = name;
            fprintf(stderr, "[ws] edge art %s %.0f-%.0f%s\n", name, s.left, s.right, gameplay ? " (gameplay camera)" : "");
        }
    }
    int32_t img = MEM_W(obj_image, MEM_W(node_obj, cur_node));
    if (img != 0 && image_is(rdram, img, "PLYTOP.WMS")) {  // play select's header (Redo Play, savestate.cpp)
        play_select_draws++;
        play_select_frame = gfx_frames.load();
    }
    if (!mirror_backdrops.empty() && edge && s.right - s.left >= 128.0f && aspect != G_EX_ASPECT_STRETCH && mirror_ranges.size() < 32) {
        bool listed = false;
        for (const char* n : mirror_backdrops) listed = listed || (img != 0 && image_is(rdram, img, n));
        if (listed) {
            mirror_gp = gp;
            mirror_start = MEM_W(0, gp);
        }
    }
}

// Art as wide as the picture, this sprite (tall_view.cpp, before it's scaled): a screen, not play (halftime, stats).
extern "C" int blitz_sprite_full_width(uint8_t* rdram, int32_t node) {
    SpriteInfo s = sprite_info(rdram, node);
    int32_t obj = MEM_W(node_obj, node);
    int32_t image = MEM_W(obj_image, obj);
    if (image == 0 || s.left > 1.0f || s.right < 511.0f) return 0;
    // At its own size, and as tall as a screen's art: a call growing past the edges (TURNOVER ON DOWNS, IT'S GOOD) is wide
    // for a moment and is still play.
    float scale_x = read_float(rdram, obj + obj_scale_x), scale_y = read_float(rdram, obj + obj_scale_x + 4);
    if (scale_x < 0.95f || scale_x > 1.05f || scale_y < 0.95f || scale_y > 1.05f) return 0;
    return (float)(uint32_t)MEM_W(image_width + 4, image) >= 200.0f;
}

extern "C" uint32_t blitz_play_select_draws(void) { return play_select_draws.load(); }
extern "C" int blitz_play_select_on_screen(void) { return play_select_draws.load() != 0 && gfx_frames.load() - play_select_frame.load() < 4; }

// sprite_draw, after sprite_texrect returns (0x80013DB0): the end of a noted sprite's commands.
extern "C" void blitz_sprite_after_texrect(uint8_t* rdram) {
    if (mirror_start == 0) return;
    mirror_ranges.emplace_back(mirror_start, MEM_W(0, mirror_gp));
    mirror_start = 0;
}

// Called at rsp_task_start for every task; graphics tasks get gEXEnable as their first command.
namespace {
    constexpr uint32_t frame_list_capacity = 0x5DC0;  // gfx_build_frame overflows past this (NOTES.md 14)

    // Moves each noted sprite's commands into the unused tail of the frame's own list (called from the
    // sprite's old place with G_DL), adding after every strip that touches a 4:3 edge a mirrored copy
    // past it: an extended texrect (signed coordinates) reading the strip backwards from the edge.
    // The tail lives exactly as long as the frame's list (lists above 4 MB were read as zeros by RT64).
    void mirror_edges(uint8_t* rdram, int32_t dl, uint32_t used) {
        if (used == 0 || used > frame_list_capacity) return;
        int32_t out = dl + (int32_t)((used + 7) & ~7u), out_end = dl + (int32_t)frame_list_capacity;
        auto w0 = [&](int32_t a) { return (uint32_t)MEM_W(0, a); };
        auto w1 = [&](int32_t a) { return (uint32_t)MEM_W(4, a); };
        for (auto [start, end] : mirror_ranges) {
            if (start < dl || end > dl + (int32_t)used || end - start < 24) continue;
            int strips = 0;
            for (int32_t a = start; a + 24 <= end; a += 8) {
                if ((w0(a) >> 24) == 0xE4 && (w0(a + 8) >> 24) == 0xE1 && (w0(a + 16) >> 24) == 0xF1) strips++;
            }
            if (strips == 0 || out + (end - start) + strips * 48 + 48 > out_end) continue;
            std::vector<GfxCommand> list;
            GfxCommand c[3];
            gEXSetScissor(c, 0, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, -1, 255);
            list.insert(list.end(), c, c + 2);
            for (int32_t a = start; a < end; a += 8) {
                GfxCommand cmd;
                cmd.values = {w0(a), w1(a)};
                list.push_back(cmd);
                if (a >= start + 16 && (w0(a - 16) >> 24) == 0xE4 && (w0(a - 8) >> 24) == 0xE1 && (w0(a) >> 24) == 0xF1) {
                    int32_t r = a - 16;
                    int16_t lrx = (int16_t)((w0(r) >> 12) & 0xFFF), lry = (int16_t)(w0(r) & 0xFFF);
                    int16_t ulx = (int16_t)((w1(r) >> 12) & 0xFFF), uly = (int16_t)(w1(r) & 0xFFF);
                    uint32_t tile = (w1(r) >> 24) & 7;
                    int16_t s = (int16_t)(w1(r + 8) >> 16), t = (int16_t)(w1(r + 8) & 0xFFFF);
                    int16_t dsdx = (int16_t)(w1(r + 16) >> 16), dtdy = (int16_t)(w1(r + 16) & 0xFFFF);
                    int w = std::min((lrx - ulx) / 4, 256);
                    if (w <= 0) continue;
                    // Mirror about the edge: the texel next to it comes first, then away from it.
                    int16_t s_near_left = s;  // the strip's first texel is at its left edge
                    if (ulx <= 4) {  // touches the left edge: a copy over [-w, 0)
                        gEXTextureRectangle(c, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, (int16_t)(ulx - w * 4), uly, ulx, lry, tile,
                                            (int16_t)(s_near_left + (((w - 1) * (int32_t)dsdx) >> 5)), t, (int16_t)-dsdx, dtdy);
                        list.insert(list.end(), c, c + 3);
                    }
                    if (lrx >= 2044 && w >= 2) {
                        // Touches the right edge: the art's last column is black, so the copy starts over it,
                        // a pixel early, from the column before.
                        int16_t s_back = (int16_t)(s + (((w - 2) * (int32_t)dsdx) >> 5));
                        gEXTextureRectangle(c, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, (int16_t)(lrx - 4), uly,
                                            (int16_t)(lrx - 4 + (w - 1) * 4), lry, tile, s_back, t, (int16_t)-dsdx, dtdy);
                        list.insert(list.end(), c, c + 3);
                    }
                }
            }
            gEXSetScissor(c, 0, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 511, 255);
            list.insert(list.end(), c, c + 2);
            GfxCommand enddl;
            enddl.values = {0xDF000000u, 0};
            list.push_back(enddl);
            if (out + (int32_t)list.size() * 8 > out_end) continue;
            for (size_t k = 0; k < list.size(); k++) {
                MEM_W(8 * (int32_t)k, out) = list[k].values.word0;
                MEM_W(8 * (int32_t)k + 4, out) = list[k].values.word1;
            }
            MEM_W(0, start) = (int32_t)0xDE000000;  // the sprite's place: call the copy, then no-ops
            MEM_W(4, start) = out;
            for (int32_t a = start + 8; a < end; a += 8) {
                MEM_W(0, a) = 0;
                MEM_W(4, a) = 0;
            }
            out += (int32_t)list.size() * 8;
        }
    }
}

extern "C" int blitz_force_color_clear(void);  // widescreen.cpp: Aspect Ratio is Expand

extern "C" void blitz_gfx_prologue(uint8_t* rdram, int32_t task) {
    if ((uint32_t)MEM_W(0, task) != 1) {  // M_GFXTASK
        return;
    }
    static const bool mirror_on = !getenv("BLITZ_WS_MIRROR") || getenv("BLITZ_WS_MIRROR")[0] != '0';
    if (mirror_on && blitz_force_color_clear() && !mirror_ranges.empty()) {
        mirror_edges(rdram, MEM_W(0x30, task), (uint32_t)MEM_W(0x34, task));
    }
    mirror_ranges.clear();
    gfx_frames++;
    int32_t dl = MEM_W(0x30, task);
    if ((uint32_t)MEM_W(0, dl) == 0xDB060000 && (uint32_t)MEM_W(4, dl) == 0) {
        GfxCommand enable;
        gEXEnable(&enable);
        MEM_W(0, dl) = enable.values.word0;
        MEM_W(4, dl) = enable.values.word1;
    }
}
