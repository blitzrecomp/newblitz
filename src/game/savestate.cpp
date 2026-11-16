// Save states, slots, Save & Quit and Redo Play (experimental; build/analysis/savestates/PLAN.md).
// Built for real with BLITZ_SAVESTATES (`make app` / `make app2026`; SAVESTATES=0 builds without),
// where the game code comes from tools/savestate/resumable.py and every function that can reach
// process_sleep or osRecvMesg keeps a shadow stack of the call sites it is inside.
//
// Snapshots are taken and restored by the process dispatcher at its loop head (hook at 0x80273D7C),
// and only when the game is quiescent:
//   - every game process is asleep in process_sleep (an unbroken chain of call sites from its entry
//     function down to the stop), or has never run;
//   - every other game thread (scheduler, audio, controller, worker, idle) is in osRecvMesg (or, for
//     idle, outside game code) and no process is waiting to run;
//   - no RSP task is in flight and no RSP/RDP/PI/SI completion is waiting to be delivered (runtime
//     patch 0005).
// A snapshot is RDRAM plus registers and call-site stacks. A restore (in the same session) writes
// RDRAM back; the fixed threads stay where they are (they must be at the same call sites as in the
// snapshot) and get their registers back; process threads are destroyed and recreated, each walking
// its chain back down to process_sleep as if the dispatcher had just woken it.
//
// BLITZ_SS_VALIDATE=1 logs how often a snapshot would be possible, and why not.
#include <map>
#include "../main/platform_compat.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "SDL2/SDL.h"  // the app going to the background: the Auto slot
#include <cstring>
#include <deque>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "util/file.h"
#include "recompui/recompui.h"

#include "blitz_savestate.h"

// The PS5's breadcrumbs (platform_ps5.cpp): straight to /data/blitzrecomp/boot.log, past the log pipe, so a hang during a
// Save & Quit or a resume still says how far it got.
#ifdef __PROSPERO__
extern "C" void blitz_ps5_crumb(const char* step);
#define ps5_crumb(step) blitz_ps5_crumb(step)
#else
#define ps5_crumb(step) ((void)0)
#endif

namespace blitz {
    void checkpoint_discard_on_quit();  // checkpoint.cpp
    void post_status(const std::string& msg);  // notices.cpp: shown as a toast (toast.cpp)
    void post_status_held(const std::string& msg);  // notices.cpp: a toast that stays up until release_status
    void release_status();
    float fps_last_second();                   // fps_counter.cpp
    uint64_t game_frames();                    // fps_counter.cpp: the game's swaps this launch
    void ui_step(const char* step);            // fps_counter.cpp: where the UI thread is (the PS5's stall line)
    void replay_close();  // replay_screen.cpp

    struct StateModule {
        std::string name;
        std::function<StateBlob()> save;
        std::function<void(const StateBlob&)> load;
        std::function<std::string(const StateBlob&)> check;
    };

    std::vector<StateModule>& state_modules() {
        static std::vector<StateModule> modules;  // filled by static initializers
        return modules;
    }

    void savestate_register(const std::string& name, std::function<StateBlob()> save, std::function<void(const StateBlob&)> load,
                            std::function<std::string(const StateBlob&)> check) {
        state_modules().push_back({name, std::move(save), std::move(load), std::move(check)});
    }

    std::string active_pack_id();  // content_pack*.cpp: "" for none
    int fs_sync_ram_toc(uint8_t* rdram);  // art_dropin.cpp
}

namespace {
    // Each roster pack keeps its own Controller Pak and season files, so a state saved under one pack must not
    // be loaded under another (its season would then be saved into the wrong pack's files).
    [[maybe_unused]] const bool pack_registered = (blitz::savestate_register(
        "content_pack",
        [] {
            std::string id = blitz::active_pack_id();
            return blitz::StateBlob(id.begin(), id.end());
        },
        [](const blitz::StateBlob&) {},
        [](const blitz::StateBlob& b) -> std::string {
            std::string saved(b.begin(), b.end()), now = blitz::active_pack_id();
            if (saved == now) return "";
            return "This state was saved with " + (saved.empty() ? std::string("no roster pack") : "the roster pack \"" + saved + "\"") +
                   ". Switch back to it to load the state.";
        }), true);
}

#ifdef BLITZ_SAVESTATES
#include "ss_runtime.h"
#include "ss_sites.h"
extern "C" ss_stack* ss_current_stack(void);  // ss_runtime.c
extern "C" void blitz_overlay_current(int* segment, int* stadium);  // overlays.cpp
extern "C" void blitz_overlay_load(int segment, int stadium);
extern "C" uint32_t blitz_play_select_draws(void);    // gfx_ext.cpp: play select's header drawn so far
extern "C" int blitz_play_select_on_screen(void);     //   ... and in the last few frames
namespace blitz {
    bool redo_play_enabled();  // config.cpp: Settings → General → Redo Play
}

namespace {
    constexpr int32_t process_list = (int32_t)0x802A9B80;  // first process; next at +0x0
    constexpr int process_entry = 0x24, process_arg = 0x10, process_thread = 0x30;
    constexpr uint32_t process_sleep_vram = 0x80273CCC;
    constexpr size_t rdram_size = 8 * 1024 * 1024;

    struct StackImage {
        uint32_t depth = 0, overflow = 0;
        ss_entry e[SS_MAX_DEPTH];

        static StackImage of(const ss_stack* s) {
            StackImage im;
            im.depth = s->depth;
            im.overflow = s->overflow;
            std::memcpy(im.e, s->e, sizeof(ss_entry) * std::min<uint32_t>(s->depth, SS_MAX_DEPTH));
            return im;
        }
        bool same_sites(const StackImage& o) const {
            if (depth != o.depth) return false;
            for (uint32_t i = 0; i < std::min<uint32_t>(depth, SS_MAX_DEPTH); i++) {
                if (e[i].site != o.e[i].site || e[i].target != o.e[i].target) return false;
            }
            return true;
        }
        // Top entry is a call into the runtime (osRecvMesg, or process_sleep's osStopThread).
        bool at_sink() const {
            if (depth == 0 || depth > SS_MAX_DEPTH) return false;
            const ss_entry& top = e[depth - 1];
            return top.site <= SS_SITE_COUNT && ss_site_vrams[top.site][2] == 0 && top.target == 0;
        }
    };

    // Every game thread, registered as it starts (thread_create_callback in main.cpp).
    struct LiveThread {
        recomp_context* ctx;
        ss_stack* stack;
        UltraThreadContext* host;  // the OSThread's context while this thread lives
        uint64_t serial = 0;       // which registration this is (Registration, drop_exited)
    };
    // A process thread that stopped itself: where, and its registers (valid until started again).
    struct Sleeper {
        bool valid = false;
        StackImage stack;
        recomp_context* ctx = nullptr;
    };
    struct ThreadImage {
        int32_t thread = 0;
        recomp_context ctx{};
        uintptr_t ctx_addr = 0;  // where ctx lived, to re-point f_odd
        StackImage stack;
    };
    struct ProcessImage {
        int32_t process = 0;
        bool fresh = false;  // never ran: recreated from its entry function
        ThreadImage thread;
    };
    struct Snapshot {
        bool valid = false;
        std::vector<uint8_t> rdram;
        ThreadImage dispatcher;
        std::vector<ProcessImage> processes;
        std::vector<ThreadImage> fixed;
        std::vector<int32_t> run_queue;
        int overlay_segment = -1, overlay_stadium = -1;
        std::vector<std::pair<std::string, blitz::StateBlob>> modules;  // blitz_savestate.h
        int slot = 0;  // which slot it came from or went to (not stored in the file)
    };

    std::mutex threads_mutex;  // live, sleepers, pending_resume
    std::unordered_map<int32_t, LiveThread> live;
    std::unordered_map<int32_t, Sleeper> sleepers;
    std::unordered_map<int32_t, ThreadImage> pending_resume;  // recreated threads, applied as they start
    uint64_t next_serial = 0;  // threads_mutex

    // Game threads whose host thread has exited (thread, registration serial), kept apart from `live` under their own lock.
    // A host thread's exit can't take threads_mutex: on Windows thread_local destructors run under the loader lock, and the
    // dispatcher holds threads_mutex while restore_snapshot creates threads (osCreateThread waits for the new thread, which
    // needs the loader lock to start). Taking it there froze Back in the menus on Windows (build/winpc_dumps/nb_freeze2.dmp).
    std::mutex exited_mutex;  // held only for the push or the swap, never while waiting on anything
    std::vector<std::pair<int32_t, uint64_t>> exited;

