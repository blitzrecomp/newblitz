// Play art's walker and display list (src/game/play_art_walk.h) over the ROM's 45 offense plays.
//
//   play_art_test [build/baserom.z64]          self-checks: caps, the display list's shape, sizes
//   play_art_test ROM --dump                   also every player's raw path, for tests/test_play_art.py to compare
//                                              with tools/analysis/play_routes.py
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../src/game/play_art_walk.h"

namespace {
    std::vector<uint8_t> rom;
    struct Seg {
        uint32_t rom, vram, rom_end;
    };
    // tools/gen_syms.py: main ROM 0x2FC20 -> 0x80248F00, ovl_a ROM 0x93EC0 -> 0x802EFE70.
    const Seg segs[] = {{0x2FC20, 0x80248F00, 0x93EC0}, {0x93EC0, 0x802EFE70, 0xDE350}};

    bool read(uint32_t a, int32_t& out) {
        for (const Seg& s : segs) {
            if (a >= s.vram && a < s.vram + (s.rom_end - s.rom) && (a & 3) == 0) {
                size_t o = s.rom + (a - s.vram);
                if (o + 4 > rom.size()) return false;
                out = (int32_t)((uint32_t)rom[o] << 24 | (uint32_t)rom[o + 1] << 16 | (uint32_t)rom[o + 2] << 8 | rom[o + 3]);
                return true;
            }
        }
        return false;
    }
    uint32_t u32(uint32_t a) {
        int32_t v = 0;
        read(a, v);
        return (uint32_t)v;
    }
    float f32(uint32_t a) {
        uint32_t v = u32(a);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }

    int failures = 0;
    void fail(const std::string& what) {
        if (failures++ < 30) fprintf(stderr, "FAIL %s\n", what.c_str());
    }

    float length(const playart::Route& r) {
        float t = 0;
        for (size_t i = 1; i < r.pts.size(); i++) t += std::hypot(r.pts[i].d - r.pts[i - 1].d, r.pts[i].l - r.pts[i - 1].l);
        return t;
    }

    // Walks a list the way the RSP would and checks it's one the microcode takes. Returns its triangle count.
    int check_list(const std::vector<uint32_t>& w, uint32_t base, const std::string& name) {
        if (w.size() % 2) fail(name + ": odd word count");
        if (w.size() < 2 || w[0] != 0xDE010000u) {
            fail(name + ": doesn't start with a branch");
            return 0;
        }
        const uint32_t cmds = w[1], end = base + (uint32_t)w.size() * 4;
        if (cmds <= base + 8 || cmds >= end || (cmds - base) % 8) fail(name + ": branch target outside the list");
        const uint32_t data = base + 8;
        int loaded = 0, tris = 0;
        bool popped = false, pushed = false;
        for (uint32_t a = cmds; a < end; a += 8) {
            const uint32_t w0 = w[(a - base) / 4], w1 = w[(a - base) / 4 + 1];
            const uint32_t op = w0 >> 24;
            auto tri_ok = [&](uint32_t t) {
                for (int s : {16, 8, 0}) {
                    uint32_t i = (t >> s) & 0xFF;
                    if (i % 2 || (int)(i / 2) >= loaded) return false;
                }
                return true;
            };
            switch (op) {
                case 0x01: {
                    int n = (int)((w0 >> 12) & 0xFF), v0end = (int)((w0 & 0xFF) >> 1);
                    if (n < 1 || n > 32 || v0end != n) fail(name + ": bad G_VTX count");
                    if (w1 % 8 || w1 < data + 64 || w1 + 16u * (uint32_t)n > cmds) fail(name + ": G_VTX outside the data");
                    loaded = n;
                    break;
                }
                case 0x05:
                    if (!tri_ok(w0 & 0xFFFFFF)) fail(name + ": G_TRI1 index past the load");
                    tris++;
                    break;
                case 0x06:
                    if (!tri_ok(w0 & 0xFFFFFF) || !tri_ok(w1)) fail(name + ": G_TRI2 index past the load");
                    tris += 2;
                    break;
                case 0xDA:
                    if (w1 != data) fail(name + ": matrix not at the data's start");
                    pushed = true;
                    break;
                case 0xD8: popped = true; break;
                case 0xD9: case 0xD7: case 0xE3: case 0xE2: case 0xE7: case 0xFC: break;
                case 0x64: break;  // RT64's extended commands (matrix groups)
                default: fail(name + ": unexpected opcode " + std::to_string(op));
            }
        }
        if (!pushed || !popped) fail(name + ": matrix push without its pop");
        // The scale matrix: 1/4 on the diagonal, 1 at w.
        const uint32_t m[16] = {0, 0, 0, 0, 0, 0, 0, 1, 0x40000000u, 0, 0x4000u, 0, 0, 0x40000000u, 0, 0};
        for (int i = 0; i < 16; i++) {
            if (w[2 + (size_t)i] != m[i]) fail(name + ": matrix word " + std::to_string(i));
        }
        // It hands back the state gfx_load_camera left.
        if (w[w.size() - 1] != playart::rm_camera || w[w.size() - 3] != playart::geom_camera) fail(name + ": state not restored");
        return tris;
    }
}

