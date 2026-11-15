// Creator tools: screenshots and clips of the game window, saved to ~/Pictures/New Blitz (Windows: Pictures\\
// New Blitz, screenshots only; clips are the Xbox Game Bar's. The PS5: the controller's Create button).
//   F12 (or a controller's capture button: Xbox Share, Switch Capture, DualSense mic)  screenshot
//   Shift+F12                                      start or stop a clip (a .mov of the window's area)
//   F5 / F7 / F6 / F8                              save / load a state, next slot, Redo Play (savestate.cpp)
// macOS's screencapture does the capturing. The first time, macOS asks to allow screen recording
// for the app (System Settings → Privacy & Security → Screen & System Audio Recording).
#include "librecomp/game.hpp"
#include "blitz_host.h"
#include "blitz_platform.h"
#include <chrono>
#include <csignal>
#include <thread>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef BLITZ_MACOS
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define SDL_MAIN_HANDLED
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"

#ifdef BLITZ_MACOS
extern char** environ;
#endif
extern SDL_Window* window;  // main.cpp
extern "C" void blitz_savestate_request(int op);  // savestate.cpp
extern "C" void blitz_savestate_slot_step(int delta);
extern "C" void blitz_redo_request();

// Trailer footage (tools/capture): with BLITZ_CAPTURE_EVENTS=<file>, one JSON line per moment worth a clip (every play's
// end from play_log.cpp, every screenshot as a bookmark), stamped with the Unix time in ms, so tools/capture/cut.py can cut
// them out of a recording made alongside (tools/capture/record.swift writes the recording's start time the same way).
#include <mutex>
extern "C" void blitz_capture_event(const char* fields) {
    static const char* path = getenv("BLITZ_CAPTURE_EVENTS");
    if (path == nullptr || *path == 0) return;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (FILE* f = fopen(path, "a")) {
        fprintf(f, "{\"t\": %lld, %s}\n", ms, fields);
        fclose(f);
    }
}

namespace blitz {
    void dev_dump_rdram();  // dev_monitor.cpp
    void post_status(const std::string& msg);  // notices.cpp
    bool replay_event(const SDL_Event& e);     // replay_screen.cpp: its keys and buttons, while it's up
}

#if defined(_WIN32)
// Windows: the window's area copied from the screen (what's showing, D3D12 or Vulkan alike) into a PNG.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace {
    int window_id() { return 0; }

    std::filesystem::path folder() {
        std::filesystem::path p;
        PWSTR pictures = nullptr;
        // BRANDING: screenshot folder name (Pictures\New Blitz)
        if (SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures) == S_OK) p = std::filesystem::path(pictures) / "New Blitz";
        CoTaskMemFree(pictures);
        if (p.empty()) p = recomp::get_config_path() / "screenshots";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
    }

    std::string stamp() {
        time_t now = time(nullptr);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d at %H.%M.%S", localtime(&now));
        return buf;
    }

    // The window's client area as RGB rows; false when there's no window to copy.
    bool grab(std::vector<unsigned char>& rgb, int& w, int& h) {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (window == nullptr || !SDL_GetWindowWMInfo(window, &info)) return false;
        HWND hwnd = info.info.win.window;
        RECT r;
        if (!GetClientRect(hwnd, &r)) return false;
        w = r.right - r.left;
        h = r.bottom - r.top;
        if (w <= 0 || h <= 0) return false;
        POINT origin{0, 0};
        ClientToScreen(hwnd, &origin);
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down rows
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        bool ok = bmp != nullptr;
        if (ok) {
            HGDIOBJ old = SelectObject(mem, bmp);
            ok = BitBlt(mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT) != 0;
            SelectObject(mem, old);
        }
        if (ok) {
            rgb.resize((size_t)w * h * 3);
            const unsigned char* px = (const unsigned char*)bits;
            for (size_t i = 0, n = (size_t)w * h; i < n; i++) {  // BGRA to RGB
                rgb[i * 3 + 0] = px[i * 4 + 2];
                rgb[i * 3 + 1] = px[i * 4 + 1];
                rgb[i * 3 + 2] = px[i * 4 + 0];
            }
        }
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return ok;
    }

    // Grabs now (on the calling thread, the main one) and writes the PNG in the background.
    bool save_png(const std::filesystem::path& path) {
        auto rgb = std::make_shared<std::vector<unsigned char>>();
        int w = 0, h = 0;
        if (!grab(*rgb, w, h)) return false;
        std::thread([rgb, w, h, path] {
            stbi_write_png(path.string().c_str(), w, h, 3, rgb->data(), w * 3);
        }).detach();
        return true;
    }
}

namespace blitz {
    void capture_screenshot() {
        std::filesystem::path path = folder() / ("Blitz " + stamp() + ".png");
        if (!save_png(path)) return;
        // BRANDING: screenshot folder name in a toast
        post_status("Screenshot saved to Pictures\\New Blitz");
        fprintf(stderr, "[capture] screenshot %s\n", path.string().c_str());
    }
    void capture_clip_toggle() { post_status("Clips: use the Xbox Game Bar (Windows key + Alt + R)"); }
    bool clip_recording() { return false; }
}
#elif !defined(BLITZ_MACOS)
// Phones and the PS5 capture the screen themselves (the Create button, the side buttons); save states and Redo Play
// below work. (Linux: not yet.)
namespace {
    int window_id() { return 0; }
}

