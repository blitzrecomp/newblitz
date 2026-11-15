// The 2026 Season pak: installing, removing and downloading it, and the season server's catalog (season_pak.h,
// docs/design/SEASON_PAK.md), 2026-11-13.
//
// A pak is a zip (.nbpak) with pak.json at its root and a tree shaped like the app's Resources (packs/, rosters/,
// uniforms/, art/, announcer/, houston/, probowl/). It unpacks into <app folder>/paks/season2026/, which the loaders read
// between Resources and the app folder (content_dirs.cpp). A copy of the zip stays beside it (paks/season2026.nbpak) for
// Blitz Builder to send to a phone or a console with the ROM.
//
// Checks, all before the installed pak is touched: the whole file's SHA-256 against the catalog's (a download); every
// entry's name (no absolute paths, "..", backslashes or drive letters, and only the folders above); the count and the
// unpacked size; pak.json (format, id, the oldest build that reads it); every file against pak.json's SHA-256 list (a
// file put in by hand has only that); the roster pack parses. Then the old folder is swapped out for the new in two
// renames. miniz checks each entry's CRC-32 as it unpacks.
//
// The server (tools/rosters_hub_worker, its public /paks/ routes): catalog.json names each pak's file, size and SHA-256,
// and carries the switches every installed copy follows: removing the pak from the catalog stops new installs (the
// kill switch), "headshots": false stops the crew cards' ESPN photos, "live_rosters": false the weekly downloads. The
// address is in Resources/paks/server, which a release build writes from a file outside the source (the Makefile's
// build/pak_server), never from the code.
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
#include "recompui/config.h"
#include "util/file.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace blitz {
    void post_notice(const std::string& msg);  // notices.cpp
}

namespace {
    constexpr size_t max_pak = 64u << 20;        // a 2026 pak is about 2 MB
    constexpr size_t max_unpacked = 128u << 20;
    constexpr mz_uint max_entries = 8000;        // about 1,500 today
    const std::set<std::string> top_folders = {"packs", "rosters", "uniforms", "art", "announcer", "houston", "probowl"};
    const std::set<std::string> top_files = {"pak.json", "LICENSE-CONTENT.txt"};

    std::mutex catalog_mutex;  // the cached catalog (paks/catalog.json)
    std::mutex install_mutex;  // one install or removal at a time
    std::atomic<bool> downloading{false};

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

    fs::path catalog_file() { return blitz::paks_dir() / "catalog.json"; }
    fs::path pak_copy() { return blitz::paks_dir() / (std::string(blitz::season_pak_id) + ".nbpak"); }

    // The last catalog read (paks/catalog.json), or null.
    json cached_catalog() {
        std::lock_guard lock{catalog_mutex};
        json c = json::parse(read_file(catalog_file()), nullptr, false);
        return c.is_object() ? c : json();
    }

    // The season pak's entry in a catalog, or null (withdrawn).
    json catalog_entry(const json& catalog) {
        if (!catalog.is_object() || !catalog.contains("paks") || !catalog["paks"].is_object()) return json();
        auto it = catalog["paks"].find(blitz::season_pak_id);
        if (it == catalog["paks"].end() || !it->is_object() || !it->value("available", true)) return json();
        return *it;
    }

    // A switch in the cached catalog's entry: off when the entry says so, when the pak is withdrawn, or when no
    // catalog has been read yet (a pak installed from a file, before its first catalog check).
    bool catalog_switch(const char* key) {
        json e = catalog_entry(cached_catalog());
        return e.is_object() && e.value(key, true);
    }

    // An entry's name: relative, forward slashes, no "..", inside the pak's folders.
    bool safe_name(const std::string& n) {
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
        return top_folders.count(n.substr(0, slash)) > 0;
    }

    struct Zip {
        mz_zip_archive zip{};
        bool open = false;
        ~Zip() {
            if (open) mz_zip_reader_end(&zip);
        }
    };