    // The exited threads leave `live` (call with threads_mutex held).
    void drop_exited() {
        std::vector<std::pair<int32_t, uint64_t>> gone;
        {
            std::lock_guard lock{exited_mutex};
            gone.swap(exited);
        }
        for (auto [t, serial] : gone) {
            auto it = live.find(t);
            if (it != live.end() && it->second.serial == serial) live.erase(it);
        }
    }
    Snapshot snap;

    // Redo Play: a snapshot at the top of every play of a real game (the checkpoint hook at the top of
    // the game's per-play loop), the last few kept in memory. Redo goes back to the start of the play
    // just run (or being run); at play select it goes back to the play before, and pressing again
    // keeps stepping back.
    // Plays Redo can go back, each an 8 MB snapshot: 8, or in Potato Mode 2 (16 MB: the play just run, and at play select the
    // one before needs the snapshot before that), and no menu step-back (its snapshot at every confirm swapped an RG DS game
    // out: the team, 2026-11-02).
    bool redo_lean() { static const bool lean = getenv("BLITZ_LEAN") != nullptr; return lean; }  // asked late: the env comes after load
    size_t redo_keep_n() { return redo_lean() ? 2 : 8; }
    bool menu_steps_on() { return !redo_lean() && blitz::redo_play_enabled(); }
    std::deque<Snapshot> redo_ring;  // oldest first
    std::atomic_bool redo_capture{false}, redo_request{false};
    bool play_ended_since = false;      // a play ended after the newest snapshot
    uint32_t select_draws_at_newest = 0;
    int capture_wait = 0, redo_wait = 0;

    // Menu steps (team, 2026-10-23: picking the wrong team or Arcade for Season meant a reset): in the menus before a
    // game, each A press is held back a frame or two while the screen it was pressed on is snapshotted, then let
    // through. Redo there goes back one press at a time, to the mode select if need be. The game's own □ BACK still
    // works where it does; this covers what it doesn't (a confirmed team, the mode).
    constexpr size_t step_keep = 12;
    std::deque<Snapshot> step_ring;  // oldest first
    std::atomic_bool step_capture{false};
    std::atomic_bool in_menus{false};  // the menus (phase 1), where steps are kept
    int step_wait = 0;
    std::atomic_int request{0};            // op (1 save, 2 load) | slot << 4; slot 0 is quit.state
    std::atomic_bool in_game{false};       // a game is being played (phase 2), for the quit prompt
    std::atomic_bool confirm_quit{false};
    std::atomic_bool quit_now{false};      // Save & Quit finished: quit on the UI thread  // Don't Save was chosen: ask once more (savestate_startup_prompt)
    std::atomic_int offense_side{0};       // side with the ball (0 left team, 1 right), for phone controllers
    std::atomic_int port_side[4] = {-1, -1, -1, -1};  // each controller's team (0 left, 1 right, -1 none)
    std::mutex done_mutex;
    std::function<void(bool)> on_done;     // called once the requested save or load finished (or failed)
    int on_done_slot = -1;                 // ... for that slot only (Save & Quit: slot 0), not any other request
    void finish(bool ok, int slot) {
        std::function<void(bool)> f;
        {
            std::lock_guard lock{done_mutex};
            if (slot != on_done_slot) return;
            f = std::move(on_done);
            on_done = nullptr;
            on_done_slot = -1;
        }
        if (f) f(ok);
    }
    int waited = 0;  // dispatcher ticks, one per retrace (about 60 a second, whatever the frame rate)
    std::map<std::string, int> wait_tally;  // what each refused tick was waiting on
    std::atomic<bool> ask_resume_again{false};  // a Resume gave up: offer "Pick up your game?" again

    // A load waits up to about 60 s for a moment it can restore at (BLITZ_SS_LOAD_WAIT ticks): on an RG DS that moment can come
    // only once the attract demo has loaded, past the 10 s a save gets.
    int load_wait_ticks() {
        static const int n = [] { const char* s = getenv("BLITZ_SS_LOAD_WAIT"); const int v = s ? atoi(s) : 0; return v > 0 ? v : 3600; }();
        return n;
    }

    std::string tally_text() {
        std::string t;
        for (const auto& [why, n] : wait_tally) t += (t.empty() ? "" : ", ") + why + " " + std::to_string(n);
        return t;
    }

    enum Refusal { ok, not_stopped, stopped_by_other, not_asleep, broken_chain, overflowed, run_queue,
                   dispatcher_deep, fixed_not_waiting, rsp_busy, messages_pending, season_screen, refusal_count };
    const char* refusal_names[refusal_count] = {"ok", "process not stopped", "process stopped by another thread",
        "process not asleep", "broken chain", "stack overflow", "process waiting to run", "dispatcher inside a call",
        "thread outside osRecvMesg", "RSP task in flight", "completion not delivered", "Season screen"};
    constexpr uint32_t adv_week_driver_vram = 0x802FBBE4;  // ovl_b's week driver; Advanced Season runs it in host code

    uint32_t sleep_site() {
        static const uint32_t site = [] {
            for (uint32_t i = 1; i <= SS_SITE_COUNT; i++) {
                if (ss_site_vrams[i][0] == process_sleep_vram && ss_site_vrams[i][2] == 0) return i;
            }
            return 0u;
        }();
        return site;
    }

    // From the entry function down: each entry runs in the function the one before it called (its
    // recorded target, for an indirect call), and the last is process_sleep stopping the thread.
    Refusal check_sleeper(const StackImage& s, uint32_t entry) {
        if (s.overflow != 0 || s.depth > SS_MAX_DEPTH) return overflowed;
        if (s.depth == 0 || s.e[s.depth - 1].site != sleep_site()) return not_asleep;
        for (uint32_t i = 0; i < s.depth; i++) {
            if (s.e[i].site == 0 || s.e[i].site > SS_SITE_COUNT) return broken_chain;
        }
        if (ss_site_vrams[s.e[0].site][0] != entry) return broken_chain;
        for (uint32_t i = 1; i < s.depth; i++) {
            uint32_t prev = s.e[i - 1].site;
            uint32_t callee = ss_site_vrams[prev][2] != 0 ? ss_site_vrams[prev][2] : s.e[i - 1].target;
            // Inside Advanced Season's host driver (adv_engine.cpp) between the week's games: its message screens
            // (bye week, playoff news, the rating) are host frames no save can come back into. The user's game is
            // fine: play() leaves the vanilla call site's entry for it.
            if (callee != ss_site_vrams[s.e[i].site][0]) return callee == adv_week_driver_vram ? season_screen : broken_chain;
        }
        return ok;
    }

    std::vector<int32_t> processes(uint8_t* rdram) {
        std::vector<int32_t> out;
        for (int32_t p = MEM_W(0, process_list); p != 0 && out.size() < 4096; p = MEM_W(0, p)) out.push_back(p);
        return out;
    }

    std::vector<int32_t> run_queue_now(uint8_t* rdram) {
        std::vector<int32_t> out;
        for (int32_t t = ultramodern::thread_queue_peek(rdram, ultramodern::running_queue); t != 0 && out.size() < 64;
             t = TO_PTR(OSThread, t)->next) {
            out.push_back(t);
        }
        return out;
    }

