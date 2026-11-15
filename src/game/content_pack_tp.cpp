// Content packs for the 2026 flavour ("blitz-pack/2"; docs/BLITZ2026.md, packs/README.md).
//
// team+ keeps everything about a team in one 0x220-byte block in ovl_b's data (32 used, up to 60):
// 16 player records, uniform/logo/endzone asset indices, CPU play lists, CPU skill, ratings and
// names. The only per-team data outside the blocks is the uniform-clash bit matrix. Like the
// Classic loader, packs are applied right after every ovl_b DMA (hook at dma_read 0x80001748);
// ovl_b's copier later hands the chosen teams to gameplay.
//
// Teams are identified by franchise (from the nickname in the loaded ROM: "Raiders" is LV whether
// the ROM says Oakland or Las Vegas), so a pack works on team+ itself or on a community update
// built on it. A pack can also set the menu order: blocks are permuted whole and the clash matrix
// with them, so every per-team setting stays with its franchise.
#if BLITZ_MODERN
#include "../main/platform_compat.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "json/json.hpp"

#include "recomp.h"
#include "librecomp/game.hpp"
#include "util/file.h"
#include "season_data.h"
#include "season_pak.h"
#include "rosters_live.h"
#include "team_store.h"
namespace blitz {
    std::map<int, LineupPlayer> crew_players(const std::string& franchise);  // crew.cpp: the saved crew, slot by slot
}

namespace blitz {
    void post_notice(const std::string& msg);  // notices.cpp
    bool art_active();                          // art_dropin.cpp
    int announcer_vs_pause(const std::string& bank);  // art_dropin.cpp
    const std::vector<uint8_t>* art_assets_of(const std::string& franchise);  // art_dropin.cpp
    bool advanced_mode();                       // adv_mode.cpp
    bool rom_rebranded();                       // art_dropin.cpp
    uint32_t rosters_setting();                 // season_settings.cpp: 0 Auto, 1 Blitz 2026, 2 the ROM's own, 3+ another pack
    std::string rosters_pack_id();              // season_settings.cpp: that pack's id
    void adv_on_ovl_b(uint8_t* rdram, const std::vector<std::string>& franchise_of_slot);  // adv_season.cpp
    bool fs_register_file(const std::string& name, std::vector<uint8_t> data);  // art_dropin.cpp: added game files
    int32_t fs_file_index(const std::string& name);
    uint32_t fs_ram_count(uint8_t* rdram);
    void s32_on_ovl_b(uint8_t* rdram, const std::vector<std::string>& franchise_of_slot);  // season32.cpp
}

namespace {
    using json = nlohmann::json;

    constexpr uint32_t block_base = 0x80330C9C, block_size = 0x220, max_blocks = 60;
    constexpr uint32_t clash_matrix = 0x8032D5E0, clash_row = 16;
    constexpr uint32_t ovl_b_rom_end = 0x129A90, ovl_b_ram_end = 0x8033B5B0;

    // Block layout (docs/BLITZ2026.md).
    constexpr uint32_t player_size = 0x18, players_per_team = 16;
    constexpr uint32_t p_scale = 0x0, p_heavy = 0x4, p_skin = 0x5, p_number = 0x6, p_call = 0x7, p_last = 0x8;
    constexpr uint32_t t_ratings = 0x1D0, t_vs_pause = 0x1D6, t_name = 0x1E0, t_select = 0x1F0, t_city = 0x200, t_abbr = 0x210;
    constexpr uint32_t t_uniform = 0x180, t_cpu_skill = 0x1D5, t_vs_right = 0x21E;  // uniform: 10 parts x (home, away) u16 file ids
    // League paks (build/analysis/league_paks.md): the team count team select and the attract demo read
    // (bytes 0 and 2; tp_count_teams recounts byte 0 from the blocks), and the 1-player CPU ladder.
    constexpr uint32_t team_count_bytes = 0x800248FC, ladder_tiers = 0x802AB0E0, ladder_done_teams = 0x80338C28;

    struct Franchise {
        const char* id;
        std::vector<const char*> nicknames;  // as found in team+ / community ROMs (team +0x1E0)
    };
    const std::vector<Franchise> franchises = {
        {"ARI", {"Cardinals"}}, {"ATL", {"Falcons"}}, {"BAL", {"Ravens"}}, {"BUF", {"Bills"}},
        {"CAR", {"Panthers"}}, {"CHI", {"Bears"}}, {"CIN", {"Bengals"}}, {"CLE", {"Browns"}},
        {"DAL", {"Cowboys"}}, {"DEN", {"Broncos"}}, {"DET", {"Lions"}}, {"GB", {"Packers"}},
        {"HOU", {"Texans"}}, {"IND", {"Colts"}}, {"JAX", {"Jaguars"}}, {"KC", {"Chiefs"}},
        {"LV", {"Raiders"}}, {"LAC", {"Chargers"}}, {"LAR", {"Rams"}}, {"MIA", {"Dolphins"}},
        {"MIN", {"Vikings"}}, {"NE", {"Patriots"}}, {"NO", {"Saints"}}, {"NYG", {"Giants"}},
        {"NYJ", {"Jets"}}, {"PHI", {"Eagles"}}, {"PIT", {"Steelers"}}, {"SF", {"49ers"}},
        {"SEA", {"Seahawks"}}, {"TB", {"Buccaneers"}}, {"TEN", {"Titans", "Oilers"}},
        {"WAS", {"Redskins", "Commanders"}},
    };
    const char* const rating_names[5] = {"passing", "rushing", "linemen", "defense", "special_teams"};

    struct PlayerEdit {
        int slot;
        std::optional<std::string> last;
        std::optional<int> number;
        std::optional<float> size;
        std::optional<int> skin, heavy;
        bool clear_call;
        std::optional<std::string> expect_last;
    };
    struct TeamEdit {
        std::string franchise;
        std::optional<std::string> city, nickname, select, abbr;
        std::array<std::optional<int>, 5> ratings;
        std::vector<PlayerEdit> players;
    };
    // A league pak's team: a whole new team in slot k, built on an NFL franchise's block (CPU plays,
    // skill, uniforms and art). Its images come from the pak's files/ folder, named after the base
    // team's files (no NFL team is on the field in a league pak, so replacing theirs is safe).
    struct LeagueTeam {
        std::string id, base, city, nickname, select, abbr;
        std::string announcer = "none";  // "none" (silent) or "base" (the base franchise's calls)
        std::optional<int> cpu_skill;
        std::optional<std::string> uniform_home, uniform_away;  // "PIT", "PIT.home", "PIT.away"
        std::vector<std::string> clashes_with;
        std::optional<std::string> vs_helmet_right;  // files/added/<name>.wms: its own right-side VS helmet
        std::map<uint32_t, std::string> uniform_files;  // part*2 + side (0 home, 1 away) -> files/added/<name>.wms
        std::array<std::optional<int>, 5> ratings;
        std::vector<PlayerEdit> players;
    };
    struct League {
        std::string name;
        bool tournament = false, play_editor = false;  // Arcade is always on, Season never
        std::vector<std::vector<std::string>> ladder;  // CPU tiers, easiest first
        std::vector<std::string> ladder_done;          // the 4 picks once every team is beaten
        std::vector<LeagueTeam> teams;
    };
    struct Pack {
        std::string id, title, version;
        std::vector<std::string> order;  // franchise ids in menu order (empty: keep the ROM's)
        std::vector<TeamEdit> teams;
        std::optional<blitz::SeasonData> season;
        std::optional<League> league;
    };

