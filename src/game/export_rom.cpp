// Export ROM (launcher, Settings → Add-Ons; docs/design/ONE_APP.md): a .z64 with the player's rosters, skin shades
// and art built in, for an emulator or a flash cart. Data only: the rules, play clock, Smarter CPU, uniform picker,
// hints and save states are the app's own code and don't come along.
//
// How: the app already builds a modified ROM image at every start (added game files: uniform sets, league art, the
// art drop-in). Export takes that image, copies ovl_b's data (ROM 0xDE350..0x129A90, stored uncompressed) into a
// scratch RDRAM, runs the same pack apply the game runs after loading ovl_b (blitz_on_dma_done), points each team's
// home and away at its 2026 sets (Modern), writes ovl_b back, fixes the header checksum (CIC 6102) and saves.
#include "../main/os_shell.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "recomp.h"
#include "../main/platform_compat.h"
#include <cstddef>
#include <optional>
#if BLITZ_MODERN
#include "team_store.h"
#endif
#if BLITZ_ENGINE_ORIGINAL
#include "engine32_layout.h"
#include "season_data.h"
#include "../cart/cart_season.h"
namespace blitz {
    std::vector<std::string> team_franchises();  // content_pack_tp.cpp
}
namespace {
#include "cart_season.inc"  // tools/cart/build.sh: the cart's Advanced season
}
namespace {
    struct E32Site { uint8_t seg; uint32_t vram, word; uint8_t table; uint32_t offset; uint8_t hi; };
#include "engine32_sites.inc"  // build/engine32 (tools/engine32_toml.py): the relocated address loads
}
#endif
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "librecomp/game.hpp"
#include "util/file.h"
#include "ultramodern/ultramodern.hpp"
#include "blitz_config.h"
#include "season_pak.h"

extern "C" void blitz_on_dma_done(uint8_t* rdram, uint32_t rom_end, int32_t ram_end);  // content_pack*.cpp

namespace blitz {
    int uniform_export_2026(uint8_t* rdram);  // uniform_picker.cpp
#if BLITZ_MODERN
    int uniform_export_2026(std::vector<teamdata::Team>& teams);
#endif
    int uniform_set_count(int team);
    void uniform_set_parts(uint8_t* rdram, int team, int set, const uint16_t base[20], uint16_t out[10]);
    struct ExportRules {  // house_rules.cpp
        std::vector<uint32_t> flags, flags_play;
        bool home_field = false, field_open = false, weather_open = false;
        int play_clock = 0;
        int first_down = 30, touchback = 20, two_point = 90;
        bool pat_miss = false, one_ot = false;
    };
    ExportRules export_rules();
    int export_venue(const std::string& abbr);
    uint8_t* monitor_rdram();                  // dev_monitor.cpp: set once the game's on_init ran
    uint32_t fs_cart_count(int mark);          // art_dropin.cpp: the file list a cart of that kind keeps
    void export_rosters(int mode);             // content_pack_tp.cpp: the players Rosters asks for; -1 the app's own
}

namespace {
#include "rom_picker_code.inc"

    // Settings → Export: what an exported ROM carries besides rosters, shades and art (each one patches the game's
    // own code, so it works on a real N64).
    bool opt(const char* id) {
        return std::get<bool>(recompui::config::get_config("export").get_option_value(id));
    }

    // Instruction and data patches in ROM terms: boot, main, ovl_a and ovl_b by RAM address.
    struct Rom {
        std::vector<uint8_t>& b;
        size_t at(uint32_t v) const {
            if (v >= 0x802EFE70 && v < 0x8033B5B0) return 0;  // overlay: use ovl_a / ovl_b
            if (v >= 0x80248F00) return 0x2FC20 + (v - 0x80248F00);
            return 0x1000 + (v - 0x80000400);
        }
        static size_t ovl_a(uint32_t v) { return 0x93EC0 + (v - 0x802EFE70); }
        static size_t ovl_b(uint32_t v) { return 0xDE350 + (v - 0x802EFE70); }
        uint32_t get(size_t o) const { return (uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | b[o + 3]; }
        void put(size_t o, uint32_t w) { for (int k = 0; k < 4; k++) b[o + k] = (uint8_t)(w >> (24 - 8 * k)); }
        // Only over the word the game has there (a different ROM leaves it alone); false if it didn't match.
        bool swap(size_t o, uint32_t was, uint32_t now) {
            if (get(o) != was) {
                fprintf(stderr, "[export] %06zX is %08X, not %08X: left as is\n", o, get(o), was);
                return false;
            }
            put(o, now);
            return true;
        }
        static uint32_t jal(uint32_t v) { return 0x0C000000u | ((v >> 2) & 0x03FFFFFFu); }
        static uint32_t imm(uint32_t w, int v) { return (w & 0xFFFF0000u) | ((uint32_t)v & 0xFFFF); }
    };

