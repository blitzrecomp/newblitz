// Save Sync's bundle (docs/SAVE_SYNC.md, phase 1): one .nbsave file (a zip) with the player's Controller Pak images, the
// Advanced season files those paks point at, rules.json and season.json, and a manifest. No game or runtime needed, so
// tests/save_sync_test.cpp runs it on its own; src/main/save_sync.cpp is the screens and controller_pak.cpp applies a
// staged import at a pak's first access.
//
// What goes in, per pak folder under saves/ (saves/, original/, packs/<id>/, advanced/<id>/, and original/ of those):
// blitz2000_pak1.mpk and every season/*.adv beside it. Never save states (savestates/, *.state, quit.*), pak_history/,
// .bak copies, per-device settings, the ROM, art, packs or logs: the bundle is built only from those two patterns.
//
// Import: read_bundle checks everything (hashes, sizes, magic and CRC of each season file, every stub in an incoming pak
// resolving to a season file, the pack being installed, the format being no newer than ours) and decides each folder
// (Verdict) before anything is written. stage() then writes the chosen folders' files into <folder>/sync_incoming/ and a
// "ready" mark last; apply_staged() puts them in place, season files first and the pak last, and removes the staging only
// once the pak is written, so a crash anywhere is finished at the next start, before the game reads that pak.
#pragma once
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace blitz::savesync {
    namespace fs = std::filesystem;

    constexpr int format_version = 1;
    constexpr const char* extension = ".nbsave";
    constexpr const char* pak_name = "blitz2000_pak1.mpk";
    constexpr const char* staging = "sync_incoming";

    struct Env {
        fs::path app;                    // the app folder: saves/ under it, rules.json and season.json at its top
        std::string version = "0", build = "0", platform = "?";
        // Whether a roster pack is here (saves/packs/<id>, saves/advanced/<id>). "default" (Advanced without a pack) always is.
        std::function<bool(const std::string& pack)> pack_installed;
        std::time_t now = 0;             // 0: the clock
    };

    // A season file's identity, as its stub in a pak names it.
    using SeasonRef = std::tuple<uint64_t, uint32_t, uint32_t>;  // id, gen, crc

    // ---- season files ------------------------------------------------------------------------------------------------
    struct AdvInfo {
        bool ok = false;
        uint64_t id = 0;
        uint32_t gen = 0, crc = 0, version = 0;
        uint8_t team = 0, week = 0;
        std::string team_name;           // the franchise of the player's team, from the file's own list
    };
    AdvInfo adv_info(const std::vector<uint8_t>& file);  // magic, size, version and CRC checked
    std::string adv_name(uint64_t id, uint32_t gen);     // "season-<ID>-<gen>.adv"

    // Every stub in the folder's pak has a season file with the same id, gen and CRC (what adv_save.cpp loads).
    bool pair_ok(const fs::path& folder, std::string* why = nullptr);

    // ---- export ------------------------------------------------------------------------------------------------------
    struct ExportResult {
        bool ok = false;
        std::string why;                 // when not ok, for the player
        std::vector<uint8_t> zip;
        int paks = 0, seasons = 0;
        std::vector<std::string> entries;  // the zip's file names
    };
    // Builds the bundle and records each exported pak in saves/sync.json (what this device has synced).
    ExportResult make_bundle(const Env& env);
    std::string bundle_file_name(std::time_t when);  // "New Blitz Saves 2026-11-27 14.03.22.nbsave"

    // ---- import ------------------------------------------------------------------------------------------------------
    enum class Verdict {
        Same,      // nothing to do
        New,       // nothing here yet (no pak, or a blank one): theirs goes in
        Newer,     // theirs carries on from what this device has: theirs goes in
        Older,     // this device already went past theirs: kept as is unless the player says otherwise
        Conflict,  // both changed: the player picks
    };
    const char* verdict_name(Verdict v);

    struct FolderPlan {
        std::string folder;              // relative to saves/, "" for saves/ itself, with '/' separators
        std::string label;               // "Advanced (blitz2026)", "Standard", "Original 1999"
        std::string pack;                // "" when none
        Verdict verdict = Verdict::Same;
        std::string why;                 // a word for the log: which rule decided
        std::string theirs, mine;        // "LAR, week 6", "No saves"
        std::string theirs_when, mine_when;  // when each pak was last saved ("2026-11-27 14:03"), "" when unknown
        std::vector<uint8_t> pak;        // theirs
        std::map<std::string, std::vector<uint8_t>> adv;  // theirs, by file name
        std::vector<std::string> seen;   // the pak states their device had synced (sha256), carried into sync.json
        bool take = false;               // set by the screens (New and Newer start true)
    };

    struct Plan {
        bool ok = false;
        std::string why;                 // when not ok, for the player
        std::string device, made, version;
        std::vector<FolderPlan> folders;
        std::map<std::string, std::string> settings;  // "rules.json" -> contents (only those that differ from ours)
    };
    Plan read_bundle(const Env& env, const std::vector<uint8_t>& zip);

    // What a bundle holds, for a list (Cloud Sync's slots): its device label, when it was made, and per folder "label:
    // what" ("Advanced (blitz2026): LAR, week 6"). ok false when it isn't a bundle (nothing is checked beyond that).
    struct Summary {
        bool ok = false;
        std::string device, made;
        std::vector<std::string> folders;
    };
    Summary summarize(const std::vector<uint8_t>& zip);

    // Writes the chosen folders (take) into their sync_incoming/ and the settings into <app>/sync_incoming/settings/.
    // Nothing outside those folders changes. False with why when something couldn't be written (what was staged is
    // removed again).
    bool stage(const Env& env, const Plan& plan, bool take_settings, std::string& why);
    // The folders stage() wrote into (absolute), for the caller to apply.
    std::vector<fs::path> staged_folders(const Env& env, const Plan& plan);

    // Puts a staged import in place in `folder`: each season file (one already there with other bytes is kept in
    // pak_history/ first), then the pak through `write_pak`, which keeps the pak as it is in pak_history/ and writes
    // the new image (controller_pak.cpp: the restore mark path). The staging goes once the pak is written.
    enum class Applied { Nothing, Done, Failed };
    using PakWriter = std::function<bool(const fs::path& pak_path, const std::vector<uint8_t>& image)>;
    Applied apply_staged(const fs::path& folder, const PakWriter& write_pak, std::string* log = nullptr);
    bool has_staged(const fs::path& folder);
    // A plain writer, for tests and tools: the pak as it is into pak_history/ (a dated copy), then the new image.
    bool write_pak_with_history(const fs::path& pak_path, const std::vector<uint8_t>& image);

    // Season files a restored pak points at that an import replaced: put back from pak_history/ (the current one
    // goes there in turn). For Controller Pak Backups, after it restores a pak. Returns how many.
    int restore_replaced_seasons(const fs::path& folder, const std::vector<uint8_t>& image);

    // At start, before the settings are read: rules.json and season.json from <app>/sync_incoming/settings/. The ones
    // they replace are kept as <name>.before-sync. Returns how many were put in place.
    int apply_staged_settings(const fs::path& app);

    // Records an import's pak states in saves/sync.json (each taken folder: theirs and what their device had seen).
    void record_import(const Env& env, const Plan& plan);

    // This device's generated label ("macOS 7KQ3"): the platform and four random characters, made once and kept in
    // saves/sync.json. Never the device's own name.
    std::string device_label(const Env& env);

    // What a bundle made now would carry, as one hash: each pak and season file make_bundle would add (by its path in
    // the bundle and its SHA-256), settings left out. bundle_fingerprint gives the same hash for a bundle's own files,
    // so Cloud Sync (save_sync_cloud.h) can tell "nothing changed since the last sync" without making a bundle, and
    // "this device holds exactly what the cloud holds" after a pull. "" when there are no saves.
    std::string saves_fingerprint(const Env& env);
    std::string bundle_fingerprint(const std::vector<uint8_t>& zip);
    // A folder under saves/ with an import staged in it (sync_incoming/), absolute paths.
    std::vector<fs::path> folders_with_staging(const Env& env);
    // The absolute folder of a plan's folder name ("" is saves/ itself).
    fs::path folder_dir(const Env& env, const std::string& folder);

    // A whole file, or empty; and a file written via a synced temporary and a rename.
    std::vector<uint8_t> read_file(const fs::path& p, bool* ok = nullptr);
    bool write_file(const fs::path& p, const std::vector<uint8_t>& data);
    std::string sha256(const std::vector<uint8_t>& data);
}
