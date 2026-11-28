// Per-set texture-coordinate overrides (uv_overrides.h, tools/uv_override.py), 2026-11-28: a league pak's set can wear
// a uniform part (the shoulder) with its own texture AND its own texture coordinates on the player models, because an
// art-only fit can't make some designs run across the stock chart (Pac-12: CSU's and USU's shoulder stripes).
//
// How the game draws a player's body: func_8027139C, per player, loads his jersey and calls his model's body display
// list (main, static: model descriptor + 8). Each body list has the part's block: a G_TEXTURE that selects the part's
// tile (the side's part texture, loaded into TMEM once per side), then per cap a G_MTX (the bone) and a G_VTX of
// model-space vertices whose S/T are the stock coordinates, then its triangles.
//
// Here, while a league pak with tables is active, each listed block's first command becomes a branch to segment 15
// (unused by the game) + the block's slot; each player gets segment 15 just before his body (the hook before his
// jersey load), pointing at the lists of the set he wears, or at stock lists (the block's own commands) for everyone
// else. A set's list: the G_TEXTURE, its texture and palette loaded over the part's (tile 2 TMEM, palette bank 2),
// the block's draws with each G_VTX pointing at a copy of the vertices with the table's S/T (model space, so before
// the bones move them), the side's own texture and palette loaded back, and a branch back to the block's end. A model
// the table doesn't list (the low-detail ones), and every other part, keeps the stock texture with the stock
// coordinates. Which set a side wears is read where the game dresses it (func_80256580's own part ids), so team
// select, the Uniform Picker, clash switching and kickoff all agree; leaving the pak puts the blocks back.
//
// Per-cap textures (CFL right sleeves): a set's list may also load another texture just before a cap's draw (the
// block's G_MTX naming that cap's bone, the table's "caps"), and its own (or the set's) texture again before the next
// cap's. A table whose coordinates are the stock ones changes nothing but those textures.
//
// RDRAM the game never uses (beside mixed_uniforms.cpp's 0x80600000..0x806D0800): 0x806E0000..0x80700000.
#if BLITZ_MODERN && BLITZ_ENGINE_ORIGINAL
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "blitz_savestate.h"
#include "blitz_soft_reset.h"
#include "uv_overrides.h"

namespace blitz {
    int32_t fs_file_index(const std::string& name);  // art_dropin.cpp
}

namespace {
    constexpr uint32_t region = 0x806E0000;
    // A list per listed block (stock lists, then each entry's: + mesh * stride); 80 commands hold a block of up to 25
    // with four texture loads (the set's, a cap's, the set's again, the side's own back).
    constexpr uint32_t stock_lists = 0x806E0100, list_stride = 0x280, max_meshes = 16;
    constexpr uint32_t vertex_area = 0x806E3000, vertex_end = 0x806E6000;
    constexpr uint32_t textures = 0x806E6000, textures_end = 0x806E8000;  // the sets' and caps' textures, one per file
    constexpr uint32_t entry_lists = 0x806E8000, entry_lists_end = 0x80700000, max_entries = 12;
    constexpr uint32_t dl_cursor = 0x8005EA78;  // the player renderer's display-list cursor (mixed_uniforms.cpp)
    constexpr uint32_t uni_home = 0x80720E80, uni_away = 0x80721380, uni_stride = 0x28;  // engine32_layout.h
    constexpr uint32_t side_buffer[2] = {0x002BE4B0, 0x002C4590};  // a side's uniform buffers (physical)
    constexpr uint32_t seg15 = 0xDB06003C, branch = 0xDE010000;
    // A part's place in a side's uniform buffer and TMEM (main func_80256580 / func_8027139C's side loads).
    struct PartLoad { uint32_t pixels, palette, tile_cmd[2], load_cmd[2], tmem_pal; };
    const PartLoad shoulder{0x4000, 0x4C40, {0xF5500040, 0x07090250}, {0xF3000000, 0x0707F400}, 0x120};

