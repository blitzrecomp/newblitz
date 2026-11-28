// Paks: installing, removing and downloading them, and the server's catalog (season_pak.h, docs/design/SEASON_PAK.md).
// The 2026 Season pak came first (2026-11-13); since 1.0.6 every pak the catalog lists (the season, the Pac-12 and CFL
// league paks, any later one) installs and updates itself while content updates are on, and Add-Ons' Check syncs now.
//
// A pak is a zip (.nbpak) with pak.json at its root and a tree shaped like the app's Resources (packs/, uniforms/, art/,
// announcer/; the season's also rosters/, houston/, probowl/). It unpacks into <app folder>/paks/<id>/, which the loaders
// read between Resources and the app folder (content_dirs.cpp). A copy of the season's zip stays beside it
// (paks/season2026.nbpak) for Blitz Builder to send to a phone or a console with the ROM. paks/<id>.origin.json says where
// an installed pak came from ("catalog": the server's, kept up to date; "file": the player's own, from Add…, left alone)
// and its SHA-256; paks/removed.json lists the paks the player removed (no automatic download brings those back).
//
// Checks, all before the installed pak is touched: the whole file's SHA-256 against the catalog's (a download); every
// entry's name (no absolute paths, "..", backslashes or drive letters, and only the folders above); the count and the
// unpacked size; pak.json (format, id, the oldest build that reads it); every file against pak.json's SHA-256 list (a
// file put in by hand has only that); every roster pack parses. Then the old folder is swapped out for the new in two
// renames. miniz checks each entry's CRC-32 as it unpacks.
//
// The server (the newblitz Worker, tools/rosters_hub_worker's public /paks/ routes): catalog.json names each pak's file,
// size and SHA-256, and carries the switches every installed copy follows: removing a pak from the catalog stops new
// installs (the kill switch), "headshots": false stops the crew cards' ESPN photos, "live_rosters": false the weekly
// downloads. The address is in Resources/paks/server, which a release build writes from a file outside the source (the
// Makefile's build/pak_server), never from the code.
#include "season_pak.h"
#include "rosters_live.h"
#include "../common/sha256.h"
#include "../main/platform_compat.h"
#include "blitz_host.h"
#include "blitz_platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include "json/json.hpp"
#include "miniz.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "recompui/config.h"
#include "util/file.h"
#include "blitz_soft_reset.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace blitz {
    void post_notice(const std::string& msg);  // notices.cpp
    void post_status(const std::string& msg);  // notices.cpp: a toast once the game is up
#if BLITZ_MODERN
    void league_pak_arrived(const std::string& pak_id);  // league_select.cpp: "Play CFL 2026 now?"
    void league_notice(const std::string& header, const std::string& msg);  // season_settings.cpp: our modal
#endif
}

namespace {
    constexpr size_t max_pak = 64u << 20;        // a 2026 pak is about 2 MB
    constexpr size_t max_unpacked = 128u << 20;
    constexpr mz_uint max_entries = 8000;        // about 1,500 today
    // Every pak's folders; the season's own besides (the roster snapshot, Houston, Pro Bowl: one of each in the game).
    const std::set<std::string> top_folders = {"packs", "uniforms", "art", "announcer"};
    const std::set<std::string> season_folders = {"rosters", "houston", "probowl"};
    const std::set<std::string> top_files = {"pak.json", "LICENSE-CONTENT.txt"};

    std::mutex catalog_mutex;  // the cached catalog (paks/catalog.json)
    std::mutex install_mutex;  // one install or removal at a time
    std::mutex marks_mutex;    // paks/removed.json and the origin files
    std::atomic<bool> syncing{false};
    std::mutex progress_mutex;
    blitz::PakSyncProgress progress_now;
    std::set<std::string> wifi_ids;  // left for Wi-Fi by the last automatic sync (progress_mutex)

    std::string read_file(const fs::path& p) {
        std::ifstream in{p, std::ios::binary};
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }

    std::string trimmed(std::string v) {
        v.erase(std::remove_if(v.begin(), v.end(), [](char c) { return std::isspace((unsigned char)c); }), v.end());
        return v;
    }

