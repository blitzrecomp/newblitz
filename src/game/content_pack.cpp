// Content packs: the first step towards editable rosters ("Blitz 2026"; STATUS.md "Direction").
//
// NFL Blitz 2000 keeps its team table (31 x 0x64 bytes) and rosters (31 x 16 x 0x3C bytes) as plain
// data in ovl_b, the menu overlay, which is DMA'd from ROM every time the menus load. Menus read
// those tables directly; when two teams are picked, ovl_b 0x802F5360 copies them (and their
// rosters) into main's bss, which gameplay reads. So patching the ovl_b tables right after each
// ovl_b DMA (hook at dma_read 0x80001748) reaches every consumer, with no game code changed.
// Layouts and evidence: NOTES "Data loaders" and docs/CONTENT_MAP.md.
//
// A pack is <dir>/pack.json (format "blitz-pack/1"), chosen by BLITZ_PACK=<dir or pack.json>
// (development) or the first line of <app folder>/packs/active.txt (a pack id under packs/).
// Validation is strict: any problem means Classic (nothing written) plus a notice. Before writing,
// the tables are checked against vanilla values (and each player's "expect", if given).
// The 2026 flavour has its own loader (content_pack_tp.cpp, blitz-pack/2).
#if !BLITZ_MODERN
#include "../main/platform_compat.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "json/json.hpp"

#include "recomp.h"
#include "librecomp/game.hpp"
#include "team_store.h"
#include "blitz_soft_reset.h"

namespace blitz {
    void post_notice(const std::string& msg);  // notices.cpp
}

namespace {
    using json = nlohmann::json;

    constexpr uint32_t team_table = 0x8033800C, team_stride = 0x64, team_count = 31;
    constexpr uint32_t roster_table = 0x80330BCC, player_stride = 0x3C, players_per_team = 16;
    constexpr uint32_t city_upper_table = 0x80338E68, city_upper_stride = 16;
    constexpr uint32_t ovl_b_rom_end = 0x129A90, ovl_b_ram_end = 0x8033B5B0;

    // Team record fields.
    constexpr uint32_t team_ratings = 0x00, team_nickname = 0x10, team_city = 0x30, team_city_abbr = 0x50, team_roster = 0x58;
    // Player record fields.
    constexpr uint32_t player_size = 0x00, player_body = 0x04, player_skin = 0x08, player_number = 0x0C,
                       player_call = 0x18, player_last = 0x1C, player_first = 0x2C;

    // Vanilla city abbreviations in team-index order (team +0x50).
    const char* const team_abbrs[team_count] = {
        "ARZ", "ATL", "BAL", "BUF", "CAR", "CHI", "CIN", "CLE", "DAL", "DEN", "DET", "G.B", "IND", "JAC", "K.C", "MIA",
        "MIN", "N.E", "N.O", "NYG", "NYJ", "OAK", "PHI", "PIT", "S.D", "S.F", "SEA", "STL", "T.B", "TEN", "WAS",
    };
    const char* const rating_names[5] = {"passing", "rushing", "linemen", "defense", "special_teams"};

    struct Write {
        uint32_t vaddr;
        std::vector<uint8_t> bytes;  // big-endian, as the game sees them
    };
    struct Expect {
        uint32_t vaddr;
        bool is_string;
        std::string text;   // compared case-insensitively up to the field's NUL
        uint32_t len;
        uint32_t value;     // u32 compare otherwise
        std::string what;
    };
    struct Pack {
        std::string id, title;
        std::vector<Write> writes;
        std::vector<Expect> expects;
    };

    std::once_flag load_once;
    std::optional<Pack> active;
    bool reported_mismatch = false;

    // Restart in place (soft_restart.cpp): the pack is read again, as a fresh start reads it.
    [[maybe_unused]] const bool pack_soft_reset = blitz::on_soft_reset("content pack", [] {
        blitz::reset_once(load_once);
        active.reset();
        reported_mismatch = false;
    });

    struct PackError {
        std::string msg;
    };
    [[noreturn]] void fail(const std::string& msg) { throw PackError{msg}; }

