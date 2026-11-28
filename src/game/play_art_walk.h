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
//
// The look follows the playbook's own cards (play select): a fat line, lit from the top left so its lower-right edge is
// a darker shade of its colour, a black drop shadow to the lower right, and a broad barbed arrowhead.

struct Vtx {
    float x, y, z;  // world units
    uint32_t rgba;
};

// Vertices and their triangles, drawn from one vertex load (at most 32 vertices).
struct Prim {
    std::vector<Vtx> v;
    std::vector<std::array<uint8_t, 3>> t;
};

struct Style {
    float half_width = 3.0f;    // world units (7.8 a yard): the line
    float head_length = 14.0f;  // the arrowhead, tip to barbs
    float head_half = 10.0f;    // half its span at the barbs
    float notch = 0.4f;         // the barbs swept back: the notch sits this far (of head_length) ahead of them; 0 a plain triangle
    float shade = 0.5f;         // the shadow side's colour, times the line's (1: flat)
    float shadow = 2.6f;        // the drop shadow's offset, world units down and right on screen (0: none)
    float shadow_rim = 0.8f;    // and how much wider than the line it is, each side
    int shadow_alpha = 200;     // at the line's full alpha
    float lift = 0.8f;          // above the turf (y = 0)
};

// Where the light comes from, in the world: the unit direction the drop shadow falls (screen down and right: towards the
// offense's own backfield and its right). Zero: no shadow, no shading.
struct Light {
    float x = 0.0f, z = 0.0f;
};

using Path2 = std::vector<std::array<float, 2>>;

inline uint32_t scaled(uint32_t rgba, float f) {
    auto ch = [&](int s) { return (uint32_t)std::clamp((int)std::lround((float)((rgba >> s) & 0xFFu) * f), 0, 255) << s; };
    return ch(24) | ch(16) | ch(8) | (rgba & 0xFFu);
}

// A vertex's colour: full on the lit side, shade times it on the shadow side (by its offset from the line's centre).
inline uint32_t lit(uint32_t rgba, float ox, float oz, const Light& light, float shade) {
    const float len = std::hypot(ox, oz);
    if (shade >= 1.0f || (light.x == 0.0f && light.z == 0.0f)) return rgba;
    if (len < 1e-4f) return scaled(rgba, 0.5f + 0.5f * shade);
    const float t = std::clamp(0.5f + 0.5f * (ox * light.x + oz * light.z) / len, 0.0f, 1.0f);
    return scaled(rgba, 1.0f - (1.0f - shade) * t);
}

