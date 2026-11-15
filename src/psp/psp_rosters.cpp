// Weekly rosters on the PSP (docs/PSP_PORT.md §6, M8): rosters_live.h's interface over one text file, psp/pack.txt, instead of
// the desktop's players.json (600 KB of JSON) and lineups/*.json. The weekly job (or tools/psp/make_pack.py) writes it:
//   #blitz-psp-pack/1 <latest week> <updated>
//   P|<id>|<first>|<last>|<number>|<pos>|<team>|<heavy>|<skin>     one per player any lineup in the pack uses
//   L|<label>|<TEAM>|<id 1>|...|<id 15>                          one per team per lineup ("" = the pack's own player)
// It is kept in <app folder>/rosters/pack.txt (the kit bundles the current one, so offline play works) with the hub's
// manifest.json beside it. A check (at boot once the net is up, on the `rosters` command, on resume) reads the hub's
// manifest.json; if psp/pack.txt's hash differs from the saved manifest's, the pack is fetched, its SHA-1 checked against
// the manifest, written tmp + rename, and parsed. content_pack_tp.cpp lays the lineup over the teams on every ovl_b load
// (lineup_now), so a new pack shows from the next menu, as on the desktop.
//
// The hub: net.cfg's hub_base on the PSP; psp-host takes BLITZ_ROSTER_SOURCE (a base URL).
#if BLITZ_MODERN
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "recompui/config.h"
#include "librecomp/config.hpp"
#include "rosters_live.h"
#include "psp_http.h"
#include "psp_log.h"
#include "psp_sha256.h"
#include "psp_sys.h"
#if defined(__PSP__)
#include "psp_net.h"
#include "psp_remote.h"
#endif

namespace blitz {
    void post_status(const std::string& msg);  // notices.cpp
}

namespace {
    using json = nlohmann::json;
    constexpr const char* opt_lineups = "lineups";
    constexpr const char* pack_path = "psp/pack.txt";
    enum Choice : uint32_t { ThisWeek = 0, Full = 1, Packed = 2, FirstWeek = 10 };

    std::mutex mutex;
    std::map<std::string, blitz::RosterPlayer> players;                  // id -> player
    std::vector<std::string> player_order;                              // the file's order (roster_of)
    std::map<std::string, std::shared_ptr<blitz::Lineup>> lineups;      // label -> lineup
    std::string week, updated;
    bool loaded = false;
    std::vector<std::string> week_keys;      // the Lineups choices 10, 11, ...
    std::vector<std::string> pending_messages;
    std::atomic<bool> checking{false};
    std::atomic<int> checks{0};
    int check_thread_id = 0;                 // the boot check's thread, joined (its 64 KB stack freed) once it has ended

    std::string dir() { return psplog::app_path("rosters"); }

    int ordinal(const std::string& label) {
        int year = 0, n = 0;
        if (std::sscanf(label.c_str(), "%d-w%d", &year, &n) == 2) return year * 100 + n;
        if (std::sscanf(label.c_str(), "%d-post%d", &year, &n) == 2) return year * 100 + 18 + n;
        return 0;
    }
    std::string display(const std::string& label) {
        int o = ordinal(label) % 100;
        static const char* post[] = {"Wild Card", "Divisional Round", "Conference Finals", "Pro Bowl", "Super Bowl"};
        if (o > 18 && o <= 23) return post[o - 19];
        return o ? "Week " + std::to_string(o) : label;
    }