    std::once_flag load_once;
    std::optional<Pack> active;
    bool reported = false;

    struct PackError {
        std::string msg;
    };
    [[noreturn]] void fail(const std::string& msg) { throw PackError{msg}; }

    bool known_franchise(const std::string& id) {
        return std::any_of(franchises.begin(), franchises.end(), [&](const Franchise& f) { return id == f.id; });
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

    bool drawable(const std::string& s, bool name) {
        return std::all_of(s.begin(), s.end(), [&](char c) {
            return std::isalpha((unsigned char)c) || (!name && std::isdigit((unsigned char)c)) || c == ' ' || c == '.' || c == '\'' || c == '-';
        });
    }

    std::string text(const json& obj, const char* key, size_t max_len, bool name, const std::string& where) {
        if (!obj.is_object() || !obj.contains(key)) {  // const operator[] on a missing key is undefined
            fail(where + ": missing \"" + key + "\"");
        }
        if (!obj[key].is_string()) {
            fail(where + ": \"" + key + "\" must be a string");
        }
        std::string s = obj[key].get<std::string>();
        if (s.empty() || s.size() > max_len) {
            fail(where + ": \"" + key + "\" must be 1-" + std::to_string(max_len) + " characters (\"" + s + "\")");
        }
        if (!drawable(s, name)) {
            fail(where + ": \"" + key + "\" has characters the game can't draw (\"" + s + "\")");
        }
        return s;
    }

    int integer(const json& obj, const char* key, int lo, int hi, const std::string& where) {
        if (!obj.is_object() || !obj.contains(key)) {
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

    // "season": {"year", "divisions": 8 x {"name", "teams": 4 franchises} (AFC East, North, South,
    // West, then the NFC's), "schedule": up to 18 weeks of [away, home] pairs}.
    blitz::SeasonData parse_season(const json& s) {
        using blitz::SeasonData;
        SeasonData out;
        only_keys(s, {"year", "divisions", "schedule"}, "season");
        out.year = integer(s, "year", 2000, 2100, "season");
        if (!s.contains("divisions") || !s["divisions"].is_array() || s["divisions"].size() != SeasonData::division_count) {
            fail("season: \"divisions\" must list 8 divisions (AFC East, North, South, West, then the NFC's)");
        }
        std::vector<std::string> placed;
        for (int d = 0; d < SeasonData::division_count; d++) {
            const json& div = s["divisions"][d];
            std::string where = "season division " + std::to_string(d + 1);
            only_keys(div, {"name", "teams"}, where);
            out.division_names[d] = text(div, "name", 15, false, where);
            if (!div.contains("teams") || !div["teams"].is_array() || div["teams"].size() != SeasonData::division_size) {
                fail(where + ": \"teams\" must list 4 franchises");
            }
            for (int k = 0; k < SeasonData::division_size; k++) {
                const json& f = div["teams"][k];
                if (!f.is_string() || !known_franchise(f.get<std::string>())) {
                    fail(where + ": unknown franchise " + f.dump());
                }
                std::string id = f.get<std::string>();
                if (std::find(placed.begin(), placed.end(), id) != placed.end()) {
                    fail("season: " + id + " is in two divisions");
                }
                placed.push_back(id);
                out.divisions[d][k] = id;
            }
        }
        if (!s.contains("schedule") || !s["schedule"].is_array() || s["schedule"].empty() || s["schedule"].size() > SeasonData::max_weeks) {
            fail("season: \"schedule\" must list 1-18 weeks");
        }
        for (size_t w = 0; w < s["schedule"].size(); w++) {
            const json& week = s["schedule"][w];
            std::string where = "season week " + std::to_string(w + 1);
            if (!week.is_array() || week.size() > SeasonData::max_games) {
                fail(where + " must be a list of at most 16 games");
            }
            std::vector<std::string> playing;
            auto& games = out.weeks.emplace_back();
            for (const json& g : week) {
                if (!g.is_array() || g.size() != 2 || !g[0].is_string() || !g[1].is_string() ||
                    !known_franchise(g[0].get<std::string>()) || !known_franchise(g[1].get<std::string>())) {
                    fail(where + ": each game must be [away, home] franchise ids (got " + g.dump() + ")");
                }
                for (const json& f : g) {
                    if (std::find(playing.begin(), playing.end(), f.get<std::string>()) != playing.end()) {
                        fail(where + ": " + f.get<std::string>() + " plays twice");
                    }
                    playing.push_back(f.get<std::string>());
                }
                games.emplace_back(g[0].get<std::string>(), g[1].get<std::string>());
            }
        }
        return out;
    }

    std::vector<PlayerEdit> parse_players(const json& t, const std::string& where) {
        std::vector<PlayerEdit> out;
        if (t.contains("players")) {
            if (!t["players"].is_array()) {
                fail(where + ": \"players\" must be a list");
            }
            for (const json& p : t["players"]) {
                only_keys(p, {"slot", "expect", "last", "number", "size", "skin", "heavy", "announcer_call"}, where + " player");
                PlayerEdit pe{};
                pe.slot = integer(p, "slot", 0, players_per_team - 1, where + " player");
                std::string pw = where + " slot " + std::to_string(pe.slot);
                if (std::any_of(out.begin(), out.end(), [&](const PlayerEdit& o) { return o.slot == pe.slot; })) {
                    fail(pw + " appears twice");
                }
                if (p.contains("expect")) {
                    only_keys(p["expect"], {"last"}, pw + " expect");
                    pe.expect_last = text(p["expect"], "last", 15, true, pw + " expect");
                }
                if (p.contains("last")) pe.last = text(p, "last", 15, true, pw);
                if (p.contains("number")) pe.number = integer(p, "number", 0, 99, pw);
                if (p.contains("skin")) pe.skin = integer(p, "skin", 0, 5, pw);  // 0 Darker, 1 Light, 2-4 in between, 5 Lighter (skin_tones.cpp)
                if (p.contains("heavy")) pe.heavy = integer(p, "heavy", 0, 1, pw);
                if (p.contains("size")) {
                    if (!p["size"].is_number()) {
                        fail(pw + ": \"size\" must be a number");
                    }
                    float s = p["size"].get<float>();
                    if (!(s >= 0.9f && s <= 1.3f)) {
                        fail(pw + ": \"size\" must be 0.90-1.30 (vanilla 1.05)");
                    }
                    pe.size = s;
                }
                // A renamed player must not keep the old name call (+7, the announcer sample id).
                std::string call = p.value("announcer_call", pe.last ? "none" : "keep");
                if (call != "none" && call != "keep") {
                    fail(pw + ": \"announcer_call\" must be \"none\" or \"keep\"");
                }
                pe.clear_call = call == "none";
                out.push_back(pe);
            }
        }
        return out;
    }

    // "league": {"name", "modes": ["arcade", "tournament"?, "play_editor"?], "ladder"?: tiers of team ids,
    // "ladder_done"?: 4 ids}, with "teams" as the complete list of the league's teams (2-31).
    League parse_league(const json& l, const json& teams) {
        League out;
        only_keys(l, {"name", "modes", "ladder", "ladder_done"}, "league");
        out.name = text(l, "name", 15, false, "league");
        if (l.contains("modes")) {
            if (!l["modes"].is_array()) fail("league: \"modes\" must be a list");
            for (const json& m : l["modes"]) {
                std::string mode = m.is_string() ? m.get<std::string>() : "";
                if (mode == "tournament") out.tournament = true;
                else if (mode == "play_editor") out.play_editor = true;
                else if (mode != "arcade") fail("league: modes are \"arcade\", \"tournament\" and \"play_editor\" (got " + m.dump() + ")");
            }
        }
        if (!teams.is_array() || teams.size() < 2 || teams.size() > 31) {
            fail("a league pak's \"teams\" must list 2-31 teams");
        }
        for (const json& t : teams) {
            only_keys(t, {"id", "base", "city", "nickname", "select", "abbr", "announcer", "cpu_skill", "uniform", "uniform_files", "clashes_with", "vs_helmet_right", "ratings", "players"}, "league team");
            LeagueTeam lt;
            lt.id = text(t, "id", 4, false, "league team");
            std::string where = "team " + lt.id;
            if (known_franchise(lt.id)) fail(where + ": use an id that isn't an NFL franchise's");
            if (std::any_of(out.teams.begin(), out.teams.end(), [&](const LeagueTeam& o) { return o.id == lt.id; })) fail(where + " appears twice");
            if (!t.contains("base") || !t["base"].is_string() || !known_franchise(t["base"].get<std::string>())) {
                fail(where + ": \"base\" must be an NFL franchise id (ARI ... WAS): its CPU plays, skill and art");
            }
            lt.base = t["base"].get<std::string>();
            lt.city = text(t, "city", 15, false, where);
            lt.nickname = text(t, "nickname", 15, false, where);
            lt.select = t.contains("select") ? text(t, "select", 15, false, where) : lt.city;
            lt.abbr = text(t, "abbr", 3, false, where);
            if (t.contains("announcer")) {
                lt.announcer = t["announcer"].is_string() ? t["announcer"].get<std::string>() : "";
                if (lt.announcer != "none" && lt.announcer != "base") fail(where + ": \"announcer\" must be \"none\" or \"base\"");
            }
            if (t.contains("cpu_skill")) lt.cpu_skill = integer(t, "cpu_skill", 3, 11, where);
            if (t.contains("uniform_files")) {
                // "jersey_home": "bsu_jer_h.wms", ...: the team's own uniform textures (files/added), same sizes as the base's.
                static const char* const parts[10] = {"helmet_side", "helmet_stripe_front", "helmet_stripe_back", "facemask", "shoulder",
                                                      "jersey", "pant_stripe", "pants", "socks", "numbers"};
                if (!t["uniform_files"].is_object()) fail(where + ": \"uniform_files\" must be an object");
                for (auto& [key, val] : t["uniform_files"].items()) {
                    std::string f = val.is_string() ? val.get<std::string>() : "";
                    for (char& c : f) c = (char)std::tolower((unsigned char)c);
                    bool found = false;
                    for (uint32_t k = 0; k < 10 && !found; k++) {
                        for (uint32_t side = 0; side < 2 && !found; side++) {
                            if (key == std::string(parts[k]) + (side ? "_away" : "_home")) {
                                lt.uniform_files[k * 2 + side] = f;
                                found = true;
                            }
                        }
                    }
                    if (!found) fail(where + ": unknown uniform part \"" + key + "\" (e.g. jersey_home, pants_away)");
                    if (f.size() < 5 || f.size() > 14 || f.substr(f.size() - 4) != ".wms") fail(where + " " + key + ": a .wms in files/added, at most 14 characters");
                }
            }
            if (t.contains("vs_helmet_right")) {
                std::string f = t["vs_helmet_right"].is_string() ? t["vs_helmet_right"].get<std::string>() : "";
                for (char& c : f) c = (char)std::tolower((unsigned char)c);
                if (f.size() < 5 || f.size() > 14 || f.substr(f.size() - 4) != ".wms") {
                    fail(where + ": \"vs_helmet_right\" names a .wms in files/added, at most 14 characters");
                }
                lt.vs_helmet_right = f;
            }
            if (t.contains("uniform")) {
                only_keys(t["uniform"], {"home", "away"}, where + " uniform");
                for (const char* side : {"home", "away"}) {
                    if (!t["uniform"].contains(side)) continue;
                    const json& u = t["uniform"][side];
                    std::string v = u.is_string() ? u.get<std::string>() : "";
                    std::string f = v.substr(0, v.find('.')), part = v.find('.') == std::string::npos ? "" : v.substr(v.find('.') + 1);
                    if (!known_franchise(f) || (part != "" && part != "home" && part != "away")) {
                        fail(where + " uniform: \"" + side + "\" must be a franchise id, optionally with .home or .away (got " + u.dump() + ")");
                    }
                    (std::string(side) == "home" ? lt.uniform_home : lt.uniform_away) = v;
                }
            }
            if (t.contains("ratings")) {
                only_keys(t["ratings"], {"passing", "rushing", "linemen", "defense", "special_teams"}, where + " ratings");
                for (int k = 0; k < 5; k++) {
                    if (t["ratings"].contains(rating_names[k])) lt.ratings[k] = integer(t["ratings"], rating_names[k], 0, 5, where + " ratings");
                }
            }
            lt.players = parse_players(t, where);
            if (std::none_of(lt.players.begin(), lt.players.end(), [](const PlayerEdit& pe) { return pe.slot == 3 && pe.last; })) {
                fail(where + ": name at least the quarterback (slot 3), who stands at team select");
            }
            out.teams.push_back(std::move(lt));
        }
        auto team_id = [&](const json& v, const std::string& where) {
            std::string id = v.is_string() ? v.get<std::string>() : "";
            if (std::none_of(out.teams.begin(), out.teams.end(), [&](const LeagueTeam& t) { return t.id == id; })) fail(where + ": unknown team " + v.dump());
            return id;
        };
        for (size_t k = 0; k < out.teams.size(); k++) {
            if (!teams[k].contains("clashes_with")) continue;
            if (!teams[k]["clashes_with"].is_array()) fail("team " + out.teams[k].id + ": \"clashes_with\" must be a list of team ids");
            for (const json& c : teams[k]["clashes_with"]) out.teams[k].clashes_with.push_back(team_id(c, "team " + out.teams[k].id + " clashes_with"));
        }
        if (l.contains("ladder")) {
            if (!l["ladder"].is_array() || l["ladder"].empty() || l["ladder"].size() > 9) fail("league: \"ladder\" must list 1-9 tiers");
            for (const json& tier : l["ladder"]) {
                if (!tier.is_array() || tier.empty() || tier.size() > 4) fail("league: each ladder tier lists 1-4 teams");
                auto& row = out.ladder.emplace_back();
                for (const json& v : tier) row.push_back(team_id(v, "league ladder"));
            }
        }
        if (l.contains("ladder_done")) {
            if (!l["ladder_done"].is_array() || l["ladder_done"].size() != 4) fail("league: \"ladder_done\" must list 4 teams");
            for (const json& v : l["ladder_done"]) out.ladder_done.push_back(team_id(v, "league ladder_done"));
        }
        return out;
    }

    Pack parse(const json& root) {
        Pack pack;
        only_keys(root, {"format", "id", "title", "version", "requires_rom", "order", "teams", "season", "league"}, "pack");
        if (root.value("format", "") != "blitz-pack/2") {
            fail("\"format\" must be \"blitz-pack/2\" for the 2026 build");
        }
        if (root.value("requires_rom", "") != "blitz2000-teamplus") {
            fail("\"requires_rom\" must be \"blitz2000-teamplus\"");
        }
        pack.id = root.value("id", "");
        if (pack.id.empty() || pack.id.size() > 40 ||
            !std::all_of(pack.id.begin(), pack.id.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '-' || c == '_'; })) {
            fail("\"id\" must be 1-40 letters, digits, '-' or '_'");
        }
        pack.title = root.value("title", pack.id);
        if (root.contains("version") && !root["version"].is_string()) {
            fail("\"version\" must be text, e.g. \"2026.10.16\"");
        }
        pack.version = root.value("version", "");

        if (root.contains("league")) {
            if (root.contains("order") || root.contains("season")) {
                fail("a league pak has no \"order\" or \"season\" (it plays Arcade, not Season)");
            }
            pack.league = parse_league(root["league"], root.contains("teams") ? root["teams"] : json::array());
            return pack;
        }

        if (root.contains("order")) {
            if (!root["order"].is_array()) {
                fail("\"order\" must be a list of franchise ids");
            }
            for (const json& f : root["order"]) {
                if (!f.is_string() || !known_franchise(f.get<std::string>())) {
                    fail("\"order\": unknown franchise " + f.dump());
                }
                std::string id = f.get<std::string>();
                if (std::find(pack.order.begin(), pack.order.end(), id) != pack.order.end()) {
                    fail("\"order\": " + id + " appears twice");
                }
                pack.order.push_back(id);
            }
            if (pack.order.size() != franchises.size()) {
                fail("\"order\" must list all " + std::to_string(franchises.size()) + " franchises");
            }
        }

        if (root.contains("teams")) {
            if (!root["teams"].is_array()) {
                fail("\"teams\" must be a list");
            }
            for (const json& t : root["teams"]) {
                only_keys(t, {"franchise", "city", "nickname", "select", "abbr", "ratings", "players"}, "team entry");
                if (!t.contains("franchise") || !t["franchise"].is_string() || !known_franchise(t["franchise"].get<std::string>())) {
                    fail("team entry: \"franchise\" must be one of ARI ATL ... WAS");
                }
                TeamEdit te;
                te.franchise = t["franchise"].get<std::string>();
                std::string where = "team " + te.franchise;
                if (std::any_of(pack.teams.begin(), pack.teams.end(), [&](const TeamEdit& o) { return o.franchise == te.franchise; })) {
                    fail(where + " appears twice");
                }
                if (t.contains("city")) te.city = text(t, "city", 15, false, where);
                if (t.contains("nickname")) te.nickname = text(t, "nickname", 15, false, where);
                if (t.contains("select")) te.select = text(t, "select", 15, false, where);
                if (t.contains("abbr")) te.abbr = text(t, "abbr", 3, false, where);
                if (t.contains("ratings")) {
                    only_keys(t["ratings"], {"passing", "rushing", "linemen", "defense", "special_teams"}, where + " ratings");
                    for (int k = 0; k < 5; k++) {
                        if (t["ratings"].contains(rating_names[k])) {
                            te.ratings[k] = integer(t["ratings"], rating_names[k], 0, 5, where + " ratings");
                        }
                    }
                }
                te.players = parse_players(t, where);
                pack.teams.push_back(std::move(te));
            }
        }
        if (root.contains("season")) {
            pack.season = parse_season(root["season"]);
        }
        return pack;
    }

    // A pack the app carries: the installed season pak's, else the bundle's (season_pak.h content_dirs; the 2026
    // Season pak brings packs/blitz2026 to a public build, 2026-11-13). Empty when neither has it.
    std::filesystem::path content_pack_file(const std::string& id) {
        for (const std::filesystem::path& root : blitz::content_dirs_newest_first()) {
            std::filesystem::path f = root / "packs" / id / "pack.json";
            if (blitz::file_exists(f)) return f;
        }
        return {};
    }

    std::filesystem::path find_pack_file() {
        if (const char* env = getenv("BLITZ_PACK"); env != nullptr && *env != '\0') {
            std::filesystem::path p{env};
            return blitz::is_folder(p) ? p / "pack.json" : p;
        }
        // <app folder>/packs/active.txt picks a pack by id ("none" = the ROM's own teams), for pack authors.
        // Without it, Settings → Season → Rosters decides between the built-in Blitz 2026 roster pack
        // (Resources/packs/blitz2026) and the ROM's own teams.
        std::filesystem::path packs = recomp::get_config_path() / "packs";
        std::ifstream active_file{packs / "active.txt"};
        std::string id;
        if (active_file && std::getline(active_file, id)) {
            id.erase(std::remove_if(id.begin(), id.end(), [](char c) { return std::isspace((unsigned char)c); }), id.end());
        }
        if (id.empty()) {
            uint32_t rosters = blitz::rosters_setting();
            id = blitz::rosters_pack_id();
            bool own = id.empty() && (rosters == 2 || (rosters == 0 && blitz::rom_rebranded()));
            fprintf(stderr, "[pack] rosters: %s\n", own ? "the ROM's own" : !id.empty() ? id.c_str()
                    : blitz::season_content() ? "Blitz 2026 (built in)" : "Blitz 2026, not here: the ROM's own (no 2026 Season)");
            if (own) return {};
        }
        if (id == "none") {
            return {};
        }
        if (!id.empty()) {
            std::filesystem::path user = packs / id / "pack.json";
            if (blitz::file_exists(user)) return user;
            std::filesystem::path bundled = content_pack_file(id);
            return bundled.empty() ? user : bundled;
        }
        return content_pack_file("blitz2026");
    }

    std::string label(const std::filesystem::path& file) {
        return file.filename() == "pack.json" && file.has_parent_path() ? file.parent_path().filename().string() : file.filename().string();
    }

    void load_pack() {
        std::filesystem::path file = find_pack_file();
        if (file.empty()) {
            return;
        }
        try {
            std::ifstream in{file};
            if (!in) {
                fail("can't open it");
            }
            json root;
            try {
                root = json::parse(in);
            }
            catch (const json::exception& e) {
                fail(std::string("not valid JSON: ") + e.what());
            }
            active = parse(root);
            fprintf(stderr, "[pack] '%s' (%s%s%s): %zu %s\n", active->title.c_str(), active->id.c_str(),
                    active->version.empty() ? "" : ", version ", active->version.c_str(),
                    active->league ? active->league->teams.size() : active->teams.size(), active->league ? "league teams" : "team edits");
        }
        catch (const PackError& e) {
            blitz::post_notice("Content pack \"" + label(file) + "\" wasn't loaded: " + e.msg + ". Playing without it.");
        }
        catch (const json::exception& e) {
            blitz::post_notice("Content pack \"" + label(file) + "\" wasn't loaded: " + e.what() + ". Playing without it.");
        }
    }

    // Guest memory access (sign-extended KSEG0 addresses, as the recompiled code holds them).
    gpr guest(uint32_t vaddr) { return (gpr)(int32_t)vaddr; }
    uint8_t rd8(uint8_t* rdram, uint32_t a) { return (uint8_t)MEM_B(0, guest(a)); }
    void wr8(uint8_t* rdram, uint32_t a, uint8_t v) { MEM_B(0, guest(a)) = (int8_t)v; }

    std::string rd_text(uint8_t* rdram, uint32_t a, uint32_t len) {
        std::string s;
        for (uint32_t i = 0; i < len; i++) {
            char c = (char)rd8(rdram, a + i);
            if (c == '\0') break;
            s += c;
        }
        return s;
    }
    void wr_text(uint8_t* rdram, uint32_t a, const std::string& s, uint32_t len) {
        for (uint32_t i = 0; i < len; i++) {
            wr8(rdram, a + i, i < s.size() ? (uint8_t)s[i] : 0);
        }
    }

    std::optional<std::string> franchise_of(const std::string& nickname) {
        for (const Franchise& f : franchises) {
            for (const char* n : f.nicknames) {
                if (nickname == n) return std::string{f.id};
            }
        }
        return std::nullopt;
    }

    uint32_t block(uint32_t index) { return block_base + index * block_size; }


    std::vector<std::string> franchise_by_slot;  // after the latest apply(); written on the game thread only
    std::mutex franchise_mutex;                  // held for writes, and for team_franchises() (the Roster Room's thread)
    void set_franchises(std::vector<std::string> v) {
        std::lock_guard lock{franchise_mutex};
        franchise_by_slot = std::move(v);
    }

    // The pack's team edits on the store's records (both engines), and each replaced announcer bank's VS pause.
    void edit_teams(blitz::teamdata::Store& st, const Pack& pack) {
        for (const TeamEdit& te : pack.teams) {
            blitz::teamdata::Team* t = st.find(te.franchise);
            if (!t) continue;
            if (te.city) t->set_city(*te.city);
            if (te.nickname) t->set_nickname(*te.nickname);
            if (te.select) t->set_select(*te.select);
            if (te.abbr) t->set_abbr(*te.abbr);
            for (int k = 0; k < 5; k++) {
                if (te.ratings[k]) t->set_rating(k, *te.ratings[k]);
            }
            for (const PlayerEdit& pe : te.players) {
                if (pe.last) t->set_last(pe.slot, *pe.last);
                if (pe.number) t->set_number(pe.slot, *pe.number);
                if (pe.size) t->set_scale(pe.slot, *pe.size);
                if (pe.skin) t->set_skin(pe.slot, *pe.skin);
                if (pe.heavy) t->set_heavy(pe.slot, *pe.heavy);
                if (pe.clear_call) t->clear_call(pe.slot);
            }
        }
        // Replaced announcer calls: the VS pause after the full name follows the new call's length.
        for (const blitz::teamdata::Team& t0 : st.slots()) {
            blitz::teamdata::Team* t = st.find(t0.franchise);
            std::string bank = t->nickname().substr(0, 8);
            for (char& c : bank) c = (char)std::tolower((unsigned char)c);
            int pause = blitz::announcer_vs_pause(bank);
            if (pause > 0) t->set_vs_pause(pause);
        }
    }

    // Every expected name the pack checks (its players' "expect"), or the first that doesn't match.
    std::optional<std::string> check_expects(blitz::teamdata::Store& st, const Pack& pack) {
        for (const TeamEdit& te : pack.teams) {
            const blitz::teamdata::Team* t = st.find(te.franchise);
            for (const PlayerEdit& pe : te.players) {
                if (!pe.expect_last || !t) continue;
                std::string have = t->last(pe.slot);
                if (have != *pe.expect_last)
                    return te.franchise + " slot " + std::to_string(pe.slot) + " is \"" + have + "\", not \"" + *pe.expect_last + "\"";
            }
        }
        return std::nullopt;
    }

    void wr32(uint8_t* rdram, uint32_t a, uint32_t v) { MEM_W(0, guest(a)) = (int32_t)v; }

    // A league pak's teams, each built on its base franchise's record in `st` (the NFL teams as loaded): the records
    // and their clash matrix. Both engines (team+ blocks, the original game's tables) take them as they are.
    std::pair<std::vector<blitz::teamdata::Team>, std::vector<std::vector<bool>>> build_league(uint8_t* rdram, const League& lg,
                                                                                            blitz::teamdata::Store& st) {
        std::map<std::string, size_t> slot_of;  // franchise -> its index in the store
        {
            std::vector<std::string> by = st.franchise_of_slot();
            for (size_t i = 0; i < by.size(); i++) if (!by[i].empty()) slot_of[by[i]] = i;
        }
        const uint32_t n = (uint32_t)lg.teams.size();
        std::vector<std::string> owner(n);   // whose home uniform each team wears (for the clash matrix)
        std::vector<uint16_t> jersey(n);
        std::vector<bool> own_jersey(n, false);  // its own home jersey file: no NFL base clash rule
        std::vector<blitz::teamdata::Team> league(n);
        for (uint32_t k = 0; k < n; k++) {
            const LeagueTeam& t = lg.teams[k];
            blitz::teamdata::Team& lt = league[k];
            lt = *st.find(t.base);
            lt.franchise.clear();  // a league team, not an NFL franchise
            owner[k] = t.base;
            auto uniform = [&](const std::optional<std::string>& ref, int side) {
                if (!ref) return;
                std::string f = ref->substr(0, ref->find('.'));
                int from = ref->find(".away") != std::string::npos ? 1 : ref->find(".home") != std::string::npos ? 0 : side;
                const blitz::teamdata::Team* src = st.find(f);
                for (int part = 0; part < 10; part++) lt.set_uniform_part(part, side, src->uniform_part(part, from));
                if (side == 0) owner[k] = f;
            };
            uniform(t.uniform_home, 0);
            uniform(t.uniform_away, 1);
            for (auto& [slot, name] : t.uniform_files) {
                int32_t idx = blitz::fs_file_index(name);
                if (idx < 0 || (uint32_t)idx >= blitz::fs_ram_count(rdram)) continue;  // not added (or an old save state)
                lt.set_uniform_part((int)(slot / 2), (int)(slot % 2), (uint16_t)(idx + 1));
                if (slot == 5 * 2) own_jersey[k] = true;
            }
            jersey[k] = lt.uniform_part(5, 0);
            lt.set_city(t.city);
            lt.set_nickname(t.nickname);
            lt.set_select(t.select);
            lt.set_abbr(t.abbr);
            for (int i = 0; i < 5; i++) {
                if (t.ratings[i]) lt.set_rating(i, *t.ratings[i]);
            }
            if (t.cpu_skill) lt.set_cpu_skill(*t.cpu_skill);
            // The right side's VS helmet: its own image (facing left) instead of the left one mirrored.
            if (t.vs_helmet_right) {
                int32_t idx = blitz::fs_file_index(*t.vs_helmet_right);
                if (idx >= 0 && (uint32_t)idx < blitz::fs_ram_count(rdram)) lt.set_vs_helmet_right((uint16_t)(idx + 1));
            }
            // The base team's players go: unnamed slots are blank, and no NFL name calls.
            for (int slot = 0; slot < (int)players_per_team; slot++) {
                if (slot < 15) lt.set_last(slot, "");
                lt.clear_call(slot);
            }
            for (const PlayerEdit& pe : t.players) {
                if (pe.last) lt.set_last(pe.slot, *pe.last);
                if (pe.number) lt.set_number(pe.slot, *pe.number);
                if (pe.size) lt.set_scale(pe.slot, *pe.size);
                if (pe.skin) lt.set_skin(pe.slot, *pe.skin);
                if (pe.heavy) lt.set_heavy(pe.slot, *pe.heavy);
            }
        }
        // Uniform clashes: the same home jersey always clashes; otherwise as the owners' NFL teams do,
        // plus any the pak lists.
        auto listed = [&](uint32_t a, uint32_t b) {
            const auto& c = lg.teams[a].clashes_with;
            return std::find(c.begin(), c.end(), lg.teams[b].id) != c.end();
        };
        std::vector<std::vector<bool>> clash(n, std::vector<bool>(n, false));
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t j = i; j < n; j++)
                clash[i][j] = clash[j][i] = i == j || jersey[i] == jersey[j] || listed(i, j) || listed(j, i) ||
                                            (!own_jersey[i] && !own_jersey[j] && st.clashes(slot_of.at(owner[i]), slot_of.at(owner[j])));
        return {std::move(league), std::move(clash)};
    }

    // The 1-player CPU ladder (main, resident) and its last picks (ovl_b): the pak's tiers, or its teams two at a time
    // in pak order.
    void write_league_ladder(uint8_t* rdram, const League& lg) {
        const uint32_t n = (uint32_t)lg.teams.size();
        auto index_of = [&](const std::string& id) {
            for (uint32_t k = 0; k < n; k++) if (lg.teams[k].id == id) return (int32_t)k;
            return (int32_t)0;
        };
        std::vector<std::vector<int32_t>> tiers;
        for (const auto& tier : lg.ladder) {
            auto& row = tiers.emplace_back();
            for (const std::string& id : tier) row.push_back(index_of(id));
        }
        if (tiers.empty()) {
            for (uint32_t k = 0; k < n && tiers.size() < 9; k += 2) {
                auto& row = tiers.emplace_back();
                for (uint32_t i = k; i < std::min(n, k + 2); i++) row.push_back((int32_t)i);
            }
        }
        for (uint32_t tier = 0; tier < 9; tier++) {
            for (uint32_t w = 0; w < 5; w++) {
                int32_t v = tier < tiers.size() && w < tiers[tier].size() ? tiers[tier][w] : -1;
                wr32(rdram, ladder_tiers + tier * 0x14 + 4 * w, (uint32_t)v);
            }
        }
        for (uint32_t w = 0; w < 4; w++) {
            int32_t v = w < lg.ladder_done.size() ? index_of(lg.ladder_done[w]) : (int32_t)((n - 1 - w % n + n) % n);
            wr32(rdram, ladder_done_teams + 4 * w, (uint32_t)v);
        }
    }

#if BLITZ_TEAMPLUS
    // A league pak: its teams take slots 0..n-1, each built on its base franchise's block, and a
    // terminator at slot n ends the list (as team+ ends its own at 32). Idempotent per ovl_b load.
    std::optional<std::string> apply_league(uint8_t* rdram, const League& lg) {
        blitz::teamdata::Store& st = blitz::teamdata::store();  // the NFL teams as loaded, to build the league's on
        st.write(rdram);  // the NFL teams (with any drop-in art) stay in the blocks past the league's, as before
        const uint32_t n = (uint32_t)lg.teams.size();
        auto [league, clash] = build_league(rdram, lg, st);
        st.replace(std::move(league), clash);
        st.write(rdram);
        wr32(rdram, block(n), 0xFFFFFFFF);

        wr8(rdram, team_count_bytes, (uint8_t)n);
        wr8(rdram, team_count_bytes + 2, (uint8_t)n);
        write_league_ladder(rdram, lg);
        set_franchises(std::vector<std::string>(n));
        return std::nullopt;
    }

    // Each block's franchise, read only (no pack applied); empty unless the ROM's teams are exactly
    // the franchises this build knows. Standard's 32-team Season needs it with or without a pack.
    std::vector<std::string> recognise(uint8_t* rdram) {
        uint32_t count = 0;
        while (count < max_blocks && (int32_t)MEM_W(0, guest(block(count))) >= 0) {
            count++;
        }
        if (count != franchises.size()) return {};
        std::vector<std::string> out(count);
        for (uint32_t i = 0; i < count; i++) {
            auto f = franchise_of(rd_text(rdram, block(i) + t_name, 16));
            if (!f || std::find(out.begin(), out.end(), *f) != out.end()) return {};
            out[i] = *f;
        }
        return out;
    }

    // Apply the pack to the freshly loaded ovl_b data. Returns a problem description, or nothing.
    std::optional<std::string> apply(uint8_t* rdram, const Pack* pack_ptr) {
        static const Pack no_pack{};
        const Pack& pack = pack_ptr ? *pack_ptr : no_pack;
        // Read the blocks the ROM has (until a negative first word, as tp_count_teams does).
        uint32_t count = 0;
        while (count < max_blocks && (int32_t)MEM_W(0, guest(block(count))) >= 0) {
            count++;
        }
        if (count != franchises.size()) {
            return "the ROM has " + std::to_string(count) + " teams; this pack needs " + std::to_string(franchises.size());
        }
        std::map<std::string, uint32_t> slot_of;  // franchise -> current slot
        for (uint32_t i = 0; i < count; i++) {
            std::string nick = rd_text(rdram, block(i) + t_name, 16);
            auto f = franchise_of(nick);
            if (!f || slot_of.count(*f)) {
                return "team " + std::to_string(i) + " (\"" + nick + "\") isn't a franchise this build knows";
            }
            slot_of[*f] = i;
        }
        // From here the teams are edited in the team store (team_store.h) and written back once.
        std::vector<std::string> by_slot(count);
        for (auto& [f, i] : slot_of) by_slot[i] = f;
        blitz::teamdata::Store& st = blitz::teamdata::store();
        st.load(rdram, by_slot);
        // Art drop-in: each franchise's asset ids from the drop-in ROM (league teams are built on them too).
        if (blitz::art_active()) {
            for (const std::string& f : by_slot)
                if (const auto* ids = blitz::art_assets_of(f)) st.find(f)->set_art(*ids);
        }
        if (auto problem = check_expects(st, pack)) return problem;

        if (pack.league) {
            return apply_league(rdram, *pack.league);
        }

        // Reorder: the store's slot order and the clash matrix. Advanced only: Standard keeps the ROM's
        // slots, since a saved Season names teams by slot (team+'s 31-team seasons, and ours with
        // Houston, whose divisions season32.cpp assigns by franchise).
        if (!pack.order.empty() && blitz::advanced_mode()) st.reorder(pack.order);  // the clash rows go with the teams
        set_franchises(st.franchise_of_slot());

        edit_teams(st, pack);
        st.keep_packed();
        st.write(rdram);
        return std::nullopt;
    }
#endif
}

namespace blitz {
    std::optional<std::string> franchise_of_nickname(const std::string& nickname) {
        return franchise_of(nickname);
    }