    struct State {
        int8_t side_entry[2] = {-1, -1};   // the override entry each side was last dressed in (-1: stock)
        int8_t last = -1;                  // the latest dress's (func_80256580)
        int8_t mixed[2][15];               // Pro Bowl players dressed in their own sets
        State() { std::memset(mixed, -1, sizeof(mixed)); }
    } st;
    [[maybe_unused]] const bool registered = blitz::savestate_register_pod("uv_overrides", &st);

    // Worked out from the pak once its files are registered (file indices), kept for the session.
    struct Mesh { uint32_t block, end; uint32_t original[2]; bool ok; };
    struct Built {
        bool done = false, logged_gap = false;
        const blitz::UvOverrides* ov = nullptr;
        std::vector<Mesh> meshes;               // every listed block, in slot order
        std::map<int32_t, int> entry_of_file;   // the stock part file's index -> entry
        std::vector<bool> entry_ok;
        std::vector<std::vector<bool>> cap_ok;  // per entry, per cap texture
        uint32_t entry_stride() const { return list_stride * (uint32_t)meshes.size(); }
    } b;
    // Restart in place reads the pack again (content_pack_tp.cpp): work it out again too.
    [[maybe_unused]] const bool soft_reset = blitz::on_soft_reset("uv overrides", [] { b = Built{}; });

    gpr guest(uint32_t a) { return (gpr)(int32_t)a; }
    uint32_t phys(uint32_t a) { return a & 0x1FFFFFFF; }
    uint32_t rd(uint8_t* rdram, uint32_t a) { return (uint32_t)MEM_W(0, guest(a)); }
    void wr(uint8_t* rdram, uint32_t a, uint32_t v) { MEM_W(0, guest(a)) = (int32_t)v; }
    uint16_t rd16(uint8_t* rdram, uint32_t a) { return (uint16_t)MEM_HU(0, guest(a)); }
    void wr16(uint8_t* rdram, uint32_t a, uint16_t v) { MEM_H(0, guest(a)) = (int16_t)v; }
    void wr8(uint8_t* rdram, uint32_t a, uint8_t v) { MEM_B(0, guest(a)) = (int8_t)v; }

    struct Writer {
        uint8_t* rdram;
        uint32_t p, limit;
        bool overflow = false;
        void put(uint32_t w0, uint32_t w1) {
            if (p + 8 > limit) { overflow = true; return; }
            wr(rdram, p, w0);
            wr(rdram, p + 4, w1);
            p += 8;
        }
        void load(uint32_t pixels, uint32_t palette, const PartLoad& part) {  // a CI4 part texture and its 16 colours
            put(0xE7000000, 0);
            put(0xFD500000, pixels);
            put(part.tile_cmd[0], part.tile_cmd[1]);
            put(0xE6000000, 0);
            put(part.load_cmd[0], part.load_cmd[1]);
            put(0xE7000000, 0);
            put(0xFD100000, palette);
            put(0xE8000000, 0);
            put(0xF5000000 | part.tmem_pal, 0x07000000);
            put(0xE6000000, 0);
            put(0xF0000000, 0x0703C000);
            put(0xE7000000, 0);
        }
    };

    // Is the block in RDRAM the one the table describes: its G_TEXTURE first, every G_VTX one of the listed buffers
    // (and each of those with the stock S/T the table recorded)?
    bool block_matches(uint8_t* rdram, const blitz::UvMesh& m, std::string& why) {
        if ((rd(rdram, m.block) >> 24) != 0xD7) { why = "no G_TEXTURE at the block"; return false; }
        if (m.end <= m.block || m.end - m.block > 8 * 48) { why = "block size"; return false; }
        for (uint32_t a = m.block + 8; a < m.end; a += 8) {
            uint32_t w0 = rd(rdram, a), w1 = rd(rdram, a + 4);
            if ((w0 >> 24) != 0x01) continue;
            bool listed = false;
            for (const auto& v : m.verts) listed |= v.at == w1 && ((w0 >> 12) & 0xFF) == v.st.size();
            if (!listed) { char s[64]; snprintf(s, sizeof s, "vertex load %08X not in the table", w1); why = s; return false; }
        }
        for (const auto& v : m.verts) {
            for (size_t i = 0; i < v.st.size(); i++) {
                uint32_t at = v.at + 16 * (uint32_t)i;
                if ((int16_t)rd16(rdram, at + 8) != v.st[i][0] || (int16_t)rd16(rdram, at + 10) != v.st[i][1]) {
                    char s[96]; snprintf(s, sizeof s, "vertex %zu at %08X: S/T differ from the table's stock ones", i, v.at); why = s;
                    return false;
                }
            }
        }
        return true;
    }

