// Host services: what each platform's own file provides (src/main/macos_support.mm, platform_windows.cpp,
// platform_linux.cpp, platform_ios.mm, platform_android.cpp, platform_ps5.cpp). The rest of the app calls these and
// nothing OS-specific; what a platform has at all is include/blitz_platform.h.
#ifndef BLITZ_HOST_H
#define BLITZ_HOST_H

#include <filesystem>
#include <string>

struct SDL_Window;

#ifdef __ANDROID__
struct ANativeWindow;  // <android/native_window.h>
#endif

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
    // A blocking HTTP POST of a body (call it off the main thread): true when the server answers 2xx. Crash reports
    // (crash_report.cpp). The PS5 has none (false).
    bool http_post(const std::string& url, const std::string& content_type, const std::string& body, int& status);
    // One small secret kept by name in the platform's secure storage (Cloud Sync's code, save_sync.cpp): the Keychain on
    // macOS and iOS, the Android Keystore (an AES key that never leaves it), DPAPI on Windows, the Secret Service
    // (libsecret, when the desktop has it) on Linux. False when there's none or it failed: the caller then keeps a file
    // only this user can read. secure_kind says which, for the log and the docs. The PS5 has none (false).
    bool secure_put(const std::string& name, const std::string& value);
    bool secure_get(const std::string& name, std::string& value);
    void secure_erase(const std::string& name);
    const char* secure_kind();
    // Where a ROM, pack or add-on put on the device by hand is picked up (BLITZ_HAS_ROM_DROP_FOLDER): iOS's Documents
    // (Files app, Finder), Android's app files folder (adb, file managers), the PS5's /data/blitzrecomp (FTP).
    // Empty on a computer, which has file dialogs.
    std::filesystem::path rom_drop_folder();
    // Once the game's window exists (main.cpp): Android follows the app's window to and from the background.
    // Elsewhere nothing.
    void window_created(SDL_Window* window);
#ifdef __ANDROID__
    // The window the renderer is made on: the game's surface as it is now (held), waiting while there's none
    // (platform_android.cpp). The one create_window returned may have gone with a trip to the background.
    ANativeWindow* render_window(ANativeWindow* from_create_window);
#endif
}

#endif
