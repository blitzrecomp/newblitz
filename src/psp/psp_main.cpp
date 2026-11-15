// New Blitz on the PSP: boot (docs/PSP_PORT.md §4.8). One file for both builds:
//   the EBOOT (PSP_TARGET=psp, M5): module info, clocks, exit and power callbacks, the log's four sinks, the remote
//                 commands over host0 and the Worker, the debug-screen overlay, the watchdog's HANG block
//   psp-host (the Mac's headless core, M4): the same boot without the PSP-only steps, which are marked below
//
// On the PSP the app folder is the EBOOT's own (ms0:/PSP/GAME/NBCLASSIC). Switch files there (docs/PSP_PORT.md §14.12):
//   input_script.txt  scripted input (dev_input's line format, below)      stackpool  "<slots> <KB>" for the process pool
//   romhash           hash the whole ROM at boot (else the sidecar is trusted)  cpu222  run at 222 MHz, not 333
//   freezevi          "<at> [secs]": freeze the VI thread at game second <at> for <secs> (for good without), the
//                     watchdog's HANG test; the command "freeze vi [secs]" does the same at once
//   nogfx             the null renderer and full debug screen (M5) instead of the GE renderer (M7, psp_gfx.cpp)
//   present           "30|auto|20": presentation skip (default auto)     overlay  "off|hud|log" over the GE's frame (default hud)
//   cull_ccw, noclip  the renderer's test switches (as in the DL replay)
//   vsync             swap at the vblank and wait for it before drawing into the shown buffer (default off: hsync swap)
//   logsync           every log line to stdout at once (not on the writer's flush): for PPSSPP crashes
//   audio             audio v1 (n_aspMain on the CPU, sceAudioSRC output: psp_audio.cpp); without it the game is silent (v0)
//   noaudio           audio v0 even with the audio switch
//
//   newblitz-host [--app DIR] [--rom FILE] [--input FILE] [--secs N] [--stats N] [--lockstep]
//     --app     the app folder (configs, saves, logs, the stored ROM); default build/psp/host_classic/app
//     --rom     a ROM to store in the app folder first (as the kit puts it there on the PSP)
//     --input   a script in dev_input's line format: "<seconds since the game started> <what> [hold seconds]"
//     --secs    quit after N seconds (default: run until killed)
//     --stats   seconds between [perf] lines (default 10)
//     --lockstep [VIS]  virtual time (docs/PSP_PORT.md §3.4): a VI only when the game waits for one; RDRAM's CRC32 every 100 VIs
//               as "[lockstep] vi=<n> crc=<crc>"; with VIS, quit after that many VIs
// Environment: BLITZ_ROM_TRACE=<file> (every ROM read), BLITZ_PSP_STACK_KB=<n> (every game thread's stack, for measuring).
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#if defined(__PSP__)
#include <pspkernel.h>
#include <pspthreadman.h>
#include <psppower.h>
#include <ctime>
#include <pspfpu.h>
#endif
#include <unistd.h>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/error_handling.hpp"
#include "recompui/config.h"
#include "recompui/program_config.h"
#include "recompui/recompui.h"

#include "blitz_audio.h"
#include "blitz_config.h"
#include "psp_log.h"
#include "psp_sys.h"
#if defined(__PSP__)
#include "psp_net.h"
#include "psp_remote.h"
#endif
#include "recompui_lite/lite.h"
#include "host/host_backends.h"
#if defined(BLITZ_PSP_MEMCHECK)
#include "psp_memcheck.h"
#endif

#ifndef BLITZ_SOURCE_ROOT
#define BLITZ_SOURCE_ROOT "."
#endif
#if BLITZ_MODERN
#define BLITZ_FLAVOUR_NAME "modern"
// BRANDING: PSP app title
#define BLITZ_APP_TITLE "New Blitz"
#define BLITZ_PSP_FOLDER "NEWBLITZ"
#else
#define BLITZ_FLAVOUR_NAME "classic"
// BRANDING: PSP app title
#define BLITZ_APP_TITLE "New Blitz Classic"
#define BLITZ_PSP_FOLDER "NBCLASSIC"
#endif
#if defined(BLITZ_PSP_KIT)
#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"
#include "psp_sha256.h"
#include "util/file.h"
#endif

