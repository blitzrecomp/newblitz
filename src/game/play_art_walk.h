// Play art (docs/notes/PLAY_ART.md): the route walker, the ribbon geometry and the display list that draws it, with
// nothing of the game or RT64 in it, so tests/play_art_test.cpp runs the same code over the ROM's 45 offense plays.
//
// Frames used here:
//   offense frame  D = yards downfield from the line (negative: the backfield), L = yards across, + = the offense's
//                  right before mirroring; the formation's own numbers (f.x / 7.8, f.z / 7.8), as play_routes.py.
//   world          the game's units (7.8 a yard): x along the field, z across it, y up; the camera's projection
//                  already holds the view, so world points go straight into vertices (under a 1/4 scale matrix).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace playart {

constexpr float units_per_yard = 7.8f;  // 0x40F9999A in the lineup code (0x803028EC)
constexpr int full_turn = 1024;         // route directions: 0 straight downfield, 1024 a full turn

enum class Cut {
    Raw,         // the whole script, as play_routes.py traces it (tests)
    FirstBreak,  // to the first change of direction, then a short stub of the new direction
    Full,        // to the script's end (IMPROVISE, BLOCK), capped
};

// Caps (yards of path). First break: the stub after the break, and the whole route.
constexpr float first_break_stub = 5.0f, first_break_cap = 15.0f, full_cap = 40.0f;
constexpr int break_angle = 71;   // 25 degrees: a smaller turn (a swing's 2-yard steps bend 10-20) isn't a break
constexpr int max_points = 14;    // a route's points, start included

struct Point {
    float d, l;
};

// Big-endian word of game memory at a virtual address (0x80xxxxxx); false when it can't be read.
using Read32 = std::function<bool(uint32_t addr, int32_t& out)>;

// Route script opcodes (interpreter ovl_a 0x80310B98): size in words (0: unknown) and whether it ends the path.
struct OpInfo {
    int words;
    bool ends;
};
inline OpInfo op_info(int32_t op) {
    switch (op) {
        case 1: return {1, false};   // NOP
        case 2: return {2, false};   // TURN dir
        case 3: return {2, false};   // RUN_TO_LOS yards
        case 4: return {2, false};   // RUN_YDS yards
        case 5: return {1, true};    // IMPROVISE
        case 6: case 7: return {1, true};  // mode 3
        case 8: return {1, true};    // BLOCK
        case 9: case 11: case 12: case 13: case 14: return {1, false};  // waits, drone flags
        case 10: return {4, false};  // COVER_HINT dir, chance, time
        case 15: return {3, false};  // COVER_HINT2 dir, delay
        case 16: return {2, false};  // WAIT_TICKS
        case 20: return {2, false};  // SET_68
        default: return {0, false};
    }
}

struct Route {
    std::vector<Point> pts;  // the start, then a point per leg
    const char* ending = "end";
    int ops = 0;
};

inline int angle_between(int a, int b) {
    int d = ((a - b) % full_turn + full_turn) % full_turn;
    return d > full_turn / 2 ? full_turn - d : d;
}