// A strip along a polyline (x, z pairs, legs over 0.5 units): two vertices a joint, mitred where the turn allows; a turn
// over 120 degrees gets square ends and a wedge over the outside of the joint. back extends the start backwards.
inline void strip(const Path2& p, float hw, float back, uint32_t rgba, const Light& light, float shade, float lift,
                  std::vector<Prim>& out) {
    const size_t n = p.size();
    if (n < 2) return;
    std::vector<std::array<float, 2>> t(n - 1), nrm(n - 1);
    for (size_t i = 0; i + 1 < n; i++) {
        float x = p[i + 1][0] - p[i][0], z = p[i + 1][1] - p[i][1], len = std::max(std::hypot(x, z), 1e-4f);
        t[i] = {x / len, z / len};
        nrm[i] = {-t[i][1], t[i][0]};
    }
    auto vtx = [&](float cx, float cz, float ox, float oz) { return Vtx{cx + ox, lift, cz + oz, lit(rgba, ox, oz, light, shade)}; };
    // Runs of stations (a centre and its offset), split at the sharp joints.
    struct Station {
        float x, z, ox, oz;
    };
    std::vector<std::vector<Station>> runs(1);
    runs[0].push_back({p[0][0] - t[0][0] * back, p[0][1] - t[0][1] * back, nrm[0][0] * hw, nrm[0][1] * hw});
    for (size_t j = 1; j + 1 < n; j++) {
        const auto& a = nrm[j - 1];
        const auto& b = nrm[j];
        float mx = a[0] + b[0], mz = a[1] + b[1], ml = std::hypot(mx, mz);
        float c = ml < 1e-3f ? 0.0f : (mx * a[0] + mz * a[1]) / ml;
        if (c >= 0.5f) {
            runs.back().push_back({p[j][0], p[j][1], mx / ml * hw / c, mz / ml * hw / c});
            continue;
        }
        runs.back().push_back({p[j][0], p[j][1], a[0] * hw, a[1] * hw});
        runs.push_back({{p[j][0], p[j][1], b[0] * hw, b[1] * hw}});
        Prim w;  // the wedge: the joint, and both legs' corners on each side
        const float x = p[j][0], z = p[j][1];
        w.v = {vtx(x, z, 0, 0), vtx(x, z, a[0] * hw, a[1] * hw), vtx(x, z, b[0] * hw, b[1] * hw), vtx(x, z, -a[0] * hw, -a[1] * hw),
               vtx(x, z, -b[0] * hw, -b[1] * hw)};
        w.t = {{0, 1, 2}, {0, 3, 4}};
        out.push_back(w);
    }
    runs.back().push_back({p[n - 1][0], p[n - 1][1], nrm[n - 2][0] * hw, nrm[n - 2][1] * hw});
    // A run of m stations is 2m vertices; one load takes 16 stations, the next starts at the last one again.
    for (const auto& r : runs) {
        for (size_t s0 = 0; s0 + 1 < r.size(); s0 += 15) {
            const size_t s1 = std::min(r.size(), s0 + 16);
            Prim q;
            for (size_t s = s0; s < s1; s++) {
                q.v.push_back(vtx(r[s].x, r[s].z, r[s].ox, r[s].oz));
                q.v.push_back(vtx(r[s].x, r[s].z, -r[s].ox, -r[s].oz));
            }
            for (uint8_t k = 0; k + 1 < (uint8_t)(s1 - s0); k++) {
                q.t.push_back({(uint8_t)(2 * k), (uint8_t)(2 * k + 1), (uint8_t)(2 * k + 2)});
                q.t.push_back({(uint8_t)(2 * k + 1), (uint8_t)(2 * k + 3), (uint8_t)(2 * k + 2)});
            }
            out.push_back(q);
        }
    }
}

