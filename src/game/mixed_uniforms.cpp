// Mixed uniforms (Modern): each player in his own team's jersey, sleeves, pants, socks and helmet. Pro Bowl
// crews plan who wears what (pro_bowl.cpp: mixed_plan); BLITZ_MIXED_UNIFORMS=1 (dev) dresses every player of
// both sides in a different team's home uniform, for an eyeball test.
//
// How the game dresses a side (main func_80256580, the uniform builder): one 0x60E0-byte buffer per side at
// 0x802BE4B0 + side * 0x60E0 holds the helmet (three RGBA16 textures at +0x54E0), pants, shoulders, stripe,
// socks and their palettes (+0x4C00), and a jersey with the player's number for each roster slot (+slot*0x400).
// Its renderer (main func_8027139C) points segment 12 at that buffer and loads the side's palettes and
// textures into TMEM once; each player then loads only his jersey (0x0C000000 + slot << 10), and the helmet pass
// loads the helmet once for all of the side's helmets.
//
// So each player gets a buffer of his own, built at kickoff by the game's own builder with his team's uniform
// ids in the side's game copy (then put back; the game rebuilds the real sides right after). Each frame, just
// before a player's jersey load (0x802723E8) and his helmet (0x802726F8), one G_DL goes into the frame's display
// list: a fixed sub-list that points segment 12 at his buffer and replays the game's own loads (the body's
// palettes, pants, shoulders, stripe and socks; the helmet's three textures). The jersey load the game emits
// next then reads his buffer too. Buffers and sub-lists live in RDRAM the game never uses (0x80600000..).
#if BLITZ_MODERN
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "recomp.h"
#include "blitz_savestate.h"
#include "team_store.h"
#include "uv_overrides.h"

extern "C" void func_80256580(uint8_t* rdram, recomp_context* ctx);  // the uniform builder (side, team, alt)

namespace blitz {
    std::vector<std::string> team_franchises();  // content_pack_tp.cpp
    void uniform_set_parts(uint8_t* rdram, int team, int set, const uint16_t base[20], uint16_t out[10]);  // uniform_picker.cpp
}

namespace {
    constexpr uint32_t side_buffers = 0x802BE4B0, side_size = 0x60E0;
    constexpr int32_t params_table = (int32_t)0x802EB2E0;  // render parameters, one 0x44 entry per player
    constexpr int32_t params_stride = 0x44, entry_slot = 0x38;
    constexpr uint32_t dl_cursor = 0x8005EA78;  // the frame display list's cursor (lui 0x8006; lw -0x1588)

    constexpr uint32_t buffers = 0x80600000, buffer_stride = 0x6100;   // [side][15 slots]
    constexpr uint32_t sublists = 0x806C0000, sublist_stride = 0x200;  // [side][slot][body, helmet]

    std::vector<std::array<uint16_t, 20>> team_ids;  // every team's uniform ids, as of the latest ovl_b load
    struct Live {
        bool active[2][15] = {};
        bool side_mixed[2] = {};  // a side with any planned player: the others get the side's own loads back
    } live;
    bool (&active)[2][15] = live.active;
    bool (&side_mixed)[2] = live.side_mixed;
    // With the game in a save state: a state saved mid Pro Bowl game dresses its players again (their buffers and
    // sub-lists are in RDRAM, which the state holds).
    [[maybe_unused]] const bool registered = blitz::savestate_register_pod("mixed_uniforms", &live);
    struct Plan { int team = -1, set = 0; };
    Plan plan[2][15];  // this game's: who wears which team's set (team -1: the side's own)

    bool enabled() {
        static const bool on = [] {
            const char* e = getenv("BLITZ_MIXED_UNIFORMS");
            return e != nullptr && e[0] == '1';
        }();
        return on;
    }

    gpr guest(uint32_t a) { return (gpr)(int32_t)a; }
    uint32_t phys(uint32_t a) { return a & 0x1FFFFFFF; }

    void copy_words(uint8_t* rdram, uint32_t dst, uint32_t src, uint32_t len) {
        std::memmove(rdram + phys(dst), rdram + phys(src), len);  // word-aligned: the swapped layout copies as is
    }

