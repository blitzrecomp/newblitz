#include <SDL.h>
// Pick Your Crew (Modern; Settings → Rules → Pick Your Crew): after team select, each human picks who plays for
// their team, fantasy style: a QB, a RB, a WR and a FLEX from the skill players left, the offensive line if they
// want to, then (optionally) their defense, three linemen and four behind them, and the CPU's team the same way.
// The kicker is always the team's own.
//
// Picks are cosmetic: names, numbers, bodies and shades (the game rates teams, not players). They're remembered
// per team (<app folder>/rosters/crews.json, only the rows someone changed) and written into each game's copy of
// the two teams at kickoff, so they play in every mode, Season weeks included. Anything not picked is the
// Lineups setting's player (rosters_live.cpp).
//
// Team select waits while the panel is up (blitz2000.toml: ovl_b 0x802F89CC, its own wait for every player's pick);
// the panel captures input, so the game sees none of it. The CPU's team isn't picked until after that wait, so
// opponent_select.cpp picks it early (blitz_opp_preview) and puts it in after the game's CPU block. Headshots are
// ESPN's 96x70 JPEGs (about 2 KB), kept in rosters/faces: fetched ahead (every team's offense in the background, the
// team a cursor rests on at team select) or, at the latest, when a card shows.
#if BLITZ_MODERN
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "json/json.hpp"
#include "blitz_host.h"
#include "recomp.h"
#include "recompui/recompui.h"
#include "util/file.h"
#include "recompui/config.h"
#include "librecomp/game.hpp"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_image.h"
#include "elements/ui_label.h"
#include "elements/ui_modal.h"
#include "elements/ui_scroll_container.h"
#include "ultramodern/ultramodern.hpp"
#include "rosters_live.h"
#include "../main/ui_kit.h"
#include "team_store.h"
#include "season_pak.h"
#include "blitz_soft_reset.h"
namespace blitz { SDL_GameController* pad_on_port(int port); }  // controllers.cpp

namespace live = blitz::teamdata::live;
#include "../../lib/rt64/src/contrib/stb/stb_image.h"  // declarations only; RT64 builds the decoder (and its PNG path skips JPEG)

extern "C" void blitz_redo_request();  // savestate.cpp: in the menus, back a step
extern "C" int blitz_play_mode(void);    // savestate.cpp: 0 menus, 1 play select, 2 a play

namespace blitz {
    bool blitz_portrait(int skin, const std::string& franchise, std::vector<char>& rgba, uint32_t& w, uint32_t& h);  // portraits.cpp
    std::vector<std::string> team_franchises();  // content_pack_tp.cpp: the store's teams, by team index
    std::string team_display_name(const std::string& franchise);  // content_pack_tp.cpp
    int team_of_slot(int slot);                         // content_pack_tp.cpp: the team in a game slot
    int game_lineup_week(uint8_t* rdram);               // content_pack_tp.cpp: the week whose lineup the game lays
    uint8_t* monitor_rdram();                           // dev_monitor.cpp
    std::string active_pack_id();
    bool probowl_on(int side);  // uniform_picker.cpp: R at team select
    bool redo_play_enabled();   // config.cpp: Settings → General → Redo Play (the menu steps too)
    int uniform_set_count(int team);
    int uniform_set_index(int team, const std::string& id);
    int uniform_set_pick(int team, int mode, bool dark, uint32_t random);
    void mixed_plan(int side, int slot, int team, int set);  // mixed_uniforms.cpp
    bool fs_register_file(const std::string& name, std::vector<uint8_t> data);  // art_dropin.cpp: added game files
    int32_t fs_file_index(const std::string& name);
    uint32_t fs_ram_count(uint8_t* rdram);
}

namespace {
    namespace fs = std::filesystem;
    using json = nlohmann::json;
    using Clock = std::chrono::steady_clock;

    constexpr uint32_t phase = 0x802EA420;    // 7 = attract demo
    constexpr uint32_t humans = 0x802EA418;   // a bit per controller slot with a human (0-1 left, 2-3 right)
    constexpr uint32_t chosen = 0x802EA434;   // [side] team
    constexpr uint32_t game_copy = 0x802E9460;  // [side * 0x64]: this game's copy of the team
    constexpr uint32_t player_size = 0x18, p_heavy = 0x4, p_skin = 0x5, p_number = 0x6, p_call = 0x7, p_last = 0x8;
    constexpr uint32_t t_abbr = 0x210;  // the scoreboard code a Pro Bowl crew takes
    // In a game the play select, halftime and postgame screens read the team's short record (the original game's
    // 0x64 bytes at 0x802E9408 + side * 0x64; team+ fills it from the block's +0x1D0..): its logos, nickname and
    // win wordmark. A Pro Bowl side's go to its own art (tools/make_probowl_art.py) and its code.
    constexpr uint32_t short_record = 0x802E9408, r_nickname = 0x10;
    constexpr std::pair<blitz::teamdata::Art, const char*> pro_art[] = {
        {blitz::teamdata::Art::Logo, "lg"}, {blitz::teamdata::Art::SmallLogo, "ls"}, {blitz::teamdata::Art::Win, "win"}};

    // A team's full name: the game's own (content_pack_tp.cpp), so no club's name is built into the app.
    std::string team_name(const std::string& franchise) { return blitz::team_display_name(franchise); }

    bool enabled() {
        return std::get<bool>(recompui::config::get_config("rules").get_option_value("pick_crew"));
    }

    // --- Rows and pools -------------------------------------------------------------------------------------------

    enum Row { QB, RB, WR, FLEX, LINE, DLINE, BACK, RowCount };
    struct RowDef {
        const char* label;
        int picks;
        std::vector<int> slots;
    };
    const RowDef rows[RowCount] = {
        {"QB", 1, {3}}, {"RB", 1, {4}}, {"WR", 1, {5}}, {"FLEX", 1, {6}},
        {"LINE", 3, {0, 1, 2}}, {"D-LINE", 3, {7, 8, 9}}, {"BACK SEVEN", 4, {10, 11, 12, 13}}};

    bool in(const std::string& pos, std::initializer_list<const char*> set) {
        for (const char* s : set) if (pos == s) return true;
        return false;
    }
    bool is_rb(const std::string& p) { return in(p, {"RB", "FB", "HB"}); }
    bool is_ol(const std::string& p) { return in(p, {"OT", "T", "G", "OG", "C", "OC", "OL"}); }
    bool is_dl(const std::string& p) { return in(p, {"DE", "DT", "NT", "DL", "EDGE", "OLB"}); }
    bool is_back(const std::string& p) { return in(p, {"LB", "ILB", "MLB", "OLB", "CB", "S", "SS", "FS", "DB"}); }

    int rank(const blitz::RosterPlayer& p) {
        size_t i = p.depth.find_first_of("0123456789");
        return i == std::string::npos ? 99 : std::atoi(p.depth.c_str() + i);
    }

    struct TeamCrew {
        std::string f;                              // franchise, or a Pro Bowl crew's key (PRO_LEFT, PRO_RIGHT, PRO_CPU)
        bool pro = false;                           // a Pro Bowl crew: every team's full-strength players
        std::map<std::string, int> slot_of;         // Pro Bowl: each candidate's slot in his team's full-strength lineup
        std::vector<blitz::RosterPlayer> roster;   // by depth rank, then file order
        std::array<std::string, 15> base{};        // the Lineups setting's ids
        std::array<std::string, 15> full{};        // full strength (hurt starters included)
        std::array<std::vector<std::string>, RowCount> picks;

        const blitz::RosterPlayer* find(const std::string& id) const {
            for (const auto& p : roster) if (p.id == id) return &p;
            return nullptr;
        }
        bool used_elsewhere(const std::string& id, int row) const {
            for (int r = 0; r < RowCount; r++) {
                if (r == row) continue;
                if (std::find(picks[r].begin(), picks[r].end(), id) != picks[r].end()) return true;
            }
            return false;
        }
        bool fits(int row, const blitz::RosterPlayer& p) const {
            switch (row) {
                case QB: return p.pos == "QB";
                case RB: return is_rb(p.pos);
                case WR: return p.pos == "WR";
                case FLEX: return (p.pos == "WR" || p.pos == "TE" || is_rb(p.pos)) && !used_elsewhere(p.id, FLEX);
                case LINE: return is_ol(p.pos);
                case DLINE: return is_dl(p.pos) && !used_elsewhere(p.id, DLINE);
                case BACK: return is_back(p.pos) && !used_elsewhere(p.id, BACK);
            }
            return false;
        }
        // Who a row offers, best first: its positions by depth rank, capped; then full-strength starters who
        // fit (a hurt star has no depth-chart spot, but you can still pick him), and whoever it holds now.
        std::vector<const blitz::RosterPlayer*> pool(int row) const {
            static const size_t caps[RowCount] = {3, 3, 5, 6, 6, 6, 10};
            std::vector<const blitz::RosterPlayer*> out;
            auto add = [&](const blitz::RosterPlayer* p) {
                if (p && p->number >= 0 && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
            };
            if (pro) {  // the league's full-strength players at the row's slots, team by team
                for (const auto& p : roster) {
                    auto it = slot_of.find(p.id);
                    if (it == slot_of.end()) continue;
                    if (std::find(rows[row].slots.begin(), rows[row].slots.end(), it->second) == rows[row].slots.end()) continue;
                    if (!used_elsewhere(p.id, row)) add(&p);
                }
                for (const std::string& id : picks[row]) add(find(id));
                return out;
            }
            for (const auto& p : roster) {
                if (out.size() >= caps[row]) break;
                if (p.number >= 0 && fits(row, p)) add(&p);
            }
            for (const std::string& id : full) {
                const auto* p = id.empty() ? nullptr : find(id);
                if (p && fits(row, *p)) add(p);
            }
            for (const std::string& id : picks[row]) add(find(id));
            return out;
        }
        // Fills a row to its count from the pool (anyone not picked in another row).
        void top_up(int row) {
            for (const auto* p : pool(row)) {
                if ((int)picks[row].size() >= rows[row].picks) break;
                if (std::find(picks[row].begin(), picks[row].end(), p->id) == picks[row].end() && !used_elsewhere(p->id, row)) {
                    picks[row].push_back(p->id);
                }
            }
        }
    };

    // --- Saved crews --------------------------------------------------------------------------------------------

    std::mutex crews_mutex;
    json crews;  // {franchise: {slot: id}}
    bool crews_loaded = false;

    fs::path crews_file() { return blitz::rosters_dir() / "crews.json"; }
    // Restart in place (soft_restart.cpp): saved crews are read again, as at a fresh start.
    [[maybe_unused]] const bool crews_soft_reset = blitz::on_soft_reset("crews", [] {
        std::lock_guard lock{crews_mutex};
        crews_loaded = false;
    });

    void load_crews() {
        if (crews_loaded) return;
        crews_loaded = true;
        std::ifstream in{crews_file()};
        try {
            if (in) crews = json::parse(in);
        }
        catch (...) {
        }
        if (!crews.is_object()) crews = json::object();
    }

    void save_crews() {
        std::error_code ec;
        fs::create_directories(crews_file().parent_path(), ec);
        std::ofstream out{crews_file()};
        out << crews.dump(1) << "\n";
    }

    std::map<int, std::string> saved_crew(const std::string& f) {
        std::lock_guard lock{crews_mutex};
        load_crews();
        std::map<int, std::string> out;
        if (crews.contains(f) && crews[f].is_object()) {
            for (auto& [k, v] : crews[f].items()) {
                if (v.is_string()) out[std::atoi(k.c_str())] = v.get<std::string>();
            }
        }
        return out;
    }

    void store_row(const TeamCrew& t, int row) {
        std::lock_guard lock{crews_mutex};
        load_crews();
        json& c = crews[t.f];
        if (!c.is_object()) c = json::object();
        for (size_t i = 0; i < rows[row].slots.size(); i++) {
            std::string key = std::to_string(rows[row].slots[i]);
            if (i < t.picks[row].size()) c[key] = t.picks[row][i];
            else c.erase(key);
        }
        save_crews();
    }

