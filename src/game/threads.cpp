// libultra thread control for the game (renamed in syms/names.toml, left out of recompilation
// in blitz2000.toml): osStopThread / osStartThread / osSetThreadPri / osYieldThread with
// libultra's semantics, which the runtime only partly implements.
//
// Blitz depends on that. The game thread (0x80273D48) is a process dispatcher: each frame it
// sets the current process (0x802A9B88) and osStartThreads the process's thread, which runs
// until it stops itself. A process that needs I/O (e.g. func_80003490, a request to the
// controller/pak thread) first calls osStopThread(&game_thread) so the dispatcher can't move
// "current process" on while it waits for the reply.
//
// The runtime only supports a thread stopping itself (stopping another compiles to nothing), and
// its osStartThread queues the target unconditionally. With those, the dispatcher kept running,
// cleared the current process, and the waiting process then read through a null pointer.
//
// Here a stopped thread is taken out of whatever queue it is in (the run queue, or a message
// queue's blocked list) and marked STOPPED; osStartThread re-queues only STOPPED threads. A thread
// that was blocked on a message queue resumes inside the runtime's receive loop, sees the queue
// state again and re-blocks if it is still empty, so no message is lost.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

// The runtime's other gaps this file covers:
//   - osYieldThread is `assert(false)`, i.e. nothing in release builds. The dispatcher hands the
//     CPU to a process waiting on I/O with setpri(low) / yield / setpri(high), and busy-waits on
//     it, which only terminates if a yield lets the awaited hardware event through.
//   - osSetThreadPri on another thread re-sorts it with thread_queue_remove, which only ever
//     compares against the head of the queue (and loops forever otherwise), so a thread that
//     isn't first could end up queued twice.
//   - thread_queue_insert puts a thread *ahead* of others with equal priority; libultra puts it
//     behind them, and yielding depends on that.

extern "C" void blitz_ss_note_stop(uint8_t* rdram, int32_t self);  // savestate.cpp
extern "C" void blitz_ss_note_start(int32_t t);
extern "C" void blitz_ss_note_destroy(int32_t t);

namespace {
    using ThreadList = std::vector<PTR(OSThread)>;

    // Who last stopped each thread (stopping thread, return address in game code), for the
    // BLITZ_MONITOR diagnostics: a thread that stays STOPPED shows who stopped it.
    std::mutex stop_log_mutex;
    std::unordered_map<int32_t, std::pair<int32_t, uint32_t>> stop_log;

    // BLITZ_TRACE_THREADS=<n>: log the first n thread-control calls (time, op, target, caller).
    void trace(uint8_t* rdram, const char* op, PTR(OSThread) target, uint32_t ra) {
        static const char* env = getenv("BLITZ_TRACE_THREADS");
        static const int cap = env ? atoi(env) : 0;  // BLITZ_TRACE_THREADS=<max events>
        static int count = 0;
        static const auto t0 = std::chrono::steady_clock::now();
        if (count >= cap) {
            return;
        }
        count++;
        (void)ra;  // recompiled code doesn't maintain $ra for direct calls
        PTR(OSThread) self = ultramodern::this_thread();
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        fprintf(stderr, "[thr %8.3f] %-8s %d(%08X,st%d) by %d(%08X)\n", t, op,
                target ? TO_PTR(OSThread, target)->id : 0, (uint32_t)target, target ? TO_PTR(OSThread, target)->state : -1,
                self ? TO_PTR(OSThread, self)->id : 0, (uint32_t)self);
    }

    void record_stop(PTR(OSThread) target, PTR(OSThread) by, uint32_t ra) {
        std::lock_guard lock{stop_log_mutex};
        stop_log[target] = {by, ra};
    }

    ThreadList drain(uint8_t* rdram, PTR(PTR(OSThread)) queue) {
        ThreadList out;
        while (!ultramodern::thread_queue_empty(rdram, queue)) {
            out.push_back(ultramodern::thread_queue_pop(rdram, queue));
        }
        return out;
    }

    // Rebuild a queue in exactly this order (highest priority first). thread_queue_insert puts a
    // thread ahead of equal-priority ones, so inserting in reverse reproduces the order.
    void rebuild(uint8_t* rdram, PTR(PTR(OSThread)) queue, const ThreadList& order) {
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            ultramodern::thread_queue_insert(rdram, queue, *it);
        }
    }

    // libultra __osEnqueueThread order: behind every thread of equal or higher priority.
    void insert_fifo(uint8_t* rdram, ThreadList& list, PTR(OSThread) t_) {
        OSPri pri = TO_PTR(OSThread, t_)->priority;
        auto pos = list.begin();
        while (pos != list.end() && TO_PTR(OSThread, *pos)->priority >= pri) {
            ++pos;
        }
        list.insert(pos, t_);
    }

    void remove_from_queue(uint8_t* rdram, PTR(PTR(OSThread)) queue, PTR(OSThread) t_) {
        ThreadList list = drain(rdram, queue);
        std::erase(list, t_);
        rebuild(rdram, queue, list);
    }
}

extern "C" void blitz_osStopThread(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSThread) t_ = (int32_t)ctx->r4;
    PTR(OSThread) self = ultramodern::this_thread();

    trace(rdram, "stop", t_ == NULLPTR ? self : t_, (uint32_t)ctx->r31);
    record_stop(t_ == NULLPTR ? self : t_, self, (uint32_t)ctx->r31);
    if (t_ == NULLPTR || t_ == self) {
        // Mark it stopped so a later osStartThread from another thread wakes it.
        TO_PTR(OSThread, self)->state = OSThreadState::STOPPED;
        blitz_ss_note_stop(rdram, self);
        osStopThread(rdram, self);
        return;
    }

    OSThread* t = TO_PTR(OSThread, t_);
    if (t->state == OSThreadState::STOPPED) {
        return;
    }
    if (t->queue != NULLPTR) {
        remove_from_queue(rdram, t->queue, t_);
        t->queue = NULLPTR;
    }
    t->state = OSThreadState::STOPPED;
}