    // Copy a block's commands (G_TEXTURE excluded when `from_second`), each listed G_VTX pointing at `vtx_copy`.
    void copy_block(uint8_t* rdram, Writer& w, const Mesh& mesh, bool from_second, const std::map<uint32_t, uint32_t>* vtx_copy) {
        for (uint32_t a = mesh.block + (from_second ? 8 : 0); a < mesh.end; a += 8) {
            uint32_t w0 = a == mesh.block ? mesh.original[0] : rd(rdram, a);
            uint32_t w1 = a == mesh.block ? mesh.original[1] : rd(rdram, a + 4);
            if (vtx_copy && (w0 >> 24) == 0x01) {
                auto it = vtx_copy->find(w1);
                if (it != vtx_copy->end()) w1 = phys(it->second);
            }
            w.put(w0, w1);
        }
    }

    bool patched(uint8_t* rdram, const Mesh& m, size_t k) {
        return rd(rdram, m.block) == branch && rd(rdram, m.block + 4) == (0x0F000000u | (uint32_t)k * list_stride);
    }

    // "UVO2": the region holds lists (and may have patched blocks); "UVO1" was the layout before per-cap textures.
    constexpr uint32_t magic = 0x55564F32;

    // A .wms type 16 (32-byte header, CI4 pixels, 16 RGBA5551 colours): its pixel bytes, or 0 if it isn't one.
    uint32_t ci4_bytes(const std::vector<uint8_t>& d) {
        if (d.size() < 32 || d[7] != 16) return 0;
        uint32_t w_ = (uint32_t)d[8] << 24 | (uint32_t)d[9] << 16 | (uint32_t)d[10] << 8 | d[11];
        uint32_t h_ = (uint32_t)d[12] << 24 | (uint32_t)d[13] << 16 | (uint32_t)d[14] << 8 | d[15];
        uint32_t npix = w_ * h_ / 2;
        return npix && npix <= 0x400 && d.size() >= 32 + npix + 32 ? npix : 0;
    }