    void forget_rows(const std::string& f, std::initializer_list<int> which) {
        std::lock_guard lock{crews_mutex};
        load_crews();
        if (!crews.contains(f)) return;
        for (int row : which)
            for (int s : rows[row].slots) crews[f].erase(std::to_string(s));
        if (crews[f].empty()) crews.erase(f);
        save_crews();
    }

    // The week whose lineup the game plays (content_pack_tp.cpp game_lineup_week), from team select. A row nobody
    // changes isn't saved and plays the game's lineup, so the panel must show that one: with Advanced on it showed the
    // latest week's instead (the Rams' two tight ends) while the game laid full strength (Puka Nacua; the team, 1.0.1).
    std::atomic<int> lineup_week{-1};

    TeamCrew make_team(const std::string& f) {
        TeamCrew t;
        t.f = f;
        t.roster = blitz::roster_of(f);
        std::stable_sort(t.roster.begin(), t.roster.end(), [](const auto& a, const auto& b) { return rank(a) < rank(b); });
        t.base = blitz::lineup_ids(f, lineup_week.load());
        t.full = blitz::full_ids(f);
        auto saved = saved_crew(f);
        for (int r = 0; r < RowCount; r++) {
            for (int s : rows[r].slots) {
                std::string id = saved.count(s) ? saved[s] : t.base[(size_t)s];
                if (!id.empty() && t.find(id) && !t.used_elsewhere(id, r) &&
                    std::find(t.picks[r].begin(), t.picks[r].end(), id) == t.picks[r].end()) {
                    t.picks[r].push_back(id);
                }
            }
        }
        for (int r = 0; r < RowCount; r++) t.top_up(r);
        return t;
    }

    // The offense a team's panel opens with, for the log (a console run can't show the panel): "QB #9 Stafford, ...".
    std::string offense_text(const TeamCrew& t) {
        std::string out;
        for (int r = QB; r <= FLEX; r++) {
            for (const std::string& id : t.picks[r]) {
                const blitz::RosterPlayer* p = t.find(id);
                if (!out.empty()) out += ", ";
                out += std::string(rows[r].label) + " #" + (p ? std::to_string(p->number) + " " + p->last : "? " + id);
            }
        }
        return out;
    }

    // --- Pro Bowl crews -------------------------------------------------------------------------------------------

    // A crew's name is its scoreboard code: three letters (the VS screen's city and nickname come from the game's
    // own text tables, so a longer name would show nowhere). Presets, or typed.
    const char* pro_names[] = {"ALL", "PRO", "MVP", "HOF", "AFC", "NFC", "ACE", "TOP"};
    const char* uniform_modes[] = {"2026", "1999", "Random"};

    struct ProMeta {
        std::string name = "ALL";
        int uniforms = 0;  // 0 2026, 1 1999, 2 Random (from each player's own team's sets)
    };

    std::string pro_abbr(const std::string& name) {
        std::string out;
        for (char c : name) if (std::isalnum((unsigned char)c) && out.size() < 3) out += (char)std::toupper((unsigned char)c);
        return out.empty() ? "PRO" : out;
    }

    ProMeta load_meta(const std::string& key) {
        std::lock_guard lock{crews_mutex};
        load_crews();
        ProMeta m;
        const std::string k = key + "_META";
        if (crews.contains(k) && crews[k].is_object()) {
            m.name = pro_abbr(crews[k].value("name", m.name));  // an older, longer name: its first three letters
            m.uniforms = crews[k].value("uniforms", m.uniforms);
        }
        return m;
    }

    void save_meta(const std::string& key, const ProMeta& m) {
        std::lock_guard lock{crews_mutex};
        load_crews();
        crews[key + "_META"] = {{"name", m.name}, {"uniforms", m.uniforms}};
        save_crews();
    }

    std::mt19937& rng() {
        static std::mt19937 r{std::random_device{}()};
        return r;
    }

    // Fills a Pro Bowl crew's rows (and the kicker) at random from the league's full-strength players.
    void pro_random_fill(TeamCrew& t, std::initializer_list<int> which) {
        for (int r : which) {
            while ((int)t.picks[r].size() < rows[r].picks) {
                auto pool = t.pool(r);
                std::vector<const blitz::RosterPlayer*> open;
                for (const auto* p : pool)
                    if (std::find(t.picks[r].begin(), t.picks[r].end(), p->id) == t.picks[r].end()) open.push_back(p);
                if (open.empty()) break;
                t.picks[r].push_back(open[std::uniform_int_distribution<size_t>(0, open.size() - 1)(rng())]->id);
            }
        }
    }

    TeamCrew make_pro(const std::string& key, bool fresh) {
        TeamCrew t;
        t.f = key;
        t.pro = true;
        for (const std::string& f : blitz::team_franchises()) {
            auto ids = blitz::full_ids(f);
            for (size_t k = 0; k < ids.size(); k++) {
                if (ids[k].empty() || t.slot_of.count(ids[k])) continue;
                auto p = blitz::roster_player(ids[k]);
                if (!p || p->number < 0) continue;
                t.slot_of[ids[k]] = (int)k;
                t.roster.push_back(*p);
            }
        }
        std::stable_sort(t.roster.begin(), t.roster.end(), [](const auto& a, const auto& b) { return a.team < b.team; });
        if (fresh) forget_rows(key, {QB, RB, WR, FLEX, LINE, DLINE, BACK});
        auto saved = saved_crew(key);
        for (int r = 0; r < RowCount; r++)
            for (int sl : rows[r].slots)
                if (saved.count(sl) && t.find(saved[sl]) && !t.used_elsewhere(saved[sl], r)) t.picks[r].push_back(saved[sl]);
        pro_random_fill(t, {QB, RB, WR, FLEX, LINE, DLINE, BACK});
        for (int r = 0; r < RowCount; r++) store_row(t, r);
        // The kicker: one of the league's, at random, kept with the crew.
        if (fresh || !saved.count(14)) {
            std::vector<std::string> kickers;
            for (const auto& [id, sl] : t.slot_of) if (sl == 14) kickers.push_back(id);
            if (!kickers.empty()) {
                std::lock_guard lock{crews_mutex};
                crews[key]["14"] = kickers[std::uniform_int_distribution<size_t>(0, kickers.size() - 1)(rng())];
                save_crews();
            }
        }
        return t;
    }

    // From team select's end to kickoff, Pro Bowl crews' names go on their teams' ovl_b blocks (the VS screen reads
    // them; every ovl_b load puts the pack's back, so content_pack_tp.cpp calls probowl_names again).
    std::atomic<bool> names_pending{false};
    bool cpu_named = false;  // the CPU crew's code was dealt at team select (probowl_code), so keep it this game

    // The VS screen with a Pro Bowl crew playing: the game would show the base teams' helmets and names, so a card
    // covers them (not the cheat boxes along the bottom) until kickoff. It takes no input, so codes still work.
    std::atomic<bool> vs_wanted{false};
    std::string vs_key[2];   // each side's crew key (PRO_LEFT, PRO_RIGHT, PRO_CPU) or franchise
    bool vs_pro[2] = {false, false};

    // --- Headshots ------------------------------------------------------------------------------------------------
    // ESPN's headshot for each player (back on 2026-11-03, the team: the app is private), from this device's cache or the
    // app's bundled copy, else downloaded in the background. A player with none (no ESPN id, offline) gets his Blitz
    // portrait (portraits.cpp: the game's own face in his shade) instead, so no card stays blank.

    // Fetched by a few workers at once (2026-11-15, the team: one at a time, they popped in slowly), in three tiers: the
    // open panel's (and the Pro Bowl VS card's) first, then the team a cursor rests on at team select, then the
    // background preload of every team's offense while the player is in the menus (a few a second, paused while a
    // play runs; not on a lean device). A prefetched face only goes to disk; a card that asks for one jumps the
    // queue, or waits for the fetch already under way.
    enum FaceTier { Shown, Hovered, Preload, TierCount };
    constexpr int kFaceWorkers = 4;                               // requests at once: gentle on ESPN's CDN
    constexpr auto kPreloadGap = std::chrono::milliseconds(250);  // the background preload: four a second at most
    constexpr int InFlight = 10, Fetched = 11;                    // Faces::state past the tiers

    // What the workers share. Made once and never freed: the workers are detached, and the app's exit mustn't take
    // their queues and lock from under one still waiting or fetching.
    struct Faces {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::string> wanted[TierCount];
        std::map<std::string, int> state;  // queued (its tier), InFlight or Fetched
        std::set<std::string> shown;       // asked for by a card: handed to the main thread when it's here
        std::set<std::string> none;        // no photo (no ESPN id, a 404, offline, switched off): the portrait
        std::vector<std::string> ready, missing;
        int threads = 0;
        Clock::time_point preload_next{};
        // The log's batches (Hovered, Preload): how long, how many fetched, already here, without a photo.
        struct Batch {
            std::string what;
            Clock::time_point from{};
            int queued = 0, done = 0, fetched = 0, cached = 0, none = 0;
        } batch[TierCount];
        std::map<std::string, int> batch_of;  // a prefetched id's batch, until it's done
    };
    Faces& faces() {
        static Faces* f = new Faces;
        return *f;
    }
    std::atomic<bool> preload_go{true};  // false while a play runs (main thread, each tick)

    std::map<std::string, int> faces_queued;  // handed to the renderer, on which UI tick (main thread only)
    int ui_tick = 0;