    // Live threads that aren't processes or the dispatcher (call with threads_mutex held).
    std::vector<int32_t> fixed_threads(uint8_t* rdram, const std::vector<int32_t>& procs, int32_t self) {
        drop_exited();
        std::vector<int32_t> out;
        for (auto& [t, info] : live) {
            if (t == self || TO_PTR(OSThread, t)->context != info.host) continue;  // ended (or not yet registered)
            bool process = false;
            for (int32_t p : procs) process = process || p + process_thread == t;
            if (!process) out.push_back(t);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // Whether this moment can be snapshotted or restored into (call with threads_mutex held).
    Refusal quiescent(uint8_t* rdram, int32_t self, uint64_t* fresh = nullptr) {
        if (ss_current_stack()->depth != 0) return dispatcher_deep;
        if (ultramodern::rsp_tasks_in_flight_count() != 0) return rsp_busy;
        if (ultramodern::pending_must_deliver_messages() != 0) return messages_pending;
        auto procs = processes(rdram);
        for (int32_t t : run_queue_now(rdram)) {
            for (int32_t p : procs) if (p + process_thread == t) return run_queue;
        }
        for (int32_t p : procs) {
            int32_t t = p + process_thread;
            if (TO_PTR(OSThread, t)->state != OSThreadState::STOPPED) return not_stopped;
            auto it = sleepers.find(t);
            if (it == sleepers.end()) {  // never ran
                if (fresh) (*fresh)++;
                continue;
            }
            if (!it->second.valid) return stopped_by_other;
            Refusal r = check_sleeper(it->second.stack, (uint32_t)MEM_W(process_entry, p));
            if (r != ok) return r;
        }
        for (int32_t t : fixed_threads(rdram, procs, self)) {
            StackImage s = StackImage::of(live[t].stack);
            if (s.depth != 0 && !s.at_sink()) return fixed_not_waiting;
        }
        return ok;
    }

    ThreadImage image_thread(int32_t t, const recomp_context* ctx, const StackImage& stack) {
        ThreadImage im;
        im.thread = t;
        im.ctx = *ctx;
        im.ctx_addr = (uintptr_t)ctx;
        im.stack = stack;
        return im;
    }

    void put_context(recomp_context* dst, const ThreadImage& im) {
        *dst = im.ctx;
        dst->f_odd = (uint32_t*)((uint8_t*)dst + ((uintptr_t)im.ctx.f_odd - im.ctx_addr));
    }

    Snapshot make_snapshot(uint8_t* rdram, recomp_context* ctx, int32_t self) {
        Snapshot s;
        s.rdram.assign(rdram, rdram + rdram_size);
        s.dispatcher = image_thread(self, ctx, StackImage{});
        auto procs = processes(rdram);
        for (int32_t p : procs) {
            ProcessImage pi;
            pi.process = p;
            int32_t t = p + process_thread;
            auto it = sleepers.find(t);
            if (it == sleepers.end()) {
                pi.fresh = true;
                pi.thread.thread = t;
            }
            else {
                pi.thread = image_thread(t, it->second.ctx, it->second.stack);
            }
            s.processes.push_back(pi);
        }
        for (int32_t t : fixed_threads(rdram, procs, self)) {
            s.fixed.push_back(image_thread(t, live[t].ctx, StackImage::of(live[t].stack)));
        }
        s.run_queue = run_queue_now(rdram);
        blitz_overlay_current(&s.overlay_segment, &s.overlay_stadium);
        for (const auto& m : blitz::state_modules()) s.modules.emplace_back(m.name, m.save());
        // No host thread pointers in the image: a process thread destroyed by the restore must never
        // read its own context back from it (it would take that as not destroyed and run on).
        auto clear_context = [&](int32_t t) {
            size_t at = (size_t)((uint8_t*)&TO_PTR(OSThread, t)->context - rdram);
            if (at + sizeof(void*) <= s.rdram.size()) std::memset(&s.rdram[at], 0, sizeof(void*));
        };
        clear_context(self);
        for (const ProcessImage& pi : s.processes) clear_context(pi.thread.thread);
        for (const ThreadImage& f : s.fixed) clear_context(f.thread);
        s.valid = true;
        return s;
    }

    void take_snapshot(uint8_t* rdram, recomp_context* ctx, int32_t self) {
        snap = make_snapshot(rdram, ctx, self);
        fprintf(stderr, "[savestate] saved: %zu processes, %zu other threads\n", snap.processes.size(), snap.fixed.size());
    }

    // The fixed threads must be the same ones, at the same call sites, as in the snapshot.
    bool fixed_match(uint8_t* rdram, int32_t self, const Snapshot& sn, std::string& why) {
        auto now = fixed_threads(rdram, processes(rdram), self);
        if (now.size() != sn.fixed.size()) {
            why = "different threads";
            return false;
        }
        for (size_t i = 0; i < now.size(); i++) {
            if (now[i] != sn.fixed[i].thread) {
                why = "different threads";
                return false;
            }
            if (!StackImage::of(live[now[i]].stack).same_sites(sn.fixed[i].stack)) {
                char buf[64];
                snprintf(buf, sizeof buf, "thread %d elsewhere", TO_PTR(OSThread, now[i])->id);
                why = buf;
                return false;
            }
        }
        return true;
    }

    void restore_snapshot(uint8_t* rdram, recomp_context* ctx, int32_t self, const Snapshot& sn) {
        // Every process thread is destroyed below (bypassing blitz_osDestroyThread), so no earlier restore's
        // waiting image may survive: it would take over whatever process the game creates next in that slot.
        pending_resume.clear();
        // The current processes' threads end (each wakes, sees it was destroyed and unwinds).
        for (int32_t p : processes(rdram)) {
            int32_t t = p + process_thread;
            sleepers.erase(t);
            live.erase(t);
            if (TO_PTR(OSThread, t)->context != nullptr) osDestroyThread(rdram, t);
        }
        while (!ultramodern::thread_queue_empty(rdram, ultramodern::running_queue)) {
            ultramodern::thread_queue_pop(rdram, ultramodern::running_queue);
        }
        ultramodern::discard_external_messages();

        // RDRAM back, keeping the live threads' host contexts.
        std::vector<std::pair<int32_t, UltraThreadContext*>> keep;
        keep.emplace_back(self, TO_PTR(OSThread, self)->context);
        for (const ThreadImage& f : sn.fixed) keep.emplace_back(f.thread, TO_PTR(OSThread, f.thread)->context);
        std::memcpy(rdram, sn.rdram.data(), rdram_size);
        for (auto& [t, c] : keep) TO_PTR(OSThread, t)->context = c;

        for (auto it = sn.run_queue.rbegin(); it != sn.run_queue.rend(); ++it) {
            ultramodern::thread_queue_insert(rdram, ultramodern::running_queue, *it);
        }
        for (const ThreadImage& f : sn.fixed) put_context(live[f.thread].ctx, f);

        int seg = -1, stadium = -1;
        blitz_overlay_current(&seg, &stadium);
        if (seg != sn.overlay_segment || stadium != sn.overlay_stadium) {
            blitz_overlay_load(sn.overlay_segment, sn.overlay_stadium);
        }

        // Each snapshot process gets a new thread; the asleep ones resume in process_sleep.
        for (const ProcessImage& pi : sn.processes) {
            int32_t t = pi.thread.thread;
            OSThread* th = TO_PTR(OSThread, t);
            OSId id = th->id;
            OSPri pri = th->priority;
            int32_t sp = th->sp + 0x10;  // osCreateThread takes 0x10 off again
            if (!pi.fresh) {
                pending_resume[t] = pi.thread;
                Sleeper& s = sleepers[t];
                s.valid = true;
                s.stack = pi.thread.stack;
                s.ctx = &pending_resume[t].ctx;
            }
            osCreateThread(rdram, t, id, (uint32_t)MEM_W(process_entry, pi.process), MEM_W(process_arg, pi.process), sp, pri);
        }
        // Re-point the pending images' saved ctx address to where they are kept now, so a snapshot
        // taken before they run again copies the right f_odd.
        for (auto& [t, im] : pending_resume) {
            im.ctx.f_odd = (uint32_t*)((uint8_t*)&im.ctx + ((uintptr_t)im.ctx.f_odd - im.ctx_addr));
            im.ctx_addr = (uintptr_t)&im.ctx;
        }

        for (const auto& m : blitz::state_modules()) {
            for (const auto& [name, blob] : sn.modules) {
                if (name == m.name) m.load(blob);
            }
        }
        blitz::fs_sync_ram_toc(rdram);  // art_dropin.cpp: the file table's positions are this run's ROM image's
        put_context(ctx, sn.dispatcher);  // f_odd points into ctx: re-pointed (another session's address)
        fprintf(stderr, "[savestate] restored: %zu processes\n", sn.processes.size());
    }

    // Files (state_file below), readable only by the same build (site ids).
    constexpr char file_magic[8] = {'B', 'L', 'Z', 'S', 'T', 'A', 'T', 'E'};
    constexpr uint32_t file_version = 3;  // 2: the dispatcher's context address (f_odd); 3: module state

    uint64_t build_id() {
        static const uint64_t id = [] {
            uint64_t h = 1469598103934665603ull;  // FNV-1a over the site table (and the flavour)
            auto mix = [&](uint32_t v) {
                for (int i = 0; i < 4; i++) {
                    h ^= (v >> (8 * i)) & 0xFF;
                    h *= 1099511628211ull;
                }
            };
            mix(SS_SITE_COUNT);
            for (uint32_t i = 1; i <= SS_SITE_COUNT; i++) {
                for (int k = 0; k < 3; k++) mix(ss_site_vrams[i][k]);
            }
#if BLITZ_TEAMPLUS
            mix(2026);
#elif BLITZ_MODERN
            mix(2027);
#endif
            return h;
        }();
        return id;
    }

    // Slots 1-3: <app folder>/savestates/slot<N>.state (the single quick.state from before slots
    // becomes slot 1). Slot 4 is Auto: written when the app goes to the background mid-game (a clamshell closed, an app
    // left on a phone: the team, 2026-11-02), and stepped to, loaded or saved like the others.
    constexpr int slot_count = 4, auto_slot = 4;
    std::string slot_name(int slot) { return slot == auto_slot ? std::string("Auto slot") : "Slot " + std::to_string(slot); }
    std::atomic_int current_slot{1};

    // Slot 0 is the game saved on quitting (quit.state, with a one-line quit.txt for the startup prompt).
    std::filesystem::path state_file(int slot) {
        std::filesystem::path dir = recompui::file::get_app_folder_path() / "savestates";
        std::filesystem::path path = dir / (slot == 0 ? std::string("quit.state") : "slot" + std::to_string(slot) + ".state");
        std::error_code ec;
        if (slot == 1 && !std::filesystem::exists(path, ec) && std::filesystem::exists(dir / "quick.state", ec)) {
            std::filesystem::rename(dir / "quick.state", path, ec);
        }
        return path;
    }

    // ---- The resume guard: a saved game that hangs or crashes the app is offered once, not at every start ----
    // (the team's PS5, 2026-11-15: after a Save & Quit that froze, every start hung right after the game came up, with
    // quit.state there, until it was moved aside by hand over FTP.) savestates/quit.resuming is written before the
    // "Pick up your game?" prompt opens and again before a Resume, and removed once this start has shown the game's
    // frames afterwards with the UI thread still answering (resume_guard_tick). Found at a start, the last start never
    // got that far: quit.state goes aside as quit.state.failed (with quit.txt.failed) and isn't offered again.
    // A resumed quit.state waits as quit.state.resuming until the frames come, so a resume that hangs leaves it for a look.
    enum class Guard { off, offer, resume, restored };
    std::atomic<Guard> guard{Guard::off};       // UI thread; the dispatcher only sets restored/failed below
    std::atomic<bool> guard_restored{false};     // the dispatcher restored quit.state (slot 0)
    std::atomic<bool> guard_failed{false};       // ... or gave up on it: nothing was restored
    // Set where the stage is armed (the prompt's button may run on the UI's draw), read on the UI thread.
    std::atomic<uint64_t> guard_frames{0};       // game_frames() when the current stage began
    std::atomic<uint64_t> guard_ms{0};           // ... and the time
    std::atomic<bool> guard_said_late{false};

    uint64_t guard_now_ms() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::filesystem::path guard_marker() { return state_file(0).parent_path() / "quit.resuming"; }
    std::filesystem::path with_suffix(std::filesystem::path p, const char* suffix) { return p += suffix; }

    void guard_arm(Guard stage) {
        std::error_code ec;
        std::filesystem::create_directories(guard_marker().parent_path(), ec);
        {
            std::ofstream f(guard_marker(), std::ios::trunc);
            f << (stage == Guard::offer ? "offer" : "resume") << " " << (long long)std::time(nullptr) << "\n";
        }
        blitz::sync_file(guard_marker());  // on the disk before anything that might hang
        guard_frames = blitz::game_frames();
        guard_ms = guard_now_ms();
        guard_said_late = false;
        guard_restored = false;
        guard_failed = false;
        guard = stage;
    }

    void guard_disarm(const char* why) {
        std::error_code ec;
        std::filesystem::remove(guard_marker(), ec);
        if (guard.exchange(Guard::off) != Guard::off) fprintf(stderr, "[savestate] resume guard off: %s\n", why);
    }

    // Once, at the first UI tick of a start: the last start's guard still there means it hung or crashed while the saved
    // game was being offered or resumed. "" or the notice for the player.
    std::string guard_startup() {
        std::error_code ec;
        const std::filesystem::path resuming = with_suffix(state_file(0), ".resuming");
        if (!std::filesystem::exists(guard_marker(), ec)) {
            std::filesystem::remove(resuming, ec);  // a resume that went fine, whose removal didn't get done
            return "";
        }
        std::string stage;
        std::getline(std::ifstream(guard_marker()), stage);
        const std::filesystem::path txt = std::filesystem::path(state_file(0)).replace_extension(".txt");
        const std::filesystem::path failed = with_suffix(state_file(0), ".failed");
        bool moved = false;
        for (const std::filesystem::path& from : {resuming, state_file(0)}) {
            if (moved || !std::filesystem::exists(from, ec)) continue;
            std::filesystem::remove(failed, ec);
            std::filesystem::rename(from, failed, ec);
            if (ec) std::filesystem::remove(from, ec);  // can't be kept aside: it mustn't be offered again either
            moved = true;
        }
        if (std::filesystem::exists(txt, ec)) {
            std::filesystem::path txt_failed = with_suffix(txt, ".failed");
            std::filesystem::remove(txt_failed, ec);
            std::filesystem::rename(txt, txt_failed, ec);
            if (ec) std::filesystem::remove(txt, ec);
        }
        std::filesystem::remove(resuming, ec);
        std::filesystem::remove(guard_marker(), ec);
        fprintf(stderr, "[savestate] the last start stopped while its saved game was being %s (%s): %s\n",
                stage.rfind("resume", 0) == 0 ? "resumed" : "offered", stage.c_str(),
                moved ? "quit.state set aside as quit.state.failed, not offered again" : "no quit.state left");
        return moved ? "Your saved game couldn't be picked up last time, so it was set aside" : "";
    }

    // UI thread, every tick: the guard comes off once the game's frames show after the stage began (the UI thread is
    // answering, as it runs this). A Resume that failed or gave up restored nothing: off too.
    void guard_tick() {
        Guard g = guard.load();
        if (g == Guard::off) return;
        const uint64_t now = guard_now_ms();
        if (g == Guard::resume && guard_failed.exchange(false)) {
            guard_disarm("the resume didn't happen");
            return;
        }
        if (g == Guard::resume && guard_restored.exchange(false)) {
            guard = g = Guard::restored;
            guard_frames = blitz::game_frames();
            guard_ms = now;
            guard_said_late = false;
        }
        const uint64_t frames = blitz::game_frames() - guard_frames;
        if (g == Guard::offer && now - guard_ms >= 2000 && (frames >= 10 || now - guard_ms >= 15000)) {
            guard_disarm("the game runs with the prompt up");
        }
        else if (g == Guard::restored && now - guard_ms >= 1000 && frames >= 30) {
            std::error_code ec;
            std::filesystem::remove(with_suffix(state_file(0), ".resuming"), ec);  // used up
            guard_disarm("the resumed game runs");
        }
        else if ((g == Guard::restored || g == Guard::resume) && !guard_said_late && now - guard_ms >= 30000) {
            guard_said_late = true;  // the guard stays: the next start sets quit.state aside
            fprintf(stderr, "[savestate] resume guard: %s and no frames for 30 s (%llu since)\n",
                    g == Guard::restored ? "restored" : "not restored yet", (unsigned long long)frames);
        }
    }

    template <typename T> void put(std::ofstream& f, const T& v) { f.write((const char*)&v, sizeof v); }
    template <typename T> void put_vec(std::ofstream& f, const std::vector<T>& v) {
        put(f, (uint32_t)v.size());
        f.write((const char*)v.data(), sizeof(T) * v.size());
    }
    template <typename T> bool get(std::ifstream& f, T& v) { return (bool)f.read((char*)&v, sizeof v); }
    template <typename T> bool get_vec(std::ifstream& f, std::vector<T>& v, uint32_t max) {
        uint32_t n = 0;
        if (!get(f, n) || n > max) return false;
        v.resize(n);
        return (bool)f.read((char*)v.data(), sizeof(T) * n);
    }

    bool write_file(const Snapshot& s, int slot) {
        // One writer at a time: two quick saves to a slot must not interleave in its temp file.
        static std::mutex write_mutex;
        std::lock_guard write_lock{write_mutex};
        std::error_code ec;
        std::filesystem::path path = state_file(slot);
        std::filesystem::create_directories(path.parent_path(), ec);
        std::filesystem::path tmp = path;
        tmp += ".tmp";
        auto failed = [&] {
            std::filesystem::remove(tmp, ec);  // e.g. disk full: don't leave 8 MB behind
            return false;
        };
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return failed();
            f.write(file_magic, sizeof file_magic);
            put(f, file_version);
            put(f, build_id());
            put_vec(f, s.rdram);
            put(f, s.dispatcher);
            put_vec(f, s.processes);
            put_vec(f, s.fixed);
            put_vec(f, s.run_queue);
            put(f, s.overlay_segment);
            put(f, s.overlay_stadium);
            put(f, (uint32_t)s.modules.size());
            for (const auto& [name, blob] : s.modules) {
                put_vec(f, std::vector<char>(name.begin(), name.end()));
                put_vec(f, blob);
            }
            f.flush();
            if (!f) return failed();
        }
        blitz::sync_file(tmp);  // on disk before it replaces the old state
        blitz::replace_file(tmp, path, ec);
        if (ec) return failed();
        return true;
    }

    // A loaded file's call chains index the site table and its threads go into game memory: anything out of
    // range means the file is damaged.
    bool image_sane(const ThreadImage& t) {
        if (t.stack.depth > SS_MAX_DEPTH) return false;
        for (uint32_t i = 0; i < t.stack.depth; i++) {
            if (t.stack.e[i].site > SS_SITE_COUNT) return false;
        }
        return t.thread == 0 || ((uint32_t)t.thread >= 0x80000000u && (uint32_t)t.thread < 0x80000000u + rdram_size);
    }

    // The file's header says this build can load it (magic, version and build id).
    bool file_compatible(int slot) {
        std::ifstream f(state_file(slot), std::ios::binary);
        char magic[8];
        uint32_t version = 0;
        uint64_t id = 0;
        return f.read(magic, sizeof magic) && std::memcmp(magic, file_magic, sizeof magic) == 0 && get(f, version) &&
               version == file_version && get(f, id) && id == build_id();
    }

    // "" when read; otherwise why not.
    std::string read_file(Snapshot& s, int slot) {
        std::ifstream f(state_file(slot), std::ios::binary);
        if (!f) return slot == 0 ? std::string("The saved game is gone") : slot_name(slot) + " is empty";
        char magic[8];
        uint32_t version = 0;
        uint64_t id = 0;
        if (!f.read(magic, sizeof magic) || std::memcmp(magic, file_magic, sizeof magic) != 0 || !get(f, version) || version != file_version) {
            return "Save state file not readable";
        }
        if (!get(f, id) || id != build_id()) return "Save state is from another version of the app";
        Snapshot in;
        bool ok = get_vec(f, in.rdram, rdram_size) && in.rdram.size() == rdram_size && get(f, in.dispatcher) &&
                  get_vec(f, in.processes, 4096) && get_vec(f, in.fixed, 64) && get_vec(f, in.run_queue, 64) &&
                  get(f, in.overlay_segment) && get(f, in.overlay_stadium);
        uint32_t module_count = 0;
        ok = ok && get(f, module_count) && module_count <= 64;
        for (uint32_t i = 0; ok && i < module_count; i++) {
            std::vector<char> name;
            blitz::StateBlob blob;
            ok = get_vec(f, name, 256) && get_vec(f, blob, 1 << 20);
            if (ok) in.modules.emplace_back(std::string(name.begin(), name.end()), std::move(blob));
        }
        ok = ok && image_sane(in.dispatcher);
        for (const ProcessImage& pi : in.processes) {
            ok = ok && image_sane(pi.thread) && (uint32_t)pi.process >= 0x80000000u && (uint32_t)pi.process < 0x80000000u + rdram_size;
        }
        for (const ThreadImage& t : in.fixed) ok = ok && image_sane(t);
        for (int32_t t : in.run_queue) ok = ok && (uint32_t)t >= 0x80000000u && (uint32_t)t < 0x80000000u + rdram_size;
        if (!ok) return "Save state file not readable";
        in.valid = true;
        s = std::move(in);
        return "";
    }

    int32_t word_in(const Snapshot& sn, uint32_t addr) {
        int32_t v = 0;
        size_t at = addr - 0x80000000u;
        if (at + 4 <= sn.rdram.size()) std::memcpy(&v, &sn.rdram[at], 4);
        return v;
    }

    // "2nd & 7 at the 35, Q2 1:23" from the game state block (play_log.cpp has the field list).
    std::string describe_play(const Snapshot& sn) {
        static const char* quarters[] = {"Q1", "Q2", "Q3", "Q4", "OT1", "OT2", "OT3"};
        static const char* downs[] = {"", "1st", "2nd", "3rd", "4th"};
        int q = word_in(sn, 0x802EA424), c = word_in(sn, 0x802EA428), d = word_in(sn, 0x802EA498);
        int los = word_in(sn, 0x802EA48C), mk = word_in(sn, 0x802EA49C), kind = word_in(sn, 0x802EA508);
        char clock[16];
        snprintf(clock, sizeof clock, "%d:%02d", (c >> 16) & 0xFF, (c >> 8) & 0xFF);
        std::string where = kind == 1 ? "kickoff" : kind == 4 ? "extra point" : "";
        if (where.empty() && d >= 1 && d <= 4) {
            std::string togo = mk >= 100 ? "goal" : std::to_string(std::max(1, mk - los));
            int yard = los <= 50 ? los : 100 - los;
            where = std::string(downs[d]) + " & " + togo + " at the " + std::to_string(yard);
        }
        std::string when = (q >= 0 && q < 7 ? std::string(quarters[q]) : "") + " " + clock;
        return where.empty() ? when : where + ", " + when;
    }

    // "ARI 7 - 0 CHI, 2nd & 7 at the 35, Q2 1:23"
    std::string describe_game(const Snapshot& sn) {
#if BLITZ_TEAMPLUS
        constexpr uint32_t abbr[2] = {0x802E9448, 0x802E94AC};
#else
        constexpr uint32_t abbr[2] = {0x802E9458, 0x802E94BC};
#endif
        std::string team[2];
        int score[2] = {0, 0};
        for (int side = 0; side < 2; side++) {
            for (int i = 0; i < 4; i++) {
                size_t at = ((abbr[side] + i) - 0x80000000u) ^ 3;
                char c = at < sn.rdram.size() ? (char)sn.rdram[at] : 0;
                if (c == 0) break;
                team[side] += c;
            }
            for (int q = 0; q < 7; q++) score[side] += word_in(sn, 0x802EA43C + 4 * (side * 7 + q));
        }
        return team[0] + " " + std::to_string(score[0]) + " - " + std::to_string(score[1]) + " " + team[1] + ", " + describe_play(sn);
    }

    struct Stats {
        uint64_t frames = 0, quiescent = 0, fresh = 0;
        uint64_t refused[refusal_count] = {};
    } stats;
}

// The call site of fn's call at call_vram (0 if this build has none): host code that stands in for a game function
// (adv_engine.cpp) pushes the vanilla site's entry so a sleeping thread's chain still reads as unbroken.
extern "C" uint32_t blitz_ss_site(uint32_t fn, uint32_t call_vram) {
    for (uint32_t i = 1; i <= SS_SITE_COUNT; i++) {
        if (ss_site_vrams[i][0] == fn && ss_site_vrams[i][1] == call_vram) return i;
    }
    return 0;
}

extern "C" void blitz_ss_note_stop(uint8_t* rdram, int32_t self) {
    (void)rdram;
    ss_stack* st = ss_current_stack();
    std::lock_guard lock{threads_mutex};
    Sleeper& s = sleepers[self];
    s.valid = true;
    s.stack = StackImage::of(st);
    auto it = live.find(self);
    s.ctx = it != live.end() && it->second.stack == st ? it->second.ctx : nullptr;
    if (s.ctx == nullptr) s.valid = false;
}

// Started: from now on only a self-stop makes it snapshottable again. (A thread that has never run
// has no entry at all.)
extern "C" void blitz_ss_note_start(int32_t t) {
    std::lock_guard lock{threads_mutex};
    sleepers[t].valid = false;
}

extern "C" void blitz_ss_note_destroy(int32_t t) {
    std::lock_guard lock{threads_mutex};
    sleepers.erase(t);
    live.erase(t);
    pending_resume.erase(t);
}

namespace {
    // Unregisters a game thread as its host thread exits: queued for drop_exited, never threads_mutex here (see exited).
    struct Registration {
        int32_t thread = 0;
        uint64_t serial = 0;
        ~Registration() {
            if (thread == 0) return;
            std::lock_guard lock{exited_mutex};
            exited.emplace_back(thread, serial);
        }
    };
    thread_local Registration registration;
}

// Every game thread, as it starts (run_thread_function): register it, and if it is a recreated
// process thread, load its registers and call-site stack so its entry function resumes.
extern "C" void blitz_thread_created(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    int32_t self = ultramodern::this_thread();
    ss_stack* st = ss_current_stack();
    std::unique_lock lock{threads_mutex};
    drop_exited();
    uint64_t serial = ++next_serial;
    live[self] = {ctx, st, TO_PTR(OSThread, self)->context, serial};
    registration.thread = self;
    registration.serial = serial;
    sleepers[self] = Sleeper{};  // running
    auto it = pending_resume.find(self);
    if (it == pending_resume.end()) return;
    put_context(ctx, it->second);
    const StackImage& im = it->second.stack;
    st->depth = im.depth;
    st->overflow = 0;
    std::memcpy(st->e, im.e, sizeof(ss_entry) * std::min<uint32_t>(im.depth, SS_MAX_DEPTH));
    st->resume_pos = 0;
    st->resuming = 1;
    // Run the entry function the snapshot recorded (the address may now hold another overlay's), and
    // end the thread when it returns, as run_thread_function would.
    auto entry = (recomp_func_t*)ss_site_func[im.e[0].site];
    pending_resume.erase(it);
    lock.unlock();
    entry(rdram, ctx);
    throw ultramodern::thread_terminated{};
}

// Redo Play hooks: the top of every play (checkpoint.cpp), the end of a play (play_log.cpp), game over.
extern "C" void blitz_redo_play_top(uint8_t* rdram) {
    if (MEM_W(0, (gpr)(int32_t)0x802EA420) != 2 || !blitz::redo_play_enabled()) return;  // real games only
    redo_capture = true;
}

extern "C" void blitz_redo_play_ended(uint8_t* rdram) {
    if (MEM_W(0, (gpr)(int32_t)0x802EA420) != 2) return;
    play_ended_since = true;
}

extern "C" void blitz_redo_clear() {
    redo_capture = false;
    std::lock_guard lock{threads_mutex};
    redo_ring.clear();
}

extern "C" void blitz_redo_request() {
    if (!blitz::redo_play_enabled()) return;
    redo_request = true;
}

// Team select's loop, each frame (uniform_picker.cpp): there the game takes □ (B) as a pick, and has no back.
static std::atomic<int64_t> team_select_ms{0};
static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" void blitz_team_select_frame() {
    team_select_ms = now_ms();
}

