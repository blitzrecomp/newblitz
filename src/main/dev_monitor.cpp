// Thread/process monitor for development: with BLITZ_MONITOR=<seconds>, print every game thread's
// priority, libultra state and the message queue it is blocked on, plus Blitz's process list, at
// that interval. Read-only snapshots taken from another host thread, so values can be a frame stale.
#include <filesystem>
#include "librecomp/game.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "blitz_hooks.h"

extern "C" uint32_t blitz_thread_stopped_by(int32_t t_, int32_t* by);

namespace {
    constexpr int num_counters = 8;
    std::atomic<uint32_t> counters[num_counters];
    // Names for the hook counters wired in blitz2000.toml.
    const char* counter_names[num_counters] = {"sc_wake", "sc_retrace", "audio_recv", "audio_retrace", "audio_ai_ok", "audio_paused", "audio_cmds", "audio_empty"};

    uint8_t* rdram_base = nullptr;
    std::mutex threads_mutex;
    std::vector<int32_t> threads;  // N64 addresses of every OSThread the game created
    std::chrono::steady_clock::time_point start;
    const auto process_start = std::chrono::steady_clock::now();  // names live dumps (DUMP command)
    std::atomic<uint32_t> last_gfx_dlist{0}, last_gfx_ucode{0}, last_gfx_size{0};

    // BLITZ_DUMP_RDRAM_AT="31.5,33": at those times since boot, write the first 8 MB of RDRAM (native
    // word order) to build/dumps/rdram_<t>.bin plus a .txt with the last graphics task's display list.
    void dump_rdram(double t) {
        // In the app folder (a dev run's build/appdata*), so it works however the app was opened.
        std::filesystem::path dir = recomp::get_config_path() / "dumps";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string base = (dir / ("rdram_" + std::to_string((int)(t * 10)))).string();
        if (FILE* f = fopen((base + ".bin").c_str(), "wb")) {
            fwrite(rdram_base, 1, 0x800000, f);
            fclose(f);
        }
        if (FILE* f = fopen((base + ".txt").c_str(), "w")) {
            fprintf(f, "t=%.2f dlist=%08X size=%u ucode=%08X\n", t, last_gfx_dlist.load(), last_gfx_size.load(), last_gfx_ucode.load());
            fclose(f);
        }
        fprintf(stderr, "[mon] dumped RDRAM to %s.bin (dlist %08X)\n", base.c_str(), last_gfx_dlist.load());
    }

    // RDRAM is stored as native-endian 32-bit words.
    uint32_t r32(int32_t addr) { return *reinterpret_cast<uint32_t*>(rdram_base + (uint32_t)(addr - (int32_t)0x80000000)); }
    int16_t r16(int32_t addr) { return *reinterpret_cast<int16_t*>(rdram_base + ((uint32_t)(addr - (int32_t)0x80000000) ^ 2)); }
    char r8(int32_t addr) { return (char)rdram_base[(uint32_t)(addr - (int32_t)0x80000000) ^ 3]; }
    bool valid(uint32_t addr) { return addr >= 0x80000400 && addr < 0x80400000; }

    const char* state_name(uint32_t s) {
        switch (s) {
            case 1: return "STOPPED";
            case 2: return "RUNNABLE";
            case 4: return "RUNNING";
            case 8: return "WAITING";
            default: return "?";
        }
    }

    std::string describe_thread(int32_t t_) {
        uint8_t* rdram = rdram_base;  // TO_PTR expects a local named rdram
        OSThread* t = TO_PTR(OSThread, t_);
        char buf[200];
        uint32_t state = blitz_libultra_thread_state(rdram_base, t_);
        int n = snprintf(buf, sizeof(buf), "thread %08X id=%-5d pri=%-3d %-8s", (uint32_t)t_, t->id, t->priority, state_name(state));
        if (state == 8 && t->queue != NULLPTR) {
            int32_t mq = t->queue;  // address of the queue's blocked_on_recv/send field = the queue itself (or +4)
            OSMesgQueue* q = TO_PTR(OSMesgQueue, mq);
            snprintf(buf + n, sizeof(buf) - n, " on mq %08X (msgs %d/%d)", (uint32_t)mq, q->validCount, q->msgCount);
        }
        int32_t by = 0;
        uint32_t ra = blitz_thread_stopped_by(t_, &by);
        if (state == 1 && ra != 0) {
            snprintf(buf + n, sizeof(buf) - n, " stopped by thread %08X (id %d) at ra %08X", (uint32_t)by,
                     TO_PTR(OSThread, by)->id, ra);
        }
        return buf;
    }