namespace blitz {
#if defined(BLITZ_IOS)
    void capture_screenshot() { post_status("Screenshots: press the side and volume-up buttons"); }
    void capture_clip_toggle() { post_status("Clips: Screen Recording in Control Center"); }
#elif defined(__ANDROID__)
    void host_screenshot();  // platform_android.cpp: to Pictures/New Blitz, the gallery's
    void capture_screenshot() { host_screenshot(); }
    void capture_clip_toggle() { post_status("Clips: Screen record in Quick Settings"); }
#elif defined(BLITZ_PLATFORM_LINUX)
    void capture_screenshot() { post_status("Screenshots: use your desktop's screenshot key"); }
    void capture_clip_toggle() { post_status("Clips: use your desktop's screen recorder"); }
#else
    void capture_screenshot() { post_status("Screenshots: use the controller's Create button"); }
    void capture_clip_toggle() { post_status("Clips: use the controller's Create button"); }
#endif
    bool clip_recording() { return false; }
}
#else
namespace {
    pid_t clip_pid = 0;

    std::filesystem::path folder() {
        const char* home = getenv("HOME");
        // BRANDING: screenshot folder name (~/Pictures/New Blitz)
        std::filesystem::path p = std::filesystem::path(home ? home : "/tmp") / "Pictures" / "New Blitz";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
    }

    std::string stamp() {
        time_t now = time(nullptr);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d at %H.%M.%S", localtime(&now));
        return buf;
    }

    pid_t run(const std::vector<std::string>& args) {
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        pid_t pid = 0;
        if (posix_spawn(&pid, "/usr/sbin/screencapture", nullptr, nullptr, argv.data(), environ) != 0) return 0;
        return pid;
    }

    int window_id() {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (window == nullptr || !SDL_GetWindowWMInfo(window, &info)) return 0;
        return blitz::window_number(info.info.cocoa.window);
    }
}

namespace blitz {
    void capture_screenshot() {
        int id = window_id();
        if (id == 0) return;
        std::string path = (folder() / ("Blitz " + stamp() + ".png")).string();
        pid_t pid = run({"screencapture", "-o", "-l" + std::to_string(id), path});
        if (pid) std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
        fprintf(stderr, "[capture] screenshot %s\n", path.c_str());
        blitz_capture_event("\"kind\": \"mark\"");
    }

    void capture_clip_toggle() {
        if (clip_pid) {
            kill(clip_pid, SIGINT);  // screencapture saves the movie and exits
            pid_t pid = clip_pid;
            clip_pid = 0;
            std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
            fprintf(stderr, "[capture] clip stopped\n");
            return;
        }
        if (window == nullptr) return;
        int x, y, w, h;
        SDL_GetWindowPosition(window, &x, &y);
        SDL_GetWindowSize(window, &w, &h);
        std::string path = (folder() / ("Blitz " + stamp() + ".mov")).string();
        std::string rect = std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(w) + "," + std::to_string(h);
        clip_pid = run({"screencapture", "-v", "-R" + rect, path});
        fprintf(stderr, "[capture] clip recording to %s\n", path.c_str());
    }

    bool clip_recording() { return clip_pid != 0; }
}
#endif

namespace blitz {

    // Main thread, for every SDL event (main.cpp's event filter).
    void capture_event(const SDL_Event& e) {
        if (replay_event(e)) return;
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F12) {
            if ((e.key.keysym.mod & KMOD_CTRL) && (e.key.keysym.mod & KMOD_SHIFT)) {
                // Bug report: a few frames of the window and an RDRAM dump, marked in the log (the magenta flicker).
                fprintf(stderr, "[report] Ctrl+Shift+F12: capturing for a bug report\n");
                blitz::dev_dump_rdram();
                // Three frames a third of a second apart (a flicker alternates), into the app folder's dumps.
                int id = window_id();
#ifdef BLITZ_MACOS
                if (id != 0) {
                    std::filesystem::path dir = recomp::get_config_path() / "dumps";
                    std::string base = (dir / ("report " + stamp())).string();
                    std::thread([id, base] {
                        for (int i = 1; i <= 3; i++) {
                            pid_t pid = run({"screencapture", "-o", "-x", "-l" + std::to_string(id), base + " " + std::to_string(i) + ".png"});
                            int st;
                            if (pid) waitpid(pid, &st, 0);
                            std::this_thread::sleep_for(std::chrono::milliseconds(330));
                        }
                    }).detach();
                }
#elif defined(_WIN32)
                (void)id;
                std::filesystem::path dir = recomp::get_config_path() / "dumps";
                std::string base = "report " + stamp();
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                for (int i = 1; i <= 3; i++) {  // a flicker alternates: three frames a third of a second apart
                    save_png(dir / (base + " " + std::to_string(i) + ".png"));
                    if (i < 3) SDL_Delay(330);
                }
#else
                (void)id;
#endif
                blitz::post_status("Bug report: saved to the dumps folder");
            }
            else if (e.key.keysym.mod & KMOD_SHIFT) capture_clip_toggle();
            else capture_screenshot();
        }
        // Save states: F5 saves, F7 loads, F6 picks the next slot (Shift+F6 the previous).
        if (e.type == SDL_KEYDOWN && !e.key.repeat && (e.key.keysym.scancode == SDL_SCANCODE_F5 || e.key.keysym.scancode == SDL_SCANCODE_F7)) {
            blitz_savestate_request(e.key.keysym.scancode == SDL_SCANCODE_F5 ? 1 : 2);
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F8) {
            blitz_redo_request();  // Redo Play
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F6) {
            blitz_savestate_slot_step((e.key.keysym.mod & KMOD_SHIFT) ? -1 : 1);
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_MISC1) {
            capture_screenshot();  // SDL's MISC1: Xbox Share, Switch Capture, DualSense mic button
        }
    }

    void capture_shutdown() {
        if (clip_recording()) capture_clip_toggle();
    }
}
