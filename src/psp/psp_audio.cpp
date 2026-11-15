// Audio v1 on the PSP (docs/PSP_PORT.md §4.6, M8): the game's audio tasks run RSPRecomp's n_aspMain (build/rsp/n_aspMain.cpp,
// the SISD vector-unit path) on ultramodern's SP Task thread, and the buffers the game hands to osAiSetNextBuffer go through
// a ring to an output thread on sceAudioSRCChReserve at the game's own rate (the hardware resamples to 44.1 kHz), right
// channel first in RDRAM as on the desktop (src/main/audio.cpp). The switch file <app>/noaudio is v0 again: no microcode,
// the silent sink (backends_common.cpp) paces the game.
//
// Pacing: ai.cpp asks how many frames are still queued (pending_frames: the ring plus the block being played); the output
// thread takes one block at a time and blocks in sceAudioSRCOutputBlocking until the hardware wants the next, so the ring
// drains at exactly the output rate. Over 200 ms queued, a buffer is dropped (the desktop's safety valve). A block that
// finds the ring short is padded with silence and counted as an underrun (once the game has sent its first buffer).
#if defined(BLITZ_PSP_AUDIO)
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <pspaudio.h>
#include <pspthreadman.h>

#include "blitz_audio.h"
#include "recompui/config.h"
#include "psp_log.h"
#include "psp_sys.h"
#include "host/host_backends.h"

extern RspUcodeFunc n_aspMain;  // build/rsp/n_aspMain.cpp

namespace {
    constexpr uint32_t block = 512;               // frames per sceAudioSRCOutputBlocking (23 ms at 22,050 Hz)
    constexpr uint32_t ring_frames = 16384;       // 0.74 s at 22,050 Hz; the game keeps it far lower
    constexpr uint32_t max_queued_ms = 200;

    bool g_on = false;                            // v1 (decided at boot: no noaudio switch)
    std::mutex g_mutex;
    int16_t g_ring[ring_frames * 2];
    uint32_t g_head = 0, g_fill = 0;              // frames: read position and how many are queued
    std::atomic<uint32_t> g_rate{22050}, g_reserved_rate{0}, g_last{0}, g_in_flight{0};
    std::atomic<bool> g_started{false}, g_mute{false};
    // counters (since boot) and the perf window's
    std::atomic<uint64_t> g_frames_in{0}, g_frames_out{0}, g_pad_frames{0}, g_underruns{0}, g_drops{0};
    std::atomic<uint64_t> g_tasks{0}, g_task_us{0}, g_task_worst_us{0};
    uint64_t g_first_us = 0;
    int16_t g_out[block * 2];

    int volume() {
        int v = 100;
        try {
            v = recompui::config::sound::get_main_volume();
        } catch (...) {
        }
        return std::clamp(v, 0, 100) * PSP_AUDIO_VOLUME_MAX / 100;
    }

    // The output thread (priority 0x12, above the game: a late block is an audible gap).
    void output_thread(void*) {
        int reserved = -1;
        // Not before the game's first buffer: until then the rate is the runtime's placeholder (48 kHz), not the game's.
        while (!g_started.load()) pspsys::sleep_ms(5);
        for (;;) {
            uint32_t rate = g_rate.load();
            if (reserved < 0 || rate != g_reserved_rate.load()) {
                if (reserved >= 0) {
                    // The channel is released only once its last block has played out (busy until then).
                    int rc, tries = 0;
                    while ((rc = sceAudioSRCChRelease()) < 0 && tries++ < 500) pspsys::sleep_ms(2);
                    if (rc < 0) PWARN("audio", "sceAudioSRCChRelease: 0x%08X", (unsigned)rc);
                    PLOG("audio", "output: the game rate changed from %u to %u Hz", g_reserved_rate.load(), rate);
                }
                reserved = sceAudioSRCChReserve(block, (int)rate, 2);
                if (reserved < 0) {
                    PERR("audio", "sceAudioSRCChReserve(%u, %u, 2): 0x%08X; trying again in 1 s", block, rate, (unsigned)reserved);
                    pspsys::sleep_ms(1000);
                    continue;
                }
                g_reserved_rate = rate;
                PLOG("audio", "output: sceAudioSRCChReserve(%u frames, %u Hz, stereo) ok", block, rate);
            }
            uint32_t got = 0;
            {
                std::lock_guard l(g_mutex);
                got = std::min(g_fill, block);
                for (uint32_t i = 0; i < got; i++) {
                    uint32_t at = (g_head + i) % ring_frames;
                    g_out[2 * i] = g_ring[2 * at];
                    g_out[2 * i + 1] = g_ring[2 * at + 1];
                }
                g_head = (g_head + got) % ring_frames;
                g_fill -= got;
                g_in_flight = got;  // the game's frames in it: padding isn't the game's audio, so ai.cpp mustn't wait on it
            }
            if (got < block) {
                memset(g_out + 2 * got, 0, (block - got) * 4);
                if (g_started.load()) {
                    g_underruns++;
                    g_pad_frames += block - got;
                }
            }
            g_frames_out += got;
            sceAudioSRCOutputBlocking(g_mute.load() ? 0 : volume(), g_out);
            g_in_flight = 0;
        }
    }