// Walks a receiver's script from his lineup spot (offense frame). mirrored: he lines up left of the ball, so every
// direction is negated (player +0x1A4 bit 0x200; the rule play_routes.py uses and the play-select tiles agree with).
inline Route walk(const Read32& rd, uint32_t script, Point start, bool mirrored, Cut cut) {
    Route r;
    r.pts.push_back(start);
    float d = start.d, l = start.l, total = 0.0f;
    int ang = 0, last_leg_ang = 0, stub_ang = 0;
    bool have_leg = false, in_stub = false;
    float stub_left = 0.0f;
    const float cap = cut == Cut::FirstBreak ? first_break_cap : cut == Cut::Full ? full_cap : 1e9f;
    uint32_t a = script;
    for (int n = 0; n < 64; n++) {
        int32_t op = 0;
        if (!rd(a, op)) {
            r.ending = "unreadable";
            break;
        }
        OpInfo info = op_info(op);
        if (info.words == 0) {
            r.ending = "unknown";
            break;
        }
        int32_t arg = 0;
        if (info.words > 1 && !rd(a + 4, arg)) {
            r.ending = "unreadable";
            break;
        }
        a += 4u * (uint32_t)info.words;
        r.ops++;
        if (info.ends) {
            r.ending = op == 5 ? "improvise" : op == 8 ? "block" : "mode3";
            break;
        }
        if (op == 2) {
            ang = mirrored ? -arg : arg;
            continue;
        }
        if (op != 3 && op != 4) continue;
        const float t = (float)ang * 2.0f * 3.14159265f / (float)full_turn;
        const float dd = std::cos(t), dl = -std::sin(t);
        float dist;
        if (op == 4) {
            dist = (float)arg;
        }
        else {
            if (dd <= 1e-3f) continue;
            dist = std::max(0.0f, ((float)arg - d) / dd);
        }
        if (cut == Cut::Raw) {
            d += dd * dist;
            l += dl * dist;
            r.pts.push_back({d, l});
            continue;
        }
        if (dist < 0.05f) continue;
        bool stop = false;
        if (cut == Cut::FirstBreak) {
            // The break: show which way it goes for a few yards (over however many legs keep that way), then stop.
            if (in_stub && angle_between(ang, stub_ang) > break_angle) {
                r.ending = "cut";
                break;
            }
            if (!in_stub && have_leg && angle_between(ang, last_leg_ang) > break_angle) {
                in_stub = true;
                stub_ang = ang;
                stub_left = first_break_stub;
            }
            if (in_stub) {
                if (dist >= stub_left) {
                    dist = stub_left;
                    stop = true;
                }
                stub_left -= dist;
            }
        }
        if (total + dist >= cap) {
            dist = cap - total;
            stop = true;
        }
        total += dist;
        d += dd * dist;
        l += dl * dist;
        r.pts.push_back({d, l});
        have_leg = true;
        last_leg_ang = ang;
        if (stop || (int)r.pts.size() >= max_points) {
            r.ending = stop ? "cut" : "points";
            break;
        }
    }
    return r;
}

// ---- Geometry ----------------------------------------------------------------------------------------------------

struct Vtx {
    float x, y, z;  // world units
    uint32_t rgba;
};

// A few vertices and their triangles, drawn from one vertex load.
struct Prim {
    std::array<Vtx, 4> v{};
    int nv = 0;
    std::array<std::array<uint8_t, 3>, 2> t{};
    int nt = 0;
};

struct Style {
    float half_width = 2.2f;   // world units (a 0.55-yard ribbon)
    float head_length = 9.0f;  // the arrowhead
    float head_half = 5.5f;
    float lift = 0.8f;         // above the turf (y = 0)
};