    // The lists, copies and textures in RDRAM, then the blocks patched. Again whenever a block isn't (a save state
    // from before, a new boot); the region is plain RDRAM, so a save state carries it.
    void install(uint8_t* rdram) {
        const blitz::UvOverrides& ov = *b.ov;
        // Vertex copies: per table, per listed buffer whose coordinates change (stock ones draw from the model's own).
        std::vector<std::map<uint32_t, uint32_t>> copies(ov.tables.size());
        uint32_t vp = vertex_area;
        for (size_t t = 0; t < ov.tables.size(); t++) {
            for (const auto& m : ov.tables[t].meshes) {
                for (const auto& v : m.verts) {
                    uint32_t bytes = 16 * (uint32_t)v.st.size();
                    if (v.uv == v.st || vp + bytes > vertex_end) continue;
                    for (uint32_t i = 0; i < bytes; i += 4) wr(rdram, vp + i, rd(rdram, v.at + i));
                    for (size_t i = 0; i < v.uv.size(); i++) {
                        wr16(rdram, vp + 16 * (uint32_t)i + 8, (uint16_t)v.uv[i][0]);
                        wr16(rdram, vp + 16 * (uint32_t)i + 10, (uint16_t)v.uv[i][1]);
                    }
                    copies[t][v.at] = vp;
                    vp += (bytes + 15) & ~15u;
                }
            }
        }
        // Stock lists: the block as it is.
        for (size_t k = 0; k < b.meshes.size(); k++) {
            Writer w{rdram, stock_lists + (uint32_t)k * list_stride, stock_lists + (uint32_t)(k + 1) * list_stride};
            copy_block(rdram, w, b.meshes[k], false, nullptr);
            w.put(branch, phys(b.meshes[k].end));
        }
        // The textures, one copy per file (sets may share a cap's texture).
        std::map<std::string, uint32_t> tex_at;
        uint32_t tp = textures;
        auto place = [&](const std::string& file, const std::vector<uint8_t>& d) -> uint32_t {
            auto it = tex_at.find(file);
            if (it != tex_at.end()) return it->second;
            uint32_t npix = ci4_bytes(d), bytes = (npix + 32 + 15) & ~15u;
            if (!npix || tp + bytes > textures_end) return 0;
            for (uint32_t i = 0; i < npix + 32; i++) wr8(rdram, tp + i, d[32 + i]);
            tex_at[file] = tp;
            tp += bytes;
            return tex_at[file];
        };
        const uint32_t own_pixels = 0x0C000000 | shoulder.pixels, own_palette = 0x0C000000 | shoulder.palette;
        // Each entry's lists.
        for (size_t e = 0; e < ov.entries.size() && e < max_entries; e++) {
            if (!b.entry_ok[e]) continue;
            const blitz::UvEntry& en = ov.entries[e];
            // Its texture (0: the set's own, already in TMEM) and each cap's, by the cap's G_MTX.
            uint32_t main = 0, main_pal = 0;
            if (!en.texture_file.empty()) {
                main = place(en.texture_file, en.texture);
                main_pal = main + ci4_bytes(en.texture);
            }
            std::map<uint32_t, std::array<uint32_t, 2>> cap_tex;
            for (size_t c = 0; c < en.caps.size(); c++) {
                if (!b.cap_ok[e][c]) continue;
                uint32_t at = place(en.caps[c].file, en.caps[c].texture);
                if (at) cap_tex[en.caps[c].mtx] = {at, at + ci4_bytes(en.caps[c].texture)};
            }
            const blitz::UvTable& table = ov.tables[en.table];
            for (size_t k = 0; k < b.meshes.size(); k++) {
                uint32_t at = entry_lists + (uint32_t)e * b.entry_stride() + (uint32_t)k * list_stride;
                Writer w{rdram, at, at + list_stride};
                const Mesh& mesh = b.meshes[k];
                bool mine = false;
                for (const auto& m : table.meshes) mine |= m.block == mesh.block;
                if (!mine || !mesh.ok) {  // not this table's model: stock
                    copy_block(rdram, w, mesh, false, nullptr);
                } else {
                    w.put(mesh.original[0], mesh.original[1]);  // the part's G_TEXTURE
                    if (main) w.load(phys(main), phys(main_pal), shoulder);
                    // What TMEM holds: 0 the set's (main, or the side's own), else a cap's texture.
                    uint32_t loaded = 0;
                    bool own = !main;
                    for (uint32_t a = mesh.block + 8; a < mesh.end; a += 8) {
                        uint32_t w0 = rd(rdram, a), w1 = rd(rdram, a + 4);
                        if ((w0 >> 24) == 0xDA) {  // G_MTX: a cap's draw starts
                            auto c = cap_tex.find(w1);
                            uint32_t want = c == cap_tex.end() ? 0 : c->first;
                            if (want != loaded) {
                                if (want) w.load(phys(c->second[0]), phys(c->second[1]), shoulder);
                                else if (main) w.load(phys(main), phys(main_pal), shoulder);
                                else w.load(own_pixels, own_palette, shoulder);
                                loaded = want;
                                own = !want && !main;
                            }
                        }
                        if ((w0 >> 24) == 0x01) {
                            auto it = copies[en.table].find(w1);
                            if (it != copies[en.table].end()) w1 = phys(it->second);
                        }
                        w.put(w0, w1);
                    }
                    if (!own) w.load(own_pixels, own_palette, shoulder);  // the side's own back
                }
                w.put(branch, phys(mesh.end));
                if (w.overflow) fprintf(stderr, "[uvo] list overflow: entry %zu block %08X\n", e, mesh.block);
            }
        }
        wr(rdram, region, magic);
        int n = 0;
        for (size_t k = 0; k < b.meshes.size(); k++) {
            if (!b.meshes[k].ok) continue;
            wr(rdram, b.meshes[k].block, branch);
            wr(rdram, b.meshes[k].block + 4, 0x0F000000u | (uint32_t)k * list_stride);
            n++;
        }
        static int installs = 0;
        if (installs++ < 4) fprintf(stderr, "[uvo] %d model block(s) take per-set coordinates or textures\n", n);
    }