    bool write_file(const fs::path& p, const std::string& data) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        fs::path tmp = p;
        tmp += ".part";
        {
            std::ofstream out{tmp, std::ios::binary};
            out.write(data.data(), (std::streamsize)data.size());
            out.flush();
            if (!out) {
                out.close();
                fs::remove(tmp, ec);
                return false;
            }
        }
        fs::rename(tmp, p, ec);
        return !ec;
    }

    std::string random_tag() {
        std::random_device rd;
        char buf[16];
        snprintf(buf, sizeof(buf), "%08x", (unsigned)rd());
        return buf;
    }

    std::string sha_of(const std::string& data) { return blitzsha::sha256_hex(data.data(), data.size()); }

    fs::path catalog_file() { return blitz::paks_dir() / "catalog.json"; }
    fs::path copy_file(const std::string& id) { return blitz::paks_dir() / (id + ".nbpak"); }
    fs::path origin_file(const std::string& id) { return blitz::paks_dir() / (id + ".origin.json"); }
    fs::path update_file(const std::string& id) { return blitz::paks_dir() / (id + ".update.nbpak"); }
    fs::path update_sha_file(const std::string& id) { return blitz::paks_dir() / (id + ".update.sha256"); }
    fs::path removed_file() { return blitz::paks_dir() / "removed.json"; }
    bool is_season(const std::string& id) { return id == blitz::season_pak_id; }

    // ---- Where each installed pak came from, and the ones the player removed ----------------------------------------

    struct Origin {
        std::string from = "catalog";  // "catalog" (the server's: kept up to date) or "file" (the player's own: left alone)
        std::string sha256;            // the installed zip's, "" when not known
    };

    // A pak installed before 1.0.6 has no origin file: the server's (1.0.5 updated the season whatever its source), with the
    // SHA-256 of the season's kept copy.
    Origin read_origin(const std::string& id) {
        std::lock_guard lock{marks_mutex};
        Origin o;
        json j = json::parse(read_file(origin_file(id)), nullptr, false);
        if (j.is_object()) {
            o.from = j.value("from", "catalog");
            o.sha256 = j.value("sha256", "");
            return o;
        }
        if (blitz::file_exists(copy_file(id))) o.sha256 = sha_of(read_file(copy_file(id)));
        return o;
    }

    void write_origin(const std::string& id, const std::string& from, const std::string& sha) {
        std::lock_guard lock{marks_mutex};
        write_file(origin_file(id), json{{"from", from}, {"sha256", sha}}.dump(1) + "\n");
    }

    std::set<std::string> removed_ids() {
        std::lock_guard lock{marks_mutex};
        std::set<std::string> out;
        json j = json::parse(read_file(removed_file()), nullptr, false);
        if (j.is_array()) {
            for (const json& v : j) if (v.is_string()) out.insert(v.get<std::string>());
        }
        return out;
    }

    void mark_removed(const std::string& id, bool removed) {
        std::set<std::string> ids = removed_ids();
        if (removed == (ids.count(id) > 0)) return;
        if (removed) ids.insert(id);
        else ids.erase(id);
        std::lock_guard lock{marks_mutex};
        std::error_code ec;
        if (ids.empty()) fs::remove(removed_file(), ec);
        else write_file(removed_file(), json(std::vector<std::string>(ids.begin(), ids.end())).dump() + "\n");
    }

    // ---- The catalog ----------------------------------------------------------------------------------------------

    // The last catalog read (paks/catalog.json), or null.
    json cached_catalog() {
        std::lock_guard lock{catalog_mutex};
        json c = json::parse(read_file(catalog_file()), nullptr, false);
        return c.is_object() ? c : json();
    }

    // A pak's entry in a catalog, or null (withdrawn).
    json catalog_entry(const json& catalog, const std::string& id) {
        if (!catalog.is_object() || !catalog.contains("paks") || !catalog["paks"].is_object()) return json();
        auto it = catalog["paks"].find(id);
        if (it == catalog["paks"].end() || !it->is_object() || !it->value("available", true)) return json();
        return *it;
    }
    json catalog_entry(const json& catalog) { return catalog_entry(catalog, blitz::season_pak_id); }

    // Every pak the catalog offers (a valid id, an object, available), the season first, then by id.
    std::vector<std::pair<std::string, json>> catalog_entries(const json& catalog) {
        std::vector<std::pair<std::string, json>> out;
        if (!catalog.is_object() || !catalog.contains("paks") || !catalog["paks"].is_object()) return out;
        for (const auto& [id, e] : catalog["paks"].items()) {
            if (blitz::valid_pak_id(id) && e.is_object() && e.value("available", true)) out.emplace_back(id, e);
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (is_season(a.first) != is_season(b.first)) return is_season(a.first);
            return a.first < b.first;
        });
        return out;
    }

    // A switch in the cached catalog's entry: off when the entry says so, when the pak is withdrawn, or when no
    // catalog has been read yet (a pak installed from a file, before its first catalog check).
    bool catalog_switch(const char* key) {
        json e = catalog_entry(cached_catalog());
        return e.is_object() && e.value(key, true);
    }

    // ---- Unpacking and installing ---------------------------------------------------------------------------------

    // An entry's name: relative, forward slashes, no "..", inside the pak's folders (the season's own besides, for it).
    bool safe_name(const std::string& n, bool season) {
        if (n.empty() || n[0] == '/' || n.find('\\') != std::string::npos || n.find(':') != std::string::npos) return false;
        if (n.rfind("__MACOSX", 0) == 0) return false;
        size_t start = 0;
        while (start <= n.size()) {
            size_t end = n.find('/', start);
            std::string part = n.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (part == ".." || part == "." || (part.empty() && end != std::string::npos && end + 1 != n.size())) return false;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        size_t slash = n.find('/');
        if (slash == std::string::npos) return top_files.count(n) > 0;
        std::string top = n.substr(0, slash);
        return top_folders.count(top) > 0 || (season && season_folders.count(top) > 0);
    }

    struct Zip {
        mz_zip_archive zip{};
        bool open = false;
        ~Zip() {
            if (open) mz_zip_reader_end(&zip);
        }
    };

    // The pak.json of a zip in memory, or null: its id decides which folders it may have, before anything is unpacked.
    json peek_manifest(const std::string& data) {
        Zip z;
        if (!mz_zip_reader_init_mem(&z.zip, data.data(), data.size(), 0)) return json();
        z.open = true;
        int i = mz_zip_reader_locate_file(&z.zip, "pak.json", nullptr, 0);
        mz_zip_archive_file_stat st;
        if (i < 0 || !mz_zip_reader_file_stat(&z.zip, (mz_uint)i, &st) || st.m_uncomp_size > (1u << 20)) return json();
        std::string body((size_t)st.m_uncomp_size, '\0');
        if (!body.empty() && !mz_zip_reader_extract_to_mem(&z.zip, (mz_uint)i, body.data(), body.size(), 0)) return json();
        json m = json::parse(body, nullptr, false);
        return m.is_object() ? m : json();
    }

    // Unpacks and checks a pak (in memory) into `to`. id: the pak's (from its pak.json); expect: the id it must be ("" for
    // any). Every failure says why.
    bool unpack(const std::string& data, const fs::path& to, const std::string& expect, std::string& id, std::string& error) {
        json peek = peek_manifest(data);
        if (!peek.is_object()) {
            error = "it isn't a New Blitz pak (no pak.json)";
            return false;
        }
        id = peek.value("id", "");
        if (peek.value("format", "") != "blitz-pak/1" || !blitz::valid_pak_id(id)) {
            error = "it isn't a New Blitz pak";
            return false;
        }
        if (!expect.empty() && id != expect) {
            error = is_season(expect) ? "it isn't the 2026 Season pak" : "it's the " + id + " pak, not " + expect;
            return false;
        }
        const bool season = is_season(id);
        Zip z;
        if (!mz_zip_reader_init_mem(&z.zip, data.data(), data.size(), 0)) {
            error = "it isn't a zip";
            return false;
        }
        z.open = true;
        mz_uint n = mz_zip_reader_get_num_files(&z.zip);
        if (n == 0 || n > max_entries) {
            error = "it has " + std::to_string(n) + " entries";
            return false;
        }
        size_t total = 0;
        std::map<std::string, std::string> hashes;  // unpacked name -> SHA-256
        std::error_code ec;
        for (mz_uint i = 0; i < n; i++) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z.zip, i, &st)) {
                error = "an entry can't be read";
                return false;
            }
            std::string name = st.m_filename;
            if (mz_zip_reader_is_file_a_directory(&z.zip, i)) {
                std::string dir = name;
                while (!dir.empty() && dir.back() == '/') dir.pop_back();
                if (!safe_name(dir + "/x", season)) {
                    error = "it has a folder outside the pak's (" + name + ")";
                    return false;
                }
                continue;
            }
            if (!safe_name(name, season)) {
                error = "it has a file outside the pak's folders (" + name + ")";
                return false;
            }
            total += (size_t)st.m_uncomp_size;
            if (st.m_uncomp_size > max_unpacked || total > max_unpacked) {
                error = "it unpacks to more than " + std::to_string(max_unpacked >> 20) + " MB";
                return false;
            }
            std::string body((size_t)st.m_uncomp_size, '\0');
            if (!body.empty() && !mz_zip_reader_extract_to_mem(&z.zip, i, body.data(), body.size(), 0)) {
                error = "the file " + name + " is damaged";
                return false;
            }
            hashes[name] = sha_of(body);
            fs::path out = to / fs::path(name);
            fs::create_directories(out.parent_path(), ec);
            std::ofstream f{out, std::ios::binary};
            f.write(body.data(), (std::streamsize)body.size());
            f.flush();
            if (!f) {
                error = "the disk is full (or can't be written)";
                return false;
            }
        }

        json manifest = json::parse(read_file(to / "pak.json"), nullptr, false);
        if (!manifest.is_object() || manifest.value("id", "") != id || manifest.value("format", "") != "blitz-pak/1") {
            error = "its pak.json can't be read";
            return false;
        }
        if (manifest.value("min_build", 0) > std::atoi(BLITZ_BUILD)) {
            error = "it needs a newer New Blitz (build " + std::to_string(manifest.value("min_build", 0)) + ")";
            return false;
        }
        // Every file is listed with its SHA-256, and nothing unlisted came along.
        if (!manifest.contains("files") || !manifest["files"].is_object()) {
            error = "its pak.json has no file list";
            return false;
        }
        const json& files = manifest["files"];
        for (const auto& [name, hash] : hashes) {
            if (name == "pak.json") continue;
            auto it = files.find(name);
            if (it == files.end() || !it->is_string() || it->get<std::string>() != hash) {
                error = it == files.end() ? "it has a file pak.json doesn't list (" + name + ")" : "the file " + name + " isn't the one pak.json lists";
                return false;
            }
        }
        for (const auto& [name, _] : files.items()) {
            if (!hashes.count(name)) {
                error = "the file " + name + " is missing";
                return false;
            }
        }
        // Every roster pack in it parses as one, named for its folder; the season's must have the 2026 pack.
        for (const auto& [name, _] : hashes) {
            if (name.rfind("packs/", 0) != 0 || name.size() < 16 || name.compare(name.size() - 10, 10, "/pack.json") != 0) continue;
            std::string folder = name.substr(6, name.size() - 16);
            if (folder.find('/') != std::string::npos) continue;  // a pack's own files below it
            json pack = json::parse(read_file(to / name), nullptr, false);
            if (!pack.is_object() || pack.value("format", "") != "blitz-pack/2" || pack.value("id", "") != folder) {
                error = "its roster pack " + folder + " can't be read";
                return false;
            }
        }
        if (season && !hashes.count("packs/blitz2026/pack.json")) {
            error = "its roster pack can't be read";
            return false;
        }
        return true;
    }

    // from: "catalog" (the server's; also a pak sent from a computer's Blitz Builder) or "file" (the player's own, Add…).
    bool install(const std::string& data, const std::string& want_sha256, const std::string& expect, const std::string& from,
                 std::string& id, std::string& error) {
        if (data.size() > max_pak) {
            error = "the file is too big for a pak";
            return false;
        }
        std::string got = sha_of(data);
        if (!want_sha256.empty() && got != want_sha256) {
            error = "the download doesn't match the server's checksum";
            fprintf(stderr, "[paks] SHA-256 %s, the catalog says %s\n", got.c_str(), want_sha256.c_str());
            return false;
        }
        std::lock_guard lock{install_mutex};
        std::error_code ec;
        fs::path paks = blitz::paks_dir();
        fs::create_directories(paks, ec);
        if (auto space = fs::space(paks, ec); !ec && space.available < 4 * data.size() + (16u << 20)) {
            error = "there isn't enough free space";
            return false;
        }
        fs::path incoming = paks / (".incoming-" + random_tag());
        fs::remove_all(incoming, ec);
        if (!unpack(data, incoming, expect, id, error)) {
            fs::remove_all(incoming, ec);
            return false;
        }
        fs::path target = paks / id;
        // The swap: the old folder aside, the new one in, the old one gone. A failure between the two renames puts
        // the old one back.
        fs::path old = paks / (".old-" + random_tag());
        bool had = fs::exists(target, ec);
        if (had) {
            fs::rename(target, old, ec);
            if (ec) {
                error = "the installed pak couldn't be moved aside (" + ec.message() + ")";
                fs::remove_all(incoming, ec);
                return false;
            }
        }
        fs::rename(incoming, target, ec);
        if (ec) {
            error = "the new pak couldn't be put in place (" + ec.message() + ")";
            std::error_code ec2;
            fs::remove_all(incoming, ec2);
            if (had) fs::rename(old, target, ec2);
            return false;
        }
        if (had) fs::remove_all(old, ec);
        // The season's zip is kept for Send to Device (device_send.cpp); not needed to play.
        if (is_season(id) && !write_file(copy_file(id), data)) fprintf(stderr, "[paks] couldn't keep a copy at %s\n", copy_file(id).string().c_str());
        write_origin(id, from, got);
        mark_removed(id, false);
        blitz::content_dirs_refresh();
        fprintf(stderr, "[paks] installed %s (%s, %zu bytes, SHA-256 %s)\n", id.c_str(), from == "file" ? "by hand" : "the server's", data.size(), got.c_str());
#if BLITZ_MODERN
        if (is_season(id)) blitz::rosters_seed();  // the pak's lineups for the next game, before any download
        else if (!had) blitz::league_pak_arrived(id);  // a new league pak: offered once it's safe (league_select.cpp)
#endif
        return true;
    }

    // The catalog from the server, kept in paks/catalog.json. False (and the cached one kept) when it can't be read.
    bool fetch_catalog(json& catalog, std::string& error) {
        std::string server = blitz::pak_server();
        if (server.empty()) {
            error = "this build has no New Blitz server";
            return false;
        }
        std::string body;
        int status = 0;
        if (!blitz::http_get(server + "catalog.json", "", "", body, status)) {
            error = status ? "the New Blitz server answered " + std::to_string(status) : "the New Blitz server can't be reached";
            fprintf(stderr, "[paks] %scatalog.json: HTTP %d\n", server.c_str(), status);
            return false;
        }
        catalog = json::parse(body, nullptr, false);
        if (!catalog.is_object() || catalog.value("format", "") != "blitz-paks/1") {
            error = "the New Blitz server's catalog can't be read";
            return false;
        }
        std::lock_guard lock{catalog_mutex};
        write_file(catalog_file(), body);
        return true;
    }

    // Relative addresses in the catalog are under the server's.
    std::string resolve(const std::string& server, const std::string& ref) {
        if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) return ref;
        return server + ref;
    }

    // ---- Waiting downloads ----------------------------------------------------------------------------------------
    // A pak downloaded while a game is running (an update, or a new pak found by the automatic check) waits as
    // paks/<id>.update.nbpak and goes in at the next start, before anything reads the paks: never under a running game.

    void drop_waiting(const std::string& id) {
        std::error_code ec;
        fs::remove(update_file(id), ec);
        fs::remove(update_sha_file(id), ec);
    }

    // At start: every waiting pak goes in (its SHA-256 and every file checked, as any install), then a toast once the game
    // is up.
    void install_waiting_updates() {
        std::error_code ec;
        std::vector<std::string> ids;
        for (const auto& e : fs::directory_iterator(blitz::paks_dir(), ec)) {
            std::string name = e.path().filename().string();
            const std::string tail = ".update.nbpak";
            if (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
                std::string id = name.substr(0, name.size() - tail.size());
                if (blitz::valid_pak_id(id)) ids.push_back(id);
            }
        }
        std::sort(ids.begin(), ids.end());
        std::vector<std::string> done;
        for (const std::string& id : ids) {
            if (!blitz::content_updates_on()) {  // turned off since: it isn't wanted
                drop_waiting(id);
                fprintf(stderr, "[paks] %s: content updates are off now; the waiting download dropped\n", id.c_str());
                continue;
            }
            std::string want = trimmed(read_file(update_sha_file(id))), error, got_id;
            std::string before = blitz::pak_installed(id) ? "installed" : "new";
            bool ok = install(read_file(update_file(id)), want, id, "catalog", got_id, error);
            drop_waiting(id);
            if (!ok) {
                fprintf(stderr, "[paks] %s: the waiting download wasn't installed (%s)\n", id.c_str(), error.c_str());
                continue;
            }
            fprintf(stderr, "[paks] %s (%s): the waiting download is in\n", id.c_str(), before.c_str());
            for (const blitz::InstalledPak& p : blitz::installed_paks()) if (p.id == id) done.push_back(p.title);
        }
        if (done.empty()) return;
        std::string what = done.size() == 1 ? done[0] : std::to_string(done.size()) + " paks";
        // The toast waits for the game: one posted before it starts is dropped (main.cpp's update_gfx).
        std::thread([what] {
            while (!ultramodern::is_game_started()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::this_thread::sleep_for(std::chrono::seconds(3));
            blitz::post_status(what + " updated");
        }).detach();
    }

    // Restart in place (soft_restart.cpp; a phone's Restart, 1.0.6): the game is stopped, so a pak that downloaded during
    // the game goes in now, as at a launch. Before, it waited for the whole app to start again, which a phone rarely does
    // (the Surface Duo's CFL pak never went in).
    [[maybe_unused]] const bool waiting_soft_reset = blitz::on_soft_reset("waiting paks", [] { install_waiting_updates(); });

    // ---- The sync: the catalog, then every pak that's new or changed ----------------------------------------------

    void set_progress(const std::string& doing, uint64_t got = 0, uint64_t total = 0) {
        std::lock_guard lock{progress_mutex};
        progress_now.busy = true;
        progress_now.doing = doing;
        progress_now.got = got;
        progress_now.total = total;
    }

    std::string megabytes(uint64_t n) {
        char buf[32];
        snprintf(buf, sizeof(buf), n < (1u << 20) ? "%.2f MB" : "%.1f MB", n / 1048576.0);
        return buf;
    }

    std::string join(const std::vector<std::string>& v) {
        std::string out;
        for (size_t i = 0; i < v.size(); i++) {
            if (i > 0) out += i + 1 == v.size() ? " and " : ", ";
            out += v[i];
        }
        return out;
    }

    void sync(bool by_hand, const std::vector<std::string>& only, blitz::PakSyncResult& r) {
        set_progress("Checking for new paks…");
        json catalog;
        if (!fetch_catalog(catalog, r.error)) return;
        r.reached = true;
        const bool started = ultramodern::is_game_started();
        const std::set<std::string> removed = removed_ids();
        struct Job {
            std::string id, title, sha;
            json entry;
            uint64_t size = 0;
            bool update = false, waiting = false;
        };
        std::vector<Job> jobs;
        std::set<std::string> offered;
        for (const auto& [id, e] : catalog_entries(catalog)) {
            offered.insert(id);
            bool asked = std::find(only.begin(), only.end(), id) != only.end();
            if (!only.empty() && !asked) continue;
            Job j{id, e.value("title", id), e.value("sha256", ""), e, e.value("size", (uint64_t)0)};
            if (!e.contains("file") || j.sha.empty()) continue;
            if (e.value("min_build", 0) > std::atoi(BLITZ_BUILD)) {
                if (asked || by_hand) r.failed.push_back(j.title + ": it needs a newer New Blitz");
                continue;
            }
            if (!by_hand && !e.value("auto", true)) continue;  // offered, not pushed: Check or Get
            j.update = blitz::pak_installed(id);
            if (j.update) {
                Origin o = read_origin(id);
                if (o.from == "file") {  // the player's own: left as it is
                    fprintf(stderr, "[paks] %s: installed by hand, left alone\n", id.c_str());
                    continue;
                }
                if (o.sha256 == j.sha) continue;
                if (o.sha256.empty()) {  // no record of what's in: its version
                    std::string have;
                    for (const blitz::InstalledPak& p : blitz::installed_paks()) if (p.id == id) have = p.version;
                    if (have == e.value("version", "")) continue;
                }
            }
            else if (removed.count(id) && !asked) {
                continue;  // the player removed it: only Get (or Add…) brings it back
            }
            if (blitz::file_exists(update_file(id)) && trimmed(read_file(update_sha_file(id))) == j.sha) {
                if (started) continue;  // downloaded already: in at the next start
                j.waiting = true;       // the game isn't running yet: in now
            }
            jobs.push_back(std::move(j));
        }
        for (const std::string& id : only) if (!offered.count(id)) r.unavailable.push_back(id);
        // Mobile data: the automatic downloads take a little; the rest wait for Wi-Fi (Check takes them now).
        // Tests: BLITZ_TEST_METERED=1 plays a metered network, BLITZ_TEST_METERED_LIMIT=<bytes> a smaller allowance.
        const char* test_metered = getenv("BLITZ_TEST_METERED");
        if (!by_hand && !jobs.empty() && (test_metered ? *test_metered == '1' : blitz::network_metered())) {
            uint64_t budget = blitz::auto_metered_limit;
            if (const char* l = getenv("BLITZ_TEST_METERED_LIMIT"); l && *l) budget = std::strtoull(l, nullptr, 10);
            std::vector<Job> keep;
            for (Job& j : jobs) {
                if (j.waiting || j.size <= budget) {
                    if (!j.waiting) budget -= j.size;
                    keep.push_back(std::move(j));
                }
                else {
                    fprintf(stderr, "[paks] %s (%s): mobile data, waiting for Wi-Fi\n", j.id.c_str(), megabytes(j.size).c_str());
                    r.wifi.push_back(j.title);
                    std::lock_guard lock{progress_mutex};
                    wifi_ids.insert(j.id);
                }
            }
            jobs = std::move(keep);
        }
        for (const Job& j : jobs) {
            std::lock_guard lock{progress_mutex};
            wifi_ids.erase(j.id);
        }
        const std::string server = blitz::pak_server();
        for (size_t i = 0; i < jobs.size(); i++) {
            const Job& j = jobs[i];
            std::string body, error, id;
            const std::string count = jobs.size() > 1 ? " (" + std::to_string(i + 1) + " of " + std::to_string(jobs.size()) + ")" : "";
            if (j.waiting) body = read_file(update_file(j.id));
            else {
                std::string url = resolve(server, j.entry.value("file", ""));
                set_progress("Downloading " + j.title + count + "…", 0, j.size);
                fprintf(stderr, "[paks] downloading %s (%s)\n", url.c_str(), megabytes(j.size).c_str());
                int status = 0;
                uint64_t size = j.size;
                bool ok = blitz::http_get_progress(url, body, status, [&j, count, size](uint64_t got, uint64_t total) {
                    set_progress("Downloading " + j.title + count + "…", got, total ? total : size);
                });
                if (!ok) {
                    r.failed.push_back(j.title + ": " + (status ? "the server answered " + std::to_string(status) : std::string("it couldn't be downloaded")));
                    continue;
                }
                if (body.size() > max_pak || sha_of(body) != j.sha) {
                    r.failed.push_back(j.title + ": the download doesn't match the server's checksum");
                    continue;
                }
            }
            // A running game keeps what it has: the download waits for the next start (by hand, a new pak goes in now,
            // as Add… does, for the game to pick up after a restart).
            if (started && (j.update || !by_hand)) {
                if (!write_file(update_file(j.id), body) || !write_file(update_sha_file(j.id), j.sha + "\n")) {
                    drop_waiting(j.id);
                    r.failed.push_back(j.title + ": it couldn't be kept for the next start");
                    continue;
                }
                fprintf(stderr, "[paks] %s downloaded: it goes in at the next start\n", j.id.c_str());
                r.staged.push_back(j.title);
                continue;
            }
            set_progress("Installing " + j.title + count + "…", body.size(), body.size());
            if (install(body, j.sha, j.id, "catalog", id, error)) {
                drop_waiting(j.id);
                (j.update ? r.updated : r.added).push_back(j.title);
            }
            else r.failed.push_back(j.title + ": " + error);
        }
    }

    // The last summary the game hasn't shown yet (an automatic sync at launch): a toast once the game is up.
    void toast_when_up(const std::string& what) {
        std::thread([what] {
            while (!ultramodern::is_game_started()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::this_thread::sleep_for(std::chrono::seconds(3));
            blitz::post_status(what);
        }).detach();
    }
}

namespace blitz {
    std::string PakSyncResult::summary() const {
        if (!reached) return "Couldn't check for new paks: " + (error.empty() ? std::string("no answer") : error) + ".";
        std::vector<std::string> parts;
        if (!added.empty()) parts.push_back("Added " + join(added) + ".");
        if (!updated.empty()) parts.push_back("Updated " + join(updated) + ".");
        if (!staged.empty()) parts.push_back(join(staged) + (staged.size() == 1 ? " is" : " are") + " downloaded: in at the next start.");
        if (!wifi.empty()) parts.push_back(join(wifi) + (wifi.size() == 1 ? " waits" : " wait") + " for Wi-Fi (or press Check).");
        for (const std::string& f : failed) parts.push_back(f + ".");
        if (parts.empty()) return "Up to date.";
        std::string out;
        for (const std::string& p : parts) out += (out.empty() ? "" : " ") + p;
        return out;
    }

    std::string pak_server() {
        std::string s;
        if (const char* e = getenv("BLITZ_PAK_SERVER"); e != nullptr) s = trimmed(e);  // tests ("" = none)
        else s = trimmed(read_file(recompui::file::get_program_path() / "paks" / "server"));
        if (s.rfind("https://", 0) != 0 && s.rfind("http://", 0) != 0) return {};
        if (s.back() != '/') s += '/';
        return s;
    }

    bool pak_install_bytes(const std::string& zip, const std::string& want_sha256, std::string& error) {
        try {
            std::string id;
            return install(zip, want_sha256, "", "catalog", id, error);
        }
        catch (const std::exception& e) {  // nothing may escape an install thread
            error = e.what();
            return false;
        }
    }

    bool pak_install_file(const fs::path& zip, const std::string& want_sha256, std::string& error) {
        std::error_code ec;
        auto size = fs::file_size(zip, ec);
        if (ec || size > max_pak) {
            error = ec ? "the file can't be read" : "the file is too big for a pak";
            return false;
        }
        return pak_install_bytes(read_file(zip), want_sha256, error);
    }

    bool pak_install_by_hand(const fs::path& zip, std::string& id_out, std::string& error) {
        std::error_code ec;
        auto size = fs::file_size(zip, ec);
        if (ec || size > max_pak) {
            error = ec ? "the file can't be read" : "the file is too big for a pak";
            return false;
        }
        try {
            return install(read_file(zip), "", "", "file", id_out, error);
        }
        catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    bool pak_remove(const std::string& id, std::string& error) {
        if (!valid_pak_id(id)) {
            error = "no such pak";
            return false;
        }
        {
            std::lock_guard lock{install_mutex};
            std::error_code ec;
            fs::path dir = paks_dir() / id, old = paks_dir() / (".old-" + random_tag());
            if (fs::exists(dir, ec)) {
                fs::rename(dir, old, ec);
                if (ec) {
                    error = ec.message();
                    return false;
                }
                fs::remove_all(old, ec);
            }
            fs::remove(copy_file(id), ec);
            fs::remove(origin_file(id), ec);
            drop_waiting(id);
        }
        mark_removed(id, true);
        content_dirs_refresh();
        fprintf(stderr, "[paks] removed %s\n", id.c_str());
        return true;
    }

    bool pak_remove(std::string& error) { return pak_remove(season_pak_id, error); }

    bool is_pak_file(const fs::path& p) {
        std::string ext = p.extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".nbpak") return true;
        if (ext != ".zip") return false;
        Zip z;
        if (!mz_zip_reader_init_file(&z.zip, p.string().c_str(), 0)) return false;
        z.open = true;
        return mz_zip_reader_locate_file(&z.zip, "pak.json", nullptr, 0) >= 0;
    }

    bool content_updates_on() {
#if !BLITZ_MODERN
        return false;
#else
        // Read from season.json itself: the first check runs at launch, before the settings exist. On unless turned off.
        json season = json::parse(read_file(recomp::get_config_path() / "season.json"), nullptr, false);
        if (season.is_object() && season.contains("live_rosters") && season["live_rosters"].is_boolean()) return season["live_rosters"].get<bool>();
        return true;
#endif
    }

    std::vector<PostedPak> posted_paks() {
        std::vector<PostedPak> out;
        json catalog = cached_catalog();
        std::set<std::string> removed = removed_ids();
        std::vector<InstalledPak> have = installed_paks();
        for (const auto& [id, e] : catalog_entries(catalog)) {
            PostedPak p;
            p.id = id;
            p.title = e.value("title", id);
            p.version = e.value("version", "");
            p.kind = e.value("kind", is_season(id) ? "season" : "");
            p.summary = e.value("summary", "");
            p.size = e.value("size", (uint64_t)0);
            p.too_new = e.value("min_build", 0) > std::atoi(BLITZ_BUILD);
            for (const InstalledPak& h : have) {
                if (h.id != id) continue;
                p.installed = true;
                Origin o = read_origin(id);
                p.by_hand = o.from == "file";
                p.newer = !o.sha256.empty() ? o.sha256 != e.value("sha256", "") : h.version != p.version;
            }
            p.removed = !p.installed && removed.count(id) > 0;
            p.waiting = file_exists(update_file(id)) && trimmed(read_file(update_sha_file(id))) == e.value("sha256", "");
            {
                std::lock_guard lock{progress_mutex};
                p.wifi = wifi_ids.count(id) > 0;
            }
            out.push_back(std::move(p));
        }
        return out;
    }

    bool pak_sync_start(bool by_hand, const std::vector<std::string>& only, std::function<void(const PakSyncResult&)> done) {
        if (syncing.exchange(true)) return false;
        {
            std::lock_guard lock{progress_mutex};
            progress_now = PakSyncProgress{true, "Checking for new paks…", 0, 0};
        }
        std::thread([by_hand, only, done] {
            PakSyncResult r;
            try {
                sync(by_hand, only, r);
            }
            catch (const std::exception& e) {  // nothing may escape the thread
                fprintf(stderr, "[paks] sync: %s\n", e.what());
                r.failed.push_back(std::string("the check stopped: ") + e.what());
            }
            fprintf(stderr, "[paks] sync (%s): %s\n", by_hand ? "by hand" : "automatic", r.summary().c_str());
            {
                std::lock_guard lock{progress_mutex};
                progress_now = PakSyncProgress{};
            }
            syncing = false;
            if (done) done(r);
        }).detach();
        return true;
    }

    bool pak_sync_busy() { return syncing.load(); }

    PakSyncProgress pak_sync_progress() {
        std::lock_guard lock{progress_mutex};
        return progress_now;
    }

    // Blitz Builder's Add It (and Get the 2026 Season): the season by itself, by hand.
    // An automatic sync may be running already (it starts with the app, and first run asks this at the same time): this
    // one waits for it, then runs (by then the season is often in, and it just says so).
    bool pak_download_start(std::function<void(bool, const std::string&)> done) {
        auto finish = [done](const PakSyncResult& r) {
            if (!r.reached) return done(false, "Couldn't get the 2026 Season: " + r.error + ".");
            if (!r.unavailable.empty()) return done(false, "The 2026 Season isn't available from the server right now.");
            if (!r.failed.empty()) {
                std::string why = r.failed[0];
                if (why.rfind("2026 Season: ", 0) == 0) why = why.substr(13);
                return done(false, "The 2026 Season wasn't installed: " + why + ".");
            }
            if (!r.staged.empty()) return done(true, "2026 Season downloaded: it goes in at the next start");
            done(true, "2026 Season added");
        };
        if (pak_sync_start(true, {season_pak_id}, finish)) return true;
        fprintf(stderr, "[paks] the season's download waits for the sync that's running\n");
        std::thread([finish] {
            while (!pak_sync_start(true, {season_pak_id}, finish)) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }).detach();
        return true;
    }

    bool pak_download_busy() { return syncing.load(); }

    std::string pak_update_available() {
        if (!season_pak_installed()) return {};
        json e = catalog_entry(cached_catalog());
        std::string v = e.is_object() ? e.value("version", "") : "";
        return !v.empty() && v > season_pak_version() ? v : std::string{};
    }

    bool headshots_allowed() {
        if (season_content_bundled()) return true;  // a private build, as before the pak
        return season_pak_installed() && season_pak_flag("headshots", false) && catalog_switch("headshots");
    }

    bool live_rosters_on() {
#if !BLITZ_MODERN
        return false;
#else
        // The same setting as the paks' (content_updates_on): on unless the player turned it off (1.0.6; before, a public
        // build had it off until asked).
        if (!content_updates_on()) return false;
        if (rosters_build_source()) return true;  // a private build's own source
        return season_pak_installed() && catalog_switch("live_rosters");
#endif
    }

    void set_live_rosters(bool on) {
#if !BLITZ_MODERN
        (void)on;
        return;
#endif
        recomp::config::Config& season = recompui::config::get_config("season");
        season.update_option_value("live_rosters", on);
        season.save_config();
        fprintf(stderr, "[paks] content updates (rosters, uniforms, paks) %s\n", on ? "on" : "off");
    }

    std::string pak_rosters_hub() {
        if (!season_pak_installed()) return {};
        std::string server = pak_server();
        json c = cached_catalog();
        if (server.empty() || !c.is_object() || !c.contains("live_rosters") || !c["live_rosters"].is_object()) return {};
        std::string hub = c["live_rosters"].value("hub", "");
        return hub.empty() ? std::string{} : resolve(server, hub);
    }

    void paks_at_launch() {
        // Dev and tests: BLITZ_PAK_INSTALL=<file> installs a pak as Add-Ons' Add… would (the player's own); BLITZ_PAK_DOWNLOAD=1
        // runs Blitz Builder's download (from BLITZ_PAK_SERVER). Each logs how it went ("[paks] ...", "[season pak] ...").
        if (const char* f = getenv("BLITZ_PAK_INSTALL"); f && *f) {
            std::string error, id;
            bool ok = pak_install_by_hand(f, id, error);
            fprintf(stderr, "[season pak] BLITZ_PAK_INSTALL %s: %s\n", f, ok ? ("installed " + id).c_str() : error.c_str());
        }
        if (const char* d = getenv("BLITZ_PAK_DOWNLOAD"); d && *d == '1') {
            std::atomic<bool> finished{false};
            bool started = pak_download_start([&finished](bool ok, const std::string& message) {
                fprintf(stderr, "[season pak] BLITZ_PAK_DOWNLOAD: %s (%s)\n", ok ? "installed" : "failed", message.c_str());
                finished = true;
            });
            while (started && !finished) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        install_waiting_updates();
#if BLITZ_HAS_ROM_DROP_FOLDER
        // A pak in the drop folder (Send to Device from a computer, or copied there by hand): installed before the
        // game starts, then removed from there. Not a second time if it's the one already in. Kept up to date afterwards
        // as the server's (a computer's Blitz Builder sends the season it downloaded).
        {
            const fs::path drop = rom_drop_folder();
            std::error_code ec;
            std::vector<fs::path> found;
            for (const auto& e : fs::directory_iterator(drop, ec)) {
                std::string name = e.path().filename().string();
                if (name.rfind("._", 0) == 0 || !e.is_regular_file(ec)) continue;
                std::string ext = e.path().extension().string();
                for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                if (ext == ".nbpak") found.push_back(e.path());
            }
            std::sort(found.begin(), found.end());
            for (const fs::path& p : found) {
                std::string error, data = read_file(p);
                json m = peek_manifest(data);
                std::string id = m.is_object() ? m.value("id", "") : "";
                if (valid_pak_id(id) && pak_installed(id) && read_origin(id).sha256 == sha_of(data)) {
                    fprintf(stderr, "[paks] %s: already installed\n", p.string().c_str());
                    fs::remove(p, ec);
                    continue;
                }
                if (pak_install_bytes(data, "", error)) {
                    fprintf(stderr, "[paks] %s: installed\n", p.string().c_str());
                    fs::remove(p, ec);
                }
                else {
                    fprintf(stderr, "[paks] %s: %s\n", p.string().c_str(), error.c_str());
#if BLITZ_MODERN
                    blitz::league_notice("Pak Not Installed", "The pak in " + p.filename().string() + " wasn't installed: " + error + ".");
#else
                    post_notice("The pak in " + p.filename().string() + " wasn't installed: " + error + ".");
#endif
                    fs::rename(p, fs::path(p).replace_extension(".failed"), ec);
                }
            }
        }
#endif
        if (pak_server().empty()) return;
        if (content_updates_on()) {
            // The posted paks, in the background (1.0.6): new ones in, changed ones updated. Before the game starts they go
            // straight in; once it's running they wait for the next start.
            (void)network_metered();  // starts the system's watch early (an iPhone's answer comes a moment later)
            bool started = pak_sync_start(false, {}, [](const PakSyncResult& r) {
                if (!r.added.empty() || !r.updated.empty() || !r.staged.empty() || !r.wifi.empty()) toast_when_up(r.summary());
            });
            if (!started) fprintf(stderr, "[paks] a sync is already running\n");
            return;
        }
        // Content updates off: no downloads. The catalog's switches still count for a pak whose headshots are on.
        if (!season_pak_installed() || !season_pak_flag("headshots", false)) return;
        std::thread([] {
            try {
                json catalog;
                std::string error;
                if (fetch_catalog(catalog, error)) fprintf(stderr, "[paks] catalog read (content updates off: nothing downloaded)\n");
                else fprintf(stderr, "[paks] catalog: %s (the last one read stands)\n", error.c_str());
            }
            catch (const std::exception& e) {
                fprintf(stderr, "[paks] catalog: %s\n", e.what());
            }
        }).detach();
    }
}