    fs::path face_file(const std::string& id) { return blitz::rosters_dir() / "faces" / (id + ".jpg"); }
    fs::path bundled_face(const std::string& id) { return recompui::file::get_program_path() / "rosters" / "faces" / (id + ".jpg"); }
    std::string face_src(const std::string& id) { return "?/crew/" + id; }
    // The id names a file and goes in a URL: ESPN ids are digits, and a crew locked in from the network could send
    // anything (../). Anything else (an nfl: id) has no ESPN photo.
    bool espn_id(const std::string& id) {
        return !id.empty() && std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum((unsigned char)c); });
    }
    bool face_on_disk(const std::string& id) {
        std::error_code ec;
        return fs::exists(face_file(id), ec) || fs::exists(bundled_face(id), ec);
    }
    bool lean_device() { return getenv("BLITZ_LEAN") != nullptr; }

    // With the lock held: a prefetched face is done; its batch's log line once the whole batch is.
    void batch_done(Faces& F, const std::string& id, bool got) {
        auto it = F.batch_of.find(id);
        if (it == F.batch_of.end()) return;
        Faces::Batch& b = F.batch[it->second];
        F.batch_of.erase(it);
        b.done++;
        (got ? b.fetched : b.none)++;
        if (b.done < b.queued) return;
        double secs = std::chrono::duration<double>(Clock::now() - b.from).count();
        fprintf(stderr, "[crew] %s: prefetched %d headshots in %.1f s (%d from cache, %d without a photo)\n", b.what.c_str(),
                b.fetched + b.cached, secs, b.cached, b.none);
        b = Faces::Batch{};
    }

    // With the lock held: whether a tier has a live id at the front (an entry moved up a tier, or taken, is dropped).
    bool tier_has(Faces& F, int tier) {
        auto& q = F.wanted[tier];
        while (!q.empty()) {
            auto it = F.state.find(q.front());
            if (it != F.state.end() && it->second == tier) return true;
            q.pop_front();
        }
        return false;
    }

    void faces_worker() {
        Faces& F = faces();
        for (;;) {
            std::string id;
            {
                std::unique_lock lock{F.mutex};
                for (;;) {
                    int tier = -1;
                    for (int t = Shown; t < Preload && tier < 0; t++) if (tier_has(F, t)) tier = t;
                    if (tier < 0 && tier_has(F, Preload)) {
                        Clock::time_point now = Clock::now();
                        if (!preload_go) {  // a play is running: on again after it
                            F.cv.wait_for(lock, std::chrono::milliseconds(500));
                            continue;
                        }
                        if (now < F.preload_next) {
                            F.cv.wait_until(lock, F.preload_next);
                            continue;
                        }
                        F.preload_next = now + kPreloadGap;
                        tier = Preload;
                    }
                    if (tier < 0) {
                        F.cv.wait(lock);
                        continue;
                    }
                    id = F.wanted[tier].front();
                    F.wanted[tier].pop_front();
                    F.state[id] = InFlight;
                    break;
                }
            }
            // The season server's switch (season_pak.h headshots_allowed) can go off while the queue waits: nothing
            // more is asked of ESPN then.
            bool got = false;
            if (face_on_disk(id)) got = true;  // saved while it waited (an older session, a bundled copy)
            else if (blitz::headshots_allowed()) {
                std::string body;
                int status = 0;
                std::string url = "https://a.espncdn.com/combiner/i?img=/i/headshots/nfl/players/full/" + id + ".png&w=96&h=70&format=jpg";
                got = blitz::http_get(url, "", "", body, status) && body.size() > 100;
                if (got) {
                    // Written whole, then renamed: the panel may read it the moment it's there.
                    std::error_code ec;
                    fs::path file = face_file(id), part = file;
                    part += ".part";
                    fs::create_directories(file.parent_path(), ec);
                    {
                        std::ofstream out{part, std::ios::binary};
                        out << body;
                        got = (bool)out;
                    }
                    if (got) fs::rename(part, file, ec);
                    if (!got || ec) {
                        fs::remove(part, ec);
                        got = false;
                    }
                }
                else {
                    fprintf(stderr, "[crew] no headshot for %s (HTTP %d): his Blitz portrait\n", id.c_str(), status);
                }
            }
            std::lock_guard lock{F.mutex};
            F.state[id] = Fetched;
            if (!got) F.none.insert(id);
            if (F.shown.count(id)) (got ? F.ready : F.missing).push_back(id);
            batch_done(F, id, got);
        }
    }

    // With the lock held: the workers, the first time there's work (detached, as the single worker was).
    void start_workers(Faces& F) {
        while (F.threads < kFaceWorkers) {
            F.threads++;
            std::thread(faces_worker).detach();
        }
    }

    // A card's face that isn't on disk: fetched ahead of any prefetch (or the fetch under way is waited for).
    void want_face(const std::string& id) {
        // Photos only where they're allowed (season_pak.h headshots_allowed: a private build, or the 2026 Season pak
        // with headshots on that the server hasn't switched off); else every card gets the Blitz portrait and nothing
        // is asked of ESPN (2026-11-13).
        bool espn = blitz::headshots_allowed() && espn_id(id);
        Faces& F = faces();
        std::lock_guard lock{F.mutex};
        if (!F.shown.insert(id).second) return;
        auto it = F.state.find(id);
        if (!espn && it == F.state.end()) {
            it = F.state.emplace(id, Fetched).first;
            F.none.insert(id);
        }
        if (it != F.state.end() && it->second == Fetched) {  // fetched already, or there's none: the result now
            (F.none.count(id) ? F.missing : F.ready).push_back(id);
            return;
        }
        if (it != F.state.end() && it->second == InFlight) return;  // the worker hands it over
        F.state[id] = Shown;
        F.wanted[Shown].push_back(id);
        start_workers(F);
        F.cv.notify_all();
    }

    // Main thread: faces to have on disk before a card asks (a hovered team, the background preload), at `tier`.
    // Ones already here count as from cache; the batch's line comes when its last fetch is done.
    void prefetch(const std::vector<std::string>& ids, int tier, const std::string& what) {
        std::vector<std::string> fetch;
        int cached = 0;
        std::set<std::string> seen;
        for (const std::string& id : ids) {
            if (!espn_id(id) || !seen.insert(id).second) continue;
            if (face_on_disk(id)) cached++;
            else fetch.push_back(id);
        }
        Faces& F = faces();
        std::lock_guard lock{F.mutex};
        Faces::Batch& b = F.batch[tier];
        if (b.queued == b.done) b = Faces::Batch{what, Clock::now()};
        else if (b.what.find(what) == std::string::npos) b.what += ", " + what;
        b.cached += cached;
        bool added = false;
        for (const std::string& id : fetch) {
            auto it = F.state.find(id);
            // Queued as soon or sooner, under way, or fetched this session (none to be had: no second try).
            if (it != F.state.end() && (it->second <= tier || it->second >= InFlight)) continue;
            F.state[id] = tier;
            F.wanted[tier].push_back(id);
            if (!F.batch_of.count(id)) {
                F.batch_of[id] = tier;
                b.queued++;
            }
            added = true;
        }
        if (b.queued == b.done) {  // nothing to fetch
            fprintf(stderr, "[crew] %s: all %d headshots already here\n", b.what.c_str(), b.cached);
            b = Faces::Batch{};
            return;
        }
        if (added) {
            start_workers(F);
            F.cv.notify_all();
        }
    }

    // Main thread: hands a saved headshot to the renderer; false if it isn't on disk yet. An image can be used
    // from the next tick (set as a source the same tick, it loads before the renderer has it, and stays blank).
    bool queue_face(const std::string& id) {
        if (faces_queued.count(id)) return true;
        if (!espn_id(id)) return false;
        if (!blitz::headshots_allowed()) return false;  // switched off: not the saved photos either
        std::ifstream in{face_file(id), std::ios::binary};
        // Else the app's own copy (Resources/rosters/faces, tools/roster_headshots.py at build time): a console or an
        // offline phone has faces without downloading any.
        if (!in) in = std::ifstream{bundled_face(id), std::ios::binary};
        if (!in) return false;
        std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (bytes.size() < 100) return false;
        // The renderer reads PNG and DDS files only, so a JPEG goes in as pixels.
        int w = 0, h = 0;
        stbi_uc* rgba = stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w, &h, nullptr, 4);
        if (rgba == nullptr) return false;
        std::vector<char> pixels((const char*)rgba, (const char*)rgba + (size_t)w * h * 4);
        stbi_image_free(rgba);
        recompui::queue_image_from_bytes_rgba32(face_src(id), pixels, (uint32_t)w, (uint32_t)h);
        faces_queued[id] = ui_tick;
        return true;
    }

    // Main thread: his Blitz portrait in place of a photo; false without the ROM's faces.
    bool queue_portrait(const std::string& id) {
        if (faces_queued.count(id)) return true;
        auto p = blitz::roster_player(id);
        std::vector<char> pixels;
        uint32_t w = 0, h = 0;
        if (!blitz::blitz_portrait(p ? p->skin : -1, p ? p->team : "", pixels, w, h)) return false;
        recompui::queue_image_from_bytes_rgba32(face_src(id), pixels, w, h);
        faces_queued[id] = ui_tick;
        return true;
    }

    // A card's face: the photo if it's here, else asked for (the portrait if there's none).
    void show_face(const std::string& id) {
        if (!queue_face(id)) want_face(id);
    }
    bool face_usable(const std::string& id) {
        auto it = faces_queued.find(id);
        return it != faces_queued.end() && it->second < ui_tick;
    }

    // --- The flow -------------------------------------------------------------------------------------------------

    enum WaitState : int { Idle, Requested, Open, Done, Cancelled };
    std::atomic<int> wait_state{Idle};
    Clock::time_point requested_at, cancelled_at;

    enum PageKind { Offense, Line, Defense, TeamPage };
    struct Page {
        int side;
        PageKind kind;
    };
    struct Flow {
        std::string team[2];
        bool human[2] = {false, false};
        bool pro[2] = {false, false};
        std::vector<Page> stack;
        std::map<int, TeamCrew> crews;  // by side
    } flow;
    std::mutex flow_mutex;  // the request (game thread) and the flow (main thread)
    std::atomic<bool> pregame{false};  // the panel is Season's pre-game screen's (season_pregame.cpp), not team select's

    // --- The panel ------------------------------------------------------------------------------------------------

    class CrewRoot : public recompui::Element {
    protected:
        std::string_view get_type_name() override { return "CrewRoot"; }
        void process_event(const recompui::Event& e) override;
    public:
        CrewRoot(recompui::ResourceId rid, recompui::Element* parent)
            : Element(rid, parent, recompui::Events(recompui::EventType::MenuAction), "div", false) {}
    };

    class CrewHints : public recompui::Element {
        recompinput::InputDevice shown_device = recompinput::InputDevice::COUNT;
        int shown_profile = -2;
    protected:
        std::string_view get_type_name() override { return "CrewHints"; }
        void process_event(const recompui::Event& e) override {
            if (e.type != recompui::EventType::Update) return;
            auto [device, profile] = recompui::hint_input_info();
            if (device != shown_device || profile != shown_profile) {
                shown_device = device;
                shown_profile = profile;
                recompui::build_hint_row(recompui::get_current_context(), this, {
                    {{recompui::MenuAction::Accept}, "Pick"},
                    {{recompui::MenuAction::Back}, "Back"},
                }, device, profile);
            }
            queue_update();
        }
    public:
        CrewHints(recompui::ResourceId rid, recompui::Element* parent)
            : Element(rid, parent, recompui::Events(recompui::EventType::Update), "div", false) {
            using namespace recompui;
            set_position(Position::Absolute);
            set_left(0);
            set_right(0);
            set_bottom(0);
            set_height(64.0f);
            set_display(Display::Flex);
            set_justify_content(JustifyContent::Center);
            set_align_items(AlignItems::Center);
            set_gap(45.0f);
            queue_update();
        }
    };

    struct Ui {
        recompui::ContextId context = recompui::ContextId::null();
        recompui::Element* card = nullptr;
        recompui::Label* eyebrow = nullptr;
        recompui::Label* title = nullptr;
        recompui::Element* body = nullptr;
        recompui::Element* bar = nullptr;
        struct CardRef {
            int row;
            std::string id;
            recompui::Button* button;
            recompui::Image* face;
        };
        std::vector<CardRef> cards;
        std::set<std::string> blank;  // cards whose portrait isn't showing yet
        std::map<int, recompui::Element*> row_lines;  // row -> its card holder (rebuilt when the pool changes)
        bool rebuild_page = false;
        std::set<int> rebuild_rows;
        std::string focus_after;  // a card id to focus after a rebuild
        int focus_after_row = -1;  // and its row (a WR can be in WR and in FLEX)
        std::function<void()> start_action;
        recompui::Element* scroll = nullptr;  // the rows' viewport: scrolls when they're taller than the screen allows
        float layout_w = 0.0f;  // the screen width (design px) the page was laid out for
        int fit = 7;            // cards per line on this page (its widest row, as far as the screen allows)
        int fit_max = 7;        // cards per line the screen allows
        float lines_w = 0.0f;   // the card lines' width (design px)
        bool scroll_on = false; // the rows' viewport shows its scroll bar
        bool scroll_refocus = false;
        // How long a page's faces take to show (the log line): from the build to the last blank card filled.
        Clock::time_point faces_from;
        size_t faces_cards = 0, faces_blank = 0;
        bool faces_timed = true;
    } pv;  // the panel

    // The panel's width (design px, blitz::ui::px): cards 84 wide with 6 between, after a 96 label column and a 14 gap;
    // 10 kept clear on the right for the rows' scroll bar, 18 padding each side, 28 between the panel and the screen's
    // edges. Height is 1080 dp on every screen (recompui), so only the width changes with the window.
    constexpr float kCardW = 84.0f, kCardGap = 6.0f, kLabelW = 96.0f, kLabelGap = 14.0f, kScrollRoom = 10.0f;
    constexpr float kPad = 18.0f, kSideRoom = 28.0f, kPanelMin = 800.0f;
    constexpr float row_chrome() { return 2 * kPad + kLabelW + kLabelGap + kScrollRoom; }
    constexpr float line_width(int n) { return n * kCardW + (n - 1) * kCardGap; }

    // The window's width in design px (0 before the first layout).
    float screen_w() {
        recompui::Element* root = pv.context.get_root_element();
        float ratio = root ? root->get_dp_to_pixel_ratio() : 0.0f;
        float w = root ? root->get_client_width() : 0.0f;
        return ratio > 0.0f && w > 0.0f ? w / ratio / blitz::ui::k : 0.0f;
    }

    // Sizes the panel for a page whose widest row has `widest` cards: wide enough for that row on one line where the
    // screen allows, never past the screen (narrower windows wrap rows over more lines). 0: a page without card rows.
    void fit_panel(int widest) {
        float sw = screen_w();
        pv.layout_w = sw;
        if (sw <= 0.0f) sw = 960.0f;  // not laid out yet: 4:3, the narrowest; the first update lays it out again
        float avail = sw - 2 * kSideRoom;
        pv.fit_max = std::max(1, (int)std::floor((avail - row_chrome() + kCardGap) / (kCardW + kCardGap)));
        pv.fit = std::clamp(widest, 1, pv.fit_max);
        float w = std::min(std::max(kPanelMin, widest > 0 ? row_chrome() + line_width(pv.fit) : 0.0f), avail);
        pv.lines_w = w - row_chrome();
        pv.card->set_width(blitz::ui::px(w));
    }

    // Remembers the focused card so a rebuild (a resize, a wider row) puts focus back on it.
    void keep_focus() {
        recompui::Element* f = pv.context.get_focused_element();
        for (const auto& c : pv.cards) {
            if (c.button == f) {
                pv.focus_after = c.id;
                pv.focus_after_row = c.row;
            }
        }
    }

    void back();
    void finish();

    void CrewRoot::process_event(const recompui::Event& e) {
        if (e.type == recompui::EventType::MenuAction &&
            std::get<recompui::EventMenuAction>(e.variant).action == recompui::MenuAction::Back) {
            back();
        }
    }

    std::string upper(std::string s) {
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }

    // The Roster Room's look (roster_room.cpp): navy cards and chips, a yellow rim on what's chosen.
    void mark_chosen(recompui::Button* b, bool chosen) {
        using namespace recompui;
        b->apply_button_style(ButtonStyle::Secondary);
        b->set_background_color(chosen ? Color{58, 50, 8, 235} : Color{7, 15, 34, 235});
        b->set_border_color(chosen ? Color{246, 212, 0, 255} : Color{143, 239, 245, 70});
    }

    void style_card(const Ui::CardRef& c, const TeamCrew& t) {
        mark_chosen(c.button, std::find(t.picks[c.row].begin(), t.picks[c.row].end(), c.id) != t.picks[c.row].end());
    }

    void restyle(const TeamCrew& t) {
        for (const auto& c : pv.cards) style_card(c, t);
    }

    void pick(int side, int row, const std::string& id) {
        TeamCrew& t = flow.crews[side];
        auto& p = t.picks[row];
        if (std::find(p.begin(), p.end(), id) != p.end()) return;
        if ((int)p.size() >= rows[row].picks) p.erase(p.begin());  // the oldest pick makes room
        p.push_back(id);
        store_row(t, row);
        // A player can't be in two rows: FLEX follows WR and RB, the back seven follows the line (and back).
        for (int r = 0; r < RowCount; r++) {
            if (r == row) continue;
            auto& q = t.picks[r];
            auto it = std::find(q.begin(), q.end(), id);
            if (it != q.end()) {
                q.erase(it);
                t.top_up(r);
                store_row(t, r);
            }
        }
        if (row == WR || row == RB) pv.rebuild_rows.insert(FLEX);
        if (row == DLINE) pv.rebuild_rows.insert(BACK);
        if (row == BACK) pv.rebuild_rows.insert(DLINE);
        restyle(t);
    }

    recompui::Button* add_card(int side, int row, const blitz::RosterPlayer& p, recompui::Element* line) {
        using namespace recompui;
        ContextId ctx = pv.context;
        Button* b = ctx.create_element<Button>(line, "", ButtonStyle::Secondary, ButtonSize::Small);
        b->hide_focus_ring();
        b->set_width(blitz::ui::px(84));
        b->set_height(blitz::ui::px(96));
        b->set_min_height(blitz::ui::px(96));  // a Button fixes its height to its size preset
        b->set_max_height(blitz::ui::px(96));
        b->set_flex_shrink(0);  // a Pro Bowl strip scrolls rather than squeezing its cards
        b->set_padding(blitz::ui::px(4));
        b->set_display(Display::Flex);
        b->set_flex_direction(FlexDirection::Column);
        b->set_align_items(AlignItems::Center);
        b->set_justify_content(JustifyContent::FlexStart);
        b->set_gap(0);
        show_face(p.id);
        // A navy frame holds the photo; the image stays hidden until it has one (an empty one draws white).
        Element* frame = ctx.create_element<Element>(b);
        frame->set_width(blitz::ui::px(58));
        frame->set_height(blitz::ui::px(42));
        frame->set_border_radius(blitz::ui::px(3));
        frame->set_background_color(Color{24, 40, 78, 255});
        frame->set_flex_shrink(0);
        frame->set_margin_bottom(blitz::ui::px(3));
        Image* face = ctx.create_element<Image>(frame, face_usable(p.id) ? face_src(p.id) : "");
        face->set_width(blitz::ui::px(58));
        face->set_height(blitz::ui::px(42));
        face->set_border_radius(blitz::ui::px(3));
        if (!face_usable(p.id)) face->set_display(Display::None);
        // "#7 BRISSETT" on one line in the display face, as the Roster Room; a long name ends in "…" (team).
        std::string last = upper(p.last);
        std::string num = p.number >= 0 ? "#" + std::to_string(p.number) + " " : "";
        if (num.size() + last.size() > 12) last = last.substr(0, std::max<size_t>(3, 11 - num.size())) + "\u2026";
        Label* name = ctx.create_element<Label>(b, num + last, theme::Typography::Header3);
        name->set_font_size(blitz::ui::px(15));
        name->set_letter_spacing(0);
        name->set_white_space(WhiteSpace::Nowrap);
        name->set_line_height(blitz::ui::px(17));
        name->set_text_align(TextAlign::Center);
        name->set_color(Color{242, 242, 242, 255});
        std::string tag = p.status == "reserve" ? "IR" : p.status == "out" ? "OUT" : flow.crews[side].pro ? p.team : upper(p.depth);
        Label* t = ctx.create_element<Label>(b, tag, theme::Typography::LabelXS);
        t->set_font_size(blitz::ui::px(10));
        t->set_line_height(blitz::ui::px(12));
        t->set_color(p.status == "reserve" || p.status == "out" ? Color{255, 120, 110, 255} : Color{184, 196, 218, 255});
        std::string id = p.id;
        b->add_pressed_callback([side, row, id] { pick(side, row, id); });
        b->set_focus_callback([b](bool focused) { if (focused) b->scroll_into_view(); });  // the rows and Pro Bowl strips scroll
        pv.cards.push_back({row, p.id, b, face});
        if (!face_usable(p.id)) pv.blank.insert(p.id);
        return b;
    }

    void build_row_cards(int side, int row) {
        using namespace recompui;
        Element* holder = pv.row_lines[row];
        holder->clear_children();
        pv.cards.erase(std::remove_if(pv.cards.begin(), pv.cards.end(), [row](const auto& c) { return c.row == row; }), pv.cards.end());
        TeamCrew& t = flow.crews[side];
        auto pool = t.pool(row);
        if (t.pro) {  // one scrolling strip: every team's player at this spot
            ScrollContainer* strip = pv.context.create_element<ScrollContainer>(holder, ScrollDirection::Horizontal);
            strip->set_flex(0.0f, 0.0f, blitz::ui::px(104), Unit::Dp);  // its own height (in a column the basis is height)
            strip->set_height(blitz::ui::px(104));
            strip->set_width(blitz::ui::px(pv.lines_w));
            Element* line = pv.context.create_element<Element>(strip);
            line->set_display(Display::Flex);
            line->set_flex_direction(FlexDirection::Row);
            line->set_gap(blitz::ui::px(6));
            line->set_as_navigation_container(NavigationType::GridRow);
            for (const auto* pl : pool) add_card(side, row, *pl, line);
            for (const auto& c : pv.cards) if (c.row == row) style_card(c, t);
            return;
        }
        // As many as the panel holds on a line; a row that needs more lines shares them out evenly (9: 5 and 4).
        const size_t fit = (size_t)std::max(1, pv.fit);
        const size_t lines = std::max<size_t>(1, (pool.size() + fit - 1) / fit);
        const size_t per_line = std::max<size_t>(1, (pool.size() + lines - 1) / lines);
        Element* line = nullptr;
        for (size_t i = 0; i < pool.size(); i++) {
            if (i % per_line == 0) {
                line = pv.context.create_element<Element>(holder);
                line->set_display(Display::Flex);
                line->set_flex_direction(FlexDirection::Row);
                line->set_gap(blitz::ui::px(6));
                line->set_as_navigation_container(NavigationType::GridRow);
            }
            add_card(side, row, *pool[i], line);
        }
        for (const auto& c : pv.cards) if (c.row == row) style_card(c, t);
    }

    recompui::Button* bar_button(const std::string& text, std::function<void()> fn, bool primary = false) {
        using namespace recompui;
        Button* b = pv.context.create_element<Button>(pv.bar, text, primary ? ButtonStyle::Primary : ButtonStyle::Tertiary, ButtonSize::Card);
        b->hide_focus_ring();
        b->set_min_width(blitz::ui::px(110));
        pv.start_action = fn;  // the last one in the bar is the way forward: Start presses it too
        b->add_pressed_callback(std::move(fn));
        return b;
    }

    int other_human_unvisited() {
        for (int s = 0; s < 2; s++) {
            if (!flow.human[s]) continue;
            bool seen = false;
            for (const Page& p : flow.stack) if (p.side == s) seen = true;
            if (!seen) return s;
        }
        return -1;
    }

    int cpu_side() {
        for (int s = 0; s < 2; s++) if (!flow.human[s] && !flow.team[s].empty()) return s;
        return -1;
    }

    std::string pro_key(int side) { return !flow.human[side] ? "PRO_CPU" : side == 0 ? "PRO_LEFT" : "PRO_RIGHT"; }

    void build_team_page(int side);

    void build_page() {
        using namespace recompui;
        const Page page = flow.stack.back();
        if (!flow.crews.count(page.side)) {
            flow.crews[page.side] = flow.pro[page.side] ? make_pro(pro_key(page.side), false) : make_team(flow.team[page.side]);
            if (!flow.pro[page.side])
                fprintf(stderr, "[crew] panel: %s (lineup week %d): %s\n", flow.team[page.side].c_str(), lineup_week.load(),
                        offense_text(flow.crews[page.side]).c_str());
        }
        TeamCrew& t = flow.crews[page.side];
        pv.cards.clear();
        pv.blank.clear();
        pv.row_lines.clear();
        pv.body->clear_children();
        pv.bar->clear_children();

        int humans_n = (int)flow.human[0] + (int)flow.human[1];
        std::string who = !flow.human[page.side] ? "CPU" : humans_n > 1 ? (page.side == 0 ? "LEFT SIDE" : "RIGHT SIDE") : "YOUR TEAM";
        static const char* kinds[] = {"OFFENSE", "OFFENSIVE LINE", "DEFENSE", "TEAM"};
        pv.eyebrow->set_text(std::string(t.pro ? "PRO BOWL · " : "PICK YOUR CREW · ") + who + " · " + kinds[page.kind]);
        pv.title->set_text(t.pro ? load_meta(t.f).name : upper(team_name(t.f)));
        if (page.kind == TeamPage) {
            fit_panel(0);
            build_team_page(page.side);
            return;
        }

        std::vector<int> page_rows = page.kind == Defense ? std::vector<int>{DLINE, BACK}
                                   : page.kind == Line ? std::vector<int>{LINE} : std::vector<int>{QB, RB, WR, FLEX};
        int widest = 0;  // a Pro Bowl row is a scrolling strip, as wide as seven cards
        for (int row : page_rows) widest = std::max(widest, t.pro ? 7 : (int)t.pool(row).size());
        fit_panel(widest);
        for (int row : page_rows) {
            Element* r = pv.context.create_element<Element>(pv.body);
            r->set_display(Display::Flex);
            r->set_flex_direction(FlexDirection::Row);
            r->set_align_items(AlignItems::Center);
            r->set_gap(blitz::ui::px(kLabelGap));
            Element* label_box = pv.context.create_element<Element>(r);
            label_box->set_width(blitz::ui::px(kLabelW));
            label_box->set_flex_shrink(0);
            label_box->set_display(Display::Flex);
            label_box->set_flex_direction(FlexDirection::Column);
            Label* l = pv.context.create_element<Label>(label_box, rows[row].label, theme::Typography::LabelSM);
            l->set_color(Color{143, 239, 245, 255});
            l->set_letter_spacing(blitz::ui::px(1.5f));
            if (rows[row].picks > 1) {
                Label* n = pv.context.create_element<Label>(label_box, "PICK " + std::to_string(rows[row].picks), theme::Typography::LabelXS);
                n->set_color(Color{184, 196, 218, 255});
            }
            Element* holder = pv.context.create_element<Element>(r);
            holder->set_display(Display::Flex);
            holder->set_flex_direction(FlexDirection::Column);
            holder->set_gap(blitz::ui::px(6));
            pv.row_lines[row] = holder;
            build_row_cards(page.side, row);
        }
        if (page_rows.size() && t.roster.empty()) {
            Label* none = pv.context.create_element<Label>(pv.body, "No roster yet: the weekly rosters haven't downloaded.", theme::Typography::Body);
            none->set_color(theme::color::TextDim);
        }

        // The bar: back, the line toggle, this week's lineup, defense, the CPU's team, and play.
        int side = page.side;
        if (flow.stack.size() > 1 || blitz::redo_play_enabled()) bar_button("Back", [] { back(); });
        if (t.pro) {
            if (page.kind != Offense) bar_button("Offense", [side] { flow.stack.push_back({side, Offense}); pv.rebuild_page = true; });
            if (page.kind != Line) bar_button("Line", [side] { flow.stack.push_back({side, Line}); pv.rebuild_page = true; });
            if (page.kind != Defense) bar_button("Defense", [side] { flow.stack.push_back({side, Defense}); pv.rebuild_page = true; });
            bar_button("Shuffle", [side, page] {  // this page's rows, at random again
                TeamCrew& t = flow.crews[side];
                std::vector<int> which = page.kind == Defense ? std::vector<int>{DLINE, BACK}
                                       : page.kind == Line ? std::vector<int>{LINE} : std::vector<int>{QB, RB, WR, FLEX};
                for (int r : which) t.picks[r].clear();
                for (int r : which) {
                    pro_random_fill(t, {r});
                    store_row(t, r);
                }
                pv.rebuild_page = true;
            });
        }
        if (!t.pro && page.kind == Offense) bar_button("Pick Line", [side] { flow.stack.push_back({side, Line}); pv.rebuild_page = true; });
        if (!t.pro) bar_button("This Week's", [side, page] {
            TeamCrew& t = flow.crews[side];
            if (page.kind == Defense) forget_rows(t.f, {DLINE, BACK});
            else if (page.kind == Line) forget_rows(t.f, {LINE});
            else forget_rows(t.f, {QB, RB, WR, FLEX});
            flow.crews[side] = make_team(t.f);
            pv.rebuild_page = true;
        });
        if (!t.pro && page.kind != Defense) bar_button("Pick Defense", [side] { flow.stack.push_back({side, Defense}); pv.rebuild_page = true; });
        int cpu = cpu_side();
        bool cpu_seen = false;
        for (const Page& p : flow.stack) if (p.side == cpu) cpu_seen = true;
        if (flow.human[side] && cpu >= 0 && !cpu_seen) {
            bar_button("Pick CPU", [cpu] { flow.stack.push_back({cpu, flow.pro[cpu] ? TeamPage : Offense}); pv.rebuild_page = true; });
        }
        int next = other_human_unvisited();
        if (next >= 0) bar_button("Next Team", [next] { flow.stack.push_back({next, flow.pro[next] ? TeamPage : Offense}); pv.rebuild_page = true; }, true);
        else bar_button("Go Play", [] { finish(); }, true);

        pv.faces_from = Clock::now();
        pv.faces_cards = pv.cards.size();
        pv.faces_blank = (size_t)std::count_if(pv.blank.begin(), pv.blank.end(), [](const std::string& id) { return !faces_queued.count(id); });
        pv.faces_timed = pv.cards.empty();

        // Focus: the card that had it before a rebuild (a resize), else the first pick of the first row.
        Element* focus = nullptr;
        for (const auto& c : pv.cards) {
            if (!focus && c.id == pv.focus_after && c.row == pv.focus_after_row) focus = c.button;
        }
        pv.focus_after.clear();
        pv.focus_after_row = -1;
        for (const auto& c : pv.cards) {
            if (!focus && std::find(t.picks[c.row].begin(), t.picks[c.row].end(), c.id) != t.picks[c.row].end()) focus = c.button;
        }
        if (!focus && !pv.cards.empty()) focus = pv.cards.front().button;
        if (focus) {
            pv.context.set_autofocus_element(focus);
            focus->focus();
        }
    }

    // A Pro Bowl crew's team page: its name (a preset, or typed) and its uniforms (2026, 1999 or Random, each player
    // in his own team's set).
    void build_team_page(int side) {
        using namespace recompui;
        TeamCrew& t = flow.crews[side];
        ProMeta meta = load_meta(t.f);
        auto section = [&](const char* label) {
            Label* l = pv.context.create_element<Label>(pv.body, label, theme::Typography::LabelSM);
            l->set_color(Color{143, 239, 245, 255});
        };
        auto row = [&]() {
            Element* r = pv.context.create_element<Element>(pv.body);
            r->set_display(Display::Flex);
            r->set_flex_direction(FlexDirection::Row);
            r->set_flex_wrap(FlexWrap::Wrap);
            r->set_gap(blitz::ui::px(8));
            r->set_as_navigation_container(NavigationType::GridRow);
            return r;
        };
        Element* first = nullptr;
        section("TEAM CODE · ON THE SCOREBOARD");
        Element* names = row();
        for (const char* n : pro_names) {
            std::string nm = n;
            Button* b = pv.context.create_element<Button>(names, nm, ButtonStyle::Secondary, ButtonSize::Card);
            mark_chosen(b, meta.name == nm);
            b->hide_focus_ring();
            b->add_pressed_callback([side, nm] {
                std::string key = flow.crews[side].f;
                ProMeta m = load_meta(key);
                m.name = nm;
                save_meta(key, m);
                pv.rebuild_page = true;
            });
            if (!first || meta.name == nm) first = b;
        }
        bool preset = false;
        for (const char* n : pro_names) preset |= meta.name == n;
        {
            Button* b = pv.context.create_element<Button>(names, preset ? "TYPE ONE…" : meta.name, preset ? ButtonStyle::Tertiary : ButtonStyle::Secondary, ButtonSize::Card);
            if (!preset) mark_chosen(b, true);
            b->hide_focus_ring();
            b->add_pressed_callback([side] {  // the on-screen keyboard (recompui), capitals, three of them
                recompui::open_keyboard("Team code · on the scoreboard", "", 3, true, [side](const std::string& typed) {
                    if (typed.empty()) return;
                    std::string key = flow.crews[side].f;
                    ProMeta m = load_meta(key);
                    m.name = pro_abbr(typed);
                    save_meta(key, m);
                    pv.rebuild_page = true;
                });
            });
        }
        section("UNIFORMS · EACH PLAYER IN HIS OWN TEAM'S");
        Element* unis = row();
        for (int i = 0; i < 3; i++) {
            Button* b = pv.context.create_element<Button>(unis, uniform_modes[i], ButtonStyle::Secondary, ButtonSize::Card);
            mark_chosen(b, meta.uniforms == i);
            b->hide_focus_ring();
            b->set_min_width(blitz::ui::px(110));
            b->add_pressed_callback([side, i] {
                std::string key = flow.crews[side].f;
                ProMeta m = load_meta(key);
                m.uniforms = i;
                save_meta(key, m);
                pv.rebuild_page = true;
            });
        }
        // The crew so far, in one line.
        std::string line;
        for (int r : {QB, RB, WR, FLEX}) {
            for (const auto& id : t.picks[r]) {
                const auto* pl = t.find(id);
                if (pl) line += (line.empty() ? "" : " · ") + upper(pl->last) + " (" + pl->team + ")";
            }
        }
        section("YOUR CREW");
        Label* crew = pv.context.create_element<Label>(pv.body, line, theme::Typography::Body);
        crew->set_color(Color{242, 242, 242, 255});

        int cpu = cpu_side();
        bool cpu_seen = false;
        for (const Page& p : flow.stack) if (p.side == cpu) cpu_seen = true;
        if (flow.stack.size() > 1 || blitz::redo_play_enabled()) bar_button("Back", [] { back(); });
        bar_button("Offense", [side] { flow.stack.push_back({side, Offense}); pv.rebuild_page = true; });
        bar_button("Line", [side] { flow.stack.push_back({side, Line}); pv.rebuild_page = true; });
        bar_button("Defense", [side] { flow.stack.push_back({side, Defense}); pv.rebuild_page = true; });
        if (flow.human[side] && cpu >= 0 && !cpu_seen) {
            bar_button("Pick CPU", [cpu] { flow.stack.push_back({cpu, flow.pro[cpu] ? TeamPage : Offense}); pv.rebuild_page = true; });
        }
        int next = other_human_unvisited();
        if (next >= 0) bar_button("Next Team", [next] { flow.stack.push_back({next, flow.pro[next] ? TeamPage : Offense}); pv.rebuild_page = true; }, true);
        else bar_button("Go Play", [] { finish(); }, true);
        if (first) {
            pv.context.set_autofocus_element(first);
            first->focus();
        }
    }

    void build_ui() {
        using namespace recompui;
        blitz::ui::load();
        pv.context = create_context();
        pv.context.open();
        Element* root = pv.context.create_element<CrewRoot>(pv.context.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(0);
        root->set_left(0);
        root->set_right(0);
        root->set_bottom(0);
        root->set_display(Display::Flex);
        root->set_align_items(AlignItems::Center);
        root->set_justify_content(JustifyContent::Center);
        root->set_background_color(Color{4, 10, 26, 150});
        // The panel stays between the screen's top and the Pick / Back hints (CrewHints, 64 dp at the bottom).
        root->set_padding_top(blitz::ui::px(12));
        root->set_padding_bottom(64.0f + blitz::ui::px(8));
        pv.card = pv.context.create_element<Element>(root);
        pv.card->set_position(Position::Relative);
        pv.card->set_width(blitz::ui::px(kPanelMin));  // fit_panel sizes it for each page
        pv.card->set_max_height(100.0f, Unit::Percent);
        pv.card->set_display(Display::Flex);
        pv.card->set_flex_direction(FlexDirection::Column);
        blitz::ui::PanelOptions o;
        o.corners = true;
        Element* content = blitz::ui::panel(pv.context, pv.card, o);
        content->set_display(Display::Flex);
        content->set_flex_direction(FlexDirection::Column);
        content->set_flex(0.0f, 1.0f);  // shrinks to the panel's height: the rows scroll, the bar stays
        content->set_min_height(0.0f);
        content->set_gap(blitz::ui::px(10));
        content->set_padding(blitz::ui::px(kPad));
        content->set_as_navigation_container(NavigationType::Vertical);
        pv.eyebrow = pv.context.create_element<Label>(content, "", theme::Typography::LabelXS);
        pv.eyebrow->set_color(Color{143, 239, 245, 255});
        pv.eyebrow->set_flex_shrink(0);
        pv.title = pv.context.create_element<Label>(content, "", theme::Typography::Header2);
        pv.title->set_color(theme::color::SecondaryL);
        pv.title->set_flex_shrink(0);
        Element* rule = pv.context.create_element<Element>(content, 0, "div", false);
        rule->set_height(blitz::ui::px(4));
        rule->set_flex_shrink(0);
        rule->set_position(Position::Relative);
        blitz::ui::stretch(pv.context, rule, "rule_flame");
        // The rows' viewport: as tall as they are, or what's left of the panel, scrolling (a focused card scrolls
        // itself into view: add_card).
        pv.scroll = pv.context.create_element<Element>(content);
        pv.scroll->set_flex(0.0f, 1.0f);
        pv.scroll->set_min_height(0.0f);
        pv.scroll->set_overflow_x(Overflow::Hidden);
        pv.scroll->set_overflow_y(Overflow::Hidden);  // Auto when the rows are taller than it (crew_update)
        pv.body = pv.context.create_element<Element>(pv.scroll);
        pv.body->set_display(Display::Flex);
        pv.body->set_flex_direction(FlexDirection::Column);
        pv.body->set_gap(blitz::ui::px(10));
        pv.body->set_as_navigation_container(NavigationType::GridCol);
        pv.bar = pv.context.create_element<Element>(content);
        pv.bar->set_display(Display::Flex);
        pv.bar->set_flex_direction(FlexDirection::Row);
        pv.bar->set_flex_wrap(FlexWrap::Wrap);
        pv.bar->set_justify_content(JustifyContent::FlexEnd);
        pv.bar->set_gap(blitz::ui::px(8));
        pv.bar->set_margin_top(blitz::ui::px(6));
        pv.bar->set_flex_shrink(0);
        pv.bar->set_as_navigation_container(NavigationType::Horizontal);
        pv.context.create_element<CrewHints>(root);
        pv.context.close();
    }

    void open_panel() {
        if (pv.context == recompui::ContextId::null()) build_ui();
        pv.context.open();
        build_page();
        pv.context.close();
        recompui::show_context(pv.context, "");
    }

    void back() {
        if (flow.stack.size() > 1) {
            flow.stack.pop_back();
            pv.rebuild_page = true;
            return;
        }
        if (pregame) {  // Season's pre-game screen: back to it (Go Play there plays); once, as a held key repeats
            if (wait_state != Open) return;
            finish();
            wait_state = Cancelled;
            return;
        }
        // The first page: back to team select, as □ there (team, 2026-10-29: no way back, and none at all by touch).
        // Redo Play's menu step goes back to before the pick that brought the panel up; the game waits here meanwhile.
        if (!blitz::redo_play_enabled()) return;
        recompui::hide_context(pv.context);
        {
            std::lock_guard lock{flow_mutex};
            flow.stack.clear();
            flow.crews.clear();
        }
        cancelled_at = Clock::now();
        wait_state = Cancelled;
        blitz_redo_request();
        fprintf(stderr, "[crew] back to team select\n");
    }

    void finish() {
        recompui::hide_context(pv.context);
        std::lock_guard lock{flow_mutex};
        flow.stack.clear();
        flow.crews.clear();
        wait_state = Done;
    }

    // --- Writing a crew into a game -------------------------------------------------------------------------------

    void write_text(uint8_t* rdram, uint32_t a, const std::string& s, uint32_t len) {
        for (uint32_t i = 0; i < len; i++) MEM_B(i, (gpr)(int32_t)a) = (int8_t)(i < s.size() && i < len - 1 ? s[i] : 0);
    }
    std::string read_text(uint8_t* rdram, uint32_t a, uint32_t len) {
        std::string s;
        for (uint32_t i = 0; i < len; i++) {
            char c = (char)MEM_B(i, (gpr)(int32_t)a);
            if (c == 0) break;
            s += c;
        }
        return s;
    }
}

