// Modern's added game files on the PSP (docs/PSP_PORT.md §4.3, M8). On the desktop, art_dropin.cpp rewrites the whole ROM in
// memory at on_init: the 2027 logo over the title images, Modern's team art, Houston, the uniform sets, the button icons,
// the credits page, appended past the ROM's end with a longer file table. The PSP can't hold the ROM, so psp-host's
// --make-kit runs that same code on the Mac and writes the result as the kit's rom/blitz.z64 (about 25 MB), plus
// rom/kit.txt (what the merge left behind outside the ROM: the uniform sets, the announcer pauses) and rom/artfs.bin (the
// extra file-table entries art_dropin keeps at 0x80408000; none for the original game's 'fs' table).
//
// Here the same functions answer from that ROM's own file table, read once through the ROM provider at fs_commit_files:
// fs_file_index(name) is the entry of that name, which is the index the Mac's commit gave it, since the table is the
// one it wrote. Registering does nothing (the data is in the ROM already); the modules that register still run, so a
// file that is missing from the kit shows as missing from the table, not as a wrong picture.
#if BLITZ_MODERN
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "psp_log.h"
#include "psp_sys.h"

namespace blitz {
    size_t uniform_sets_import(const std::string& text);  // uniform_picker.cpp
}

namespace {
    constexpr size_t fs_base = 0x150C40;      // the asset file system
    constexpr uint32_t extra_toc = 0x80408000; // art_dropin.cpp's extra entries in RDRAM
    constexpr int32_t extra_index = 0x7000;

    std::map<std::string, int32_t> toc;       // every name in the kit ROM's table -> its index
    uint32_t toc_count = 0, toc_original = 0;
    bool loaded = false;
    std::string kit_text;
    std::vector<std::vector<uint8_t>> extra_files;
    std::map<std::string, int> vs_pause;

    uint32_t be(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

    std::string fs_name(std::string name) {
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        if (name.empty() || name[0] != '~') name = "~" + name;
        return name;
    }

    std::string kit_value(const std::string& key) {
        size_t at = kit_text.find("\n" + key + " ");
        if (at == std::string::npos) return "";
        size_t s = at + key.size() + 2, e = kit_text.find('\n', s);
        return kit_text.substr(s, e == std::string::npos ? std::string::npos : e - s);
    }

    void load_kit() {
        static bool done = false;
        if (done) return;
        done = true;
        std::string path = psplog::app_path("rom/kit.txt");
        if (!pspsys::read_file(path, kit_text, 1 << 20)) {
            PWARN("artfs", "no %s: a ROM without Modern's art (tools/psp/kit.sh makes it)", path.c_str());
            return;
        }
        kit_text = "\n" + kit_text;
        for (size_t at = 0; (at = kit_text.find("\nvs_pause ", at)) != std::string::npos; at++) {
            char bank[32] = {};
            int pause = -1;
            if (sscanf(kit_text.c_str() + at + 10, "%31s %d", bank, &pause) == 2) vs_pause[bank] = pause;
        }
        std::string extras;
        if (pspsys::read_file(psplog::app_path("rom/artfs.bin"), extras, 64 * 1024)) {
            for (size_t i = 0; i + 32 <= extras.size(); i += 32) extra_files.emplace_back(extras.begin() + i, extras.begin() + i + 32);
        }
        PLOG("artfs", "kit: %s (%zu extra table entries, %zu announcer pauses)", kit_value("kit").c_str(), extra_files.size(),
             vs_pause.size());
    }

    // The kit ROM's file table, through the ROM provider (once; 40 bytes an entry, about 110 KB read).
    void load_toc() {
        if (loaded) return;
        loaded = true;
        recomp::rom_provider* rom = recomp::get_rom_provider();
        if (rom == nullptr) return;
        uint8_t hdr[16];
        rom->read(fs_base, hdr, sizeof hdr);
        uint32_t magic = be(hdr) >> 16;
        if (magic != 0x6673 && magic != 0x7A73) {
            PERR("artfs", "the ROM has no file system at 0x%zX (magic %04X)", fs_base, (unsigned)magic);
            return;
        }
        size_t es = magic == 0x7A73 ? 32 : 40, ns = magic == 0x7A73 ? 0x14 : 0x18;
        uint32_t off = be(hdr + 8), count = be(hdr + 12);
        if (count > 0x10000 || fs_base + off + (size_t)count * es > rom->size()) {
            PERR("artfs", "the ROM's file table looks damaged (%u entries at +0x%X)", (unsigned)count, (unsigned)off);
            return;
        }
        std::vector<uint8_t> table((size_t)count * es);
        rom->read((uint32_t)(fs_base + off), table.data(), table.size());
        toc_count = count;
        load_kit();
        uint32_t game_count = (uint32_t)strtoul(kit_value("toc_game").c_str(), nullptr, 10);
        for (uint32_t i = 0; i < count; i++) {
            const char* e = reinterpret_cast<const char*>(table.data() + (size_t)i * es);
            std::string name(e, strnlen(e, ns));
            toc.emplace(name, (int32_t)i);
        }
        toc_original = game_count ? game_count : count;
        PLOG("artfs", "file table: %u entries (%u the game's, %u added on the Mac)", (unsigned)count, (unsigned)toc_original,
             (unsigned)(count - toc_original));
    }
}

namespace {
    // engine32.cpp's team loader reads the ROM's code region (team records, rosters, art ids in main, ovl_a and ovl_b):
    // ROM bytes 0 to the asset file system, 1.3 MB, read once and freed by the caller.
}
std::vector<uint8_t> psp_rom_head() {
    std::vector<uint8_t> head;
    recomp::rom_provider* rom = recomp::get_rom_provider();
    if (rom == nullptr) return head;
    head.resize(fs_base);
    rom->read(0, head.data(), head.size());
    return head;
}

namespace blitz {
    std::string psp_kit_text() {
        load_kit();
        return kit_text;
    }