    // The options chosen in Settings → Export, patched into the image. Returns what went in, for the message.
    std::vector<std::string> patch_options(std::vector<uint8_t>& image, const std::vector<std::string>& abbr_of_team) {
        Rom r{image};
        std::vector<std::string> done;
        auto A = Rom::ovl_a;
#if BLITZ_TEAMPLUS
        // team+ fixes the app makes in its recompiled code (tools/teamplus.py FIXES), so a cart plays the same: the
        // right VS helmet mirrored only when the team has no right-facing one of its own (the 2026 art has them, so
        // team+'s always-mirror turned them backwards), each team's own VS-screen pause, and the vanilla announcer
        // table read instead of one into ovl_a's code.
        r.swap(r.at(0x8024C6B4), 0x2C100001, 0x2E100001);
        r.swap(r.at(0x8024C1D0), 0x9042A434, 0x9042A437);
        r.swap(A(0x8030656C), 0x8CA50E8C, 0x8CA5796C);
        // Team select in name order (tools/rom_picker/order.asm, as src/game/team_select.cpp): city, then select
        // name, read from the blocks the export just wrote.
        {
            constexpr int teams = 32;
            constexpr uint32_t blocks = 0x80330C9C, block_size = 0x220;
            auto name = [&](int t, uint32_t off) {
                std::string n;
                for (uint32_t i = 0; i < 16; i++) {
                    char c = (char)image[Rom::ovl_b(blocks + (uint32_t)t * block_size + off + i)];
                    if (!c) break;
                    n += (char)std::tolower((unsigned char)c);
                }
                return n;
            };
            std::vector<int> order(teams);
            for (int t = 0; t < teams; t++) order[(size_t)t] = t;
            std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
                return std::make_pair(name(x, 0x200), name(x, 0x1F0)) < std::make_pair(name(y, 0x200), name(y, 0x1F0));
            });
            const std::pair<uint32_t, uint32_t> sites[] = {{0x802F9B60, 0x02C2082A}, {0x802FA3AC, 0x8FA20054}, {0x802FA4EC, 0x24040014}};
            bool known = true;
            for (const auto& [v, w] : sites) known &= r.get(Rom::ovl_b(v)) == w;
            if (known) {
                for (size_t i = 0; i < std::size(rom_order_code); i++) r.put(r.at(rom_order_vram) + 4 * i, rom_order_code[i]);
                image[r.at(rom_order_n)] = (uint8_t)teams;
                for (int p = 0; p < teams; p++) {
                    image[r.at(rom_order_order) + (size_t)p] = (uint8_t)order[(size_t)p];
                    image[r.at(rom_order_pos) + (size_t)order[(size_t)p]] = (uint8_t)p;
                }
                r.put(Rom::ovl_b(0x802F9B60), Rom::jal(rom_order_moved));
                r.put(Rom::ovl_b(0x802FA3AC), Rom::jal(rom_order_above));
                r.put(Rom::ovl_b(0x802FA4EC), Rom::jal(rom_order_below));
            }
        }
#endif
        if (opt("export_rumble") && r.swap(Rom::ovl_b(0x802F8D24), 0x27BDFF98, 0x03E00008) &&
            r.swap(Rom::ovl_b(0x802F8D28), 0xAFBF0050, 0x00000000)) {
            done.push_back("no Rumble Pak screen");
        }
        blitz::ExportRules e = blitz::export_rules();
        if (opt("export_clock") && e.play_clock > 0) {
            r.swap(A(0x8032C498), 0x24070005, Rom::imm(0x24070005, e.play_clock));
            r.swap(A(0x8032C4BC), 0x24070009, Rom::imm(0x24070009, e.play_clock));
            done.push_back(std::to_string(e.play_clock) + "-second play clock");
        }
        if (opt("export_realism")) {
            bool any = false;
            if (e.first_down != 30) {
                for (uint32_t v : {0x8030152Cu, 0x80301738u, 0x80301838u, 0x803019A4u, 0x80301D00u, 0x80301D3Cu, 0x80301DA8u})
                    r.swap(A(v), 0x2442001E, Rom::imm(0x2442001E, e.first_down));
                r.swap(A(0x80301DE8), 0x24050046, Rom::imm(0x24050046, std::min(40 + e.first_down, 100)));  // kickoff out of bounds
                image[A(0x8033809C)] = (uint8_t)('0' + e.first_down / 10);  // "30 YARDS NEEDED FOR FIRST DOWN"
                any = true;
            }
            if (e.first_down != 30 || e.touchback != 20) {
                // Touchback: first down from the spot (a0 = v0 + yards), and a kickoff's spot from the rules code.
                r.swap(A(0x8030188C), 0x24040032, 0x24440000u | (uint32_t)e.first_down);
                any = true;
            }
            if (e.touchback != 20 && r.swap(A(0x80301870), 0x24020014, Rom::jal(rom_rules_touchback))) {
                r.put(r.at(rom_rules_tb_spot), (uint32_t)e.touchback);
                any = true;
            }
            if (e.two_point != 90 && r.swap(A(0x80301A50), 0x2402005A, Rom::imm(0x2402005A, e.two_point))) any = true;
            if (e.pat_miss) {  // the game's own rare miss (after halftime, 3%): every quarter, 6%
                r.swap(A(0x8032CECC), 0x28420002, 0x28420000);
                r.swap(A(0x8032CEE0), 0x28420003, 0x28420006);
                for (int i = 0; i < 4; i++) image[A(0x80337E04) + i] = (uint8_t)"1 PT"[i];
                any = true;
            }
            if (e.one_ot && r.swap(A(0x8030211C), 0x24020006, 0x24020004)) any = true;
            if (any) done.push_back("Realism rules");
        }
        bool codes = opt("export_codes") && (!e.flags.empty() || !e.flags_play.empty());
        bool home = opt("export_home") && e.home_field;
        if (codes || home) {
            for (size_t i = 0; i < std::size(rom_rules_code); i++) r.put(r.at(rom_rules_vram) + 4 * i, rom_rules_code[i]);
            if (e.touchback != 20 && opt("export_realism")) r.put(r.at(rom_rules_tb_spot), (uint32_t)e.touchback);
            if (codes) {
                for (size_t i = 0; i < e.flags.size() && i < 23; i++) r.put(r.at(rom_rules_flags) + 4 * i, e.flags[i]);
                for (size_t i = 0; i < e.flags_play.size() && i < 23; i++) r.put(r.at(rom_rules_flags_play) + 4 * i, e.flags_play[i]);
                done.push_back(std::to_string(e.flags.size() + e.flags_play.size()) + " Blitz codes");
            }
            if (home) {
                size_t h = r.at(rom_rules_home);
                image[h] = (uint8_t)((e.field_open ? 1 : 0) | (e.weather_open ? 2 : 0));
                for (size_t t = 0; t < abbr_of_team.size() && t < 32; t++) image[h + 1 + t] = (uint8_t)blitz::export_venue(abbr_of_team[t]);
                done.push_back("Home Field");
            }
            if (!r.swap(r.at(0x8024B880), 0x0000A021, Rom::jal(rom_rules_vram))) done.push_back("(codes left out: unknown ROM)");
        }
        else if (e.touchback != 20 && opt("export_realism")) {
            for (size_t i = 0; i < std::size(rom_rules_code); i++) r.put(r.at(rom_rules_vram) + 4 * i, rom_rules_code[i]);
            r.put(r.at(rom_rules_tb_spot), (uint32_t)e.touchback);
        }
        // Choose CPU Opponent (tools/rom_picker/opponent.asm): after the rules code, whose image is zeros where its
        // second part goes. Every site is checked first, so an unknown ROM gets none of it.
        if (opt("export_opponent")) {
            const std::pair<uint32_t, uint32_t> sites[] = {
                {0x802F85E0, 0x0C092BA9}, {0x802F9178, 0x8C42A418}, {0x802F91F8, 0x3C03802C}, {0x802F91FC, 0x9463D864},
                {0x802F9A1C, 0x0C093578}, {0x802F9A28, 0x0C093552}, {0x802F9A34, 0x0C09355F}, {0x802F590C, 0x3C02802F},
                {0x802F5910, 0x8C42A418}, {0x802F89C4, 0x3C02802C}, {0x802F89C8, 0x8442D864}, {0x802F8BC8, 0x0C09EE54}};
            bool known = true;
            for (const auto& [v, w] : sites) known &= r.get(Rom::ovl_b(v)) == w;
            if (known) {
                for (size_t i = 0; i < std::size(rom_opp_code); i++) r.put(r.at(rom_opp_vram) + 4 * i, rom_opp_code[i]);
                for (size_t i = 0; i < std::size(rom_opp_code2); i++) r.put(r.at(rom_opp_vram2) + 4 * i, rom_opp_code2[i]);
                for (size_t i = 0; i < std::size(rom_opp_code3); i++) r.put(r.at(rom_opp_vram3) + 4 * i, rom_opp_code3[i]);
                const uint32_t now[] = {Rom::jal(rom_opp_reset), Rom::jal(rom_opp_wake), Rom::jal(rom_opp_setup), 0,
                                        Rom::jal(rom_opp_in_a), Rom::jal(rom_opp_in_b), Rom::jal(rom_opp_in_c),
                                        Rom::jal(rom_opp_panel), 0, Rom::jal(rom_opp_hold), 0, Rom::jal(rom_opp_restore)};
                for (size_t k = 0; k < std::size(sites); k++) r.put(Rom::ovl_b(sites[k].first), now[k]);
                done.push_back("Choose CPU Opponent");
            }
            else {
                done.push_back("(CPU opponent left out: unknown ROM)");
            }
        }
        else if (r.get(r.at(rom_picker_vram)) == rom_picker_code[0]) {
            // The picker reads the opponent list's state (OPP_STATE): zeros, not guPosition's code, without it.
            for (int k = 0; k < 4; k++) r.put(r.at(rom_opp_woken) + 4 * (size_t)k, 0);
        }
        return done;
    }
    // Your own announcer calls whose bank isn't in the game (commande, texans; docs/ANNOUNCER.md): the app finds them
    // through hooks, a cart through tools/rom_picker/lookup.asm. They're appended to the ROM's file system (data, then
    // a new TOC at the end with them last; indices stay), and the lookup's not-found branch calls the scan. On the
    // original game (Modern's build) the app's image already has them (art_dropin.cpp): only the lookup goes in.
    int add_announcer_files(std::vector<uint8_t>& rom) {
        constexpr size_t fs_base = 0x150C40;
        auto be = [&](size_t o) { return (uint32_t)rom[o] << 24 | (uint32_t)rom[o + 1] << 16 | (uint32_t)rom[o + 2] << 8 | rom[o + 3]; };
        auto put = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) rom[o + i] = (uint8_t)(v >> (24 - 8 * i)); };
        if (rom.size() < fs_base + 16) return 0;
        uint32_t magic = be(fs_base) >> 16;
        if (magic != 0x7A73 && magic != 0x6673) return 0;  // team+'s 'zs' or the original 'fs'
        const bool zs = magic == 0x7A73;
        const size_t es = zs ? 32 : 40, ns = zs ? 0x14 : 0x18;
        size_t toc = fs_base + be(fs_base + 8);
        uint32_t count = be(fs_base + 12);
        if (count > 0x10000 || toc + count * es > rom.size()) return 0;
        std::vector<uint8_t> table(rom.begin() + (long)toc, rom.begin() + (long)(toc + count * es));
        auto name_at = [&](uint32_t i) { return reinterpret_cast<const char*>(&table[i * es]); };
        auto have = [&](const std::string& n) {
            for (uint32_t i = 0; i < count; i++)
                if (strncasecmp(name_at(i), n.c_str(), ns) == 0) return true;
            return false;
        };
        // What the game's own binary search finds (boot func_80001C2C).
        auto searched = [&](const std::string& n) {
            int lo = 0, hi = (int)count - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2, c = strncmp(n.c_str(), name_at((uint32_t)mid), ns);
                if (c == 0) return true;
                if (c > 0) lo = mid + 1;
                else hi = mid - 1;
            }
            return false;
        };
        std::vector<std::filesystem::path> files;
        int unsorted = 0;  // yours already in the image, past the sorted names
        std::error_code ec;
        std::map<std::string, std::filesystem::path> found;  // the app's own (Resources, the 2026 Season pak), then yours, which win
        std::vector<std::filesystem::path> dirs;
        for (const auto& c : blitz::content_dirs()) dirs.push_back(c / "announcer");
        dirs.push_back(recomp::get_config_path() / "announcer");
        for (const std::filesystem::path& dir : dirs)
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) found[e.path().filename().string()] = e.path();
        for (const auto& [n, path] : found) {
            std::string ext = path.extension().string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            if ((ext != ".wbk" && ext != ".ptr" && ext != ".bfx") || n.size() >= ns) continue;
            std::string lower = n;
            for (char& c : lower) c = (char)std::tolower((unsigned char)c);
            if (!have(n)) files.push_back(path);
            else if (!searched(lower)) unsorted++;
        }
        if (files.empty() && unsorted == 0) return 0;
        Rom r{rom};
        if (r.get(r.at(0x80001CDC)) != 0x12600027) return 0;  // not the lookup we know
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            std::ifstream in{f, std::ios::binary};
            std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
            rom.resize((rom.size() + 15) & ~size_t(15));
            uint32_t off = (uint32_t)(rom.size() - fs_base);
            rom.insert(rom.end(), data.begin(), data.end());
            std::vector<uint8_t> e(es, 0);
            std::string n = f.filename().string();
            for (size_t i = 0; i < n.size(); i++) e[i] = (uint8_t)std::tolower((unsigned char)n[i]);
            auto w = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) e[o + i] = (uint8_t)(v >> (24 - 8 * i)); };
            w(ns, (uint32_t)data.size());
            w(ns + 4, off);
            if (!zs) { w(ns + 8, 5); w(ns + 12, (uint32_t)data.size()); }  // method 5 = stored
            table.insert(table.end(), e.begin(), e.end());
            fprintf(stderr, "[export] announcer file %s added\n", n.c_str());
        }
        if (!files.empty()) {
            rom.resize((rom.size() + 15) & ~size_t(15));
            put(fs_base + 8, (uint32_t)(rom.size() - fs_base));
            put(fs_base + 12, (uint32_t)(table.size() / es));
            rom.insert(rom.end(), table.begin(), table.end());
        }
        Rom r2{rom};
        const uint32_t* code = zs ? rom_lookup_code : rom_lookup40_code;
        for (size_t i = 0; i < std::size(rom_lookup_code); i++) r2.put(r2.at(rom_lookup_vram) + 4 * i, code[i]);
        r2.put(r2.at(0x80001CDC), Rom::jal(rom_lookup_vram));
        return (int)files.size() + unsorted;
    }

    constexpr uint32_t ovl_rom = 0xDE350, ovl_rom_end = 0x129A90, ovl_ram = 0x802EFE70, ovl_ram_end = 0x8033B5B0;

    uint32_t be32(const std::vector<uint8_t>& r, size_t o) {
        return (uint32_t)r[o] << 24 | (uint32_t)r[o + 1] << 16 | (uint32_t)r[o + 2] << 8 | r[o + 3];
    }
    void put32(std::vector<uint8_t>& r, size_t o, uint32_t v) {
        for (int i = 0; i < 4; i++) r[o + i] = (uint8_t)(v >> (24 - 8 * i));
    }
    uint32_t rol(uint32_t v, uint32_t n) { return (v << n) | (v >> ((32 - n) & 31)); }

    // The header checksum the CIC 6102 boot code checks (n64crc): over ROM 0x1000..0x101000.
    void fix_crc(std::vector<uint8_t>& rom) {
        uint32_t seed = 0xF8CA4DDC, t1 = seed, t2 = seed, t3 = seed, t4 = seed, t5 = seed, t6 = seed;
        for (size_t i = 0x1000; i < 0x101000; i += 4) {
            uint32_t d = be32(rom, i);
            if (t6 + d < t6) t4++;
            t6 += d;
            t3 ^= d;
            uint32_t r = rol(d, d & 0x1F);
            t5 += r;
            if (t2 > d) t2 ^= r;
            else t2 ^= t6 ^ d;
            t1 += t5 ^ d;
        }
        put32(rom, 0x10, t6 ^ t4 ^ t3);
        put32(rom, 0x14, t5 ^ t2 ^ t1);
    }

    // Skin shades: the game itself draws two, light (1) and dark (anything else). The app's six (skin_tones.cpp)
    // become the nearer of the two: 5 Lighter, 2 Light-medium, 3 Medium on the light set (as the app draws them),
    // 4 Medium-dark and 0 Darker on the dark one.
    uint8_t shade(uint8_t v) { return (v == 1 || v == 2 || v == 3 || v == 5) ? 1 : 0; }

