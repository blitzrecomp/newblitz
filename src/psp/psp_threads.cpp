// The PSP's host_thread backend (ULTRAMODERN_HOST_THREAD_PLATFORM; docs/PSP_PORT.md §4.1, §14.10 "As built (M5)").
//
// sceKernelCreateThread takes a stack size, not a stack, so a "static stack pool" is a pool of kernel threads made at boot:
//   process pool  the game's processes (osCreateThread at priority 20, func_8027427C): N slots of one size, each a kernel
//                 thread that waits on its own semaphore, runs one body, and goes back to the pool. The game makes and ends
//                 processes all the time (21,058 in 300,000 VIs on psp-host), so no kernel thread or stack is made or freed
//                 while it plays: no fragmentation, and the memory is taken once, at boot, where [mem] can count it.
//                 M4 counted at most 71 processes alive at once; the default is 96 slots of 24 KB (the switch file
//                 <app>/stackpool, "<slots> <KB>", changes it without a rebuild).
//   dedicated     everything else (runtime threads, the fixed game threads, the game start thread): one kernel thread
//                 each, with the stack host_thread's attributes ask for. They are made once per run.
//   fallback      a process when every slot is busy: a dedicated thread of the slot size, one "WARN stackpool exhausted"
//                 line, never fatal.
// Every stack is filled with a pattern from its bottom to just below where the thread starts, so its high-water mark is
// the first word that changed (§4.1). A pool slot's mark is the most any of its processes used.
// Thread-locals (emulated TLS) persist in a pooled kernel thread from one process to the next; ultramodern sets its own
// (thread_self, is_game_thread) at every thread start.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <pspkernel.h>
#include <pspthreadman.h>

#include "ultramodern/host_thread.hpp"
#include "psp_log.h"
#include "psp_sys.h"
#include "host/host_backends.h"

#ifndef PSP_THREAD_ATTR_LOW_MEM_STACK
#define PSP_THREAD_ATTR_LOW_MEM_STACK 0x00400000   // not in pspsdk's enum; the firmware's value (psp_sys.cpp)
#endif

extern "C" void blitz_tls_thread_exit(void);   // psp_tls.cpp

namespace {
    constexpr uint32_t stack_pattern = 0xA5C3E1F7u;
    constexpr uint32_t stack_skip = 16;    // the kernel's own words at the very bottom of a stack are left alone

    struct Slot {
        SceUID thid = -1;
        SceUID go = -1, done = -1;         // go: a body is ready (pool); done: the body has returned (join waits)
        bool pooled = false;
        uint32_t stack_bytes = 0;
        uintptr_t stack_lo = 0;            // the stack's low end (filled at the thread's start)
        uint32_t high_water = 0;           // bytes used at most
        std::function<void()>* body = nullptr;
        ultramodern::HostThreadKind kind = ultramodern::HostThreadKind::Runtime;
        int game_id = 0, game_pri = 0;
        char name[32] = {};
        bool busy = false, finished = false, detached = false, ended = false;
        int uses = 0;
    };

    pspsys::Mutex* g_mutex = nullptr;
    std::vector<Slot*> g_all;                       // every slot ever made (records are kept)
    std::vector<Slot*> g_free;                      // idle pool slots
    std::unordered_map<SceUID, Slot*> g_by_thid;
    host::StackSizes g_sizes;
    int g_pool_slots = 0, g_pool_busy = 0, g_pool_max_busy = 0, g_fallbacks = 0;
    uint32_t g_pool_bytes = 0;
    int g_created = 0, g_alive = 0, g_max_alive = 0, g_alive_game = 0, g_max_alive_game = 0;

    pspsys::Mutex& lock_obj() {
        if (g_mutex == nullptr) g_mutex = new pspsys::Mutex();   // first use is at boot, before any of these threads
        return *g_mutex;
    }

    uint32_t measure(const Slot* s) {
        if (s->stack_lo == 0) return 0;
        const uint32_t* p = reinterpret_cast<const uint32_t*>(s->stack_lo + stack_skip);
        const uint32_t* end = reinterpret_cast<const uint32_t*>(s->stack_lo + s->stack_bytes);
        while (p < end && *p == stack_pattern) p++;
        return (uint32_t)(reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(p));
    }