#if defined(__PSP__)
// §4.8 step 1. The heap is fixed (libcglue takes it at start); thread stacks, the net modules and the process pool come from
// what is left of the user partition (the [mem] line's free_kb).
// BRANDING: PSP module name
PSP_MODULE_INFO("NewBlitz", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
#define BLITZ_PSP_HEAP_KB 3584   // M5: 375 KB at most (null renderer); M7: the renderer's 512 KB list and 512 KB texture
                                 // overflow come from here too (heap_hi_kb 1,839 KB in attract with 2 MB: a shot's 261 KB then ran it out); M8: 2,902 KB
                                 // with networking and a shot in Modern, so 3.5 MB (3,094 KB was free besides)
PSP_HEAP_SIZE_KB(BLITZ_PSP_HEAP_KB);
#endif

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

namespace blitz {
    void on_init(uint8_t* rdram, recomp_context* ctx);   // game_init.cpp
    void register_overlays();                            // register_overlays.cpp
    void monitor_register_thread(const OSThread* t);     // dev_monitor.cpp
    void psp_register_settings();                        // psp_settings.cpp
#if BLITZ_MODERN
    void rosters_check_start();                          // psp_rosters.cpp
    std::string rosters_check_now();
    void rosters_update();
    bool original_game();                                // adv_mode.cpp: Original 1999
    void e32_preload_rom_teams();                        // engine32.cpp
#endif
#if defined(BLITZ_PSP_KIT)
    std::string uniform_sets_export();                   // uniform_picker.cpp
    int announcer_vs_pause(const std::string& bank);     // art_dropin.cpp
#endif
}

namespace {
    using namespace std::chrono;

    std::atomic<uint64_t> g_vi{0};
    std::atomic<uint64_t> g_audio_tasks{0};
    std::atomic<bool> g_game_started{false};
    // The watchdog's test switch: the VI callback stalls the VI thread from game second freeze_at for freeze_for seconds
    // (for ever if < 0). freeze_at < 0: off.
    std::atomic<double> g_freeze_at{-1}, g_freeze_for{-1};
    steady_clock::time_point g_game_t0;
    bool g_lockstep = false;
    std::atomic<int> g_procs_now{0}, g_procs_max{0};
    uint64_t g_lockstep_vis = 0;
    std::filesystem::path g_app;

    uint32_t crc32(const uint8_t* p, size_t n) {
        static uint32_t table[256];
        static bool init = false;
        if (!init) {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            init = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    // Every OSThread the game created (RDRAM offsets): their context field is a host pointer (ultra64.h), different in every
    // run, so the lockstep CRC reads it as zero.
    std::mutex g_threads_seen_mutex;
    std::vector<uint32_t> g_threads_seen;

    uint32_t lockstep_crc() {
        static std::vector<uint8_t> copy(0x800000);
        std::memcpy(copy.data(), host::rdram(), copy.size());
        std::lock_guard l(g_threads_seen_mutex);
        for (uint32_t off : g_threads_seen) {
            size_t at = off + offsetof(OSThread, context);
            if (at + sizeof(void*) <= copy.size()) std::memset(&copy[at], 0, sizeof(void*));
        }
        return crc32(copy.data(), copy.size());
    }

    // Lockstep (VI thread, inside a VI step: every game thread is blocked, so RDRAM is still).
    void lockstep_vi(uint64_t vi) {
        if (vi % 100 == 0 && host::rdram() != nullptr) {
            PLOG("lockstep", "vi=%" PRIu64 " crc=%08X", vi, lockstep_crc());
        }
        // BLITZ_LOCKSTEP_DUMP=<vi>: RDRAM at that VI (native word order) to <app>/lockstep_<vi>.bin, to find a divergence.
        static const uint64_t dump_at = getenv("BLITZ_LOCKSTEP_DUMP") ? strtoull(getenv("BLITZ_LOCKSTEP_DUMP"), nullptr, 10) : 0;
        if (dump_at != 0 && vi == dump_at && host::rdram() != nullptr) {
            std::string path = (g_app / ("lockstep_" + std::to_string(vi) + ".bin")).string();
            if (FILE* f = fopen(path.c_str(), "wb")) {
                fwrite(host::rdram(), 1, 0x800000, f);
                fclose(f);
            }
            PLOG("lockstep", "RDRAM at vi=%" PRIu64 " written to %s", vi, path.c_str());
        }
        if (g_lockstep_vis != 0 && vi >= g_lockstep_vis) {
#if defined(BLITZ_PSP_MEMCHECK)
            PLOG("memcheck", "%llu escapes", (unsigned long long)blitz_psp_memcheck_escapes());
#endif
            PLOG("main", "lockstep: %" PRIu64 " VIs done, most processes alive at once %d", vi, g_procs_max.load());
            host::stack_report(true);
            psplog::shutdown("lockstep done");
            _exit(0);
        }
    }

    // The game's process list (docs/SAVE_STATES.md): live head at 0x802A9B80, next at +0. Walked once per VI from the VI
    // thread (read only; a frame-stale count is fine), for the stack pool's size (§4.1).
    void count_processes() {
        uint8_t* rdram = host::rdram();
        if (rdram == nullptr) return;
        auto r32 = [rdram](uint32_t a) { return *reinterpret_cast<uint32_t*>(rdram + (a - 0x80000000u)); };
        int n = 0;
        for (uint32_t p = r32(0x802A9B80u); p >= 0x80000400u && p < 0x80800000u && n < 256; p = r32(p)) n++;
        g_procs_now = n;
        int m = g_procs_max.load();
        while (n > m && !g_procs_max.compare_exchange_weak(m, n)) {}
        if (n > m && n >= 24) {
            // A new most: which processes (names at +0x1C, 16 bytes), counted by name.
            std::map<std::string, int> names;
            int k = 0;
            for (uint32_t p = r32(0x802A9B80u); p >= 0x80000400u && p < 0x80800000u && k < 256; p = r32(p), k++) {
                char name[17] = {};
                for (int i = 0; i < 16; i++) {
                    char c = (char)rdram[(p + 0x1C + i - 0x80000000u) ^ 3];
                    if (c < 32 || c >= 127) break;  // as dev_monitor.cpp reads them
                    name[i] = c;
                }
                // The process's id (+0x18) is its thread's OSId, which names it in the [stack] lines.
                names[std::string(name) + "/" + std::to_string(r32(p + 0x18))]++;
            }
            std::string list;
            for (auto& [nm, cnt] : names) list += " " + nm + (cnt > 1 ? "x" + std::to_string(cnt) : "");
            PLOG("procs", "%d processes alive at VI %" PRIu64 ":%s", n, g_vi.load(), list.c_str());
        }
    }

    std::string get_game_thread_name(const OSThread* t) {
        blitz::monitor_register_thread(t);
        if (host::rdram() != nullptr) {
            uint32_t off = (uint32_t)((const uint8_t*)t - host::rdram());
            std::lock_guard l(g_threads_seen_mutex);
            if (std::find(g_threads_seen.begin(), g_threads_seen.end(), off) == g_threads_seen.end()) g_threads_seen.push_back(off);
        }
        std::string name = "[Game] ";
        switch (t->id) {
            case 1: name += "IDLE"; break;
            case 6: name += "SCHED_INIT"; break;
            case 7: name += "GAME"; break;
            default: name += std::to_string(t->id); break;
        }
        return name;
    }

    RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
        if (task->t.type == M_AUDTASK) {
            g_audio_tasks++;
#if defined(BLITZ_PSP_AUDIO)
            return host::audio_microcode();   // audio v1: n_aspMain (psp_audio.cpp), v0 with the noaudio switch
#else
            return host_silent_aspMain;
#endif
        }
        PWARN("rsp", "unknown task type %" PRIu32, task->t.type);
        return nullptr;
    }

    ultramodern::input::connected_device_info_t connected(int controller) {
        if (controller == 0) return { ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak };
        return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }

    std::vector<recomp::GameEntry> games() {
        return {{
            .rom_hash = blitz::rom_hash,
            .internal_name = "blitz2k",
            .display_name = blitz::display_name,
            .game_id = blitz::game_id,
            .mod_game_id = blitz::mod_game_id,
            .save_type = recomp::SaveType::None,
            .is_enabled = false,
            .has_compressed_code = false,
            .entrypoint_address = (gpr)(int32_t)0x80000400,
            .entrypoint = recomp_entrypoint,
            .thread_create_callback = nullptr,  // save states are off on the PSP
            .on_init_callback = blitz::on_init,
        }};
    }

#if defined(__PSP__)
    double g_quit_after = 0, g_stats_every = 5;    // [perf] every 5 s (§7.3)
#else
    double g_quit_after = 0, g_stats_every = 10;
#endif

    // The VI thread stops here (the HANG test): no VI reaches the game, so every game thread ends up waiting.
    void freeze_vi_thread() {
        double secs = g_freeze_for.load();
        PWARN("main", "freezing the VI thread %s (test switch)", secs < 0 ? "for good" : (std::to_string((int)secs) + " s").c_str());
        uint64_t t0 = pspsys::now_us();
        while (g_freeze_at.load() >= 0 && (secs < 0 || pspsys::now_us() - t0 < (uint64_t)(secs * 1e6))) pspsys::sleep_ms(100);
        g_freeze_at = -1;
        PWARN("main", "VI thread running again after %.1f s", (pspsys::now_us() - t0) / 1e6);
    }
    std::string g_input;

    // [perf] (§7.3): every g_stats_every seconds (PSP 5, psp-host 10). On the PSP the renderer's half comes from
    // psp_gfx.cpp (ticks/s, shown/s, skip, game_ms, gfx_cpu_ms, ge_ms, tex, unk_cc), then the ROM, heap and stacks.
    void perf_line(double t) {
        static uint64_t last_vi = 0;
        static double last_t = 0;
        uint64_t vi = g_vi.load();
        double dt = t - last_t;
        double rate = t > last_t ? (vi - last_vi) / dt : 0;
        last_vi = vi;
        last_t = t;
        host::GfxStats g = host::gfx_stats();
        host::RomStats r = host::rom_stats();
        host::AudioStats a = host::audio_stats();
        host::ThreadCounts c = host::thread_counts();
        double hit = r.hits + r.misses ? 100.0 * r.hits / (double)(r.hits + r.misses) : 100.0;
#if defined(__PSP__)
        std::string gfx = host::gfx_perf_fields(dt);
        std::string stack_name;
        int stack_pct = host::stack_high(stack_name);
        host::MemStats m = host::mem_stats();
#if defined(BLITZ_PSP_AUDIO)
        std::string audio = host::audio_perf_fields(dt);
#else
        std::string audio = "audio_ms=0.0(off)";
#endif
        PLOG("perf", "t=%.1f vi/s=%.1f%s %s rom_hit=%.1f%% rom_miss=%" PRIu64 " rom_stall=%" PRIu64 " rom_stall_ms=%.0f"
             " rom_wait=%" PRIu64 " rom_worst_ms=%.2f heap_hi_kb=%d heap_used_kb=%d stack_hi=%s:%d%% net=%s vi=%" PRIu64 " dl=%" PRIu64
             " procs=%d procs_max=%d threads=%d game_threads=%d(max %d) audio_frames=%" PRIu64,
             t, rate, gfx.c_str(), audio.c_str(), hit, r.misses, r.stalls, r.stall_ms, r.waits, r.worst_ms, host::heap_high_kb(), m.heap_used_kb,
             stack_name.c_str(), stack_pct, pspnet::state_name(pspnet::state()), vi, g.display_lists, g_procs_now.load(),
             g_procs_max.load(), c.alive, c.alive_game, c.max_alive_game, a.frames);
#else
        PLOG("perf", "t=%.1f vi=%" PRIu64 " vi/s=%.1f dl=%" PRIu64 " frames=%" PRIu64 " rom_hit=%.1f%% rom_miss=%" PRIu64
             " rom_worst_ms=%.2f audio_frames=%" PRIu64 " audio_drop=%" PRIu64 " procs=%d procs_max=%d threads=%d game_threads=%d(max %d)",
             t, vi, rate, g.display_lists, g.frames, hit, r.misses, r.worst_ms, a.frames, a.dropped, g_procs_now.load(),
             g_procs_max.load(), c.alive, c.alive_game, c.max_alive_game);
#endif
#if defined(BLITZ_PSP_MEMCHECK)
        PLOG("memcheck", "t=%.1f escapes=%llu", t, (unsigned long long)blitz_psp_memcheck_escapes());
#endif
    }

    // RDRAM's high-water mark (M8: it must stay below the PI handles at 0x807FE000): the top non-zero word under them, and the
    // highest such mark seen this run. Scanned from the top, so it costs little while the top is empty.
    std::atomic<uint32_t> g_rdram_hi{0};
    uint32_t rdram_high_water() {
        uint8_t* r = host::rdram();
        if (r == nullptr) return 0;
        uint32_t top = 0x80000000u;
        for (int32_t off = 0x7FE000 - 4; off >= 0; off -= 4) {
            uint32_t w;
            memcpy(&w, r + off, 4);
            if (w != 0) { top = 0x80000000u + (uint32_t)off + 4; break; }
        }
        if (top > g_rdram_hi.load()) g_rdram_hi = top;
        return top;
    }

#if defined(__PSP__)
    // [mem]: the user partition's free memory, the heap, and the RDRAM guards (every 10 s, §4.2).
    host::MemStats mem_line() {
        host::MemStats m = host::mem_stats();
        host::PoolStats p = host::pool_stats();
        rdram_high_water();
        if (m.guards_ok) {
            PLOG("mem", "free_kb=%d max_kb=%d heap_kb=%d of %d heap_hi_kb=%d heap_used_kb=%d guards=ok pool=%d/%d worst_slot=%u of %u "
                 "rdram_hi=%08X",
                 m.free_kb, m.max_kb, m.heap_kb, BLITZ_PSP_HEAP_KB, host::heap_high_kb(), m.heap_used_kb, p.busy, p.slots,
                 (unsigned)p.worst_used, (unsigned)p.slot_bytes, (unsigned)g_rdram_hi.load());
        } else {
            PERR("mem", "free_kb=%d max_kb=%d heap_kb=%d heap_used_kb=%d guards=BAD (below RDRAM at +%u, above at +%u: host code "
                 "wrote outside RDRAM)", m.free_kb, m.max_kb, m.heap_kb, m.heap_used_kb, (unsigned)m.guard_low_bad,
                 (unsigned)m.guard_high_bad);
        }
        return m;
    }

    // The HANG block (§5.1): every thread, the counters, the last 200 log lines; logged, kept in logs/pending/ and sent to
    // the Worker as kind=crash when it is up.
    void hang_block(double stalled, uint64_t vi, const char* what = "no VI") {
        static int n = 0;
        n++;
        std::string head = "HANG " + std::to_string(n) + ": " + what + " for " + std::to_string((int)stalled) + " s (vi=" + std::to_string(vi) +
                           ", launch " + psplog::launch_id() + ", uptime " + std::to_string((int)psplog::uptime()) + " s)\n";
        std::string threads = host::thread_dump();
        PERR("watchdog", "%s", head.c_str());
        size_t s = 0;
        while (s < threads.size()) {
            size_t e = threads.find('\n', s);
            if (e == std::string::npos) e = threads.size();
            PERR("watchdog", "%s", threads.substr(s, e - s).c_str());
            s = e + 1;
        }
        host::stack_report(true);
        mem_line();
        std::string tail = psplog::tail(48 * 1024);
        size_t cut = tail.size();
        for (int lines = 0; cut > 0 && lines <= 200; cut--) {
            if (tail[cut - 1] == '\n') lines++;
        }
        if (cut > 0) tail.erase(0, cut + 1);
        std::string body = head + host::gfx_breadcrumb() + threads + host::overlay_status() + "---- last 200 log lines ----\n" + tail;
        std::string path = psplog::app_path("logs/pending/hang-" + psplog::launch_id() + "-" + std::to_string(n) + ".log");
        pspsys::write_file(path, body.data(), body.size());
        psplog::flush();
        if (pspremote::worker_ready() && pspremote::worker_upload("crash", body, nullptr, false)) pspsys::remove_file(path);
        PERR("watchdog", "HANG block written to %s (%u bytes)", path.c_str(), (unsigned)body.size());
        psplog::flush();
    }
#endif

    // Stats, the hang watchdog (§5.1: VI and game progress still for 10 s) and the timed quit. On the PSP it runs at
    // priority 0x11, above the game's threads, so a game thread that spins can't starve it.
    void watchdog(void*) {
        auto t0 = steady_clock::now();
        uint64_t last_vi = 0, last_dl = 0;
        double last_move = 0, last_dl_move = 0, next_stats = g_stats_every, next_stack = 60, next_mem = 10;
        bool hang_reported = false, stall_reported = false;
        while (true) {
            pspsys::sleep_ms(250);
            double t = duration<double>(steady_clock::now() - t0).count();
            uint64_t vi = g_vi.load();
            if (vi != last_vi) {
                last_vi = vi;
                last_move = t;
                hang_reported = false;
            } else if (g_game_started && !hang_reported && t - last_move > 10.0) {
                hang_reported = true;
#if defined(__PSP__)
                perf_line(t);
                hang_block(t - last_move, vi);
#else
                PERR("watchdog", "HANG: no VI for %.1f s (vi=%" PRIu64 ")", t - last_move, vi);
                host::stack_report(true);
                perf_line(t);
                psplog::flush();
#endif
            }
            // The game stopped drawing while VIs still come (the PSP-1001, 2026-11-04: Classic's attract went to 0 ticks/s at
            // 88 s with VI at 60, and the VI check above never fired): 10 s without a display list, once the game draws.
            uint64_t dl = host::gfx_stats().display_lists;
            if (dl != last_dl) {
                last_dl = dl;
                last_dl_move = t;
                stall_reported = false;
            } else if (g_game_started && dl > 0 && !stall_reported && !hang_reported && vi != 0 && t - last_dl_move > 10.0) {
                stall_reported = true;
#if defined(__PSP__)
                // The Gfx thread was runnable, not waiting, in the first one (it outranks the game and the Wi-Fi driver, so
                // nothing below it ran, not even the upload). Note where it is, stop it, report, then go back to the menu.
                std::string crumb = host::gfx_breadcrumb();
                int stopped = host::suspend_thread_named("Gfx");
                PERR("watchdog", "%sGfx thread suspended (%d)", crumb.c_str(), stopped);
                perf_line(t);
                hang_block(t - last_dl_move, vi, "no display list (VI still running)");
                if (!pspremote::worker_ready()) {   // the upload needs the net up: give it a little longer
                    for (int i = 0; i < 40 && !pspremote::worker_ready(); i++) pspsys::sleep_ms(250);
                    if (pspremote::worker_ready()) {
                        std::string again = psplog::tail(64 * 1024);
                        pspremote::worker_upload("crash", crumb + again, nullptr, false);
                    }
                }
                // Start the game again, so a hang costs a relaunch, not a trip to the PSP; three in ten minutes and it goes
                // to the menu instead (logs/hang_restarts: the time of each, RTC seconds).
                std::string rpath = psplog::app_path("logs/hang_restarts");
                std::string prev;
                pspsys::read_file(rpath, prev);
                time_t now_s = time(nullptr);
                std::string kept;
                int recent = 0;
                for (size_t a = 0; a < prev.size();) {
                    size_t b = prev.find('\n', a);
                    if (b == std::string::npos) b = prev.size();
                    long v = atol(prev.substr(a, b - a).c_str());
                    if (v > 0 && now_s - v < 600) { recent++; kept += std::to_string(v) + "\n"; }
                    a = b + 1;
                }
                if (recent < 3) {
                    kept += std::to_string((long)now_s) + "\n";
                    pspsys::write_file(rpath, kept.data(), kept.size());
                    PERR("watchdog", "relaunching after the hang report (%d in the last 10 min)", recent + 1);
                    pspremote::relaunch("");
                }
                PERR("watchdog", "exiting to the menu after the hang report");
                pspremote::go_home("hang");   // NB Remote with home=remote, else the XMB
#else
                PERR("watchdog", "HANG: no display list for %.1f s (vi=%" PRIu64 ")", t - last_dl_move, vi);
                host::stack_report(true);
                psplog::flush();
#endif
            }
#if defined(__PSP__)
            // Keep the PSP awake while the game runs: no input in the attract demo let Power Save dim the screen and sleep the
            // system, which took the Wi-Fi down (2026-11-04). The same "the user is active" tick homebrew players send.
            static double next_tick = 0;
            if (t >= next_tick) {
                next_tick = t + 5;
                scePowerTick(PSP_POWER_TICK_ALL);
            }
#endif
            if (t >= next_stats) {
                next_stats += g_stats_every;
                perf_line(t);
#if !defined(__PSP__)
                rdram_high_water();
                PLOG("mem", "rdram_hi=%08X", (unsigned)g_rdram_hi.load());
#endif
#if BLITZ_MODERN
                if (g_game_started) blitz::rosters_update();
#endif
            }
            if (t >= next_stack) {
                next_stack += 60;
                host::stack_report(false);
            }
#if defined(__PSP__)
            if (t >= next_mem) {
                next_mem += 10;
                mem_line();
            }
#endif
            if (g_quit_after > 0 && t >= g_quit_after) {
                perf_line(t);
                host::stack_report(true);
#if defined(BLITZ_PSP_MEMCHECK)
                PLOG("memcheck", "%llu escapes", (unsigned long long)blitz_psp_memcheck_escapes());
#endif
                PLOG("main", "quitting after %.0f s: %" PRIu64 " VIs, most processes alive at once %d", t, vi, g_procs_max.load());
                psplog::shutdown("timed quit");
#if defined(__PSP__)
                sceKernelExitGame();
#else
                _exit(0);
#endif
            }
        }
    }

    void update_gfx(void*) {
        static int frames = 0;
        // Not before the VI thread has run: until the game starts it sets a dummy VI mode, and its first VI after the start
        // reads the game's mode (null before osViSetMode: a null read in update_vi). The GE renderer's start (sceGuInit and a
        // vblank wait on the gfx thread) delays the VI thread past the 30th call here, which the null renderer never did.
        // (Lockstep: a VI happens only when a game thread waits for one, so before the game there are none to wait for.)
        if (frames < 30 || (g_vi.load() < 2 && !g_lockstep)) {
            if (frames < 30) frames++;
            return;
        }
        if (frames++ == 30) {
            // As the desktop's launcher does with --game: start the stored ROM.
            std::u8string id = blitz::game_id;
            if (recomp::is_rom_valid(id)) {
                g_game_t0 = steady_clock::now();
                g_game_started = true;
                PLOG("main", "starting the game");
                recomp::start_game(id, {});
            } else {
                PERR("main", "no valid ROM");
            }
        }
    }
}

// ultra_extras.cpp's two clocks, with a lockstep path (the desktop's file reads the host clock only).
extern "C" void osViGetCurrentLine_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
#if defined(ULTRAMODERN_LOCKSTEP)
    if (g_lockstep) {
        ctx->r2 = (gpr)((ultramodern::lockstep::virtual_ticks() % 781250) * 525 / 781250);
        return;
    }
#endif
    uint64_t us = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    ctx->r2 = (gpr)((us % 16683) * 525 / 16683);
}

extern "C" uint64_t blitz_cop0_count(void) {
#if defined(ULTRAMODERN_LOCKSTEP)
    if (g_lockstep) return (uint64_t)(int64_t)(int32_t)(uint32_t)ultramodern::lockstep::virtual_ticks();
#endif
    uint64_t ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    return (uint64_t)(int64_t)(int32_t)(uint32_t)(ns * 3 / 64);
}

namespace blitz {
    bool controller_connected(int controller_num) { return controller_num == 0; }
    bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) { return host::get_input(controller_num, buttons, x, y); }
}