// A ribbon along a world polyline (x, z pairs) with an arrowhead at its end: a quad per leg, mitred where legs meet
// (a wedge fills the outside of a turn too sharp to mitre), and a triangle for the head.
inline void ribbon(const std::vector<std::array<float, 2>>& in, uint32_t rgba, const Style& s, std::vector<Prim>& out) {
    std::vector<std::array<float, 2>> p;
    for (const auto& q : in) {
        if (p.empty() || std::hypot(q[0] - p.back()[0], q[1] - p.back()[1]) > 0.5f) p.push_back(q);
    }
    if (p.size() < 2) return;
    // The last leg stops where the head starts.
    auto& tip = p.back();
    const auto& before = p[p.size() - 2];
    float lx = tip[0] - before[0], lz = tip[1] - before[1], last = std::hypot(lx, lz);
    float head = std::min(s.head_length, last * 0.6f);
    std::array<float, 2> point = tip, dir{lx / last, lz / last};
    tip = {tip[0] - dir[0] * head, tip[1] - dir[1] * head};
    const size_t n = p.size();
    // Each leg's unit direction and its left normal.
    std::vector<std::array<float, 2>> t(n - 1), nrm(n - 1);
    for (size_t i = 0; i + 1 < n; i++) {
        float x = p[i + 1][0] - p[i][0], z = p[i + 1][1] - p[i][1], len = std::max(std::hypot(x, z), 1e-4f);
        t[i] = {x / len, z / len};
        nrm[i] = {-t[i][1], t[i][0]};
    }
    // The offset at each end of each leg: mitred at a joint when the turn allows, square otherwise.
    auto offset_at = [&](size_t joint, size_t leg) -> std::array<float, 2> {
        const auto& m0 = nrm[leg];
        if (joint == 0 || joint == n - 1) return {m0[0] * s.half_width, m0[1] * s.half_width};
        const auto& a = nrm[joint - 1];
        const auto& b = nrm[joint];
        float mx = a[0] + b[0], mz = a[1] + b[1], ml = std::hypot(mx, mz);
        if (ml < 1e-3f) return {m0[0] * s.half_width, m0[1] * s.half_width};
        mx /= ml;
        mz /= ml;
        float c = mx * a[0] + mz * a[1];
        if (c < 0.5f) return {m0[0] * s.half_width, m0[1] * s.half_width};  // over 120 degrees: square ends + wedge
        return {mx * s.half_width / c, mz * s.half_width / c};
    };
    auto vtx = [&](float x, float z) { return Vtx{x, s.lift, z, rgba}; };
    for (size_t i = 0; i + 1 < n; i++) {
        auto o0 = offset_at(i, i), o1 = offset_at(i + 1, i);
        Prim q;
        q.v = {vtx(p[i][0] + o0[0], p[i][1] + o0[1]), vtx(p[i][0] - o0[0], p[i][1] - o0[1]),
               vtx(p[i + 1][0] + o1[0], p[i + 1][1] + o1[1]), vtx(p[i + 1][0] - o1[0], p[i + 1][1] - o1[1])};
        q.nv = 4;
        q.t[0] = {0, 1, 2};
        q.t[1] = {1, 3, 2};
        q.nt = 2;
        out.push_back(q);
        // A joint too sharp to mitre: a wedge over the gap on each side (one of them lies inside the ribbon).
        if (i + 2 < n) {
            const auto& a = nrm[i];
            const auto& b = nrm[i + 1];
            float mx = a[0] + b[0], mz = a[1] + b[1], ml = std::hypot(mx, mz);
            float c = ml < 1e-3f ? 0.0f : (mx * a[0] + mz * a[1]) / ml;
            if (c < 0.5f) {
                const auto& j = p[i + 1];
                for (float side : {1.0f, -1.0f}) {
                    Prim w;
                    w.v[0] = vtx(j[0], j[1]);
                    w.v[1] = vtx(j[0] + side * a[0] * s.half_width, j[1] + side * a[1] * s.half_width);
                    w.v[2] = vtx(j[0] + side * b[0] * s.half_width, j[1] + side * b[1] * s.half_width);
                    w.nv = 3;
                    w.t[0] = {0, 1, 2};
                    w.nt = 1;
                    out.push_back(w);
                }
            }
        }
    }
    const auto& nl = nrm.back();
    Prim h;
    h.v[0] = vtx(tip[0] + nl[0] * s.head_half, tip[1] + nl[1] * s.head_half);
    h.v[1] = vtx(tip[0] - nl[0] * s.head_half, tip[1] - nl[1] * s.head_half);
    h.v[2] = vtx(point[0], point[1]);
    h.nv = 3;
    h.t[0] = {0, 1, 2};
    h.nt = 1;
    out.push_back(h);
}

// ---- The display list ----------------------------------------------------------------------------------------------
//
// Written at the game's own cursor (the frame's list, so it lives exactly as long as the frame, like gfx_ext.cpp's
// additions): a branch over the data, a 1/4 scale matrix and the vertices, then the commands. F3DEX2 encodings (the
// players' pass runs F3DLX2.Rej, the same command set; loads stay at 32 vertices so either microcode takes them).

constexpr float vertex_scale = 4.0f;  // vertices in quarter units: s16 has room (the field is +-460), joints are smooth
constexpr int max_vertices_per_load = 32;

// Render modes. ZB_XLU_SURF: depth test, no depth write, blended (IN * IN_A + MEM * (1 - A)).
constexpr uint32_t rm_xlu_ztest = 0x00504A50, rm_xlu_noz = 0x00504240;
// What gfx_load_camera leaves (0x8002B818 / 0x8002B7D8), put back afterwards.
constexpr uint32_t rm_camera = 0x0F0A4000, geom_camera = 0x00000401;  // G_ZBUFFER | G_CULL_BACK

struct ListOptions {
    bool ztest = true;
    std::array<uint32_t, 4> ex_group_push{};  // gEXMatrixGroup (2 commands), or zeros for none
    std::array<uint32_t, 2> ex_group_pop{};   // gEXPopMatrixGroup (1 command)
    bool use_ex = false;
};

inline uint32_t vtx_word0(const Vtx& v) {
    auto s16 = [](float f) { return (uint32_t)(uint16_t)(int16_t)std::lround(std::clamp(f * vertex_scale, -32000.0f, 32000.0f)); };
    return (s16(v.x) << 16) | s16(v.y);
}
inline uint32_t vtx_word1(const Vtx& v) {
    return (uint32_t)(uint16_t)(int16_t)std::lround(std::clamp(v.z * vertex_scale, -32000.0f, 32000.0f)) << 16;
}