#if BLITZ_ENGINE_ORIGINAL
    // Modern on the original game: a cart runs the game's own code, so its 31 slots take the store's first 31 teams
    // (the ROM's, with the pack, this week's lineups and your crews; Houston, the 32nd, stays in the app) in the
    // game's own tables. Each segment (stored uncompressed) goes through a scratch RDRAM and back.
    // Houston on the cart (tools/rom_picker/cart32.asm, engine32_layout.h cart): every site the 32-team cart changes
    // holds the original game's instruction.
    bool cart32_ready(const std::vector<uint8_t>& rom) {
        auto get = [&](size_t o) { return o + 4 <= rom.size() ? (uint32_t)rom[o] << 24 | (uint32_t)rom[o + 1] << 16 | (uint32_t)rom[o + 2] << 8 | rom[o + 3] : 0u; };
        for (const E32Site& e : e32_sites) {
            size_t o = e.seg == 0 ? 0x2FC20 + (e.vram - 0x80248F00) : e.seg == 1 ? Rom::ovl_a(e.vram) : Rom::ovl_b(e.vram);
            if (get(o) != e.word) return false;
            if (!e.hi && (e.word >> 26) == 0x0D) return false;  // an ori: the carried upper half wouldn't do
        }
        const std::pair<size_t, uint32_t> words[] = {
            {0x1000 + (0x80001260 - 0x80000400), 0x3C050005}, {0x1000 + (0x80001264 - 0x80000400), 0x34A53C00},  // heap size
            {0x1000 + (0x800014A8 - 0x80000400), 0x0C00129E},                                                   // the loader's site
            {Rom::ovl_b(0x802F9B74), 0x00168400}, {Rom::ovl_b(0x802FA3A8), 0x27D3FFFF}, {Rom::ovl_b(0x802FA454), 0x2673FFFF},
            {Rom::ovl_b(0x802FA47C), 0x27D30001}, {Rom::ovl_b(0x802FA53C), 0xC6E20000}, {Rom::ovl_b(0x802FA504), 0x2A31001F},
            {Rom::ovl_b(0x802F5564), 0x90420000}, {Rom::ovl_b(0x802F5568), 0x3C01802E},
            {(size_t)(0x2FC20 + (0x8024C680 - 0x80248F00)), 0x28820009}};                                       // right helmets: 9 entries
        for (const auto& [o, w] : words)
            if (get(o) != w) return false;
        return true;
    }

    std::optional<std::string> export_store(std::vector<uint8_t>& rom, int& uniforms, std::vector<std::string>& abbrs,
                                            std::vector<int>& order, std::vector<std::array<uint16_t, 20>>& own,
                                            std::vector<uint8_t>* block) {
        namespace td = blitz::teamdata;
        const int slots = block ? blitz::engine32::teams : td::original::slots;  // 32 on the Houston cart
        std::vector<uint8_t> ram(0x800000, 0);
        auto ram_byte = [&](uint32_t vaddr) -> uint8_t& { return ram[(vaddr - 0x80000000u) ^ 3]; };
        // The store as the game has it: built on the first ovl_b load, so the same apply on a scratch ovl_b.
        for (uint32_t i = 0; i < ovl_rom_end - ovl_rom; i++) ram_byte(ovl_ram + i) = rom[ovl_rom + i];
        blitz_on_dma_done(ram.data(), ovl_rom_end, (int32_t)ovl_ram_end);
        const td::Store& st = td::store();
        if (st.slots().size() < (size_t)slots) return "the teams aren't loaded";
        std::vector<td::Team> teams(st.slots().begin(), st.slots().begin() + slots);
        std::vector<std::vector<bool>> clash(slots, std::vector<bool>(slots));
        for (int a = 0; a < slots; a++)
            for (int b = 0; b < slots; b++) clash[(size_t)a][(size_t)b] = st.clashes((size_t)a, (size_t)b);
        // Each team's own 20 ids (10 parts x home, away) before the 2026 sets go in: the cart picker's 1999.
        own.assign(teams.size(), {});
        for (size_t t = 0; t < teams.size(); t++)
            for (int i = 0; i < 20; i++) own[t][(size_t)i] = teams[t].uniform[(size_t)(i / 2)][(size_t)(i % 2)];
        uniforms = blitz::uniform_export_2026(teams);
        for (td::Team& t : teams)
            for (td::Team::Player& p : t.players) p.skin = shade(p.skin);
        for (const td::Team& t : teams) abbrs.push_back(std::string(t.code.data(), strnlen(t.code.data(), t.code.size())));
        // Team select's order (order_orig.asm): A-Z by city, then select name, as the app lists them.
        auto lower = [](const char* p, size_t n) {
            std::string out(p, strnlen(p, n));
            for (char& c : out) c = (char)std::tolower((unsigned char)c);
            return out;
        };
        order.resize(slots);
        for (int i = 0; i < slots; i++) order[(size_t)i] = i;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const td::Team &x = teams[(size_t)a], &y = teams[(size_t)b];
            return std::make_pair(lower(x.city_name.data(), x.city_name.size()), lower(x.select.data(), x.select.size())) <
                   std::make_pair(lower(y.city_name.data(), y.city_name.size()), lower(y.select.data(), y.select.size()));
        });
        td::Store cart;
        cart.set_all(std::move(teams), std::move(clash));
        struct Seg { td::original::Segment seg; uint32_t rom, rom_end, ram; };
        const Seg segs[] = {{td::original::Segment::Main, 0x2FC20, 0x93EC0, 0x80248F00},
                            {td::original::Segment::OvlA, 0x93EC0, ovl_rom, ovl_ram},
                            {td::original::Segment::OvlB, ovl_rom, ovl_rom_end, ovl_ram}};
        std::optional<td::original::GameLayout> game;  // the game's own tables, not Modern's moved ones
        std::optional<td::original::CartLayout> cart32;  // or the Houston cart's
        if (block) cart32.emplace();
        else game.emplace();
        for (const Seg& sg : segs) {
            for (uint32_t i = 0; i < sg.rom_end - sg.rom; i++) ram_byte(sg.ram + i) = rom[sg.rom + i];
            td::original::write(ram.data(), sg.seg, cart);
            if (block && sg.seg == td::original::Segment::Main) {
                // The 1-player CPU ladder (main 0x802AB0E0: 9 tiers of 4, -1 ended): Houston joins the last, with Denver.
                constexpr uint32_t last_tier = 0x802AB0E0 + 8 * 0x14;
                auto word = [&](uint32_t a) -> uint32_t {
                    return (uint32_t)ram_byte(a) << 24 | (uint32_t)ram_byte(a + 1) << 16 | (uint32_t)ram_byte(a + 2) << 8 | ram_byte(a + 3);
                };
                auto put = [&](uint32_t a, uint32_t v) { for (int k = 0; k < 4; k++) ram_byte(a + (uint32_t)k) = (uint8_t)(v >> (24 - 8 * k)); };
                if (word(last_tier + 4) == 0xFFFFFFFFu) {
                    put(last_tier + 4, 31);
                    put(last_tier + 8, 0xFFFFFFFFu);
                }
            }
            for (uint32_t i = 0; i < sg.rom_end - sg.rom; i++) rom[sg.rom + i] = ram_byte(sg.ram + i);
        }
        if (block) {
            // The jersey clash, 32 x 32 bytes, then the block as the boot loader will put it in RAM.
            namespace c = blitz::engine32::cart;
            for (int a = 0; a < slots; a++)
                for (int b = 0; b < slots; b++) ram_byte(c::clash + (uint32_t)(a * slots + b)) = cart.clashes((size_t)a, (size_t)b) ? 1 : 0;
            block->assign(c::end - c::base, 0);
            for (uint32_t i = 0; i < c::end - c::base; i++) (*block)[i] = ram_byte(c::base + i);
        }
        return std::nullopt;
    }

    // The Houston cart's code changes, after the export wrote its tables (export_store) and before patch_options:
    // the 176 address loads moved to the cart's tables, the default heap 16 KB smaller, the loader at boot, and the
    // block itself past the ROM's end. The block's code and team select's order go in with install_cart32_select.
    void install_cart32(std::vector<uint8_t>& rom, std::vector<uint8_t>& block, uint32_t& block_rom, bool lean) {
        namespace c = blitz::engine32::cart;
        Rom r{rom};
        for (const E32Site& e : e32_sites) {
            size_t o = e.seg == 0 ? 0x2FC20 + (e.vram - 0x80248F00) : e.seg == 1 ? Rom::ovl_a(e.vram) : Rom::ovl_b(e.vram);
            uint32_t now = c::table_at(e.table) + e.offset;
            r.put(o, (e.word & 0xFFFF0000u) | (e.hi ? ((now + 0x8000u) >> 16) : (now & 0xFFFFu)));
        }
        r.put(r.at(0x8024C680), 0x28820000u | (uint32_t)blitz::engine32::vs_right_entries);  // the moved right-helmet list
        const uint32_t heap = lean ? c::heap_size_lean : c::heap_size;  // Lite: Season's season sits under the block
        r.put(r.at(0x80001260), 0x3C050000u | (heap >> 16));
        r.put(r.at(0x80001264), 0x34A50000u | (heap & 0xFFFFu));
        for (size_t i = 0; i < std::size(rom_cart32_boot); i++) r.put(r.at(rom_cart32_boot_vram) + 4 * i, rom_cart32_boot[i]);
        r.put(r.at(0x800014A8), Rom::jal(rom_cart32_load));
        block_rom = (uint32_t)((rom.size() + 15) & ~size_t(15));
        rom.resize(block_rom, 0);
        rom.insert(rom.end(), block.begin(), block.end());
        Rom w{rom};
        w.put(w.at(rom_cart32_block), block_rom);
        w.put(w.at(rom_cart32_block) + 4, c::base);
        w.put(w.at(rom_cart32_block) + 8, c::end - c::base);
    }

    // The Advanced season on the Houston cart (src/cart, tools/cart/build.sh): its code with the pack's league (8
    // divisions of 4 and the regular-season schedule, by the cart's team slots) appended to the ROM, which the boot
    // loader copies under the cart32 block on any N64 (lean: Lite: Season) or to the Expansion Pak when there's one
    // (Full). False without a league for all 32 teams.
    static_assert(cart_season_lean_vram == blitz::engine32::cart::season_lean &&
                  cart_season_lean_vram + sizeof(cart_season_lean) <= blitz::engine32::cart::base,
                  "the cart season must fit between the heap's end and the cart32 block");
    bool install_cart_season(std::vector<uint8_t>& rom, bool lean) {
        const blitz::SeasonData* sd = blitz::pack_season();
        const std::vector<std::string>& slot_of = blitz::team_franchises();
        if (sd == nullptr || slot_of.size() != CS_TEAMS || sd->weeks.size() < CS_REGULAR_WEEKS) return false;
        auto slot = [&](const std::string& f) {
            for (int t = 0; t < CS_TEAMS; t++) if (slot_of[(size_t)t] == f) return t;
            return -1;
        };
        std::vector<uint8_t> code;
        if (lean) for (uint32_t w : cart_season_lean) for (int k = 0; k < 4; k++) code.push_back((uint8_t)(w >> (24 - 8 * k)));
        else for (uint32_t w : cart_season_pak) for (int k = 0; k < 4; k++) code.push_back((uint8_t)(w >> (24 - 8 * k)));
        const uint32_t vram = lean ? cart_season_lean_vram : cart_season_pak_vram;
        const size_t lg = 0x40;  // the league block (src/cart/link.ld)
        auto put32 = [&](size_t o, uint32_t v) { for (int k = 0; k < 4; k++) code[lg + o + (size_t)k] = (uint8_t)(v >> (24 - 8 * k)); };
        auto put8 = [&](size_t o, uint8_t v) { code[lg + o] = v; };
        put32(offsetof(CartLeague, magic), CS_LEAGUE_MAGIC);
        put32(offsetof(CartLeague, team_table), blitz::engine32::cart::table_at(0));
        for (int d = 0; d < CS_DIVISIONS; d++)
            for (int k = 0; k < CS_DIVISION_SIZE; k++) {
                int t = slot(sd->divisions[(size_t)d][(size_t)k]);
                if (t < 0) return false;
                put8(offsetof(CartLeague, members) + (size_t)(d * CS_DIVISION_SIZE + k), (uint8_t)t);
            }
        for (int w = 0; w < CS_REGULAR_WEEKS; w++) {
            const auto& games = sd->weeks[(size_t)w];
            int n = (int)std::min<size_t>(games.size(), CS_MAX_GAMES);
            put8(offsetof(CartLeague, games) + (size_t)w, (uint8_t)n);
            for (int g = 0; g < n; g++) {
                int away = slot(games[(size_t)g].first), home = slot(games[(size_t)g].second);
                if (away < 0 || home < 0) return false;
                size_t o = offsetof(CartLeague, schedule) + (size_t)((w * CS_MAX_GAMES + g) * 2);
                put8(o, (uint8_t)away);
                put8(o + 1, (uint8_t)home);
            }
        }
        auto text = [&](size_t o, size_t room, std::string t) {
            for (size_t i = 0; i < room; i++) put8(o + i, i + 1 < room && i < t.size() ? (uint8_t)t[i] : 0);
        };
        for (int d = 0; d < CS_DIVISIONS; d++) {  // "AFC East" -> "EAST", as the app's standings menu
            std::string n = sd->division_names[(size_t)d];
            std::string word = n.substr(n.find(' ') == std::string::npos ? 0 : n.find(' ') + 1);
            for (char& c : word) c = (char)std::toupper((unsigned char)c);
            text(offsetof(CartLeague, div_names) + (size_t)d * 12, 12, word);
        }
        static const char* const rounds[] = {"Wildcard", "Divisional", "Championship", "SuperBowl"};
        for (int w = 0; w < CS_TOTAL_WEEKS; w++)
            text(offsetof(CartLeague, week_names) + (size_t)w * 14, 14,
                 w < CS_REGULAR_WEEKS ? "Week " + std::to_string(w + 1) : std::string(rounds[w - CS_REGULAR_WEEKS]));
        uint32_t at = (uint32_t)((rom.size() + 15) & ~size_t(15));
        rom.resize(at, 0);
        rom.insert(rom.end(), code.begin(), code.end());
        rom.resize((rom.size() + 15) & ~size_t(15), 0);
        Rom r{rom};
        r.put(r.at(rom_cart32_season), at);
        r.put(r.at(rom_cart32_season) + 4, vram);
        r.put(r.at(rom_cart32_season) + 8, (uint32_t)code.size());
        return true;
    }

    // After patch_options (Choose CPU Opponent's reset may be at team select's start: ours goes on to it): the block's
    // code (team select over 32 in name order, the clash), in the ROM's copy of the block.
    void install_cart32_select(std::vector<uint8_t>& rom, uint32_t block_rom, const std::vector<int>& order) {
        namespace c = blitz::engine32::cart;
        static_assert(rom_cart32_vram == c::code, "cart32.asm's CODE is engine32_layout.h's cart::code");
        Rom r{rom};
        auto in_block = [&](uint32_t vram) { return (size_t)block_rom + (vram - c::base); };
        uint32_t start = r.get(Rom::ovl_b(0x802F85E0));
        for (size_t i = 0; i < std::size(rom_cart32_code); i++) r.put(in_block(rom_cart32_vram) + 4 * i, rom_cart32_code[i]);
        r.put(in_block(rom_cart32_next), 0x80000000u | ((start & 0x03FFFFFFu) << 2));
        for (size_t p = 0; p < order.size(); p++) {
            rom[in_block(rom_cart32_order) + p] = (uint8_t)order[p];
            rom[in_block(rom_cart32_pos) + (size_t)order[p]] = (uint8_t)p;
        }
        r.put(Rom::ovl_b(0x802F85E0), Rom::jal(rom_cart32_reset));
        r.put(Rom::ovl_b(0x802F9B74), Rom::jal(rom_cart32_moved));
        r.put(Rom::ovl_b(0x802FA3A8), Rom::jal(rom_cart32_above_0));
        r.put(Rom::ovl_b(0x802FA454), Rom::jal(rom_cart32_above_n));
        r.put(Rom::ovl_b(0x802FA47C), Rom::jal(rom_cart32_below_0));
        r.put(Rom::ovl_b(0x802FA53C), Rom::jal(rom_cart32_below_n));
        r.put(Rom::ovl_b(0x802FA504), 0x2A310020);  // a row below may be the 32nd team
        r.put(Rom::ovl_b(0x802F5564), 0x08000000u | ((rom_cart32_clash >> 2) & 0x03FFFFFFu));  // j clash
    }