namespace {
    recompui::ContextId vs_ctx = recompui::ContextId::null();
    bool vs_shown = false;
    int vs_stage = 0;  // 1: portraits queued, 2: built

    void vs_side(recompui::Element* parent, int s) {
        using namespace recompui;
        Element* col = vs_ctx.create_element<Element>(parent);
        col->set_display(Display::Flex);
        col->set_flex_direction(FlexDirection::Column);
        col->set_align_items(AlignItems::Center);
        col->set_gap(blitz::ui::px(6));
        col->set_width(blitz::ui::px(280));
        std::string title, sub;
        std::vector<std::string> ids;
        if (vs_pro[s]) {
            ProMeta m = load_meta(vs_key[s]);
            title = m.name;
            sub = upper(uniform_modes[m.uniforms % 3]) + " UNIFORMS";
            auto crew = saved_crew(vs_key[s]);
            for (int sl : {3, 4, 5, 6}) if (crew.count(sl)) ids.push_back(crew[sl]);
        }
        else {
            title = upper(team_name(vs_key[s]));
        }
        Label* t = vs_ctx.create_element<Label>(col, title, theme::Typography::Header1);
        t->set_color(theme::color::SecondaryL);
        t->set_font_size(blitz::ui::px(vs_pro[s] ? 64 : 30));
        if (!sub.empty()) {
            Label* u = vs_ctx.create_element<Label>(col, sub, theme::Typography::LabelXS);
            u->set_color(Color{143, 239, 245, 255});
        }
        Element* faces = vs_ctx.create_element<Element>(col);
        faces->set_display(Display::Flex);
        faces->set_flex_direction(FlexDirection::Row);
        faces->set_gap(blitz::ui::px(6));
        for (const std::string& id : ids) {
            auto p = blitz::roster_player(id);
            if (!p) continue;
            Element* card = vs_ctx.create_element<Element>(faces);
            card->set_display(Display::Flex);
            card->set_flex_direction(FlexDirection::Column);
            card->set_align_items(AlignItems::Center);
            card->set_width(blitz::ui::px(70));
            Element* frame = vs_ctx.create_element<Element>(card);
            frame->set_width(blitz::ui::px(58));
            frame->set_height(blitz::ui::px(42));
            frame->set_border_radius(blitz::ui::px(3));
            frame->set_background_color(Color{24, 40, 78, 255});
            if (face_usable(id)) {
                Image* img = vs_ctx.create_element<Image>(frame, face_src(id));
                img->set_width(blitz::ui::px(58));
                img->set_height(blitz::ui::px(42));
                img->set_border_radius(blitz::ui::px(3));
            }
            std::string last = upper(p->last);
            if (last.size() > 8) last = last.substr(0, 7) + "\u2026";
            Label* n = vs_ctx.create_element<Label>(card, last, theme::Typography::LabelXS);
            n->set_font_size(blitz::ui::px(10));
            n->set_letter_spacing(blitz::ui::px(0.4f));
            n->set_white_space(WhiteSpace::Nowrap);
            Label* tm = vs_ctx.create_element<Label>(card, p->team, theme::Typography::LabelXS);
            tm->set_font_size(blitz::ui::px(9));
            tm->set_color(theme::color::TextDim);
        }
    }