    // At the thread's start: find the stack and fill it below the current frame.
    void fill_stack(Slot* s) {
        SceKernelThreadInfo ti;
        memset(&ti, 0, sizeof ti);
        ti.size = sizeof ti;
        if (sceKernelReferThreadStatus(sceKernelGetThreadId(), &ti) < 0 || ti.stack == nullptr) return;
        uintptr_t lo = (uintptr_t)ti.stack;
        uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
        uintptr_t top = (sp - 512) & ~(uintptr_t)3;
        for (uint32_t* p = reinterpret_cast<uint32_t*>(lo + stack_skip); (uintptr_t)p < top; p++) *p = stack_pattern;
        s->stack_bytes = (uint32_t)ti.stackSize;
        s->stack_lo = lo;
    }

    void run_body(Slot* s) {
        std::function<void()>* body = s->body;
        s->body = nullptr;
        try {
            (*body)();
        } catch (const std::exception& e) {
            PERR("thread", "%s: uncaught exception: %s", s->name, e.what());
        } catch (...) {
            PERR("thread", "%s: uncaught exception", s->name);
        }
        delete body;
        uint32_t used = measure(s);
        pspsys::Lock l(lock_obj());
        s->high_water = std::max(s->high_water, used);
    }

    void release_pooled_locked(Slot* s) {
        s->busy = false;
        s->finished = false;
        s->detached = false;
        g_pool_busy--;
        g_free.push_back(s);
    }

    // The bookkeeping when a body has returned (the lock is held).
    void count_end_locked(Slot* s) {
        g_alive--;
        if (s->kind == ultramodern::HostThreadKind::Game) g_alive_game--;
    }

    int pool_entry(SceSize, void* argp) {
        if (pspsys::thread_cwd()[0]) sceIoChdir(pspsys::thread_cwd());   // PPSSPP: host0: needs a cwd per thread (§14.3)
        Slot* s = *static_cast<Slot**>(argp);
        fill_stack(s);
        for (;;) {
            sceKernelWaitSema(s->go, 1, nullptr);
            run_body(s);
            pspsys::Lock l(lock_obj());
            count_end_locked(s);
            if (s->detached) {
                release_pooled_locked(s);
            } else {
                s->finished = true;
                sceKernelSignalSema(s->done, 1);
            }
        }
        return 0;
    }

    int dedicated_entry(SceSize, void* argp) {
        if (pspsys::thread_cwd()[0]) sceIoChdir(pspsys::thread_cwd());
        Slot* s = *static_cast<Slot**>(argp);
        fill_stack(s);
        run_body(s);
        blitz_tls_thread_exit();   // its thread_locals (psp_tls.cpp): a later thread may get the same kernel id
        bool self_delete;
        {
            pspsys::Lock l(lock_obj());
            count_end_locked(s);
            s->finished = true;
            self_delete = s->detached;
            if (self_delete) s->ended = true;
            else sceKernelSignalSema(s->done, 1);
        }
        if (self_delete) sceKernelExitDeleteThread(0);
        return 0;
    }

    uint32_t stack_for(const ultramodern::host_thread_attrs& a) {
        if (a.stack_bytes != 0) return a.stack_bytes;
        switch (a.kind) {
            case ultramodern::HostThreadKind::GameStart: return g_sizes.game_start;
            case ultramodern::HostThreadKind::Game:
                return host::is_fixed_game_thread(a.game_id, a.game_pri) ? g_sizes.game_fixed : g_sizes.game_process;
            default: return g_sizes.runtime;
        }
    }

    [[noreturn]] void fatal(const char* what, const char* name, int rc) {
        PERR("thread", "%s for %s failed: 0x%08X", what, name, (unsigned)rc);
        psplog::flush();
        sceKernelExitGame();
        for (;;) sceKernelSleepThread();
    }

    Slot* make_dedicated(const char* name, uint32_t stack, int prio, bool vfpu) {
        Slot* s = new Slot();
        s->done = sceKernelCreateSema("thread_done", 0, 0, 1, nullptr);
        s->stack_bytes = stack;
        snprintf(s->name, sizeof s->name, "%s", name);
        SceUID id = sceKernelCreateThread(name, dedicated_entry, prio, (int)stack,
                                          PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_LOW_MEM_STACK | (vfpu ? PSP_THREAD_ATTR_VFPU : 0), nullptr);
        if (id < 0) fatal("sceKernelCreateThread", name, id);
        s->thid = id;
        return s;
    }