namespace host {
    uint64_t vi_count() { return g_vi.load(); }
#if defined(__PSP__)
    uint64_t audio_tasks() { return g_audio_tasks.load(); }
#endif
    bool lockstep() { return g_lockstep; }
    double game_seconds() {
        if (!g_game_started) return 0;
        if (g_lockstep) return g_vi.load() / 60.0;
        return duration<double>(steady_clock::now() - g_game_t0).count();
    }
}

#if defined(__PSP__)
// ---- PSP only: callbacks, the fatal exit, the extra command verbs ----------------------------------------------------------
namespace {
    uint64_t g_suspend_us = 0;

    // HOME -> Exit: flush, delete the running marker (a clean exit), leave.
    // If the exit doesn't happen (2026-11-04: the stuck Classic run logged its HOME exit, then kept running and the PSP
    // needed a power-off), a thread tries again 3 s later, then asks the kernel to stop every thread of ours.
    void exit_retry(void*) {
        pspsys::sleep_ms(3000);
        PERR("main", "still running 3 s after HOME: exiting again");
        psplog::flush();
        sceKernelExitGame();
        pspsys::sleep_ms(3000);
        sceKernelExitDeleteThread(0);
    }

    int exit_callback(int, int, void*) {
        PLOG("main", "exit requested (HOME)");
        pspsys::thread_start("blitz_exit", exit_retry, nullptr, 8 * 1024, 0x08);
        pspremote::go_home("HOME exit");   // NB Remote with home=remote, else the XMB
        return 0;
    }