    void vs_build() {
        using namespace recompui;
        if (vs_ctx == ContextId::null()) {
            vs_ctx = create_context();
            vs_ctx.open();
            vs_ctx.set_captures_input(false);
            vs_ctx.set_captures_mouse(false);
            vs_ctx.close();
        }
        vs_ctx.open();
        vs_ctx.get_root_element()->clear_children();
        Element* root = vs_ctx.create_element<Element>(vs_ctx.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(0);
        root->set_left(0);
        root->set_right(0);
        root->set_height(80, Unit::Percent);  // the cheat boxes along the bottom stay in view
        root->set_pointer_events(PointerEvents::None);
        root->set_display(Display::Flex);
        root->set_align_items(AlignItems::Center);
        root->set_justify_content(JustifyContent::Center);
        root->set_background_color(Color{6, 14, 34, 255});
        Element* box = vs_ctx.create_element<Element>(root);
        box->set_position(Position::Relative);
        box->set_width(blitz::ui::px(760));
        blitz::ui::PanelOptions o;
        o.corners = true;
        Element* content = blitz::ui::panel(vs_ctx, box, o);
        content->set_display(Display::Flex);
        content->set_flex_direction(FlexDirection::Column);
        content->set_align_items(AlignItems::Center);
        content->set_padding(blitz::ui::px(18));
        content->set_gap(blitz::ui::px(10));
        Label* eyebrow = vs_ctx.create_element<Label>(content, "PRO BOWL", theme::Typography::LabelSM);
        eyebrow->set_color(Color{143, 239, 245, 255});
        Element* rule = vs_ctx.create_element<Element>(content, 0, "div", false);
        rule->set_height(blitz::ui::px(4));
        rule->set_width(100, Unit::Percent);
        rule->set_position(Position::Relative);
        blitz::ui::stretch(vs_ctx, rule, "rule_flame");
        Element* row = vs_ctx.create_element<Element>(content);
        row->set_display(Display::Flex);
        row->set_flex_direction(FlexDirection::Row);
        row->set_align_items(AlignItems::Center);
        row->set_justify_content(JustifyContent::Center);
        row->set_gap(blitz::ui::px(24));
        vs_side(row, 0);
        Label* vs = vs_ctx.create_element<Label>(row, "VS", theme::Typography::Header1);
        vs->set_font_size(blitz::ui::px(56));
        vs->set_color(Color{246, 212, 0, 255});
        vs_side(row, 1);
        vs_ctx.close();
    }
}

namespace {
    // --- Headshot prefetch ----------------------------------------------------------------------------------------

