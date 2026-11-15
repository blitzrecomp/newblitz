// One app, two games (docs/design/ONE_APP.md). BlitzRecomp.app holds a starter (src/starter/starter.c) and both
// engines, Contents/MacOS/Classic and Contents/MacOS/Modern, each with its own resources (Contents/Resources/<Engine>)
// and its own app folder, saves and stored ROM, exactly as the separate apps had. This file is each engine's side:
//   - which engine this is, and whether the other one is in the bundle beside it;
//   - whether the other game has a ROM stored;
//   - a ROM picked in the wrong game is handed to the other engine (`--import-rom … --exit-after-import`) and the
//     player is offered the switch;
//   - switching: remember the choice (the shared `experience` file the starter reads), quit, and exec the other.
// Built separately (today's two apps, dev runs) nothing here shows: there's no sibling engine.
// Windows: the two games' folders side by side (BlitzRecomp\\Classic\\Classic Blitz.exe, BlitzRecomp\\Modern\\Modern Blitz.exe;
// `make win`); the PS5: the two titles (PPSA99960 Classic, PPSA99961 Modern; platform/ps5/ps5_stage.cmake), switching
// by asking the system to start the other.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#elif defined(__PROSPERO__)
#include <cstdint>
#else
#if defined(BLITZ_MACOS)
#include <mach-o/dyld.h>
#endif
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "SDL2/SDL.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include "blitz_config.h"

#ifdef BLITZ_MACOS
extern char** environ;
#endif
#ifdef __PROSPERO__
extern "C" {
    struct LaunchAppParam {
        uint32_t size;
        int32_t user_id;
        uint32_t app_opt;
        uint64_t crash_report;
        uint32_t check_flag;
    };
    int sceUserServiceGetForegroundUser(int32_t*);
    int sceSystemServiceLaunchApp(const char*, const char**, LaunchAppParam*);
}
#endif

namespace {
    namespace fs = std::filesystem;

#if BLITZ_MODERN
    constexpr const char* self_name = "Modern";
    constexpr const char* other_name = "Classic";
    // BRANDING: the other engine's shown name
    constexpr const char* other_title = "Classic Blitz";
    constexpr const char* other_program_id = "BlitzRecompiled";
    constexpr const char* other_game_file = "blitz2000.n64.us.1.1.z64";
    [[maybe_unused]] constexpr const char* other_ps5_title = "PPSA99960";
#else
    constexpr const char* self_name = "Classic";
    constexpr const char* other_name = "Modern";
    // BRANDING: the other engine's shown name
    constexpr const char* other_title = "Modern Blitz";
    constexpr const char* other_program_id = "Blitz2026Recompiled";
    constexpr const char* other_game_file = "blitz2000.teamplus.n64.us.z64";
    [[maybe_unused]] constexpr const char* other_ps5_title = "PPSA99961";
#endif

#ifdef _WIN32
    fs::path local_app_data() {
        fs::path p;
        PWSTR known = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known) == S_OK) p = known;
        CoTaskMemFree(known);
        return p;
    }
#endif

    fs::path exe_dir() {
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0) return {};
        return fs::path(buf).parent_path();
#elif !defined(BLITZ_MACOS)
        return {};
#else
        char buf[4096];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) != 0) return {};
        std::error_code ec;
        fs::path p = fs::weakly_canonical(fs::path(buf), ec);
        return p.parent_path();
#endif
    }

    fs::path sibling() {
#ifdef _WIN32
        // Only in the combined folder: this engine in "Classic" or "Modern", the other beside it.
        fs::path dir = exe_dir();
        if (dir.filename() != self_name) return {};
        fs::path s = dir.parent_path() / other_name / (std::string(other_title) + ".exe");
        std::error_code ec;
        return fs::exists(s, ec) ? s : fs::path{};
#elif defined(__PROSPERO__)
        // The other title, installed beside this one.
        fs::path s = fs::path("/data/homebrew") / other_ps5_title / "eboot.bin";
        std::error_code ec;
        return fs::exists(s, ec) ? s : fs::path{};
#elif !defined(BLITZ_MACOS)
        return {};
#else
        fs::path dir = exe_dir();
        // Only inside BlitzRecomp.app (this engine named Classic or Modern): today's apps are both "BlitzRecompiled".
        char buf[4096];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) != 0 || fs::path(buf).filename() != self_name) return {};
        fs::path s = dir / other_name;
        std::error_code ec;
        return fs::exists(s, ec) ? s : fs::path{};
