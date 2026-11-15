// Messages for the player from game-thread code (Controller Pak, content packs). They are queued
// here and shown by the main thread (main.cpp update_gfx), because SDL's message box must not be
// opened from the game thread. Each message is also logged.
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace {
    std::mutex notice_mutex;
    std::string pending_notice;
    std::string pending_status;
    std::string last_status;
    unsigned status_serial = 0;
}

namespace blitz {
    void post_notice(const std::string& msg) {
        fprintf(stderr, "[notice] %s\n", msg.c_str());
        std::lock_guard lock{notice_mutex};
        if (pending_notice.find(msg) != std::string::npos) return;  // already waiting to be shown
        pending_notice = pending_notice.empty() ? msg : pending_notice + "\n\n" + msg;
    }

    std::string take_notice() {
        std::lock_guard lock{notice_mutex};
        return std::exchange(pending_notice, std::string{});
    }

    // A short status ("State saved") shown for a moment as a toast (toast.cpp), without a dialog.
    void post_status(const std::string& msg) {
        fprintf(stderr, "[status] %s\n", msg.c_str());
        std::lock_guard lock{notice_mutex};
        pending_status = msg;
        last_status = msg;
        status_serial++;
    }

    // A status that stays up (toast.cpp) until release_status(): a load the game takes a while over (team, RG DS).
    std::atomic<int> hold_pending{0};  // 1 hold the next toast, 2 let it go (main.cpp)
    void post_status_held(const std::string& msg) {
        post_status(msg);
        hold_pending = 1;
    }
    void release_status() { hold_pending = 2; }
    int take_status_hold() { return hold_pending.exchange(0); }

    // The latest status and a counter that changes with each one (phone controllers show it too).
    unsigned latest_status(std::string& out) {
        std::lock_guard lock{notice_mutex};
        out = last_status;
        return status_serial;
    }

    std::string take_status() {
        std::lock_guard lock{notice_mutex};
        return std::exchange(pending_status, std::string{});
    }
}