// The words (in pairs) of the list placed at address base. Empty if there's nothing to draw.
inline std::vector<uint32_t> build_list(uint32_t base, const std::vector<Prim>& prims, const ListOptions& o) {
    std::vector<uint32_t> w;
    if (prims.empty()) return w;
    // Batches of whole prims, at most 32 vertices each.
    std::vector<std::pair<size_t, size_t>> batches;  // [first prim, end prim)
    size_t first = 0;
    int count = 0;
    for (size_t i = 0; i < prims.size(); i++) {
        if (count + prims[i].nv > max_vertices_per_load) {
            batches.emplace_back(first, i);
            first = i;
            count = 0;
        }
        count += prims[i].nv;
    }
    batches.emplace_back(first, prims.size());

    auto cmd = [&](uint32_t a, uint32_t b) {
        w.push_back(a);
        w.push_back(b);
    };
    cmd(0xDE010000u, 0);  // G_DL branch over the data; target filled below
    const uint32_t matrix = base + 8;
    // 1/4 scale: integer halves all 0 but m[3][3] = 1; fraction halves 0x4000 on m[0][0], m[1][1], m[2][2].
    const uint32_t mtx[16] = {0, 0, 0, 0, 0, 0, 0, 1, 0x40000000u, 0, 0x00004000u, 0, 0, 0x40000000u, 0, 0};
    w.insert(w.end(), mtx, mtx + 16);
    std::vector<uint32_t> batch_addr;
    for (auto [b, e] : batches) {
        batch_addr.push_back(base + (uint32_t)w.size() * 4);
        for (size_t i = b; i < e; i++) {
            for (int k = 0; k < prims[i].nv; k++) {
                const Vtx& v = prims[i].v[(size_t)k];
                w.push_back(vtx_word0(v));
                w.push_back(vtx_word1(v));
                w.push_back(0);  // s, t
                w.push_back(v.rgba);
            }
        }
    }
    w[1] = base + (uint32_t)w.size() * 4;

    cmd(0xE7000000u, 0);  // G_RDPPIPESYNC
    if (o.use_ex) w.insert(w.end(), o.ex_group_push.begin(), o.ex_group_push.end());
    cmd(0xDA380002u, matrix);           // G_MTX modelview, LOAD | PUSH
    cmd(0xD9000000u, 0);                // G_GEOMETRYMODE: clear all
    cmd(0xD9FFFFFFu, 0x00200005u);      // set G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH (no culling)
    cmd(0xD7000000u, 0);                // G_TEXTURE off
    cmd(0xE3000A01u, 0);                // cycle type 1CYCLE
    cmd(0xE200001Cu, o.ztest ? rm_xlu_ztest : rm_xlu_noz);
    cmd(0xFCFFFFFFu, 0xFFFE793Cu);      // G_CC_SHADE, G_CC_SHADE
    for (size_t bi = 0; bi < batches.size(); bi++) {
        auto [b, e] = batches[bi];
        uint32_t n = 0;
        for (size_t i = b; i < e; i++) n += (uint32_t)prims[i].nv;
        cmd(0x01000000u | (n << 12) | (n << 1), batch_addr[bi]);  // G_VTX n, v0 = 0
        std::vector<std::array<uint32_t, 3>> tris;
        uint32_t at = 0;
        for (size_t i = b; i < e; i++) {
            for (int k = 0; k < prims[i].nt; k++) {
                const auto& t = prims[i].t[(size_t)k];
                tris.push_back({at + t[0], at + t[1], at + t[2]});
            }
            at += (uint32_t)prims[i].nv;
        }
        auto tri = [](const std::array<uint32_t, 3>& t) { return (t[0] * 2) << 16 | (t[1] * 2) << 8 | (t[2] * 2); };
        for (size_t k = 0; k < tris.size(); k += 2) {
            if (k + 1 < tris.size()) cmd(0x06000000u | tri(tris[k]), tri(tris[k + 1]));  // G_TRI2
            else cmd(0x05000000u | tri(tris[k]), 0);                                    // G_TRI1
        }
    }
    cmd(0xE7000000u, 0);                // G_RDPPIPESYNC
    cmd(0xD8380002u, 0x40);             // G_POPMTX modelview, 1
    if (o.use_ex) w.insert(w.end(), o.ex_group_pop.begin(), o.ex_group_pop.end());
    cmd(0xD9000000u, 0);                // the camera's geometry mode and render mode again
    cmd(0xD9FFFFFFu, geom_camera);
    cmd(0xE200001Cu, rm_camera);
    return w;
}

