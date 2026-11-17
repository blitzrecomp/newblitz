// The frame counter (Settings → General → Frame Counter; docs/IOS_FEASIBILITY.md "Next pass: stutter"): the game's
// own frames per second and the longest gap between two of them in the last second, at the top right. A frame is
// the game swapping its framebuffer (the VI origin changing), counted on the VI thread, so it measures what the game
// actually shows, whatever the display's refresh. Every 5 seconds the log gets a line too, shown or not:
//   [fps] 30.0 fps, worst gap 50 ms, 14 frames held 3 vblanks, 0 hitches
// so a stutter report comes with numbers (tools/stutter_report.py sums a log up). Gaps are whole 60 Hz vblanks
// (33, 50, 67 ms...). Frames held 3 vblanks are counted apart from hitches (4 or more vblanks, over 58 ms), which are the
// stutter. (Before 2026-11-14 the counter read the renderer's copy of the VI registers, which lags now and then, and so
// reported about 5 frames a second held 3 vblanks, each paired with one held 1, everywhere: that was the counter, not the
// game, which swaps every 2 VIs.) It can't see the display's pacing (ProMotion), only the game's.
//
// Potato Mode on Android also has a watchdog, on its own thread, because the VI thread can't see its own stall. Any gap of 250 ms
// or more in VI ticks, game swaps or first-screen presents gets one line when it ends (and again if it's still going at 3 s, 6 s...):
//   [stall] 412 ms (game 412, vi 0, first 400, watchdog late 2): gfx uploads 380 S, present frame 390 D, work gpu 300 S, tex gpu 350 S; evict 40, ...
// with, per renderer thread, the phase it sat in longest and the kernel's state for it 250 ms in. A budget of 30 lines plus one every
// 2 s keeps it from writing a flood of its own. Every 5 seconds, [frames] counts swaps and both screens' presents over the same
// interval. It only wakes to read atomics, and reads /proc once a second at most outside a stall, never per frame.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"

#include <cstdint>
#include <cstdlib>
extern "C" void plume_save_pipeline_cache();  // lib/rt64's Plume
extern "C" void blitz_rt64_stats(uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*);  // rt64_raster_shader_cache.cpp

// The same struct as rt64_raster_shader_cache.cpp's: all running totals, but presentRule.
struct BlitzRt64Watch {
    uint64_t evictions, uploads, uploadWaitUs, compileMs, firstPresents, secondPresents;
    uint64_t viPasses, viPassUs, acquireUs, presentUs, secondPasses, secondPassUs, textureDraws, texturesPast256;
    uint64_t presentPicks, presentSkips, presentNewer0, presentNewer1, presentNewer2, presentHoldUs, waitPresentUs, workloadFullUs, presentRule, pictureFirst;
    uint64_t uiSkips;
};
extern "C" void blitz_rt64_watch_read(BlitzRt64Watch* w);

#ifdef __ANDROID__
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

extern "C" void blitz_rt64_watch_peaks(uint64_t* vi_pass_max_us, uint64_t* texture_highest);
extern "C" void blitz_rt64_watch_sizes(uint32_t* size);
extern "C" const char* blitz_rt64_watch_phase(int thread, int* tid);
extern "C" int plume_narrow_barriers_on();
#endif

namespace blitz { bool replay_is_open(); }  // replay_screen.cpp
namespace blitz { void rescue_watch(double frames_away_s); }  // savestate.cpp: the freeze rescue
using blitz::replay_is_open;

namespace {
    std::atomic<float> latest_fps{0.0f};
    using Clock = std::chrono::steady_clock;