#endif

#if BLITZ_ENGINE_ORIGINAL
    // Team select in name order on the original game's ROM (tools/rom_picker/order_orig.asm), after patch_options
    // (Choose CPU Opponent's reset may be at the team select start site: ours goes on to it).
    bool install_order(std::vector<uint8_t>& rom, const std::vector<int>& order) {
        Rom r{rom};
        const std::pair<uint32_t, uint32_t> sites[] = {{0x802F9B74, 0x00168400}, {0x802FA3A8, 0x27D3FFFF}, {0x802FA454, 0x2673FFFF},
                                                       {0x802FA47C, 0x27D30001}, {0x802FA53C, 0xC6E20000}};
        for (const auto& [v, w] : sites)
            if (r.get(Rom::ovl_b(v)) != w) return false;
        uint32_t start = r.get(Rom::ovl_b(0x802F85E0));
        if ((start >> 26) != 3 || order.size() != 31) return false;  // a jal there (the game's, or the opponent list's)
        for (size_t i = 0; i < std::size(rom_order_orig_code); i++) r.put(r.at(rom_order_orig_vram) + 4 * i, rom_order_orig_code[i]);
        r.put(r.at(rom_order_orig_next), 0x80000000u | ((start & 0x03FFFFFFu) << 2));
        for (size_t p = 0; p < order.size(); p++) {
            rom[r.at(rom_order_orig_order) + p] = (uint8_t)order[p];
            rom[r.at(rom_order_orig_pos) + (size_t)order[p]] = (uint8_t)p;
        }
        r.put(Rom::ovl_b(0x802F85E0), Rom::jal(rom_order_orig_reset));
        r.put(Rom::ovl_b(0x802F9B74), Rom::jal(rom_order_orig_moved));
        r.put(Rom::ovl_b(0x802FA3A8), Rom::jal(rom_order_orig_above_0));
        r.put(Rom::ovl_b(0x802FA454), Rom::jal(rom_order_orig_above_n));
        r.put(Rom::ovl_b(0x802FA47C), Rom::jal(rom_order_orig_below_0));
        r.put(Rom::ovl_b(0x802FA53C), Rom::jal(rom_order_orig_below_n));
        return true;
    }