    // Suspend: flush (the battery may run flat while suspended). Resume: say how long it slept (§4.8 step 4; the apctl
    // reconnect and the rosters recheck come with M10).
    int power_callback(int, int flags, void*) {
        if (flags & (PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_STANDBY)) {
            g_suspend_us = pspsys::now_us();
            PLOG("main", "suspending (power flags 0x%08X)", flags);
            psplog::flush();
        } else if ((flags & PSP_POWER_CB_RESUME_COMPLETE) && g_suspend_us) {
            PLOG("main", "resumed after %.0f s", (pspsys::now_us() - g_suspend_us) / 1e6);
            g_suspend_us = 0;
        }
        return 0;
    }

    int callback_thread(SceSize, void*) {
        int cb = sceKernelCreateCallback("blitz_exit", exit_callback, nullptr);
        sceKernelRegisterExitCallback(cb);
        int pcb = sceKernelCreateCallback("blitz_power", power_callback, nullptr);
        scePowerRegisterCallback(-1, pcb);
        sceKernelSleepThreadCB();
        return 0;
    }

    bool v_live(const std::vector<std::string>& a, std::string& out, void*) {
        std::string line;
        for (size_t i = 1; i < a.size(); i++) line += (i > 1 ? " " : "") + a[i];
        if (!host::add_input_line(line)) {
            out = "live: live [+secs|secs] <A|B|Z|START|UP|...|A+B|STICK x,y> [hold secs]\n";
            return false;
        }
        char buf[96];
        snprintf(buf, sizeof buf, "live %s at game second %.2f\n", line.c_str(), host::game_seconds());
        out = buf;
        PLOG("input", "%s", out.substr(0, out.size() - 1).c_str());
        return true;
    }

