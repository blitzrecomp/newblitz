// Controller Pak backups (src/game/controller_pak.cpp): the dated copies in each pak folder's pak_history/, for
// Settings → General → Controller Pak Backups (src/main/pak_backups.cpp).
#pragma once
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace blitz {
    struct PakBackup {
        std::filesystem::path file;  // the copy, in pak_history/
        std::filesystem::path pak;   // the pak it goes back to
        std::time_t when = 0;
        std::string folder;          // "Advanced", "Standard", "Original 1999"; empty when only one folder has copies
        std::string what;            // "LAR, week 6", "No seasons", ...
    };

    // Newest first; copies the same as their pak now are left out.
    std::vector<PakBackup> pak_backups();
    bool pak_backups_exist();
    // "" when restored (or, with needs_restart, marked for the next start); else why not.
    std::string pak_restore(const PakBackup& backup, bool& needs_restart);
}
