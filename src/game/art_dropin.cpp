// Art drop-in for the 2026 flavour (docs/BLITZ2026.md "Art drop-in").
//
// The user can put a ROM with team+'s code but other art (e.g. their own copy of a community
// update) in <app folder>/art/. At startup its asset file system (ROM 0x150C40 to the end: every
// image and sound file plus the TOC) replaces ours in the in-memory ROM image, and each franchise's
// 30 asset indices (uniform parts, logos, title boxes, endzones, win text, VS helmets) are taken
// from that ROM's matching team block (content_pack_tp.cpp applies them after each ovl_b load).
// Code, team order, rosters and everything else stay ours. Nothing is written to disk.
#if BLITZ_MODERN
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "recomp.h"
#include "util/file.h"
#include "librecomp/game.hpp"
#if BLITZ_MODERN
#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"
#include "season_pak.h"
#endif

namespace blitz {
    bool rom_rebranded();  // below
    bool rom_data_rebranded(std::span<const uint8_t> rom);
    void post_notice(const std::string& msg);                   // notices.cpp
    void fs_register_exact(const std::string& name, std::vector<uint8_t> data);  // below: a new file by its own name
    bool teamplus_rom_check(const std::vector<uint8_t>& rom);   // main.cpp
    std::optional<std::string> franchise_of_nickname(const std::string& nickname);  // content_pack_tp.cpp
    std::filesystem::path active_pack_dir();                    // content_pack_tp.cpp
}

namespace {
    constexpr size_t rom_size = 0x1000000, fs_start = 0x150C40;
    constexpr size_t block_rom = 0x11F17C, block_size = 0x220, max_blocks = 60;
    // Asset index ranges inside a team block: uniform parts (20 u16), menu art (4 u16), endzones,
    // win text and VS helmets (6 u16).
    constexpr std::array<std::pair<uint32_t, uint32_t>, 3> asset_ranges = {{{0x180, 0x1A8}, {0x1D8, 0x1E0}, {0x214, 0x220}}};
    constexpr uint32_t t_name = 0x1E0;

    // The file system's TOC entries: team+'s 'zs' (32 bytes: name[0x14], size, offset, packed size or 0) or the
    // original game's 'fs' (40 bytes: name[0x18], size, offset, method 5 stored / 4 LZO, packed or stored size).
    struct FsFormat {
        bool zs;
        size_t es() const { return zs ? 32 : 40; }
        size_t ns() const { return zs ? 0x14 : 0x18; }
    };
    uint32_t be_at(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
    void put_at(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - 8 * i)); }
    // An entry's size, offset and packed size (0: stored).
    void entry_fields(const uint8_t* e, FsFormat f, uint32_t& usize, uint32_t& off, uint32_t& psize) {
        if (f.zs) { usize = be_at(e + 0x14); off = be_at(e + 0x18); psize = be_at(e + 0x1C); }
        else { usize = be_at(e + 0x18); off = be_at(e + 0x1C); psize = be_at(e + 0x20) == 4 ? be_at(e + 0x24) : 0; }
    }
    // The entry as a stored file of `usize` bytes at `off`.
    void set_entry(uint8_t* e, FsFormat f, uint32_t usize, uint32_t off) {
        if (f.zs) { put_at(e + 0x14, usize); put_at(e + 0x18, off); put_at(e + 0x1C, 0); }
        else { put_at(e + 0x18, usize); put_at(e + 0x1C, off); put_at(e + 0x20, 5); put_at(e + 0x24, usize); }
    }

    std::map<std::string, std::vector<uint8_t>> art_assets;  // franchise -> concatenated asset bytes (big-endian)
    std::vector<std::vector<uint8_t>> extra_files;           // 32-byte 'zs' TOC entries for names the game lacks
    constexpr uint32_t extra_toc = 0x80408000;               // where they live in RDRAM (unused by the game)
    constexpr int32_t extra_index = 0x7000;                  // lookup result for extra k: extra_index + k
    std::string art_label;
    std::map<std::string, int> full_name_ticks;  // announcer bank -> its full-name call's length (effect 2)

    // A libmus effect's note length in ticks (about 1/96 s): effect `index` of a .bfx, or -1.
    int effect_ticks(const std::vector<uint8_t>& b, uint32_t index) {
        auto be32 = [&](size_t o) { return o + 4 <= b.size() ? (uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | b[o + 3] : 0u; };
        if (index >= be32(0)) return -1;
        size_t i = be32(0x18 + 8 * index);
        static const std::map<uint8_t, int> args = {{0x81, 1}, {0x84, 7}, {0x9C, 1}, {0xA6, 1}, {0x8B, 1}, {0x8D, 1}, {0xA2, 1}, {0x85, 1}, {0x86, 1}};
        while (i < b.size()) {
            uint8_t c = b[i];
            if (c < 0x80) {  // note, then its length: 1 byte, or 2 with bit 7 set
                if (i + 1 >= b.size()) return -1;
                int len = b[i + 1];
                if (len & 0x80) len = i + 2 < b.size() ? ((len & 0x7F) << 8) | b[i + 2] : -1;
                return len;
            }
            auto it = args.find(c);
            if (it == args.end()) return -1;  // stop (0x80) or a command these banks don't use
            i += 1 + it->second;
        }
        return -1;
    }

    std::optional<std::vector<uint8_t>> read_rom(const std::filesystem::path& p) {
        std::ifstream f{p, std::ios::binary};
        std::vector<uint8_t> d{std::istreambuf_iterator<char>(f), {}};
        if (d.size() != rom_size) return std::nullopt;
        auto swap = [&](int mode) {
            for (size_t i = 0; i + 3 < d.size(); i += 4) {
                if (mode == 2) { std::swap(d[i], d[i + 1]); std::swap(d[i + 2], d[i + 3]); }
                else { std::swap(d[i], d[i + 3]); std::swap(d[i + 1], d[i + 2]); }
            }
        };
        if (d[0] == 0x37 && d[1] == 0x80) swap(2);        // .v64
        else if (d[0] == 0x40 && d[1] == 0x12) swap(4);   // .n64
        else if (!(d[0] == 0x80 && d[1] == 0x37)) return std::nullopt;
        return d;
    }

    std::filesystem::path find_art_rom() {
        if (const char* env = getenv("BLITZ_ART_ROM"); env != nullptr && *env != '\0') {
            return env;
        }
        std::filesystem::path dir = recomp::get_config_path() / "art";
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return {};
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            std::string ext = e.path().extension().string();
            if (ext == ".z64" || ext == ".n64" || ext == ".v64") return e.path();
        }
        return {};
    }
}