    // A team's full name as the game has it: the ROM's city and nickname with the pack's edits ("Las Vegas Raiders",
    // Houston's from the 2026 Season pak). The crew panel and the Roster Room show it; no club's name is written into
    // the app for them (2026-11-13). The franchise id when the team isn't in the store.
    std::string team_display_name(const std::string& franchise) {
        for (const blitz::teamdata::Team& t : blitz::teamdata::store().slots()) {
            if (t.franchise != franchise) continue;
            std::string city(t.city_name.data(), strnlen(t.city_name.data(), t.city_name.size()));
            std::string nick = t.nickname();
            return city.empty() ? nick : nick.empty() ? city : city + " " + nick;
        }
        return franchise;
    }

    // Franchise id of each team index, as of the latest ovl_b load (empty before the first).
    // A copy: the Roster Room reads it from its own thread while an ovl_b load can replace it.
    std::vector<std::string> team_franchises() {
        std::lock_guard lock{franchise_mutex};
        return franchise_by_slot;
    }

    // The active pack's season; a pack without one (or no pack) uses the built-in pack's.
    const SeasonData* pack_season() {
        std::call_once(load_once, load_pack);
        if (active && active->season) {
            return &*active->season;
        }
        static std::optional<SeasonData> builtin;
        static std::once_flag builtin_once;
        std::call_once(builtin_once, [] {
            std::filesystem::path file = content_pack_file("blitz2026");
            if (file.empty()) {
                fprintf(stderr, "[pack] no built-in season (no 2026 Season: Advanced steps aside)\n");
                return;
            }
            try {
                std::ifstream in{file};
                builtin = parse(json::parse(in)).season;
            }
            catch (...) {
                fprintf(stderr, "[pack] no built-in season (%s)\n", file.string().c_str());
            }
        });
        return builtin ? &*builtin : nullptr;
    }

