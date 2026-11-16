// Weekly rosters (docs/design/ROSTER_UPDATES.md): the player file and lineup checkpoints that the
// blitz-rosters job publishes twice a week. At launch the app downloads what's new into
// <app folder>/rosters (manifest.json, players.json, lineups/*.json); on every ovl_b load the chosen
// lineup's players (by ESPN id, looked up in the player file) go into the Blitz 2026 pack's 15 slots
// per team (content_pack_tp.cpp). Without a download it uses what's saved, or the pack as it is.
//
// The source (docs/PLATFORMS.md "Content and updates"): a hub, any base URL serving the files (the rosters post office,
// a Mac on the network): BLITZ_ROSTER_SOURCE=<url>, else the one line in <app folder>/rosters/hub (or the build's
// Resources/rosters/hub), which needs no token, so a phone or console can follow a hub; else, for the 2026 Season pak,
// the season server's own copy (season_pak.h pak_rosters_hub). Or the rosters repo on GitHub, read with a read-only
// token (BLITZ_ROSTER_TOKEN, else rosters/token) at the owner/name in BLITZ_ROSTER_REPO or rosters/repo: no repo is
// named in the source (2026-11-13; a private build gets its files from build/, the Makefile's roster_snapshot).
// BLITZ_ROSTER_SOURCE=<folder> copies from a local folder instead (tests).
//
// Settings → Teams → Roster Updates (live_rosters, 2026-11-13) turns the downloads off: nothing is asked of any server
// then. On by default only in a build that carries its own source; with the pak it's the player's choice (Blitz
// Builder asks), and the season server can switch it off for everyone (its catalog).
//
// The manifest can prune per device: "targets": {path: {"platforms": [...], "min_build": N}} skips a file this
// platform or build doesn't take (blitz_platform.h names), and "app": {"build": N, "platforms": [...], "notes": ""}
// announces a newer app build once ("Update available").
#if BLITZ_MODERN
#include "../main/platform_compat.h"
#include "blitz_host.h"
#include "rosters_live.h"
#include "blitz_platform.h"
#include "season_pak.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "json/json.hpp"
#include "SDL2/SDL.h"
#include "librecomp/game.hpp"
#include "librecomp/config.hpp"
#include "recompui/config.h"
#include "util/file.h"
#include "ultramodern/ultramodern.hpp"

namespace blitz {
    void post_status(const std::string& msg);  // notices.cpp
}

namespace {
    namespace fs = std::filesystem;
    using json = nlohmann::json;

    constexpr const char* opt_lineups = "lineups";
    enum Choice : uint32_t { ThisWeek = 0, Full = 1, Packed = 2, FirstWeek = 10 };

    std::mutex mutex;
    std::vector<std::string> week_keys;      // the Lineups choices 10, 11, ...: saved checkpoints at startup
    std::vector<std::string> pending_messages;  // "Rosters updated: Week 7", "Update available", shown once the game is running
    int generation = 0;                      // bumped by each download; the cache below follows it
    std::map<std::string, std::shared_ptr<const blitz::Lineup>> cache;  // label -> lineup
    int cache_generation = -1;

    fs::path dir() { return recomp::get_config_path() / "rosters"; }

    std::string read_file(const fs::path& p) {
        std::ifstream in{p, std::ios::binary};
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }

    // "2026-w07" -> 202607, "2026-post2" -> 202620 (the playoffs follow week 18), else 0.
    int ordinal(const std::string& label) {
        int year = 0, n = 0;
        if (std::sscanf(label.c_str(), "%d-w%d", &year, &n) == 2) return year * 100 + n;
        if (std::sscanf(label.c_str(), "%d-post%d", &year, &n) == 2) return year * 100 + 18 + n;
        return 0;
    }

    // "Week 7", "Wild Card", ...
    std::string display(const std::string& label) {
        int o = ordinal(label) % 100;
        static const char* post[] = {"Wild Card", "Divisional Round", "Conference Finals", "Pro Bowl", "Super Bowl"};
        if (o > 18 && o <= 23) return post[o - 19];
        return o ? "Week " + std::to_string(o) : label;
    }

