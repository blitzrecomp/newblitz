// The PSP's ROM (docs/PSP_PORT.md §4.3): recomp::rom_provider over <app>/rom/blitz.z64 on the Memory Stick, never held whole
// in RAM. 96 blocks of 16 KB (1.5 MB in .bss) in LRU, filled by sceIoLseek + sceIoRead: M4's trace simulation found 16 KB
// blocks beat 32 KB at every cache size (§14.10). A miss blocks only the thread that asked.
// The file is the plain normalized .z64 (big-endian, the stored byte order the provider returns). The kit's sidecar
// rom/blitz.z64.sha1 is trusted instead of hashing 16 MB on the device: it must name this build's ROM (its XXH3), the file's
// size, and the header CRCs the file really has. The switch file <app>/romhash makes it hash the whole file too.
// M7: the cache lock is no longer held across sceIoRead (copy_block): only the thread that missed, and any thread that wants
// that same block, waits for the Memory Stick; hits on other blocks (and rom_stats) go on.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <pspkernel.h>
#include <pspiofilemgr.h>

#include "librecomp/game.hpp"
#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#include "psp_log.h"
#include "psp_sys.h"
#include "host/host_backends.h"

namespace {
    constexpr uint32_t block = 16 * 1024, blocks = 96;
    alignas(64) uint8_t g_cache[block * blocks];

    class MsRomProvider final : public recomp::rom_provider {
    public:
        bool open(const std::string& path) {
            fd_ = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
            if (fd_ < 0) return false;
            SceOff end = sceIoLseek(fd_, 0, PSP_SEEK_END);
            size_ = end > 0 ? (size_t)end : 0;
            for (uint32_t i = 0; i < blocks; i++) {
                tags_[i] = UINT32_MAX;
                ages_[i] = 0;
            }
            return size_ > 0;
        }
        size_t size() const override { return size_; }

        // A hit copies under the cache lock (microseconds). A miss marks its slot as loading, drops the lock, and reads the
        // block under the file lock only, so other threads' hits go on meanwhile; a thread that wants a block being loaded
        // blocks on loaded_ (registered in waiters_ under the cache lock, so no wake-up is lost) until a load finishes, then
        // looks again. (Waiting on the file lock instead could spin: a higher-priority waiter that ran before the loader
        // reached io_.lock() found both locks free and looped for ever, the loader never getting the CPU again.)
        void read(uint32_t rom_offset, uint8_t* dst, size_t len) override {
            while (len > 0) {
                uint32_t b = rom_offset / block;
                size_t in = rom_offset % block, n = std::min(len, (size_t)(block - in));
                copy_block(b, dst, in, n);
                dst += n;
                rom_offset += (uint32_t)n;
                len -= n;
            }
        }

        host::RomStats stats() {
            pspsys::Lock l(mutex_);
            return stats_;
        }

        bool read_at(uint32_t offset, void* dst, size_t n) {
            if (sceIoLseek(fd_, offset, PSP_SEEK_SET) != (SceOff)offset) return false;
            size_t got = 0;
            while (got < n) {
                int r = sceIoRead(fd_, (uint8_t*)dst + got, (SceSize)(n - got));
                if (r <= 0) break;
                got += (size_t)r;
            }
            return got == n;
        }

        // The whole file's XXH3-64, through the (still empty) block cache as the buffer.
        uint64_t hash() {
            XXH3_state_t st;
            XXH3_64bits_reset(&st);
            for (uint32_t at = 0; at < size_;) {
                uint32_t n = (uint32_t)std::min<size_t>(sizeof g_cache, size_ - at);
                if (!read_at(at, g_cache, n)) break;
                XXH3_64bits_update(&st, g_cache, n);
                at += n;
            }
            return XXH3_64bits_digest(&st);
        }

    private:
        void copy_block(uint32_t b, uint8_t* dst, size_t in, size_t n) {
            if ((size_t)b * block >= size_) { std::memset(dst, 0, n); return; }
            for (bool waited = false;;) {
                mutex_.lock();
                tick_++;
                int found = -1;
                for (uint32_t i = 0; i < blocks; i++) {
                    if (tags_[i] == b) { found = (int)i; break; }
                }
                if (found >= 0 && !loading_[found]) {
                    ages_[found] = tick_;
                    stats_.hits++;
                    std::memcpy(dst, &g_cache[found * block] + in, n);
                    mutex_.unlock();
                    return;
                }
                if (found >= 0) {
                    // another thread is reading this block: wait for it (it holds the file lock until the block is in)
                    if (!waited) stats_.waits++;
                    waited = true;
                    waiters_++;
                    mutex_.unlock();
                    loaded_.wait(-1);
                    continue;
                }
                // a miss: the least recently used slot that nobody is loading
                int victim = -1;
                for (uint32_t i = 0; i < blocks; i++) {
                    if (loading_[i]) continue;
                    if (victim < 0 || ages_[i] < ages_[victim]) victim = (int)i;
                }
                if (victim < 0) {   // every slot is being loaded (cannot happen with fewer readers than slots): wait
                    waiters_++;
                    mutex_.unlock();
                    loaded_.wait(-1);
                    continue;
                }
                tags_[victim] = b;
                loading_[victim] = true;
                ages_[victim] = tick_;
                mutex_.unlock();

                io_.lock();
                uint64_t t0 = pspsys::now_us();
                uint8_t* slot = &g_cache[victim * block];
                size_t want = std::min<size_t>(block, size_ - (size_t)b * block);
                bool ok = read_at(b * block, slot, want);
                if (!ok) std::memset(slot, 0, block);
                else if (want < block) std::memset(slot + want, 0, block - want);
                double ms = (pspsys::now_us() - t0) / 1000.0;
                mutex_.lock();
                io_.unlock();
                loading_[victim] = false;
                if (!ok) {
                    if (stats_.misses < 5) PERR("rom", "read of block %u failed", (unsigned)b);
                    tags_[victim] = UINT32_MAX;
                }
                stats_.misses++;
                stats_.worst_ms = std::max(stats_.worst_ms, ms);
                if (ms > 20.0) { stats_.stalls++; stats_.stall_ms += ms; }
                std::memcpy(dst, slot + in, n);
                uint32_t wake = waiters_;
                waiters_ = 0;
                mutex_.unlock();
                for (uint32_t i = 0; i < wake; i++) loaded_.signal();
                return;
            }
        }

