// The desktop's file browser and Downloads folder, per platform (os_shell.cpp).
#pragma once
#include <filesystem>

namespace blitz {
    std::filesystem::path downloads_folder();
    void open_in_file_browser(const std::filesystem::path& dir);
    void reveal_in_file_browser(const std::filesystem::path& file);
}