// The game thread, each controller read (dev_input.cpp get_input, last): in the menus, an A press waits for its
// screen's snapshot (at most ~20 reads), then goes through as a press of a few reads. At team select □ goes back a
// step as everywhere else (team, 2026-10-28: "I'm at team select and I can't go back"), not a second ✕.
extern "C" void blitz_menu_step_input(int port, uint16_t* buttons) {
    constexpr uint16_t A = 0x8000, B = 0x4000;
    static uint16_t prev[4];
    static int waiting[4], releasing[4];
    static bool eat_b[4];  // □ went back: it stays away from the game until let go (else it backs out again)
    if (port < 0 || port > 3) return;
    uint16_t raw = *buttons;
    bool edge = (raw & A) && !(prev[port] & A);
    bool b_edge = (raw & B) && !(prev[port] & B);
    prev[port] = raw;
    if (!(raw & B)) eat_b[port] = false;
    if (b_edge && in_menus.load() && menu_steps_on() && now_ms() - team_select_ms.load() < 250) {
        eat_b[port] = true;
        blitz_redo_request();
    }
    if (eat_b[port]) *buttons &= (uint16_t)~B;
    if (releasing[port] > 0) {  // the held-back press, now
        releasing[port]--;
        *buttons |= A;
        return;
    }
    if (waiting[port] > 0) {
        *buttons &= (uint16_t)~A;
        if (!step_capture.load() || ++waiting[port] > 20) {
            waiting[port] = 0;
            releasing[port] = 4;
        }
        return;
    }
    if (edge && in_menus.load() && menu_steps_on()) {
        step_capture = true;
        waiting[port] = 1;
        *buttons &= (uint16_t)~A;
    }
}