        SceUID fd_ = -1;
        size_t size_ = 0;
        uint32_t tags_[blocks];
        uint64_t ages_[blocks];
        bool loading_[blocks] = {};
        uint64_t tick_ = 0;
        host::RomStats stats_{};
        pspsys::Mutex mutex_;   // tags, ages, loading flags, stats; never held across a read of the Memory Stick
        pspsys::Mutex io_;      // the file (seek + read)
        pspsys::Event loaded_;  // one signal per waiter registered when a load finishes
        uint32_t waiters_ = 0;  // threads blocked on loaded_ (under mutex_)
    };

    MsRomProvider g_rom;

    std::string sidecar_value(const std::string& text, const char* key) {
        std::string k = std::string("\n") + key + " ";
        std::string t = "\n" + text;
        size_t at = t.find(k);
        if (at == std::string::npos) return "";
        size_t s = at + k.size(), e = t.find_first_of("\r\n", s);
        return t.substr(s, e == std::string::npos ? std::string::npos : e - s);
    }
}

namespace host {
    bool open_rom(const std::string& path, uint64_t expected_xxh3) {
        if (!g_rom.open(path)) {
            PERR("rom", "can't open %s (the kit puts the ROM there: tools/psp/kit.sh)", path.c_str());
            return false;
        }
        uint8_t header[64];
        if (!g_rom.read_at(0, header, sizeof header) || header[0] != 0x80 || header[1] != 0x37) {
            PERR("rom", "%s is not a big-endian .z64 (first bytes %02X %02X)", path.c_str(), header[0], header[1]);
            return false;
        }
        char crcs[24];
        snprintf(crcs, sizeof crcs, "%02X%02X%02X%02X %02X%02X%02X%02X", header[0x10], header[0x11], header[0x12], header[0x13],
                 header[0x14], header[0x15], header[0x16], header[0x17]);
        char want_hash[20];
        snprintf(want_hash, sizeof want_hash, "%016llX", (unsigned long long)expected_xxh3);
        std::string side;
        if (!pspsys::read_file(path + ".sha1", side, 4096)) {
            PERR("rom", "no sidecar %s.sha1 (the kit writes it)", path.c_str());
            return false;
        }
        std::string sz = sidecar_value(side, "size"), crc = sidecar_value(side, "crc"), xx = sidecar_value(side, "xxh3");
        if (sz != std::to_string(g_rom.size()) || crc != crcs || strcasecmp(xx.c_str(), want_hash) != 0) {
            PERR("rom", "%s doesn't match its sidecar or this build (size %u vs %s, crc %s vs %s, xxh3 %s vs %s)", path.c_str(),
                 (unsigned)g_rom.size(), sz.c_str(), crcs, crc.c_str(), xx.c_str(), want_hash);
            return false;
        }
        std::string how = sidecar_value(side, "merged_xxh3").empty() ? "sidecar trusted" : "sidecar trusted (a kit ROM: Modern's art merged)";
        if (pspsys::exists(psplog::app_path("romhash"))) {
            uint64_t t0 = pspsys::now_us();
            uint64_t h = g_rom.hash();
            // A Modern kit ROM (art merged on the Mac): the file's own hash is merged_xxh3; xxh3 names its base ROM.
            std::string merged = sidecar_value(side, "merged_xxh3");
            if (!merged.empty()) expected_xxh3 = strtoull(merged.c_str(), nullptr, 16);
            if (h != expected_xxh3) {
                PERR("rom", "%s: XXH3 %016llX, expected %s", path.c_str(), (unsigned long long)h, want_hash);
                return false;
            }
            char buf[64];
            snprintf(buf, sizeof buf, "XXH3 checked in %.1f s", (pspsys::now_us() - t0) / 1e6);
            how = buf;
        }
        PLOG("rom", "%s: %u bytes, %s, header %.20s crc %s; read through a %u x %u KB block cache", path.c_str(),
             (unsigned)g_rom.size(), how.c_str(), (const char*)header + 0x20, crcs, (unsigned)blocks, (unsigned)block / 1024);
        recomp::set_rom_provider(&g_rom);
        return true;
    }
    RomStats rom_stats() { return g_rom.stats(); }
}