    void name_for(Slot* s, const ultramodern::host_thread_attrs& attrs) {
        if (attrs.kind == ultramodern::HostThreadKind::Game) {
            snprintf(s->name, sizeof s->name, "game %d pri %d", attrs.game_id, attrs.game_pri);
        } else {
            snprintf(s->name, sizeof s->name, "%s", attrs.name);
        }
    }
}

struct ultramodern::host_thread_handle {
    Slot* slot;
};

ultramodern::host_thread_handle* ultramodern::host_thread_create(const host_thread_attrs& attrs, std::function<void()>* body) {
    Slot* s = nullptr;
    bool process = attrs.kind == HostThreadKind::Game && !host::is_fixed_game_thread(attrs.game_id, attrs.game_pri) && attrs.stack_bytes == 0;
    bool fallback = false;
    {
        pspsys::Lock l(lock_obj());
        if (process && !g_free.empty()) {
            s = g_free.back();
            g_free.pop_back();
            g_pool_busy++;
            g_pool_max_busy = std::max(g_pool_max_busy, g_pool_busy);
        } else if (process) {
            fallback = true;
            g_fallbacks++;
        }
    }
    if (fallback && g_fallbacks == 1) {
        PWARN("thread", "stackpool exhausted (%d slots busy): processes get their own %u KB kernel thread from now on", g_pool_slots,
              (unsigned)(g_pool_bytes ? g_pool_bytes : g_sizes.game_process) / 1024);
    }
    if (s == nullptr) {
        uint32_t stack = process ? (g_pool_bytes ? g_pool_bytes : g_sizes.game_process) : stack_for(attrs);
        char name[32];
        if (attrs.kind == HostThreadKind::Game) snprintf(name, sizeof name, "game %d", attrs.game_id);
        else snprintf(name, sizeof name, "%s", attrs.name);
        // The Gfx thread runs the GE renderer (psp_gfx.cpp, pspgu): it gets the VFPU context, as any thread that may use it
        // must (a VFPU instruction without THREAD_ATTR_VFPU faults). No other thread uses the VFPU.
        bool vfpu = attrs.kind == HostThreadKind::Runtime && strcmp(attrs.name, "Gfx") == 0;
        s = make_dedicated(name, stack, attrs.priority > 0 ? attrs.priority : 0x20, vfpu);
    }
    {
        pspsys::Lock l(lock_obj());
        s->body = body;
        s->kind = attrs.kind;
        s->game_id = attrs.game_id;
        s->game_pri = attrs.game_pri;
        s->busy = true;
        s->finished = s->detached = false;
        s->uses++;
        name_for(s, attrs);
        if (!s->pooled) {
            g_all.push_back(s);
            g_by_thid[s->thid] = s;
        }
        g_created++;
        g_alive++;
        g_max_alive = std::max(g_max_alive, g_alive);
        if (attrs.kind == HostThreadKind::Game) {
            g_alive_game++;
            g_max_alive_game = std::max(g_max_alive_game, g_alive_game);
        }
    }
    if (s->pooled) {
        sceKernelSignalSema(s->go, 1);
    } else {
        Slot* arg = s;
        int rc = sceKernelStartThread(s->thid, sizeof arg, &arg);
        if (rc < 0) fatal("sceKernelStartThread", s->name, rc);
    }
    return new host_thread_handle{ s };
}

void ultramodern::host_thread_join(host_thread_handle* handle) {
    if (handle == nullptr) return;
    Slot* s = handle->slot;
    delete handle;
    sceKernelWaitSema(s->done, 1, nullptr);
    if (s->pooled) {
        pspsys::Lock l(lock_obj());
        release_pooled_locked(s);
        return;
    }
    sceKernelWaitThreadEnd(s->thid, nullptr);
    sceKernelDeleteThread(s->thid);
    pspsys::Lock l(lock_obj());
    s->ended = true;
}