namespace blitz {
    // The quit prompt (RecompFrontend's override): during a game, offer to save it first. True when
    // this prompt was shown instead of the usual one.
    // Settings → Main Menu's Save and Leave (firstrun.cpp): a save to the quit slot, as Save & Quit makes, then done(ok)
    // (on the game's thread). False when there's no game being played to save (the menus): nothing to keep.
    bool savestate_save_then(std::function<void(bool)> done) {
        if (!in_game.load() || !ultramodern::is_game_started()) return false;
        replay_close();
        {
            std::lock_guard lock{done_mutex};
            on_done_slot = 0;
            on_done = std::move(done);
        }
        request = 1;  // slot 0: the next start offers to pick it up
        return true;
    }

    bool savestate_quit_prompt() {
        if (!in_game.load() || !ultramodern::is_game_started()) return false;
        replay_close();  // Save & Quit needs the game running: it saves at the dispatcher's next turn
        std::string summary;  // as of the last play select (Redo Play's latest snapshot)
        {
            std::lock_guard lock{threads_mutex};
            if (!redo_ring.empty()) summary = describe_game(redo_ring.back());
        }
        recompui::open_choice_prompt(
            "Save this game?",
            "Save & Quit keeps it exactly as it is. Next time, Blitz offers to pick up from here.",
            "Save & Quit", "Don't Save",
            [] {
                {
                    std::lock_guard lock{done_mutex};
                    on_done_slot = 0;
                    ps5_crumb("save & quit: chosen");
                    on_done = [](bool ok) {
                        if (ok) quit_now = true;  // quit from the UI thread (savestate_startup_prompt)
                        else blitz::post_status("Couldn't save the game here; quit again to leave without saving");
                    };
                }
                request = 1;  // slot 0
            },
            [] { confirm_quit = true; },  // asked on the next UI tick: this prompt closes after its action runs
            recompui::ButtonStyle::Success, recompui::ButtonStyle::Tertiary, false);
        recompui::set_prompt_scoreline(summary);
        return true;
    }