    // The audio task, timed for [perf].
    RspExitReason timed_aspMain(uint8_t* rdram, uint32_t ucode_addr) {
        uint64_t t0 = pspsys::now_us();
        RspExitReason r = n_aspMain(rdram, ucode_addr);
        uint64_t us = pspsys::now_us() - t0;
        g_tasks++;
        g_task_us += us;
        if (us > g_task_worst_us.load()) g_task_worst_us = us;
        return r;
    }
}

namespace host {
    bool audio_v1() { return g_on; }

    // At boot (psp_main.cpp): v1 unless the switch file noaudio is there.
    void audio_init(bool noaudio) {
        g_on = !noaudio;
        if (!g_on) {
            PLOG("audio", "audio v0 (no microcode, a silent sink paces the game; the audio switch file turns v1 on)");
            return;
        }
        if (!pspsys::thread_start("blitz_audio", output_thread, nullptr, 16 * 1024, 0x12)) {
            PERR("audio", "the output thread didn't start: audio v0");
            g_on = false;
            return;
        }
        PLOG("audio", "v1: n_aspMain on the SP Task thread, a %u-frame ring, %u-frame blocks to sceAudioSRC", ring_frames, block);
    }

    RspUcodeFunc* audio_microcode() { return g_on ? timed_aspMain : host_silent_aspMain; }

    std::string audio_perf_fields(double dt) {
        if (!g_on) return "audio_ms=0.0(off)";
        static uint64_t last_tasks = 0, last_us = 0, last_in = 0, last_out = 0, last_under = 0, last_pad = 0;
        uint64_t tasks = g_tasks.load(), us = g_task_us.load(), in = g_frames_in.load(), out = g_frames_out.load();
        uint64_t under = g_underruns.load(), pad = g_pad_frames.load();
        uint64_t dtasks = tasks - last_tasks, dus = us - last_us;
        double per = dtasks ? dus / 1000.0 / dtasks : 0.0;
        double expected = g_rate.load() * dt;
        double pct = expected > 0 ? 100.0 * (double)(out - last_out) / expected : 0.0;
        double since = g_first_us ? (pspsys::now_us() - g_first_us) / 1e6 : 0;
        double total_pct = since > 1 ? 100.0 * (double)out / (g_rate.load() * since) : 0.0;
        char buf[256];
        snprintf(buf, sizeof buf, "audio_ms=%.2f(cpu) audio_cpu_ms/s=%.0f audio_tasks=%" PRIu64 " audio_worst_ms=%.1f audio_out=%.1f%% "
                 "audio_out_all=%.1f%% audio_in=%" PRIu64 " underruns=%" PRIu64 "(%" PRIu64 " frames) audio_drops=%" PRIu64 " queued=%u",
                 per, dt > 0 ? dus / 1000.0 / dt : 0.0, dtasks, g_task_worst_us.exchange(0) / 1000.0, pct, total_pct, in - last_in,
                 under - last_under, pad - last_pad, g_drops.load(), pending_frames_now());
        last_tasks = tasks; last_us = us; last_in = in; last_out = out; last_under = under; last_pad = pad;
        return buf;
    }

    uint32_t pending_frames_now() {
        std::lock_guard l(g_mutex);
        return g_fill + g_in_flight.load();
    }

    AudioStats audio_stats() {
        if (!g_on) return silent_audio_stats();
        return { g_frames_in.load(), g_drops.load(), g_rate.load() };
    }
}

namespace blitz::audio {
    bool reset(uint32_t f) { return g_on ? true : host::silent::reset(f); }
    void set_frequency(uint32_t freq) {
        if (!g_on) return host::silent::set_frequency(freq);
        g_rate = freq ? freq : 22050;
        PLOG("audio", "game rate %u Hz", g_rate.load());
    }
    void queue_samples(int16_t* data, size_t sample_count) {
        if (!g_on) return host::silent::queue_samples(data, sample_count);
        uint32_t frames = (uint32_t)(sample_count / 2);
        g_last = frames;
        std::lock_guard l(g_mutex);
        if (!g_started.exchange(true)) g_first_us = pspsys::now_us();
        if ((g_fill + g_in_flight.load()) * 1000ull / g_rate.load() > max_queued_ms || g_fill + frames > ring_frames) {
            g_drops++;
            return;
        }
        uint32_t tail = (g_head + g_fill) % ring_frames;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t at = (tail + i) % ring_frames;
            // RDRAM's word byteswap also swaps each pair of 16-bit samples, so right comes first (audio.cpp).
            g_ring[2 * at] = data[2 * i + 1];
            g_ring[2 * at + 1] = data[2 * i];
        }
        g_fill += frames;
        g_frames_in += frames;
    }
    size_t get_frames_remaining() { return pending_frames(); }
    void set_mute(bool mute) {
        if (!g_on) return host::silent::set_mute(mute);
        g_mute = mute;
    }
    uint32_t pending_frames() { return g_on ? host::pending_frames_now() : host::silent::pending_frames(); }
    uint32_t last_buffer_frames() { return g_on ? g_last.load() : host::silent::last_buffer_frames(); }
    uint32_t game_rate() { return g_on ? g_rate.load() : host::silent::game_rate(); }
}
#endif