namespace blitz {
    // Single game files from a folder, by file name: the data is appended past the end of the ROM
    // image and the file's 'zs' TOC entry points at it as a stored (uncompressed) file. A name the game
    // doesn't have yet (a new announcer bank, e.g. texans.wbk) is added to the TOC, which is rebuilt
    // sorted past the ROM end (the game binary-searches it by lowercase name). Returns the count.
    //   <app folder>/art/files/<name>.wms        images (tools/wms.py)
    //   <app folder>/announcer/<bank>.wbk/.ptr/.bfx  announcer banks (tools/announcer.py, docs/ANNOUNCER.md)
#if BLITZ_MODERN
    // ---- The NFL Blitz 2027 logo in the game's own images (Modern Blitz) ----
    // The team's marks are bundled as overlays fitted to each slot (Resources/art/logo27, tools/make_logo2027.py
    // --overlays). At startup they're laid over this player's own title, New Features and badge images from
    // their ROM, so every copy of the app shows the 2027 logo without shipping any game art. A file of the same
    // name in art/files still wins.

    // LZO1X decompression (as tools/announcer/lzo.py): the ROM's file system stores most files packed.
    bool lzo1x_decompress(const uint8_t* src, size_t n, size_t out_len, std::vector<uint8_t>& dst) {
        dst.clear();
        dst.reserve(out_len);
        size_t ip = 0;
        int state = 0;
        auto lits = [&](size_t k) {
            if (ip + k > n || dst.size() + k > out_len) return false;
            dst.insert(dst.end(), src + ip, src + ip + k);
            ip += k;
            return true;
        };
        auto byte = [&](uint8_t& b) {
            if (ip >= n) return false;
            b = src[ip++];
            return true;
        };
        if (n == 0) return false;
        if (src[0] > 17) {
            size_t t = src[0] - 17;
            ip = 1;
            if (!lits(t)) return false;
            state = t < 4 ? (int)t : 4;
        }
        while (ip < n) {
            uint8_t b0;
            if (!byte(b0)) return false;
            size_t t = b0, length = 0;
            long m = 0;
            int next = 0;
            uint8_t b = 0;
            if (t < 16) {
                if (state == 0) {
                    if (t == 0) {
                        while (ip < n && src[ip] == 0) { t += 255; ip++; }
                        if (!byte(b)) return false;
                        t += 15 + b;
                    }
                    if (!lits(t + 3)) return false;
                    state = 4;
                    continue;
                }
                next = (int)(t & 3);
                if (!byte(b)) return false;
                if (state != 4) {
                    m = (long)dst.size() - 1 - (long)(t >> 2) - ((long)b << 2);
                    length = 2;
                } else {
                    m = (long)dst.size() - (1 + 0x800) - (long)(t >> 2) - ((long)b << 2);
                    length = 3;
                }
            } else if (t >= 64) {
                next = (int)(t & 3);
                if (!byte(b)) return false;
                m = (long)dst.size() - 1 - (long)((t >> 2) & 7) - ((long)b << 3);
                length = (t >> 5) + 1;
            } else if (t >= 32) {
                length = t & 31;
                if (length == 0) {
                    while (ip < n && src[ip] == 0) { length += 255; ip++; }
                    if (!byte(b)) return false;
                    length += 31 + b;
                }
                length += 2;
                if (ip + 2 > n) return false;
                uint32_t v = src[ip] | (src[ip + 1] << 8);
                ip += 2;
                m = (long)dst.size() - 1 - (long)(v >> 2);
                next = (int)(v & 3);
            } else {
                m = (long)dst.size() - (long)((t & 8) << 11);
                length = t & 7;
                if (length == 0) {
                    while (ip < n && src[ip] == 0) { length += 255; ip++; }
                    if (!byte(b)) return false;
                    length += 7 + b;
                }
                length += 2;
                if (ip + 2 > n) return false;
                uint32_t v = src[ip] | (src[ip + 1] << 8);
                ip += 2;
                m -= (long)(v >> 2);
                next = (int)(v & 3);
                if (m == (long)dst.size()) break;  // end of stream
                m -= 0x4000;
            }
            if (m < 0 || dst.size() + length > out_len) return false;
            for (size_t k = 0; k < length; k++) dst.push_back(dst[(size_t)m + k]);
            state = next;
            if (!lits((size_t)next)) return false;
        }
        return dst.size() == out_len;
    }

    struct Image8 {  // a type-8 .wms (RGBA5551) as RGBA8
        uint8_t flags = 0;
        int32_t w = 0, h = 0, ox = 0, oy = 0;
        std::vector<uint8_t> px;
        uint8_t* at(int x, int y) { return &px[((size_t)y * w + x) * 4]; }
    };

