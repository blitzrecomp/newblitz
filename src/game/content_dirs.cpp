// Where New Blitz's content is read from (season_pak.h; docs/design/SEASON_PAK.md), 2026-11-13. Before the season
// pak every loader read the bundle's Resources and then the app folder; now an installed pak sits between the two,
// shaped like Resources, so the public build plus the pak loads exactly what a private build loads from its bundle,
// and a player's own add-on in the app folder still wins over both.
//
// Worked out on first use and kept until content_dirs_refresh() (an install or removal from Blitz Builder or Add-Ons),
// so a pak installed before the game starts is used by that game with no restart.
#include "season_pak.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>

#include "json/json.hpp"
#include "librecomp/game.hpp"
#include "util/file.h"

namespace fs = std::filesystem;

namespace {
    std::mutex mutex;
    bool known = false;
    bool installed = false, bundled = false;
    std::string version;
    nlohmann::json manifest;
    // The other paks (1.0.6: the league paks), by id: each a pak.json that reads, is a blitz-pak/1 named for its folder, and
    // is for this build. Read after the season's, in id order.
    std::vector<blitz::InstalledPak> others;

    bool has_season(const fs::path& root) {
        std::error_code ec;
        return fs::is_regular_file(root / "packs" / "blitz2026" / "pack.json", ec);
    }

    void find_others() {
        others.clear();
        std::error_code ec;
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(blitz::paks_dir(), ec)) {
            std::string id = e.path().filename().string();
            if (id != blitz::season_pak_id && blitz::valid_pak_id(id) && e.is_directory(ec)) dirs.push_back(e.path());
        }
        std::sort(dirs.begin(), dirs.end());
        for (const fs::path& dir : dirs) {
            std::string id = dir.filename().string();
            std::ifstream in{dir / "pak.json"};
            if (!in) continue;
            try {
                nlohmann::json j = nlohmann::json::parse(in);
                if (j.value("format", "") != "blitz-pak/1" || j.value("id", "") != id) {
                    fprintf(stderr, "[paks] %s/pak.json isn't a pak for that folder: not used\n", dir.string().c_str());
                    continue;
                }
                if (j.value("min_build", 0) > std::atoi(BLITZ_BUILD)) {
                    fprintf(stderr, "[paks] %s needs build %d or newer (this is %s): not used\n", id.c_str(), j.value("min_build", 0), BLITZ_BUILD);
                    continue;
                }
                others.push_back({id, j.value("title", id), j.value("version", ""), j.value("summary", ""), dir});
                fprintf(stderr, "[paks] %s %s installed\n", id.c_str(), others.back().version.c_str());
            }
            catch (const std::exception& e) {
                fprintf(stderr, "[paks] %s/pak.json: %s\n", dir.string().c_str(), e.what());
            }
        }
    }

    // Call with the mutex held.
    void work_out() {
        if (known) return;
        known = true;
        installed = false;
        version.clear();
        manifest = nlohmann::json::object();
        bundled = has_season(recompui::file::get_program_path());
        find_others();
        fs::path dir = blitz::season_pak_dir();
        std::ifstream in{dir / "pak.json"};
        if (!in) return;
        try {
            nlohmann::json j = nlohmann::json::parse(in);
            int min_build = j.value("min_build", 0);
            if (j.value("format", "") != "blitz-pak/1" || j.value("id", "") != blitz::season_pak_id) {
                fprintf(stderr, "[season pak] %s/pak.json isn't a season pak: not used\n", dir.string().c_str());
                return;
            }
            if (min_build > std::atoi(BLITZ_BUILD)) {
                fprintf(stderr, "[season pak] the installed season pak needs build %d or newer (this is %s): not used\n", min_build, BLITZ_BUILD);
                return;
            }
            if (!has_season(dir)) {
                fprintf(stderr, "[season pak] the installed season pak has no roster pack: not used\n");
                return;
            }
            installed = true;
            version = j.value("version", "");
            manifest = std::move(j);
            fprintf(stderr, "[season pak] season pak %s installed\n", version.c_str());
        }
        catch (const std::exception& e) {
            fprintf(stderr, "[season pak] %s/pak.json: %s\n", dir.string().c_str(), e.what());
        }
    }
}

namespace blitz {
    fs::path paks_dir() { return recomp::get_config_path() / "paks"; }
    fs::path season_pak_dir() { return paks_dir() / season_pak_id; }

    std::vector<fs::path> content_dirs() {
        std::lock_guard lock{mutex};
        work_out();
        std::vector<fs::path> out{recompui::file::get_program_path()};
        if (installed) out.push_back(season_pak_dir());
        for (const InstalledPak& p : others) out.push_back(p.dir);
        return out;
    }

    bool valid_pak_id(const std::string& id) {
        if (id.empty() || id.size() > 32) return false;
        for (char c : id) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
        }
        return true;
    }

    std::vector<InstalledPak> installed_paks() {
        std::lock_guard lock{mutex};
        work_out();
        std::vector<InstalledPak> out;
        if (installed) out.push_back({season_pak_id, manifest.value("title", "2026 Season"), version, manifest.value("summary", ""), season_pak_dir()});
        out.insert(out.end(), others.begin(), others.end());
        return out;
    }

    bool pak_installed(const std::string& id) {
        std::lock_guard lock{mutex};
        work_out();
        if (id == season_pak_id) return installed;
        for (const InstalledPak& p : others) if (p.id == id) return true;
        return false;
    }

    std::vector<fs::path> content_dirs_newest_first() {
        std::vector<fs::path> out = content_dirs();
        return {out.rbegin(), out.rend()};
    }

    bool season_pak_installed() {
        std::lock_guard lock{mutex};
        work_out();
        return installed;
    }

    std::string season_pak_version() {
        std::lock_guard lock{mutex};
        work_out();
        return version;
    }

    bool season_pak_flag(const char* key, bool fallback) {
        std::lock_guard lock{mutex};
        work_out();
        auto it = manifest.find(key);
        return installed && it != manifest.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }

    bool season_content_bundled() {
        std::lock_guard lock{mutex};
        work_out();
        return bundled;
    }

    bool season_content() {
        std::lock_guard lock{mutex};
        work_out();
        return bundled || installed;
    }

    std::atomic<int> generation{0};
    void content_dirs_refresh() {
        std::lock_guard lock{mutex};
        known = false;
        generation++;
    }

    int content_dirs_generation() { return generation.load(); }
}
