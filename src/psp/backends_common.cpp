// The parts of the PSP layer that are the same on the PSP and on psp-host (docs/PSP_PORT.md §4.4-4.6, v0):
//   renderer      null: send_dl completes at once and counts, update_screen counts frames (§4.5 v0); on the PSP the
//                 debug-screen overlay (psp_overlay.cpp) shows the counts
//   audio         silent, paced like a 22,050 Hz device would drain it, so ai.cpp's FIFO model behaves (§4.6 v0)
//   input         a script in dev_input's line format: "<seconds since the game started> <what> [hold seconds]", plus lines
//                 added while running (the "live" command); the PSP merges it with sceCtrl (psp_input.cpp)
// Moved out of host/host_backends.cpp unchanged for M5 (apart from script_input's name and add_input_line).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"

#include "blitz_audio.h"
#include "psp_log.h"
#include "host/host_backends.h"

using namespace std::chrono;

// ================================================================== renderer (null) ==========================================
namespace {
    std::atomic<uint64_t> g_dl_count{0}, g_frames{0}, g_dummy{0}, g_shown{0}, g_skipped{0};
    std::atomic<bool> g_ge_mode{false};

    class NullRenderer final : public ultramodern::renderer::RendererContext {
    public:
        NullRenderer() {
            setup_result = ultramodern::renderer::SetupResult::Success;
            chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        }
        bool valid() override { return true; }
        bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
        void enable_instant_present() override {}
        void send_dl(const OSTask*) override { g_dl_count++; }
        void send_dummy_workload(uint32_t) override { g_dummy++; }
        void update_screen() override { g_frames++; }
        void shutdown() override {}
        uint32_t get_display_framerate() const override { return 60; }
        float get_resolution_scale() const override { return 1.0f; }
    };
}

namespace host {
    std::unique_ptr<ultramodern::renderer::RendererContext> create_null_renderer(uint8_t*, ultramodern::renderer::WindowHandle, bool) {
        PLOG("gfx", "null renderer: display lists complete at once and are counted");
        return std::make_unique<NullRenderer>();
    }
    GfxStats gfx_stats() { return { g_dl_count.load(), g_frames.load(), g_dummy.load(), g_shown.load(), g_skipped.load() }; }
    void gfx_count(int dl, int frames, int shown, int skipped, int dummy) {
        if (dl) g_dl_count += dl;
        if (frames) g_frames += frames;
        if (shown) g_shown += shown;
        if (skipped) g_skipped += skipped;
        if (dummy) g_dummy += dummy;
    }
    bool gfx_mode_ge() { return g_ge_mode.load(); }
    void set_gfx_mode_ge(bool on) { g_ge_mode = on; }
}

// ================================================================== audio (silent) ===========================================
// The game paces its audio thread from osAiGetStatus/osAiGetLength, which ai.cpp answers from how many frames are still
// queued on the host. Here nothing plays: queued frames drain at the game's own rate in real time (or, in lockstep, at
// one VI's worth per VI), capped at 200 ms like the desktop's queue.
namespace {
    std::mutex g_audio_mutex;
    double g_audio_queued = 0;             // game-rate frames
    uint32_t g_audio_rate = 22050, g_audio_last = 0;
    steady_clock::time_point g_audio_t = steady_clock::now();
    uint64_t g_audio_vi = 0;
    uint64_t g_audio_frames_total = 0, g_audio_dropped = 0;

    void drain_locked() {
        if (host::lockstep()) {
            uint64_t vi = host::vi_count();
            g_audio_queued -= (double)(vi - g_audio_vi) * g_audio_rate / 60.0;
            g_audio_vi = vi;
        } else {
            auto now = steady_clock::now();
            g_audio_queued -= duration<double>(now - g_audio_t).count() * g_audio_rate;
            g_audio_t = now;
        }
        if (g_audio_queued < 0) g_audio_queued = 0;
    }
}

// The silent sink's functions (host::silent). blitz::audio is them on psp-host and in the M5-M7 EBOOTs; with the PSP's audio
// v1 (psp_audio.cpp, BLITZ_PSP_AUDIO) blitz::audio is the real output, which falls back to these with the noaudio switch.
namespace host::silent {
    bool reset(uint32_t) { return true; }
    void set_frequency(uint32_t freq) {
        std::lock_guard l(g_audio_mutex);
        drain_locked();
        g_audio_rate = freq ? freq : 22050;
        PLOG("audio", "silent sink at %u Hz", g_audio_rate);
    }
    void queue_samples(int16_t*, size_t sample_count) {
        std::lock_guard l(g_audio_mutex);
        drain_locked();
        uint32_t frames = (uint32_t)(sample_count / 2);
        g_audio_last = frames;
        if (g_audio_queued * 1000.0 / g_audio_rate > 200.0) {
            g_audio_dropped++;
            return;
        }
        g_audio_queued += frames;
        g_audio_frames_total += frames;
    }
    size_t get_frames_remaining() { return pending_frames(); }
    void set_mute(bool) {}
    uint32_t pending_frames() {
        std::lock_guard l(g_audio_mutex);
        drain_locked();
        return (uint32_t)g_audio_queued;
    }
    uint32_t last_buffer_frames() { return g_audio_last; }
    uint32_t game_rate() { return g_audio_rate; }
}
#if !defined(BLITZ_PSP_AUDIO)
namespace blitz::audio {
    bool reset(uint32_t f) { return host::silent::reset(f); }
    void set_frequency(uint32_t freq) { host::silent::set_frequency(freq); }
    void queue_samples(int16_t* d, size_t n) { host::silent::queue_samples(d, n); }
    size_t get_frames_remaining() { return host::silent::get_frames_remaining(); }
    void set_mute(bool m) { host::silent::set_mute(m); }
    uint32_t pending_frames() { return host::silent::pending_frames(); }
    uint32_t last_buffer_frames() { return host::silent::last_buffer_frames(); }
    uint32_t game_rate() { return host::silent::game_rate(); }
}
namespace host {
    AudioStats audio_stats() { return silent_audio_stats(); }
}
#endif