    std::vector<std::string> split(const std::string& line) {
        std::vector<std::string> f;
        for (size_t p = 0;;) {
            size_t bar = line.find('|', p);
            f.push_back(line.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
            if (bar == std::string::npos) break;
            p = bar + 1;
        }
        return f;
    }

    struct Parsed {
        std::map<std::string, blitz::RosterPlayer> players;
        std::vector<std::string> order;
        std::map<std::string, std::shared_ptr<blitz::Lineup>> lineups;
        std::string week, updated;
        std::string error;
    };

    // The pack's text -> players and lineups (the lineup's slots as the desktop builds them from players.json).
    bool parse(const std::string& text, Parsed& out) {
        if (text.rfind("#blitz-psp-pack/1 ", 0) != 0) {
            out.error = "not a blitz-psp-pack/1 file";
            return false;
        }
        size_t at = 0;
        int lines = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? text.size() : end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines++;
            if (line.empty()) continue;
            if (line[0] == '#') {
                if (lines == 1) {
                    char w[32] = {}, u[40] = {};
                    if (sscanf(line.c_str(), "#blitz-psp-pack/1 %31s %39s", w, u) >= 1) {
                        out.week = w;
                        out.updated = u;
                    }
                }
                continue;
            }
            std::vector<std::string> f = split(line);
            if (f[0] == "P" && f.size() == 9) {
                blitz::RosterPlayer p;
                p.id = f[1];
                p.first = f[2];
                p.last = f[3];
                p.number = f[4].empty() ? -1 : atoi(f[4].c_str());
                p.pos = f[5];
                p.team = f[6];
                p.heavy = atoi(f[7].c_str());
                p.skin = f[8].empty() ? -1 : atoi(f[8].c_str());
                p.status = "active";
                if (!out.players.count(p.id)) out.order.push_back(p.id);
                out.players[p.id] = p;
            }
            else if (f[0] == "L" && f.size() == 18) {
                auto& l = out.lineups[f[1]];
                if (!l) {
                    l = std::make_shared<blitz::Lineup>();
                    l->label = f[1];
                }
                auto& slots = l->teams[f[2]];
                auto& ids = l->ids[f[2]];
                for (size_t s = 0; s < 15; s++) {
                    const std::string& id = f[3 + s];
                    auto it = out.players.find(id);
                    if (id.empty() || it == out.players.end()) continue;
                    ids[s] = id;
                    if (it->second.number < 0) continue;
                    blitz::LineupPlayer lp;
                    lp.last = it->second.last.substr(0, 15);  // the record's 16 bytes keep their NUL (team_store.cpp's put)
                    lp.number = it->second.number;
                    lp.heavy = (s <= 2 || (s >= 7 && s <= 9)) ? 1 : (s == 3 || s == 14) ? 0 : it->second.heavy;
                    lp.skin = it->second.skin;
                    slots[s] = lp;
                }
            }
            else {
                out.error = "line " + std::to_string(lines) + " has the wrong shape";
                return false;
            }
        }
        if (out.players.empty() || out.lineups.empty()) {
            out.error = "no players or no lineups";
            return false;
        }
        return true;
    }

    void install(Parsed&& p) {
        std::lock_guard lock{mutex};
        players = std::move(p.players);
        player_order = std::move(p.order);
        lineups = std::move(p.lineups);
        week = p.week;
        updated = p.updated;
        loaded = true;
    }

    // The saved pack, once (lazily: the settings ask for the week keys before boot has read anything).
    void ensure_loaded() {
        static std::once_flag once;
        std::call_once(once, [] {
            std::string text;
            if (!pspsys::read_file(dir() + "/pack.txt", text, 4 << 20)) {
                PLOG("rosters", "no rosters/pack.txt: the pack's own rosters until a download");
                return;
            }
            Parsed p;
            if (!parse(text, p)) {
                PERR("rosters", "rosters/pack.txt: %s", p.error.c_str());
                return;
            }
            PLOG("rosters", "saved: %s (made %s), %zu players, %zu lineups", p.week.c_str(), p.updated.c_str(), p.players.size(),
                 p.lineups.size());
            install(std::move(p));
        });
    }