    void work_out(uint8_t* rdram) {
        if (b.done) return;
        b.done = true;
        b.ov = blitz::league_uv_overrides();
        if (!b.ov || b.ov->entries.empty()) { b.ov = nullptr; return; }
        std::set<uint32_t> seen;
        for (const auto& t : b.ov->tables) {
            for (const auto& m : t.meshes) {
                if (seen.count(m.block) || b.meshes.size() >= max_meshes) continue;
                seen.insert(m.block);
                std::string why;
                bool ok = block_matches(rdram, m, why);
                b.meshes.push_back({m.block, m.end, {rd(rdram, m.block), rd(rdram, m.block + 4)}, ok});
                if (!ok) fprintf(stderr, "[uvo] gap: %s model %d block %08X keeps the stock texture and coordinates (%s)\n",
                                 t.name.c_str(), m.model, m.block, why.c_str());
            }
        }
        b.entry_ok.assign(b.ov->entries.size(), false);
        b.cap_ok.assign(b.ov->entries.size(), {});
        for (size_t e = 0; e < b.ov->entries.size(); e++) {
            const blitz::UvEntry& en = b.ov->entries[e];
            int32_t idx = blitz::fs_file_index(en.stock_file);
            bool tex = en.texture_file.empty() || (en.texture.size() >= 32 + 16 * 16 / 2 + 32 && en.texture[7] == 16);
            bool fits = entry_lists + (uint32_t)(e + 1) * b.entry_stride() <= entry_lists_end;
            if (e >= max_entries || !fits || idx < 0 || !tex || b.entry_of_file.count(idx)) {
                fprintf(stderr, "[uvo] %s %s: not used (%s)\n", en.team.c_str(), en.set.c_str(),
                        e >= max_entries || !fits ? "too many sets" : idx < 0 ? "its part file isn't added" : !tex ? "texture" : "part file shared");
                if (idx >= 0) b.entry_of_file[idx] = -1;  // a shared file: neither set
                continue;
            }
            // A cap's texture: CI4 at the set's texture's size (the part's tile is loaded for that size).
            std::string caps;
            uint32_t want = en.texture_file.empty() ? 32 * 16 / 2 : ci4_bytes(en.texture);
            for (const blitz::UvCapTexture& c : en.caps) {
                bool ok = ci4_bytes(c.texture) == want;
                b.cap_ok[e].push_back(ok);
                caps += ", " + c.cap + " cap " + (ok ? c.file : c.file + " not used (texture)");
            }
            b.entry_of_file[idx] = (int)e;
            b.entry_ok[e] = true;
            fprintf(stderr, "[uvo] %s %s: %s with %s's coordinates%s (keyed on %s)\n", en.team.c_str(), en.set.c_str(),
                    en.texture_file.empty() ? "its own shoulder" : en.texture_file.c_str(), b.ov->tables[en.table].name.c_str(),
                    caps.c_str(), en.stock_file.c_str());
        }
    }

    // Keep the blocks patched while a league with tables is active (and only then).
    bool ensure(uint8_t* rdram) {
        work_out(rdram);
        if (!b.ov) return false;
        bool all = true;
        for (size_t k = 0; k < b.meshes.size(); k++) {
            if (b.meshes[k].ok) all &= patched(rdram, b.meshes[k], k);
        }
        if (!all || rd(rdram, region) != magic) install(rdram);
        return true;
    }