    std::vector<std::string> saved_weeks() {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir() / "lineups", ec)) {
            std::string stem = e.path().stem().string();
            if (e.path().extension() == ".json" && ordinal(stem)) out.push_back(stem);
        }
        std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return ordinal(a) < ordinal(b); });
        return out;
    }

    // --- Download -------------------------------------------------------------------------------------------------

    bool safe_path(const std::string& p) {
        return (p == "players.json" || p.rfind("lineups/", 0) == 0) && p.find("..") == std::string::npos && p.find('\\') == std::string::npos;
    }

    struct Source {
        std::string token, folder, url;  // url: a hub's base address, ending in /
        std::string repo;  // with a token: GitHub's owner/name
        bool get(const std::string& path, std::string& body) const {
            if (!url.empty()) {
                int status = 0;
                if (blitz::http_get(url + path, "", "", body, status)) return true;
                fprintf(stderr, "[rosters] hub %s%s: HTTP %d\n", url.c_str(), path.c_str(), status);
                return false;
            }
            if (!folder.empty()) {
                std::ifstream in{fs::path{folder} / path, std::ios::binary};
                if (!in) return false;
                std::stringstream s;
                s << in.rdbuf();
                body = s.str();
                return true;
            }
            int status = 0;
            if (blitz::http_get("https://api.github.com/repos/" + repo + "/contents/" + path, token, "application/vnd.github.raw", body, status)) return true;
            fprintf(stderr, "[rosters] %s: HTTP %d\n", path.c_str(), status);
            return false;
        }
    };

    void write_atomic(const fs::path& p, const std::string& body) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        fs::path tmp = p;
        tmp += ".part";
        {
            std::ofstream out{tmp, std::ios::binary};
            out << body;
            out.flush();
            if (!out) {  // a full disk: keep the good file, not a cut-off one
                fprintf(stderr, "[rosters] couldn't write %s\n", p.string().c_str());
                out.close();
                fs::remove(tmp, ec);
                return;
            }
        }
        fs::rename(tmp, p, ec);
    }

    bool is_url(const std::string& s) { return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0; }

    // Whether this device takes a file (the manifest's optional "targets").
    bool wanted(const json& remote, const std::string& path) {
        auto targets = remote.find("targets");
        if (targets == remote.end() || !targets->is_object()) return true;
        auto t = targets->find(path);
        if (t == targets->end() || !t->is_object()) return true;
        if (auto p = t->find("platforms"); p != t->end() && p->is_array()) {
            bool listed = false;
            for (const auto& name : *p) listed = listed || (name.is_string() && name.get<std::string>() == blitz::platform::name);
            if (!listed) return false;
        }
        if (auto b = t->find("min_build"); b != t->end() && b->is_number() && b->get<int>() > std::atoi(BLITZ_BUILD)) return false;
        return true;
    }

    // A newer app build announced in the manifest ("app"), once per build.
    void check_app_update(const json& remote) {
        auto app = remote.find("app");
        if (app == remote.end() || !app->is_object()) return;
        auto b = app->find("build");
        if (b == app->end() || !b->is_number()) return;
        int build = b->get<int>();
        if (build <= std::atoi(BLITZ_BUILD)) return;
        if (auto p = app->find("platforms"); p != app->end() && p->is_array()) {
            bool listed = false;
            for (const auto& name : *p) listed = listed || (name.is_string() && name.get<std::string>() == blitz::platform::name);
            if (!listed) return;
        }
        if (std::atoi(read_file(dir() / "app_notified").c_str()) >= build) return;
        write_atomic(dir() / "app_notified", std::to_string(build));
        fprintf(stderr, "[rosters] app build %d is available (this is %s)\n", build, BLITZ_BUILD);
        std::lock_guard lock{mutex};
        pending_messages.push_back("Update available: build " + std::to_string(build));
    }

    // A one-line setting file (rosters/hub, rosters/token) without its spaces and line ends: the app folder's, else
    // the one the app was built with (Resources/rosters; the Makefile's roster_snapshot), so every device a build goes
    // to downloads with no setup there.
    std::string setting_file(const char* name) {
        std::string v = read_file(dir() / name);
        if (v.empty()) v = read_file(recompui::file::get_program_path() / "rosters" / name);
        v.erase(std::remove_if(v.begin(), v.end(), [](char c) { return std::isspace((unsigned char)c); }), v.end());
        return v;
    }

    void check_now() {
        Source src;
        const char* forced = getenv("BLITZ_ROSTER_SOURCE");  // dev and tests: always, whatever the setting
        if (!(forced && *forced) && !blitz::live_rosters_on()) {
            fprintf(stderr, "[rosters] Roster Updates is off: using the saved lineups\n");
            return;
        }
        std::string hub = setting_file("hub");
        if (hub.empty()) hub = blitz::pak_rosters_hub();
        if (forced && *forced) hub = forced;
        if (is_url(hub)) src.url = hub.back() == '/' ? hub : hub + "/";
        else src.folder = hub;
        if (const char* t = getenv("BLITZ_ROSTER_TOKEN"); t && *t) src.token = t;
        if (src.token.empty()) src.token = setting_file("token");
        if (const char* r = getenv("BLITZ_ROSTER_REPO"); r && *r) src.repo = r;
        if (src.repo.empty()) src.repo = setting_file("repo");
        if (src.repo.empty()) src.token.clear();  // a token without a repo to read leads nowhere
        if (src.folder.empty() && src.url.empty() && src.token.empty()) {
            fprintf(stderr, "[rosters] no source (rosters/hub, or a token and rosters/repo): using the saved lineups\n");
            return;
        }
        std::string body;
        json remote, local;
        try {
            if (!src.get("manifest.json", body)) return;
            remote = json::parse(body);
            if (remote.value("format", "") != "blitz-rosters/1") {
                fprintf(stderr, "[rosters] unknown format '%s'\n", remote.value("format", "").c_str());
                return;
            }
            local = json::parse(read_file(dir() / "manifest.json"), nullptr, false);
        }
        catch (const std::exception& e) {
            fprintf(stderr, "[rosters] manifest: %s\n", e.what());
            return;
        }
        const json no_files = json::object();
        const json& had = local.is_object() && local.contains("files") ? local["files"] : no_files;
        // Everything new is fetched first, then written together under the lock, so a game loading meanwhile
        // never sees a lineup without its players.
        std::vector<std::pair<std::string, std::string>> fetched;
        check_app_update(remote);
        for (auto& [path, hash] : remote["files"].items()) {
            if (!safe_path(path) || !wanted(remote, path)) continue;
            if (had.contains(path) && had[path] == hash && blitz::file_exists(dir() / path)) continue;
            std::string file;
            if (!src.get(path, file)) return;  // try again next launch (nothing's written)
            try {
                (void)json::parse(file);
            }
            catch (const std::exception&) {
                fprintf(stderr, "[rosters] %s isn't JSON\n", path.c_str());
                return;
            }
            fetched.emplace_back(path, std::move(file));
        }
        std::string latest = remote.value("latest", "");
        bool newer = !latest.empty() && (!local.is_object() || local.value("latest", "") != latest);
        std::lock_guard lock{mutex};
        for (const auto& [path, file] : fetched) write_atomic(dir() / path, file);
        write_atomic(dir() / "manifest.json", body);
        fprintf(stderr, "[rosters] %zu file(s) downloaded; latest %s, made %s\n", fetched.size(), latest.c_str(),
                remote.value("updated", "?").c_str());
        if (!fetched.empty()) generation++;
        if (newer) pending_messages.push_back("Rosters updated: " + display(latest));
    }

    std::atomic<bool> checking{false};
    std::chrono::system_clock::time_point last_check{};

    // The download thread: nothing may escape it (an exception off the main thread ends the app). A manifest of
    // the wrong shape, say.
    void check() {
        try {
            check_now();
        }
        catch (const std::exception& e) {
            fprintf(stderr, "[rosters] check: %s\n", e.what());
        }
        checking = false;
    }

    // One check at a time, in the background.
    void start_check() {
        if (checking.exchange(true)) return;
        last_check = std::chrono::system_clock::now();
        std::thread(check).detach();
    }

    // --- Lineups ----------------------------------------------------------------------------------------------------

    // The player file's "players" (parsed once per download). Call with the mutex held.
    const json& players_json() {
        static json players = json::object();
        static int players_generation = -1;
        if (players_generation != generation) {
            players_generation = generation;
            try {
                players = json::parse(read_file(dir() / "players.json")).at("players");
            }
            catch (const std::exception& e) {
                fprintf(stderr, "[rosters] players.json: %s\n", e.what());
                players = json::object();
            }
        }
        return players;
    }

    // A player's field, or the fallback when it's missing or another type (downloaded files have no schema, and a
    // const json's operator[] on a missing key is undefined behaviour, not an exception).
    std::string str_at(const json& p, const char* key, const std::string& fallback = "") {
        auto it = p.find(key);
        return it != p.end() && it->is_string() ? it->get<std::string>() : fallback;
    }
    int int_at(const json& p, const char* key, int fallback) {
        auto it = p.find(key);
        return it != p.end() && it->is_number() ? it->get<int>() : fallback;
    }

    blitz::RosterPlayer roster_player_of(const std::string& id, const json& p) {
        blitz::RosterPlayer r;
        r.id = id;
        r.first = str_at(p, "first");
        r.last = str_at(p, "name", str_at(p, "last"));
        r.team = str_at(p, "team");
        r.number = int_at(p, "number", -1);
        r.pos = str_at(p, "pos");
        r.depth = str_at(p, "depth");
        r.status = str_at(p, "status");
        r.skin = int_at(p, "skin", -1);
        r.heavy = int_at(p, "heavy", 0);
        return r;
    }

    std::shared_ptr<const blitz::Lineup> load(const std::string& label) {
        if (cache_generation != generation) {
            cache.clear();
            cache_generation = generation;
        }
        if (auto it = cache.find(label); it != cache.end()) return it->second;
        std::shared_ptr<blitz::Lineup> out;
        try {
            const json& players = players_json();
            json lineup = json::parse(read_file(dir() / "lineups" / (label + ".json")));
            out = std::make_shared<blitz::Lineup>();
            out->label = label;
            for (auto& [franchise, ids] : lineup["teams"].items()) {
                auto& slots = out->teams[franchise];
                auto& slot_ids = out->ids[franchise];
                for (size_t s = 0; s < 15 && s < ids.size(); s++) {
                    if (!ids[s].is_string() || !players.contains(ids[s].get<std::string>())) continue;
                    slot_ids[s] = ids[s].get<std::string>();
                    const json& p = players[ids[s].get<std::string>()];
                    if (int_at(p, "number", -1) < 0) continue;
                    blitz::LineupPlayer lp;
                    lp.last = str_at(p, "name", str_at(p, "last"));
                    lp.number = int_at(p, "number", -1);
                    lp.heavy = (s <= 2 || (s >= 7 && s <= 9)) ? 1 : (s == 3 || s == 14) ? 0 : int_at(p, "heavy", 0);
                    lp.skin = int_at(p, "skin", -1);
                    slots[s] = lp;
                }
            }
        }
        catch (const std::exception& e) {
            fprintf(stderr, "[rosters] lineup %s: %s\n", label.c_str(), e.what());
            out = nullptr;
        }
        cache[label] = out;
        return out;
    }

    uint32_t choice() {
        return std::get<uint32_t>(recompui::config::get_config("season").get_option_value(opt_lineups));
    }
}