    uint32_t rd32(const std::vector<uint8_t>& d, size_t o) { return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3]; }

    bool decode_wms8(const std::vector<uint8_t>& d, Image8& img) {
        if (d.size() < 32 || d[0] != 'w' || d[1] != 'n' || rd32(d, 4) != 8) return false;
        img.flags = d[3];
        img.w = (int32_t)rd32(d, 8);
        img.h = (int32_t)rd32(d, 12);
        img.ox = (int32_t)rd32(d, 16);
        img.oy = (int32_t)rd32(d, 20);
        if (img.w <= 0 || img.h <= 0 || img.w > 1024 || img.h > 1024 || d.size() < 32 + (size_t)img.w * img.h * 2) return false;
        img.px.resize((size_t)img.w * img.h * 4);
        for (size_t i = 0; i < (size_t)img.w * img.h; i++) {
            uint16_t v = (uint16_t)(d[32 + 2 * i] << 8 | d[33 + 2 * i]);
            img.px[4 * i + 0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
            img.px[4 * i + 1] = (uint8_t)(((v >> 6) & 31) * 255 / 31);
            img.px[4 * i + 2] = (uint8_t)(((v >> 1) & 31) * 255 / 31);
            img.px[4 * i + 3] = (v & 1) ? 255 : 0;
        }
        return true;
    }

    std::vector<uint8_t> encode_wms8(const Image8& img) {
        std::vector<uint8_t> d(32 + (size_t)img.w * img.h * 2, 0);
        d[0] = 'w'; d[1] = 'n'; d[2] = 2; d[3] = img.flags;
        auto w32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) d[o + i] = (uint8_t)(v >> (24 - 8 * i)); };
        w32(4, 8); w32(8, (uint32_t)img.w); w32(12, (uint32_t)img.h); w32(16, (uint32_t)img.ox); w32(20, (uint32_t)img.oy);
        for (size_t i = 0; i < (size_t)img.w * img.h; i++) {
            const uint8_t* p = &img.px[4 * i];
            uint16_t v = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 3) << 6 | (p[2] >> 3) << 1 | (p[3] >= 128 ? 1 : 0));
            d[32 + 2 * i] = (uint8_t)(v >> 8);
            d[33 + 2 * i] = (uint8_t)v;
        }
        return d;
    }

    struct Overlay {
        int x = 0, y = 0, w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };

    bool read_overlay(const std::filesystem::path& path, Overlay& o) {
        std::ifstream f{path, std::ios::binary};
        std::vector<uint8_t> d{std::istreambuf_iterator<char>(f), {}};
        if (d.size() < 12 || memcmp(d.data(), "LOV1", 4) != 0) return false;
        o.x = d[4] << 8 | d[5]; o.y = d[6] << 8 | d[7]; o.w = d[8] << 8 | d[9]; o.h = d[10] << 8 | d[11];
        if (d.size() != 12 + (size_t)o.w * o.h * 4) return false;
        o.rgba.assign(d.begin() + 12, d.end());
        return true;
    }

    // Alpha over, as PIL's paste with the art as its own mask.
    void paste(Image8& img, const Overlay& o) {
        for (int y = 0; y < o.h; y++) {
            for (int x = 0; x < o.w; x++) {
                int tx = o.x + x, ty = o.y + y;
                if (tx < 0 || ty < 0 || tx >= img.w || ty >= img.h) continue;
                const uint8_t* s = &o.rgba[((size_t)y * o.w + x) * 4];
                uint8_t* t = img.at(tx, ty);
                int a = s[3];
                for (int c = 0; c < 4; c++) t[c] = (uint8_t)((s[c] * a + t[c] * (255 - a) + 127) / 255);
            }
        }
    }

    // The N64 keeps 1-bit alpha: faint edge pixels go, soft ones are darkened instead of haloing.
    void one_bit(Image8& img) {
        for (size_t i = 0; i < img.px.size(); i += 4) {
            uint8_t* p = &img.px[i];
            if (p[3] < 110) { p[0] = p[1] = p[2] = p[3] = 0; }
            else if (p[3] < 255) {
                float k = p[3] / 255.0f;
                for (int c = 0; c < 3; c++) p[c] = (uint8_t)(p[c] * k);
                p[3] = 255;
            }
        }
    }

    // New Features: erase the 2000 logo in the top right (tools/make_logo2027.py from_rom has the reasoning).
    void erase_new_features_logo(Image8& img) {
        if (img.w != 512 || img.h != 256) return;
        Image8 orig = img;
        for (int x = 300; x < 512; x++) {
            const uint8_t* base = orig.at(x, 84);
            for (int y = 0; y < 84; y++) {
                float w;
                if (y < 29) w = std::min(1.0f, std::max(0.0f, (x - 300) / 24.0f));
                else if (x < 320) continue;
                else w = std::min(1.0f, std::max(0.0f, (x - 336) / 32.0f));
                const uint8_t* o = orig.at(x, y);
                bool sat = std::max({o[0], o[1], o[2]}) - std::min({o[0], o[1], o[2]}) > 40;
                float ww = (sat && x >= 320) ? 1.0f : w;
                uint8_t* t = img.at(x, y);
                for (int c = 0; c < 3; c++) t[c] = (uint8_t)(int)(o[c] * (1 - ww) + base[c] * ww);
                t[3] = 255;
            }
        }
    }

    // name -> the new image, built from the ROM's own one. Empty when the overlays aren't bundled. Only images
    // that are still the Team Extension's own (XXH3-64 of the unpacked file) are touched, and none at all when
    // the title screen isn't (`rebranded`): a community release such as Resurrection's Gold or Silver Edition
    // keeps its own branding everywhere.
    // BRANDING: BLITZ '27 logo overlays (Resources/art/logo27)
    std::vector<std::pair<std::string, std::vector<uint8_t>>> logo27_files(const std::vector<uint8_t>& rom, size_t toc, uint32_t count,
                                                                         FsFormat fmt, bool& rebranded) {
        rebranded = false;
        static const std::pair<const char*, uint64_t> originals[] = {
            {"blitztit", 0xee0f77d4cb48c48fULL}, {"nf_bg", 0x82c931b9f40fc441ULL}, {"blitzmm", 0x2bc25f3634c399b6ULL},
            {"blitz_logo", 0xc1bae2179b70204eULL}, {"blitzlogo", 0x150eb69ce8975939ULL}, {"blitz2000", 0x3ce3c08e79d6a031ULL}};
        std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
        std::filesystem::path dir = recompui::file::get_program_path() / "art" / "logo27";
        for (auto [name, original] : originals) {
            Overlay ov;
            if (!read_overlay(dir / (std::string(name) + ".ovl"), ov)) continue;
            std::string file = std::string(name) + ".wms";
            const uint8_t* e = nullptr;
            for (uint32_t i = 0; i < count; i++) {
                const uint8_t* cand = rom.data() + toc + (size_t)i * fmt.es();
                if (strnlen(reinterpret_cast<const char*>(cand), fmt.ns()) == file.size() && memcmp(cand, file.data(), file.size()) == 0) { e = cand; break; }
            }
            if (e == nullptr) continue;
            uint32_t usize, off, psize;
            entry_fields(e, fmt, usize, off, psize);
            size_t at = fs_start + off, stored = psize ? psize : usize;
            if (usize > (1u << 20) || at + stored > rom.size()) continue;
            std::vector<uint8_t> data;
            if (psize == 0) data.assign(rom.begin() + at, rom.begin() + at + usize);
            else if (!lzo1x_decompress(rom.data() + at, psize, usize, data)) continue;
            if (XXH3_64bits(data.data(), data.size()) != original) {
                fprintf(stderr, "[art] %s is the ROM's own art: no 2027 logo over it\n", file.c_str());
                if (std::string(name) == "blitztit") {
                    rebranded = true;
                    fprintf(stderr, "[art] a rebranded ROM: its own logos stay\n");
                    return {};
                }
                continue;
            }
            Image8 img;
            if (!decode_wms8(data, img)) continue;
            std::string n = name;
            if (n == "nf_bg") erase_new_features_logo(img);
            if (n == "blitzmm" || n == "blitz_logo" || n == "blitzlogo" || n == "blitz2000") {
                // Clear the inside to the badge's own ground, then the mark.
                uint8_t ground[4] = {0, 0, 0, 0};
                if (n != "blitz_logo") memcpy(ground, img.at(img.w / 2, 1), 4);
                int inset = n == "blitzmm" ? 3 : 0;
                for (int y = inset; y < img.h - inset; y++)
                    for (int x = inset; x < img.w - inset; x++) memcpy(img.at(x, y), ground, 4);
            }
            paste(img, ov);
            if (n == "blitz_logo") one_bit(img);
            out.emplace_back(file, encode_wms8(img));
        }
        return out;
    }