#endif
    }

    std::string env_key(const char* engine) {
        return std::string("BLITZ_APP_FOLDER_") + (std::string(engine) == "Classic" ? "CLASSIC" : "MODERN");
    }

    // The other game's app folder: a dev override, else its Application Support folder.
    fs::path other_folder() {
        if (const char* f = getenv(env_key(other_name).c_str())) return f;
#ifdef _WIN32
        return local_app_data() / other_program_id;
#elif defined(__PROSPERO__)
        return fs::path("/data/blitzrecomp") / other_program_id;
#elif defined(__linux__)  // Linux, Android: the frontend's ~/.config/<program id>
        const char* config_home = getenv("HOME");
        return fs::path(config_home ? config_home : "/tmp") / ".config" / other_program_id;
#endif
        const char* home = getenv("HOME");
        return fs::path(home ? home : "/tmp") / "Library" / "Application Support" / other_program_id;
    }

    // BRANDING: "BlitzRecomp" folder names below (user-visible)
    fs::path experience_file() {
        if (const char* d = getenv("BLITZ_EXPERIENCE_DIR")) return fs::path(d) / "experience";
#ifdef _WIN32
        return local_app_data() / "BlitzRecomp" / "experience";
#elif defined(__PROSPERO__)
        return "/data/blitzrecomp/experience";
#elif defined(__linux__)
        const char* config_home = getenv("HOME");
        return fs::path(config_home ? config_home : "/tmp") / ".config" / "BlitzRecomp" / "experience";
#endif
        const char* home = getenv("HOME");
        return fs::path(home ? home : "/tmp") / "Library" / "Application Support" / "BlitzRecomp" / "experience";
    }

    void remember(const char* engine) {
        std::error_code ec;
        fs::create_directories(experience_file().parent_path(), ec);
        std::ofstream f{experience_file(), std::ios::trunc};
        f << engine << "\n";
    }

    bool switch_requested = false;
}

namespace blitz {
    void post_status(const std::string& msg);  // notices.cpp
}

namespace blitz {
    bool other_engine_available() { return !sibling().empty(); }
    const char* other_engine_title() { return other_title; }

    // Both games play the one USA v1.1 ROM: this bundle's Modern is the original-game engine (Resources/Modern/
    // engine-original), not the Team Extension's.
    bool engines_share_rom() {
        if (!other_engine_available()) return false;
#if BLITZ_ENGINE_ORIGINAL
        return true;
#elif BLITZ_MODERN
        return false;
#elif defined(BLITZ_MACOS)
        std::error_code ec;
        return fs::exists(exe_dir().parent_path() / "Resources" / "Modern" / "engine-original", ec);
#else
        return true;  // Windows' and the PS5's Modern is always the original-game engine (`make win-modern`, ps5-modern)
#endif
    }

    bool other_engine_has_rom() {
        std::error_code ec;
        if (!other_engine_available()) return false;
        if (fs::exists(other_folder() / other_game_file, ec)) return true;
#if !BLITZ_MODERN
        // Modern on the original game plays this game's ROM and takes ours when it starts (experience_started).
        if (engines_share_rom() && (fs::exists(other_folder() / "blitz2000.n64.us.1.1.z64", ec) ||
                                    fs::exists(recomp::get_config_path() / "blitz2000.n64.us.1.1.z64", ec)))
            return true;
#endif
        return false;
    }

    // At startup: this game is the one to open next time. When both games play the one v1.1 ROM (this bundle's Modern
    // is the original-game engine), a game with none of its own takes the one the other stored.
    void experience_started() {
        if (engines_share_rom()) {
            std::error_code ec;
            const char* file = "blitz2000.n64.us.1.1.z64";
            fs::path own = recomp::get_config_path() / file, theirs = other_folder() / file;
            if (!fs::exists(own, ec) && fs::exists(theirs, ec)) {
                fs::create_directories(own.parent_path(), ec);
                fs::copy_file(theirs, own, ec);
                fprintf(stderr, "[experience] %s's ROM %s\n", other_name, ec ? ("not taken: " + ec.message()).c_str() : "taken");
            }
        }
        if (other_engine_available()) remember(self_name);
    }

    // The launcher's Switch to …: remember it, then quit; main execs the other engine (experience_exec).
    void experience_switch() {
        if (!other_engine_available()) return;
#ifdef __PROSPERO__
        // The PS5: ask the system to start the other title first; if it won't, this one keeps running.
        LaunchAppParam param{};
        param.size = sizeof(param);
        int32_t user = -1;
        sceUserServiceGetForegroundUser(&user);
        param.user_id = user;
        const char* args[] = {nullptr};
        int rc = sceSystemServiceLaunchApp(other_ps5_title, args, &param);
        fprintf(stderr, "[experience] starting %s (%s): 0x%08x\n", other_name, other_ps5_title, (unsigned)rc);
        if (rc < 0) {
            post_status(std::string("The console wouldn't start ") + other_title);
            return;
        }
#endif
        remember(other_name);
        switch_requested = true;
        ultramodern::quit();
    }