namespace blitz {
    std::filesystem::path rosters_dir() { return dir(); }

    // Which roster day this device has, and where it follows (Copy Debug Info, Save Logs; config.cpp): "2026-w08,
    // updated 2026-10-28 05:01Z, from the post office".
    std::string rosters_status() {
        json m = json::parse(read_file(dir() / "manifest.json"), nullptr, false);
        std::string latest = m.is_object() ? m.value("latest", "") : "";
        std::string updated = m.is_object() ? m.value("updated", "") : "";
        std::string hub = setting_file("hub");
        if (hub.empty()) hub = blitz::pak_rosters_hub();
        if (const char* f = getenv("BLITZ_ROSTER_SOURCE"); f && *f) hub = f;
        std::string from = !hub.empty() ? (hub.find("workers.dev") != std::string::npos ? "the post office" : "a hub")
                         : !setting_file("token").empty() ? "GitHub" : "the app's own snapshot (no source set)";
        if (!blitz::live_rosters_on()) from += ", Roster Updates off";
        if (latest.empty()) return "none yet, from " + from;
        return latest + ", updated " + (updated.empty() ? "?" : updated) + ", from " + from;
    }

    std::vector<RosterPlayer> roster_of(const std::string& franchise) {
        std::lock_guard lock{mutex};
        std::vector<RosterPlayer> out;
        for (auto& [id, p] : players_json().items()) {
            if (!p.is_object() || p.value("team", json()) != franchise) continue;
            std::string status = p.value("status", "");
            if (status == "practice" || status == "free") continue;
            out.push_back(roster_player_of(id, p));
        }
        return out;
    }