    std::vector<std::string> saved_weeks() {
        ensure_loaded();
        std::vector<std::string> out;
        std::lock_guard lock{mutex};
        for (auto& [label, _] : lineups)
            if (ordinal(label)) out.push_back(label);
        std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return ordinal(a) < ordinal(b); });
        return out;
    }

    std::shared_ptr<const blitz::Lineup> load(const std::string& label) {
        auto it = lineups.find(label);
        return it == lineups.end() ? nullptr : it->second;
    }

    uint32_t choice() {
        return std::get<uint32_t>(recompui::config::get_config("season").get_option_value(opt_lineups));
    }

    // A manifest's hash for a file: "<sha1>" (the weekly job's) or {"sha1": "<sha1>"}.
    std::string hash_of(const json& files, const std::string& path) {
        if (!files.is_object()) return "";
        auto it = files.find(path);
        if (it == files.end()) return "";
        if (it->is_string()) return it->get<std::string>();
        if (it->is_object() && it->contains("sha1") && (*it)["sha1"].is_string()) return (*it)["sha1"].get<std::string>();
        return "";
    }

    // The manifest's "targets": this device takes the file only when "PSP" is listed (or there is no list).
    bool wanted(const json& m, const std::string& path) {
        auto t = m.find("targets");
        if (t == m.end() || !t->is_object()) return true;
        auto e = t->find(path);
        if (e == t->end() || !e->is_object()) return true;
        auto p = e->find("platforms");
        if (p == e->end() || !p->is_array()) return true;
        for (const auto& n : *p)
            if (n.is_string() && n.get<std::string>() == "PSP") return true;
        return false;
    }

    std::string hub_base() {
#if defined(__PSP__)
        return pspremote::net_cfg().hub_base;
#else
        const char* s = getenv("BLITZ_ROSTER_SOURCE");
        std::string h = s ? s : "";
        while (!h.empty() && h.back() == '/') h.pop_back();
        return h;
#endif
    }

    // One check: manifest, then the pack if its hash changed. Returns a line for the log and the command's result.
    std::string check_now() {
        std::string base = hub_base();
        if (base.empty()) return "rosters: no hub (net.cfg hub_base)";
        psphttp::Response r;
        if (!psphttp::get(base + "/manifest.json", r, 15000) || r.status != 200)
            return "rosters: manifest: " + (r.status ? "HTTP " + std::to_string(r.status) : r.error);
        json m = json::parse(r.body, nullptr, false);
        if (!m.is_object() || m.value("format", "") != "blitz-rosters/1") return "rosters: manifest isn't blitz-rosters/1";
        std::string want = hash_of(m.value("files", json::object()), pack_path);
        if (want.empty()) return "rosters: the manifest has no " + std::string(pack_path);
        if (!wanted(m, pack_path)) return "rosters: the manifest's targets leave psp/pack.txt out for the PSP";
        std::string saved;
        json had = pspsys::read_file(dir() + "/manifest.json", saved, 1 << 20) ? json::parse(saved, nullptr, false) : json();
        std::string have = had.is_object() ? hash_of(had.value("files", json::object()), pack_path) : "";
        {
            // Current only if the saved pack also loaded: one that's there but doesn't parse is fetched again.
            std::lock_guard lock{mutex};
            if (have == want && loaded && pspsys::exists(dir() + "/pack.txt"))
                return "rosters: " + week + " is current (" + std::to_string(players.size()) + " players)";
        }
        psphttp::Response p;
        uint64_t t0 = pspsys::now_us();
        if (!psphttp::request("GET", base + "/" + pack_path, std::string(), p, nullptr, 20000, 4 << 20) || p.status != 200)
            return "rosters: pack: " + (p.status ? "HTTP " + std::to_string(p.status) : p.error);
        std::string got = pspsha::sha1_hex(p.body.data(), p.body.size());
        if (got != want) return "rosters: pack SHA-1 " + got + ", the manifest says " + want + ": not used";
        Parsed parsed;
        if (!parse(p.body, parsed)) return "rosters: pack: " + parsed.error;
        pspsys::make_dirs(dir());
        if (!pspsys::write_file(dir() + "/pack.txt", p.body.data(), p.body.size()) ||
            !pspsys::write_file(dir() + "/manifest.json", r.body.data(), r.body.size()))
            return "rosters: couldn't write rosters/pack.txt";
        char line[200];
        snprintf(line, sizeof line, "rosters: %s, %zu players, sha1 ok (%zu bytes in %.1f s; %zu lineups, made %s)",
                 parsed.week.c_str(), parsed.players.size(), p.body.size(), (pspsys::now_us() - t0) / 1e6, parsed.lineups.size(),
                 parsed.updated.c_str());
        std::string latest = parsed.week;
        bool newer;
        {
            std::lock_guard lock{mutex};
            newer = latest != week;
        }
        install(std::move(parsed));
        if (newer) {
            std::lock_guard lock{mutex};
            pending_messages.push_back("Rosters updated: " + display(latest));
        }
        return line;
    }

    void check_thread(void* wait_net) {
#if defined(__PSP__)
        if (wait_net != nullptr && !pspnet::wait_up(60000)) {
            PLOG("rosters", "no network within 60 s: no check this time");
            checking = false;
            return;
        }
#else
        (void)wait_net;
#endif
        std::string line;
        try {
            line = check_now();
        } catch (const std::exception& e) {
            line = std::string("rosters: check failed: ") + e.what();
        }
        checks++;
        PLOG("rosters", "%s", line.c_str());
        checking = false;
    }
}

namespace blitz {
    std::filesystem::path rosters_dir() { return dir(); }

    std::string rosters_status() {
        ensure_loaded();
        std::lock_guard lock{mutex};
        if (!loaded) return "none yet";
        return week + ", updated " + (updated.empty() ? "?" : updated) + (hub_base().empty() ? ", no hub" : ", from the hub");
    }

    std::vector<RosterPlayer> roster_of(const std::string& franchise) {
        ensure_loaded();
        std::lock_guard lock{mutex};
        std::vector<RosterPlayer> out;
        for (const std::string& id : player_order) {
            const RosterPlayer& p = players[id];
            if (p.team == franchise) out.push_back(p);
        }
        return out;
    }

    std::optional<RosterPlayer> roster_player(const std::string& id) {
        ensure_loaded();
        std::lock_guard lock{mutex};
        auto it = players.find(id);
        if (it == players.end()) return std::nullopt;
        return it->second;
    }

