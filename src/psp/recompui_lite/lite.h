// recompui-lite's own calls (the rest is the frontend's signatures, platform/psp/include).
#pragma once
#include <filesystem>

namespace recompui_lite {
    // The app folder (configs, saves, the ROM, logs) and the program folder (assets). Before anything reads a file.
    void set_paths(const std::filesystem::path& app_folder, const std::filesystem::path& program_folder);
}