    // The game's own body loads (side setup, as func_8027139C emits them each frame) after segment 12 = buffer.
    const uint32_t body_cmds[][2] = {
        {0xE3001001, 0x00008000},
        {0xFD100000, 0x0C004C00}, {0xE8000000, 0}, {0xF5000100, 0x07000000}, {0xE6000000, 0}, {0xF0000000, 0x073FC000}, {0xE7000000, 0},
        {0xFD500000, 0x0C003C00}, {0xF5500000, 0x07014050}, {0xE6000000, 0}, {0xF3000000, 0x070FF400}, {0xE7000000, 0},
        {0xF5400400, 0x01114050}, {0xF2000000, 0x0107C07C},
        {0xFD500000, 0x0C004000}, {0xF5500040, 0x07090250}, {0xE6000000, 0}, {0xF3000000, 0x0707F400}, {0xE7000000, 0},
        {0xF5400440, 0x02290250}, {0xF2000000, 0x0207C03C},
        {0xFD500000, 0x0C004400}, {0xF5500060, 0x07084230}, {0xE6000000, 0}, {0xF3000000, 0x07003800}, {0xE7000000, 0},
        {0xF5400260, 0x03384230}, {0xF2000000, 0x0301C004},
        {0xFD500000, 0x0C004800}, {0xF5500062, 0x07084230}, {0xE6000000, 0}, {0xF3000000, 0x07003800}, {0xE7000000, 0},
        {0xF5400262, 0x04484230}, {0xF2000000, 0x0401C004},
    };
    // ...and the helmet pass's (three RGBA16 textures in one load; tiles 0-2).
    const uint32_t helmet_cmds[][2] = {
        {0xE3001001, 0x00000000},
        {0xFD100000, 0x0C0054E0}, {0xF5100000, 0x07000000}, {0xE6000000, 0}, {0xF3000000, 0x077FF100}, {0xE7000000, 0},
        {0xF5101000, 0x00014050}, {0xF2000000, 0x0007C07C},
        {0xF5101100, 0x0104C050}, {0xF2000000, 0x0107C01C},
        {0xF5101140, 0x0204C050}, {0xF2000000, 0x0207C01C},
    };

    uint32_t buffer_of(int side, int slot) { return buffers + (uint32_t)(side * 15 + slot) * buffer_stride; }
    uint32_t sublist_of(int side, int slot, int which) {
        if (slot < 0) return sublists + 0x10000 + (uint32_t)(side * 2 + which) * sublist_stride;  // the side's own
        return sublists + (uint32_t)((side * 15 + slot) * 2 + which) * sublist_stride;
    }

    template <size_t N>
    void write_sublist(uint8_t* rdram, uint32_t at, uint32_t buffer, const uint32_t (&cmds)[N][2]) {
        uint32_t p = at;
        auto put = [&](uint32_t w0, uint32_t w1) {
            MEM_W(0, guest(p)) = (int32_t)w0;
            MEM_W(4, guest(p)) = (int32_t)w1;
            p += 8;
        };
        put(0xDB060030, phys(buffer));  // segment 12 = this player's buffer
        for (const auto& c : cmds) put(c[0], c[1]);
        put(0xDF000000, 0);
    }

    // The side and roster slot of a render entry (entries 0-6 are the left side's players, 7-13 the right's); slot
    // -1 for a player of a mixed side who wears the side's own uniform.
    bool who(uint8_t* rdram, int32_t entry, int& side, int& slot) {
        int32_t off = entry - params_table;
        if (off < 0 || off % params_stride != 0) return false;
        int index = off / params_stride;
        if (index >= 14) return false;
        side = index / 7;
        if (!side_mixed[side]) return false;
        slot = MEM_W(entry_slot, guest((uint32_t)entry));
        if (slot < 0 || slot >= 15 || !active[side][slot]) slot = -1;
        return true;
    }

    void emit_call(uint8_t* rdram, uint32_t sublist) {
        uint32_t cur = (uint32_t)MEM_W(0, guest(dl_cursor));
        MEM_W(0, guest(cur)) = (int32_t)0xDE000000;  // G_DL, push
        MEM_W(4, guest(cur)) = (int32_t)phys(sublist);
        MEM_W(0, guest(dl_cursor)) = (int32_t)(cur + 8);
    }
}

namespace blitz {
    // uv_overrides.cpp: a render entry wearing his own set (his side and roster slot).
    bool mixed_player_of(uint8_t* rdram, int32_t entry, int& side, int& slot) {
        return who(rdram, entry, side, slot) && slot >= 0;
    }

    // Pro Bowl (pro_bowl.cpp), before kickoff: player `slot` of `side` wears `team`'s set `set` (uniform_set_parts).
    void mixed_plan(int side, int slot, int team, int set) {
        if (side >= 0 && side < 2 && slot >= 0 && slot < 15) plan[side][slot] = {team, set};
    }

