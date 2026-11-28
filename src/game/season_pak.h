// The 2026 Season pak (docs/design/SEASON_PAK.md): the licensed half of New Blitz (the 2026 rosters and lineups,
// uniforms, team art, VS helmets, the announcer's calls, Houston, Pro Bowl) as one download, installed into
// <app folder>/paks/season2026/ and read by every loader as a second Resources folder. A private build may carry the same
// content in its Resources instead (make app-modern without BLITZ_CONTENT=public); either way counts as "the season".
//
// content_dirs.cpp: where content is read from, and whether the season is there (every build, the PSP's too).
// season_pak.cpp: installing, removing, downloading, the server's catalog (computers, phones and consoles).
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace blitz {
    // The pak's id: its folder under <app folder>/paks, its .nbpak's name, the catalog's key.
    inline constexpr const char* season_pak_id = "season2026";

    // Content roots in rising precedence: the bundle's Resources, then the installed season pak. Loaders read each in
    // turn and the app folder after them (the player's own add-ons win), as they did with Resources alone.
    std::vector<std::filesystem::path> content_dirs();
    // The same, highest precedence first (for "the first that has it" lookups).
    std::vector<std::filesystem::path> content_dirs_newest_first();
    std::filesystem::path paks_dir();        // <app folder>/paks
    std::filesystem::path season_pak_dir();  // <app folder>/paks/season2026 (whether or not it's there)
    // A usable pak there: its pak.json reads, is a blitz-pak/1 for this id, and this build is new enough for it.
    bool season_pak_installed();
    std::string season_pak_version();  // its pak.json "version", or "" without one
    // A boolean in the installed pak's pak.json (e.g. "headshots"), or the fallback.
    bool season_pak_flag(const char* key, bool fallback);
    // The 2026 season's content at all: the roster pack in Resources (a private build) or in the installed pak.
    // What needs it (Lineups, the Roster Room, crews, Pro Bowl, uniform sets, Advanced) hides or steps aside without it.
    bool season_content();
    bool season_content_bundled();  // in Resources (a private build)
    // After an install or a removal (season_pak.cpp): the answers above are worked out again.
    void content_dirs_refresh();
    // Bumped by each refresh: what keeps a list of the installed paks (Settings → Teams → League) reads it again.
    int content_dirs_generation();

    // ---- Every pak (1.0.6): the season and the league paks (Pac-12, CFL), each in <app folder>/paks/<id>/ ----------------
    // A pak's id: 1 to 32 of a-z, 0-9 and _ (its folder, its .nbpak's name, its catalog key).
    bool valid_pak_id(const std::string& id);
    struct InstalledPak {
        std::string id, title, version, summary;  // pak.json's
        std::filesystem::path dir;
    };
    // Every usable pak installed, the season first, then by id. content_dirs() reads them in that order after Resources.
    std::vector<InstalledPak> installed_paks();
    bool pak_installed(const std::string& id);

    // ---- season_pak.cpp (not on the PSP) ----

    // The season server's base address (".../paks/", from Resources/paks/server, which the release build writes from
    // a file outside the source; BLITZ_PAK_SERVER overrides it), or "" when this build has none.
    std::string pak_server();
    // Install a .nbpak (a zip with pak.json at its root). want_sha256: the catalog's hash for a download, else "" (a
    // local file: its pak.json's per-file hashes are the check). On failure the installed pak is left as it was.
    bool pak_install_file(const std::filesystem::path& zip, const std::string& want_sha256, std::string& error);
    bool pak_install_bytes(const std::string& zip, const std::string& want_sha256, std::string& error);
    // Removes the installed pak (the player's own rosters folder, crews and downloads stay).
    bool pak_remove(std::string& error);
    // Any pak by id. A pak the player removes stays out: the automatic downloads skip it until it's added again (Get, Add…).
    bool pak_remove(const std::string& id, std::string& error);
    // Whether a file looks like a pak (a zip whose root has pak.json): Add-Ons routes it to pak_install_by_hand.
    bool is_pak_file(const std::filesystem::path& p);
    // A file installed by hand (Add…): any pak, recorded as the player's own, so the automatic downloads leave it alone.
    // id_out: the pak's id once it's in.
    bool pak_install_by_hand(const std::filesystem::path& zip, std::string& id_out, std::string& error);

    // The one-click download: catalog, pak, hash, install, on a thread of its own. done(ok, message) runs on that
    // thread when it ends (message: what to tell the player). False if one is already running.
    bool pak_download_start(std::function<void(bool ok, const std::string& message)> done);
    bool pak_download_busy();
    // The catalog's newer version than the installed one (from the last catalog read), or "".
    std::string pak_update_available();

    // ---- The posted paks: automatic downloads and Add-Ons' Check (1.0.6; the team: "just have these autoload") ---------------
    // Every pak the server's catalog lists installs and updates itself while Settings → Teams → Keep Rosters and Uniforms
    // Up to Date is on (the default; content_updates_on). Off, nothing downloads by itself; Check still does, when pressed.
    // Never touched: a pak the player installed by hand (Add…), one they removed, one the catalog marks "auto": false (by
    // itself; Check and Get take those), one for a newer build. On a metered network (a phone's mobile data, a hotspot) the
    // automatic downloads take at most auto_metered_limit bytes a start; the rest wait for Wi-Fi (or Check).
    inline constexpr uint64_t auto_metered_limit = 10u << 20;
    bool content_updates_on();
    // What the catalog offers, for Add-Ons' rows (the last catalog read).
    struct PostedPak {
        std::string id, title, version, kind, summary;
        uint64_t size = 0;
        bool installed = false;  // a usable copy is in
        bool by_hand = false;    // installed from a file by the player: left alone
        bool removed = false;    // the player removed it: no automatic download
        bool newer = false;      // installed, and the catalog's is another one
        bool too_new = false;    // needs a newer New Blitz
        bool waiting = false;    // downloaded, goes in at the next start
        bool wifi = false;       // the last automatic check left it for Wi-Fi (mobile data)
    };
    std::vector<PostedPak> posted_paks();
    // How a sync went (pak_sync_start's done, on the sync's thread).
    struct PakSyncResult {
        bool reached = false;                     // the catalog was read
        std::string error;                        // why not, when it wasn't
        std::vector<std::string> added, updated;  // titles, in now
        std::vector<std::string> staged;          // titles, downloaded: in at the next start (the game's running)
        std::vector<std::string> wifi;            // titles left for Wi-Fi (automatic, metered)
        std::vector<std::string> failed;          // "<title>: why"
        std::vector<std::string> unavailable;     // ids asked for (only) that the catalog doesn't offer now
        std::string summary() const;              // one line for the player ("Up to date.", "Added Pac-12." ...)
    };
    // A sync on a thread of its own: the catalog, then every pak that's new or changed, each checked as any install. by_hand:
    // Check or Get (also the "auto": false paks, also on a metered network). only: just these ids (Get, Blitz Builder's
    // season), which also brings back one the player removed. False when one is already running.
    bool pak_sync_start(bool by_hand, const std::vector<std::string>& only, std::function<void(const PakSyncResult&)> done);
    bool pak_sync_busy();
    // Where a running sync is, for Add-Ons' progress line: what it's doing, and the download's bytes (total 0: unknown).
    struct PakSyncProgress {
        bool busy = false;
        std::string doing;  // "Checking for new paks…", "Downloading Pac-12 (2 of 3)…"
        uint64_t got = 0, total = 0;
    };
    PakSyncProgress pak_sync_progress();

    // At launch (main.cpp): a .nbpak in a phone's or console's drop folder (sent by a computer's Blitz Builder, or put
    // there by hand) is installed before the game starts; then, when something needs it, the catalog is read in the
    // background (its switches below).
    void paks_at_launch();
    // ESPN headshots in Pick Your Crew: a private build's bundled season, or the pak with "headshots": true whose server
    // hasn't switched them off. Without either, no request leaves the app (the crew cards show Blitz portraits).
    bool headshots_allowed();
    // Settings → Teams → Roster Updates is on, and (for a pak) the server hasn't switched live rosters off.
    bool live_rosters_on();
    void set_live_rosters(bool on);  // Blitz Builder's "Keep rosters up to date?"
    // The live-rosters address a pak's install follows when the build itself carries none (the catalog's
    // "live_rosters", under the season server), or "".
    std::string pak_rosters_hub();
}