    std::mutex hover_mutex;
    std::vector<std::string> hover_teams;  // franchises a team select cursor rested on (game thread -> main thread)

    // Main thread: the players a team's crew pages show as cards, the offense page's rows first; or only those.
    std::vector<std::string> crew_card_ids(const std::string& f, bool offense_only) {
        std::vector<std::string> ids;
        TeamCrew t = make_team(f);
        for (int r = 0; r < (offense_only ? (int)LINE : (int)RowCount); r++) {
            for (const auto* p : t.pool(r)) ids.push_back(p->id);
        }
        return ids;
    }

    // Headshots are worth fetching ahead: Modern's 2026 Season, photos allowed (the season server's switch), and
    // not a lean device (BLITZ_LEAN: on demand only, so a potato's memory and network go to the game).
    bool prefetch_wanted() {
        return !lean_device() && blitz::active_pack_id() == "blitz2026" && blitz::headshots_allowed();
    }

    // Every UI tick (crew_update): the hovered teams' cards, and the background preload of every team's offense,
    // a team a tick, once the game has been up a few seconds in the menus (the rosters are in by then).
    void faces_tick() {
        preload_go = blitz_play_mode() != 2;
        std::vector<std::string> hovered;
        {
            std::lock_guard lock{hover_mutex};
            hovered.swap(hover_teams);
        }
        for (const std::string& f : hovered) {
            if (enabled() && prefetch_wanted() && !blitz::roster_of(f).empty()) prefetch(crew_card_ids(f, false), Hovered, "team select (" + f + ")");
        }

        static int stage = 0;  // 0 waiting, 1 a team a tick, 2 done
        static Clock::time_point started{}, looked{};
        static std::vector<std::string> teams, ids;
        static size_t next = 0;
        if (stage == 2) return;
        Clock::time_point now = Clock::now();
        if (stage == 0) {
            if (!ultramodern::is_game_started()) return;
            if (started == Clock::time_point{}) started = now;
            if (now - started < std::chrono::seconds(8) || now - looked < std::chrono::seconds(1)) return;
            looked = now;
            if (lean_device()) {
                fprintf(stderr, "[crew] lean device: no headshot preload (fetched when a card shows)\n");
                stage = 2;
                return;
            }
            if (blitz_play_mode() != 0 || !prefetch_wanted()) return;  // looked at again in a second
            for (const std::string& f : blitz::team_franchises()) {
                if (!f.empty() && std::find(teams.begin(), teams.end(), f) == teams.end()) teams.push_back(f);
            }
            if (teams.empty() || blitz::roster_of(teams.front()).empty()) {  // no rosters yet
                teams.clear();
                return;
            }
            stage = 1;
        }
        if (next < teams.size()) {
            auto team_ids = crew_card_ids(teams[next++], true);
            ids.insert(ids.end(), team_ids.begin(), team_ids.end());
        }
        if (next >= teams.size()) {
            fprintf(stderr, "[crew] background preload: the offense's headshots for %zu teams (%zu players)\n", teams.size(), ids.size());
            prefetch(ids, Preload, "background preload");
            ids.clear();
            ids.shrink_to_fit();
            stage = 2;
        }
    }
}

namespace blitz {
    // Each frame of team select's loop (uniform_picker.cpp, game thread): a side's cursor (a game slot). A team it
    // rests on for half a second has its crew cards' faces fetched, so Pick Your Crew opens with them.
    void crew_hover(int side, int slot) {
        static int last[2] = {-1, -1};
        static Clock::time_point since[2];
        static bool sent[2] = {false, false};
        if (side < 0 || side > 1 || slot < 0) return;
        Clock::time_point now = Clock::now();
        if (slot != last[side]) {
            last[side] = slot;
            since[side] = now;
            sent[side] = false;
            return;
        }
        if (sent[side] || now - since[side] < std::chrono::milliseconds(500)) return;
        sent[side] = true;
        int t = team_of_slot(slot);
        auto franchises = team_franchises();
        if (t < 0 || (size_t)t >= franchises.size()) return;
        std::lock_guard lock{hover_mutex};
        hover_teams.push_back(franchises[(size_t)t]);
    }
}

namespace blitz {
    // Roster Room (roster_room.cpp): every team's saved crew, and Lock In's replacement for the teams it sends
    // (a team with no slots left loses its crew). Pro Bowl crews (PRO_*) are left alone.
    std::string crews_json() {
        std::lock_guard lock{crews_mutex};
        load_crews();
        return crews.dump();
    }
    int crews_lock_in(const std::string& body) {
        json in = json::parse(body, nullptr, false);
        if (!in.is_object()) return -1;
        std::lock_guard lock{crews_mutex};
        load_crews();
        int n = 0;
        for (auto& [f, slots] : in.items()) {
            if (f.rfind("PRO_", 0) == 0 || !slots.is_object()) continue;
            json clean = json::object();
            for (auto& [k, v] : slots.items()) {
                int slot = std::atoi(k.c_str());
                if (slot >= 0 && slot <= 14 && v.is_string() && !v.get<std::string>().empty()) clean[k] = v;
            }
            if (clean.empty()) crews.erase(f);
            else crews[f] = clean;
            n++;
        }
        save_crews();
        return n;
    }