void ultramodern::host_thread_detach(host_thread_handle* handle) {
    if (handle == nullptr) return;
    Slot* s = handle->slot;
    delete handle;
    bool reap = false;
    {
        pspsys::Lock l(lock_obj());
        if (!s->finished) {
            s->detached = true;            // the thread releases (pool) or deletes (dedicated) itself when its body returns
        } else if (s->pooled) {
            sceKernelPollSema(s->done, 1);
            release_pooled_locked(s);
        } else {
            reap = true;
        }
    }
    if (reap) {                            // a dedicated thread that already returned: it is dormant, delete it here
        sceKernelPollSema(s->done, 1);
        sceKernelWaitThreadEnd(s->thid, nullptr);
        sceKernelDeleteThread(s->thid);
        pspsys::Lock l(lock_obj());
        s->ended = true;
    }
}

// ultramodern's set_native_thread_name (patch 0021): the name its report shows for this thread.
extern "C" void blitz_psp_thread_name(const char* name) {
    pspsys::Lock l(lock_obj());
    auto it = g_by_thid.find(sceKernelGetThreadId());
    if (it != g_by_thid.end()) snprintf(it->second->name, sizeof it->second->name, "%s", name);
}

namespace host {
    void set_stack_sizes(const StackSizes& sizes) { g_sizes = sizes; }
    const StackSizes& stack_sizes() { return g_sizes; }

    // The same rule as psp-host (host_backends.cpp): every process ran at priority 20 in M4; the fixed threads at 5, 6,
    // 10, 80, 81, 90 and 100.
    bool is_fixed_game_thread(int id, int pri) {
        (void)id;
        return pri != 20;
    }

    void init_threads(int slots, uint32_t slot_bytes) {
        g_pool_bytes = slot_bytes;
        g_sizes.game_process = slot_bytes;
        for (int i = 0; i < slots; i++) {
            Slot* s = new Slot();
            s->pooled = true;
            s->stack_bytes = slot_bytes;
            s->go = sceKernelCreateSema("pool_go", 0, 0, 1, nullptr);
            s->done = sceKernelCreateSema("pool_done", 0, 0, 1, nullptr);
            char name[32];
            snprintf(name, sizeof name, "proc %02d", i);
            snprintf(s->name, sizeof s->name, "%s (idle)", name);
            SceUID id = sceKernelCreateThread(name, pool_entry, 0x20, (int)slot_bytes, PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_LOW_MEM_STACK, nullptr);
            if (id < 0) {
                PERR("thread", "process pool: slot %d of %d failed (0x%08X); the pool has %d slots", i, slots, (unsigned)id, i);
                sceKernelDeleteSema(s->go);
                sceKernelDeleteSema(s->done);
                delete s;
                break;
            }
            s->thid = id;
            Slot* arg = s;
            sceKernelStartThread(id, sizeof arg, &arg);   // it fills its stack, then waits for a body
            pspsys::Lock l(lock_obj());
            g_all.push_back(s);
            g_by_thid[id] = s;
            g_free.push_back(s);
            g_pool_slots++;
        }
        // the slots run their fill at priority 0x20 before the caller (also 0x20) goes on: let them
        sceKernelDelayThread(1000);
        PLOG("thread", "process pool: %d slots of %u KB (%u KB in all)", g_pool_slots, (unsigned)slot_bytes / 1024,
             (unsigned)(g_pool_slots * slot_bytes / 1024));
    }

    PoolStats pool_stats() {
        pspsys::Lock l(lock_obj());
        PoolStats p{ g_pool_slots, g_pool_busy, g_pool_max_busy, g_fallbacks, g_pool_bytes, 0 };
        for (Slot* s : g_all) {
            if (s->pooled) p.worst_used = std::max(p.worst_used, std::max(s->high_water, measure(s)));
        }
        return p;
    }

    ThreadCounts thread_counts() {
        pspsys::Lock l(lock_obj());
        return { g_created, g_alive, g_max_alive, g_alive_game, g_max_alive_game };
    }