    std::optional<RosterPlayer> roster_player(const std::string& id) {
        std::lock_guard lock{mutex};
        const json& players = players_json();
        auto it = players.find(id);
        if (it == players.end() || !it->is_object()) return std::nullopt;
        return roster_player_of(id, *it);
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
        std::array<std::string, 15> out{};
        std::shared_ptr<const Lineup> full;
        {
            std::lock_guard lock{mutex};
            full = load("full");
        }
        if (full) {
            auto it = full->ids.find(franchise);
            if (it != full->ids.end()) out = it->second;
        }
        return out;
    }

    // The app's own snapshot (Resources/rosters, the weekly job's latest when the app was built; or the 2026 Season
    // pak's rosters/, the newest first) into <app folder>/rosters when there's none there yet, or when the snapshot was
    // made later than what's there (a pak newer than the last download): no token, hub or network needed, so Pick Your
    // Crew and the lineups work on a console or an offline phone. At launch, and right after a pak goes in.
    void rosters_seed() {
        std::error_code ec;
        auto usable = [&](const fs::path& p) { return blitz::file_exists(p) && fs::file_size(p, ec) > 2 && !ec; };
        auto updated = [&](const fs::path& p) {
            json m = json::parse(read_file(p / "manifest.json"), nullptr, false);
            return m.is_object() && m.contains("updated") && m["updated"].is_string() ? m["updated"].get<std::string>() : std::string{};
        };
        for (const fs::path& root : blitz::content_dirs_newest_first()) {
            fs::path snapshot = root / "rosters";
            if (!usable(snapshot / "players.json")) continue;
            bool have = usable(dir() / "players.json");
            if (have && !(updated(snapshot) > updated(dir()))) return;  // what's here is as new (ISO times compare as text)
            std::lock_guard lock{mutex};
            fs::create_directories(dir() / "lineups", ec);
            for (const char* f : {"manifest.json", "players.json"}) fs::copy_file(snapshot / f, dir() / f, fs::copy_options::overwrite_existing, ec);
            for (fs::directory_iterator it{snapshot / "lineups", ec}, end; !ec && it != end; it.increment(ec)) {
                fs::copy_file(it->path(), dir() / "lineups" / it->path().filename(), fs::copy_options::overwrite_existing, ec);
            }
            fprintf(stderr, "[rosters] %s the snapshot in %s (%s)\n", have ? "updated from" : "started from",
                    root.string().c_str(), ec ? ec.message().c_str() : "ok");
            generation++;
            return;
        }
    }