    // pad [+secs] <BUTTON[+BUTTON]> [hold secs]: synthetic PSP buttons before the map (psp_input.cpp), e.g. the hotkeys.
    bool v_pad(const std::vector<std::string>& a, std::string& out, void*) {
        std::string line;
        for (size_t i = 1; i < a.size(); i++) line += (i > 1 ? " " : "") + a[i];
        if (!host::add_pad_line(line, out)) return false;
        out = "pad " + line + "\n";
        PLOG("input", "%s", out.substr(0, out.size() - 1).c_str());
        return true;
    }

    // freeze vi [secs] | freeze off: the watchdog's HANG test from a command.
    bool v_freeze(const std::vector<std::string>& a, std::string& out, void*) {
        if (a.size() >= 2 && a[1] == "off") {
            g_freeze_at = -1;
            out = "freeze off\n";
            return true;
        }
        if (a.size() < 2 || a[1] != "vi") {
            out = "freeze: freeze vi [secs] | freeze off\n";
            return false;
        }
        g_freeze_for = a.size() > 2 ? atof(a[2].c_str()) : -1.0;
        g_freeze_at = 0.0;
        out = "freezing the VI thread at its next VI\n";
        return true;
    }

    // set <key> <value>: clock 222|333, stats <secs>, flush_ms <ms>, or <config id>.<option id> (a registered option; saved to
    // <app>/<config id>.json). Anything else is stored for later, as before.
    bool v_set(const std::vector<std::string>& a, std::string& out, void*) {
        if (a.size() < 3) {
            out = "set: set <key> <value>\n";
            return false;
        }
        const std::string& key = a[1];
        std::string value;
        for (size_t i = 2; i < a.size(); i++) value += (i > 2 ? " " : "") + a[i];
        char buf[160];
        if (key == "clock") {
            int mhz = atoi(value.c_str());
            if (mhz != 222 && mhz != 333) {
                out = "set clock: 222 or 333\n";
                return false;
            }
            int rc = mhz == 333 ? scePowerSetClockFrequency(333, 333, 166) : scePowerSetClockFrequency(222, 222, 111);
            snprintf(buf, sizeof buf, "clock %d: rc %d, cpu %d MHz bus %d MHz\n", mhz, rc, scePowerGetCpuClockFrequencyInt(),
                     scePowerGetBusClockFrequencyInt());
        } else if (key == "stats") {
            g_stats_every = std::max(1.0, atof(value.c_str()));
            snprintf(buf, sizeof buf, "stats every %.0f s\n", g_stats_every);
        } else if (key == "present") {
            int m = host::present_mode_from(value);
            if (m < 0) { out = "set present: 30, auto or 20\n"; return false; }
            host::set_present_mode(m);
            snprintf(buf, sizeof buf, "present %s\n", host::present_mode_name(m));
        } else if (key == "overlay") {
            int m = host::overlay_mode_from(value);
            if (m < 0) { out = "set overlay: off, hud or log\n"; return false; }
            host::set_overlay_mode(m);
            snprintf(buf, sizeof buf, "overlay %s%s\n", host::overlay_mode_name(m), host::gfx_mode_ge() ? "" : " (null renderer: the full screen stays)");
        } else if (key == "flush_ms") {
            psplog::set_flush_ms(atoi(value.c_str()));
            snprintf(buf, sizeof buf, "log flush every %d ms\n", atoi(value.c_str()));
        } else if (key.find('.') != std::string::npos) {
            std::string id = key.substr(0, key.find('.')), option = key.substr(key.find('.') + 1);
            try {
                recomp::config::Config& c = recompui::config::get_config(id);
                if (!c.has_option(option)) {
                    out = "set: no option " + option + " in " + id + "\n";
                    return false;
                }
                recomp::config::ConfigValueVariant now = c.get_option_value(option), v;
                if (std::holds_alternative<bool>(now)) v = value == "true" || value == "1" || value == "on";
                else if (std::holds_alternative<double>(now)) v = atof(value.c_str());
                else if (std::holds_alternative<uint32_t>(now)) v = (uint32_t)strtoul(value.c_str(), nullptr, 0);
                else v = value;
                c.update_option_value(option, v);
                bool saved = c.save_config();
                snprintf(buf, sizeof buf, "%s.%s = %s (%s)\n", id.c_str(), option.c_str(), value.c_str(), saved ? "saved" : "NOT saved");
            } catch (const std::exception& e) {
                out = std::string("set: ") + e.what() + "\n";
                return false;
            }
        } else {
            snprintf(buf, sizeof buf, "%s=%s (stored; nothing reads it yet)\n", key.c_str(), value.c_str());
        }
        out = buf;
        PLOG("remote", "set %s", out.substr(0, out.size() - 1).c_str());
        return true;
    }

    bool peek_rdram(uint32_t addr, uint32_t len, std::string& out) { return host::peek(addr, len, out); }

#if BLITZ_MODERN
    // rosters: check the hub now (§6); the new lineup applies at the next menu.
    bool v_rosters(const std::vector<std::string>&, std::string& out, void*) {
        out = blitz::rosters_check_now() + "\n";
        return true;
    }
#endif

    void stat_hook(std::string& out, void*) {
        out += host::overlay_status();
        host::MemStats m = host::mem_stats();
        char buf[200];
        snprintf(buf, sizeof buf, "mem free_kb=%d max_kb=%d heap_kb=%d of %d heap_used_kb=%d guards=%s\n", m.free_kb, m.max_kb, m.heap_kb,
                 BLITZ_PSP_HEAP_KB, m.heap_used_kb, m.guards_ok ? "ok" : "BAD");
        out += buf;
        host::ThreadCounts c = host::thread_counts();
        host::PoolStats p = host::pool_stats();
        snprintf(buf, sizeof buf, "game threads alive %d (most %d), started %d; pool %d/%d busy (most %d), fallbacks %d, worst slot %u of %u\n",
                 c.alive_game, c.max_alive_game, c.created, p.busy, p.slots, p.max_busy, p.fallbacks, (unsigned)p.worst_used,
                 (unsigned)p.slot_bytes);
        out += buf;
        out += host::thread_dump();
    }

    // <app>/<name>: its first numbers, or the defaults when it isn't there.
    bool switch_numbers(const char* name, double& a, double& b) {
        std::string text;
        if (!pspsys::read_file(psplog::app_path(name), text, 256)) return false;
        double x = a, y = b;
        int n = sscanf(text.c_str(), "%lf %lf", &x, &y);
        if (n >= 1) a = x;
        if (n >= 2) b = y;
        return true;
    }
}

// ultramodern's quick_exit on the PSP (patch 0021): log, flush, and leave with the running marker in place, so the next
// launch keeps this run's tail in pending/ (it counts as a dead run).
extern "C" void blitz_psp_fatal_exit(int status) {
    PERR("main", "fatal exit %d", status);
    pspremote::go_home("fatal exit", false);   // flushed, the marker kept; NB Remote with home=remote, else the XMB
}
#endif