    // After the game loop returns (main.cpp): if a switch was asked for, become the other engine.
    void experience_exec(char** argv) {
        (void)argv;
#ifdef _WIN32
        if (!switch_requested) return;
        fs::path s = sibling();
        if (s.empty()) return;
        // The other engine finds its resources beside its .exe (main.cpp), unless it inherits ours.
        _wputenv_s(L"BLITZ_RESOURCES", s.parent_path().wstring().c_str());
        _wputenv_s(L"BLITZ_WAIT_PID", std::to_wstring(GetCurrentProcessId()).c_str());  // it starts once this one has ended
        std::wstring cmd = L"\"" + s.wstring() + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        fprintf(stderr, "[experience] switching to %s\n", other_name);
        if (CreateProcessW(s.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, s.parent_path().wstring().c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            std::exit(0);
        }
        fprintf(stderr, "[experience] CreateProcess failed: %lu\n", GetLastError());
        return;
#elif !defined(BLITZ_MACOS)
        return;
#else
        if (!switch_requested) return;
        fs::path s = sibling();
        if (s.empty()) return;
        // Its resources and (dev) its app folder.
        fs::path res = exe_dir().parent_path() / "Resources" / other_name;
        setenv("BLITZ_RESOURCES", res.string().c_str(), 1);
        if (const char* f = getenv(env_key(other_name).c_str())) setenv("APP_FOLDER_PATH", f, 1);
        else unsetenv("APP_FOLDER_PATH");
        std::string exe = s.string();
        std::vector<char*> args{exe.data(), nullptr};
        fprintf(stderr, "[experience] switching to %s\n", other_name);
        execv(exe.c_str(), args.data());
        fprintf(stderr, "[experience] execv failed: %s\n", strerror(errno));
#endif
    }

    // The other game took the ROM: say so and offer the switch.
    [[maybe_unused]] static bool offer_switch() {
        std::string msg = std::string("That's a ") + other_title + " ROM. It's stored for " + other_title + ", which you can switch to now or any time from the launcher.";
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Switch"},
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Not now"},
        };
        std::string title = std::string("Stored for ") + other_title;
        SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, nullptr, title.c_str(), msg.c_str(), 2, buttons, nullptr};
        int choice = 0;
        if (getenv("BLITZ_AUTO_SWITCH")) choice = 1;  // dev: no dialog
        else SDL_ShowMessageBox(&box, &choice);
        if (choice == 1) experience_switch();
        return true;
    }

    // Load ROM / Change ROM picked the other game's ROM: store it there (the other engine's own import, which also
    // unpacks zips) and offer the switch. Returns true when the other game took it.
    bool experience_hand_over(const std::filesystem::path& rom) {
#ifdef _WIN32
        fs::path s = sibling();
        if (s.empty()) return false;
        // The other engine's own import, with its resources, while this one waits.
        std::wstring cmd = L"\"" + s.wstring() + L"\" --import-rom \"" + rom.wstring() + L"\" --exit-after-import";
        wchar_t ours[32768];
        DWORD had = GetEnvironmentVariableW(L"BLITZ_RESOURCES", ours, 32768);
        SetEnvironmentVariableW(L"BLITZ_RESOURCES", s.parent_path().wstring().c_str());
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL started = CreateProcessW(s.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                      s.parent_path().wstring().c_str(), &si, &pi);
        SetEnvironmentVariableW(L"BLITZ_RESOURCES", had ? ours : nullptr);
        if (!started) return false;
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        bool ok = code == 0;
        fprintf(stderr, "[experience] handed %s to %s: %s\n", rom.string().c_str(), other_name, ok ? "stored" : "not taken");
        if (!ok) return false;
        return offer_switch();
#elif !defined(BLITZ_MACOS)
        (void)rom;
        return false;  // the PS5 has no file picker: both games import the ROM in /data/blitzrecomp themselves
#else
        fs::path s = sibling();
        if (s.empty()) return false;
        std::string exe = s.string(), path = rom.string(), flag1 = "--import-rom", flag2 = "--exit-after-import";
        std::vector<char*> args{exe.data(), flag1.data(), path.data(), flag2.data(), nullptr};
        // The other engine's resources and app folder for that run.
        std::string res = "BLITZ_RESOURCES=" + (exe_dir().parent_path() / "Resources" / other_name).string();
        std::vector<std::string> env_strings;
        for (char** e = environ; *e; e++) {
            std::string v = *e;
            if (v.rfind("BLITZ_RESOURCES=", 0) == 0 || v.rfind("APP_FOLDER_PATH=", 0) == 0) continue;
            env_strings.push_back(v);
        }
        env_strings.push_back(res);
        if (const char* f = getenv(env_key(other_name).c_str())) env_strings.push_back(std::string("APP_FOLDER_PATH=") + f);
        std::vector<char*> envp;
        for (auto& v : env_strings) envp.push_back(v.data());
        envp.push_back(nullptr);
        pid_t pid = 0;
        if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, args.data(), envp.data()) != 0) return false;
        int status = 0;
        waitpid(pid, &status, 0);
        bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        fprintf(stderr, "[experience] handed %s to %s: %s\n", path.c_str(), other_name, ok ? "stored" : "not taken");
        if (!ok) return false;
        return offer_switch();
#endif
    }
}