    // Unpacks and checks a pak (in memory) into `to`. Every failure says why.
    bool unpack(const std::string& data, const fs::path& to, std::string& error) {
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
                if (!safe_name(dir + "/x")) {
                    error = "it has a folder outside the pak's (" + name + ")";
                    return false;
                }
                continue;
            }
            if (!safe_name(name)) {
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
            hashes[name] = blitzsha::sha256_hex(body.data(), body.size());
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
        if (!manifest.is_object()) {
            error = "it has no pak.json";
            return false;
        }
        if (manifest.value("format", "") != "blitz-pak/1" || manifest.value("id", "") != blitz::season_pak_id) {
            error = "it isn't the 2026 Season pak";
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
        json pack = json::parse(read_file(to / "packs" / "blitz2026" / "pack.json"), nullptr, false);
        if (!pack.is_object() || pack.value("format", "") != "blitz-pack/2" || pack.value("id", "") != "blitz2026") {
            error = "its roster pack can't be read";
            return false;
        }
        return true;
    }

    bool install(const std::string& data, const std::string& want_sha256, bool keep_copy, std::string& error) {
        if (data.size() > max_pak) {
            error = "the file is too big for a season pak";
            return false;
        }
        std::string got = blitzsha::sha256_hex(data.data(), data.size());
        if (!want_sha256.empty() && got != want_sha256) {
            error = "the download doesn't match the server's checksum";
            fprintf(stderr, "[season pak] SHA-256 %s, the catalog says %s\n", got.c_str(), want_sha256.c_str());
            return false;
        }
        std::lock_guard lock{install_mutex};
        std::error_code ec;
        fs::path paks = blitz::paks_dir(), target = blitz::season_pak_dir();
        fs::create_directories(paks, ec);
        if (auto space = fs::space(paks, ec); !ec && space.available < 4 * data.size() + (16u << 20)) {
            error = "there isn't enough free space";
            return false;
        }
        fs::path incoming = paks / (".incoming-" + random_tag());
        fs::remove_all(incoming, ec);
        if (!unpack(data, incoming, error)) {
            fs::remove_all(incoming, ec);
            return false;
        }
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
        // Kept for Send to Device (device_send.cpp); not needed to play.
        if (keep_copy && !write_file(pak_copy(), data)) fprintf(stderr, "[season pak] couldn't keep a copy at %s\n", pak_copy().string().c_str());
        blitz::content_dirs_refresh();
        fprintf(stderr, "[season pak] installed %s %s (%zu bytes, SHA-256 %s)\n", blitz::season_pak_id, blitz::season_pak_version().c_str(), data.size(), got.c_str());
#if BLITZ_MODERN
        blitz::rosters_seed();  // the pak's lineups for the next game, before any download
#endif
        return true;
    }

    // The catalog from the server, kept in paks/catalog.json. False (and the cached one kept) when it can't be read.
    bool fetch_catalog(json& catalog, std::string& error) {
        std::string server = blitz::pak_server();
        if (server.empty()) {
            error = "this build has no season server";
            return false;
        }
        std::string body;
        int status = 0;
        if (!blitz::http_get(server + "catalog.json", "", "", body, status)) {
            error = status ? "the season server answered " + std::to_string(status) : "the season server can't be reached";
            fprintf(stderr, "[season pak] %scatalog.json: HTTP %d\n", server.c_str(), status);
            return false;
        }
        catalog = json::parse(body, nullptr, false);
        if (!catalog.is_object() || catalog.value("format", "") != "blitz-paks/1") {
            error = "the season server's catalog can't be read";
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

    void download(std::function<void(bool, const std::string&)> done) {
        auto fail = [&](const std::string& why) {
            fprintf(stderr, "[season pak] download: %s\n", why.c_str());
            done(false, why);
        };
        std::string error;
        json catalog;
        if (!fetch_catalog(catalog, error)) return fail("Couldn't get the 2026 Season: " + error + ".");
        json entry = catalog_entry(catalog);
        if (!entry.is_object() || !entry.contains("file") || !entry.contains("sha256")) {
            return fail("The 2026 Season isn't available from the server right now.");
        }
        if (entry.value("min_build", 0) > std::atoi(BLITZ_BUILD)) {
            return fail("The 2026 Season on the server needs a newer New Blitz.");
        }
        std::string url = resolve(blitz::pak_server(), entry.value("file", "")), body;
        int status = 0;
        fprintf(stderr, "[season pak] downloading %s\n", url.c_str());
        if (!blitz::http_get(url, "", "", body, status)) {
            return fail("Couldn't download the 2026 Season (HTTP " + std::to_string(status) + ").");
        }
        if (!install(body, entry.value("sha256", ""), true, error)) return fail("The 2026 Season wasn't installed: " + error + ".");
        done(true, "2026 Season added");
    }
}

namespace blitz {
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
            return install(zip, want_sha256, true, error);
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
            error = ec ? "the file can't be read" : "the file is too big for a season pak";
            return false;
        }
        return pak_install_bytes(read_file(zip), want_sha256, error);
    }

    bool pak_remove(std::string& error) {
        std::lock_guard lock{install_mutex};
        std::error_code ec;
        fs::path old = paks_dir() / (".old-" + random_tag());
        if (fs::exists(season_pak_dir(), ec)) {
            fs::rename(season_pak_dir(), old, ec);
            if (ec) {
                error = ec.message();
                return false;
            }
            fs::remove_all(old, ec);
        }
        fs::remove(pak_copy(), ec);
        content_dirs_refresh();
        fprintf(stderr, "[season pak] removed %s\n", season_pak_id);
        return true;
    }

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

    bool pak_download_start(std::function<void(bool, const std::string&)> done) {
        if (downloading.exchange(true)) return false;
        std::thread([done] {
            try {
                download(done);
            }
            catch (const std::exception& e) {
                fprintf(stderr, "[season pak] download: %s\n", e.what());
                done(false, std::string("Couldn't get the 2026 Season: ") + e.what());
            }
            downloading = false;
        }).detach();
        return true;
    }

    bool pak_download_busy() { return downloading.load(); }

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
        // Read from season.json itself: the first check runs at launch, before the settings exist.
        bool on = rosters_build_source();
        json season = json::parse(read_file(recomp::get_config_path() / "season.json"), nullptr, false);
        if (season.is_object() && season.contains("live_rosters") && season["live_rosters"].is_boolean()) on = season["live_rosters"].get<bool>();
        if (!on) return false;
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
        fprintf(stderr, "[season pak] Roster Updates %s\n", on ? "on" : "off");
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
        // Dev and tests: BLITZ_PAK_INSTALL=<file> installs a pak as Add-Ons would; BLITZ_PAK_DOWNLOAD=1 runs Blitz
        // Builder's download (from BLITZ_PAK_SERVER). Each logs how it went ("[season pak] ...").
        if (const char* f = getenv("BLITZ_PAK_INSTALL"); f && *f) {
            std::string error;
            bool ok = pak_install_file(f, "", error);
            fprintf(stderr, "[season pak] BLITZ_PAK_INSTALL %s: %s\n", f, ok ? "installed" : error.c_str());
        }
        if (const char* d = getenv("BLITZ_PAK_DOWNLOAD"); d && *d == '1') {
            std::atomic<bool> finished{false};
            bool started = pak_download_start([&finished](bool ok, const std::string& message) {
                fprintf(stderr, "[season pak] BLITZ_PAK_DOWNLOAD: %s (%s)\n", ok ? "installed" : "failed", message.c_str());
                finished = true;
            });
            while (started && !finished) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
#if BLITZ_HAS_ROM_DROP_FOLDER
        // A pak in the drop folder (Send to Device from a computer, or copied there by hand): installed before the
        // game starts, then removed from there. Not a second time if it's the version already in.
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
                std::string error;
                bool same = blitz::file_exists(pak_copy()) && read_file(pak_copy()) == read_file(p);
                if (same && season_pak_installed()) {
                    fprintf(stderr, "[season pak] %s: already installed\n", p.string().c_str());
                    fs::remove(p, ec);
                    continue;
                }
                if (pak_install_file(p, "", error)) {
                    fprintf(stderr, "[season pak] %s: installed\n", p.string().c_str());
                    fs::remove(p, ec);
                }
                else {
                    fprintf(stderr, "[season pak] %s: %s\n", p.string().c_str(), error.c_str());
                    post_notice("The 2026 Season in " + p.filename().string() + " wasn't installed: " + error + ".");
                    fs::rename(p, fs::path(p).replace_extension(".failed"), ec);
                }
            }
        }
#endif
        // The catalog's switches (headshots, live rosters) and newer versions, in the background, only when something
        // would use them: no request at all for a player with both off, or without the pak.
        if (!season_pak_installed() || pak_server().empty()) return;
        json season = json::parse(read_file(recomp::get_config_path() / "season.json"), nullptr, false);
        bool live = season.is_object() && season.value("live_rosters", false);
        if (!live && !season_pak_flag("headshots", false)) return;
        std::thread([] {
            try {
                json catalog;
                std::string error;
                if (fetch_catalog(catalog, error)) fprintf(stderr, "[season pak] catalog read%s\n", pak_update_available().empty() ? "" : ": a newer 2026 Season is out");
                else fprintf(stderr, "[season pak] catalog: %s (the last one read stands)\n", error.c_str());
            }
            catch (const std::exception& e) {
                fprintf(stderr, "[season pak] catalog: %s\n", e.what());
            }
        }).detach();
    }
}