#if defined(BLITZ_PSP_KIT)
// psp-host --make-kit (docs/PSP_PORT.md §4.3, M8): Modern's art merged into the ROM on the Mac. The base ROM is loaded whole
// (no provider), game_init.cpp's on_init runs as it does when the desktop starts a game (art_dropin.cpp's art_init, the
// added files, fs_commit_files: the same code), and the result is written for the kit:
//   <out>/blitz.z64        the merged ROM image (16 MB + the added files and the longer file table)
//   <out>/blitz.z64.sha1   the sidecar psp_rom.cpp checks: sha1, size, crc, xxh3 (the BASE ROM's, which the build expects),
//                          merged_xxh3 (this file's, for the romhash switch)
//   <out>/kit.txt          what the merge leaves outside the ROM: the uniform sets, the announcer pauses, the table size
//   <out>/artfs.bin        art_dropin's extra file-table entries (0x80408000 in RDRAM; none for the original game's table)
namespace blitz {
    std::string psp_kit_text() { return ""; }  // the kit maker reads the content folders (uniform_picker.cpp)
}
// engine32.cpp's team loader: the kit maker has the ROM in memory.
std::vector<uint8_t> psp_rom_head() {
    auto rom = recomp::get_rom();
    return std::vector<uint8_t>(rom.begin(), rom.begin() + std::min<size_t>(rom.size(), 0x150C40));
}
namespace {
    std::string sha1_file_hex(const std::vector<uint8_t>& d);
    int make_kit(const std::string& rom_src, const std::string& out_dir) {
        std::error_code ec;
        std::vector<uint8_t> rom;
        {
            FILE* f = fopen(rom_src.c_str(), "rb");
            if (f == nullptr) { PERR("kit", "can't open the base ROM %s", rom_src.c_str()); return 1; }
            fseek(f, 0, SEEK_END);
            rom.resize((size_t)ftell(f));
            fseek(f, 0, SEEK_SET);
            size_t got = fread(rom.data(), 1, rom.size(), f);
            fclose(f);
            if (got != rom.size()) { PERR("kit", "short read of %s", rom_src.c_str()); return 1; }
        }
        uint64_t base_hash = XXH3_64bits(rom.data(), rom.size());
        if (base_hash != blitz::rom_hash) {
            PERR("kit", "%s: XXH3 %016llX, this build expects %016llX (the big-endian v1.1 ROM)", rom_src.c_str(),
                 (unsigned long long)base_hash, (unsigned long long)blitz::rom_hash);
            return 1;
        }
        uint32_t toc_game = (uint32_t)rom[0x150C40 + 12] << 24 | (uint32_t)rom[0x150C40 + 13] << 16 | (uint32_t)rom[0x150C40 + 14] << 8 | rom[0x150C40 + 15];
        recomp::set_rom_contents(std::move(rom));
        std::vector<uint8_t> ram(8u << 20, 0);
        recomp_context ctx{};
        PLOG("kit", "on_init with the base ROM in memory (program %s, app %s)", recompui::file::get_program_path().string().c_str(),
             g_app.string().c_str());
        blitz::on_init(ram.data(), &ctx);
        auto merged = recomp::get_rom();
        std::filesystem::create_directories(out_dir, ec);
        std::string rom_out = out_dir + "/blitz.z64";
        {
            FILE* f = fopen((rom_out + ".part").c_str(), "wb");
            if (f == nullptr || fwrite(merged.data(), 1, merged.size(), f) != merged.size()) { PERR("kit", "can't write %s", rom_out.c_str()); return 1; }
            fclose(f);
            std::filesystem::rename(rom_out + ".part", rom_out, ec);
        }
        auto be = [&](size_t o) { return (uint32_t)merged[o] << 24 | (uint32_t)merged[o + 1] << 16 | (uint32_t)merged[o + 2] << 8 | merged[o + 3]; };
        uint32_t toc_count = be(0x150C40 + 12);
        std::vector<uint8_t> copy(merged.begin(), merged.end());  // (a copy: get_rom's span is the runtime's)
        char side[512];
        snprintf(side, sizeof side, "%s  blitz.z64\nsize %zu\ncrc %02X%02X%02X%02X %02X%02X%02X%02X\nxxh3 %016llX\nmerged_xxh3 %016llX\n",
                 sha1_file_hex(copy).c_str(), merged.size(), merged[0x10], merged[0x11], merged[0x12], merged[0x13], merged[0x14],
                 merged[0x15], merged[0x16], merged[0x17], (unsigned long long)blitz::rom_hash,
                 (unsigned long long)XXH3_64bits(merged.data(), merged.size()));
        pspsys::write_file(rom_out + ".sha1", side, strlen(side));
        // kit.txt
        std::string kit = "kit Modern's art merged by newblitz-kit build " BLITZ_BUILD " (" BLITZ_COMMIT ")\n";
        kit += "toc " + std::to_string(toc_count) + "\n";
        kit += "toc_game " + std::to_string(toc_game) + "\n";
        std::vector<std::string> banks;
        for (const std::filesystem::path& d : {recompui::file::get_program_path() / "announcer", recomp::get_config_path() / "announcer"})
            for (const auto& e : std::filesystem::directory_iterator(d, ec)) {
                std::string n = e.path().filename().string();
                for (char& c : n) c = (char)std::tolower((unsigned char)c);
                if (n.size() > 4 && n.substr(n.size() - 4) == ".bfx") banks.push_back(n.substr(0, n.size() - 4));
            }
        for (const std::string& b : banks) {
            int p = blitz::announcer_vs_pause(b);
            if (p >= 0) kit += "vs_pause " + b + " " + std::to_string(p) + "\n";
        }
        std::string sets = blitz::uniform_sets_export();
        kit += sets;
        pspsys::write_file(out_dir + "/kit.txt", kit.data(), kit.size());
        // artfs.bin: the extra entries art_install_extras put at 0x80408000, up to the first empty one
        std::string extras;
        for (uint32_t a = 0x408000; a + 32 <= ram.size(); a += 32) {
            bool any = false;
            for (uint32_t i = 0; i < 32; i++) any |= ram[(a + i) ^ 3] != 0;
            if (!any) break;
            for (uint32_t i = 0; i < 32; i++) extras.push_back((char)ram[(a + i) ^ 3]);
        }
        pspsys::write_file(out_dir + "/artfs.bin", extras.data(), extras.size());
        size_t set_lines = (size_t)std::count(sets.begin(), sets.end(), '\n');
        PLOG("kit", "%s: %zu bytes (%.1f MB over the base ROM), file table %u entries; kit.txt %zu uniform sets, %zu announcer pauses; "
             "artfs.bin %zu entries", rom_out.c_str(), merged.size(), (merged.size() - (16u << 20)) / 1048576.0, (unsigned)toc_count,
             set_lines, banks.size(), extras.size() / 32);
        psplog::shutdown("kit made");
        return 0;
    }
    std::string sha1_file_hex(const std::vector<uint8_t>& d) { return pspsha::sha1_hex(d.data(), d.size()); }
}
#endif