#endif

#if BLITZ_ENGINE_ORIGINAL
    // The Uniform Picker on the original game's ROM (tools/rom_picker/picker.asm -equ ORIG 1): its code over
    // guLookAtF, _frexpf and osViFade; team select's per-frame sleep calls it; each team's sets in a table past the
    // ROM's end (its 1999 home and away, then its extra sets, 2026 first; 10 at most); and its reset first at team
    // select's start (after install_order: it goes on to the call that was there). Returns the sets put in.
    // Only sets whose files are all on this cart (file ids up to `files`: Lite: Season keeps the 2026 home and away).
    int install_picker_orig(std::vector<uint8_t>& rom, const std::vector<std::array<uint16_t, 20>>& own, const uint32_t uni[3],
                            uint32_t files) {
        uint8_t* live = blitz::monitor_rdram();
        Rom r{rom};
        constexpr uint32_t sleep_call = 0x802F9C90;  // jal process_sleep (0x0C09CF33)
        uint32_t start = r.get(Rom::ovl_b(0x802F85E0));
        if (live == nullptr || r.get(Rom::ovl_b(sleep_call)) != 0x0C09CF33 || (start >> 26) != 3) return 0;
        size_t table = (rom.size() + 15) & ~size_t(15);
        rom.resize(table + own.size() * 0x100, 0);
        Rom w{rom};
        for (size_t i = 0; i < std::size(rom_picker_orig_code); i++) w.put(w.at(rom_picker_vram) + 4 * i, rom_picker_orig_code[i]);
        for (size_t i = 0; i < std::size(rom_picker_orig_code2); i++) w.put(w.at(rom_picker_orig_vram2) + 4 * i, rom_picker_orig_code2[i]);
        for (size_t i = 0; i < std::size(rom_picker_orig_code3); i++) w.put(w.at(rom_picker_orig_vram3) + 4 * i, rom_picker_orig_code3[i]);
        int total = 0;
        for (size_t t = 0; t < own.size(); t++) {
            int n = 0;
            for (int k = 0; k < blitz::uniform_set_count((int)t) && n < 10; k++) {
                uint16_t parts[10];
                blitz::uniform_set_parts(live, (int)t, k, own[t].data(), parts);
                if (std::any_of(std::begin(parts), std::end(parts), [&](uint16_t id) { return files && id > files; })) continue;
                for (int p = 0; p < 10; p++) {
                    rom[table + t * 0x100 + (size_t)n * 0x18 + 2 * (size_t)p] = (uint8_t)(parts[p] >> 8);
                    rom[table + t * 0x100 + (size_t)n * 0x18 + 2 * (size_t)p + 1] = (uint8_t)parts[p];
                }
                n++;
            }
            rom[w.at(rom_picker_counts) + t] = (uint8_t)n;
            total += n;
        }
        w.put(w.at(rom_picker_table), (uint32_t)table);
        w.put(w.at(rom_picker_orig_next), 0x80000000u | ((start & 0x03FFFFFFu) << 2));
        for (int k = 0; k < 3; k++) w.put(w.at(rom_picker_orig_uni) + 4 * (size_t)k, uni[k]);
        w.put(Rom::ovl_b(0x802F85E0), Rom::jal(rom_picker_orig_reset));
        w.put(Rom::ovl_b(sleep_call), Rom::jal(rom_picker_vram));
        return total;
    }
