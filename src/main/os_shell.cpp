// The desktop's file browser and Downloads folder, per platform: Open Folder (Add-Ons, House Rules, the launcher) and
// Export ROM's "saved in Downloads" (macOS: Finder via `open`; Windows: Explorer; Linux: xdg-open; phones and the PS5:
// none, include/blitz_platform.h's file browser column).
#include <cstdlib>
#include <filesystem>
#include <string>

#include "blitz_platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#elif defined(BLITZ_MACOS) || defined(BLITZ_PLATFORM_LINUX)
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <vector>
extern char** environ;

namespace {
    // /usr/bin/open (Linux: xdg-open) with these arguments, no shell between (a path's quotes or $ can't break it),
    // not waited on.
    void run_open(std::vector<std::string> args) {
#if defined(BLITZ_PLATFORM_LINUX)
        const char* tool = "xdg-open";
        if (!args.empty() && args.front() == "-R") {  // xdg-open can't select a file: its folder instead
            args.erase(args.begin());
            if (!args.empty()) args.front() = std::filesystem::path(args.front()).parent_path().string();
        }
#else
        const char* tool = "/usr/bin/open";
#endif
        args.insert(args.begin(), "open");
        std::vector<char*> argv;
        for (std::string& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        pid_t pid = 0;
        if (posix_spawnp(&pid, tool, nullptr, nullptr, argv.data(), environ) == 0)
            std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
    }
}
#endif

namespace blitz {
    // Where Export ROM saves: the user's Downloads (the PS5: /data/blitzrecomp/exports, reached over FTP).
    std::filesystem::path downloads_folder() {
#ifdef _WIN32
        std::filesystem::path p;
        PWSTR known = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &known) == S_OK) p = known;
        CoTaskMemFree(known);
        return p;
#elif defined(__PROSPERO__)
        // BRANDING: PS5 export folder name
        return "/data/blitzrecomp/exports";
#else
        const char* home = getenv("HOME");
        return std::filesystem::path(home ? home : "/tmp") / "Downloads";
#endif
    }

    // Shows a folder in the file browser.
    void open_in_file_browser(const std::filesystem::path& dir) {
#ifdef _WIN32
        ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif !BLITZ_HAS_FILE_BROWSER
        (void)dir;
#else
        run_open({dir.string()});
#endif
    }

    // Shows a file selected in its folder.
    void reveal_in_file_browser(const std::filesystem::path& file) {
#ifdef _WIN32
        std::wstring args = L"/select,\"" + file.wstring() + L"\"";
        ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
#elif !BLITZ_HAS_FILE_BROWSER
        (void)file;
#else
        run_open({"-R", file.string()});
#endif
    }
}
