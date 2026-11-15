// Where New Blitz's content is read from (season_pak.h; docs/design/SEASON_PAK.md), 2026-11-13. Before the season
// pak every loader read the bundle's Resources and then the app folder; now an installed pak sits between the two,
// shaped like Resources, so the public build plus the pak loads exactly what a private build loads from its bundle,
// and a player's own add-on in the app folder still wins over both.
//
// Worked out on first use and kept until content_dirs_refresh() (an install or removal from Blitz Builder or Add-Ons),
// so a pak installed before the game starts is used by that game with no restart.
#include "season_pak.h"

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

    bool has_season(const fs::path& root) {
        std::error_code ec;
        return fs::is_regular_file(root / "packs" / "blitz2026" / "pack.json", ec);
    }

    // Call with the mutex held.
    void work_out() {
        if (known) return;
        known = true;
        installed = false;
        version.clear();
        manifest = nlohmann::json::object();
        bundled = has_season(recompui::file::get_program_path());
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
        return out;
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

    void content_dirs_refresh() {
        std::lock_guard lock{mutex};
        known = false;
    }
}