#endif

    // Cuts the ROM's file list after the files a cart of this kind uses (art_dropin.cpp's marks: 1 the 2026 home and
    // away, 2 every uniform set); the app's own (button icons, Pro Bowl art, the credits page) never go. Each entry
    // costs the N64 40 bytes of heap. Returns the entries kept (0: the list is as it was).
    uint32_t keep_files(std::vector<uint8_t>& rom, int mark) {
        constexpr size_t fs_base = 0x150C40;
        uint32_t keep = blitz::fs_cart_count(mark);
        if (keep == 0 || rom.size() < fs_base + 16) return 0;
        Rom r{rom};
        if ((r.get(fs_base) >> 16) != 0x6673 || keep > r.get(fs_base + 12)) return 0;  // the original's 'fs' only
        fprintf(stderr, "[export] file list: %u of %u entries\n", keep, r.get(fs_base + 12));
        r.put(fs_base + 12, keep);
        return keep;
    }

    std::string last_path;  // the last export's file (Blitz Builder says where it went)

    std::string export_now(std::string& detail) {
#if BLITZ_MODERN
        // The players the Rosters option asks for, for this export only.
        blitz::export_rosters((int)std::get<uint32_t>(recompui::config::get_config("export").get_option_value("export_rosters")));
        struct Restore { ~Restore() { blitz::export_rosters(-1); } } restore;
#endif
        auto cur = recomp::get_rom();
        std::vector<uint8_t> rom(cur.begin(), cur.end());
        if (rom.size() < ovl_rom_end) return "the game's ROM isn't loaded";
#if BLITZ_ENGINE_ORIGINAL
        int uniforms = 0;
        std::vector<std::string> abbrs;  // each team's scoreboard abbreviation (Home Field's stadiums)
        std::vector<int> order;  // team select A-Z (order_orig.asm, cart32.asm)
        std::vector<std::array<uint16_t, 20>> own;  // each team's 1999 ids (the cart picker)
        // Houston on the cart when every site the 32-team cart changes is the game's; else the 31 teams. The 2026
        // season with every uniform set goes to the Expansion Pak (Full); with the 2026 home and away only, under the
        // cart32 block on any N64 (Lite: Season), whose heap is then smaller.
        const bool houston = opt("export_teams32") && cart32_ready(rom);
        const bool every_set = opt("export_uniforms");
        const bool lean = houston && opt("export_season") && !every_set;
        // The file list a plain N64 keeps in its heap: only the files this kind of cart uses (game_init.cpp's order).
        const uint32_t files = keep_files(rom, every_set ? 2 : 1);
        std::vector<uint8_t> block;
        uint32_t block_rom = 0;
        if (auto problem = export_store(rom, uniforms, abbrs, order, own, houston ? &block : nullptr)) return *problem;
        bool season = false;
        if (houston) {
            install_cart32(rom, block, block_rom, lean);
            if (opt("export_season")) season = install_cart_season(rom, lean);
        }
#else
        // ovl_b's data in RDRAM word order (native 32-bit words: byte i of a word at (addr ^ 3)).
        std::vector<uint8_t> ram(0x800000, 0);
        auto ram_byte = [&](uint32_t vaddr) -> uint8_t& { return ram[(vaddr - 0x80000000u) ^ 3]; };
        for (uint32_t i = 0; i < ovl_rom_end - ovl_rom; i++) ram_byte(ovl_ram + i) = rom[ovl_rom + i];
#if BLITZ_TEAMPLUS
        // The pack goes on the scratch copy the way it goes on the game's; the team store (team_store.h) is the
        // running game's, so it's kept and put back.
        blitz::teamdata::Store kept = blitz::teamdata::store();
        blitz_on_dma_done(ram.data(), ovl_rom_end, (int32_t)ovl_ram_end);
        blitz::teamdata::store() = kept;
#else
        blitz_on_dma_done(ram.data(), ovl_rom_end, (int32_t)ovl_ram_end);
#endif
#if BLITZ_TEAMPLUS
        // Each team's own 20 uniform ids (10 parts x home, away), before the 2026 sets go in: the cart picker's 1999.
        constexpr int picker_teams = 32;
        std::vector<std::array<uint16_t, 20>> own(picker_teams);
        for (int t = 0; t < picker_teams; t++)
            for (int i = 0; i < 20; i++) own[(size_t)t][(size_t)i] = blitz::teamdata::live::uniform_part(ram.data(), t, i / 2, i % 2);
#endif
        int uniforms = blitz::uniform_export_2026(ram.data());
#if BLITZ_TEAMPLUS
        for (int t = 0; t < 60; t++)  // every block (unused ones too, as before)
            for (int p = 0; p < 16; p++)
                blitz::teamdata::live::set_skin(ram.data(), t, p, shade((uint8_t)blitz::teamdata::live::skin(ram.data(), t, p)));
#else
        for (uint32_t r = 0; r < 31 * 16; r++) {  // the roster table: 31 teams x 16 records, skin a big-endian word
            uint32_t word = 0x80330BCC + r * 0x3C + 0x08;
            if (word + 4 > ovl_ram_end) break;
            uint8_t& v = ram_byte(word + 3);
            v = shade(v);
        }
#endif
        for (uint32_t i = 0; i < ovl_rom_end - ovl_rom; i++) rom[ovl_rom + i] = ram_byte(ovl_ram + i);
#if BLITZ_TEAMPLUS
        // The Uniform Picker on the cart (tools/rom_picker/picker.asm): its code over guLookAtF (never called), team
        // select's per-frame sleep calls it, and each team's sets go in a table past the ROM's end: the game's 1999
        // home and away, then the team's extra sets (2026 first), 10 at most.
        int picker_sets = 0;
        if (uint8_t* live = blitz::monitor_rdram(); live != nullptr && opt("export_picker")) {
            auto put16 = [&](size_t o, uint16_t v) { rom[o] = (uint8_t)(v >> 8); rom[o + 1] = (uint8_t)v; };
            auto put32 = [&](size_t o, uint32_t v) { for (int k = 0; k < 4; k++) rom[o + k] = (uint8_t)(v >> (24 - 8 * k)); };
            size_t table = (rom.size() + 15) & ~size_t(15);
            rom.resize(table + picker_teams * 0x100, 0);
            constexpr size_t code_rom = 0x1000 + (rom_picker_vram - 0x80000400);
            for (size_t i = 0; i < std::size(rom_picker_code); i++) put32(code_rom + 4 * i, rom_picker_code[i]);
            for (int t = 0; t < picker_teams; t++) {
                int n = std::min(10, blitz::uniform_set_count(t));
                for (int k = 0; k < n; k++) {
                    uint16_t parts[10];
                    blitz::uniform_set_parts(live, t, k, own[(size_t)t].data(), parts);
                    for (int p = 0; p < 10; p++) put16(table + (size_t)t * 0x100 + (size_t)k * 0x18 + 2 * (size_t)p, parts[p]);
                }
                rom[code_rom + (rom_picker_counts - rom_picker_vram) + (size_t)t] = (uint8_t)n;
                picker_sets += n;
            }
            put32(code_rom + (rom_picker_table - rom_picker_vram), (uint32_t)table);
            constexpr size_t sleep_call = ovl_rom + (0x802F9C90 - ovl_ram);  // jal process_sleep (0x0C09CF33)
            if (rom[sleep_call] == 0x0C && rom[sleep_call + 1] == 0x09 && rom[sleep_call + 2] == 0xCF && rom[sleep_call + 3] == 0x33) {
                put32(sleep_call, 0x0C000000u | ((rom_picker_vram >> 2) & 0x03FFFFFFu));
            }
            else {
                picker_sets = 0;  // not team+'s team select: leave the code unreachable
            }
        }
#endif
        std::vector<std::string> abbrs;  // each team's scoreboard abbreviation (Home Field's stadiums)
        for (uint32_t t = 0; t < 32; t++) {
#if BLITZ_TEAMPLUS
            abbrs.push_back(blitz::teamdata::live::abbr(ram.data(), (int)t));
#else
            uint32_t a = 0x8033800C + t * 0x64 + 0x50;
            if (t >= 31) break;
            std::string ab;
            for (uint32_t i = 0; i < 4 && ram_byte(a + i); i++) ab += (char)ram_byte(a + i);
            abbrs.push_back(ab);
#endif
        }
#endif
        std::vector<std::string> extras = patch_options(rom, abbrs);
#if BLITZ_ENGINE_ORIGINAL
        if (houston) {
            install_cart32_select(rom, block_rom, order);
            extras.push_back("Houston");
            if (season) extras.push_back(lean ? "the 2026 season (32 teams, 8 divisions; no Expansion Pak needed)"
                                              : "the 2026 season (32 teams, 8 divisions; with an Expansion Pak)");
            extras.push_back("team select A-Z");
        }
        else if (install_order(rom, order)) extras.push_back("team select A-Z");
        int picker_sets = 0;
        if (opt("export_picker")) {
            namespace c = blitz::engine32::cart;
            constexpr uint32_t home = 0x8028AAB0, away = 0x8028AF88;
            const uint32_t uni[3] = {houston ? c::now(home) : home,
                                     houston ? block_rom + (c::now(home) - c::base) : (uint32_t)(0x2FC20 + (home - 0x80248F00)),
                                     houston ? c::now(away) - c::now(home) : away - home};
            picker_sets = install_picker_orig(rom, own, uni, files);
        }
#endif
        if (int n = add_announcer_files(rom); n > 0) extras.push_back(std::to_string(n) + " announcer files of your own");
        fix_crc(rom);
        rom.resize((rom.size() + 0xFFFFF) & ~size_t(0xFFFFF), 0xFF);  // whole megabytes, as carts and emulators expect

        std::filesystem::path dir = blitz::downloads_folder();
        if (const char* d = getenv("BLITZ_EXPORT_DIR")) dir = d;  // dev runs
        static const bool reveal_it = getenv("BLITZ_EXPORT_DIR") == nullptr;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        // Which kind it is (the Export screen's shortcut; export_screen.cpp), so carts made by different builds or
        // shortcuts can't be mixed up on an SD card.
        const uint32_t kind = std::get<uint32_t>(recompui::config::get_config("export").get_option_value("export_preset"));
#if BLITZ_TEAMPLUS
        // BRANDING: exported ROM file names
        std::string name = kind == 2 ? "Modern Blitz Team Extension Custom" : "Modern Blitz Team Extension";
#elif BLITZ_MODERN
        // BRANDING: exported ROM file names
        std::string name = kind == 0 ? "New Blitz Full" : kind == 1 ? "New Blitz Lite Season"
                         : kind == 3 ? "New Blitz Lite Arcade" : kind == 4 ? "Original Blitz 2000" : "New Blitz Custom";
#else
        // BRANDING: exported ROM file names
        std::string name = kind == 2 ? "Classic Blitz Custom" : "Classic Blitz";
#endif
        // Each export its own name (team): an emulator that has the last one open keeps it, and nothing is overwritten.
        {
            std::time_t now = std::time(nullptr);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), " %Y-%m-%d %H.%M.%S", std::localtime(&now));
            // BRANDING: exported ROM file name suffix
            name += std::string(" (BlitzRecomp export") + stamp + ").z64";
        }
        std::filesystem::path out = dir / name;
        std::ofstream f{out, std::ios::binary};
        if (!f) return "couldn't write " + out.string();
        f.write(reinterpret_cast<const char*>(rom.data()), (std::streamsize)rom.size());
        f.close();