    std::string active_pack_id() {
        std::call_once(load_once, load_pack);
        return active ? active->id : std::string{};
    }

    // The active pack's folder (its files/ are game images, applied at art_init), or empty.
    std::filesystem::path active_pack_dir() {
        std::filesystem::path file = find_pack_file();
        return file.empty() || !blitz::file_exists(file) ? std::filesystem::path{} : file.parent_path();
    }

    // At on_init, before the file system loads: a league pak's files/added/*.wms become new game files
    // (found by index; the team's block points at them), e.g. each team's right-side VS helmet.
    void league_register_files() {
        std::filesystem::path dir = active_pack_dir();
        if (dir.empty()) return;
        std::error_code ec;
        if (!std::filesystem::is_directory(dir / "files" / "added", ec)) return;
        size_t n = 0;
        for (const auto& e : std::filesystem::directory_iterator(dir / "files" / "added", ec)) {
            std::string name = e.path().filename().string();
            for (char& c : name) c = (char)std::tolower((unsigned char)c);
            if (e.path().extension() != ".wms" && e.path().extension() != ".WMS") continue;
            std::ifstream f{e.path(), std::ios::binary};
            std::vector<uint8_t> data{std::istreambuf_iterator<char>(f), {}};
            if (data.size() < 32 || data[0] != 'w' || data[1] != 'n' || !blitz::fs_register_file(name, std::move(data))) {
                blitz::post_notice("League pak file \"" + name + "\" wasn't added (a .wms of at most 14 characters).");
                continue;
            }
            n++;
        }
        if (n) fprintf(stderr, "[pack] %zu league file(s) added\n", n);
    }

