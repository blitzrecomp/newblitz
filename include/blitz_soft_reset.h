// Restart in place (src/main/soft_restart.cpp): what the app keeps per boot goes back to how a fresh start finds it.
#ifndef BLITZ_SOFT_RESET_H
#define BLITZ_SOFT_RESET_H

#include <functional>
#include <mutex>
#include <new>

namespace blitz {
    // Registered at static initialization; run on the boot thread with none of the game's threads left, before the next
    // boot: `[[maybe_unused]] const bool r = blitz::on_soft_reset("name", [] { ... });`
    bool on_soft_reset(const char* name, std::function<void()> fn);
    // How many restarts in place this run (recomp::soft_reset_count); 0 before the first.
    int soft_reset_count();

    // A per-boot std::once_flag made new (in a hook: no thread is inside its call_once then).
    inline void reset_once(std::once_flag& flag) {
        flag.~once_flag();
        new (&flag) std::once_flag;
    }
}

#endif