#ifdef __PROSPERO__
        // BRANDING: PS5 export folder name shown to the user
        const char* where = "/data/blitzrecomp/exports (copy it off over FTP)";
#else
        const char* where = "Downloads";
#endif
        detail = "Saved \"" + name + "\" in " + where + " (" + std::to_string(rom.size() >> 20) + " MB)" +
                 (uniforms > 0 ? ", with the 2026 uniforms for " + std::to_string(uniforms) + " teams." : ".");
#if BLITZ_MODERN
        fprintf(stderr, "[export] cart uniform picker: %d sets\n", picker_sets);
        if (picker_sets > 0) detail += " At team select, C-left changes the uniform.";
#endif
        if (!extras.empty()) {
            detail += " Also in: ";
            for (size_t i = 0; i < extras.size(); i++) detail += (i ? ", " : "") + extras[i];
            detail += ".";
        }
        fprintf(stderr, "[export] %s (%zu bytes, %d teams in 2026 uniforms)\n", out.string().c_str(), rom.size(), uniforms);
        last_path = out.string();
        if (reveal_it) blitz::reveal_in_file_browser(out);
        return "";
    }

    bool pending = false;  // Export from the launcher: the game starts first, then the export runs
}

namespace blitz {
    bool firstrun_export_finished(bool ok, const std::string& path, const std::string& detail);  // firstrun.cpp
    void export_screen_open();  // export_screen.cpp
    extern bool export_applying;  // export_screen.cpp: a shortcut is setting the toggles
    void export_preset_values(uint32_t value);

    // The export, with the game running, and what was saved.
    void export_rom_finish() {
        std::string detail;
        last_path.clear();
        std::string problem = export_now(detail);
        // Blitz Builder's N64 or emulator path says where it went on its own last screen.
        if (blitz::firstrun_export_finished(problem.empty(), last_path, problem.empty() ? detail : "The ROM wasn't saved: " + problem + "."))
            return;
        if (problem.empty()) recompui::open_info_prompt("ROM exported", detail, "OK", [] {}, recompui::ButtonStyle::Secondary);
        else recompui::open_info_prompt("Couldn't export", "The ROM wasn't saved: " + problem + ".", "OK", [] {}, recompui::ButtonStyle::Secondary);
    }