    // The team's saved crew (Roster Room, Pick Your Crew) as players, slot by slot (content_pack_tp.cpp lays them over
    // the weekly lineup on every ovl_b load). A player who's left the team is skipped: the lineup's stays.
    std::map<int, LineupPlayer> crew_players(const std::string& franchise) {
        std::map<int, LineupPlayer> out;
        auto crew = saved_crew(franchise);
        if (crew.empty()) return out;
        for (auto& [slot, id] : crew) {
            if (slot < 0 || slot > 14) continue;
            auto p = roster_player(id);
            if (!p || p->number < 0 || p->team != franchise) continue;
            LineupPlayer lp;
            lp.last = p->last;
            lp.number = p->number;
            lp.heavy = (slot <= 2 || (slot >= 7 && slot <= 9)) ? 1 : (slot == 3 || slot == 14) ? 0 : p->heavy;
            lp.skin = p->skin;
            out[slot] = lp;
        }
        return out;
    }

    // At startup (game_init.cpp): <app folder>/probowl/pb_*.wms, made by tools/make_probowl_art.py.
    void probowl_register_files() {
        std::error_code ec;
        int n = 0;
        // The app's copy, then yours, which wins: a file registered again replaces the first.
        std::vector<fs::path> dirs;
        for (const auto& c : blitz::content_dirs()) dirs.push_back(c / "probowl");  // Resources, the 2026 Season pak
        dirs.push_back(recomp::get_config_path() / "probowl");
        for (const fs::path& dir : dirs) {
            std::vector<fs::path> files;  // by name: the order files are added in decides their indices
            for (const auto& e : fs::directory_iterator(dir, ec)) files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (const fs::path& path : files) {
                std::string name = path.filename().string();
                if (name.rfind("pb_", 0) != 0 || path.extension() != ".wms") continue;
                std::ifstream in{path, std::ios::binary};
                std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
                if (data.size() >= 32 && data[0] == 'w' && data[1] == 'n' && fs_register_file(name, std::move(data))) n++;
            }
        }
        if (n) fprintf(stderr, "[probowl] %d art files\n", n);
    }

    // A player's first Pro Bowl: no code saved yet, so R at team select asks for one (uniform_picker.cpp, game thread);
    // crew_update opens the keyboard on the UI tick.
    std::atomic<int> ask_side{-1};
    std::atomic<int> ask_state[2] = {0, 0};  // 0 idle, 1 asking, 2 typed, 3 cancelled
    bool probowl_code_saved(int side) {
        std::lock_guard lock{crews_mutex};
        load_crews();
        return crews.contains(std::string(side == 0 ? "PRO_LEFT" : "PRO_RIGHT") + "_META");
    }
    void probowl_ask_code(int side) {
        ask_state[side] = 1;
        ask_side = side;
    }
    int probowl_ask_state(int side) { return ask_state[side]; }

    // The code a Pro Bowl side shows at team select, when R picks it (uniform_picker.cpp): a player's saved one; the
    // CPU's is dealt now (kept for the game) so the list, the scoreboard and the panel agree.
    std::string probowl_code(int side, bool human) {
        if (human) return load_meta(side == 0 ? "PRO_LEFT" : "PRO_RIGHT").name;
        ProMeta m = load_meta("PRO_CPU");
        std::string mine = load_meta(side == 0 ? "PRO_RIGHT" : "PRO_LEFT").name;
        do m.name = pro_names[std::uniform_int_distribution<size_t>(0, std::size(pro_names) - 1)(rng())];
        while (m.name == mine);
        save_meta("PRO_CPU", m);
        cpu_named = true;
        return m.name;
    }

    void probowl_names(uint8_t* rdram) {
        if (!names_pending) return;
        const auto& franchises = team_franchises();
        uint32_t h = (uint32_t)MEM_W(0, (gpr)(int32_t)humans);
        for (int s = 0; s < 2; s++) {
            if (!probowl_on(s)) continue;
            int32_t team = MEM_W(0, (gpr)(int32_t)(chosen + 4 * s));
            if (team < 0 || (size_t)team >= franchises.size()) continue;
            bool human = ((h >> (2 * s)) & 3) != 0;
            ProMeta m = load_meta(!human ? "PRO_CPU" : s == 0 ? "PRO_LEFT" : "PRO_RIGHT");
            teamdata::live::set_abbr(rdram, team, m.name);
            // The art, here too: the game copies it into this game's record before kickoff, and the play select logos
            // load from it then. (Not the nickname: the VS screen picks the announcer by it.)
            for (const auto& [art, role] : pro_art) {
                int32_t idx = fs_file_index(std::string("pb_") + role + (s == 0 ? "_l" : "_r") + ".wms");
                if (idx >= 0 && (uint32_t)idx < fs_ram_count(rdram)) teamdata::live::set_art(rdram, team, art, (uint16_t)(idx + 1));
            }
        }
    }

    // Every UI tick (main.cpp): opens the panel when team select asks, applies deferred rebuilds, shows portraits.
    void crew_update() {
        faces_tick();
        static int asking = -1;  // the side whose keyboard is up
        if (int s = ask_side.exchange(-1); s >= 0) {
            asking = s;
            recompui::open_keyboard("Pro Bowl · your team code, on the scoreboard", "", 3, true, [s](const std::string& typed) {
                if (typed.empty()) {
                    ask_state[s] = 3;
                    return;
                }
                const std::string key = s == 0 ? "PRO_LEFT" : "PRO_RIGHT";
                ProMeta m = load_meta(key);
                m.name = pro_abbr(typed);
                save_meta(key, m);
                ask_state[s] = 2;
            });
        }
        else if (asking >= 0 && !recompui::is_keyboard_open()) {
            if (ask_state[asking] == 1) ask_state[asking] = 3;  // closed with Back
            asking = -1;
        }
        // The Pro Bowl VS card: portraits queued on one tick, the card built on the next (images load a tick late).
        if (vs_wanted && !vs_shown) {
            if (vs_stage == 0) {
                for (int s = 0; s < 2; s++) {
                    if (!vs_pro[s]) continue;
                    auto crew = saved_crew(vs_key[s]);
                    for (int sl : {3, 4, 5, 6}) if (crew.count(sl)) show_face(crew[sl]);
                }
                vs_stage = 1;
                ui_tick++;
            }
            else {
                vs_build();
                recompui::show_context(vs_ctx, "");
                vs_shown = true;
            }
        }
        else if (!vs_wanted && vs_shown) {
            recompui::hide_context(vs_ctx);
            vs_shown = false;
            vs_stage = 0;
        }
        if (wait_state == Requested) {
            if (!ultramodern::is_game_started()) return;
            wait_state = Open;
            open_panel();
            return;
        }
        if (wait_state != Open || pv.context == recompui::ContextId::null()) return;
        if (!pv.rebuild_page && !flow.stack.empty()) {
            const Page& page = flow.stack.back();
            pv.context.open();
            // The scroll bar only when the rows don't fit: fractions of a pixel over (dp to pixels) would draw one.
            // Once it's on, the focused card scrolls into view (a resize can leave it below the fold).
            if (pv.scroll_refocus) {
                pv.scroll_refocus = false;
                if (recompui::Element* f = pv.context.get_focused_element()) f->scroll_into_view();
            }
            bool overflowing = pv.body->get_client_height() > pv.scroll->get_client_height() + 1.0f;
            if (overflowing != pv.scroll_on) {
                pv.scroll_on = overflowing;
                pv.scroll_refocus = overflowing;
                pv.scroll->set_overflow_y(overflowing ? recompui::Overflow::Auto : recompui::Overflow::Hidden);
            }
            // A new window size lays the page out again (the panel's width and cards per line follow it); so does a
            // rebuilt row (FLEX after a WR pick) that's grown wider than the panel when the screen has room for it.
            float sw = screen_w();
            bool relayout = sw > 0.0f && std::fabs(sw - pv.layout_w) > 1.0f;
            if (!relayout && page.kind != TeamPage && flow.crews.count(page.side) && !flow.crews[page.side].pro) {
                for (int row : pv.rebuild_rows) {
                    if ((int)flow.crews[page.side].pool(row).size() > pv.fit && pv.fit < pv.fit_max) relayout = true;
                }
            }
            if (relayout) {
                keep_focus();
                pv.rebuild_page = true;
            }
            pv.context.close();
        }
        if (pv.rebuild_page || !pv.rebuild_rows.empty()) {
            pv.context.open();
            if (pv.rebuild_page) build_page();
            else {
                int side = flow.stack.back().side;
                for (int row : pv.rebuild_rows) if (pv.row_lines.count(row)) build_row_cards(side, row);
            }
            pv.context.close();
            pv.rebuild_page = false;
            pv.rebuild_rows.clear();
        }
        // Start moves on, without hunting for the button: any player's. Player 1's only left a phone that joined after a
        // pad (as player 2) with a START that did nothing here (team, 1.0.1, every device).
        static bool start_was_down = false;
        bool start_down = false;
        for (int port = 0; port < 4 && !start_down; port++) {
            SDL_GameController* pad = blitz::pad_on_port(port);
            start_down = pad != nullptr && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START) != 0;
        }
        if (start_down && !start_was_down && pv.start_action && !recompui::is_keyboard_open() && !recompui::is_prompt_open()) {
            start_was_down = true;
            std::function<void()> go = pv.start_action;
            go();
            return;
        }
        start_was_down = start_down;
        ui_tick++;
        {
            std::vector<std::string> ready, missing;
            {
                Faces& F = faces();
                std::lock_guard lock{F.mutex};
                ready.swap(F.ready);
                missing.swap(F.missing);
            }
            for (const std::string& id : ready) queue_face(id);  // shown from the next tick
            for (const std::string& id : missing) queue_portrait(id);
        }
        std::vector<std::string> show;
        for (const std::string& id : pv.blank) if (face_usable(id)) show.push_back(id);
        if (!show.empty()) {
            pv.context.open();
            for (const std::string& id : show) {
                for (const auto& c : pv.cards) {
                    if (c.id != id) continue;
                    c.face->set_src(face_src(id));
                    c.face->set_display(recompui::Display::Block);
                }
                pv.blank.erase(id);
            }
            pv.context.close();
        }
        if (!pv.faces_timed && pv.blank.empty()) {
            pv.faces_timed = true;
            long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - pv.faces_from).count();
            fprintf(stderr, "[crew] panel faces: %zu card(s), %zu not on hand when the page opened, all showing after %ld ms\n",
                    pv.faces_cards, pv.faces_blank, ms);
        }
    }

    // Season's pre-game screen (season_pregame.cpp): the panel for the player's team and the CPU's, outside team select
    // (picks are saved as they're made; the kickoff writes them in). False when Pick Your Crew can't open.
    bool crew_pregame_start(const std::string& mine, const std::string& cpu) {
        if (!enabled() || wait_state != Idle || mine.empty() || blitz::roster_of(mine).empty()) return false;
        // The week the game will lay, as team select reads it: a season's later week has no team select of its own, so
        // the panel showed the last team select's (or, after a restart, the latest) week's lineup with Advanced on.
        if (uint8_t* rdram = blitz::monitor_rdram()) lineup_week = blitz::game_lineup_week(rdram);
        std::lock_guard lock{flow_mutex};
        flow = Flow{};
        flow.team[0] = mine;
        flow.human[0] = true;
        flow.team[1] = cpu;
        flow.stack.push_back({0, Offense});
        pregame = true;
        requested_at = Clock::now();
        wait_state = Requested;
        return true;
    }
    // 1 while it's up; once it closes, 2 for Go Play, 0 for Back (and the panel is free again).
    int crew_pregame_state() {
        if (!pregame) return 0;
        int st = wait_state.load();
        if (st == Requested || st == Open) return 1;
        wait_state = Idle;
        pregame = false;
        return st == Done ? 2 : 0;
    }

    // Settings → Rules (house_rules.cpp).
    void add_crew_option(recomp::config::Config& c) {
        c.add_bool_option("pick_crew", "Pick Your Crew",
                          "After team select, pick who plays for your team: a quarterback, a running back, a receiver "
                          "and a flex from the skill players left, the line if you like, then your defense (three "
                          "linemen, four behind them) and the CPU's team. Looks only: the game rates teams, not players. "
                          "Picks are remembered for each team; anything you don't pick is the Lineups setting's player "
                          "(Settings → Teams). With the 2026 Season only.",
                          true);
    }
}