    // A league pak (Arcade only, no Advanced, no Season) is active.
    bool league_active() {
        std::call_once(load_once, load_pack);
        return active && active->league;
    }
}

extern "C" void func_8027A808(uint8_t* rdram, recomp_context* ctx);  // a text object's colour (handle, ARGB)

namespace {
    constexpr uint32_t menu_mode = 0x8028B998, menu_direction = 0x802B87B0;  // mode 0 Arcade .. 3 Play Editor; 1 up, 2 down

    bool mode_off(int mode) {
        if (!blitz::league_active()) return false;
        const League& lg = *active->league;
        return mode == 2 || (mode == 1 && !lg.tournament) || (mode == 3 && !lg.play_editor);
    }

    // Darker than the game's unselected grey (0xFF666666), at the entries' handles (sp+0x30 + 4*mode).
    void grey_off_modes(uint8_t* rdram, recomp_context* ctx) {
        recomp_context saved = *ctx;
        int32_t sp = (int32_t)ctx->r29;
        for (int k = 0; k < 4; k++) {
            if (!mode_off(k)) continue;
            ctx->r4 = (gpr)(int32_t)MEM_W(0x30 + 4 * k, (gpr)sp);
            ctx->r5 = (gpr)(int32_t)0xFF2A2A2A;
            func_8027A808(rdram, ctx);
        }
        *ctx = saved;
    }
}

// Main menu (main func_80256AA0) with a league pak: only its modes can be picked. 0: after the entries
// are made (0x80256D30); 1: where UP and DOWN meet (0x802570E4, v0 stays 0xF); 2: after the redraw's grey
// loop (0x8025712C); 3: before the button dispatch (0x80257250, s0 = the button).
extern "C" void blitz_league_menu(uint8_t* rdram, recomp_context* ctx, int32_t where) {
    if (!blitz::league_active()) return;
    int32_t mode = MEM_W(0, guest(menu_mode));
    switch (where) {
        case 0: {
            if (mode_off(mode)) MEM_W(0, guest(menu_mode)) = 0;
            grey_off_modes(rdram, ctx);
            static bool told = false;
            if (!told) {
                told = true;
                const League& lg = *active->league;
                std::string modes = lg.tournament && lg.play_editor ? "Arcade, Tournament and Play Editor"
                                  : lg.tournament ? "Arcade and Tournament" : lg.play_editor ? "Arcade and Play Editor" : "Arcade";
                blitz::post_notice(active->title + " plays " + modes + ". Season is off for league paks; pick another "
                                   "roster in Settings \u2192 Season \u2192 Rosters to play one.");
            }
            break;
        }
        case 1: {
            int32_t dir = MEM_W(0, guest(menu_direction)) == 1 ? -1 : 1;
            for (int i = 0; i < 4 && mode_off(mode); i++) mode = (mode + dir + 4) % 4;
            MEM_W(0, guest(menu_mode)) = mode;
            break;
        }
        case 2:
            grey_off_modes(rdram, ctx);
            break;
        case 3:
            if (mode_off(mode)) {
                MEM_W(0, guest(menu_mode)) = 0;
                ctx->r16 = 0;
            }
            break;
    }
}

// VS screen (main 0x8024C528, before the banks load): each team's announcer bank name, nickname[0:8]
// lowercased, at 0x802EF6B5 (team 0) and 0x802EA590 (team 1). A league team is silent ("none", a bank
// that doesn't exist: every call is skipped, as for the Texans) or uses its base franchise's calls.
extern "C" void blitz_league_bank(uint8_t* rdram) {
    if (!blitz::league_active()) return;
    for (uint32_t buf : {0x802EF6B5u, 0x802EA590u}) {
        std::string have = rd_text(rdram, buf, 8);
        for (char& c : have) c = (char)std::tolower((unsigned char)c);
        std::string bank = "zznone";
        for (const LeagueTeam& t : active->league->teams) {
            std::string nick = t.nickname.substr(0, 8);
            for (char& c : nick) c = (char)std::tolower((unsigned char)c);
            if (nick != have || t.announcer != "base") continue;
            for (const Franchise& f : franchises) {
                if (t.base == f.id) bank = f.nicknames.front();
            }
            for (char& c : bank) c = (char)std::tolower((unsigned char)c);
            bank = bank.substr(0, 8);
        }
        wr_text(rdram, buf, bank, 8);
    }
}

namespace {
    constexpr uint32_t adv_rec_week = 0x802EF6F1;  // the season record's week (main bss; adv_season.cpp)

