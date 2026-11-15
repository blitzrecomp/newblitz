// psp-host only (docs/PSP_PORT.md §2, M4): the Mac's stand-ins for the PSP layer, with the PSP's sizes and limits.
//   host_thread   pthreads on stacks of the PSP's sizes (§4.1), each with a guard page below it and filled with a pattern,
//                 so an overflow faults here as it would crash there, and each thread's high-water mark is known
//   RDRAM         8 MB at the bottom of a 4 GB PROT_NONE reservation (any address outside RDRAM faults)
//   ROM           recomp::rom_provider over the stored ROM file: 48 x 32 KB blocks in LRU (§4.3), counters,
//                 BLITZ_ROM_TRACE=<file> logs every read as "<vi> <offset> <len>"
//   renderer, audio, script input: backends_common.cpp (the same code on the PSP)
//   output        fds 1 and 2 go through a pipe into psp_log (the game's fprintf(stderr) lines land in the ring and sinks)
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <list>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"
#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#include "blitz_audio.h"
#include "psp_log.h"
#include "host/host_backends.h"

using namespace std::chrono;

// ================================================================== threads ==================================================
namespace {
    constexpr uint32_t stack_pattern = 0xA5C3E1F7u;

    struct ThreadRec {
        std::string name;
        ultramodern::HostThreadKind kind;
        int game_id = 0, game_pri = 0;
        uint8_t* map = nullptr;     // guard page + stack
        size_t map_size = 0, guard = 0, stack_size = 0;
        bool alive = true;
        size_t high_water = 0;      // bytes used at most (measured at exit, or by stack_report while alive)
        pthread_t th{};
    };

    std::mutex g_threads_mutex;
    std::list<ThreadRec> g_threads;     // every thread ever started (stacks are freed, records kept)
    int g_alive_game = 0, g_max_alive_game = 0, g_alive_total = 0, g_max_alive_total = 0, g_created = 0;
    host::StackSizes g_sizes;

    size_t page_size() {
        static size_t p = (size_t)sysconf(_SC_PAGESIZE);
        return p;
    }

    size_t measure(const ThreadRec& r) {
        // The stack grows down from map + map_size; the untouched pattern is at the low end.
        const uint32_t* p = reinterpret_cast<const uint32_t*>(r.map + r.guard);
        const uint32_t* end = reinterpret_cast<const uint32_t*>(r.map + r.map_size);
        while (p < end && *p == stack_pattern) p++;
        return (size_t)(reinterpret_cast<const uint8_t*>(end) - reinterpret_cast<const uint8_t*>(p));
    }

    uint32_t stack_for(const ultramodern::host_thread_attrs& a) {
        if (a.stack_bytes != 0) return a.stack_bytes;
        switch (a.kind) {
            case ultramodern::HostThreadKind::GameStart: return g_sizes.game_start;
            case ultramodern::HostThreadKind::Game: return host::is_fixed_game_thread(a.game_id, a.game_pri) ? g_sizes.game_fixed : g_sizes.game_process;
            default: return g_sizes.runtime;
        }
    }

    struct StartArgs {
        std::function<void()>* body;
        ThreadRec* rec;
    };

    void* thread_entry(void* p) {
        StartArgs args = *static_cast<StartArgs*>(p);
        delete static_cast<StartArgs*>(p);
        pthread_setname_np(args.rec->name.c_str());
        try {
            (*args.body)();
        } catch (const std::exception& e) {
            PERR("thread", "%s: uncaught exception: %s", args.rec->name.c_str(), e.what());
        } catch (...) {
            PERR("thread", "%s: uncaught exception", args.rec->name.c_str());
        }
        delete args.body;
        {
            std::lock_guard l(g_threads_mutex);
            ThreadRec* r = args.rec;
            r->high_water = std::max(r->high_water, measure(*r));
            r->alive = false;
            g_alive_total--;
            if (r->kind == ultramodern::HostThreadKind::Game) g_alive_game--;
        }
        return nullptr;
    }

    void free_stack(ThreadRec* r) {
        if (r->map != nullptr) {
            munmap(r->map, r->map_size);
            r->map = nullptr;
        }
    }

}

struct ultramodern::host_thread_handle {
    ThreadRec* rec;
};