// Route colours, left to right as the receivers line up (the Receiver Buttons order): the controller's own button
// colours where it has them. family: controllers.cpp's port_glyph_family (0 PlayStation, 1 Xbox, 2 Switch, 3 N64).
inline std::array<uint32_t, 3> colours(int family) {
    if (family == 1) return {0xFFD23AFFu, 0x6EC1FFFFu, 0xFF5A66FFu};  // Y yellow, RB blue, B red
    return {0x3FE6A0FFu, 0x6EC1FFFFu, 0xFF5A66FFu};                    // triangle green, R1 blue, circle red
}

// ---- From the game's memory to the ribbons ------------------------------------------------------------------------

namespace game {
    constexpr uint32_t players = 0x802D1590, stride = 0xB98, f_screen_x = 0x750;
    constexpr uint32_t chosen_play = 0x802EA4AC;  // [side]: the play record (+0 formation, +4 flags, +8 7 scripts)
}

struct Art {
    uint32_t record = 0, flags = 0;
    std::array<int, 3> legs{};    // each receiver's legs, left to right
    const char* why = nullptr;    // set when there's nothing to draw
};

// The ribbons of side's receivers (player slots 4-6) for the play it has chosen, from where they stand now. The play's
// world axes come from the seven players' spots against the formation's numbers (a correlation over every pair), so
// the side, its direction and a flipped play need no flags of their own. Colours go left to right on screen.
inline Art build_art(const Read32& rd, int side, Cut cut, const Style& style, const std::array<uint32_t, 3>& colour,
                     std::vector<Prim>& prims) {
    Art art;
    auto f32 = [&](uint32_t a, float& out) {
        int32_t v;
        if (!rd(a, v)) return false;
        uint32_t u = (uint32_t)v;
        std::memcpy(&out, &u, 4);
        return std::isfinite(out);
    };
    int32_t v = 0;
    if (!rd(game::chosen_play + 4u * (uint32_t)side, v)) return art.why = "no play record", art;
    art.record = (uint32_t)v;
    int32_t form = 0, flags = 0;
    if (!rd(art.record, form) || !rd(art.record + 4, flags)) return art.why = "no play record", art;
    art.flags = (uint32_t)flags;
    if ((flags & 6) && !(flags & 0x30)) return art.why = "a kick", art;
    std::array<float, 7> wx{}, wz{}, fx{}, fz{}, sx{};
    for (uint32_t s = 0; s < 7; s++) {
        const uint32_t p = game::players + (uint32_t)(side * 7 + (int)s) * game::stride;
        const uint32_t f = (uint32_t)form + 0x14u * s;
        if (!f32(p, wx[s]) || !f32(p + 8, wz[s]) || !f32(f, fx[s]) || !f32(f + 4, fz[s]) || !f32(p + game::f_screen_x, sx[s])) {
            return art.why = "can't read the players or the formation", art;
        }
    }
    float cx = 0, cz = 0;
    for (size_t i = 0; i < 7; i++) {
        for (size_t j = i + 1; j < 7; j++) {
            cx += (wx[i] - wx[j]) * (fx[i] - fx[j]);
            cz += (wz[i] - wz[j]) * (fz[i] - fz[j]);
        }
    }
    if (std::fabs(cx) < 1.0f || std::fabs(cz) < 1.0f) return art.why = "can't place the formation", art;
    const float dir = cx > 0 ? 1.0f : -1.0f, across = cz > 0 ? 1.0f : -1.0f;
    std::array<int, 3> order{4, 5, 6};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return sx[(size_t)a] < sx[(size_t)b]; });
    for (size_t k = 0; k < 3; k++) {
        const size_t s = (size_t)order[k];
        int32_t script = 0;
        if (!rd(art.record + 8 + 4u * (uint32_t)s, script)) continue;
        const Point start{fx[s] / units_per_yard, fz[s] / units_per_yard};
        const Route r = walk(rd, (uint32_t)script, start, fz[s] < 0.0f, cut);
        std::vector<std::array<float, 2>> world;
        for (const auto& q : r.pts) {
            world.push_back({wx[s] + dir * (q.d - start.d) * units_per_yard, wz[s] + across * (q.l - start.l) * units_per_yard});
        }
        art.legs[k] = (int)r.pts.size() - 1;
        ribbon(world, colour[k], style, prims);
    }
    if (prims.empty()) art.why = "no routes (blocks only)";
    return art;
}

}  // namespace playart