    // UI thread, every tick: the second quit question, and once the game runs, a game saved on quitting
    // asks to be picked up again.
    void savestate_startup_prompt() {
        if (quit_now.exchange(false)) {
            ps5_crumb("save & quit: saved, quitting");
            blitz::checkpoint_discard_on_quit();
            ultramodern::quit();
            return;
        }
        if (confirm_quit.exchange(false)) {
            recompui::open_choice_prompt(
                "Quit without saving?", "This game will be lost.", "Quit", "Keep Playing",
                [] { blitz::checkpoint_discard_on_quit(); ultramodern::quit(); }, [] {}, recompui::ButtonStyle::Danger, recompui::ButtonStyle::Tertiary, true);
        }
        // The last start's resume guard, before anything else touches quit.state (its notice once the game shows).
        static bool guard_checked = false;
        static std::string guard_notice;
        if (!guard_checked) {
            guard_checked = true;
            guard_notice = guard_startup();
        }
        guard_tick();
        static bool asked = false;
        if (ask_resume_again.exchange(false)) asked = false;
        if (asked || !ultramodern::is_game_started()) return;
        // Not at the very moment the game starts but once its frames show (about a second in; 10 s at most): a PS5
        // start straight into the game hung right there with quit.state to offer (2026-11-15), its renderer starting up.
        static uint64_t frames_at_start = UINT64_MAX, started_ms = 0;
        if (frames_at_start == UINT64_MAX) {
            frames_at_start = blitz::game_frames();
            started_ms = guard_now_ms();
        }
        if (blitz::game_frames() - frames_at_start < 30 && guard_now_ms() - started_ms < 10000) return;
        asked = true;
        if (!guard_notice.empty()) {
            blitz::post_status(guard_notice);
            guard_notice.clear();
        }
        std::error_code ec;
        if (!std::filesystem::exists(state_file(0), ec)) return;
        blitz::ui_step("savestate prompt: quit.state's header");
        if (!file_compatible(0)) {
            // Saved by another version of the app: Resume could only fail, so don't offer it; keep the file aside.
            std::filesystem::path old = state_file(0);
            old.replace_extension(".old-version.state");
            std::filesystem::rename(state_file(0), old, ec);
            std::filesystem::remove(state_file(0).replace_extension(".txt"), ec);
            blitz::post_status("The game saved on quitting is from another version of the app and can't be picked up");
            return;
        }
        std::string what;
        std::getline(std::ifstream(state_file(0).replace_extension(".txt")), what);
        guard_arm(Guard::offer);
        ps5_crumb("resume: offering quit.state");
        fprintf(stderr, "[savestate] offering the game saved on quitting: %s\n", what.c_str());
        blitz::ui_step("savestate prompt: open_choice_prompt");
        recompui::open_choice_prompt(
            "Pick up your game?",
            what.empty() ? "You saved a game when you last quit." : "You saved this game when you last quit.",
            "Resume", "Discard",
            [] {
                guard_arm(Guard::resume);
                ps5_crumb("resume: Resume chosen");
                request = 2;  // slot 0
                blitz::post_status_held("Resuming your game: a few seconds...");
            },
            [] {
                std::error_code e;
                std::filesystem::remove(state_file(0), e);
                std::filesystem::remove(state_file(0).replace_extension(".txt"), e);
                guard_disarm("discarded");
            },
            recompui::ButtonStyle::Success, recompui::ButtonStyle::Danger, false);
        blitz::ui_step("savestate prompt: set_prompt_scoreline");
        recompui::set_prompt_scoreline(what);  // ○ is "later": the save stays and this asks again next start
        blitz::ui_step("savestate prompt");
        ps5_crumb("resume: prompt open");
    }
}