    // The present queue over an interval (rt64_present_queue.cpp): presents picked, drawn and dropped, how
    // many newer ones waited at each pick, the hold (a drawn present's workload done to the next workload let go), the workload
    // thread's wait for presents and the gfx thread's for a free workload, the drop rule, presents drawn picture-first, and of
    // those, the ones that left the first window as it was (BLITZ_DS_UI_EVERY).
    //   [present] 5.0 s: picked 120, drawn 61, skipped 59, newer at pick 0:61 1:59 2+:0, hold 18.9 ms, work wait-present 1150 ms, ...
    void say_present(const BlitzRt64Watch& w, const BlitzRt64Watch& base, double secs) {
        const unsigned long long picks = w.presentPicks - base.presentPicks, skips = w.presentSkips - base.presentSkips;
        const unsigned long long drawn = picks > skips ? picks - skips : 0;
        fprintf(stderr, "[present] %.1f s: picked %llu, drawn %llu, skipped %llu, newer at pick 0:%llu 1:%llu 2+:%llu, hold %.1f ms, work wait-present %llu ms, "
                        "gfx workload-full %llu ms, rule %llu, picture first %llu, ui skipped %llu\n",
                secs, picks, drawn, skips, (unsigned long long)(w.presentNewer0 - base.presentNewer0), (unsigned long long)(w.presentNewer1 - base.presentNewer1),
                (unsigned long long)(w.presentNewer2 - base.presentNewer2), (w.presentHoldUs - base.presentHoldUs) / (drawn > 0 ? (double)drawn : 1.0) / 1000.0,
                (unsigned long long)((w.waitPresentUs - base.waitPresentUs) / 1000), (unsigned long long)((w.workloadFullUs - base.workloadFullUs) / 1000),
                (unsigned long long)w.presentRule, (unsigned long long)(w.pictureFirst - base.pictureFirst), (unsigned long long)(w.uiSkips - base.uiSkips));
    }

    // VI thread
    uint32_t last_origin = 0;
    Clock::time_point last_frame{}, window_start{}, log_start{};
    int frames = 0, log_frames = 0, log_held3 = 0, log_hitches = 0;
    double worst_ms = 0, log_worst_ms = 0;
    // VI ticks each frame was shown for, counted rather than timed (a late VI thread can't fake a held frame):
    // 1, 2, 3, 4 or more.
    int ticks_since_swap = 0;
    int log_ticks[4] = {};
    std::atomic<uint64_t> swaps_total{0};  // the game's swaps this launch (game_frames), read on any thread

#ifdef __ANDROID__
    std::atomic<uint64_t> watch_vi_ticks{0}, watch_swaps{0}, watch_save_ms{0};
    std::atomic<bool> watch_saving{false};