ultramodern::host_thread_handle* ultramodern::host_thread_create(const host_thread_attrs& attrs, std::function<void()>* body) {
    const size_t pg = page_size();
    size_t stack = ((size_t)stack_for(attrs) + pg - 1) & ~(pg - 1);
    // The stack is mapped, guarded and filled before its record is published: stack_report (the watchdog thread) measures
    // every live record under the lock, and must never see a half-made one (an unfilled stack reads as 100% used).
    uint8_t* map = (uint8_t*)mmap(nullptr, stack + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == (uint8_t*)MAP_FAILED) {
        PERR("thread", "can't map a %zu KB stack for %s: %s", stack / 1024, attrs.name, strerror(errno));
        psplog::flush();
        _exit(70);
    }
    mprotect(map, pg, PROT_NONE);  // the guard page: an overflow faults here
    for (uint32_t* p = reinterpret_cast<uint32_t*>(map + pg); p < reinterpret_cast<uint32_t*>(map + pg + stack); p++) {
        *p = stack_pattern;
    }
    ThreadRec* rec;
    {
        std::lock_guard l(g_threads_mutex);
        g_threads.emplace_back();
        rec = &g_threads.back();
        rec->kind = attrs.kind;
        rec->game_id = attrs.game_id;
        rec->game_pri = attrs.game_pri;
        if (attrs.kind == HostThreadKind::Game) {
            rec->name = "game " + std::to_string(attrs.game_id) + " pri " + std::to_string(attrs.game_pri);
        } else {
            rec->name = attrs.name;
        }
        g_created++;
        g_alive_total++;
        g_max_alive_total = std::max(g_max_alive_total, g_alive_total);
        if (attrs.kind == HostThreadKind::Game) {
            g_alive_game++;
            g_max_alive_game = std::max(g_max_alive_game, g_alive_game);
        }
        rec->guard = pg;
        rec->stack_size = stack;
        rec->map_size = stack + pg;
        rec->map = map;
    }
    pthread_attr_t pa;
    pthread_attr_init(&pa);
    pthread_attr_setstack(&pa, rec->map + pg, stack);
    auto* args = new StartArgs{ body, rec };
    int rc = pthread_create(&rec->th, &pa, thread_entry, args);
    pthread_attr_destroy(&pa);
    if (rc != 0) {
        PERR("thread", "pthread_create for %s failed: %s", rec->name.c_str(), strerror(rc));
        psplog::flush();
        _exit(70);
    }
    return new host_thread_handle{ rec };
}

void ultramodern::host_thread_join(host_thread_handle* handle) {
    if (handle == nullptr) return;
    pthread_join(handle->rec->th, nullptr);
    {
        std::lock_guard l(g_threads_mutex);
        free_stack(handle->rec);
    }
    delete handle;
}

void ultramodern::host_thread_detach(host_thread_handle* handle) {
    if (handle == nullptr) return;
    // A detached thread's stack is never unmapped: nothing can tell when the thread has fully left it (it still runs its
    // exit path after its record says it ended). Only the timer thread is detached, and it runs for the whole program.
    pthread_detach(handle->rec->th);
    delete handle;
}

namespace host {
    void set_stack_sizes(const StackSizes& sizes) { g_sizes = sizes; }
    const StackSizes& stack_sizes() { return g_sizes; }

    // Fixed game threads (docs/SAVE_STATES.md table): the scheduler, audio, controller poll, worker, dispatcher, idle,
    // main. Every other osCreateThread is a game process (func_8027427C), whose priority is about 20.
    bool is_fixed_game_thread(int id, int pri) {
        (void)id;
        return pri != 20;  // M4: every process ran at 20; the fixed threads at 5, 6, 10, 80, 81, 90 and 100
    }

    ThreadCounts thread_counts() {
        std::lock_guard l(g_threads_mutex);
        return { g_created, g_alive_total, g_max_alive_total, g_alive_game, g_max_alive_game };
    }