// For phone controllers (phone_pads.cpp): 0 menus, 1 play select, 2 a play; the side with the ball.
extern "C" int blitz_play_mode(void) {
    if (!in_game.load()) return 0;
    return blitz_play_select_on_screen() ? 1 : 2;
}
extern "C" int blitz_offense_side(void) { return offense_side.load(); }
extern "C" int blitz_port_side(int port) { return port >= 0 && port < 4 ? port_side[port].load() : -1; }
// The controller a touch plays for (BLITZ DS's taps and Stylus tiles): player 1 if he's in the game, else the first who is.
// The team played a game as player 2, and every tap was refused as the other side's (2026-11-02).
extern "C" int blitz_touch_port(void) {
    for (int p = 0; p < 4; p++) {
        if (port_side[p].load() >= 0) return p;
    }
    return 0;
}

// After a load, until the game runs at speed again (the slow handhelds compile and upload for a few seconds, and quick taps
// fall between its reads meanwhile: the team, RG DS): the notice says so and stays up. 0: not warming.
static std::atomic<uint64_t> warm_from_ms{0};

// op 1 saves, 2 loads, in the current slot.
extern "C" void blitz_savestate_request(int op) {
    int pending = request.load();
    if ((pending & 0xF) != 0 && (pending >> 4) == 0) return;  // Save & Quit or Resume is waiting: it goes first
    request = op | (current_slot.load() << 4);
    if (op == 2) blitz::post_status_held("Loading: a few seconds...");
}

// The next or previous slot (F6, or R3 + D-pad); a toast says what it holds.
extern "C" void blitz_savestate_slot_step(int delta) {
    int slot = ((current_slot.load() - 1 + delta) % slot_count + slot_count) % slot_count + 1;
    current_slot = slot;
    std::error_code ec;
    std::filesystem::path path = state_file(slot);
    std::string what = "empty";
    if (std::filesystem::exists(path, ec)) {
        auto ft = std::filesystem::last_write_time(path, ec);
        auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        std::time_t tt = std::chrono::system_clock::to_time_t(sys);
        std::tm tm = *std::localtime(&tt);
        char month[8], minute_ampm[16];
        std::strftime(month, sizeof month, "%b", &tm);
        std::strftime(minute_ampm, sizeof minute_ampm, "%M %p", &tm);
        what = "saved " + std::string(month) + " " + std::to_string(tm.tm_mday) + ", " +
               std::to_string(tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12) + ":" + minute_ampm;
    }
    blitz::post_status((slot == auto_slot ? std::string("Auto slot") : "Save slot " + std::to_string(slot)) + " (" + what + ")");
}

namespace blitz {
    extern std::atomic_bool background_hold;     // replay.cpp: the game holds at its dispatcher
    extern std::atomic_bool replay_pause_menu;   // replay.cpp: the game's pause menu is up
    void ds_press(uint16_t buttons, int reads);  // ds_view.cpp: buttons for the next few reads
}
static std::atomic_bool hold_after_save{false};  // the Auto slot first, then the hold (blitz_savestate_tick)
static std::atomic_int pause_then_save{0};       // ticks waited for the pause menu before the Auto slot (0: not waiting)
static std::atomic_bool in_background{false};

// The app going to the background (a clamshell closed, an app left on a phone). Mid-play: Start, so the game's paused when
// it's back (team), then the Auto slot once the pause menu's up (or 1.5 s on), then the game holds until it's back, so no
// plays run with nobody looking. In the menus: it holds at once. From Android's onPause (BlitzActivity, 1.4 s after an RG
// DS's lid shuts) and SDL's event (3 s later than that); the second does nothing new.
extern "C" void blitz_app_background(void) {
    if (in_background.exchange(true)) return;
    if (!in_game.load()) {
        blitz::background_hold = true;
        fprintf(stderr, "[background] in the menus: the game holds\n");
        return;
    }
    if (blitz_play_mode() == 2 && !blitz::replay_pause_menu.load()) blitz::ds_press(0x1000, 6);  // Start: the pause menu
    pause_then_save = 1;
    fprintf(stderr, "[background] mid-game: paused, then the Auto slot\n");
}

extern "C" void blitz_app_foreground(void) {
    if (!in_background.exchange(false)) return;
    pause_then_save = 0;
    hold_after_save = false;
    blitz::background_hold = false;
}

static int auto_save_watch(void*, SDL_Event* e) {
    if (e->type == SDL_APP_WILLENTERFOREGROUND || e->type == SDL_APP_DIDENTERFOREGROUND) blitz_app_foreground();
    else if (e->type == SDL_APP_WILLENTERBACKGROUND) blitz_app_background();
    return 0;
}