    // Game heaps (heap_init 0x800024C4): +4 free list, +8 used list; block: +8 next, +0x10 size.
    void heap_stats(const char* name, uint32_t heap_ptr_addr) {
        uint32_t heap = r32((int32_t)heap_ptr_addr);
        if (!valid(heap)) {
            return;
        }
        auto walk = [&](uint32_t head, uint32_t& total, uint32_t& count, uint32_t& largest) {
            total = count = largest = 0;
            for (uint32_t b = head; valid(b) && count < 100000; b = r32((int32_t)b + 8)) {
                uint32_t sz = r32((int32_t)b + 0x10);
                total += sz;
                count++;
                largest = sz > largest ? sz : largest;
            }
        };
        uint32_t ft, fc, fl, ut, uc, ul;
        walk(r32((int32_t)heap + 4), ft, fc, fl);
        walk(r32((int32_t)heap + 8), ut, uc, ul);
        fprintf(stderr, "[mon]   heap %-7s %08X used %7u in %4u blocks (largest %6u)  free %7u in %3u (largest %6u)\n",
                name, heap, ut, uc, ul, ft, fc, fl);
        // Once a heap is nearly full, dump its used blocks once: size, tag words, first data words.
        static std::vector<uint32_t> dumped;
        if (fl < 4096 && std::find(dumped.begin(), dumped.end(), heap) == dumped.end()) {
            dumped.push_back(heap);
            int n = 0;
            for (uint32_t b = r32((int32_t)heap + 8); valid(b) && n < 400; b = r32((int32_t)b + 8), n++) {
                fprintf(stderr, "[heap] %08X size %6u tag %08X %08X data %08X %08X %08X %08X\n", b, r32((int32_t)b + 0x10),
                        r32((int32_t)b + 0x14), r32((int32_t)b + 0x18), r32((int32_t)b + 0x20), r32((int32_t)b + 0x24),
                        r32((int32_t)b + 0x28), r32((int32_t)b + 0x2C));
            }
        }
    }

    void dump(double t) {
        fprintf(stderr, "[mon %.1fs] ---", t);
        for (int i = 0; i < num_counters; i++) {
            if (counters[i]) {
                fprintf(stderr, " %s=%u", counter_names[i], counters[i].load());
            }
        }
        fprintf(stderr, "\n");
        static const bool show_threads = getenv("BLITZ_MONITOR_THREADS") != nullptr;
        if (show_threads) {
            std::lock_guard lock{threads_mutex};
            for (int32_t th : threads) {
                fprintf(stderr, "[mon]   %s\n", describe_thread(th).c_str());
            }
        }
        heap_stats("default", 0x8005E33C);
        heap_stats("DF50", 0x8005DF50);
        heap_stats("DF3C", 0x8005DF3C);
        heap_stats("DF00", 0x8005DF00);
        uint32_t syn = r32((int32_t)0x8002B664);
        fprintf(stderr, "[mon]   n_syn=%08X head=%08X  frame sizes 8003A680..8C: %d %d %d %d  syn+0x1C/0x20: %d %d\n", syn,
                valid(syn) ? r32((int32_t)syn) : 0, (int)r32((int32_t)0x8003A680), (int)r32((int32_t)0x8003A684),
                (int)r32((int32_t)0x8003A688), (int)r32((int32_t)0x8003A68C),
                valid(syn) ? (int)r32((int32_t)syn + 0x1C) : 0, valid(syn) ? (int)r32((int32_t)syn + 0x20) : 0);
        // Process list: head at 0x802A9B80, current at 0x802A9B88 (see NOTES "Process system").
        uint32_t cur = r32((int32_t)0x802A9B88);
        uint32_t p = r32((int32_t)0x802A9B80);
        for (int i = 0; valid(p) && i < 64; i++, p = r32((int32_t)p)) {
            char name[17] = {};
            for (int k = 0; k < 16; k++) {
                char c = r8((int32_t)p + 0x1C + k);
                name[k] = (c >= 32 && c < 127) ? c : 0;
                if (!c) break;
            }
            fprintf(stderr, "[mon]   proc %08X%s name='%s' sleep=%d basepri=%d [+18]=%08X  %s\n", p, p == cur ? "*" : " ",
                name, r16((int32_t)p + 0xC), (int)r32((int32_t)p + 0x2C), r32((int32_t)p + 0x18), describe_thread((int32_t)p + 0x30).c_str());
        }
    }
}