    void stack_report(bool all) {
        struct Row { std::string name; size_t used, size; bool alive; ultramodern::HostThreadKind kind; };
        std::vector<Row> rows;
        {
            std::lock_guard l(g_threads_mutex);
            for (ThreadRec& r : g_threads) {
                if (r.alive && r.map != nullptr) r.high_water = std::max(r.high_water, measure(r));
                rows.push_back({ r.name, r.high_water, r.stack_size, r.alive, r.kind });
            }
        }
        // One line per distinct name (game processes reuse ids): the worst use of each.
        std::unordered_map<std::string, Row> worst;
        std::vector<std::string> order;
        for (const Row& r : rows) {
            auto it = worst.find(r.name);
            if (it == worst.end()) {
                worst[r.name] = r;
                order.push_back(r.name);
            } else if (r.used > it->second.used) {
                it->second.used = r.used;
            }
        }
        size_t worst_game = 0, worst_game_size = 1;
        for (const std::string& n : order) {
            const Row& r = worst[n];
            int pct = (int)(r.used * 100 / (r.size ? r.size : 1));
            if (r.kind == ultramodern::HostThreadKind::Game && r.used * worst_game_size > worst_game * r.size) {
                worst_game = r.used;
                worst_game_size = r.size;
            }
            if (all || pct >= 50) PLOG("stack", "%-22s %6zu of %6zu bytes (%d%%)%s", n.c_str(), r.used, r.size, pct, r.alive ? "" : " ended");
        }
        ThreadCounts c = thread_counts();
        PLOG("stack", "threads: %d started, %d alive (most %d); game threads alive %d (most %d)", c.created, c.alive, c.max_alive,
             c.alive_game, c.max_alive_game);
    }
}

// ================================================================== RDRAM ====================================================
namespace {
    uint8_t* g_rdram = nullptr;
}

uint8_t* recomp::platform_rdram(size_t size) {
    // 8 MB of RDRAM at the bottom of 4 GB that is otherwise inaccessible: whatever the address computation, an access
    // outside RDRAM faults (the desktop reserves the same 4 GB).
    constexpr size_t reserve = 4ull << 30;
    void* base = mmap(nullptr, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED || mprotect(base, size, PROT_READ | PROT_WRITE) != 0) {
        PERR("mem", "can't reserve RDRAM: %s", strerror(errno));
        return nullptr;
    }
    g_rdram = static_cast<uint8_t*>(base);
    PLOG("mem", "RDRAM %zu KB at %p (static for the run), the rest of 4 GB inaccessible", size / 1024, base);
    return g_rdram;
}

namespace host {
    uint8_t* rdram() { return g_rdram; }
}

// ================================================================== ROM ======================================================
namespace {
    class FileRomProvider final : public recomp::rom_provider {
    public:
        static constexpr size_t block = 32 * 1024, blocks = 48;

        bool open(const std::string& path) {
            fd_ = ::open(path.c_str(), O_RDONLY);
            if (fd_ < 0) return false;
            off_t end = lseek(fd_, 0, SEEK_END);
            size_ = end > 0 ? (size_t)end : 0;
            cache_.resize(block * blocks);
            tags_.assign(blocks, UINT32_MAX);
            ages_.assign(blocks, 0);
            if (const char* t = getenv("BLITZ_ROM_TRACE")) trace_ = fopen(t, "w");
            return size_ > 0;
        }
        size_t size() const override { return size_; }

        void read(uint32_t rom_offset, uint8_t* dst, size_t len) override {
            std::lock_guard l(mutex_);
            if (trace_ != nullptr) fprintf(trace_, "%llu %u %zu\n", (unsigned long long)host::vi_count(), rom_offset, len);
            while (len > 0) {
                uint32_t b = rom_offset / block;
                size_t in = rom_offset % block, n = std::min(len, block - in);
                const uint8_t* src = lookup(b);
                if (src == nullptr) {
                    std::memset(dst, 0, n);
                } else {
                    std::memcpy(dst, src + in, n);
                }
                dst += n;
                rom_offset += (uint32_t)n;
                len -= n;
            }
        }

        host::RomStats stats() {
            std::lock_guard l(mutex_);
            return stats_;
        }

        // The whole file's XXH3-64 (the Mac can afford it once; the PSP trusts the kit's .sha1 sidecar instead).
        uint64_t hash() {
            std::vector<uint8_t> buf(1 << 20);
            XXH3_state_t* st = XXH3_createState();
            XXH3_64bits_reset(st);
            for (off_t at = 0;;) {
                ssize_t n = pread(fd_, buf.data(), buf.size(), at);
                if (n <= 0) break;
                XXH3_64bits_update(st, buf.data(), (size_t)n);
                at += n;
            }
            uint64_t h = XXH3_64bits_digest(st);
            XXH3_freeState(st);
            return h;
        }