    std::array<std::string, 15> lineup_ids(const std::string& franchise) {
        std::array<std::string, 15> out{};
        auto lineup = lineup_now(-1);
        if (lineup) {
            auto it = lineup->ids.find(franchise);
            if (it != lineup->ids.end()) out = it->second;
        }
        return out;
    }

    std::array<std::string, 15> full_ids(const std::string& franchise) {
        ensure_loaded();
        std::array<std::string, 15> out{};
        std::lock_guard lock{mutex};
        auto full = load("full");
        if (full) {
            auto it = full->ids.find(franchise);
            if (it != full->ids.end()) out = it->second;
        }
        return out;
    }

    // At boot (psp_main.cpp, Modern): the saved pack, then one check in the background once the net is up.
    void rosters_check_start() {
        ensure_loaded();
        if (hub_base().empty()) {
            PLOG("rosters", "no hub set (net.cfg hub_base): the saved rosters only");
            return;
        }
        if (checking.exchange(true)) return;
        check_thread_id = pspsys::thread_start("blitz_rosters", check_thread, (void*)1, 64 * 1024, 0x30);
        if (!check_thread_id) checking = false;
    }

    // The `rosters` command: a check now, on the caller's thread (the remote thread). The result line.
    std::string rosters_check_now() {
        ensure_loaded();
        if (checking.exchange(true)) return "rosters: a check is running already";
        std::string line;
        try {
            line = check_now();
        } catch (const std::exception& e) {
            line = std::string("rosters: check failed: ") + e.what();
        }
        checking = false;
        PLOG("rosters", "%s", line.c_str());
        return line;
    }

    // Every stats tick (psp_main.cpp's main loop): a toast for a new week, and the ended check thread's stack back (a PSP
    // thread that returns stays dormant, stack and all, until it is deleted).
    void rosters_update() {
        if (check_thread_id && !checking.load() && pspsys::thread_join(check_thread_id, 0)) check_thread_id = 0;
        std::lock_guard lock{mutex};
        if (pending_messages.empty()) return;
        post_status(pending_messages.front());
        pending_messages.erase(pending_messages.begin());
    }

    // Settings → Season → Lineups: the desktop's option (id, choices, default), with the pack's weeks.
    void add_lineups_option(recomp::config::Config& config) {
        std::vector<recomp::config::ConfigOptionEnumOption> o = {
            {ThisWeek, "week", "This Week"}, {Full, "full", "Full Strength"}, {Packed, "packed", "Built-In"}};
        std::vector<std::string> weeks = saved_weeks();
        std::string pinned;
        std::string saved;
        if (pspsys::read_file(psplog::app_path("season.json"), saved, 1 << 16)) {
            json j = json::parse(saved, nullptr, false);
            if (j.is_object()) pinned = j.value(opt_lineups, "");
        }
        week_keys.clear();
        if (std::find(weeks.begin(), weeks.end(), pinned) != weeks.end()) week_keys.push_back(pinned);
        else if (!weeks.empty()) week_keys.push_back(weeks.back());
        for (size_t i = 0; i < week_keys.size(); i++) o.emplace_back((uint32_t)(FirstWeek + i), week_keys[i], display(week_keys[i]));
        config.add_enum_option(opt_lineups, "Lineups", "Which week's players take the field (rosters/pack.txt).", o, ThisWeek);
    }

    // Settings → Teams → Roster Updates (the desktop's, rosters_live.cpp): the PSP's downloads follow its own net.cfg, so
    // the option only keeps the id the settings tab expects (hidden; 2026-11-13).
    void add_live_rosters_option(recomp::config::Config& config) {
        config.add_bool_option("live_rosters", "Roster Updates", "New lineups download when the app starts.", true, true);
    }

    std::shared_ptr<const Lineup> lineup_for_export(bool full_strength) {
        std::vector<std::string> weeks = saved_weeks();
        std::lock_guard lock{mutex};
        if (full_strength || weeks.empty()) return load("full");
        return load(weeks.back());
    }

    std::shared_ptr<const Lineup> lineup_now(int adv_week) {
        std::vector<std::string> weeks = saved_weeks();
        std::lock_guard lock{mutex};
        uint32_t c = choice();
        if (c == Packed) return nullptr;
        if (c == Full) return load("full");
        if (weeks.empty()) return load("full");
        if (c >= FirstWeek && c - FirstWeek < week_keys.size()) return load(week_keys[c - FirstWeek]);
        if (adv_week < 0) return load(weeks.back());
        int year = ordinal(weeks.back()) / 100;
        int want = year * 100 + adv_week + 1 + (adv_week >= 18 ? (adv_week == 21 ? 2 : 0) : 0);
        std::string best;
        for (const std::string& w : weeks)
            if (ordinal(w) <= want) best = w;
        return load(best.empty() ? "full" : best);
    }
}
#endif