    // The weekly lineup (rosters_live.cpp) over the Blitz 2026 pack's 15 slots per team, then each team's saved crew
    // (Roster Room, Pick Your Crew: crew.cpp) over that: name, number, body and skin shade (where one's been
    // picked). A slot whose player changes loses the announcer's name call. Laid over the packed teams each time
    // (team_store.h), so a crew taken away goes too; only the player records are written.
    void write_player(blitz::teamdata::Team& t, int s, const blitz::LineupPlayer& lp) {
        if (t.last(s) != lp.last) {
            t.set_last(s, lp.last);
            t.clear_call(s);
        }
        t.set_number(s, lp.number);
        t.set_heavy(s, lp.heavy);
        if (lp.skin >= 0) t.set_skin(s, lp.skin);
    }

    // The lineup's players, then (crews) each team's saved crew, over the packed teams in the store.
    void lay_players(const std::shared_ptr<const blitz::Lineup>& lineup, bool crews) {
        blitz::teamdata::Store& st = blitz::teamdata::store();
        st.restore_packed();
        for (const std::string& f : franchise_by_slot) {
            blitz::teamdata::Team* t = st.find(f);
            if (!t) continue;
            if (lineup) {
                auto it = lineup->teams.find(f);
                if (it != lineup->teams.end()) {
                    for (int s = 0; s < 15; s++)
                        if (it->second[(size_t)s]) write_player(*t, s, *it->second[(size_t)s]);
                }
            }
            if (crews)
                for (const auto& [s, lp] : blitz::crew_players(f))
                    if (s >= 0 && s < 15) write_player(*t, s, lp);
        }
    }