    // The Export screen's and Settings → Export's Export. It needs the ROM image the game builds at startup, so from
    // the launcher the game starts first and the export follows a moment later.
    void export_rom_go() {
        if (ultramodern::is_game_started() && monitor_rdram() != nullptr) {
            export_rom_finish();
            return;
        }
        pending = true;
        std::u8string game_id = blitz::game_id;
        recompui::update_game_mod_id(blitz::mod_game_id);
        recomp::start_game(game_id, {});
        recompui::hide_all_contexts();
    }
}

namespace blitz {
    // Settings → Export: what an exported ROM carries (each is optional; they use your current Rules settings).
    void export_rom_dialog();

    void create_export_tab() {
        recomp::config::Config& c = recompui::config::create_config_tab("Export", "export", false);
        // The Export screen's shortcut (export_screen.cpp): it sets the toggles below; changing one makes it Custom.
        c.add_enum_option("export_preset", "Kind of ROM",
                          "The Export screen's shortcuts (the launcher's Export ROM). Changing anything below makes it Custom."
                          "<br /><br />An exported ROM always has your rosters, skin shades (as the game's two), art and the "
                          "2026 home and away uniforms. The options below add the app's own features as changes to the "
                          "game's code, so they work on a real N64 or flash cart too. Each uses your current settings.",
                          std::vector<recomp::config::ConfigOptionEnumOption>{
#if BLITZ_ENGINE_ORIGINAL
                              {3, "arcade", "Lite: Arcade"}, {1, "lite", "Lite: Season"}, {0, "full", "Full"},
                              {4, "original", "Original 1999"},
#elif BLITZ_MODERN
                              {0, "all", "Team Extension"},
#else
                              {0, "all", "Classic"},
#endif
                              {2, "custom", "Custom"}},
                          0u);
#if BLITZ_MODERN
        // Whose players go in (content_pack_tp.cpp export_rosters).
        c.add_enum_option("export_rosters", "Rosters",
                          "<b>Current</b>: this week's lineups, injuries and all. <b>2026 Season</b>: every team at full "
                          "strength. <b>Custom</b>: this week's with your Roster Room crews (the Export screen opens the "
                          "Roster Room).",
                          std::vector<recomp::config::ConfigOptionEnumOption>{{0, "current", "Current"}, {1, "season", "2026 Season"},
                                                                             {2, "custom", "Custom"}},
                          0u);
#endif
#if BLITZ_ENGINE_ORIGINAL
        c.add_bool_option("export_teams32", "32 Teams",
                          "Houston joins as the 32nd team, in every mode. Off: the game's own 31 teams.", true);
        c.add_bool_option("export_season", "2026 Season",
                          "Season plays the 2026 league: 8 divisions, 18 weeks, 7-team playoffs (needs 32 Teams). With every "
                          "uniform set it needs an Expansion Pak on a real N64; with the 2026 home and away only, any N64. "
                          "Off: the game's own 1999 Season.", true);
        c.add_bool_option("export_uniforms", "Every Uniform Set",
                          "Every uniform set the app has (alternates, throwbacks) for the Uniform Picker. Off: each team's 2026 "
                          "home and away (and the 1999 ones), which leaves a plain N64 room for the 2026 season.", true);
#endif
#if BLITZ_MODERN
        c.add_bool_option("export_picker", "Uniform Picker",
                          "C-left at team select steps through the team's uniforms: the game's choice, the 1999 home and "
                          "away, then its extra sets.", true);
#endif
        c.add_bool_option("export_rumble", "Skip Rumble Pak Screen",
                          "Controller Select goes straight to team select.", true);
        c.add_bool_option("export_opponent", "Choose CPU Opponent",
                          "In a 1-player game, once you've picked your team the other side's list opens for you: pick who "
                          "the CPU plays as (its marker reads 3), as Rules → Choose CPU Opponent does in the app. Tested in Ares; "
                          "emulators built on mupen64plus (RetroArch's usual N64 core) freeze at team select with it.", true);
        c.add_bool_option("export_clock", "Play Clock",
                          "Rules → Play Clock's seconds at play select (15 or 20). Nothing changes if it's set to the game's own.", true);
        c.add_bool_option("export_codes", "Blitz Codes",
                          "Rules' codes and choices (heads, weather, stadium, field, turbo …) on in every game, as the app sets "
                          "them. The gameplay ones stay off in Season, as the game rules.", true);
        c.add_bool_option("export_home", "Home Field",
                          "Rules → Home Field: the home team's grass or turf, and no rain or snow under a roof.", true);
        c.add_bool_option("export_realism", "Realism",
                          "Rules → Realism: first-down yards, kickoff touchback spot, two-point spot, extra points that "
                          "can miss, one overtime. On a cart these apply in Season too.", true);
        for (const char* id : {
#if BLITZ_ENGINE_ORIGINAL
                 "export_teams32", "export_season", "export_uniforms",
#endif
#if BLITZ_MODERN
                 "export_picker",
#endif
                 "export_rumble", "export_opponent", "export_clock", "export_codes", "export_home", "export_realism"}) {
            c.add_option_change_callback(id, [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant,
                                                recomp::config::OptionChangeContext ctx) {
                if (ctx == recomp::config::OptionChangeContext::Load || blitz::export_applying) return;
                recompui::config::get_config("export").update_option_value("export_preset", (uint32_t)2);
            });
        }
        c.add_option_change_callback("export_preset", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                         recomp::config::OptionChangeContext ctx) {
            if (ctx == recomp::config::OptionChangeContext::Load || blitz::export_applying) return;
            blitz::export_applying = true;
            blitz::export_preset_values(std::get<uint32_t>(cur));
            blitz::export_applying = false;
        });
#if BLITZ_MODERN
        // Without the 2026 Season (a public build without the pak, 2026-11-13) there are no 2026 rosters or season to
        // put on a cart: those choices hide (the Export screen offers Original 1999 then; export_screen.cpp).
        for (const char* id : {"export_rosters",
#if BLITZ_ENGINE_ORIGINAL
                               "export_season", "export_uniforms",
#endif
             })
            c.update_option_hidden(id, !blitz::season_content());
#endif
        c.add_bool_option("export_now", "Export ROM", "Saves the ROM to your Downloads folder.", false);
        recompui::config::set_option_action("export", "export_now", "Export");
        c.add_option_change_callback("export_now", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                      recomp::config::OptionChangeContext ctx) {
            if (ctx != recomp::config::OptionChangeContext::Permanent || !std::get<bool>(cur)) return;
            recompui::config::get_config("export").update_option_value("export_now", false);
            blitz::export_rom_go();
        });
    }

    // The launcher's Export ROM, and Settings → Add-Ons: the Export screen (export_screen.cpp).
    void export_rom_dialog() { export_screen_open(); }

    // Dev: the live command "EXPORTROM" exports straight away, without the prompt.
    std::atomic<bool> dev_export{false};
    void export_rom_now_dev() { dev_export = true; }
    std::atomic<bool> dev_screen{false};
    void export_screen_dev() { dev_screen = true; }

    // Every UI tick (main.cpp).
    void export_update() {
        static bool at_launch = getenv("BLITZ_EXPORT_SCREEN") != nullptr;  // dev: the screen over the launcher
        static int ticks = 0;  // the UI has to exist first
        if (at_launch && !ultramodern::is_game_started() && ++ticks > 40) {
            at_launch = false;
            export_screen_open();
        }
        if (dev_screen.exchange(false)) export_screen_open();
        if (dev_export.exchange(false)) {
            std::string detail;
            std::string problem = export_now(detail);
            fprintf(stderr, "[export] dev: %s\n", problem.empty() ? detail.c_str() : problem.c_str());
        }
        if (!pending || !ultramodern::is_game_started() || monitor_rdram() == nullptr || recompui::is_prompt_open()) return;
        static int frames = 0;
        if (++frames < 60) return;  // let the game settle (the first ovl_b load applies the pack)
        pending = false;
        frames = 0;
        export_rom_finish();
    }
}
