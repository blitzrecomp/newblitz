// Linux host services (docs/PLATFORMS.md; ROADMAP.md): the functions blitz_host.h declares. HTTP goes through the
// system's curl (every desktop and the Steam Deck have it), with the request headers on its stdin so a token never
// shows in the process list.
#include <filesystem>
#include <string>
#include <vector>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "blitz_host.h"

extern char** environ;

namespace blitz {
    // The resources sit beside the executable (platform/linux/linux_stage.cmake); the frontend reads BLITZ_RESOURCES.
    void early_init() {
        if (getenv("BLITZ_RESOURCES") != nullptr) return;
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) return;
        exe[n] = '\0';
        setenv("BLITZ_RESOURCES", std::filesystem::path(exe).parent_path().c_str(), 1);
    }

    void disable_press_and_hold() {}
    void install_app_menu(const std::string&) {}
    int window_number(void*) { return 0; }
    std::filesystem::path rom_drop_folder() { return {}; }  // a computer has file dialogs
    void window_created(SDL_Window*) {}

    // curl -s -X POST --data-binary @<file> <url>: the body in a temporary file (stdin carries nothing here), the
    // status as curl's whole output (-o /dev/null -w).
    bool http_post(const std::string& url, const std::string& content_type, const std::string& body, int& status) {
        status = 0;
        char tmpl[] = "/tmp/blitz-post-XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) return false;
        (void)!write(fd, body.data(), body.size());
        close(fd);
        std::string data = std::string("@") + tmpl;
        std::vector<std::string> args{"curl", "-s", "--max-time", "30", "-o", "/dev/null", "-w", "%{http_code}", "-X", "POST",
                                      "-H", "Content-Type: " + content_type, "-A", "BlitzRecomp", "--data-binary", data, url};
        std::vector<char*> argv;
        for (std::string& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        int out[2];
        if (pipe(out) != 0) {
            unlink(tmpl);
            return false;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, out[1], 1);
        posix_spawn_file_actions_addclose(&actions, out[0]);
        pid_t pid = 0;
        int spawned = posix_spawnp(&pid, "curl", &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(out[1]);
        std::string got;
        if (spawned == 0) {
            char buf[64];
            for (ssize_t n; (n = read(out[0], buf, sizeof(buf))) != 0;) {
                if (n < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                got.append(buf, (size_t)n);
            }
            int wstatus = 0;
            waitpid(pid, &wstatus, 0);
        }
        close(out[0]);
        unlink(tmpl);
        status = std::atoi(got.c_str());
        return status >= 200 && status < 300;
    }

    // curl -s -L -H @- <url>, with -w appending the HTTP status on a last line of its own.
    bool http_get(const std::string& url, const std::string& bearer, const std::string& accept, std::string& body, int& status) {
        body.clear();
        status = 0;
        int in[2], out[2];
        if (pipe(in) != 0) return false;
        if (pipe(out) != 0) {
            close(in[0]);
            close(in[1]);
            return false;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, in[0], 0);
        posix_spawn_file_actions_adddup2(&actions, out[1], 1);
        posix_spawn_file_actions_addclose(&actions, in[1]);
        posix_spawn_file_actions_addclose(&actions, out[0]);
        std::vector<std::string> args{"curl", "-s", "-L", "--max-time", "30", "-H", "@-", "-w", "\n%{http_code}", url};
        std::vector<char*> argv;
        for (std::string& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        pid_t pid = 0;
        int spawned = posix_spawnp(&pid, "curl", &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(in[0]);
        close(out[1]);
        if (spawned != 0) {
            close(in[1]);
            close(out[0]);
            fprintf(stderr, "[http] no curl: %s\n", std::strerror(spawned));
            return false;
        }
        // BRANDING: HTTP User-Agent
        std::string headers = "User-Agent: BlitzRecomp\n";
        if (!accept.empty()) headers += "Accept: " + accept + "\n";
        if (!bearer.empty()) headers += "Authorization: Bearer " + bearer + "\n";
        signal(SIGPIPE, SIG_IGN);
        (void)!write(in[1], headers.data(), headers.size());
        close(in[1]);
        std::string got;
        char buf[65536];
        for (ssize_t n; (n = read(out[0], buf, sizeof(buf))) != 0;) {
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            got.append(buf, (size_t)n);
        }
        close(out[0]);
        int wstatus = 0;
        waitpid(pid, &wstatus, 0);
        size_t nl = got.rfind('\n');
        if (nl == std::string::npos) return false;
        status = std::atoi(got.c_str() + nl + 1);
        got.resize(nl);
        if (status != 200) return false;
        body = std::move(got);
        return true;
    }
}

// Secure storage (Cloud Sync's code): the Secret Service through libsecret when the desktop has it
// (secure_store_linux.h), else false and save_sync.cpp keeps a 0600 file.
#include "secure_store_linux.h"
namespace blitz {
    bool secure_put(const std::string& name, const std::string& value) { return secure_linux::put(name, value); }
    bool secure_get(const std::string& name, std::string& value) { return secure_linux::get(name, value); }
    void secure_erase(const std::string& name) { secure_linux::erase(name); }
    const char* secure_kind() { return secure_linux::lib().ok ? "the desktop's Secret Service" : "none"; }
}