#endif

    size_t apply_file_overrides(std::vector<uint8_t>& rom) {
        auto be32 = [&](size_t o) { return (uint32_t)rom[o] << 24 | (uint32_t)rom[o + 1] << 16 | (uint32_t)rom[o + 2] << 8 | rom[o + 3]; };
        const uint32_t magic = be32(fs_start) >> 16;
        if (magic != 0x7A73 && magic != 0x6673) return 0;  // 'zs' (team+) or 'fs' (the original game)
        const FsFormat fmt{magic == 0x7A73};

        // The file system comes from the ROM (or an art ROM) and only its 'zs' magic is checked, so a damaged
        // one must not send these reads and writes outside the image.
        size_t toc = fs_start + be32(fs_start + 8);
        uint32_t count = be32(fs_start + 12);
        if (count > 0x10000 || toc < fs_start || toc + (size_t)count * fmt.es() > rom.size()) {
            post_notice("The game's file list looks damaged, so the art and announcer files weren't loaded.");
            return 0;
        }

        struct File { std::string name; std::vector<uint8_t> data; };
        std::vector<File> files;
        bool rebranded = false;
#if BLITZ_MODERN
        for (auto& [name, data] : logo27_files(rom, toc, count, fmt, rebranded)) files.push_back({name, std::move(data)});
#endif
        auto collect = [&](const std::filesystem::path& dir, std::initializer_list<const char*> exts) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) return;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                std::string ext = e.path().extension().string(), name = e.path().filename().string();
                for (char& c : name) c = (char)std::tolower((unsigned char)c);
                for (char& c : ext) c = (char)std::tolower((unsigned char)c);  // LOGO.WMS too
                if (std::none_of(exts.begin(), exts.end(), [&](const char* x) { return ext == x; })) continue;
                if (name.rfind("._", 0) == 0) continue;  // macOS's AppleDouble copies (a folder copied from a Mac), not game files
                if (name.size() >= 0x14) {
                    post_notice("\"" + name + "\" has too long a name for the game's file system (19 characters at most); skipped.");
                    continue;
                }
                std::ifstream f{e.path(), std::ios::binary};
                std::vector<uint8_t> data{std::istreambuf_iterator<char>(f), {}};
                auto starts = [&](const char* magic) { return data.size() >= strlen(magic) && memcmp(data.data(), magic, strlen(magic)) == 0; };
                bool ok = ext == ".wms" ? data.size() >= 32 && starts("wn")
                        : ext == ".wbk" ? starts("N64 WaveTables")
                        : ext == ".ptr" ? starts("N64 PtrTablesV2")
                        : data.size() >= 0x18;  // .bfx: a libmus effect bank (no magic)
                if (!ok) {
                    post_notice("\"" + name + "\" isn't a game file of that type (see tools/wms.py / tools/announcer.py); skipped.");
                    continue;
                }
                if (ext == ".bfx") {
                    int ticks = effect_ticks(data, 2);
                    if (ticks > 0) full_name_ticks[name.substr(0, name.size() - 4)] = ticks;
                }
                files.push_back({name, std::move(data)});
            }
        };
        // The app's own art and announcer first (Resources: the New Blitz 2027 logo, and in a private build Modern's
        // team art from make app-modern; then the installed 2026 Season pak's, season_pak.h), then the user's, which
        // wins for a file of the same name.
        for (const std::filesystem::path& root : blitz::content_dirs()) {
            if (!rebranded) collect(root / "art" / "files", {".wms"});
        }
        collect(recomp::get_config_path() / "art" / "files", {".wms"});
        for (const std::filesystem::path& root : blitz::content_dirs()) {
            if (!rebranded) collect(root / "announcer", {".wbk", ".ptr", ".bfx"});
        }
        collect(recomp::get_config_path() / "announcer", {".wbk", ".ptr", ".bfx"});
#if BLITZ_MODERN
        // A league pak's own images (named after its base teams' files), last so they win.
        if (std::filesystem::path pack = active_pack_dir(); !pack.empty()) collect(pack / "files", {".wms"});