    void lineup_overlay(uint8_t* rdram) {
        if (!active || active->id != "blitz2026" || active->league || franchise_by_slot.empty()) return;
        auto lineup = blitz::lineup_now(blitz::advanced_mode() ? (int)rd8(rdram, adv_rec_week) : -1);
        static std::string logged;
        std::string label = lineup ? lineup->label : "as packed";
        if (logged != label) {
            logged = label;
            fprintf(stderr, "[rosters] lineup: %s\n", label.c_str());
        }
        lay_players(lineup, true);
        blitz::teamdata::store().write_players(rdram);
    }
}

namespace blitz {
    // Each Advanced week, before the games (adv_season.cpp): that week's lineup.
    void lineup_overlay_now(uint8_t* rdram) { lineup_overlay(rdram); }

    uint8_t* monitor_rdram();  // dev_monitor.cpp
    // Export ROM's Rosters (export_rom.cpp), in the store it exports: 0 this week's lineups, 1 full strength, 2 this
    // week's with the Roster Room crews. -1 puts the app's own back (its Lineups setting and crews), in the game too.
    void export_rosters(int mode) {
        if (!active || active->id != "blitz2026" || active->league || franchise_by_slot.empty()) return;
        if (mode >= 0) lay_players(blitz::lineup_for_export(mode == 1), mode == 2);
        else if (uint8_t* rdram = monitor_rdram()) lineup_overlay(rdram);
    }
    void mixed_on_ovl_b(uint8_t* rdram);  // mixed_uniforms.cpp
    void probowl_game_over();             // uniform_picker.cpp
    void probowl_names(uint8_t* rdram);   // crew.cpp: Pro Bowl crews' names, team select to kickoff
}

#if BLITZ_TEAMPLUS
namespace blitz {
    int team_of_slot(int slot) { return slot; }  // team+'s slots are its teams
}

extern "C" void blitz_on_dma_done(uint8_t* rdram, uint32_t rom_end, int32_t ram_end) {
    if (rom_end != ovl_b_rom_end || (uint32_t)ram_end != ovl_b_ram_end) {
        return;
    }
    std::call_once(load_once, load_pack);
    bool adv = blitz::advanced_mode();
    if (!active && !blitz::art_active() && !adv) {
        // No pack: Standard's Season still needs to know the teams (the ROM's own, in its order).
        set_franchises(recognise(rdram));
        if (!franchise_by_slot.empty()) blitz::teamdata::store().load(rdram, franchise_by_slot);  // the store mirrors the ROM's teams
        blitz::s32_on_ovl_b(rdram, franchise_by_slot);
        return;
    }
    if (auto problem = apply(rdram, active ? &*active : nullptr)) {
        if (!reported) {
            reported = true;
            blitz::post_notice(std::string(active ? "Content pack '" + active->title + "'" : adv ? "Advanced mode" : "The art drop-in") +
                               " wasn't applied: " + *problem + ".");
        }
        if (!adv) {
            set_franchises(recognise(rdram));
            blitz::s32_on_ovl_b(rdram, franchise_by_slot);
        }
        return;
    }
    lineup_overlay(rdram);
    blitz::mixed_on_ovl_b(rdram);
    blitz::probowl_game_over();
    blitz::probowl_names(rdram);
    if (adv) {
        blitz::adv_on_ovl_b(rdram, franchise_by_slot);
    }
    else {
        blitz::s32_on_ovl_b(rdram, franchise_by_slot);
    }
    static bool logged = false;
    if (!logged && active) {
        logged = true;
        fprintf(stderr, "[pack] applied '%s' (%zu team edits%s)\n", active->title.c_str(), active->teams.size(), active->order.empty() || !blitz::advanced_mode() ? "" : ", reordered");
    }
}
extern "C" void blitz_e32_team_select(uint8_t*) {}
#else
// The original game (engine32.cpp): the store holds the ROM's 31 teams and Houston with the pack's edits; the slot
// map puts them into the game's 31 slots as team select and each game need them.
namespace blitz {
    void e32_build_store(const std::function<std::string(const std::string&)>& franchise_of);  // engine32.cpp
    int e32_team_of_slot(int slot);
    int team_of_slot(int slot) { return e32_team_of_slot(slot); }
}
extern "C" void blitz_e32_on_ovl_b(uint8_t* rdram);
extern "C" void blitz_e32_on_ovl_a(uint8_t* rdram);

namespace {
    bool teams_built = false;

