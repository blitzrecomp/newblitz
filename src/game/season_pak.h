// The 2026 Season pak (docs/design/SEASON_PAK.md): the licensed half of New Blitz (the 2026 rosters and lineups,
// uniforms, team art, VS helmets, the announcer's calls, Houston, Pro Bowl) as one download, installed into
// <app folder>/paks/season2026/ and read by every loader as a second Resources folder. A private build may carry the same
// content in its Resources instead (make app-modern without BLITZ_CONTENT=public); either way counts as "the season".
//
// content_dirs.cpp: where content is read from, and whether the season is there (every build, the PSP's too).
// season_pak.cpp: installing, removing, downloading, the server's catalog (computers, phones and consoles).
#pragma once
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
    // Whether a file looks like a season pak (a zip whose root has pak.json): Add-Ons routes it to pak_install_file.
    bool is_pak_file(const std::filesystem::path& p);

    // The one-click download: catalog, pak, hash, install, on a thread of its own. done(ok, message) runs on that
    // thread when it ends (message: what to tell the player). False if one is already running.
    bool pak_download_start(std::function<void(bool ok, const std::string& message)> done);
    bool pak_download_busy();
    // The catalog's newer version than the installed one (from the last catalog read), or "".
    std::string pak_update_available();

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