#endif
        for (size_t i = 0; i < files.size(); i++) {
            for (size_t j = i + 1; j < files.size(); j++) {
                if (files[j].name == files[i].name) { files.erase(files.begin() + i); i--; break; }
            }
        }
        if (files.empty()) return 0;
        // By name, whichever folder each came from (2026-11-13): the order decides where each file's data lands in the
        // image and a new name's index, so the same files make the same image whether they came from a private build's
        // Resources or from the season pak, and on every file system (a folder's listing order is the disk's own).
        std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.name < b.name; });

        const size_t es = fmt.es();
        std::vector<std::vector<uint8_t>> entries(count);
        for (uint32_t i = 0; i < count; i++) entries[i].assign(rom.begin() + toc + i * es, rom.begin() + toc + i * es + es);
        auto entry_name = [&](const std::vector<uint8_t>& e) { return std::string(reinterpret_cast<const char*>(e.data()), strnlen(reinterpret_cast<const char*>(e.data()), fmt.ns())); };
        for (File& f : files) {
            auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return entry_name(e) == f.name; });
            bool is_image = f.name.size() > 4 && f.name.substr(f.name.size() - 4) == ".wms";
            if (it == entries.end() && is_image) {
                post_notice("Art file \"" + f.name + "\" doesn't match any game file; skipped.");
                continue;
            }
            if (it == entries.end() && !fmt.zs) {
                // The original game: a new name (your own announcer bank) goes on the end of the TOC with the files
                // added at startup (fs_commit_files); a lookup hook finds names past the sorted ones.
                blitz::fs_register_exact(f.name, std::move(f.data));
                fprintf(stderr, "[art] %s as a new file\n", f.name.c_str());
                continue;
            }
            rom.resize((rom.size() + 15) & ~size_t(15));
            size_t at = rom.size();
            rom.insert(rom.end(), f.data.begin(), f.data.end());
            if (it == entries.end()) {
                entries.emplace_back(es, 0);
                it = entries.end() - 1;
                memcpy(it->data(), f.name.data(), f.name.size());
            }
            set_entry(it->data(), fmt, (uint32_t)f.data.size(), (uint32_t)(at - fs_start));
            fprintf(stderr, "[art] %s from %s\n", f.name.c_str(), it - entries.begin() >= (long)count ? "a new file" : "a replacement");
        }
        // The TOC itself never changes size or order: images are found by their TOC index, so an inserted
        // name would shift every later image. New names are kept apart (extra_files) and found by the
        // hooks in the game's lookup (blitz_fs_*, tools/teamplus.py DROPIN).
        for (uint32_t i = 0; i < count; i++) std::copy(entries[i].begin(), entries[i].begin() + es, rom.begin() + toc + i * es);
        for (size_t i = count; i < entries.size(); i++) extra_files.push_back(entries[i]);
        return files.size();
    }

    // At on_init: swap in the drop-in's file system and single-file overrides (before the game's
    // fs_init reads the file system).
    void art_init() {
        (void)blitz::rom_rebranded();  // decided on the ROM as loaded, before any art below replaces its title
        auto cur = recomp::get_rom();
        if (cur.size() != rom_size) return;
        // Room for the files apply_file_overrides adds (pages are only used once written): growing the copy would
        // move it with the game's image still held, three ROMs at once.
        std::vector<uint8_t> merged;
        merged.reserve(2 * cur.size());
        merged.assign(cur.begin(), cur.end());
        bool changed = false;

#if BLITZ_TEAMPLUS  // a community ROM's art: the Team Extension's file system and team blocks
        std::filesystem::path file = find_art_rom();
        if (!file.empty()) {
            auto art = read_rom(file);
            if (!art || !teamplus_rom_check(*art)) {
                post_notice("Art file \"" + file.filename().string() + "\" wasn't used: it isn't a 16 MB NFL Blitz 2000 ROM with the Team Extension's code. Playing with the standard art.");
            }
            else {
                std::memcpy(merged.data() + fs_start, art->data() + fs_start, rom_size - fs_start);
                for (size_t t = 0; t < max_blocks; t++) {
                    const uint8_t* b = art->data() + block_rom + t * block_size;
                    if ((int8_t)b[0] < 0) break;  // first word negative = end of the team list
                    std::string nick(reinterpret_cast<const char*>(b + t_name), strnlen(reinterpret_cast<const char*>(b + t_name), 16));
                    auto f = franchise_of_nickname(nick);
                    if (!f) continue;
                    std::vector<uint8_t> assets;
                    for (auto [lo, hi] : asset_ranges) assets.insert(assets.end(), b + lo, b + hi);
                    // Community art draws its right VS helmets facing the same way as the left ones (made for
                    // team+'s always-mirror bug, which our build fixes). Use the left image for both, so
                    // the game mirrors it for the right side. (VS left/right ids are the last two u16.)
                    size_t n = assets.size();
                    assets[n - 2] = assets[n - 4];
                    assets[n - 1] = assets[n - 3];
                    art_assets[*f] = std::move(assets);
                }
                art_label = file.filename().string();
                changed = true;
                fprintf(stderr, "[art] using %s: file system and %zu teams' art\n", art_label.c_str(), art_assets.size());
            }
        }
#endif
        size_t files = apply_file_overrides(merged);
        if (files > 0) {
            changed = true;
            fprintf(stderr, "[art] %zu game file(s) from art/files and announcer/\n", files);
        }
        if (changed) {
            recomp::set_rom_contents(std::move(merged));
        }
    }

    bool art_active() { return !art_assets.empty(); }

    // The VS screen waits a per-team time after the first team's full name (team block +0x1D6), set
    // for the original calls: about 0.54 frames per tick. A replaced call gets a pause to match, or
    // the second team's name cuts it off. -1 when the bank isn't replaced.
    int announcer_vs_pause(const std::string& bank) {
        auto it = full_name_ticks.find(bank);
        return it == full_name_ticks.end() ? -1 : std::min(255, (it->second * 56 + 99) / 100 + 2);
    }

    // At on_init: the extra TOC entries into RDRAM for the lookup hooks.
    void art_install_extras(uint8_t* rdram) {
        for (size_t k = 0; k < extra_files.size(); k++) {
            for (uint32_t i = 0; i < 32; i++) MEM_B(0, (gpr)(int32_t)(extra_toc + 32 * k + i)) = (int8_t)extra_files[k][i];
        }
    }

    // Overwrite a team block's asset indices with the drop-in's for that franchise (block in RDRAM).
    // A franchise's art from the drop-in ROM (its block's asset ids, team_store.h Team::set_art), or nothing.
    const std::vector<uint8_t>* art_assets_of(const std::string& franchise) {
        auto it = art_assets.find(franchise);
        return it == art_assets.end() ? nullptr : &it->second;
    }
}

// Hooks in the game's open-by-name (boot func_80001C2C), which binary-searches the sorted TOC:
// a name it didn't find is looked up among the extras.
extern "C" {
    // Before 0x80001CDC: s3 = found, s0 = index; the lowercased name is at sp+0x10.
    void blitz_fs_lookup_extra(uint8_t* rdram, recomp_context* ctx) {
        if (ctx->r19 != 0 || extra_files.empty()) return;
        char name[0x20] = {};
        for (int i = 0; i < 0x1F; i++) {
            name[i] = (char)MEM_B(0x10 + i, ctx->r29);
            if (name[i] == 0) break;
        }
        for (size_t k = 0; k < extra_files.size(); k++) {
            if (strncmp(reinterpret_cast<const char*>(extra_files[k].data()), name, 0x14) == 0) {
                ctx->r19 = 1;
                ctx->r16 = extra_index + (int32_t)k;
                return;
            }
        }
    }
    // Before 0x80001CFC: the index-in-range test (v0); an extra index passes.
    int blitz_fs_is_extra(int32_t index) {
        return index >= extra_index && index < extra_index + (int32_t)extra_files.size();
    }
    // Before 0x80001D34 (v0 = index * 32, v1 = TOC base): an extra's entry is in RDRAM at extra_toc.
    int32_t blitz_fs_extra_entry(int32_t index) {
        return (int32_t)(extra_toc + 32u * (uint32_t)(index - extra_index));
    }
}
#endif

