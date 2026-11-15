#include <algorithm>
// Skin shades beyond the game's two (build/analysis/skin_tones.md has the research).
//
// The game has two sets of skin textures, each an arm and a face/neck CI4 texture with its own 16-colour
// palette: light (arm_1a, facea; record skin == 1) and dark (arm_1b, faceb; anything else, which the packs
// call 0, Darker). Its player renderer (main func_8027139C) loads both sets into TMEM once per pass (tiles
// 0/1 light, 2/3 dark, palettes 0-3) and draws each player's arm and face with G_TEXTURE tile 0/1 or 2/3.
//
// Six shades, light to dark: 5 Lighter, 1 Light, 2 Light-medium, 3 Medium, 4 Medium-dark, 0 Darker. Values
// 2-5 are drawn with one of the game's sets' pixels and palettes of their own, made here from the palettes
// the game loaded (so from the player's own ROM; no art ships). Each shade has a target arm colour as
// rendered at team select (the game's light set renders #915b36 there and the dark set #633a2b). The ratio
// of the shade's aim colour to its base set's rendered colour gives each channel's change, applied to the palette as a multiply when darkening
// and as a screen (1 - (1 - c)(1 - a), which keeps highlights from clipping) when lightening, sized so the
// arm's average texel moves by that ratio. Before such a player's G_TEXTURE the skin pass gets the shade's
// two palettes in TMEM palettes 4/5 and tiles 4/5 (otherwise unused) pointing at the base set's pixels with
// those palettes; the arm then draws with tile 4 and the face with tile 5. Light and Darker players are
// untouched. BLITZ_SKIN_AIMS="5=e2d6df,2=855134,..." overrides the aims for tuning.
//
// A knocked-off helmet shows an RGBA16 bare head (hed_sal light, hed_rob dark); the other shades get a
// tinted copy of their base set's head.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "recomp.h"

namespace {
    constexpr int32_t params_table = (int32_t)0x802EB2E0;  // render parameters, one 0x44 entry per player
    constexpr int32_t params_stride = 0x44, max_players = 16;

    // The game's skin textures in RDRAM (loaded by main func_80256850 before a game and at team select).
    struct Set { int32_t arm_px, arm_pal, face_px, face_pal, head; };
    constexpr Set light_set{(int32_t)0x802DB7E0, (int32_t)0x802DBBE0, (int32_t)0x802DBC00, (int32_t)0x802DC000, (int32_t)0x802DC020};
    constexpr Set dark_set{(int32_t)0x802DD020, (int32_t)0x802DD420, (int32_t)0x802DD440, (int32_t)0x802DD840, (int32_t)0x802DD860};
    constexpr int arm_px_bytes = 16 * 32 / 2, head_bytes = 64 * 32 * 2;
    constexpr int32_t seg_head_offset = 0x840;  // the bare head is segment 0xD + 0x840 in the game's commands

    // Ours, in RDRAM the game doesn't use (next to art_dropin's extra TOC at 0x80408000): per tone, the arm
    // and face palettes, and a bare head.
    constexpr int32_t out_palettes = (int32_t)0x8040C000;  // + (tone - 2) * 0x40: arm palette, then face
    constexpr int32_t out_heads = (int32_t)0x8040D000;     // + (tone - 2) * 0x1000, to 0x80411000

    // Shades 2-5: which set's pixels, and the colour the mapping below aims at. Lighting and the median of a
    // dark-to-light gradient don't follow the texels exactly, so each aim was tuned live until the arm's
    // median skin colour at team select (pixels with r > g > b) matched the shade's target (in comments).
    struct Tone { bool dark_base; uint32_t aim; };
    std::array<Tone, 4> tones = {{
        {false, 0x855134},  // 2 Light-medium, target #865333
        {false, 0x774a33},  // 3 Medium, target #7a4a30
        {true, 0x6e3f2e},   // 4 Medium-dark (the dark set's pixels, lightened), target #6e422e
        {false, 0xe2d6df},  // 5 Lighter, target #e3c3a8
    }};
    constexpr uint32_t light_rendered = 0x915b36, dark_rendered = 0x633a2b;  // the game's own two, rendered
    bool mid_tone(int tone) { return tone >= 2 && tone <= 5; }

    std::array<int8_t, max_players> tone_of{};  // per render-parameters entry, from the last skin read

    // Source bytes (host order) the outputs were made from; rebuilt when the game loads other textures.
    std::vector<uint8_t> made_from;
    bool built = false;

    uint8_t* host(uint8_t* rdram, int32_t addr) { return rdram + (uint32_t)(addr - (int32_t)0x80000000); }