extern "C" void blitz_savestate_tick(uint8_t* rdram, recomp_context* ctx) {
    static const bool validate = getenv("BLITZ_SS_VALIDATE") != nullptr;
    static const bool watching = [] { SDL_AddEventWatch(auto_save_watch, nullptr); return true; }();
    (void)watching;
    int32_t phase = MEM_W(0, (gpr)(int32_t)0x802EA420);
    in_game = phase == 2;
    in_menus = phase == 1;
    if (phase == 2 && !step_ring.empty()) {  // kickoff: the menu steps are spent
        std::lock_guard lock{threads_mutex};
        step_ring.clear();
    }
    offense_side = MEM_W(0, (gpr)(int32_t)0x802EA47C) & 1;
    // Which controller sits in each player slot (Controller Select; -1 none): slots 1-2 are the left
    // team (side 0), 3-4 the right. Found by comparing RDRAM after different picks (NOTES.md).
    // Worked out first, so a phone's server thread never sees a half-updated table.
    int sides[4] = {-1, -1, -1, -1};
    for (int slot = 0; slot < 4; slot++) {
        int32_t port = MEM_W(0, (gpr)(int32_t)(0x802942A0 + 4 * slot));
        if (port >= 0 && port < 4) sides[port] = slot / 2;
    }
    for (int port = 0; port < 4; port++) port_side[port] = sides[port];
    int req = request.load();
    if (req == 0 && hold_after_save.exchange(false)) blitz::background_hold = true;  // the Auto slot's done: now hold
    // Warmed up after a load: at speed again (or 45 s on), the notice goes.
    if (uint64_t t0 = warm_from_ms.load(); t0 != 0 && now_ms() - t0 > 1500 && (blitz::fps_last_second() >= 20.0f || now_ms() - t0 > 45000)) {
        warm_from_ms = 0;
        blitz::release_status();
        blitz::post_status("Ready");
    }
    // Gone to the background mid-game: the pause menu first (it was pressed for), then the Auto slot.
    if (int w = pause_then_save.load(); w > 0) {
        if (blitz::replay_pause_menu.load() || w >= 45) {
            pause_then_save = 0;
            int expected = 0;
            if (request.compare_exchange_strong(expected, 1 | (auto_slot << 4))) fprintf(stderr, "[savestate] to the background mid-game: the Auto slot\n");
            hold_after_save = true;  // once the save's done or given up (the request clears)
        }
        else pause_then_save = w + 1;
        req = request.load();
    }
    int op = req & 0xF, slot = req >> 4;
    // Clears the request handled here only: one made meanwhile (e.g. Save & Quit) stays for the next tick.
    auto clear_request = [&] {
        int expected = req;
        request.compare_exchange_strong(expected, 0);
    };
    if (!validate && op == 0 && !redo_capture.load() && !redo_request.load() && !step_capture.load()) return;
    int32_t self = ultramodern::this_thread();
    std::lock_guard lock{threads_mutex};
    Refusal r = quiescent(rdram, self, validate ? &stats.fresh : nullptr);

    if (op != 0) {
        std::string why = r == ok ? "" : refusal_names[r];
        if (op == 2 && !(snap.valid && snap.slot == slot)) {  // not the one in memory: its file
            Snapshot loaded;
            std::string err = read_file(loaded, slot);
            if (!err.empty()) {
                blitz::release_status();
                blitz::post_status(err);
                clear_request();
                if (slot == 0) guard_failed = true;  // nothing restored: the resume guard comes off (guard_tick)
                finish(false, slot);
                return;
            }
            loaded.slot = slot;
            snap = std::move(loaded);
        }
        if (op == 2) {  // a module may refuse (e.g. another Season Mode); that is final, not a wait
            for (const auto& m : blitz::state_modules()) {
                for (const auto& [name, blob] : snap.modules) {
                    std::string err = name == m.name && m.check ? m.check(blob) : "";
                    if (!err.empty()) {
                        blitz::release_status();
                        blitz::post_status(err);
                        clear_request();
                        waited = 0;
                        wait_tally.clear();
                        if (slot == 0) guard_failed = true;
                        finish(false, slot);
                        return;
                    }
                }
            }
        }
        if (r == ok && op == 2 && !fixed_match(rdram, self, snap, why)) r = fixed_not_waiting;
        if (r == ok) {
            if (op == 1) {
                take_snapshot(rdram, ctx, self);
                snap.slot = slot;
                std::thread([copy = snap, slot] {
                    std::string n = slot_name(slot);
                    bool written = write_file(copy, slot);
                    if (slot == 0 && written) {
                        std::ofstream(state_file(0).replace_extension(".txt")) << describe_game(copy) << "\n";
                        ps5_crumb("save & quit: quit.state written");
                    }
                    else blitz::post_status(written ? n + ": state saved" : n + ": state saved, but not to a file");
                    finish(written, slot);
                }).detach();
            }
            else {
                if (slot == 0) ps5_crumb("resume: restoring");
                restore_snapshot(rdram, ctx, self, snap);
                redo_ring.clear();  // another moment, maybe another game
                if (slot == 0) {  // the game saved on quitting: used up, once its frames show (guard_tick)
                    ps5_crumb("resume: restored");
                    std::error_code ec;
                    const std::filesystem::path resuming = with_suffix(state_file(0), ".resuming");
                    std::filesystem::remove(resuming, ec);
                    std::filesystem::rename(state_file(0), resuming, ec);
                    if (ec) std::filesystem::remove(state_file(0), ec);
                    std::filesystem::remove(state_file(0).replace_extension(".txt"), ec);
                    guard_restored = true;
                    snap.valid = false;
                    snap = Snapshot{};  // its 8 MB RDRAM copy too: nothing loads it again
                    blitz::post_status_held("Game resumed: warming up, controls may lag for a moment");
                }
                else blitz::post_status_held(slot_name(slot) + ": state loaded, warming up for a moment");
                warm_from_ms = now_ms();
                finish(true, slot);
            }
            if (waited > 0) fprintf(stderr, "[savestate]   (after waiting %d ticks%s%s)\n", waited, waited > 60 ? ": " : "", waited > 60 ? tally_text().c_str() : "");
            clear_request();
            waited = 0;
            wait_tally.clear();
        }
        else if (++wait_tally[why], ++waited >= (op == 2 ? load_wait_ticks() : r == season_screen ? 20 : 600)) {
            fprintf(stderr, "[savestate] gave up %s after %d ticks: %s (%s)\n", op == 1 ? "saving" : "loading", waited, why.c_str(), tally_text().c_str());
            wait_tally.clear();
            if (op == 2 && slot == 0) ask_resume_again = true;  // quit.state is still there
            bool waiting;
            {
                std::lock_guard lock{done_mutex};
                waiting = (bool)on_done;
            }
            blitz::release_status();
            if (!waiting && r == season_screen && op == 1) blitz::post_status("Season screens can't be saved: save once the game starts");
            else if (!waiting) blitz::post_status(std::string(op == 1 ? "Can't save a state here" : "Can't load the state here") + " (" + why + ")");
            clear_request();
            waited = 0;
            if (op == 2 && slot == 0) guard_failed = true;
            finish(false, slot);
        }
    }

    // Menu steps: a snapshot for the A press waiting on it; Redo in the menus goes back one.
    if (op == 0 && step_capture.load()) {
        if (r == ok) {
            step_ring.push_back(make_snapshot(rdram, ctx, self));
            while (step_ring.size() > step_keep) step_ring.pop_front();
            fprintf(stderr, "[savestate] menu step %zu kept\n", step_ring.size());
            step_capture = false;
            step_wait = 0;
        }
        else if (++step_wait >= 15) {  // the press goes through anyway (blitz_menu_step_input)
            fprintf(stderr, "[savestate] menu step: no snapshot (%s)\n", refusal_names[r]);
            step_capture = false;
            step_wait = 0;
        }
    }
    // Redo Play (after any save/load request above).
    if (op == 0 && redo_request.load() && MEM_W(0, (gpr)(int32_t)0x802EA420) == 1) {
        if (step_ring.empty()) {
            blitz::post_status("Nothing to go back to");
            redo_request = false;
        }
        else {
            std::string why = r == ok ? "" : refusal_names[r];
            if (r == ok && !fixed_match(rdram, self, step_ring.back(), why)) r = fixed_not_waiting;
            if (r == ok) {
                restore_snapshot(rdram, ctx, self, step_ring.back());
                step_ring.pop_back();
                blitz::post_status("Back a step");
                redo_request = false;
                redo_wait = 0;
            }
            else if (++redo_wait >= 600) {
                blitz::post_status("Can't go back here (" + why + ")");
                redo_request = false;
                redo_wait = 0;
            }
        }
    }
    else if (op == 0 && redo_request.load() && MEM_W(0, (gpr)(int32_t)0x802EA420) != 2) {
        blitz::post_status("Redo Play works during a game");
        redo_request = false;
    }
    else if (op == 0 && redo_request.load()) {
        bool on_select = blitz_play_select_on_screen() != 0;
        bool select_seen = blitz_play_select_draws() != select_draws_at_newest;
        size_t n = redo_ring.size();
        // At (or before) play select with no play run since the newest snapshot: the play before.
        size_t back = (!play_ended_since && (on_select || !select_seen)) ? 2 : 1;
        if (n < back) {
            blitz::post_status(n == 0 ? "Nothing to redo yet" : "No earlier play to redo");
            redo_request = false;
        }
        else {
            const Snapshot& target = redo_ring[n - back];
            std::string why = r == ok ? "" : refusal_names[r];
            if (r == ok && !fixed_match(rdram, self, target, why)) r = fixed_not_waiting;
            if (r == ok) {
                restore_snapshot(rdram, ctx, self, target);
                std::string what = describe_play(target);
                redo_ring.resize(n - back + 1);  // the plays after it are undone
                play_ended_since = false;
                select_draws_at_newest = blitz_play_select_draws();
                redo_capture = false;
                blitz::post_status("Redo: " + what);
                redo_request = false;
                redo_wait = 0;
            }
            else if (++redo_wait >= 600) {
                blitz::post_status("Can't redo here (" + why + ")");
                redo_request = false;
                redo_wait = 0;
            }
        }
    }
    else if (op == 0 && redo_capture.load()) {
        if (r == ok) {
            redo_ring.push_back(make_snapshot(rdram, ctx, self));
            while (redo_ring.size() > redo_keep_n()) redo_ring.pop_front();
            fprintf(stderr, "[savestate] redo snapshot %zu: %s\n", redo_ring.size(), describe_play(redo_ring.back()).c_str());
            play_ended_since = false;
            select_draws_at_newest = blitz_play_select_draws();
            redo_capture = false;
            capture_wait = 0;
        }
        else if (++capture_wait >= 300) {
            fprintf(stderr, "[savestate] redo: no snapshot at this play (%s)\n", refusal_names[r]);
            redo_capture = false;
            capture_wait = 0;
        }
    }

    if (!validate) return;
    stats.frames++;
    stats.refused[r]++;
    if (r == ok) stats.quiescent++;
    if (stats.frames % 600 == 0) {
        fprintf(stderr, "[savestate] %llu frames: snapshot possible on %llu (%.0f%%); never-started processes seen %llu\n",
                (unsigned long long)stats.frames, (unsigned long long)stats.quiescent,
                100.0 * stats.quiescent / stats.frames, (unsigned long long)stats.fresh);
        for (int i = 1; i < refusal_count; i++) {
            if (stats.refused[i]) fprintf(stderr, "[savestate]   %s: %llu\n", refusal_names[i], (unsigned long long)stats.refused[i]);
        }
    }
}

#else

extern "C" void blitz_ss_note_stop(uint8_t*, int32_t) {}
extern "C" void blitz_ss_note_start(int32_t) {}
extern "C" void blitz_ss_note_destroy(int32_t) {}
extern "C" void blitz_thread_created(uint8_t*, recomp_context*) {}
extern "C" void blitz_savestate_request(int) {}  // F5/F7, R3 combos and the phone menu do nothing in a SAVESTATES=0 build
extern "C" void blitz_savestate_slot_step(int) {}
extern "C" void blitz_redo_play_top(uint8_t*) {}
extern "C" void blitz_redo_play_ended(uint8_t*) {}
extern "C" void blitz_redo_clear() {}
extern "C" void blitz_redo_request() {}
extern "C" void blitz_menu_step_input(int, uint16_t*) {}
extern "C" void blitz_team_select_frame() {}
extern "C" int blitz_play_mode(void) { return 0; }
extern "C" int blitz_offense_side(void) { return 0; }
extern "C" int blitz_port_side(int) { return -1; }
extern "C" int blitz_touch_port(void) { return 0; }
namespace blitz {
    bool savestate_quit_prompt() { return false; }
    void savestate_startup_prompt() {}
    bool savestate_save_then(std::function<void(bool)>) { return false; }
}
extern "C" void blitz_savestate_tick(uint8_t*, recomp_context*) {}

#endif