#if BLITZ_TEAMPLUS
// The loaded game ROM is a rebranded community release (its title screen isn't the Team Extension's own),
// e.g. Resurrection's Gold or Silver Edition. Settings → Season → Rosters "Auto" then plays its own teams.
// A Team Extension-based ROM image whose title screen isn't the Team Extension's own.
bool blitz::rom_data_rebranded(std::span<const uint8_t> rom) {
    auto be = [&](size_t o) { return (uint32_t)rom[o] << 24 | (uint32_t)rom[o + 1] << 16 | (uint32_t)rom[o + 2] << 8 | rom[o + 3]; };
    if (rom.size() < fs_start + 16 || (be(fs_start) & 0xFFFF0000) != 0x7A730000) return false;
    size_t toc = fs_start + be(fs_start + 8);
    uint32_t count = be(fs_start + 12);
    if (count > 0x10000 || toc + (size_t)count * 32 > rom.size()) return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = rom.data() + toc + (size_t)i * 32;
        if (strncmp(reinterpret_cast<const char*>(e), "blitztit.wms", 0x14) != 0) continue;
        uint32_t usize = be(toc + i * 32 + 0x14), off = be(toc + i * 32 + 0x18), psize = be(toc + i * 32 + 0x1C);
        size_t at = fs_start + off;
        std::vector<uint8_t> data;
        if (usize > (1u << 20) || at + (psize ? psize : usize) > rom.size()) return false;
        if (psize == 0) data.assign(rom.begin() + at, rom.begin() + at + usize);
        else if (!lzo1x_decompress(rom.data() + at, psize, usize, data)) return false;
        return XXH3_64bits(data.data(), data.size()) != 0xee0f77d4cb48c48fULL;
    }
    return false;
}

bool blitz::rom_rebranded() {
    static std::once_flag once;
    static bool rebranded = false;
    std::call_once(once, [] {
        rebranded = rom_data_rebranded(recomp::get_rom());
        fprintf(stderr, "[rom] %s\n", rebranded ? "a rebranded community ROM" : "the Team Extension's own title");
    });
    return rebranded;
}
#endif
#if BLITZ_MODERN && !BLITZ_TEAMPLUS
bool blitz::rom_rebranded() { return false; }  // the original game: no community releases to tell apart
#endif

// ---- Adding files to the game's file system (both apps) ----
// A feature registers named files at startup (before the game's fs_init reads the file system) and gets
// back their TOC index. fs_commit_files() then appends the data past the end of the in-memory ROM
// image and writes a longer copy of the TOC there: the original entries unchanged, then the new ones.
// The header's TOC offset and count point at it, so the game's own fs_init allocates and loads it and
// every index-based read works with no hook (asset id 0x0A000000 | (index + 1), fs_asset_id).
// Names start with '~': that sorts after every original name, so the game's binary search by name still
// finds all of those (team+ already ends its TOC with '~' files found only by index). Files added here
// are found by index, not by name.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"

namespace blitz {
    // fs_mark: League (a league pak's files), Season (+ Houston, the VS helmets and the 2026 home and away uniforms:
    // Lite: Season), All (+ every other uniform set: Lite: Arcade and Full). The app's own files come after.
    enum { fs_mark_league, fs_mark_season, fs_mark_all, fs_mark_count };
}

namespace {
    constexpr size_t fs_base = 0x150C40;  // the asset file system in both ROMs
    struct NewFile { std::string name; std::vector<uint8_t> data; };
    std::vector<NewFile> fs_pending;
    std::map<std::string, int32_t> fs_added;  // name -> TOC index, after fs_commit_files
    bool fs_committed = false;
    // Where each kind of exported cart's file list ends (game_init.cpp registers what carts need first, the app's
    // own last): the number of added files registered at each mark, then (after the commit) of TOC entries.
    size_t fs_marks[blitz::fs_mark_count] = {};
    uint32_t fs_original = 0;

    std::string fs_name(std::string name) {
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        if (name.empty() || name[0] != '~') name = "~" + name;
        return name;
    }
}

namespace blitz {
    void post_notice(const std::string& msg);  // notices.cpp
    uint32_t fs_ram_count(uint8_t* rdram);

    // At on_init, between registrations: the files registered so far are what that kind of cart keeps.
    void fs_mark(int mark) {
        if (mark >= 0 && mark < fs_mark_count) fs_marks[mark] = fs_pending.size();
    }
    // After fs_commit_files: the TOC entries a cart of that kind keeps (0 when unknown).
    uint32_t fs_cart_count(int mark) {
        return mark >= 0 && mark < fs_mark_count && fs_original ? fs_original + (uint32_t)fs_marks[mark] : 0;
    }

    // Before fs_commit_files (on_init). Names longer than 15 characters (with the '~') are refused:
    // the texture cache compares 16 bytes of a name.
    bool fs_register_file(const std::string& name, std::vector<uint8_t> data) {
        std::string n = fs_name(name);
        if (fs_committed || n.size() > 15 || data.empty()) {
            fprintf(stderr, "[fs] %s not added (%s)\n", n.c_str(), fs_committed ? "too late" : n.size() > 15 ? "name too long" : "empty");
            return false;
        }
        for (NewFile& f : fs_pending) {
            if (f.name == n) { f.data = std::move(data); return true; }
        }
        fs_pending.push_back({n, std::move(data)});
        return true;
    }

    // A new file by its own (lowercase) name, for the game to find by name (the original game's lookup hook scans
    // past its sorted names: blitz_fs_scan).
    void fs_register_exact(const std::string& name, std::vector<uint8_t> data) {
        std::string n = name;
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        if (fs_committed || data.empty()) return;
        fs_pending.push_back({n, std::move(data)});
    }

    // The TOC index of a file added with fs_register_file, once committed; -1 otherwise.
    int32_t fs_file_index(const std::string& name) {
        auto it = fs_added.find(fs_name(name));
        return it == fs_added.end() ? -1 : it->second;
    }

    uint32_t fs_asset_id(int32_t index) {
        return 0x0A000000u | (uint32_t)(index + 1);
    }

