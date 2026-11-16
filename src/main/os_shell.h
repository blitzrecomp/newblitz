// The desktop's file browser and Downloads folder, per platform (os_shell.cpp).
#pragma once
#include <filesystem>

namespace blitz {
    std::filesystem::path downloads_folder();
    // The user's Documents (Reset Data's backups): Windows' Known Folder, $HOME/Documents on a Mac, Linux's XDG
    // documents folder (else ~/Documents). BLITZ_DOCUMENTS_DIR overrides it (tests). Empty on a phone or console.
    std::filesystem::path documents_folder();
    void open_in_file_browser(const std::filesystem::path& dir);
    void reveal_in_file_browser(const std::filesystem::path& file);
}