    // The side whose uniform buffer the renderer last pointed segment 12 at in this display list.
    int frame_side(uint8_t* rdram) {
        uint32_t cur = rd(rdram, dl_cursor);
        if (cur < 0x80000400u || cur >= 0x80800000u) return -1;
        for (uint32_t n = 0, p = cur - 8; n < 4096 && p >= 0x80000400u; n++, p -= 8) {
            if (rd(rdram, p) != 0xDB060030u) continue;
            uint32_t base = phys(rd(rdram, p + 4));
            return base == side_buffer[0] ? 0 : base == side_buffer[1] ? 1 : -1;
        }
        return -1;
    }

    void emit_segment(uint8_t* rdram, uint32_t base) {
        uint32_t cur = rd(rdram, dl_cursor);
        wr(rdram, cur, seg15);
        wr(rdram, cur + 4, phys(base));
        wr(rdram, dl_cursor, cur + 8);
    }
}

namespace blitz {
    void uvo_mixed_dressed(int side, int slot) {
        if (side >= 0 && side < 2 && slot >= 0 && slot < 15) st.mixed[side][slot] = st.last;
    }
    void uvo_mixed_reset() { std::memset(st.mixed, -1, sizeof(st.mixed)); }
}

extern "C" {
    // func_80256580 (side, team, set) entry: the side is about to be dressed in the parts the game's tables name.
    void blitz_uvo_dress(uint8_t* rdram, int32_t side, int32_t team, int32_t set) {
        work_out(rdram);
        int e = -1;
        if (b.ov && team >= 0 && team < 64) {
            uint32_t id = rd(rdram, (set ? uni_away : uni_home) + (uint32_t)team * uni_stride + 4 * 4);  // part 4: shoulder
            if ((id >> 24) == 0x0A) {
                auto it = b.entry_of_file.find((int32_t)(id & 0xFFFFFF) - 1);
                if (it != b.entry_of_file.end()) e = it->second;
            }
        }
        st.last = (int8_t)e;
        if (side < 0 || side > 1 || st.side_entry[side] == e) return;
        st.side_entry[side] = (int8_t)e;
        if (b.ov) {
            fprintf(stderr, "[uvo] side %d dressed: %s\n", side,
                    e < 0 ? "stock coordinates" : (b.ov->entries[(size_t)e].team + " " + b.ov->entries[(size_t)e].set).c_str());
        }
    }

    // func_8027139C 0x802723E8, before a player's jersey load (his body list follows): segment 15 for his body.
    void blitz_uvo_player(uint8_t* rdram, int32_t entry) {
        if (!ensure(rdram)) {
            // No tables this session: a save state made with them may still have blocks branching to segment 15;
            // its stock lists came with it (plain RDRAM), so those blocks draw as the game's own.
            uint32_t m = rd(rdram, region);
            if (m == magic) emit_segment(rdram, stock_lists);
            else if (m == 0x55564F31) emit_segment(rdram, 0x806E1000);  // "UVO1", a state from before: its stock lists
            return;
        }
        int e = -1, side, slot;
        if (blitz::mixed_player_of(rdram, entry, side, slot)) e = st.mixed[side][slot];
        else if ((side = frame_side(rdram)) >= 0) e = st.side_entry[side];
        bool ok = e >= 0 && (size_t)e < b.entry_ok.size() && b.entry_ok[(size_t)e];
        emit_segment(rdram, ok ? entry_lists + (uint32_t)e * b.entry_stride() : stock_lists);
    }
}
#else
#include <cstdint>
namespace blitz {
    void uvo_mixed_dressed(int, int) {}
    void uvo_mixed_reset() {}
}
extern "C" void blitz_uvo_dress(uint8_t*, int32_t, int32_t, int32_t) {}
extern "C" void blitz_uvo_player(uint8_t*, int32_t) {}
#endif