    void build_teams(uint8_t* rdram) {
        blitz::e32_build_store([](const std::string& nick) { auto f = franchise_of(nick); return f ? *f : std::string{}; });
        blitz::teamdata::Store& st = blitz::teamdata::store();
        if (active && active->league) {
            // Its teams replace the NFL's (engine32.cpp fills the game's 31 slots from them).
            auto [league, clash] = build_league(rdram, *active->league, st);
            st.set_all(std::move(league), std::move(clash));
            fprintf(stderr, "[pack] applied '%s' (%zu league teams)\n", active->title.c_str(), active->league->teams.size());
        }
        else if (active) {
            if (auto problem = check_expects(st, *active)) {
                blitz::post_notice("Content pack '" + active->title + "' wasn't applied: " + *problem + ".");
            }
            else {
                edit_teams(st, *active);
                fprintf(stderr, "[pack] applied '%s' (%zu team edits)\n", active->title.c_str(), active->teams.size());
            }
        }
        st.keep_packed();
        set_franchises(st.franchise_of_slot());
    }
}

namespace {
    constexpr uint32_t first_uniform_part = 0x80720E80u;  // engine32_layout.h: uni_home, team 0
}

// Before team select's loads (ovl_b_func_802F8594): tables found empty are written again, not read as file 0. The
// runtime's dummy framebuffer used to zero them on a slow renderer (patch 0011); this stays as the safety net.
extern "C" void blitz_e32_team_select(uint8_t* rdram) {
    if (!teams_built || MEM_W(0, (gpr)(int32_t)first_uniform_part) != 0) return;
    fprintf(stderr, "[e32] team tables were empty at team select: written again\n");
    fflush(stderr);
    blitz_e32_on_ovl_b(rdram);
    lineup_overlay(rdram);
}

extern "C" void blitz_on_dma_done(uint8_t* rdram, uint32_t rom_end, int32_t ram_end) {
    if (rom_end == 0xDE350u) {  // ovl_a (a game): every slot's in-game tables
        blitz_e32_on_ovl_a(rdram);
        return;
    }
    if (rom_end != ovl_b_rom_end || (uint32_t)ram_end != ovl_b_ram_end) return;
    std::call_once(load_once, load_pack);
    if (!teams_built) {
        teams_built = true;
        build_teams(rdram);
    }
    blitz_e32_on_ovl_b(rdram);  // every team's tables from the store
    if (blitz::league_active()) write_league_ladder(rdram, *active->league);
    lineup_overlay(rdram);      // this week's players and saved crews on the store, written again
    blitz::mixed_on_ovl_b(rdram);
    blitz::probowl_game_over();
    blitz::probowl_names(rdram);
    if (blitz::advanced_mode()) blitz::adv_on_ovl_b(rdram, franchise_by_slot);
    else blitz::s32_on_ovl_b(rdram, franchise_by_slot);
}
#endif

#endif  // BLITZ_MODERN
