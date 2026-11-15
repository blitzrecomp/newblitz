// Host services: what each platform's own file provides (src/main/macos_support.mm, platform_windows.cpp,
// platform_linux.cpp, platform_ios.mm, platform_android.cpp, platform_ps5.cpp). The rest of the app calls these and
// nothing OS-specific; what a platform has at all is include/blitz_platform.h.
#ifndef BLITZ_HOST_H
#define BLITZ_HOST_H

#include <filesystem>
#include <string>

struct SDL_Window;

namespace blitz {
    // First thing in main(), before anything reads a file: Linux points BLITZ_RESOURCES at the executable's folder
    // (the frontend would otherwise look in the working folder). Elsewhere nothing.
    void early_init();
    // Before SDL starts: macOS's press-and-hold accent picker off (a held key repeats). Elsewhere nothing.
    void disable_press_and_hold();
    // The app menu (macOS: Cmd+Q, Cmd+H, Cmd+M). Elsewhere nothing.
    void install_app_menu(const std::string& app_name);
    // The Cocoa window's number, for macOS screenshots (capture.cpp). Elsewhere 0.
    int window_number(void* native_window);
    // A blocking HTTP GET over the system's own TLS (call it off the main thread): true with the body when the server
    // answers 200. Rosters at launch (rosters_live.cpp), headshots (crew.cpp).
    bool http_get(const std::string& url, const std::string& bearer, const std::string& accept, std::string& body, int& status);
    // Where a ROM, pack or add-on put on the device by hand is picked up (BLITZ_HAS_ROM_DROP_FOLDER): iOS's Documents
    // (Files app, Finder), Android's app files folder (adb, file managers), the PS5's /data/blitzrecomp (FTP).
    // Empty on a computer, which has file dialogs.
    std::filesystem::path rom_drop_folder();
    // Once the game's window exists (main.cpp): Android follows the app's window to and from the background.
    // Elsewhere nothing.
    void window_created(SDL_Window* window);
}

#endif