    // A source the build itself carries (Resources/rosters/hub or token: a private build): Roster Updates is on by
    // default there, as it always was.
    bool rosters_build_source() {
        for (const char* f : {"hub", "token"}) {
            std::string v = read_file(recompui::file::get_program_path() / "rosters" / f);
            if (v.find_first_not_of(" \t\r\n") != std::string::npos) return true;
        }
        return false;
    }

    // At launch (main.cpp): the snapshot if this device needs it, then new rosters download in the background (when
    // Roster Updates is on) and replace it.
    void rosters_check_start() {
        rosters_seed();
        start_check();
    }

    // Blitz Builder's "Keep rosters up to date?" yes, and Settings → Teams → Roster Updates turned on: a check now.
    void rosters_check_again() { start_check(); }

    // Every UI tick (main.cpp): "Rosters updated" and "Update available" toasts, once the game is running, one at a
    // time; and another check every few hours, for a phone or console that's left running (or suspended) for days.
    // By the wall clock, which keeps going while a phone sleeps.
    void rosters_update() {
        if (last_check != std::chrono::system_clock::time_point{} && std::chrono::system_clock::now() - last_check > std::chrono::hours(3)) {
            start_check();  // (the first check is rosters_check_start's, at launch)
        }
        static uint32_t last = 0;
        uint32_t now = SDL_GetTicks();
        std::lock_guard lock{mutex};
        if (!pending_messages.empty() && ultramodern::is_game_started() && (last == 0 || now - last > 3500)) {
            post_status(pending_messages.front());
            pending_messages.erase(pending_messages.begin());
            last = now;
        }
    }