    // ---- art_dropin.cpp's interface ----
    void art_init() {
        load_kit();
        PLOG("art", "Modern's art is merged into the kit's ROM (psp-host --make-kit): nothing to do at boot");
    }
    void art_install_extras(uint8_t* rdram) {
        load_kit();
        for (size_t k = 0; k < extra_files.size(); k++)
            for (uint32_t i = 0; i < 32; i++) MEM_B(0, (gpr)(int32_t)(extra_toc + 32 * k + i)) = (int8_t)extra_files[k][i];
    }
    bool art_active() { return false; }  // a community ROM's art (team+ only)
    const std::vector<uint8_t>* art_assets_of(const std::string&) { return nullptr; }
    int announcer_vs_pause(const std::string& bank) {
        load_kit();
        auto it = vs_pause.find(bank);
        return it == vs_pause.end() ? -1 : it->second;
    }
    bool rom_rebranded() { return false; }  // the original game: no community releases to tell apart

    void fs_mark(int) {}
    uint32_t fs_cart_count(int) { return 0; }  // no Export ROM here
    bool fs_register_file(const std::string& name, std::vector<uint8_t> data) {
        std::string n = fs_name(name);
        return n.size() <= 15 && !data.empty();
    }
    void fs_register_exact(const std::string&, std::vector<uint8_t>) {}
    void fs_commit_files() {
        load_kit();
        load_toc();
        if (!kit_text.empty()) {
            std::string want = kit_value("toc");
            if (!want.empty() && want != std::to_string(toc_count))
                PERR("artfs", "the ROM's table has %u entries, kit.txt says %s: a ROM and kit.txt from different kits",
                     (unsigned)toc_count, want.c_str());
        }
    }
    int32_t fs_file_index(const std::string& name) {
        load_toc();
        auto it = toc.find(fs_name(name));
        return it == toc.end() ? -1 : it->second;
    }
    uint32_t fs_asset_id(int32_t index) { return 0x0A000000u | (uint32_t)(index + 1); }
    int32_t fs_rom_index(const std::string& name) {
        load_toc();
        auto it = toc.find(name);
        return it == toc.end() ? -1 : it->second;
    }
    uint32_t fs_ram_count(uint8_t* rdram) {
        uint32_t hdr = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8005BD88);
        if (hdr < 0x80000000u || hdr >= 0x80800000u) return 0;
        return (uint32_t)MEM_W(0xC, (gpr)(int32_t)hdr);
    }
    bool fs_restore_entry(uint8_t*, int32_t) { return false; }  // save states only
}

extern "C" {
    // art_dropin.cpp's hooks in the game's open-by-name (boot func_80001C2C), as there.
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
    int blitz_fs_is_extra(int32_t index) { return index >= extra_index && index < extra_index + (int32_t)extra_files.size(); }
    int32_t blitz_fs_extra_entry(int32_t index) { return (int32_t)(extra_toc + 32u * (uint32_t)(index - extra_index)); }

    // Names added by their own name (your announcer banks) sit past the sorted ones: a scan of the whole table in RDRAM
    // when the game's binary search missed (art_dropin.cpp's, unchanged).
    void blitz_fs_scan(uint8_t* rdram, recomp_context* ctx) {
        if (ctx->r19 != 0) return;
        uint32_t hdr = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8005BD88), tbl = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8005BDF4);
        if (hdr < 0x80000000u || hdr >= 0x80800000u || tbl < 0x80000000u || tbl >= 0x80800000u) return;
        uint32_t count = (uint32_t)MEM_W(0xC, (gpr)(int32_t)hdr);
        char name[0x18] = {};
        for (int i = 0; i < 0x17; i++) {
            name[i] = (char)MEM_B(0x10 + i, ctx->r29);
            if (name[i] == 0) break;
        }
        for (uint32_t k = 0; k < count && k < 0x10000; k++) {
            uint32_t e = tbl + k * 40;
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
}
#endif