    // No exception to catch on a device out of memory, as a std::thread would throw on the VI thread.
    bool start_detached(void* (*run)(void*)) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        const bool started = pthread_create(&thread, &attr, run, nullptr) == 0;
        pthread_attr_destroy(&attr);
        return started;
    }

    // Off the VI thread: a write to a slow card shouldn't hold it, and the watchdog needs its own thread clear to watch.
    void* save_pipeline_cache_thread(void*) {
        pthread_setname_np(pthread_self(), "Blitz Cache");
        const Clock::time_point start = Clock::now();
        plume_save_pipeline_cache();
        watch_save_ms += (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        watch_saving = false;
        return nullptr;
    }

    struct TaskStat { long faults; char state; };

    TaskStat read_task_stat(const char* path) {
        TaskStat got{0, '?'};
        int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return got;
        char text[512];
        ssize_t n = ::read(fd, text, sizeof(text) - 1);
        ::close(fd);
        if (n <= 0) return got;
        text[n] = 0;
        const char* name_end = strrchr(text, ')');  // a thread's name can hold anything
        if (name_end == nullptr) return got;
        unsigned long faults = 0;
        char state = '?';
        if (sscanf(name_end + 1, " %c %*d %*d %*d %*d %*d %*u %*u %*u %lu", &state, &faults) == 2) {
            got.faults = (long)faults;
            got.state = state;
        }
        return got;
    }

    constexpr int watch_threads = 4;  // gfx, present, workload, texture: RT64's BlitzWatchThread

    struct WatchFaults { long all; long thread[watch_threads]; char state[watch_threads]; };

    WatchFaults read_watch_faults() {
        WatchFaults f{};
        f.all = read_task_stat("/proc/self/stat").faults;
        for (int i = 0; i < watch_threads; i++) {
            int tid = 0;
            blitz_rt64_watch_phase(i, &tid);
            f.state[i] = '?';
            if (tid == 0) continue;
            char path[48];
            snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
            const TaskStat stat = read_task_stat(path);
            f.thread[i] = stat.faults;
            f.state[i] = stat.state;
        }
        return f;
    }

    int watch_ms(Clock::time_point from, Clock::time_point to) {
        return (int)std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    }

    unsigned long long watch_delta(uint64_t now, uint64_t before) { return (unsigned long long)(now - before); }

    void* watchdog(void*) {
        pthread_setname_np(pthread_self(), "Blitz Watch");
        constexpr int tick_ms = 50, stall_ms = 250;

        Clock::time_point now = Clock::now(), last = now, vi_at = now, swap_at = now, first_at = now, report_at = now, faults_at = now, token_at = now;
        Clock::time_point phase_at[watch_threads];
        const char* phase[watch_threads] = {};
        const char* stuck[watch_threads] = {};
        int stuck_ms[watch_threads] = {};
        char states[watch_threads] = {'?', '?', '?', '?'};
        for (int i = 0; i < watch_threads; i++) phase_at[i] = now;

        BlitzRt64Watch w{};
        blitz_rt64_watch_read(&w);
        BlitzRt64Watch quiet = w, report_base = w;
        // A first window left as it was on purpose (BLITZ_DS_UI_EVERY) has moved on as much as one presented.
        uint64_t vi_seen = watch_vi_ticks.load(), swaps_seen = watch_swaps.load(), first_seen = w.firstPresents + w.uiSkips;
        uint64_t quiet_swaps = swaps_seen, report_swaps = swaps_seen, quiet_save = watch_save_ms.load(), report_save = quiet_save;
        WatchFaults faults_quiet = read_watch_faults();
        bool stalled = false;
        int peak_game = 0, peak_vi = 0, peak_first = 0, peak_late = 0, still_at = 0, tokens = 30, hidden = 0;

        int tids[watch_threads] = {};
        for (int i = 0; i < watch_threads; i++) blitz_rt64_watch_phase(i, &tids[i]);
        fprintf(stderr, "[stall] watching: gfx tid %d, present %d, workload %d, texture %d\n", tids[0], tids[1], tids[2], tids[3]);

        auto say_stall = [&](const char* how, int total) {
            if (tokens <= 0) {
                hidden++;
                return;
            }
            tokens--;
            const WatchFaults f = read_watch_faults();
            char more[32] = "";
            if (hidden > 0) snprintf(more, sizeof(more), " (%d not shown)", hidden);
            hidden = 0;
            char line[640];
            snprintf(line, sizeof(line),
                     "[stall] %s%d ms (game %d, vi %d, first %d, watchdog late %d): gfx %s %d %c, present %s %d %c, work %s %d %c, tex %s %d %c; "
                     "evict %llu, uploads %llu, upload wait %llu ms, save %llu ms%s, compile %llu ms; swaps %llu, first %llu, second %llu; "
                     "major faults all %ld, gfx %ld, present %ld, work %ld, tex %ld%s\n",
                     how, total, peak_game, peak_vi, peak_first, peak_late,
                     stuck[0] != nullptr ? stuck[0] : "-", stuck_ms[0], states[0], stuck[1] != nullptr ? stuck[1] : "-", stuck_ms[1], states[1],
                     stuck[2] != nullptr ? stuck[2] : "-", stuck_ms[2], states[2], stuck[3] != nullptr ? stuck[3] : "-", stuck_ms[3], states[3],
                     watch_delta(w.evictions, quiet.evictions), watch_delta(w.uploads, quiet.uploads), watch_delta(w.uploadWaitUs, quiet.uploadWaitUs) / 1000,
                     watch_delta(watch_save_ms.load(), quiet_save), watch_saving.load() ? " and saving" : "", watch_delta(w.compileMs, quiet.compileMs),
                     watch_delta(swaps_seen, quiet_swaps), watch_delta(w.firstPresents, quiet.firstPresents), watch_delta(w.secondPresents, quiet.secondPresents),
                     f.all - faults_quiet.all, f.thread[0] - faults_quiet.thread[0], f.thread[1] - faults_quiet.thread[1], f.thread[2] - faults_quiet.thread[2], f.thread[3] - faults_quiet.thread[3], more);
            fputs(line, stderr);
        };

        for (;;) {
            usleep(tick_ms * 1000);
            now = Clock::now();
            const int late = watch_ms(last, now) - tick_ms;
            last = now;
            if (watch_ms(token_at, now) >= 2000) {
                token_at = now;
                if (tokens < 30) tokens++;
            }

            blitz_rt64_watch_read(&w);
            const uint64_t vi = watch_vi_ticks.load(std::memory_order_relaxed), swaps = watch_swaps.load(std::memory_order_relaxed);
            if (vi != vi_seen) { vi_seen = vi; vi_at = now; }
            if (swaps != swaps_seen || replay_is_open()) { swaps_seen = swaps; swap_at = now; }  // a replay stops the game on purpose
            if (w.firstPresents + w.uiSkips != first_seen) { first_seen = w.firstPresents + w.uiSkips; first_at = now; }

            for (int i = 0; i < watch_threads; i++) {
                int tid = 0;
                const char* p = blitz_rt64_watch_phase(i, &tid);
                if (p != phase[i]) {
                    phase[i] = p;
                    phase_at[i] = now;
                }
            }

            const int vi_age = watch_ms(vi_at, now), game_age = watch_ms(swap_at, now), first_age = watch_ms(first_at, now);
            const int worst = std::max(vi_age, std::max(game_age, first_age));

            if (!stalled && worst >= stall_ms) {
                stalled = true;
                peak_game = peak_vi = peak_first = peak_late = 0;
                still_at = 3000;
                for (int i = 0; i < watch_threads; i++) {
                    stuck[i] = nullptr;
                    stuck_ms[i] = 0;
                }
                const WatchFaults f = read_watch_faults();
                for (int i = 0; i < watch_threads; i++) states[i] = f.state[i];
            }

            if (stalled) {
                peak_game = std::max(peak_game, game_age);
                peak_vi = std::max(peak_vi, vi_age);
                peak_first = std::max(peak_first, first_age);
                peak_late = std::max(peak_late, late);
                for (int i = 0; i < watch_threads; i++) {
                    const int dwell = watch_ms(phase_at[i], now);
                    if (dwell >= stuck_ms[i]) {
                        stuck_ms[i] = dwell;
                        stuck[i] = phase[i];
                    }
                }

                if (worst >= still_at) {
                    say_stall("still going, ", worst);
                    still_at *= 2;
                }
                else if (worst < stall_ms) {
                    say_stall("", std::max(peak_vi, std::max(peak_game, peak_first)));
                    stalled = false;
                }
            }

            if (!stalled) {
                // Nothing moved late, or the ages would say so: the whole process, or this thread, had no CPU.
                if (late >= stall_ms) {
                    if (tokens > 0) {
                        tokens--;
                        fprintf(stderr, "[stall] the watchdog itself woke %d ms late\n", late);
                    }
                    else {
                        hidden++;
                    }
                }

                quiet = w;
                quiet_swaps = swaps_seen;
                quiet_save = watch_save_ms.load(std::memory_order_relaxed);
                if (watch_ms(faults_at, now) >= 1000) {
                    faults_at = now;
                    faults_quiet = read_watch_faults();
                }
            }

            const int report_age = watch_ms(report_at, now);
            if (report_age >= 5000) {
                report_at = now;
                uint64_t vi_pass_max_us = 0, texture_highest = 0;
                blitz_rt64_watch_peaks(&vi_pass_max_us, &texture_highest);
                const uint64_t saved = watch_save_ms.load(std::memory_order_relaxed);
                const uint64_t passes = w.viPasses - report_base.viPasses, presents = w.firstPresents - report_base.firstPresents;
                const uint64_t second_passes = w.secondPasses - report_base.secondPasses;
                if (swaps_seen != report_swaps || presents != 0) {
                    uint32_t size[9] = {};
                    blitz_rt64_watch_sizes(size);
                    const double per_pass = passes > 0 ? (double)passes : 1.0, per_present = presents > 0 ? (double)presents : 1.0, per_second = second_passes > 0 ? (double)second_passes : 1.0;
                    fprintf(stderr, "[frames] %.1f s: swaps %llu, first %llu, second %llu; vi pass %.1f ms, max %.1f, acquire %.1f, present %.1f, second pass %.1f ms; evict %llu, uploads %llu, upload wait %llu ms, save %llu ms, compile %llu ms\n",
                            report_age / 1000.0, watch_delta(swaps_seen, report_swaps), (unsigned long long)presents, watch_delta(w.secondPresents, report_base.secondPresents),
                            (w.viPassUs - report_base.viPassUs) / per_pass / 1000.0, vi_pass_max_us / 1000.0, (w.acquireUs - report_base.acquireUs) / per_pass / 1000.0,
                            (w.presentUs - report_base.presentUs) / per_present / 1000.0, (w.secondPassUs - report_base.secondPassUs) / per_second / 1000.0,
                            watch_delta(w.evictions, report_base.evictions), watch_delta(w.uploads, report_base.uploads), watch_delta(w.uploadWaitUs, report_base.uploadWaitUs) / 1000,
                            watch_delta(saved, report_save), watch_delta(w.compileMs, report_base.compileMs));
                    say_present(w, report_base, report_age / 1000.0);
                    fprintf(stderr, "[frames] target %ux%u at %.2f, first %ux%u with %u images, second %ux%u with %u, %s barriers; highest texture drawn %llu, %llu of %llu at 256 or above\n",
                            size[0], size[1], size[2] / 100.0, size[3], size[4], size[5], size[6], size[7], size[8], plume_narrow_barriers_on() != 0 ? "narrow" : "full",
                            (unsigned long long)texture_highest, watch_delta(w.texturesPast256, report_base.texturesPast256), watch_delta(w.textureDraws, report_base.textureDraws));
                }
                report_base = w;
                report_swaps = swaps_seen;
                report_save = saved;
            }
        }
        return nullptr;
    }
#endif

    // Published for the UI: frames in the last second and its worst gap (tenths of a ms).
    std::atomic<int> shown_fps{-1};
    std::atomic<int> shown_worst_tenths{0};

    bool enabled() {
        return std::get<bool>(recompui::config::get_general_config().get_option_value("fps_counter"));
    }

    recompui::ContextId context = recompui::ContextId::null();
    recompui::Label* label = nullptr;
    bool shown = false;
    int drawn_fps = -2, drawn_worst = -2;

    void build() {
        using namespace recompui;
        context = create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        Element* root = context.create_element<Element>(context.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(0);
        root->set_left(0);
        root->set_right(0);
        root->set_bottom(0);
        root->set_pointer_events(PointerEvents::None);
        Element* box = context.create_element<Element>(root);
        box->set_position(Position::Absolute);
        box->set_top(12.0f);
        box->set_right(16.0f);
        box->set_padding_left(10.0f);
        box->set_padding_right(10.0f);
        box->set_padding_top(4.0f);
        box->set_padding_bottom(4.0f);
        box->set_border_radius(6.0f);
        box->set_background_color(Color{0, 0, 0, 150});
        box->set_pointer_events(PointerEvents::None);
        label = context.create_element<Label>(box, "", theme::Typography::LabelSM);
        label->set_color(Color{255, 255, 255, 230});
        context.close();
    }
}

namespace blitz {
    float fps_last_second() { return latest_fps.load(); }  // BLITZ DS holds to it (ds_view.cpp); a 5-second average
    // The game's frames (framebuffer swaps) this launch: savestate.cpp's resume guard waits for them after a resume.
    uint64_t game_frames() { return swaps_total.load(std::memory_order_relaxed); }
    // The UI thread's ticks (update_gfx, about one a millisecond), for the PS5's stall watch (platform_ps5.cpp).
    static std::atomic<uint64_t> ui_ticks_total{0};
    void ui_tick() { ui_ticks_total.fetch_add(1, std::memory_order_relaxed); }
    uint64_t ui_ticks() { return ui_ticks_total.load(std::memory_order_relaxed); }
    // Which step of the tick the UI thread is in (main.cpp, savestate.cpp's prompt): the PS5's stall line names it, so a hang
    // there says where (2026-11-15: a start with quit.state hung on the UI thread, and nothing said in what).
    static std::atomic<const char*> ui_step_now{"start"};
    void ui_step(const char* step) { ui_step_now.store(step, std::memory_order_relaxed); }
    const char* ui_step_name() { return ui_step_now.load(std::memory_order_relaxed); }
#ifdef __ANDROID__
    // Frames that really went to the first screen, this launch. fps_last_second counts the game's swaps, which keep coming with
    // nothing shown when a surface can't be presented to.
    uint64_t first_presents() {
        BlitzRt64Watch w{};
        blitz_rt64_watch_read(&w);
        return w.firstPresents;
    }
#endif

    // Every VI (main.cpp's vi_callback), on the VI thread.
    void fps_vi_tick() {
        // The VI thread's own origin, set just before this callback: the renderer's copy (get_vi_regs) is written on its
        // thread a moment later, and read here it lagged a VI now and then, which counted a steady 30 fps as frames held
        // 3 VIs and 1 VI in pairs.
        uint32_t origin = ultramodern::renderer::get_vi_origin_now();
        Clock::time_point now = Clock::now();
#ifdef __ANDROID__
        static const bool watching = getenv("BLITZ_LEAN") != nullptr;
        if (watching) {
            watch_vi_ticks.fetch_add(1, std::memory_order_relaxed);
            static bool watch_started = false;
            if (!watch_started && ultramodern::is_game_started()) {
                watch_started = true;
                if (!start_detached(watchdog)) fprintf(stderr, "[stall] no watchdog: its thread wouldn't start\n");
            }
        }
#endif
        if (window_start == Clock::time_point{}) window_start = log_start = now;
        ticks_since_swap++;
        if (origin != last_origin) {
            last_origin = origin;
            if (last_frame != Clock::time_point{}) log_ticks[std::clamp(ticks_since_swap, 1, 4) - 1]++;
            ticks_since_swap = 0;
#ifdef __ANDROID__
            watch_swaps.fetch_add(1, std::memory_order_relaxed);
#endif
            if (last_frame != Clock::time_point{}) {
                double gap = std::chrono::duration<double, std::milli>(now - last_frame).count();
                worst_ms = std::max(worst_ms, gap);
                log_worst_ms = std::max(log_worst_ms, gap);
                if (gap > 58.3) log_hitches++;       // 4+ vblanks (3.5 x 16.67 ms)
                else if (gap > 41.7) log_held3++;  // 3 vblanks
            }
            last_frame = now;
            frames++;
            log_frames++;
            swaps_total.fetch_add(1, std::memory_order_relaxed);
        }
        // The freeze rescue (savestate.cpp): how long the game's frames have been away, every VI (the VI thread runs on when
        // the game's thread is stuck: the team's iPhone, 2026-11-16).
        blitz::rescue_watch(ultramodern::is_game_started() && last_frame != Clock::time_point{} ? std::chrono::duration<double>(now - last_frame).count() : 0.0);
        if (now - window_start >= std::chrono::seconds(1)) {
            shown_fps = frames;
            shown_worst_tenths = (int)(worst_ms * 10.0);
            frames = 0;
            worst_ms = 0;
            window_start = now;
        }
        if (now - log_start >= std::chrono::seconds(5)) {
            double secs = std::chrono::duration<double>(now - log_start).count();
            // A freeze where the VI still ticks is the game (or its renderer) stuck, not the clock: said once, so a
            // phone log can tell the two apart (the team's iPhone, 2026-10-29: the lines just stopped in the menus).
            static const bool potato = getenv("BLITZ_LEAN") != nullptr;
            if (potato && ultramodern::is_game_started()) {
                uint64_t queued, compiled, compile_ms, uber, fast, gpu_wait_ms, slots, frame_gpu_ms, frame_wait_ms, ram_passes, ram_skips;
                blitz_rt64_stats(&queued, &compiled, &compile_ms, &uber, &fast, &gpu_wait_ms, &slots, &frame_gpu_ms, &frame_wait_ms, &ram_passes, &ram_skips);
                // Compiled pipelines for the next launch, once the shaders have settled.
                static uint64_t saved_compiled = 0, last_compiled = 0;
                if (compiled != saved_compiled && compiled == last_compiled) {
#ifdef __ANDROID__
                    if (!watch_saving.exchange(true)) {  // one at a time; a save still going waits for the next window
                        saved_compiled = compiled;
                        if (!start_detached(save_pipeline_cache_thread)) watch_saving = false;
                    }
#else
                    saved_compiled = compiled;
#endif
                }
                last_compiled = compiled;
                fprintf(stderr, "[render] %.0f s: RAM pass on %llu frames, off %llu, wait %llu ms, frame pass GPU %llu ms and wait %llu ms, %llu heavy and %llu fast draws, shaders %llu of %llu compiled, %llu ms compiling, last slot handed out %llu\n", secs,
                        (unsigned long long)ram_passes, (unsigned long long)ram_skips, (unsigned long long)gpu_wait_ms, (unsigned long long)frame_gpu_ms, (unsigned long long)frame_wait_ms, (unsigned long long)uber, (unsigned long long)fast, (unsigned long long)compiled,
                        (unsigned long long)queued, (unsigned long long)compile_ms, (unsigned long long)slots);
            }
            static bool stalled = false;
            if (ultramodern::is_game_started() && log_frames > 0) {
                latest_fps = (float)(log_frames / secs);
                fprintf(stderr, "[fps] %.1f fps, worst gap %.0f ms, %d frames held 3 vblanks, %d hitches; VIs per frame 1/2/3/4+: %d/%d/%d/%d\n",
                        log_frames / secs, log_worst_ms, log_held3, log_hitches, log_ticks[0], log_ticks[1], log_ticks[2], log_ticks[3]);
                stalled = false;
            } else if (ultramodern::is_game_started() && last_frame != Clock::time_point{} && !stalled) {
                fprintf(stderr, "[fps] no new frame for %.0f s; the VI still ticks\n",
                        std::chrono::duration<double>(now - last_frame).count());
                stalled = true;
            }
#ifndef __ANDROID__
            // No watchdog here, so no [frames]: while a second window draws (BLITZ DS on the Mac), [present] comes with [fps].
            {
                static BlitzRt64Watch present_base{};
                static bool present_based = false;
                BlitzRt64Watch w{};
                blitz_rt64_watch_read(&w);
                if (present_based && ultramodern::is_game_started() && w.secondPresents != present_base.secondPresents) say_present(w, present_base, secs);
                present_base = w;
                present_based = true;
            }
#endif
            // BLITZ_WAITS=1: where the renderer's threads waited in the window (ms over the 5 s), for chasing pacing.
            static const bool waits = getenv("BLITZ_WAITS") != nullptr;
            if (waits) {
                static BlitzRt64Watch base{};
                BlitzRt64Watch w{};
                blitz_rt64_watch_read(&w);
                fprintf(stderr, "[waits] texture uploads %llu (waited %llu ms), shader compiles %llu ms, gfx queue full %llu ms, workload waiting on present %llu ms, present hold %llu ms, acquire %llu ms, present %llu ms; presents %llu picked %llu skipped\n",
                        (unsigned long long)(w.uploads - base.uploads), (unsigned long long)((w.uploadWaitUs - base.uploadWaitUs) / 1000), (unsigned long long)(w.compileMs - base.compileMs),
                        (unsigned long long)((w.workloadFullUs - base.workloadFullUs) / 1000), (unsigned long long)((w.waitPresentUs - base.waitPresentUs) / 1000),
                        (unsigned long long)((w.presentHoldUs - base.presentHoldUs) / 1000), (unsigned long long)((w.acquireUs - base.acquireUs) / 1000),
                        (unsigned long long)((w.presentUs - base.presentUs) / 1000), (unsigned long long)(w.presentPicks - base.presentPicks),
                        (unsigned long long)(w.presentSkips - base.presentSkips));
                base = w;
            }
            log_frames = log_held3 = log_hitches = 0;
            for (int& n : log_ticks) n = 0;
            log_worst_ms = 0;
            log_start = now;
        }
    }

    // Every UI tick (main.cpp's update_gfx): show or hide, and the latest second.
    void fps_update() {
        bool want = ultramodern::is_game_started() && enabled() && !replay_is_open();  // the game's stopped under a replay
        if (!want) {
            if (shown) {
                recompui::hide_context(context);
                shown = false;
            }
            return;
        }
        if (context == recompui::ContextId::null()) build();
        int fps = shown_fps.load(), worst = shown_worst_tenths.load();
        if (fps != drawn_fps || worst / 10 != drawn_worst) {
            char text[64];
            if (fps < 0) snprintf(text, sizeof(text), "-- FPS");
            else snprintf(text, sizeof(text), "%d FPS  \xC2\xB7  %d MS", fps, worst / 10);
            context.open();
            label->set_text(text);
            context.close();
            drawn_fps = fps;
            drawn_worst = worst / 10;
        }
        if (!shown) {
            recompui::show_context(context, "");
            shown = true;
        }
    }
}