    void stack_report(bool all) {
        struct Row { std::string name; uint32_t used, size; bool alive, pooled; };
        std::vector<Row> rows;
        {
            pspsys::Lock l(lock_obj());
            for (Slot* s : g_all) {
                if (!s->ended && s->stack_lo != 0) s->high_water = std::max(s->high_water, measure(s));
                rows.push_back({ s->pooled ? "process pool" : s->name, s->high_water, s->stack_bytes, !s->ended, s->pooled });
            }
        }
        // One line per distinct name (the pool is one line: its worst slot).
        std::vector<Row> worst;
        for (const Row& r : rows) {
            auto it = std::find_if(worst.begin(), worst.end(), [&](const Row& w) { return w.name == r.name; });
            if (it == worst.end()) worst.push_back(r);
            else if (r.used > it->used) it->used = r.used;
        }
        for (const Row& r : worst) {
            int pct = (int)(r.used * 100ull / (r.size ? r.size : 1));
            if (all || pct >= 50)
                PLOG("stack", "%-22s %6u of %6u bytes (%d%%)%s", r.name.c_str(), (unsigned)r.used, (unsigned)r.size, pct, r.alive ? "" : " ended");
        }
        ThreadCounts c = thread_counts();
        PoolStats p = pool_stats();
        PLOG("stack", "threads: %d started, %d alive (most %d); game threads alive %d (most %d); pool %d of %d busy (most %d), %d fallbacks",
             c.created, c.alive, c.max_alive, c.alive_game, c.max_alive_game, p.busy, p.slots, p.max_busy, p.fallbacks);
    }

    // From the marks already taken (a pool slot's at each body's end, the others' at the last stack_report): no stack is
    // scanned here, so the [perf] line can ask every 5 s.
    int stack_high(std::string& name) {
        pspsys::Lock l(lock_obj());
        int best = 0;
        for (Slot* s : g_all) {
            if (s->stack_bytes == 0) continue;
            int pct = (int)(s->high_water * 100ull / s->stack_bytes);
            if (pct > best || name.empty()) { best = pct; name = s->pooled ? "pool" : s->name; }
        }
        return best;
    }

    int suspend_thread_named(const char* name) {
        std::vector<SceUID> ids(256);
        int count = 0, n = 0;
        if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids.data(), 256, &count) < 0) return 0;
        for (int i = 0; i < count && i < 256; i++) {
            SceKernelThreadInfo ti;
            memset(&ti, 0, sizeof ti);
            ti.size = sizeof ti;
            if (sceKernelReferThreadStatus(ids[i], &ti) < 0) continue;
            if (!strcmp(ti.name, name) && sceKernelSuspendThread(ids[i]) >= 0) n++;
        }
        return n;
    }

    std::string thread_dump() {
        std::vector<SceUID> ids(256);
        int count = 0;
        std::string out;
        if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids.data(), 256, &count) < 0) return "thread list unavailable\n";
        char line[200];
        snprintf(line, sizeof line, "%d kernel threads (status 1 run 2 ready 4 wait 8 suspend 16 dormant)\n", count);
        out += line;
        int idle = 0;
        for (int i = 0; i < count && i < 256; i++) {
            SceKernelThreadInfo ti;
            memset(&ti, 0, sizeof ti);
            ti.size = sizeof ti;
            if (sceKernelReferThreadStatus(ids[i], &ti) < 0) continue;
            const char* ours = nullptr;
            uint32_t used = 0;
            bool pool_idle = false;
            {
                pspsys::Lock l(lock_obj());
                auto it = g_by_thid.find(ids[i]);
                if (it != g_by_thid.end()) {
                    Slot* s = it->second;
                    ours = s->name;
                    used = std::max(s->high_water, measure(s));
                    pool_idle = s->pooled && !s->busy;
                }
            }
            if (pool_idle) { idle++; continue; }   // idle pool slots: one count below
            if (ours == nullptr) {
                int free_bytes = sceKernelGetThreadStackFreeSize(ids[i]);   // the kernel's own fill (threads made elsewhere)
                used = free_bytes >= 0 && free_bytes <= ti.stackSize ? (uint32_t)(ti.stackSize - free_bytes) : 0;
            }
            snprintf(line, sizeof line, "  %-26s %-20s st %2d pri 0x%02X wait %d/0x%X stack %6u of %6d\n", ti.name, ours ? ours : "-",
                     ti.status, ti.currentPriority, ti.waitType, (unsigned)ti.waitId, (unsigned)used, ti.stackSize);
            out += line;
        }
        snprintf(line, sizeof line, "  (+ %d idle process pool slots)\n", idle);
        out += line;
        return out;
    }
}