extern "C" void blitz_osStartThread(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSThread) t_ = (int32_t)ctx->r4;
    trace(rdram, "start", t_, (uint32_t)ctx->r31);
    // libultra ignores osStartThread on a thread that isn't stopped.
    if (TO_PTR(OSThread, t_)->state == OSThreadState::STOPPED) {
        blitz_ss_note_start(t_);
        osStartThread(rdram, t_);
    }
}

extern "C" void blitz_osSetThreadPri(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSThread) t_ = (int32_t)ctx->r4;
    OSPri pri = (OSPri)ctx->r5;
    PTR(OSThread) self = ultramodern::this_thread();
    if (t_ == NULLPTR) {
        t_ = self;
    }
    OSThread* t = TO_PTR(OSThread, t_);
    if (t->priority == pri) {
        return;
    }
    trace(rdram, pri > t->priority ? "pri_up" : "pri_down", t_, 0);
    t->priority = pri;

    // Re-sort it within whatever queue it is waiting in.
    if (t_ != self && t->state != OSThreadState::STOPPED && t->queue != NULLPTR) {
        PTR(PTR(OSThread)) queue = t->queue;
        ThreadList list = drain(rdram, queue);
        std::erase(list, t_);
        insert_fifo(rdram, list, t_);
        rebuild(rdram, queue, list);
    }

    // Run a now higher-priority thread, as libultra does.
    ultramodern::check_running_queue(rdram);
}

extern "C" void blitz_osYieldThread(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    PTR(OSThread) self = ultramodern::this_thread();
    OSThread* me = TO_PTR(OSThread, self);

    ThreadList list = drain(rdram, ultramodern::running_queue);
    bool someone_ready = !list.empty() && TO_PTR(OSThread, list.front())->priority >= me->priority;
    if (someone_ready) {
        insert_fifo(rdram, list, self);
        me->state = OSThreadState::QUEUED;
    }
    rebuild(rdram, ultramodern::running_queue, list);

    if (someone_ready) {
        ultramodern::run_next_thread_and_wait(rdram);
        return;
    }

    // Nobody else to run: Blitz yields in polling loops (the dispatcher waiting for a process
    // doing I/O, ROM reads polling for DMA completion). On hardware an interrupt would deliver
    // the awaited message meanwhile; here external events (VI, SI, PI, AI) are only delivered
    // when a game thread asks, so let one through (waiting up to 1 ms) and run whoever it woke.
    ultramodern::wait_for_external_message_timed(rdram, 1);
    ultramodern::check_running_queue(rdram);
}

// The game reads OSThread.state directly (its process system, 0x80273D48-0x80275400, checks a
// process thread's state at process+0x40). It expects libultra's values; the runtime keeps its
// own enum in that field and never records RUNNING or WAITING. Hooks at each read replace the
// loaded value with the libultra state worked out from the runtime's bookkeeping.
extern "C" uint32_t blitz_libultra_thread_state(uint8_t* rdram, int32_t t_) {
    constexpr uint32_t OS_STATE_STOPPED = 1, OS_STATE_RUNNABLE = 2, OS_STATE_RUNNING = 4, OS_STATE_WAITING = 8;
    OSThread* t = TO_PTR(OSThread, t_);
    // A thread whose function returned (or that was destroyed) has no context left. In libultra it
    // was destroyed while running, so its state still reads RUNNING, and the dispatcher relies on
    // exactly that to free a finished process (process_kill). Reporting anything else leaked every
    // finished process and its stack, and the default heap ran out when the gameplay demo loaded.
    if (t->context == nullptr || t_ == ultramodern::this_thread()) {
        return OS_STATE_RUNNING;
    }
    if (t->state == OSThreadState::STOPPED) {
        return OS_STATE_STOPPED;
    }
    if (t->queue != NULLPTR && t->queue != ultramodern::running_queue) {
        return OS_STATE_WAITING;  // on a message queue's blocked list
    }
    return OS_STATE_RUNNABLE;
}

// osDestroyThread on another thread: the runtime removes it from its queue with the broken
// thread_queue_remove. Take it out properly first, then let the runtime tear it down.
extern "C" void blitz_osDestroyThread(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSThread) t_ = (int32_t)ctx->r4;
    trace(rdram, "destroy", t_, (uint32_t)ctx->r31);
    blitz_ss_note_destroy(t_ != NULLPTR ? t_ : ultramodern::this_thread());
    PTR(OSThread) self = ultramodern::this_thread();
    if (t_ != NULLPTR && t_ != self) {
        OSThread* t = TO_PTR(OSThread, t_);
        if (t->queue != NULLPTR) {
            remove_from_queue(rdram, t->queue, t_);
            t->queue = NULLPTR;
        }
        t->state = OSThreadState::STOPPED;
    }
    osDestroyThread(rdram, t_);
}

// For dev_monitor.cpp: who stopped this thread last (0 if never).
extern "C" uint32_t blitz_thread_stopped_by(int32_t t_, int32_t* by) {
    std::lock_guard lock{stop_log_mutex};
    auto it = stop_log.find(t_);
    if (it == stop_log.end()) {
        return 0;
    }
    *by = it->second.first;
    return it->second.second;
}