namespace host {
    AudioStats silent_audio_stats() {
        std::lock_guard l(g_audio_mutex);
        return { g_audio_frames_total, g_audio_dropped, g_audio_rate };
    }
}

// The audio task: v0 runs no microcode, the task simply completes (§4.6).
RspExitReason host_silent_aspMain(uint8_t*, uint32_t) { return RspExitReason::Broke; }

// ================================================================== input (script) ===========================================
namespace {
    struct InputEvent {
        double start, end;
        uint16_t buttons = 0;
        bool stick = false;
        float x = 0, y = 0;
    };
    std::vector<InputEvent> g_events;
    std::mutex g_input_mutex;

    uint16_t button_mask(const std::string& name) {
        static const std::pair<const char*, uint16_t> table[] = {
            {"A", 0x8000}, {"B", 0x4000}, {"Z", 0x2000}, {"START", 0x1000},
            {"UP", 0x0800}, {"DOWN", 0x0400}, {"LEFT", 0x0200}, {"RIGHT", 0x0100},
            {"L", 0x0020}, {"R", 0x0010},
            {"CUP", 0x0008}, {"CDOWN", 0x0004}, {"CLEFT", 0x0002}, {"CRIGHT", 0x0001},
            // the PSP's own names, as the default map (psp_input.cpp) sends them: cross A, square B, triangle C-up,
            // circle C-right, L Z (live "CROSS" was refused on the PSP-1001, 2026-11-04)
            {"CROSS", 0x8000}, {"SQUARE", 0x4000}, {"TRIANGLE", 0x0008}, {"CIRCLE", 0x0001}, {"LTRIGGER", 0x2000},
            {"RTRIGGER", 0x0010},
        };
        for (auto& [n, m] : table) {
            if (name == n) return m;
        }
        PWARN("input", "unknown button '%s'", name.c_str());
        return 0;
    }
}

namespace host {
    // One script line, "<seconds> <what> [hold seconds]" (what: A, B, Z, START, UP, ..., A+B, or STICK x,y), at that game
    // time; at = -1: the line has no time of its own and starts now ("<what> [hold]", the live command). False if it isn't one.
    bool add_input_line(const std::string& text, double at) {
        std::string line = text;
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::stringstream ss(line);
        double t = at;
        std::string what;
        if (at < 0) {
            std::string first;
            if (!(ss >> first)) return false;
            char* end = nullptr;
            double v = strtod(first.c_str(), &end);
            if (end != first.c_str() && *end == 0) {
                // "+2 A": two seconds from now; "12.5 A": at 12.5 s of the game
                t = first[0] == '+' ? game_seconds() + v : v;
                if (!(ss >> what)) return false;
            } else {
                t = game_seconds();
                what = first;
            }
        } else if (!(ss >> t >> what)) {
            return false;
        }
        InputEvent ev{ t, t + 0.15 };
        if (what == "STICK") {
            std::string xy;
            ss >> xy;
            if (sscanf(xy.c_str(), "%f,%f", &ev.x, &ev.y) != 2) return false;
            ev.stick = true;
        } else {
            std::stringstream parts(what);
            std::string b;
            while (std::getline(parts, b, '+')) ev.buttons |= button_mask(b);
            if (ev.buttons == 0) return false;
        }
        double hold;
        if (ss >> hold) ev.end = t + hold;
        std::lock_guard l(g_input_mutex);
        g_events.push_back(ev);
        return true;
    }

    int load_input_script(const std::string& path) {
        std::ifstream f(path);
        if (!f) {
            PWARN("input", "no input script at %s", path.c_str());
            return 0;
        }
        std::string line;
        int n = 0;
        while (std::getline(f, line)) {
            std::stringstream ss(line);
            double t;
            if (!(ss >> t)) continue;  // blank lines and comments
            if (add_input_line(line, t)) n++;
        }
        PLOG("input", "%d scripted presses from %s", n, path.c_str());
        return n;
    }

    bool script_input(int controller, uint16_t* buttons, float* x, float* y) {
        if (controller != 0) return false;
        double t = game_seconds();
        uint16_t b = 0;
        float sx = 0, sy = 0;
        {
            std::lock_guard l(g_input_mutex);
            for (const InputEvent& e : g_events) {
                if (t < e.start || t >= e.end) continue;
                b |= e.buttons;
                if (e.stick) {
                    sx = e.x;
                    sy = e.y;
                }
            }
        }
        *buttons = b;
        *x = sx;
        *y = sy;
        return true;
    }
}