extern "C" int blitz_opp_preview(uint8_t* rdram);  // opponent_select.cpp

extern "C" {
    // ovl_b 0x802F89CC, once every player has picked: true while team select should wait for the panel.
    int blitz_crew_wait(uint8_t* rdram) {
        switch (wait_state.load()) {
            case Requested:
                if (Clock::now() - requested_at > std::chrono::seconds(3)) {  // the UI never came up: carry on
                    wait_state = Idle;
                    return 0;
                }
                return 1;
            case Open:
                return 1;
            case Cancelled:  // Back on the first page: hold while the menu step is restored (then this isn't reached)
                if (Clock::now() - cancelled_at < std::chrono::milliseconds(1500)) return 1;
                wait_state = Idle;  // no step to go back to: the panel comes up again
                break;
            case Done: {
                wait_state = Idle;
                names_pending = true;
                blitz::probowl_names(rdram);
                const auto& fr = blitz::team_franchises();
                uint32_t hh = (uint32_t)MEM_W(0, (gpr)(int32_t)humans);
                for (int s = 0; s < 2; s++) {
                    bool human = ((hh >> (2 * s)) & 3) != 0;
                    vs_pro[s] = blitz::probowl_on(s);
                    int32_t team = MEM_W(0, (gpr)(int32_t)(chosen + 4 * s));
                    int t = team >= 0 ? blitz::team_of_slot(team) : -1;
                    vs_key[s] = vs_pro[s] ? (!human ? "PRO_CPU" : s == 0 ? "PRO_LEFT" : "PRO_RIGHT")
                                          : t >= 0 && (size_t)t < fr.size() ? fr[(size_t)t] : "";
                }
                vs_wanted = vs_pro[0] || vs_pro[1];
                return 0;
            }
            default:
                break;
        }
        bool any_pro = blitz::probowl_on(0) || blitz::probowl_on(1);
        if ((!enabled() && !any_pro) || MEM_W(0, (gpr)(int32_t)phase) == 7 || blitz::active_pack_id() != "blitz2026") return 0;
        const auto& franchises = blitz::team_franchises();
        uint32_t h = (uint32_t)MEM_W(0, (gpr)(int32_t)humans);
        lineup_week = blitz::game_lineup_week(rdram);
        std::lock_guard lock{flow_mutex};
        flow = Flow{};
        for (int s = 0; s < 2; s++) {
            flow.human[s] = ((h >> (2 * s)) & 3) != 0;
            flow.pro[s] = blitz::probowl_on(s);
            int32_t team = MEM_W(0, (gpr)(int32_t)(chosen + 4 * s));
            if (team < 0 && !flow.human[s]) team = blitz_opp_preview(rdram);  // the CPU's, picked early
            if (team >= 0) team = blitz::team_of_slot(team);  // a game slot -> the team in it
            if (team >= 0 && (size_t)team < franchises.size()) flow.team[s] = franchises[(size_t)team];
            else if (flow.human[s]) return 0;
        }
        // A CPU Pro Bowl crew: the league's top players at random, new each game (Pick CPU can change it).
        for (int s = 0; s < 2; s++) {
            if (flow.pro[s] && !flow.human[s]) {
                (void)make_pro("PRO_CPU", true);
                ProMeta m = load_meta("PRO_CPU");
                std::string mine = load_meta(flow.pro[0] && flow.human[0] ? "PRO_LEFT" : "PRO_RIGHT").name;
                if (!cpu_named) {
                    do m.name = pro_names[std::uniform_int_distribution<size_t>(0, std::size(pro_names) - 1)(rng())];
                    while (m.name == mine);
                }
                cpu_named = false;
                m.uniforms = (int)std::uniform_int_distribution<int>(0, 2)(rng());
                save_meta("PRO_CPU", m);
            }
        }
        int first = flow.human[0] ? 0 : flow.human[1] ? 1 : -1;
        if (first < 0 || (!enabled() && !flow.pro[first] && !flow.pro[1 - first]) || blitz::roster_of(flow.team[first]).empty()) return 0;
        if (!enabled() && !flow.pro[first]) first = 1 - first;  // only the other human's Pro Bowl crew to build
        if (!flow.human[first]) return 0;
        flow.stack.push_back({first, flow.pro[first] ? TeamPage : Offense});
        fprintf(stderr, "[crew] team select: %s%s v %s%s\n", flow.team[0].c_str(), flow.human[0] ? " (human)" : "",
                flow.team[1].c_str(), flow.human[1] ? " (human)" : "");
        requested_at = Clock::now();
        wait_state = Requested;
        return 1;
    }

    // ovl_a 0x80302F40 (uniform_picker.cpp's kickoff hook): each side's crew into this game's copy of the team.
    void blitz_crew_kickoff(uint8_t* rdram, int32_t team0, int32_t team1, int32_t set0, int32_t set1) {
        names_pending = false;  // the game's copies have them from here
        vs_wanted = false;
        if (blitz::active_pack_id() != "blitz2026") return;
        const auto& franchises = blitz::team_franchises();
        int32_t team[2] = {team0, team1};
        uint32_t h = (uint32_t)MEM_W(0, (gpr)(int32_t)humans);
        for (int s = 0; s < 2; s++) {
            if (team[s] < 0 || (size_t)team[s] >= franchises.size()) continue;
            if (!live::has_copy(rdram, s)) continue;
            if (blitz::probowl_on(s)) {  // a Pro Bowl crew: all 15 players, its name, each in his own team's uniform
                bool human = ((h >> (2 * s)) & 3) != 0;
                std::string key = !human ? "PRO_CPU" : s == 0 ? "PRO_LEFT" : "PRO_RIGHT";
                auto crew = saved_crew(key);
                if (crew.empty()) {
                    (void)make_pro(key, false);
                    crew = saved_crew(key);
                }
                ProMeta m = load_meta(key);
                // One crew in colour, the other in white (team): two crews, the left dark; against a team, the
                // opposite of the set the game gave it.
                bool dark = blitz::probowl_on(1 - s) ? s == 0 : (s == 0 ? set1 : set0) != 0;
                live::set_copy_abbr(rdram, s, m.name);   // the scoreboard
                live::set_copy_names(rdram, s, m.name);  // coin toss, quarter banner, halftime and final, line scores
                for (const auto& [art, role] : pro_art) {
                    int32_t idx = blitz::fs_file_index(std::string("pb_") + role + (s == 0 ? "_l" : "_r") + ".wms");
                    if (idx >= 0 && (uint32_t)idx < blitz::fs_ram_count(rdram)) live::set_copy_art(rdram, s, art, (uint16_t)(idx + 1));
                }
                int n = 0;
                for (auto& [slot, id] : crew) {
                    if (slot < 0 || slot > 14) continue;
                    auto p = blitz::roster_player(id);
                    if (!p || p->number < 0) continue;
                    live::set_copy_last(rdram, s, slot, p->last);
                    live::clear_copy_call(rdram, s, slot);
                    live::set_copy_number(rdram, s, slot, p->number);
                    int heavy = (slot <= 2 || (slot >= 7 && slot <= 9)) ? 1 : (slot == 3 || slot == 14) ? 0 : p->heavy;
                    live::set_copy_heavy(rdram, s, slot, heavy);
                    if (p->skin >= 0) live::set_copy_skin(rdram, s, slot, p->skin);
                    // His own team's uniform: its 2026 home set, the game's 1999 home, or any of its sets.
                    auto it = std::find(franchises.begin(), franchises.end(), p->team);
                    if (it != franchises.end()) {
                        int t = (int)(it - franchises.begin());
                        int set = blitz::uniform_set_pick(t, m.uniforms, dark, (uint32_t)rng()());
                        blitz::mixed_plan(s, slot, t, set);
                    }
                    n++;
                }
                fprintf(stderr, "[probowl] kickoff: side %d \"%s\" (%s uniforms, %s), %d players\n", s, m.name.c_str(),
                        uniform_modes[m.uniforms % 3], dark ? "dark" : "light", n);
                continue;
            }
            if (!enabled()) continue;
            const std::string& f = franchises[(size_t)blitz::team_of_slot(team[s])];
            auto crew = saved_crew(f);
            if (crew.empty()) continue;
            std::set<std::string> on_team;
            for (const auto& p : blitz::roster_of(f)) on_team.insert(p.id);
            int n = 0;
            for (auto& [slot, id] : crew) {
                if (slot < 0 || slot > 13 || !on_team.count(id)) continue;  // traded or released: the lineup's player
                auto p = blitz::roster_player(id);
                if (!p || p->number < 0) continue;
                if (live::copy_last(rdram, s, slot) != p->last) {
                    live::set_copy_last(rdram, s, slot, p->last);
                    live::clear_copy_call(rdram, s, slot);
                }
                live::set_copy_number(rdram, s, slot, p->number);
                int heavy = (slot <= 2 || (slot >= 7 && slot <= 9)) ? 1 : slot == 3 ? 0 : p->heavy;
                live::set_copy_heavy(rdram, s, slot, heavy);
                if (p->skin >= 0) live::set_copy_skin(rdram, s, slot, p->skin);
                n++;
            }
            fprintf(stderr, "[crew] kickoff: %s, %d picked slot(s)\n", f.c_str(), n);
        }
    }
}
#else
#include <cstdint>
extern "C" int blitz_crew_wait(uint8_t*) { return 0; }  // Classic: no player file
#endif