    std::vector<uint8_t> be16(int16_t v) { return {(uint8_t)(v >> 8), (uint8_t)v}; }
    std::vector<uint8_t> be32(uint32_t v) { return {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; }
    std::vector<uint8_t> text_field(const std::string& s, uint32_t field_len) {
        std::vector<uint8_t> out(field_len, 0);
        std::copy(s.begin(), s.end(), out.begin());
        return out;
    }

    // Characters the game's fonts are known to draw.
    bool valid_name(const std::string& s) {
        return std::all_of(s.begin(), s.end(), [](char c) {
            return std::isalpha((unsigned char)c) || c == ' ' || c == '.' || c == '\'' || c == '-';
        });
    }
    bool valid_text(const std::string& s) {
        return std::all_of(s.begin(), s.end(), [](char c) {
            return std::isalnum((unsigned char)c) || c == ' ' || c == '.' || c == '\'' || c == '-';
        });
    }

    std::string str_field(const json& obj, const char* key, size_t max_len, bool name, const std::string& where) {
        if (!obj.contains(key)) {
            fail(where + ": missing \"" + key + "\"");
        }
        if (!obj[key].is_string()) {
            fail(where + ": \"" + key + "\" must be a string");
        }
        std::string s = obj[key].get<std::string>();
        if (s.empty() || s.size() > max_len) {
            fail(where + ": \"" + key + "\" must be 1-" + std::to_string(max_len) + " characters (\"" + s + "\")");
        }
        if (name ? !valid_name(s) : !valid_text(s)) {
            fail(where + ": \"" + key + "\" has characters the game can't draw (\"" + s + "\")");
        }
        return s;
    }

    int int_field(const json& obj, const char* key, int lo, int hi, const std::string& where) {
        if (!obj.contains(key)) {
            fail(where + ": missing \"" + key + "\"");
        }
        if (!obj[key].is_number_integer()) {
            fail(where + ": \"" + key + "\" must be a whole number");
        }
        int v = obj[key].get<int>();
        if (v < lo || v > hi) {
            fail(where + ": \"" + key + "\" must be " + std::to_string(lo) + "-" + std::to_string(hi) + " (got " + std::to_string(v) + ")");
        }
        return v;
    }

    void only_keys(const json& obj, std::initializer_list<const char*> allowed, const std::string& where) {
        if (!obj.is_object()) {
            fail(where + " must be an object");
        }
        for (auto& [k, v] : obj.items()) {
            if (std::none_of(allowed.begin(), allowed.end(), [&](const char* a) { return k == a; })) {
                fail(where + ": unknown key \"" + k + "\"");
            }
        }
    }

    std::string normalize_abbr(std::string s) {
        std::string out;
        for (char c : s) {
            if (c != '.') {
                out += (char)std::toupper((unsigned char)c);
            }
        }
        return out;
    }

    int team_index(const json& t, const std::string& where) {
        if (t.is_number_integer()) {
            int i = t.get<int>();
            if (i < 0 || i >= (int)team_count) {
                fail(where + ": team index must be 0-30");
            }
            return i;
        }
        if (t.is_string()) {
            std::string want = normalize_abbr(t.get<std::string>());
            for (uint32_t i = 0; i < team_count; i++) {
                if (normalize_abbr(team_abbrs[i]) == want) {
                    return (int)i;
                }
            }
        }
        fail(where + ": unknown team " + t.dump() + " (use 0-30 or the 1999 abbreviation, e.g. \"ARZ\", \"G.B\")");
    }

    bool iequal(const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
        });
    }

    uint32_t bcd(int n) { return (uint32_t)((n / 10) * 16 + n % 10); }

    Pack parse_pack(const json& root) {
        Pack pack;
        only_keys(root, {"format", "id", "title", "requires_rom", "teams"}, "pack");
        if (root.value("format", "") != "blitz-pack/1") {
            fail("\"format\" must be \"blitz-pack/1\"");
        }
        if (root.value("requires_rom", "") != "blitz2000-v11") {
            fail("\"requires_rom\" must be \"blitz2000-v11\" (NFL Blitz 2000 USA v1.1)");
        }
        pack.id = root.value("id", "");
        if (pack.id.empty() || pack.id.size() > 40 ||
            !std::all_of(pack.id.begin(), pack.id.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '-' || c == '_'; })) {
            fail("\"id\" must be 1-40 letters, digits, '-' or '_'");
        }
        pack.title = root.value("title", pack.id);
        if (!root.contains("teams") || !root["teams"].is_array()) {
            fail("\"teams\" must be a list");
        }

        std::vector<bool> team_seen(team_count, false);
        for (const json& t : root["teams"]) {
            only_keys(t, {"team", "ratings", "city", "city_upper", "city_abbr", "players"}, "team entry");
            if (!t.contains("team")) {
                fail("team entry: missing \"team\"");
            }
            int ti = team_index(t["team"], "team entry");
            std::string where = std::string("team ") + team_abbrs[ti];
            if (team_seen[ti]) {
                fail(where + " appears twice");
            }
            team_seen[ti] = true;
            uint32_t team = team_table + ti * team_stride;

            if (t.contains("ratings")) {
                const json& r = t["ratings"];
                only_keys(r, {"passing", "rushing", "linemen", "defense", "special_teams"}, where + " ratings");
                for (int k = 0; k < 5; k++) {
                    if (r.contains(rating_names[k])) {
                        // Read signed and used to index a 6-entry meter image table: must stay 0-5.
                        int v = int_field(r, rating_names[k], 0, 5, where + " ratings");
                        pack.writes.push_back({team + team_ratings + 2 * k, be16((int16_t)v)});
                    }
                }
            }
            if (t.contains("city")) {
                std::string city = str_field(t, "city", 31, false, where);
                pack.writes.push_back({team + team_city, text_field(city, 32)});
                // Team select lists the 16-byte uppercase name; keep the two in step.
                std::string upper = t.contains("city_upper") ? str_field(t, "city_upper", 15, false, where) : city;
                std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return (char)std::toupper((unsigned char)c); });
                if (upper.size() > 15) {
                    fail(where + ": the city is over 15 characters; give a shorter \"city_upper\" for team select");
                }
                pack.writes.push_back({city_upper_table + ti * city_upper_stride, text_field(upper, 16)});
            }
            else if (t.contains("city_upper")) {
                fail(where + ": \"city_upper\" needs \"city\"");
            }
            if (t.contains("city_abbr")) {
                std::string abbr = str_field(t, "city_abbr", 3, false, where);
                pack.writes.push_back({team + team_city_abbr, text_field(abbr, 4)});
            }

            if (t.contains("players")) {
                if (!t["players"].is_array()) {
                    fail(where + ": \"players\" must be a list");
                }
                std::vector<bool> slot_seen(players_per_team, false);
                for (const json& p : t["players"]) {
                    only_keys(p, {"slot", "expect", "first", "last", "number", "size", "skin", "body", "announcer_call"}, where + " player");
                    int slot = int_field(p, "slot", 0, players_per_team - 1, where + " player");
                    std::string pw = where + " slot " + std::to_string(slot);
                    if (slot_seen[slot]) {
                        fail(pw + " appears twice");
                    }
                    slot_seen[slot] = true;
                    uint32_t rec = roster_table + (ti * players_per_team + slot) * player_stride;

                    std::string expect_first, expect_last;
                    if (p.contains("expect")) {
                        const json& e = p["expect"];
                        only_keys(e, {"first", "last", "number"}, pw + " expect");
                        if (e.contains("last")) {
                            expect_last = str_field(e, "last", 15, true, pw + " expect");
                            pack.expects.push_back({rec + player_last, true, expect_last, 16, 0, pw + " last name"});
                        }
                        if (e.contains("first")) {
                            expect_first = str_field(e, "first", 15, true, pw + " expect");
                            pack.expects.push_back({rec + player_first, true, expect_first, 16, 0, pw + " first name"});
                        }
                        if (e.contains("number")) {
                            pack.expects.push_back({rec + player_number, false, "", 0, bcd(int_field(e, "number", 0, 99, pw + " expect")), pw + " number"});
                        }
                    }
                    // A name counts as changed unless it equals the pack's own "expect" for that field.
                    bool renamed = false;
                    if (p.contains("last")) {
                        std::string last = str_field(p, "last", 15, true, pw);
                        pack.writes.push_back({rec + player_last, text_field(last, 16)});
                        renamed |= expect_last.empty() || !iequal(last, expect_last);
                    }
                    if (p.contains("first")) {
                        std::string first = str_field(p, "first", 15, true, pw);
                        pack.writes.push_back({rec + player_first, text_field(first, 16)});
                        renamed |= expect_first.empty() || !iequal(first, expect_first);
                    }
                    if (p.contains("number")) {
                        pack.writes.push_back({rec + player_number, be32(bcd(int_field(p, "number", 0, 99, pw)))});
                    }
                    if (p.contains("size")) {
                        if (!p["size"].is_number()) {
                            fail(pw + ": \"size\" must be a number");
                        }
                        float size = p["size"].get<float>();
                        if (!(size >= 0.9f && size <= 1.3f)) {
                            fail(pw + ": \"size\" must be 0.90-1.30 (vanilla 1.05)");
                        }
                        uint32_t bits;
                        std::memcpy(&bits, &size, 4);
                        pack.writes.push_back({rec + player_size, be32(bits)});
                    }
                    if (p.contains("skin")) {
                        pack.writes.push_back({rec + player_skin, be32((uint32_t)int_field(p, "skin", 0, 5, pw))});
                    }
                    if (p.contains("body")) {
                        pack.writes.push_back({rec + player_body, be32((uint32_t)int_field(p, "body", 0, 1, pw))});
                    }
                    // The announcer calls some players by name (sample id at +0x18). A renamed player
                    // must not keep the old name call; 0 turns off the direct name call.
                    std::string call = p.value("announcer_call", renamed ? "none" : "keep");
                    if (call != "none" && call != "keep") {
                        fail(pw + ": \"announcer_call\" must be \"none\" or \"keep\"");
                    }
                    if (call == "keep" && renamed) {
                        fail(pw + ": a renamed player can't keep the old announcer name call (use \"none\")");
                    }
                    if (call == "none") {
                        pack.writes.push_back({rec + player_call, be32(0)});
                    }
                }
            }
        }
        return pack;
    }

    std::filesystem::path find_pack_file() {
        if (const char* env = getenv("BLITZ_PACK"); env != nullptr && *env != '\0') {
            std::filesystem::path p{env};
            return blitz::is_folder(p) ? p / "pack.json" : p;
        }
        std::filesystem::path packs = recomp::get_config_path() / "packs";
        std::ifstream active_file{packs / "active.txt"};
        std::string id;
        if (!active_file || !std::getline(active_file, id)) {
            return {};
        }
        id.erase(std::remove_if(id.begin(), id.end(), [](char c) { return std::isspace((unsigned char)c); }), id.end());
        if (id.empty()) {
            return {};
        }
        return packs / id / "pack.json";
    }

    // How a pack is named to the player: its folder (packs/<id>/pack.json) or file name.
    std::string pack_label(const std::filesystem::path& file) {
        return file.filename() == "pack.json" && file.has_parent_path() ? file.parent_path().filename().string() : file.filename().string();
    }

    void load_pack() {
        std::filesystem::path file = find_pack_file();
        if (file.empty()) {
            return;  // Classic
        }
        try {
            std::ifstream in{file};
            if (!in) {
                fail("can't open " + file.string());
            }
            json root;
            try {
                root = json::parse(in);
            }
            catch (const json::exception& e) {
                fail(std::string("not valid JSON: ") + e.what());
            }
            Pack pack = parse_pack(root);
            fprintf(stderr, "[pack] '%s' (%s): %zu field writes, %zu checks\n", pack.title.c_str(), pack.id.c_str(), pack.writes.size(), pack.expects.size());
            active = std::move(pack);
        }
        catch (const PackError& e) {
            blitz::post_notice("Content pack \"" + pack_label(file) + "\" wasn't loaded: " + e.msg + ". Playing Classic.");
            fprintf(stderr, "[pack] file: %s\n", file.string().c_str());
        }
        catch (const json::exception& e) {
            blitz::post_notice("Content pack \"" + pack_label(file) + "\" wasn't loaded: " + e.what() + ". Playing Classic.");
            fprintf(stderr, "[pack] file: %s\n", file.string().c_str());
        }
    }

    // Guest addresses for the MEM_ macros: sign-extended, the way the recompiled code holds them.
    // (An int32_t plus an unsigned offset would be unsigned arithmetic and lose the sign extension.)
    gpr guest(uint32_t vaddr) { return (gpr)(int32_t)vaddr; }

    std::string read_cstr(uint8_t* rdram, uint32_t vaddr, uint32_t len) {
        std::string s;
        for (int i = 0; i < (int)len; i++) {
            char c = (char)MEM_B(i, guest(vaddr));
            if (c == '\0') {
                break;
            }
            s += c;
        }
        return s;
    }

    uint32_t read_u32(uint8_t* rdram, uint32_t vaddr) { return (uint32_t)MEM_W(0, guest(vaddr)); }

    // The tables must look like vanilla v1.1 (and match the pack's expectations) before anything is written.
    std::optional<std::string> check_tables(uint8_t* rdram, const Pack& pack) {
        if (read_cstr(rdram, team_table + team_nickname, 32) != "Cardinals" ||
            read_u32(rdram, team_table + team_roster) != roster_table ||
            read_cstr(rdram, team_table + 30 * team_stride + team_nickname, 32) != "Redskins") {
            return "the game's team tables aren't where this build expects them";
        }
        for (const Expect& e : pack.expects) {
            if (e.is_string ? !iequal(read_cstr(rdram, e.vaddr, e.len), e.text) : read_u32(rdram, e.vaddr) != e.value) {
                return e.what + " isn't what the pack expects";
            }
        }
        return std::nullopt;
    }
}