    private:
        const uint8_t* lookup(uint32_t b) {
            tick_++;
            for (size_t i = 0; i < blocks; i++) {
                if (tags_[i] == b) {
                    ages_[i] = tick_;
                    stats_.hits++;
                    return &cache_[i * block];
                }
            }
            if ((size_t)b * block >= size_) return nullptr;
            size_t victim = 0;
            for (size_t i = 1; i < blocks; i++) {
                if (ages_[i] < ages_[victim]) victim = i;
            }
            auto t0 = steady_clock::now();
            ssize_t n = pread(fd_, &cache_[victim * block], block, (off_t)b * block);
            if (n < (ssize_t)block) std::memset(&cache_[victim * block] + (n > 0 ? n : 0), 0, block - (n > 0 ? (size_t)n : 0));
            double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
            stats_.misses++;
            stats_.worst_ms = std::max(stats_.worst_ms, ms);
            tags_[victim] = b;
            ages_[victim] = tick_;
            return &cache_[victim * block];
        }

        int fd_ = -1;
        size_t size_ = 0;
        std::vector<uint8_t> cache_;
        std::vector<uint32_t> tags_;
        std::vector<uint64_t> ages_;
        uint64_t tick_ = 0;
        host::RomStats stats_{};
        std::mutex mutex_;
        FILE* trace_ = nullptr;
    };

    FileRomProvider g_rom;
}

namespace host {
    bool open_rom(const std::string& path, uint64_t expected_xxh3) {
        if (!g_rom.open(path)) {
            PERR("rom", "can't open %s", path.c_str());
            return false;
        }
        uint64_t h = g_rom.hash();
        // A Modern kit ROM (Modern's art merged on the Mac, tools/psp/kit.sh): its sidecar names the base ROM's XXH3, which
        // must be this build's, and the merged file's own.
        if (FILE* sf = fopen((path + ".sha1").c_str(), "r")) {
            char line[200];
            unsigned long long base = 0, merged = 0;
            while (fgets(line, sizeof line, sf)) {
                sscanf(line, "xxh3 %llx", &base);
                sscanf(line, "merged_xxh3 %llx", &merged);
            }
            fclose(sf);
            if (merged != 0 && base == expected_xxh3 && h == merged) {
                PLOG("rom", "%s: a kit ROM (Modern's art merged), %zu bytes, XXH3 ok against its sidecar", path.c_str(), g_rom.size());
                expected_xxh3 = h;
            }
        }
        if (h != expected_xxh3) {
            PERR("rom", "%s: XXH3 %016llX, expected %016llX", path.c_str(), (unsigned long long)h, (unsigned long long)expected_xxh3);
            return false;
        }
        PLOG("rom", "%s: %zu bytes, XXH3 ok; read through a %zu x %zu KB block cache", path.c_str(), g_rom.size(),
             FileRomProvider::blocks, FileRomProvider::block / 1024);
        recomp::set_rom_provider(&g_rom);
        return true;
    }
    RomStats rom_stats() { return g_rom.stats(); }
}

// ================================================================== input =====================================================
// The script only (backends_common.cpp): the Mac has no pad.
namespace host {
    bool get_input(int controller, uint16_t* buttons, float* x, float* y) { return script_input(controller, buttons, x, y); }
}

// ================================================================== output capture ===========================================
namespace {
    void forward_line(const std::string& line) {
        if (line.empty()) return;
        // "[tag] message" keeps its tag; anything else is tagged "out".
        if (line[0] == '[') {
            size_t close = line.find(']');
            if (close != std::string::npos && close < 24 && close + 1 < line.size()) {
                std::string tag = line.substr(1, close - 1);
                size_t body = close + 1;
                while (body < line.size() && line[body] == ' ') body++;
                psplog::log(psplog::INFO, tag.c_str(), "%s", line.c_str() + body);
                return;
            }
        }
        psplog::log(psplog::INFO, "out", "%s", line.c_str());
    }
}

namespace host {
    int capture_output() {
        int saved = dup(1);
        int fds[2];
        if (saved < 0 || pipe(fds) != 0) return -1;
        fflush(stdout);
        fflush(stderr);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        int rd = fds[0];
        std::thread([rd] {
            pthread_setname_np("output capture");
            std::string pending;
            char buf[4096];
            while (true) {
                ssize_t n = ::read(rd, buf, sizeof buf);
                if (n <= 0) break;
                pending.append(buf, (size_t)n);
                size_t at;
                while ((at = pending.find('\n')) != std::string::npos) {
                    forward_line(pending.substr(0, at));
                    pending.erase(0, at + 1);
                }
            }
        }).detach();
        return saved;
    }
}