// A route: a line along a world polyline (x, z pairs) with a barbed arrowhead at its end, over its drop shadow.
inline void ribbon(const Path2& in, uint32_t rgba, const Style& s, std::vector<Prim>& out, const Light& light = {}) {
    Path2 p;
    for (const auto& q : in) {
        if (p.empty() || std::hypot(q[0] - p.back()[0], q[1] - p.back()[1]) > 0.5f) p.push_back(q);
    }
    if (p.size() < 2) return;
    // The head, scaled down on a short last leg; the line stops at its notch.
    auto& tip = p.back();
    const auto& before = p[p.size() - 2];
    const float lx = tip[0] - before[0], lz = tip[1] - before[1], last = std::hypot(lx, lz);
    const float head = std::min(s.head_length, last * 0.6f), k = head / s.head_length;
    const float half = s.head_half * k, notch = s.notch * head;
    const std::array<float, 2> point = tip, dir{lx / last, lz / last}, nl{-dir[1], dir[0]};
    const std::array<float, 2> barb_base{point[0] - dir[0] * head, point[1] - dir[1] * head};
    tip = {barb_base[0] + dir[0] * notch, barb_base[1] + dir[1] * notch};
    // tip, left barb, notch, right barb (offsets from the head's middle, for the shading)
    const std::array<std::array<float, 2>, 4> hd{{point,
                                                  {barb_base[0] + nl[0] * half, barb_base[1] + nl[1] * half},
                                                  tip,
                                                  {barb_base[0] - nl[0] * half, barb_base[1] - nl[1] * half}}};
    const std::array<float, 2> mid{barb_base[0] + dir[0] * head * 0.4f, barb_base[1] + dir[1] * head * 0.4f};
    auto head_prim = [&](float dx, float dz, float grow, uint32_t c, float shade) {
        Prim q;
        for (size_t i = 0; i < 4; i++) {
            float ox = hd[i][0] - mid[0], oz = hd[i][1] - mid[1];
            // Grown for the shadow's rim: out from the middle (the notch stays put).
            const float ol = std::hypot(ox, oz);
            const float gx = i == 2 || ol < 1e-4f ? 0.0f : ox / ol * grow, gz = i == 2 || ol < 1e-4f ? 0.0f : oz / ol * grow;
            q.v.push_back(Vtx{hd[i][0] + dx + gx, s.lift, hd[i][1] + dz + gz, shade >= 1.0f ? c : lit(c, ox, oz, light, shade)});
        }
        if (s.notch > 0.0f) q.t = {{0, 1, 2}, {0, 2, 3}};
        else q.t = {{0, 1, 3}};
        out.push_back(q);
    };
    const bool shadow = s.shadow > 0.0f && (light.x != 0.0f || light.z != 0.0f);
    if (shadow) {
        const uint32_t a = (uint32_t)std::clamp((int)std::lround((float)s.shadow_alpha * (float)(rgba & 0xFFu) / 255.0f), 0, 255);
        const float dx = light.x * s.shadow, dz = light.z * s.shadow;
        Path2 o = p;
        for (auto& q : o) q = {q[0] + dx, q[1] + dz};
        // The line runs into the head a little so no gap shows at the notch.
        o.back() = {o.back()[0] + dir[0] * s.shadow_rim, o.back()[1] + dir[1] * s.shadow_rim};
        strip(o, s.half_width + s.shadow_rim, s.shadow_rim, a, {}, 1.0f, s.lift, out);
        head_prim(dx, dz, s.shadow_rim * 1.5f, a, 1.0f);
    }
    Path2 f = p;
    f.back() = {f.back()[0] + dir[0] * notch * 0.5f, f.back()[1] + dir[1] * notch * 0.5f};
    strip(f, s.half_width, 0.0f, rgba, light, s.shade, s.lift, out);
    head_prim(0, 0, 0, rgba, s.shade);
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
        const int nv = (int)prims[i].v.size();
        if (count + nv > max_vertices_per_load && i > first) {
            batches.emplace_back(first, i);
            first = i;
            count = 0;
        }
        count += nv;
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
            for (const Vtx& v : prims[i].v) {
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
        for (size_t i = b; i < e; i++) n += (uint32_t)prims[i].v.size();
        cmd(0x01000000u | (n << 12) | (n << 1), batch_addr[bi]);  // G_VTX n, v0 = 0
        std::vector<std::array<uint32_t, 3>> tris;
        uint32_t at = 0;
        for (size_t i = b; i < e; i++) {
            for (const auto& t : prims[i].t) tris.push_back({at + t[0], at + t[1], at + t[2]});
            at += (uint32_t)prims[i].v.size();
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

// The looks (PLAYART look card|blend|madden):
//   Card    the play cards exactly: every route the cards' red (the playbook doesn't tell receivers apart).
//   Blend   the cards' line, shading, shadow and barbed head, in the Receiver Buttons' colours made as saturated as the
//           cards' red: the colour still says which button throws to whom. The default.
//   Madden  1.0.5's thin flat translucent ribbons with a plain head.
enum class Look { Card = 0, Blend = 1, Madden = 2 };
constexpr Look default_look = Look::Blend;

struct LookSpec {
    Style style;
    std::array<uint32_t, 3> colour;  // RGBA, the alpha the look's
};

inline LookSpec look_spec(Look look, int family) {
    LookSpec k;
    if (look == Look::Madden) {
        k.style.half_width = 3.5f;
        k.style.head_length = 9.0f;
        k.style.head_half = 5.5f;
        k.style.notch = 0.0f;
        k.style.shade = 1.0f;
        k.style.shadow = 0.0f;
        k.colour = colours(family);
        for (auto& c : k.colour) c = (c & 0xFFFFFF00u) | 208;
        return k;
    }
    if (look == Look::Card) {
        k.colour = {0xF03A24F5u, 0xF03A24F5u, 0xF03A24F5u};  // the cards' red (240, 58, 36)
    }
    else if (family == 1) {
        k.colour = {0xFFC814F5u, 0x2E9BFFF5u, 0xF5283CF5u};  // Y yellow, RB blue, B red
    }
    else {
        k.colour = {0x18D878F5u, 0x2E9BFFF5u, 0xF5283CF5u};  // triangle green, R1 blue, circle red
    }
    return k;
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
    // The light: the shadow falls to the lower right of the screen, behind the quarterback's camera that's back towards
    // the offense's own end and to its right.
    const Light light{-dir * 0.7071f, across * 0.7071f};
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
        ribbon(world, colour[k], style, prims, light);
    }
    if (prims.empty()) art.why = "no routes (blocks only)";
    return art;
}

}  // namespace playart