namespace blitz {
    // "" while playing Classic. Used to keep each pack's saves apart (controller_pak.cpp).
    std::string active_pack_id() {
        std::call_once(load_once, load_pack);
        return active ? active->id : std::string{};
    }
}

// The team store on the original game (team_store.h): BLITZ_ENGINE_ROUNDTRIP=1 loads every team from the ROM and
// writes each segment's tables back as it loads (a test: the game must see exactly what it would have).
namespace {
    void engine_roundtrip(uint8_t* rdram, uint32_t rom_end) {
        static const bool on = getenv("BLITZ_ENGINE_ROUNDTRIP") != nullptr;
        if (!on) return;
        namespace orig = blitz::teamdata::original;
        static bool loaded = false;
        if (!loaded) {
            loaded = true;
            auto r = recomp::get_rom();
            orig::load(blitz::teamdata::store(), std::vector<uint8_t>(r.begin(), r.end()), {});
            fprintf(stderr, "[teams] round trip: %zu teams from the ROM\n", blitz::teamdata::store().slots().size());
            if (getenv("BLITZ_ENGINE_MUTATE")) {  // the test's own check that writes land
                auto& t = const_cast<blitz::teamdata::Team&>(blitz::teamdata::store().slots()[0]);
                t.set_select("TESTVILLE");
                t.set_abbr("TST");
                t.set_last(3, "Probe");
            }
        }
        if (rom_end == ovl_b_rom_end) {
            orig::write(rdram, orig::Segment::Main, blitz::teamdata::store());
            orig::write(rdram, orig::Segment::OvlB, blitz::teamdata::store());
        }
        if (rom_end == 0xDE350u) orig::write(rdram, orig::Segment::OvlA, blitz::teamdata::store());
    }
}

// dma_read finished (hook at 0x80001748): rom_end/ram_end are one past the last byte transferred.
extern "C" void blitz_on_dma_done(uint8_t* rdram, uint32_t rom_end, int32_t ram_end) {
    engine_roundtrip(rdram, rom_end);
    if (rom_end != ovl_b_rom_end || (uint32_t)ram_end != ovl_b_ram_end) {
        return;
    }
    std::call_once(load_once, load_pack);
    if (!active) {
        return;
    }
    if (auto problem = check_tables(rdram, *active)) {
        if (!reported_mismatch) {
            reported_mismatch = true;
            blitz::post_notice("Content pack '" + active->title + "' wasn't applied: " + *problem + ". Playing Classic.");
        }
        return;
    }
    for (const Write& w : active->writes) {
        for (int i = 0; i < (int)w.bytes.size(); i++) {
            MEM_B(i, guest(w.vaddr)) = (int8_t)w.bytes[i];
        }
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        fprintf(stderr, "[pack] applied '%s' to the ovl_b team tables (%zu writes)\n", active->title.c_str(), active->writes.size());
    }
}
#endif