    void snapshot(uint8_t* rdram, std::vector<uint8_t>& out) {
        out.clear();
        for (const Set& s : {light_set, dark_set}) {
            // The arm's pixels, palette, face pixels and palette are contiguous; the head follows.
            uint8_t* p = host(rdram, s.arm_px);
            out.insert(out.end(), p, p + (s.head - s.arm_px) + head_bytes);
        }
    }

    struct Rgb { float c[3]; };
    Rgb rgb5551(uint16_t v) { return {{((v >> 11) & 31) / 31.0f, ((v >> 6) & 31) / 31.0f, ((v >> 1) & 31) / 31.0f}}; }
    uint16_t pack5551(const float c[3], uint16_t alpha) {
        auto q = [](float x) { return (uint16_t)std::lround(std::fmin(std::fmax(x, 0.0f), 1.0f) * 31.0f); };
        return (uint16_t)(q(c[0]) << 11 | q(c[1]) << 6 | q(c[2]) << 1 | (alpha & 1));
    }
    // Skin colours only: the chin strap (blue) and anything bluish keep their colour.
    bool skin_like(const Rgb& c) { return c.c[2] <= c.c[0]; }

    // Average skin texel of a set's arm, weighted by how many pixels use each palette entry.
    std::array<float, 3> mean_arm(uint8_t* rdram, const Set& s) {
        double acc[3] = {0, 0, 0};
        long n = 0;
        for (int i = 0; i < arm_px_bytes; i++) {
            uint8_t b = MEM_BU(i, s.arm_px);
            for (int idx : {b >> 4, b & 15}) {
                if (idx == 0) continue;
                Rgb c = rgb5551(MEM_HU(idx * 2, s.arm_pal));
                if (!skin_like(c)) continue;
                for (int k = 0; k < 3; k++) acc[k] += c.c[k];
                n++;
            }
        }
        if (n == 0) return {0.5f, 0.5f, 0.5f};
        return {(float)(acc[0] / n), (float)(acc[1] / n), (float)(acc[2] / n)};
    }

    void read_aims() {
        static bool done = false;
        if (done) return;
        done = true;
        const char* env = getenv("BLITZ_SKIN_AIMS");
        for (const char* p = env; p != nullptr && *p != 0;) {
            int value = 0;
            unsigned rgb = 0;
            if (sscanf(p, "%d=%6x", &value, &rgb) == 2 && mid_tone(value)) tones[value - 2].aim = rgb;
            p = strchr(p, ',');
            if (p != nullptr) p++;
        }
    }

    void build(uint8_t* rdram) {
        read_aims();
        for (size_t k = 0; k < tones.size(); k++) {
            const Tone& tone = tones[k];
            const Set& base = tone.dark_base ? dark_set : light_set;
            uint32_t from = tone.dark_base ? dark_rendered : light_rendered;
            auto mean = mean_arm(rdram, base);
            // Per channel: a multiply (darker) or a screen amount (lighter) that moves the mean texel by
            // aim / rendered.
            float mul[3], screen[3];
            for (int c = 0; c < 3; c++) {
                int sh = 16 - 8 * c;
                float f = (float)((tone.aim >> sh) & 0xFF) / std::fmax(1.0f, (float)((from >> sh) & 0xFF));
                mul[c] = 1.0f;
                screen[c] = 0.0f;
                if (f <= 1.0f) mul[c] = f;
                else screen[c] = std::fmax(0.0f, 1.0f - (1.0f - std::fmin(0.98f, mean[c] * f)) / std::fmax(0.02f, 1.0f - mean[c]));
            }
            float mean_luma = 0.299f * mean[0] + 0.587f * mean[1] + 0.114f * mean[2];
            // keep_dark: the bare head's hair and shadows (darker than the average skin) lighten less.
            auto tint_px = [&](uint16_t v, bool keep_dark) -> uint16_t {
                Rgb c = rgb5551(v);
                if (!skin_like(c)) return v;
                float w = 1.0f;
                if (keep_dark) {
                    float luma = 0.299f * c.c[0] + 0.587f * c.c[1] + 0.114f * c.c[2];
                    w = std::fmin(1.0f, std::fmax(0.0f, (luma / std::fmax(0.01f, mean_luma) - 0.5f) * 2.0f));
                }
                float out[3];
                for (int i = 0; i < 3; i++) {
                    float t = c.c[i] * mul[i];
                    out[i] = t + ((1.0f - (1.0f - t) * (1.0f - screen[i])) - t) * w;
                }
                return pack5551(out, v);
            };
            auto tint = [&](uint16_t v) { return tint_px(v, false); };
            int32_t pal_out = out_palettes + (int32_t)k * 0x40;
            for (int i = 0; i < 16; i++) {
                // Entry 0 is the transparent/unused one; keep it.
                MEM_HU(i * 2, pal_out) = i == 0 ? MEM_HU(0, base.arm_pal) : tint(MEM_HU(i * 2, base.arm_pal));
                MEM_HU(0x20 + i * 2, pal_out) = i == 0 ? MEM_HU(0, base.face_pal) : tint(MEM_HU(i * 2, base.face_pal));
            }
            int32_t head_out = out_heads + (int32_t)k * 0x1000;
            for (int i = 0; i < head_bytes; i += 2) {
                MEM_HU(i, head_out) = tint_px(MEM_HU(i, base.head), true);
            }
        }
        fprintf(stderr, "[skin] shades made from the game's skin palettes\n");
    }

