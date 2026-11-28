// Restart in place (1.0.6, 2026-11-27: "try to not make it a real relaunch... 'soft reload' inside the app"). The
// runtime ends the game's threads, keeps its own (VI, graphics, RSP, timers, saving) with the window, RT64 and the UI, and
// boots the game again on cleared RDRAM, the way an N64's reset button starts it over (n64modernruntime patch 0026,
// recomp::request_soft_reset). Between the two, with nothing of the game running, every hook registered here runs: what
// the app keeps per boot (a latch taken at the first boot, files added to the ROM image, a content pack read once) goes
// back to how a fresh start finds it, so the next boot reads the settings and files again.
//
// Where it's used: phones always (they can't start themselves again), and a computer with BLITZ_SOFT_RESTART=1 (a dev
// switch, so the same code path is tested on the Mac; a computer's own Restart relaunches the process). See main.cpp.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "librecomp/game.hpp"
#include "blitz_soft_reset.h"

namespace blitz {
    int soft_reset_count() { return recomp::soft_reset_count(); }

    namespace {
        struct Hook {
            std::string name;
            std::function<void()> fn;
        };
        std::vector<Hook>& hooks() {
            static std::vector<Hook> list;  // filled by static initializers
            return list;
        }
        std::mutex& hooks_mutex() {
            static std::mutex m;
            return m;
        }
    }

    // Registered at static initialization: `[[maybe_unused]] const bool r = blitz::on_soft_reset("name", [] {...});`
    bool on_soft_reset(const char* name, std::function<void()> fn) {
        std::lock_guard lock{hooks_mutex()};
        hooks().push_back({name, std::move(fn)});
        return true;
    }

    // The boot thread, with the game's threads gone (recomp.cpp soft_reset).
    void soft_reset_run_hooks() {
        std::lock_guard lock{hooks_mutex()};
        std::string names;
        for (const Hook& h : hooks()) {
            h.fn();
            names += (names.empty() ? "" : ", ") + h.name;
        }
        fprintf(stderr, "[reset] app state reset: %s\n", names.c_str());
        // Where /proc has it (Android, Linux): the process's threads and memory at each reset, with the game stopped, so a
        // leak shows as a climb from one reset to the next (a phone's census mid-game counts the play's processes too).
        std::error_code ec;
        int threads = 0;
        for (auto it = std::filesystem::directory_iterator("/proc/self/task", ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) threads++;
        long rss_kb = -1;
        if (FILE* f = fopen("/proc/self/status", "r")) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                if (strncmp(line, "VmRSS:", 6) == 0) rss_kb = atol(line + 6);
            }
            fclose(f);
        }
        if (threads > 0) fprintf(stderr, "[reset] process now: %d threads, %ld MB resident\n", threads, rss_kb / 1024);
    }
}