    // Settings → Season → Lineups (season_settings.cpp).
    void add_lineups_option(recomp::config::Config& config) {
        std::vector<recomp::config::ConfigOptionEnumOption> o = {
            {ThisWeek, "week", "This Week"}, {Full, "full", "Full Strength"}, {Packed, "packed", "Built-In"}};
        // One week button (a row of 18 won't fit): the latest, or the week the player pinned there before a
        // newer one arrived (read from season.json, since the option doesn't exist yet).
        std::vector<std::string> weeks = saved_weeks();
        std::string pinned;
        try {
            json saved = json::parse(read_file(recomp::get_config_path() / "season.json"));
            pinned = saved.value(opt_lineups, "");
        }
        catch (...) {
        }
        week_keys.clear();
        if (std::find(weeks.begin(), weeks.end(), pinned) != weeks.end()) week_keys.push_back(pinned);
        else if (!weeks.empty()) week_keys.push_back(weeks.back());
        for (size_t i = 0; i < week_keys.size(); i++) o.emplace_back((uint32_t)(FirstWeek + i), week_keys[i], display(week_keys[i]));
        config.add_enum_option(
            opt_lineups,
            "Lineups",
            "Which week's players take the field, with the 2026 Season's rosters. With <b>Keep Rosters and Uniforms Up to Date</b> on, new "
            "lineups download when the app starts: on Thursdays and Saturdays during the season."
            "<br /><br />"
            "<b>This Week</b>: who's starting this week, injuries and all. In an Advanced season, each week plays "
            "that week's lineup."
            "<br /><br />"
            "<b>Full Strength</b>: every team at its best, as if everyone were healthy."
            "<br /><br />"
            "<b>Built-In</b>: the rosters the 2026 Season came with."
            "<br /><br />"
            "<b>Week N</b>: keeps that week's lineup after newer ones arrive."
            "<br /><br />"
            "Takes effect from the next game.",
            o, ThisWeek);
    }

    // Settings → Teams → Roster Updates (season_settings.cpp), after Lineups.
    void add_live_rosters_option(recomp::config::Config& config) {
        // (Was Roster Updates: since 2026-11-15 a newer 2026 Season pak follows it too, season_pak.cpp.)
        config.add_bool_option(
            "live_rosters",
            "Keep Rosters and Uniforms Up to Date",
            "New lineups download when the app starts, on Thursdays and Saturdays during the season (and every few "
            "hours while it runs). A newer 2026 Season (uniforms, team art) downloads too, and goes in the next time "
            "the app starts."
            "<br /><br />"
            "<b>Off</b>: nothing is downloaded; the rosters and uniforms stay as they are (the 2026 Season's, or the "
            "last update).",
            rosters_build_source());
        config.add_option_change_callback("live_rosters", [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant,
                                                             recomp::config::OptionChangeContext ctx) {
            if (ctx == recomp::config::OptionChangeContext::Permanent && std::get<bool>(cur)) {
                recompui::config::get_config("season").save_config();  // the download thread reads season.json
                start_check();
            }
        });
    }

    std::shared_ptr<const Lineup> lineup_for_export(bool full_strength) {
        std::lock_guard lock{mutex};
        std::vector<std::string> weeks = saved_weeks();
        if (full_strength || weeks.empty()) return load("full");
        return load(weeks.back());
    }

    std::shared_ptr<const Lineup> lineup_now(int adv_week) {
        std::lock_guard lock{mutex};
        uint32_t c = choice();
        if (c == Packed) return nullptr;
        if (c == Full) return load("full");
        std::vector<std::string> weeks = saved_weeks();
        if (weeks.empty()) return load("full");  // nothing downloaded yet: full strength if there's that, else the pack
        if (c >= FirstWeek && c - FirstWeek < week_keys.size()) return load(week_keys[c - FirstWeek]);
        if (adv_week < 0) return load(weeks.back());
        // An Advanced season's week: that NFL week's checkpoint, or the latest before it; before the first
        // one, full strength.
        int year = ordinal(weeks.back()) / 100;
        int want = year * 100 + adv_week + 1 + (adv_week >= 18 ? (adv_week == 21 ? 2 : 0) : 0);  // the Super Bowl is post5
        std::string best;
        for (const std::string& w : weeks) {
            if (ordinal(w) <= want) best = w;
        }
        return load(best.empty() ? "full" : best);
    }
}
#endif