    // The TOC index of any file in the current ROM image's file system (vanilla 'fs' or team+ 'zs'), or -1.
    int32_t fs_rom_index(const std::string& name) {
        auto rom = recomp::get_rom();
        auto be = [&](size_t o) { return (uint32_t)rom[o] << 24 | (uint32_t)rom[o + 1] << 16 | (uint32_t)rom[o + 2] << 8 | rom[o + 3]; };
        if (rom.size() < fs_base + 16) return -1;
        uint32_t magic = be(fs_base) >> 16;
        if (magic != 0x7A73 && magic != 0x6673) return -1;
        size_t es = magic == 0x7A73 ? 32 : 40, ns = magic == 0x7A73 ? 0x14 : 0x18;
        size_t toc = fs_base + be(fs_base + 8);
        uint32_t count = be(fs_base + 12);
        if (count > 0x10000 || toc + count * es > rom.size()) return -1;
        for (uint32_t i = 0; i < count; i++) {
            const char* e = reinterpret_cast<const char*>(rom.data() + toc + i * es);
            if (strnlen(e, ns) == name.size() && memcmp(e, name.data(), name.size()) == 0) return (int32_t)i;
        }
        return -1;
    }

#if BLITZ_MODERN
    std::optional<std::vector<uint8_t>> rom_file_at(int32_t index);
    // A game file from the current ROM image by name, unpacked (portraits.cpp: the face and arm textures), or nothing.
    std::optional<std::vector<uint8_t>> rom_file(const std::string& name) {
        int32_t index = fs_rom_index(name);
        if (index < 0) return std::nullopt;
        return rom_file_at(index);
    }

    // The same, by its number in the file table: added files too (their data and table are in the ROM image).
    std::optional<std::vector<uint8_t>> rom_file_at(int32_t index) {
        auto rom = recomp::get_rom();
        auto be = [&](size_t o) { return (uint32_t)rom[o] << 24 | (uint32_t)rom[o + 1] << 16 | (uint32_t)rom[o + 2] << 8 | rom[o + 3]; };
        const FsFormat fmt{(be(fs_base) >> 16) == 0x7A73};
        size_t toc = fs_base + be(fs_base + 8);
        uint32_t usize, off, psize;
        entry_fields(rom.data() + toc + (size_t)index * fmt.es(), fmt, usize, off, psize);
        size_t at = fs_base + off, stored = psize ? psize : usize;
        if (usize > (1u << 20) || at + stored > rom.size()) return std::nullopt;
        std::vector<uint8_t> data;
        if (psize == 0) data.assign(rom.begin() + at, rom.begin() + at + usize);
        else if (!lzo1x_decompress(rom.data() + at, psize, usize, data)) return std::nullopt;
        return data;
    }
#endif

    // Make the running game's TOC entry `index` (in RDRAM) match the ROM image's: its size, offset and
    // packing words, after the name (which must match). A save state from an older build can carry TOC
    // entries that build pointed elsewhere in its own ROM image (its PlayStation icons, past the ROM end),
    // where this build has other data. Returns true when the entry was changed.
    bool fs_restore_entry(uint8_t* rdram, int32_t index) {
        auto rom = recomp::get_rom();
        auto be = [&](size_t o) { return (uint32_t)rom[o] << 24 | (uint32_t)rom[o + 1] << 16 | (uint32_t)rom[o + 2] << 8 | rom[o + 3]; };
        if (index < 0 || rom.size() < fs_base + 16) return false;
        uint32_t magic = be(fs_base) >> 16;
        if (magic != 0x7A73 && magic != 0x6673) return false;
        size_t es = magic == 0x7A73 ? 32 : 40, ns = magic == 0x7A73 ? 0x14 : 0x18;
        size_t toc = fs_base + be(fs_base + 8);
        uint32_t count = be(fs_base + 12);
        if ((uint32_t)index >= count || toc + count * es > rom.size()) return false;
        uint32_t ram_toc = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8005BDF4);
        if ((uint32_t)index >= fs_ram_count(rdram) || ram_toc < 0x80000000u || ram_toc >= 0x80800000u) return false;
        size_t e = toc + (size_t)index * es;
        uint32_t r = ram_toc + (uint32_t)index * (uint32_t)es;
        for (size_t k = 0; k < ns; k += 4) {  // same file?
            if ((uint32_t)MEM_W((int32_t)k, (gpr)(int32_t)r) != be(e + k)) return false;
        }
        bool changed = false;
        for (size_t k = ns; k < es; k += 4) {
            if ((uint32_t)MEM_W((int32_t)k, (gpr)(int32_t)r) != be(e + k)) {
                MEM_W((int32_t)k, (gpr)(int32_t)r) = (int32_t)be(e + k);
                changed = true;
            }
        }
        return changed;
    }

    // The number of TOC entries the running game loaded (its fs_init header in RDRAM), or 0. A save state
    // made before files were added carries the shorter TOC: an added index past it must not be used.
    uint32_t fs_ram_count(uint8_t* rdram) {
        uint32_t hdr = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8005BD88);
        if (hdr < 0x80000000u || hdr >= 0x80800000u) return 0;
        return (uint32_t)MEM_W(0xC, (gpr)(int32_t)hdr);
    }

    // On_init, after the art drop-in and every fs_register_file.
    void fs_commit_files() {
        if (fs_committed) return;
        fs_committed = true;
        if (fs_pending.empty()) return;
        auto cur = recomp::get_rom();
        auto be = [&](size_t o) { return (uint32_t)cur[o] << 24 | (uint32_t)cur[o + 1] << 16 | (uint32_t)cur[o + 2] << 8 | cur[o + 3]; };
        if (cur.size() < fs_base + 16) return;
        uint32_t magic = be(fs_base) >> 16;
        if (magic != 0x7A73 && magic != 0x6673) {
            fprintf(stderr, "[fs] unknown file system: %zu file(s) not added\n", fs_pending.size());
            return;
        }
        bool zs = magic == 0x7A73;
        size_t es = zs ? 32 : 40, ns = zs ? 0x14 : 0x18;
        size_t toc = fs_base + be(fs_base + 8);
        uint32_t count = be(fs_base + 12);
        if (count > 0x10000 || toc < fs_base || toc + count * es > cur.size()) {
            post_notice("The game's file list looks damaged, so the extra button icons weren't added.");
            return;
        }
        // The copy gets its final size up front: growing it would move it with the game's image still held, three
        // ROMs at once. (Each file: up to 15 bytes of padding, then its data; then padding and the longer TOC.)
        size_t need = cur.size() + 15 + (count + fs_pending.size()) * es;
        for (const NewFile& f : fs_pending) need += 15 + f.data.size();
        std::vector<uint8_t> rom;
        rom.reserve(need);
        rom.assign(cur.begin(), cur.end());
        auto put = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) rom[o + i] = (uint8_t)(v >> (24 - 8 * i)); };
        std::vector<uint8_t> table(cur.begin() + toc, cur.begin() + toc + count * es);
        fs_original = count;
        for (NewFile& f : fs_pending) {
            rom.resize((rom.size() + 15) & ~size_t(15));
            uint32_t off = (uint32_t)(rom.size() - fs_base), size = (uint32_t)f.data.size();
            rom.insert(rom.end(), f.data.begin(), f.data.end());
            std::vector<uint8_t>().swap(f.data);  // in the copy now: free it as we go
            std::vector<uint8_t> e(es, 0);
            memcpy(e.data(), f.name.data(), std::min(f.name.size(), ns - 1));
            auto w = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) e[o + i] = (uint8_t)(v >> (24 - 8 * i)); };
            w(ns, size);
            w(ns + 4, off);
            if (zs) w(ns + 8, 0);                         // packed size 0 = stored
            else { w(ns + 8, 5); w(ns + 12, size); }       // method 5 = stored
            fs_added[f.name] = (int32_t)(table.size() / es);
            table.insert(table.end(), e.begin(), e.end());
        }
        rom.resize((rom.size() + 15) & ~size_t(15));
        put(fs_base + 8, (uint32_t)(rom.size() - fs_base));
        put(fs_base + 12, (uint32_t)(table.size() / es));
        rom.insert(rom.end(), table.begin(), table.end());
        fprintf(stderr, "[fs] %zu file(s) added: TOC %u -> %zu entries\n", fs_pending.size(), count, table.size() / es);
        fs_pending.clear();
        recomp::set_rom_contents(std::move(rom));
    }
}