// --rdram DUMP DLIST SIZE OUT [first|full]: the art for the dump's offense, spliced into that frame's list at the
// players' camera load (after the second camera's two matrix groups, where gfx_ext.cpp's hook writes it), the list
// itself put in the frame list's unused tail and called from there. tools/gbi_dump.py --render OUT DLIST draws it.
// Dumps are BLITZ_DUMP_RDRAM_AT's: native little-endian words.
int splice(int argc, char** argv) {
    if (argc < 6) return fprintf(stderr, "--rdram DUMP DLIST SIZE OUT [first|full]\n"), 2;
    std::ifstream f(argv[2], std::ios::binary);
    std::vector<uint8_t> ram{std::istreambuf_iterator<char>(f), {}};
    if (ram.size() != 8u << 20) return fprintf(stderr, "not an 8 MB dump\n"), 2;
    const uint32_t dl = (uint32_t)strtoul(argv[3], nullptr, 16), size = (uint32_t)strtoul(argv[4], nullptr, 0);
    auto at = [&](uint32_t a) -> uint32_t& { return *reinterpret_cast<uint32_t*>(&ram[(a & 0x7FFFFF) & ~3u]); };
    playart::Read32 rd = [&](uint32_t a, int32_t& out) {
        if (a < 0x80000000u || a >= 0x80800000u || (a & 3)) return false;
        out = (int32_t)at(a);
        return true;
    };
    const int side = (int)(at(0x802EA47C) & 1);
    const bool full = argc > 6 && std::string(argv[6]) == "full";
    // "lineup": the offense's seven records put on their formation spots at the line marker (x -156, the ball's z,
    // going +x), screen x from z the way this camera has it, as if the dump were at the line (the drawn players stay).
    if (argc > 7 && std::string(argv[7]) == "lineup") {
        auto getf = [&](uint32_t a) { float v; std::memcpy(&v, &at(a), 4); return v; };
        auto putf = [&](uint32_t a, float v) { std::memcpy(&at(a), &v, 4); };
        float czs = 0;  // does screen x grow with z?
        for (uint32_t i = 0; i < 14; i++) czs += getf(0x802D1590 + i * 0xB98 + 8) * getf(0x802D1590 + i * 0xB98 + 0x750);
        const uint32_t record = at(0x802EA4AC + 4u * (uint32_t)side), form = at(record);
        const float ball_z = getf(0x802EA494);
        for (uint32_t s = 0; s < 7; s++) {
            const uint32_t p = 0x802D1590 + (uint32_t)(side * 7 + (int)s) * 0xB98;
            const float fx = getf(form + 0x14 * s), fz = getf(form + 0x14 * s + 4);
            putf(p, -156.0f + fx);
            putf(p + 8, ball_z + fz);
            putf(p + 0x750, 256.0f + (czs >= 0 ? fz : -fz));
        }
        fprintf(stderr, "lined up at x -156, z %.1f (screen x %s z)\n", ball_z, czs >= 0 ? "with" : "against");
    }
    std::vector<playart::Prim> prims;
    std::array<uint32_t, 3> colour = playart::colours(0);
    for (auto& c : colour) c = (c & 0xFFFFFF00u) | 208;
    playart::Art art = playart::build_art(rd, side, full ? playart::Cut::Full : playart::Cut::FirstBreak, playart::Style{}, colour, prims);
    fprintf(stderr, "phase %u, offense %d, record %08X flags %X: legs %d/%d/%d, %zu prims%s%s\n", at(0x802EA4A8), side, art.record,
            art.flags, art.legs[0], art.legs[1], art.legs[2], prims.size(), art.why ? ", none: " : "", art.why ? art.why : "");
    // The players' camera: its projection group (id B1xxxxx1), then the scene's group; the hook writes after both.
    uint32_t insert = 0;
    for (uint32_t a = dl; a < dl + size; a += 8) {
        if (at(a) == 0x6400000Cu && (at(a + 4) & 0xFF00000Fu) == 0xB1000001u) {
            insert = a + 32;
            break;
        }
    }
    if (insert == 0) return fprintf(stderr, "no second camera load in the list\n"), 1;
    const uint32_t tail = (dl + size + 7) & ~7u;
    std::vector<uint32_t> w = playart::build_list(tail, prims, playart::ListOptions{});
    if (w.empty()) return fprintf(stderr, "nothing to draw\n"), 1;
    if (check_list(w, tail, "dump list") == 0 || failures) return 1;
    w.push_back(at(insert));  // the command the call replaces, then back
    w.push_back(at(insert + 4));
    w.push_back(0xDF000000u);
    w.push_back(0);
    if (tail + w.size() * 4 > dl + 0x5DC0) return fprintf(stderr, "no room in the list's tail\n"), 1;
    for (size_t i = 0; i < w.size(); i++) at(tail + 4 * (uint32_t)i) = w[i];
    at(insert) = 0xDE000000u;
    at(insert + 4) = tail;
    std::ofstream o(argv[5], std::ios::binary);
    o.write(reinterpret_cast<const char*>(ram.data()), (std::streamsize)ram.size());
    fprintf(stderr, "spliced %zu bytes at %08X (called from %08X)\n", w.size() * 4, tail, insert);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--rdram") return splice(argc, argv);
    const char* path = argc > 1 ? argv[1] : "build/baserom.z64";
    const bool dump = argc > 2 && std::string(argv[2]) == "--dump";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        fprintf(stderr, "play_art_test: no ROM at %s (make rom-import)\n", path);
        return 2;
    }
    rom.assign(std::istreambuf_iterator<char>(f), {});
    if (rom.size() < 0x400000 || rom[0] != 0x80) {
        fprintf(stderr, "play_art_test: %s isn't a big-endian (z64) ROM\n", path);
        return 2;
    }
    playart::Read32 rd = read;
    const uint32_t ptrs = 0x80336B24;
    if (argc > 3 && std::string(argv[2]) == "--show") {  // a play's receivers, cut both ways (yards, offense frame)
        const uint32_t i = (uint32_t)atoi(argv[3]), record = u32(ptrs + 4 * i), form = u32(record);
        for (uint32_t s = 4; s < 7; s++) {
            const float fx = f32(form + 0x14 * s), fz = f32(form + 0x14 * s + 4);
            for (playart::Cut cut : {playart::Cut::FirstBreak, playart::Cut::Full}) {
                playart::Route r = playart::walk(rd, u32(record + 8 + 4 * s), {fx / playart::units_per_yard, fz / playart::units_per_yard}, fz < 0, cut);
                printf("slot %u %s %-9s %4.1f yd:", s, cut == playart::Cut::Full ? "full " : "first", r.ending, length(r));
                for (const auto& p : r.pts) printf(" (%.1f,%.1f)", p.d, p.l);
                printf("\n");
            }
        }
        return 0;
    }
    size_t biggest = 0;
    int drawn = 0;
    for (uint32_t i = 0; i < 45; i++) {
        const uint32_t record = u32(ptrs + 4 * i), form = u32(record);
        std::vector<playart::Prim> prims[2];
        for (int s = 0; s < 7; s++) {
            const float fx = f32(form + 0x14u * (uint32_t)s), fz = f32(form + 0x14u * (uint32_t)s + 4);
            const playart::Point start{fx / playart::units_per_yard, fz / playart::units_per_yard};
            const uint32_t script = u32(record + 8 + 4u * (uint32_t)s);
            playart::Route raw = playart::walk(rd, script, start, fz < 0, playart::Cut::Raw);
            if (dump) {
                printf("%u %d %s", i, s, raw.ending);
                for (const auto& p : raw.pts) printf(" %.3f %.3f", p.d, p.l);
                printf("\n");
            }
            if (std::string(raw.ending) == "unknown" || std::string(raw.ending) == "unreadable") {
                fail("play " + std::to_string(i) + " slot " + std::to_string(s) + ": script ends " + raw.ending);
            }
            for (int m = 0; m < 2; m++) {
                const playart::Cut cut = m ? playart::Cut::Full : playart::Cut::FirstBreak;
                playart::Route r = playart::walk(rd, script, start, fz < 0, cut);
                const float cap = m ? playart::full_cap : playart::first_break_cap;
                const std::string name = "play " + std::to_string(i) + " slot " + std::to_string(s) + (m ? " full" : " first");
                if (length(r) > cap + 0.01f) fail(name + ": longer than the cap");
                if ((int)r.pts.size() > playart::max_points) fail(name + ": too many points");
                if (r.pts.front().d != start.d || r.pts.front().l != start.l) fail(name + ": doesn't start at the spot");
                // The cut path follows the raw one: its first leg heads the same way.
                if (r.pts.size() > 1 && raw.pts.size() > 1) {
                    size_t k = 1;
                    while (k < raw.pts.size() && std::hypot(raw.pts[k].d - start.d, raw.pts[k].l - start.l) < 0.05f) k++;
                    if (k < raw.pts.size()) {
                        float ad = r.pts[1].d - start.d, al = r.pts[1].l - start.l, bd = raw.pts[k].d - start.d, bl = raw.pts[k].l - start.l;
                        float cosang = (ad * bd + al * bl) / (std::hypot(ad, al) * std::hypot(bd, bl));
                        if (cosang < 0.999f) fail(name + ": first leg differs from the raw path");
                    }
                }
                if (s >= 4 && r.pts.size() > 1) {
                    // Into the world as play_art.cpp does it (offense going +x, + across = +z), from a spot at the 30.
                    std::vector<std::array<float, 2>> world;
                    for (const auto& p : r.pts) world.push_back({-156.0f + p.d * playart::units_per_yard, p.l * playart::units_per_yard});
                    playart::ribbon(world, 0x3FE6A0D0u, playart::Style{}, prims[m]);
                }
            }
        }
        for (int m = 0; m < 2; m++) {
            playart::ListOptions o;
            o.use_ex = true;
            o.ex_group_push = {0x64000000u, 0xB4000000u, 0x64000000u, 0};
            o.ex_group_pop = {0x64000000u, 1};
            const uint32_t base = 0x80054250;
            std::vector<uint32_t> w = playart::build_list(base, prims[m], o);
            if (w.empty()) continue;
            drawn++;
            const std::string name = "play " + std::to_string(i) + (m ? " full list" : " first list");
            int tris = check_list(w, base, name);
            int want = 0;
            for (const auto& p : prims[m]) want += p.nt;
            if (tris != want) fail(name + ": " + std::to_string(tris) + " triangles of " + std::to_string(want));
            if (w.size() * 4 > 0xC00) fail(name + ": " + std::to_string(w.size() * 4) + " bytes, over play_art.cpp's 3 KB");
            biggest = std::max(biggest, w.size() * 4);
        }
    }
    fprintf(stderr, "play_art_test: 45 plays, %d lists, the biggest %zu bytes, %d failures\n", drawn, biggest, failures);
    return failures ? 1 : 0;
}