    // Every ovl_b load (content_pack_tp.cpp): every team's uniform ids, for kickoff (when ovl_b is gone).
    void mixed_on_ovl_b(uint8_t* rdram) {
        // ovl_b loads between games (ovl_a holds the game): the last game's per-player uniforms are over, or team
        // select's QB models would wear them (seen after a Pro Bowl game).
        std::memset(active, 0, sizeof(active));
        std::memset(side_mixed, 0, sizeof(side_mixed));
        // Every team's uniform ids as the team store wrote them (team_store.h): part p's home at 2p, away at 2p + 1.
        const auto& teams = blitz::teamdata::store().slots();
        size_t n = team_franchises().size();
        team_ids.assign(n, {});
        for (size_t t = 0; t < n && t < teams.size(); t++)
            for (int i = 0; i < 20; i++) team_ids[t][(size_t)i] = teams[t].uniform_part(i / 2, i % 2);
    }
}

extern "C" {
    // Kickoff (uniform_picker.cpp, just before func_80256850 dresses both sides): a buffer per player.
    void blitz_mixed_kickoff(uint8_t* rdram, recomp_context* ctx, int32_t team0, int32_t team1) {
        std::memset(active, 0, sizeof(active));
        std::memset(side_mixed, 0, sizeof(side_mixed));
        blitz::uvo_mixed_reset();
        int32_t team[2] = {team0, team1};
        int n = (int)team_ids.size();
        if (enabled() && n > 0) {  // the eyeball test: everyone a different team
            for (int s = 0; s < 2; s++)
                for (int k = 0; k < 15; k++) plan[s][k] = {(team[s] + 1 + k + s * 7) % n, 0};
        }
        bool any = false;
        for (auto& side : plan) for (auto& p : side) any |= p.team >= 0;
        if (!any || n == 0) return;
        recomp_context saved = *ctx;
        int dressed = 0;
        for (int s = 0; s < 2; s++) {
            namespace live = blitz::teamdata::live;
            if (!live::has_copy(rdram, s)) continue;
            uint16_t original[20];
            for (int i = 0; i < 20; i++) original[i] = live::copy_uniform_part(rdram, s, i / 2, i % 2);
            for (int k = 0; k < 15; k++) {
                int t = plan[s][k].team;
                if (t < 0 || t >= n) continue;
                uint16_t parts[10];
                blitz::uniform_set_parts(rdram, t, plan[s][k].set, team_ids[(size_t)t].data(), parts);
                for (int i = 0; i < 10; i++) {  // the set as both home and away; built as home
                    live::set_copy_uniform_part(rdram, s, i, 0, parts[i]);
                    live::set_copy_uniform_part(rdram, s, i, 1, parts[i]);
                }
                ctx->r4 = s;
                ctx->r5 = team[s];
                ctx->r6 = 0;  // home
                func_80256580(rdram, ctx);
                *ctx = saved;
                blitz::uvo_mixed_dressed(s, k);  // his set's coordinates, if it has a table (uv_overrides.cpp)
                copy_words(rdram, buffer_of(s, k), side_buffers + (uint32_t)s * side_size, side_size);
                write_sublist(rdram, sublist_of(s, k, 0), buffer_of(s, k), body_cmds);
                write_sublist(rdram, sublist_of(s, k, 1), buffer_of(s, k), helmet_cmds);
                active[s][k] = true;
                dressed++;
            }
            for (int i = 0; i < 20; i++) live::set_copy_uniform_part(rdram, s, i / 2, i % 2, original[i]);
            for (int k = 0; k < 15; k++) side_mixed[s] |= active[s][k];
            if (side_mixed[s]) {  // the side's own buffer (filled by the game right after) for its unplanned players
                write_sublist(rdram, sublist_of(s, -1, 0), side_buffers + (uint32_t)s * side_size, body_cmds);
                write_sublist(rdram, sublist_of(s, -1, 1), side_buffers + (uint32_t)s * side_size, helmet_cmds);
            }
        }
        *ctx = saved;
        for (auto& side : plan) for (auto& p : side) p = Plan{};
        fprintf(stderr, "[mixed] kickoff: %d players in their own teams' uniforms\n", dressed);
    }

    // func_8027139C 0x802723E8, before a player's jersey load: his body textures.
    void blitz_mixed_body(uint8_t* rdram, int32_t entry) {
        int side, slot;
        if (who(rdram, entry, side, slot)) emit_call(rdram, sublist_of(side, slot, 0));
    }

    // func_8027139C 0x802726F8, before a player's helmet: his helmet textures.
    void blitz_mixed_helmet(uint8_t* rdram, int32_t entry) {
        int side, slot;
        if (who(rdram, entry, side, slot)) emit_call(rdram, sublist_of(side, slot, 1));
    }
}
#else
#include <cstdint>
struct recomp_context;
extern "C" void blitz_mixed_body(uint8_t*, int32_t) {}
extern "C" void blitz_mixed_helmet(uint8_t*, int32_t) {}
#endif