namespace blitz {
    // Live "DUMP" command (dev_input.cpp): dump RDRAM now, stamped with seconds since start.
    void dev_dump_rdram() {
        if (rdram_base == nullptr) {
            return;
        }
        dump_rdram(std::chrono::duration<double>(std::chrono::steady_clock::now() - process_start).count());
    }

    uint8_t* monitor_rdram() {  // hints.cpp: what the game shows
        return rdram_base;
    }

    void monitor_set_rdram(uint8_t* rdram) {
        rdram_base = rdram;
        const char* env = getenv("BLITZ_MONITOR");
        if (env == nullptr && getenv("BLITZ_DUMP_RDRAM_AT") == nullptr) {
            return;
        }
        double interval = env ? atof(env) : 1e9;
        if (interval <= 0) {
            interval = 2;
        }
        start = std::chrono::steady_clock::now();
        if (const char* at = getenv("BLITZ_DUMP_RDRAM_AT")) {
            std::vector<double> times;
            for (const char* p = at; *p;) {
                times.push_back(atof(p));
                while (*p && *p != ',') p++;
                if (*p) p++;
            }
            std::thread([times] {
                for (double t : times) {
                    std::this_thread::sleep_until(start + std::chrono::duration<double>(t));
                    dump_rdram(t);
                }
            }).detach();
        }
        std::thread([interval] {
            while (true) {
                std::this_thread::sleep_for(std::chrono::duration<double>(interval));
                dump(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            }
        }).detach();
    }

    // Called for every thread the game creates; kept only while the thread list is being shown
    // (BLITZ_MONITOR_THREADS), and each OSThread address once (the game reuses them).
    void monitor_register_thread(const OSThread* t) {
        static const bool wanted = getenv("BLITZ_MONITOR") != nullptr && getenv("BLITZ_MONITOR_THREADS") != nullptr;
        if (!wanted || rdram_base == nullptr) {
            return;
        }
        int32_t addr = (int32_t)((const uint8_t*)t - rdram_base) + (int32_t)0x80000000;
        std::lock_guard lock{threads_mutex};
        if (std::find(threads.begin(), threads.end(), addr) == threads.end()) {
            threads.push_back(addr);
        }
    }
}

extern "C" void blitz_count(int id) {
    if (id >= 0 && id < num_counters) {
        counters[id]++;
    }
}

extern "C" void blitz_log_add_player(uint8_t* rdram, int32_t player) {
    if (rdram_base == nullptr) {
        rdram_base = rdram;
    }
    uint32_t syn = r32((int32_t)0x8002B664);
    fprintf(stderr, "[audio] n_alSynAddPlayer(player %08X) n_syn=%08X head=%08X\n", (uint32_t)player, syn,
            valid(syn) ? r32((int32_t)syn) : 0);
}

extern "C" void blitz_on_sp_task(uint8_t* rdram, int32_t task) {
    if (rdram_base == nullptr) {
        rdram_base = rdram;
    }
    if (r32(task) == 1) {  // M_GFXTASK
        last_gfx_ucode = r32(task + 0x10);
        last_gfx_dlist = r32(task + 0x30);
        last_gfx_size = r32(task + 0x34);
        // Frame display-list high-water mark (the game's buffer holds 0x5DC0 bytes; gfx_ext.cpp adds
        // commands to it), logged at each new 1 KB step.
        static uint32_t high_water = 0;
        uint32_t size = last_gfx_size;
        if (size / 1024 > high_water / 1024) {
            fprintf(stderr, "[gfx] display list high-water %u bytes (%.0f%% of 0x5DC0)\n", size, size * 100.0 / 0x5DC0);
        }
        high_water = std::max(high_water, size);
    }
}