    void ensure_built(uint8_t* rdram) {
        static std::vector<uint8_t> now;
        snapshot(rdram, now);
        // A menu step (Back) or a state restore rewrites RDRAM: our palettes can roll back to zeros while this file's
        // notes survive, and those players then draw black.
        const uint8_t* out = host(rdram, out_palettes);
        const bool outputs_there = std::any_of(out, out + 0x40, [](uint8_t b) { return b != 0; });
        if (built && outputs_there && now == made_from) return;
        build(rdram);
        made_from = now;
        built = true;
    }

    int entry_slot(int32_t entry) {
        int32_t off = entry - params_table;
        if (off < 0 || off % params_stride != 0 || off / params_stride >= max_players) return -1;
        return off / params_stride;
    }
}

// The render parameters' skin read (func_80255180 game players, func_80255718 the team-select QB): note the
// shade for the skin pass and give the game its base set (1 light; anything else is the dark set).
extern "C" int32_t blitz_skin_param(uint8_t* rdram, int32_t entry, int32_t raw) {
    int slot = entry_slot(entry);
    int tone = raw >= 0 && raw <= 5 ? raw : 0;
    if (slot >= 0) tone_of[slot] = (int8_t)tone;
    if (!mid_tone(tone)) return raw;
    return tones[tone - 2].dark_base ? 0 : 1;
}

// Before the skin pass's G_TEXTURE (func_8027139C 0x80271E24): s0 = the entry, a1 = the display-list cursor,
// v1 = the tile pair (0 or 2).
extern "C" void blitz_skin_pass(uint8_t* rdram, recomp_context* ctx) {
    int slot = entry_slot((int32_t)ctx->r16);
    if (slot < 0 || !mid_tone(tone_of[slot])) return;
    int k = tone_of[slot] - 2;
    ensure_built(rdram);
    bool dark = tones[k].dark_base;
    uint32_t pal = (uint32_t)(out_palettes + k * 0x40);

    // The game's own commands for its tiles 0-3, re-aimed at tiles 4/5 and palettes 4/5 (TMEM 0x140/0x150).
    const uint32_t cmds[][2] = {
        {0xFD100000, pal},               {0xE8000000, 0}, {0xF5000140, 0x07000000}, {0xE6000000, 0}, {0xF0000000, 0x0703C000}, {0xE7000000, 0},
        {0xFD100000, pal + 0x20},        {0xE8000000, 0}, {0xF5000150, 0x07000000}, {0xE6000000, 0}, {0xF0000000, 0x0703C000}, {0xE7000000, 0},
        {dark ? 0xF5400260u : 0xF5400200u, 0x04494240}, {0xF2000000, 0x0403C07C},  // arm: CI4, palette 4
        {dark ? 0xF5400280u : 0xF5400220u, 0x05598140}, {0xF2000000, 0x0503C0FC},  // face: CI4, palette 5
    };
    int32_t cursor = (int32_t)ctx->r5;
    for (const auto& c : cmds) {
        MEM_W(0, cursor) = (int32_t)c[0];
        MEM_W(4, cursor) = (int32_t)c[1];
        cursor += 8;
    }
    ctx->r5 = (gpr)cursor;
    ctx->r3 = 4;  // G_TEXTURE tile 4 (arm), then 4 | 1 (face)
}

// The bare head (func_8027139C 0x80272904): t1 = segment 0xD, whose +0x840 is the RGBA16 head; s0 = the entry.
extern "C" void blitz_skin_bare_head(uint8_t* rdram, recomp_context* ctx) {
    int slot = entry_slot((int32_t)ctx->r16);
    if (slot < 0 || !mid_tone(tone_of[slot])) return;
    ensure_built(rdram);
    ctx->r9 = (gpr)(int32_t)(out_heads + (tone_of[slot] - 2) * 0x1000 - seg_head_offset);
}