int main(int argc, char** argv) {
#if defined(__PSP__)
    // 1. Clocks (§4.1; the cpu222 switch file keeps 222 MHz) and no FPU exceptions (the N64's code makes NaNs and infinities).
    pspFpuSetEnable(0);
    std::string app = pspsys::dir_name(argc > 0 && argv[0] ? argv[0] : "ms0:/PSP/GAME/" BLITZ_PSP_FOLDER "/EBOOT.PBP");
    g_app = app;
    bool cpu222 = false;
    {
        SceIoStat st;
        cpu222 = sceIoGetstat((app + "/cpu222").c_str(), &st) >= 0;
    }
    int clock_rc = cpu222 ? scePowerSetClockFrequency(222, 222, 111) : scePowerSetClockFrequency(333, 333, 166);
    std::string rom_src;
    g_input = app + "/input_script.txt";
#else
    // Boot step 1 (PSP: module info, clocks 333/333/166, pspFpuSetEnable(0)) has nothing to do on the Mac.
    g_app = std::filesystem::path(BLITZ_SOURCE_ROOT) / "build/psp/host_" BLITZ_FLAVOUR_NAME "/app";
    std::string rom_src;
#if defined(BLITZ_PSP_KIT)
    std::string kit_out, kit_program;
#endif
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](void) -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--app") g_app = next();
#if defined(BLITZ_PSP_KIT)
        else if (a == "--make-kit") kit_out = next();
        else if (a == "--program") kit_program = next();