// The original game's open-by-name (boot func_80001C2C) binary-searches its sorted TOC; files added at startup by
// their own names (fs_register_exact: your announcer banks) sit past the sorted ones. Before 0x80001CDC (the
// "found?" test; engine32's build): when the search missed, a scan of the whole TOC (40-byte 'fs' entries). The
// lowercased name is at sp+0x10.
extern "C" void blitz_fs_scan(uint8_t* rdram, recomp_context* ctx) {
    if (ctx->r19 != 0) return;
    uint32_t hdr = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8005BD88), toc = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8005BDF4);
    if (hdr < 0x80000000u || hdr >= 0x80800000u || toc < 0x80000000u || toc >= 0x80800000u) return;
    uint32_t count = (uint32_t)MEM_W(0xC, (gpr)(int32_t)hdr);
    char name[0x18] = {};
    for (int i = 0; i < 0x17; i++) {
        name[i] = (char)MEM_B(0x10 + i, ctx->r29);
        if (name[i] == 0) break;
    }
    for (uint32_t k = 0; k < count && k < 0x10000; k++) {
        uint32_t e = toc + k * 40;
        int i = 0;
        for (; i < 0x18; i++) {
            char c = (char)MEM_B(i, (gpr)(int32_t)e);
            if (c != name[i]) break;
            if (c == 0) { i = 0x18; break; }
        }
        if (i == 0x18) {
            ctx->r19 = 1;
            ctx->r16 = (gpr)k;
            return;
        }
    }
}

// Save states and the file table (2026-11-14). The running game keeps its own copy of the file table in RDRAM (where each
// file is in the ROM image), and a save state carries that copy. The ROM image is this run's: the cartridge's files, then
// the art, uniforms and announcer banks laid over or added from the app, the 2026 Season and Add-Ons. When those change
// between saving and loading (a new pak, art added or taken out), the state's copy points at the wrong data, and the first
// file the game loads afterwards (a touchdown's graphics, say) is garbage: a crash. So a state records the table's names
// in order, refuses a different list, and on a load takes the current positions of every file (fs_restore_entry).
#include "blitz_savestate.h"

namespace blitz {
    // The current ROM image's file names, in order (0 if it has no file system we know).
    uint64_t fs_names_hash() {
        auto rom = recomp::get_rom();
        auto be = [&](size_t o) { return (uint32_t)rom[o] << 24 | (uint32_t)rom[o + 1] << 16 | (uint32_t)rom[o + 2] << 8 | rom[o + 3]; };
        if (rom.size() < fs_base + 16) return 0;
        uint32_t magic = be(fs_base) >> 16;
        if (magic != 0x7A73 && magic != 0x6673) return 0;
        size_t es = magic == 0x7A73 ? 32 : 40, ns = magic == 0x7A73 ? 0x14 : 0x18;
        size_t toc = fs_base + be(fs_base + 8);
        uint32_t count = be(fs_base + 12);
        if (count > 0x10000 || toc + count * es > rom.size()) return 0;
        XXH64_state_t* st = XXH64_createState();
        XXH64_reset(st, count);
        for (uint32_t i = 0; i < count; i++) XXH64_update(st, rom.data() + toc + (size_t)i * es, ns);
        uint64_t h = XXH64_digest(st);
        XXH64_freeState(st);
        return h;
    }

    // After a state is restored (savestate.cpp): every entry of the game's table whose name matches takes this run's size
    // and position. Returns how many changed.
    int fs_sync_ram_toc(uint8_t* rdram) {
        uint32_t n = fs_ram_count(rdram);
        int changed = 0;
        for (uint32_t i = 0; i < n; i++) changed += fs_restore_entry(rdram, (int32_t)i) ? 1 : 0;
        if (changed) fprintf(stderr, "[savestate] file table: %d of %u entries moved to this run's ROM image\n", changed, n);
        return changed;
    }
}

namespace {
    [[maybe_unused]] const bool files_registered = (blitz::savestate_register(
        "files",
        [] {
            uint64_t h = blitz::fs_names_hash();
            blitz::StateBlob b(8);
            memcpy(b.data(), &h, 8);
            return b;
        },
        [](const blitz::StateBlob&) {},
        [](const blitz::StateBlob& b) -> std::string {
            uint64_t saved = 0;
            if (b.size() != 8) return "";
            memcpy(&saved, b.data(), 8);
            if (saved == blitz::fs_names_hash()) return "";
            return "This state was saved with other Add-Ons (art, uniforms or a different 2026 Season), so it can't be loaded now.";
        }), true);
}