#endif
        else if (a == "--rom") rom_src = next();
        else if (a == "--input") g_input = next();
        else if (a == "--secs") g_quit_after = atof(next().c_str());
        else if (a == "--stats") g_stats_every = atof(next().c_str());
        else if (a == "--lockstep") {
            g_lockstep = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') g_lockstep_vis = strtoull(argv[++i], nullptr, 10);
        }
        else {
            fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(g_app, ec);
#endif

    // 2. The log ring and its sinks: stdout, <app>/logs and (PSP) host0:/blitz-psp. psp-host writes stdout through a saved fd
    // (fds 1 and 2 are captured into the ring next); the PSP captures newlib's stdout and stderr (psp_stdio.cpp).
    psplog::Options lo;
    lo.app_dir = g_app.string();
    lo.build = BLITZ_BUILD;
    lo.commit = BLITZ_COMMIT;
    lo.flavour = BLITZ_FLAVOUR_NAME;
#if defined(__PSP__)
    lo.host0_dir = "host0:/blitz-psp";
    lo.flush_ms = 1000;
    // the switch file logsync: every line straight to stdout as it is logged (a PPSSPP crash then loses nothing)
    {
        SceIoStat st;
        lo.stdout_immediate = sceIoGetstat((app + "/logsync").c_str(), &st) >= 0;
    }
#else
    int saved_stdout = dup(1);
    lo.flush_ms = 200;
    lo.stdout_fd = saved_stdout;
#endif
    // 3. psplog::init checks the running marker and moves a dead run's tail to pending/.
    psplog::init(lo);
    host::capture_output();
    std::set_terminate([] {
        PERR("main", "std::terminate (an uncaught exception or a joinable thread destroyed)");
        psplog::flush();
        abort();
    });
#if defined(__PSP__)
    PLOG("main", "New Blitz %s build %s (%s), flavour " BLITZ_FLAVOUR_NAME ", PSP, 32-bit registers%s; clock %s (rc %d, cpu %d MHz)", BLITZ_VERSION,
         BLITZ_BUILD, BLITZ_COMMIT,
#if defined(BLITZ_PSP_MEMCHECK)
         ", MEMCHECK",
#else
         "",
#endif
         cpu222 ? "222" : "333", clock_rc, scePowerGetCpuClockFrequencyInt());
#else
    PLOG("main", "New Blitz psp-host %s build %s (%s), flavour " BLITZ_FLAVOUR_NAME ", %s registers%s%s", BLITZ_VERSION, BLITZ_BUILD, BLITZ_COMMIT,
#if defined(RECOMP_GPR32)
         "32-bit",
#else
         "64-bit (stock)",
#endif
#if defined(BLITZ_PSP_MEMCHECK)
         ", MEMCHECK",
#else
         "",
#endif
         g_lockstep ? ", lockstep" : "");
#endif
    if (!psplog::dead_run().empty()) PWARN("main", "the last run died: %s", psplog::dead_run().c_str());

#if defined(__PSP__)
    // 4. Exit and power callbacks, on their own thread (it sleeps with callbacks on).
    {
        SceUID cbt = sceKernelCreateThread("blitz_callbacks", callback_thread, 0x11, 0x1000, PSP_THREAD_ATTR_USER, nullptr);
        if (cbt >= 0) sceKernelStartThread(cbt, 0, nullptr);
        else PERR("main", "callback thread failed: 0x%08X", (unsigned)cbt);
    }
    // The screen (§4.5 v0): pspDebugScreen text from here on.
    {
        char title[96];
        snprintf(title, sizeof title, BLITZ_APP_TITLE " %s build %s (%s)", BLITZ_VERSION, BLITZ_BUILD, BLITZ_COMMIT);
        host::start_overlay(title);
    }
#endif

    // 5. Probe-lite block (§7.3).
    {
        char line[256];
#if defined(__PSP__)
        host::MemStats m = host::mem_stats();
        snprintf(line, sizeof line, "PROBE v1 build=%s commit=%s launch=%s device=psp emulator=%d flavour=" BLITZ_FLAVOUR_NAME " free_kb=%d max_kb=%d heap_kb=%d",
                 BLITZ_BUILD, BLITZ_COMMIT, psplog::launch_id().c_str(), pspsys::is_emulator(), m.free_kb, m.max_kb, BLITZ_PSP_HEAP_KB);
#else
        snprintf(line, sizeof line, "PROBE v1 build=%s commit=%s launch=%s device=psp-host emulator=0 flavour=" BLITZ_FLAVOUR_NAME, BLITZ_BUILD,
                 BLITZ_COMMIT, psplog::launch_id().c_str());
#endif
        psplog::raw(line);
        host::StackSizes s;
        if (const char* kb = getenv("BLITZ_PSP_STACK_KB")) {
            s.game_fixed = s.game_process = (uint32_t)atoi(kb) * 1024;
        }
#if defined(__PSP__)
        // The game's processes run from a pool made now (psp_threads.cpp): 96 slots of 24 KB unless <app>/stackpool says.
        double slots = 96, kb = 24;
        if (switch_numbers("stackpool", slots, kb)) PLOG("main", "stackpool switch: %d slots of %d KB", (int)slots, (int)kb);
        s.game_process = (uint32_t)kb * 1024;
        host::set_stack_sizes(s);
        host::init_threads((int)slots, s.game_process);
#else
        host::set_stack_sizes(s);
#endif
        snprintf(line, sizeof line, "PROBE stack.runtime_kb=%u stack.game_start_kb=%u stack.game_fixed_kb=%u stack.game_process_kb=%u",
                 s.runtime / 1024, s.game_start / 1024, s.game_fixed / 1024, s.game_process / 1024);
        psplog::raw(line);
    }

    // The frontend's identity and paths (recompui-lite), the settings, the game.
#if defined(__PSP__)
    recompui_lite::set_paths(g_app, g_app);
#elif defined(BLITZ_PSP_KIT)
    recompui_lite::set_paths(g_app, kit_program.empty() ? std::filesystem::path(BLITZ_SOURCE_ROOT) : std::filesystem::path(kit_program));
#elif BLITZ_MODERN
    // A kit's NEWBLITZ folder as the app folder: its own houston/, packs/, rosters/ (the PSP's program path is its folder too).
    recompui_lite::set_paths(g_app, std::filesystem::exists(g_app / "rom/blitz.z64", ec) ? g_app : std::filesystem::path(BLITZ_SOURCE_ROOT));
#else
    recompui_lite::set_paths(g_app, BLITZ_SOURCE_ROOT);
#endif
    recompui::programconfig::set_program_name(blitz::program_name);
    recompui::programconfig::set_program_id(blitz::program_id);
    recomp::register_config_path(g_app);
    blitz::psp_register_settings();
    for (const auto& game : games()) recomp::register_game(game);
    blitz::register_overlays();

#if defined(BLITZ_PSP_KIT)
    if (!kit_out.empty()) return make_kit(rom_src, kit_out);
    PERR("main", "newblitz-kit: --make-kit <out> --rom <base ROM> [--program <staged resources>] [--app <folder>]");
    return 2;
#endif

    // 6. RDRAM (recomp::start asks the platform's platform_rdram for it) and the ROM provider.
#if defined(__PSP__)
    std::string rom_file = app + "/rom/blitz.z64";
#if BLITZ_MODERN
    // Original 1999 (Settings → Season → Game) is the game as it shipped: none of Modern's art, which is merged into this
    // folder's ROM. New Blitz Classic's plain ROM is beside it on the stick (the kit puts both there).
    if (blitz::original_game()) {
        std::string plain = pspsys::dir_name(app) + "/NBCLASSIC/rom/blitz.z64";
        if (pspsys::exists(plain)) {
            rom_file = plain;
            PLOG("main", "Original 1999: the plain ROM (%s)", plain.c_str());
        } else {
            PWARN("main", "Original 1999, but no %s: Modern's art stays", plain.c_str());
        }
    }
#endif
    if (!host::open_rom(rom_file, blitz::rom_hash)) {
        PERR("main", "no ROM: the game can't start (the overlay keeps the log on screen; HOME exits)");
        psplog::flush();
        for (;;) sceKernelSleepThreadCB();
    }
    if (pspsys::exists(g_input)) host::load_input_script(g_input);
    host::init_input();
#if defined(BLITZ_PSP_AUDIO)
    // Silent (v0) by default until the Media Engine audio (v2): v1 on the CPU took the game from 30 to 11-24 ticks a
    // second in play in PPSSPP (§14.14). The audio switch file turns v1 on; noaudio still forces v0.
    host::audio_init(!pspsys::exists(psplog::app_path("audio")) || pspsys::exists(psplog::app_path("noaudio")));
#endif
    {
        double at = -1, secs = -1;
        if (switch_numbers("freezevi", at, secs)) {
            g_freeze_for = secs;
            g_freeze_at = at;
            PWARN("main", "freezevi switch: the VI thread freezes at game second %.0f for %s", at,
                  secs < 0 ? "good" : (std::to_string((int)secs) + " s").c_str());
        }
    }
#else
    std::filesystem::path stored = g_app / (std::u8string(blitz::game_id) + u8".z64");
#if BLITZ_MODERN
    // The kit's ROM (Modern's art merged on the Mac, as on the PSP) when the app folder has one.
    if (std::filesystem::exists(g_app / "rom/blitz.z64", ec)) stored = g_app / "rom/blitz.z64";
#endif
    if (!rom_src.empty() && !std::filesystem::exists(stored, ec)) {
        std::filesystem::copy_file(rom_src, stored, ec);
        PLOG("rom", "stored %s as %s%s", rom_src.c_str(), stored.filename().string().c_str(), ec ? (" FAILED: " + ec.message()).c_str() : "");
    }
    if (!host::open_rom(stored.string(), blitz::rom_hash)) {
        psplog::shutdown("no ROM");
        return 1;
    }
    if (!g_input.empty()) host::load_input_script(g_input);
#endif

#if defined(__PSP__)
    // 7. Commands (host0 mailbox; the Worker when <app>/data/net.cfg is there) and the net, which connects in the background
    // and never holds up the boot.
    pspremote::Options ro;
    ro.app_dir = g_app.string();
    ro.host0_dir = "host0:/blitz-psp";
    ro.run_upload_first_s = 60;
    ro.run_upload_every_s = 300;
    pspremote::init(ro);
    pspremote::add_verb("live", v_live, nullptr, false);
    pspremote::add_verb("freeze", v_freeze, nullptr, false);
    pspremote::add_verb("pad", v_pad, nullptr, false);
    pspremote::add_verb("set", v_set, nullptr, false);
    pspremote::set_peek(peek_rdram);
    pspremote::set_stat_hook(stat_hook, nullptr);
    pspremote::start();
    if (pspremote::net_cfg().present) pspnet::start(pspremote::net_cfg().wifi_profile);
#if BLITZ_MODERN
    pspremote::add_verb("rosters", v_rosters, nullptr, false);
    blitz::rosters_check_start();   // §6: the saved pack now, a check once the net is up
#endif
    // 8. The watchdog, above the game's threads.
    if (!pspsys::thread_start("blitz_watchdog", watchdog, nullptr, 32 * 1024, 0x11)) PERR("main", "watchdog failed to start");
#else
#if BLITZ_MODERN
    blitz::rosters_check_start();   // BLITZ_ROSTER_SOURCE=<hub base URL>: one check in the background
#endif
    // 7. Net: PSP only. 8. The watchdog.
    if (!pspsys::thread_start("blitz_watchdog", watchdog, nullptr, 256 * 1024, 0x11)) PERR("main", "watchdog failed to start");
#endif

#if BLITZ_MODERN
    // engine32.cpp's team loader reads the ROM's first 1.3 MB once; now, before the renderer and the game take the heap.
    blitz::e32_preload_rom_teams();
#endif
    // 9. recomp::start with the PSP's renderer, audio and input, and no launcher.
    recomp::Version version{};
    recomp::Version::from_string(BLITZ_VERSION, version);
    recomp::rsp::callbacks_t rsp_callbacks{ .get_rsp_microcode = get_rsp_microcode };
#if defined(__PSP__)
    // The GE renderer (M7, psp_gfx.cpp); the switch file nogfx keeps the null one (M5) for an A/B.
    ultramodern::renderer::callbacks_t renderer_callbacks{ .create_render_context = host::create_psp_renderer };
#else
    ultramodern::renderer::callbacks_t renderer_callbacks{ .create_render_context = host::create_null_renderer };
#endif
    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = []() -> ultramodern::gfx_callbacks_t::gfx_data_t { return nullptr; },
#if defined(__PSP__)
        .create_window = [](ultramodern::gfx_callbacks_t::gfx_data_t) { return ultramodern::renderer::WindowHandle{ (void*)1 }; },
#else
        .create_window = [](ultramodern::gfx_callbacks_t::gfx_data_t) { return ultramodern::renderer::WindowHandle{ (void*)1, nullptr }; },
#endif
        .update_gfx = update_gfx,
    };
    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = blitz::audio::queue_samples,
        .get_frames_remaining = blitz::audio::get_frames_remaining,
        .set_frequency = blitz::audio::set_frequency,
    };
    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = [] {},
        .get_input = host::get_input,
        .set_rumble = [](int, bool) {},
        .get_connected_device_info = connected,
    };
    ultramodern::events::callbacks_t events_callbacks{
        .vi_callback = [] {
            uint64_t vi = ++g_vi;
            count_processes();
            if (g_lockstep) lockstep_vi(vi);
            double at = g_freeze_at.load();
            if (at >= 0 && g_game_started && host::game_seconds() >= at) freeze_vi_thread();
        },
        .gfx_init_callback = nullptr,
    };
    ultramodern::error_handling::callbacks_t error_callbacks{ .message_box = recompui::message_box };
    ultramodern::threads::callbacks_t threads_callbacks{ .get_game_thread_name = get_game_thread_name };

#if defined(ULTRAMODERN_LOCKSTEP)
    if (g_lockstep) ultramodern::lockstep::enable();
#endif
    PLOG("main", "recomp::start");
    recomp::start(version, {}, rsp_callbacks, renderer_callbacks, audio_callbacks, input_callbacks, gfx_callbacks,
                  events_callbacks, error_callbacks, threads_callbacks);
#if defined(__PSP__)
    pspremote::go_home("recomp::start returned");
#else
    psplog::shutdown("recomp::start returned");
#endif
    return 0;
}
