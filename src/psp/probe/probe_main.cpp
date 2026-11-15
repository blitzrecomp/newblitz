// The PSP probe EBOOT (docs/PSP_PORT.md §7): what the device can do, in sections, cheapest and safest first.
// Before each section it writes <app>/probe_stage and logs "PROBE begin <section>"; a section the last run died in is reported
// fail reason=killed_last_run and skipped. Each section runs on its own thread under a timeout (fail reason=timeout, then the
// next one). Every section prints exactly one "PROBE result" line and the run ends with "PROBE done". The report goes to every
// log sink and to <app>/probe.txt, and to the Worker as kind=probe when <app>/data/net.cfg is there and the net comes up.
// The app folder is the EBOOT's own folder (argv[0]), so the probe runs from any folder.
// Switch files in the app folder: serve (stay up N seconds, the file's number or 60, serving commands), autoexit (exit 10 s
// after the report, as under the emulator), volatile (try sceKernelVolatileMemLock).
#include "../psp_http.h"
#include "../psp_log.h"
#include "../psp_net.h"
#include "../psp_remote.h"
#include "../psp_sys.h"

#include <pspaudio.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspsuspend.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <pspmath.h>
#include <pspfpu.h>
#if PROBE_CFW_IMPORTS
#include <kubridge.h>
#include <systemctrl.h>
#endif

#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// moodycamel's lightweightsemaphore.h knows Windows, Mach, __unix__ (POSIX sem_t) and FreeRTOS. psp-gcc defines none of them, so
// the probe takes the POSIX branch over pthread-embedded's sem_t (section 4 says whether that works).
#if !defined(__unix__)
#define __unix__ 1
#define PROBE_FAKE_UNIX 1
#endif
#include "blockingconcurrentqueue.h"
#ifdef PROBE_FAKE_UNIX
#undef __unix__
#endif
#include "json.hpp"

PSP_MODULE_INFO("NBProbe", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
PSP_HEAP_SIZE_KB(4096);

#ifndef BLITZ_BUILD
#define BLITZ_BUILD "0"
#endif
#ifndef BLITZ_COMMIT
#define BLITZ_COMMIT "unknown"
#endif
#ifndef PROBE_N_ASPMAIN
#define PROBE_N_ASPMAIN 0
#endif
#ifndef PROBE_N_ASPMAIN_ERR
#define PROBE_N_ASPMAIN_ERR "not tried"
#endif

namespace {

// 8 MB of RDRAM plus a 64 KB guard either side, as the game will have it (§4.2)
constexpr size_t RDRAM = 8 * 1024 * 1024, GUARD = 64 * 1024;
uint8_t g_rdram[RDRAM + 2 * GUARD] __attribute__((aligned(64)));

std::string g_app;
int g_entry_total_kb = 0, g_entry_max_kb = 0;
std::string g_report;
pspsys::Mutex* g_report_mtx = nullptr;
volatile bool g_exit = false;

// ---- report lines ----
void pline(const char* f, ...) __attribute__((format(printf, 1, 2)));
void pline(const char* f, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    std::string line = std::string("PROBE ") + buf;
    psplog::raw(line.c_str());
    pspsys::Lock l(*g_report_mtx);
    g_report += psplog::redact(line) + "\n";
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n' || c == '\r') { out += ' '; continue; }
        out += c;
    }
    return out + "\"";
}

struct Ctx {
    const char* sec;
    int ok = 0, fail = 0;
    bool skip = false;
    std::string reason;
};

void kv(Ctx& c, const char* key, const char* f, ...) __attribute__((format(printf, 3, 4)));
void kv(Ctx& c, const char* key, const char* f, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    pline("%s.%s=%s", c.sec, key, buf);
}
void kvs(Ctx& c, const char* key, const std::string& v) { pline("%s.%s=%s", c.sec, key, quote(v).c_str()); }

// a check: "PROBE <sec>.<key> status=ok|fail <extra>"
void check(Ctx& c, const char* key, bool ok, const char* f = nullptr, ...) __attribute__((format(printf, 4, 5)));
void check(Ctx& c, const char* key, bool ok, const char* f, ...) {
    char buf[400] = "";
    if (f) {
        va_list ap;
        va_start(ap, f);
        vsnprintf(buf, sizeof buf, f, ap);
        va_end(ap);
    }
    pline("%s.%s status=%s%s%s", c.sec, key, ok ? "ok" : "fail", buf[0] ? " " : "", buf);
    (ok ? c.ok : c.fail)++;
}

double us_now() { return (double)pspsys::now_us(); }
std::string app(const char* rel) { return pspsys::join(g_app, rel); }

// ======================================== 1 id ========================================
void sec_id(Ctx& c) {
    int emu = pspsys::is_emulator();
    kv(c, "emulator", "%d", emu);
    c.ok++;
    int devkit = sceKernelDevkitVersion();
    kv(c, "devkit", "\"%X.%02X\"", (unsigned)devkit >> 24, ((unsigned)devkit >> 16) & 0xFF);
    kv(c, "devkit_raw", "%d", devkit);
    (devkit > 0 ? c.ok : c.fail)++;
#if PROBE_CFW_IMPORTS
    kv(c, "model", "%d", kuKernelGetModel());
    kv(c, "hen_version", "%d", sctrlHENGetVersion());
    c.ok += 2;
#else
    // kuKernelGetModel and sctrlHENGetVersion import CFW libraries; on a PSP without them the EBOOT would not load at all.
    // PROBE_CFW_IMPORTS=1 builds them in. Without it the model comes from the memory size: 32 MB is a PSP-1000.
    int total = g_entry_total_kb + (int)((RDRAM + 2 * GUARD) / 1024) + 4096;
    kvs(c, "model_guess", total > 30000 ? "2000+ (64 MB)" : "1000 (32 MB)");
    kvs(c, "model_src", "memory (PROBE_CFW_IMPORTS=0)");
#endif
    kvs(c, "build", BLITZ_BUILD);
    kvs(c, "commit", BLITZ_COMMIT);
    kvs(c, "launch", psplog::launch_id());
    kvs(c, "app", g_app);
    kvs(c, "rtc", pspsys::format_utc(pspsys::wall_clock(), false));
}

// ======================================== 2 clock ========================================
double loop_mhz() {
    // two instructions a turn (bnez + addiu in its delay slot), about 2 cycles on the Allegrex
    const unsigned n = 4 * 1000 * 1000;
    unsigned i = n;
    double t0 = us_now();
    __asm__ volatile(
        ".set push\n.set noreorder\n"
        "1: bnez %0, 1b\n"
        "   addiu %0, %0, -1\n"
        ".set pop\n"
        : "+r"(i));
    double us = us_now() - t0;
    return us > 0 ? (2.0 * n) / us : 0;
}

void sec_clock(Ctx& c) {
    kv(c, "before.cpu", "%d", scePowerGetCpuClockFrequencyInt());
    kv(c, "before.bus", "%d", scePowerGetBusClockFrequencyInt());
    kv(c, "before.loop_mhz", "%.1f", loop_mhz());
    int rc = scePowerSetClockFrequency(333, 333, 166);
    check(c, "set_333", rc >= 0, "rc=%d", rc);
    int cpu = scePowerGetCpuClockFrequencyInt(), bus = scePowerGetBusClockFrequencyInt();
    kv(c, "after.cpu", "%d", cpu);
    kv(c, "after.bus", "%d", bus);
    double mhz = loop_mhz();
    kv(c, "after.loop_mhz", "%.1f", mhz);
    check(c, "at_333", cpu == 333, "cpu=%d bus=%d", cpu, bus);
    kvs(c, "pll", "not in pspsdk's stubs (scePowerGetPllClockFrequencyInt)");
    pspFpuSetEnable(0);
    kv(c, "fpu_exceptions", "0");
}

// ======================================== 3 mem ========================================
void sec_mem(Ctx& c) {
    kv(c, "entry.total_kb", "%d", g_entry_total_kb);
    kv(c, "entry.max_kb", "%d", g_entry_max_kb);
    kv(c, "rdram_bss_kb", "%u", (unsigned)((RDRAM + 2 * GUARD) / 1024));
    memset(g_rdram, 0, sizeof g_rdram);
    for (size_t i = GUARD; i < GUARD + RDRAM; i += 4096) g_rdram[i] = (uint8_t)i;
    bool guards = true;
    for (size_t i = 0; i < GUARD; i++) if (g_rdram[i] || g_rdram[GUARD + RDRAM + i]) guards = false;
    check(c, "rdram_touch", guards, "addr=0x%08X", (unsigned)(uintptr_t)(g_rdram + GUARD));
    kv(c, "after_rdram.total_kb", "%d", pspsys::free_kb());
    kv(c, "after_rdram.max_kb", "%d", pspsys::max_free_kb());
    // the stack pool as §4.1 budgets it: 32 x 32 KB + 8 x 64 KB + main 256 KB
    const int pool = 32 * 32 * 1024 + 8 * 64 * 1024 + 256 * 1024;
    SceUID blk = sceKernelAllocPartitionMemory(2, "probe_stackpool", PSP_SMEM_Low, pool, NULL);
    check(c, "stackpool_alloc", blk >= 0, "kb=%d rc=0x%08X", pool / 1024, (unsigned)blk);
    kv(c, "after_stackpool.total_kb", "%d", pspsys::free_kb());
    kv(c, "after_stackpool.max_kb", "%d", pspsys::max_free_kb());
    if (blk >= 0) sceKernelFreePartitionMemory(blk);
    // the heap is fixed (PSP_HEAP_SIZE_KB(4096) here, 2560 in the game): a 2.5 MB block, then the largest single malloc
    void* big = malloc(2560 * 1024);
    if (big) memset(big, 1, 2560 * 1024);
    check(c, "heap_2560kb", big != nullptr);
    free(big);
    size_t lo = 0, hi = 8u * 1024 * 1024;
    while (hi - lo > 16 * 1024) {
        size_t mid = (lo + hi) / 2;
        void* p = malloc(mid);
        if (p) { ((volatile uint8_t*)p)[0] = 1; ((volatile uint8_t*)p)[mid - 1] = 1; free(p); lo = mid; } else hi = mid;
    }
    kv(c, "heap.largest_kb", "%u", (unsigned)(lo / 1024));
    kv(c, "heap.size_kb", "4096");
    kv(c, "after_heap_search.total_kb", "%d", pspsys::free_kb());
    kv(c, "end.total_kb", "%d", pspsys::free_kb());
    kv(c, "end.max_kb", "%d", pspsys::max_free_kb());
}

// ======================================== 4 tool ========================================
thread_local int t_counter = 0;
__attribute__((noinline)) void tl_bump() { t_counter++; }

void sec_tool(Ctx& c) {
    // std::thread
    try {
        double t0 = us_now();
        int v = 0;
        std::thread t([&] { v = 42; });
        t.join();
        check(c, "std_thread", v == 42, "us=%.0f", us_now() - t0);
    } catch (const std::exception& e) {
        check(c, "std_thread", false, "err=%s", quote(e.what()).c_str());
    }
    // std::mutex alone, contended by two threads
    try {
        std::mutex m;
        long long shared = 0;
        const int N = 20000;
        double t0 = us_now();
        std::thread t([&] { for (int i = 0; i < N; i++) { std::lock_guard<std::mutex> g(m); shared++; } });
        for (int i = 0; i < N; i++) { std::lock_guard<std::mutex> g(m); shared++; }
        t.join();
        check(c, "mutex", shared == 2 * N, "ns_per_lock=%.0f", (us_now() - t0) * 1000.0 / (2 * N));
    } catch (const std::exception& e) {
        check(c, "mutex", false, "err=%s", quote(e.what()).c_str());
    }
    // std::condition_variable ping-pong, every wait time-boxed (the first PPSSPP run livelocked here with plain waits)
    try {
        std::mutex m;
        std::condition_variable cv;
        int turn = 0;
        bool stop = false;
        const int N = 1000;
        int done = 0;
        double t0 = us_now();
        std::thread t([&] {
            for (int i = 0; i < N; i++) {
                std::unique_lock<std::mutex> l(m);
                if (!cv.wait_for(l, std::chrono::milliseconds(1000), [&] { return turn == 1 || stop; }) || stop) return;
                turn = 0;
                cv.notify_one();
            }
        });
        for (; done < N; done++) {
            std::unique_lock<std::mutex> l(m);
            turn = 1;
            cv.notify_one();
            if (!cv.wait_for(l, std::chrono::milliseconds(1000), [&] { return turn == 0; })) break;
        }
        { std::lock_guard<std::mutex> g(m); stop = true; }
        cv.notify_all();
        t.join();
        double us = us_now() - t0;
        check(c, "condvar", done == N, "roundtrips=%d of %d us_per_roundtrip=%.1f", done, N, done ? us / done : 0.0);
    } catch (const std::exception& e) {
        check(c, "condvar", false, "err=%s", quote(e.what()).c_str());
    }
    // thread_local: speed and that each thread has its own
    {
        const int N = 200000;
        t_counter = 0;
        double t0 = us_now();
        for (int i = 0; i < N; i++) tl_bump();
        double ns = (us_now() - t0) * 1000.0 / N;
        int other = -1;
        std::thread t([&] { tl_bump(); other = t_counter; });
        t.join();
        check(c, "thread_local", t_counter == N && other == 1, "ns=%.1f main=%d other=%d", ns, t_counter, other);
    }
    // 64-bit atomics
    {
        std::atomic<uint64_t> a{0};
        const int N = 50000;
        double t0 = us_now();
        for (int i = 0; i < N; i++) a.fetch_add(1, std::memory_order_relaxed);
        double ns = (us_now() - t0) * 1000.0 / N;
        std::thread t([&] { for (int i = 0; i < N; i++) a.fetch_add(0x100000000ull); });
        for (int i = 0; i < N; i++) a.fetch_add(1);
        t.join();
        uint64_t want = (uint64_t)N * 2 + (uint64_t)N * 0x100000000ull;
        uint64_t got = a.load();
        bool cas = false;
        uint64_t exp = got;
        cas = a.compare_exchange_strong(exp, 0x123456789ABCDEFull) && a.load() == 0x123456789ABCDEFull;
        check(c, "atomic64", got == want && cas, "ns=%.1f lock_free=%d got=0x%llX", ns, (int)a.is_lock_free(), (unsigned long long)got);
    }
    // std::filesystem on ms0: and host0:
    for (const char* which : {"ms0", "host0"}) {
        std::string dir = which[0] == 'm' ? g_app : std::string("host0:/blitz-psp");
        std::string key = std::string("filesystem_") + which;
        try {
            int n = 0;
            for (auto& e : std::filesystem::directory_iterator(dir)) { (void)e; n++; }
            check(c, key.c_str(), true, "entries=%d", n);
        } catch (const std::exception& e) {
            check(c, key.c_str(), false, "err=%s", quote(e.what()).c_str());
        }
    }
    // <chrono>
    {
        using clk = std::chrono::steady_clock;
        auto a = clk::now();
        long long best = -1;
        for (int i = 0; i < 20000; i++) {
            auto b = clk::now();
            long long d = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
            if (d > 0 && (best < 0 || d < best)) best = d;
            a = b;
        }
        auto sys = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        check(c, "chrono", best > 0, "steady_res_ns=%lld system_minus_rtc_s=%lld", best, (long long)sys - pspsys::wall_clock());
    }
    // moodycamel BlockingConcurrentQueue round trip
    try {
        moodycamel::BlockingConcurrentQueue<int> q1, q2;
        const int N = 1000;
        double t0 = us_now();
        std::thread t([&] { int v; for (int i = 0; i < N; i++) { q1.wait_dequeue(v); q2.enqueue(v + 1); } });
        bool good = true;
        for (int i = 0; i < N; i++) {
            q1.enqueue(i);
            int v = -1;
            if (!q2.wait_dequeue_timed(v, std::chrono::seconds(2)) || v != i + 1) { good = false; break; }
        }
        if (!good) t.detach(); else t.join();
        check(c, "moodycamel", good, "us_per_roundtrip=%.1f", (us_now() - t0) / N);
    } catch (const std::exception& e) {
        check(c, "moodycamel", false, "err=%s", quote(e.what()).c_str());
    }
    // nlohmann parse of a ~600 B document
    try {
        std::string doc = "{\"format\":\"blitz-rosters/1\",\"week\":10,\"updated\":\"2026-11-03T12:00:00Z\",\"files\":{";
        for (int i = 0; doc.size() < 560; i++) doc += (i ? "," : "") + std::string("\"lineups/T") + std::to_string(i) + ".json\":{\"sha1\":\"0123456789abcdef0123456789abcdef01234567\",\"bytes\":" + std::to_string(1000 + i) + "}";
        doc += "},\"targets\":{\"psp/pack.txt\":{\"platforms\":[\"PSP\"]}}}";
        double t0 = us_now();
        auto j = nlohmann::json::parse(doc);
        double us = us_now() - t0;
        check(c, "nlohmann", j["format"] == "blitz-rosters/1" && j["targets"]["psp/pack.txt"]["platforms"][0] == "PSP", "bytes=%u us=%.0f",
              (unsigned)doc.size(), us);
    } catch (const std::exception& e) {
        check(c, "nlohmann", false, "err=%s", quote(e.what()).c_str());
    }
    // exceptions
    {
        double t0 = us_now();
        int caught = 0;
        for (int i = 0; i < 100; i++) {
            try { throw std::runtime_error("probe"); } catch (const std::runtime_error& e) { if (!strcmp(e.what(), "probe")) caught++; }
        }
        check(c, "exceptions", caught == 100, "us_per_throw=%.1f", (us_now() - t0) / 100);
    }
    kv(c, "n_aspmain_compiled", "%d", PROBE_N_ASPMAIN);
    kvs(c, "n_aspmain_note", PROBE_N_ASPMAIN_ERR);
}

// ======================================== 5 thread ========================================
SceUID g_hold = -1;
int hold_entry(SceSize, void*) { sceKernelWaitSema(g_hold, 1, NULL); return 0; }
SceUID g_pa = -1, g_pb = -1;
volatile bool g_pp_stop = false;
int pong_entry(SceSize, void*) {
    while (!g_pp_stop) {
        if (sceKernelWaitSema(g_pa, 1, NULL) < 0) break;
        sceKernelSignalSema(g_pb, 1);
    }
    return 0;
}

void sec_thread(Ctx& c) {
    g_hold = sceKernelCreateSema("probe_hold", 0, 0, 64, NULL);
    std::vector<SceUID> ids;
    int before = pspsys::free_kb();
    SceUID last_err = 0;
    for (int i = 0; i < 64; i++) {
        SceUID id = sceKernelCreateThread("probe_slot", hold_entry, 0x30, 32 * 1024,
                                          PSP_THREAD_ATTR_USER | 0x00400000 /* LOW_MEM_STACK */, NULL);
        if (id < 0) { last_err = id; break; }
        if (sceKernelStartThread(id, 0, NULL) < 0) { sceKernelDeleteThread(id); break; }
        ids.push_back(id);
    }
    int after = pspsys::free_kb();
    check(c, "create_32kb", ids.size() >= 32, "count=%u of 64 kb_used=%d err=0x%08X", (unsigned)ids.size(), before - after, (unsigned)last_err);
    sceKernelSignalSema(g_hold, (int)ids.size());
    for (SceUID id : ids) { SceUInt t = 1000000; sceKernelWaitThreadEnd(id, &t); sceKernelDeleteThread(id); }
    sceKernelDeleteSema(g_hold);
    kv(c, "free_kb_after_delete", "%d", pspsys::free_kb());

    // semaphore ping-pong for 1 s
    g_pa = sceKernelCreateSema("probe_pa", 0, 0, 1, NULL);
    g_pb = sceKernelCreateSema("probe_pb", 0, 0, 1, NULL);
    g_pp_stop = false;
    SceUID t = sceKernelCreateThread("probe_pong", pong_entry, 0x20, 16 * 1024, PSP_THREAD_ATTR_USER, NULL);
    sceKernelStartThread(t, 0, NULL);
    uint64_t t0 = pspsys::now_us();
    int n = 0;
    while (pspsys::now_us() - t0 < 1000000) {
        sceKernelSignalSema(g_pa, 1);
        SceUInt to = 500000;
        if (sceKernelWaitSema(g_pb, 1, &to) < 0) break;
        n++;
    }
    g_pp_stop = true;
    sceKernelSignalSema(g_pa, 1);
    SceUInt to = 500000;
    sceKernelWaitThreadEnd(t, &to);
    sceKernelDeleteThread(t);
    sceKernelDeleteSema(g_pa);
    sceKernelDeleteSema(g_pb);
    check(c, "sema_pingpong", n > 100, "per_s=%d", n);
    kvs(c, "host_thread", "not built yet (00NN-host-thread.patch, M4)");
}

// ======================================== 6 bench ========================================
void sec_bench(Ctx& c) {
    {
        const int N = 2000000;
        volatile int32_t sink;
        int32_t a = 1, s = 0;
        double t0 = us_now();
        for (int i = 0; i < N; i++) { s += a; if (s > 1000000) s -= 999999; a ^= i; }
        sink = s;
        (void)sink;
        double us = us_now() - t0;
        kv(c, "int32_mops", "%.1f", N / us);
    }
    {
        const int N = 2000000;
        volatile int64_t sink;
        int64_t a = 1, s = 0;
        double t0 = us_now();
        for (int i = 0; i < N; i++) { s += a; if (s > 1000000000000ll) s -= 999999999999ll; a ^= i; }
        sink = s;
        (void)sink;
        double us = us_now() - t0;
        kv(c, "int64_mops", "%.1f", N / us);
    }
    {
        const int N = 100000;
        volatile double x = 1.0001, y = 0.9999, r = 0;
        double t0 = us_now();
        for (int i = 0; i < N; i++) r = x + y;
        double add = (us_now() - t0) * 1000.0 / N;
        t0 = us_now();
        for (int i = 0; i < N; i++) r = x * y;
        double mul = (us_now() - t0) * 1000.0 / N;
        (void)r;
        kv(c, "adddf3_ns", "%.1f", add);
        kv(c, "muldf3_ns", "%.1f", mul);
    }
    {
        const int N = 200000;
        volatile float x = 2.0f, r = 0;
        double t0 = us_now();
        for (int i = 0; i < N; i++) r = sqrtf(x);
        (void)r;
        kv(c, "sqrtf_ns", "%.1f", (us_now() - t0) * 1000.0 / N);
    }
    {
        static ScePspFMatrix4 A __attribute__((aligned(16))), B __attribute__((aligned(16))), C __attribute__((aligned(16)));
        float* a = (float*)&A;
        float* b = (float*)&B;
        for (int i = 0; i < 16; i++) { a[i] = (float)(i % 5) * 0.5f; b[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
        const int N = 100000;
        __asm__ volatile(
            "lv.q C000, 0(%0)\n lv.q C010, 16(%0)\n lv.q C020, 32(%0)\n lv.q C030, 48(%0)\n"
            "lv.q C100, 0(%1)\n lv.q C110, 16(%1)\n lv.q C120, 32(%1)\n lv.q C130, 48(%1)\n"
            ::"r"(a), "r"(b) : "memory");
        double t0 = us_now();
        for (int i = 0; i < N; i++) __asm__ volatile("vmmul.q M200, M000, M100\n");
        double ns = (us_now() - t0) * 1000.0 / N;
        __asm__ volatile("sv.q C200, 0(%0)\n sv.q C210, 16(%0)\n sv.q C220, 32(%0)\n sv.q C230, 48(%0)\n" ::"r"(&C) : "memory");
        kv(c, "vfpu_mmul4_ns", "%.1f", ns);
        float s = 0;
        t0 = us_now();
        for (int i = 0; i < N; i++) s += vfpu_sinf((float)i * 0.001f);
        kv(c, "vfpu_sinf_ns", "%.1f", (us_now() - t0) * 1000.0 / N);
        check(c, "vfpu", isfinite(s) && isfinite(((float*)&C)[0]), "sum=%.3f", (double)s);
    }
    {
        const size_t SZ = 256 * 1024;
        uint8_t* src = (uint8_t*)memalign(64, SZ);
        uint8_t* dst = (uint8_t*)memalign(64, SZ);
        if (src && dst) {
            memset(src, 0x5A, SZ);
            double t0 = us_now();
            for (int i = 0; i < 16; i++) memcpy(dst, src, SZ);
            double cached = (16.0 * SZ) / (us_now() - t0);
            sceKernelDcacheWritebackInvalidateAll();
            uint8_t* us = (uint8_t*)((uintptr_t)src | 0x40000000u);
            uint8_t* ud = (uint8_t*)((uintptr_t)dst | 0x40000000u);
            t0 = us_now();
            for (int i = 0; i < 4; i++) memcpy(ud, us, SZ);
            double uncached = (4.0 * SZ) / (us_now() - t0);
            kv(c, "memcpy_cached_mbs", "%.1f", cached);
            kv(c, "memcpy_uncached_mbs", "%.1f", uncached);
            check(c, "memcpy", dst[SZ - 1] == 0x5A);
        } else {
            check(c, "memcpy", false, "err=\"no memory\"");
        }
        free(src);
        free(dst);
    }
    kvs(c, "gpr32_func_80008C78", "not built yet (M3: gpr32.py)");
}

// ======================================== 7 ms ========================================
double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = (size_t)(p * (double)(v.size() - 1) + 0.5);
    return v[std::min(i, v.size() - 1)];
}

void sec_ms(Ctx& c) {
    std::string rom = app("rom/blitz.z64");
    if (!pspsys::exists(rom)) rom = pspsys::join(pspsys::dir_name(g_app), "NBCLASSIC/rom/blitz.z64");
    bool made = false;
    const size_t chunk = 64 * 1024;
    std::vector<uint8_t> buf(chunk);
    if (!pspsys::exists(rom)) {
        rom = app("probe_ms.bin");
        made = true;
        const int MB = 2;
        for (size_t i = 0; i < chunk; i++) buf[i] = (uint8_t)(i * 31);
        double t0 = us_now();
        SceUID fd = sceIoOpen(rom.c_str(), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        bool ok = fd >= 0;
        for (int i = 0; ok && i < MB * 16; i++) ok = sceIoWrite(fd, buf.data(), chunk) == (int)chunk;
        if (fd >= 0) sceIoClose(fd);
        check(c, "write_test_file", ok, "mbs=%.2f", (MB * 1048576.0) / (us_now() - t0));
    }
    kvs(c, "file", made ? "probe_ms.bin (no ROM in the kit)" : rom);
    SceUID fd = sceIoOpen(rom.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) { check(c, "open", false, "rc=0x%08X", (unsigned)fd); return; }
    long long size = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    long long want = std::min<long long>(size, 4ll * 1024 * 1024), got = 0;
    double t0 = us_now();
    while (got < want) {
        int n = sceIoRead(fd, buf.data(), chunk);
        if (n <= 0) break;
        got += n;
    }
    double seq = got / (us_now() - t0);
    check(c, "seq_read", got == want, "mbs=%.2f bytes=%lld", seq, got);
    for (size_t blk : {(size_t)4096, (size_t)32768}) {
        std::vector<double> ms;
        uint32_t r = 12345;
        for (int i = 0; i < 40; i++) {
            r = r * 1103515245u + 12345u;
            long long off = ((long long)(r >> 4) % std::max<long long>(1, size - (long long)blk)) & ~4095ll;
            double a = us_now();
            sceIoLseek(fd, off, PSP_SEEK_SET);
            int n = sceIoRead(fd, buf.data(), blk);
            ms.push_back((us_now() - a) / 1000.0);
            if (n != (int)blk) break;
        }
        std::string key = blk == 4096 ? "rand_4k" : "rand_32k";
        check(c, key.c_str(), ms.size() == 40, "p50_ms=%.2f p99_ms=%.2f", pct(ms, 0.5), pct(ms, 0.99));
    }
    sceIoClose(fd);
    if (made) pspsys::remove_file(rom);
    // append-and-close, as the log does
    std::string ap = app("probe_append.bin");
    std::vector<double> ms;
    for (int i = 0; i < 20; i++) {
        double a = us_now();
        bool ok = pspsys::append_file(ap, buf.data(), 16 * 1024);
        ms.push_back((us_now() - a) / 1000.0);
        if (!ok) break;
    }
    pspsys::remove_file(ap);
    check(c, "append_16k", ms.size() == 20, "p50_ms=%.2f p99_ms=%.2f", pct(ms, 0.5), pct(ms, 0.99));
}

// ======================================== 8 ge ========================================
unsigned int __attribute__((aligned(16))) g_list[256 * 1024];
struct GeVert { uint16_t u, v; uint32_t color; int16_t x, y, z, pad; };
GeVert __attribute__((aligned(16))) g_verts[1024];
uint16_t __attribute__((aligned(16))) g_idx[15000];
uint32_t __attribute__((aligned(16))) g_tex[256 * 256];

void swizzle_tex(uint8_t* out, const uint8_t* in, int width_bytes, int height) {
    int rowblocks = width_bytes / 16;
    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width_bytes; i++) {
            int blockx = i / 16, blocky = j / 8;
            int x = i - blockx * 16, y = j - blocky * 8;
            int block = blockx + blocky * rowblocks;
            out[block * 128 + y * 16 + x] = in[j * width_bytes + i];
        }
    }
}

double ge_frame(Ctx& c, bool swz, double& submit_ms) {
    double t0 = us_now();
    sceGuStart(GU_DIRECT, g_list);
    sceGuClearColor(0xFF302010);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, swz ? 1 : 0);
    sceGuTexImage(0, 256, 256, 256, g_tex);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_2D, 15000, g_idx, g_verts);
    sceGuFinish();
    double t1 = us_now();
    sceGuSync(0, 0);
    double t2 = us_now();
    submit_ms = (t1 - t0) / 1000.0;
    (void)c;
    return (t2 - t1) / 1000.0;
}

void sec_ge(Ctx& c) {
    uint32_t r = 777;
    for (int i = 0; i < 1024; i++) {
        r = r * 1103515245u + 12345u;
        g_verts[i].x = (int16_t)(r % 480);
        g_verts[i].y = (int16_t)((r >> 10) % 272);
        g_verts[i].z = 0;
        g_verts[i].u = (uint16_t)((r >> 3) % 256);
        g_verts[i].v = (uint16_t)((r >> 13) % 256);
        g_verts[i].color = 0xFF000000u | (r & 0xFFFFFF);
        g_verts[i].pad = 0;
    }
    for (int i = 0; i < 15000; i++) { r = r * 1103515245u + 12345u; g_idx[i] = (uint16_t)((r >> 8) % 1024); }
    static uint32_t lin[256 * 256];
    for (int y = 0; y < 256; y++) for (int x = 0; x < 256; x++) lin[y * 256 + x] = ((x ^ y) & 16) ? 0xFFFFFFFFu : 0xFF4080FFu;
    memcpy(g_tex, lin, sizeof lin);
    sceKernelDcacheWritebackAll();

    double t0 = us_now();
    int rc = sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_8888, (void*)0, 512);
    sceGuDispBuffer(480, 272, (void*)0x88000, 512);
    sceGuDepthBuffer((void*)0x110000, 512);
    sceGuOffset(2048 - 240, 2048 - 136);
    sceGuViewport(2048, 2048, 480, 272);
    sceGuScissor(0, 0, 480, 272);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuShadeModel(GU_SMOOTH);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    check(c, "init", true, "ms=%.2f rc=%d", (us_now() - t0) / 1000.0, rc);

    double submit = 0, ge = 0;
    for (int f = 0; f < 3; f++) {
        ge = ge_frame(c, false, submit);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
    kv(c, "tris", "5000");
    kv(c, "linear.ge_ms", "%.2f", ge);
    kv(c, "linear.submit_ms", "%.2f", submit);
    double t1 = us_now();
    swizzle_tex((uint8_t*)g_tex, (const uint8_t*)lin, 256 * 4, 256);
    sceKernelDcacheWritebackAll();
    kv(c, "swizzle_cpu_ms", "%.2f", (us_now() - t1) / 1000.0);
    for (int f = 0; f < 3; f++) {
        ge = ge_frame(c, true, submit);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
    kv(c, "swizzled.ge_ms", "%.2f", ge);
    kv(c, "swizzled.submit_ms", "%.2f", submit);
    check(c, "draw", ge >= 0);
    // a screenshot of what is shown, the same bytes the shot verb returns
    std::string name, body;
    bool ok = false;
    pspremote::execute("shot probe", name, body, ok);
    pspsys::make_dir(app("shots"));
    bool wrote = ok && pspsys::write_file(app("shots/probe.raw"), body.data(), body.size());
    check(c, "screenshot", wrote, "bytes=%u file=\"shots/probe.raw\"", (unsigned)body.size());
    sceGuTerm();
}

// ======================================== 9 audio ========================================
void sec_audio(Ctx& c) {
    const int SAMPLES = 1024;
    int ch = sceAudioSRCChReserve(SAMPLES, 22050, 2);
    check(c, "src_reserve", ch >= 0, "rc=0x%08X", (unsigned)ch);
    if (ch < 0) return;
    static int16_t buf[SAMPLES * 2] __attribute__((aligned(64)));
    int blocks = (22050 + SAMPLES - 1) / SAMPLES;
    double t0 = us_now(), worst = 0;
    float ph = 0;
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < SAMPLES; i++) {
            int16_t s = (int16_t)(sinf(ph) * 6000);
            ph += 2 * 3.14159265f * 440.0f / 22050.0f;
            if (ph > 6.2831853f) ph -= 6.2831853f;
            buf[i * 2] = buf[i * 2 + 1] = s;
        }
        double a = us_now();
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX / 4, buf);
        worst = std::max(worst, (us_now() - a) / 1000.0);
    }
    double total = (us_now() - t0) / 1000.0;
    double expect = blocks * SAMPLES * 1000.0 / 22050.0;
    kv(c, "tone_ms", "%.0f", total);
    kv(c, "expected_ms", "%.0f", expect);
    kv(c, "worst_block_ms", "%.1f", worst);
    // a blocking output that came back much faster than real time for the whole tone means the queue never drained (no underrun
    // check is possible from user mode); much slower means the CPU couldn't keep up
    check(c, "tone_1s", total > expect * 0.5 && total < expect * 2.0);
    sceAudioSRCChRelease();
}

// ======================================== 10 input ========================================
void sec_input(Ctx& c) {
    if (pspsys::is_emulator()) { c.skip = true; c.reason = "emulator"; return; }
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    unsigned seen = 0;
    int lx = 255, hx = 0, ly = 255, hy = 0;
    uint64_t t0 = pspsys::now_us();
    while (pspsys::now_us() - t0 < 5000000) {
        SceCtrlData d;
        sceCtrlReadBufferPositive(&d, 1);
        seen |= d.Buttons;
        lx = std::min(lx, (int)d.Lx); hx = std::max(hx, (int)d.Lx);
        ly = std::min(ly, (int)d.Ly); hy = std::max(hy, (int)d.Ly);
    }
    kv(c, "buttons_seen", "\"0x%08X\"", seen);
    kv(c, "nub_x", "\"%d..%d\"", lx, hx);
    kv(c, "nub_y", "\"%d..%d\"", ly, hy);
    check(c, "read", true);
}

// ======================================== 11 net, 12 net222 ========================================
std::string host_of(const std::string& base) {
    psphttp::Url u;
    return psphttp::parse_url(base + "/", u) ? u.host : std::string();
}

void sec_net(Ctx& c) {
    const pspremote::NetCfg& cfg = pspremote::net_cfg();
    kv(c, "net_cfg", "%d", cfg.present ? 1 : 0);
    pspnet::start(cfg.present ? cfg.wifi_profile : 1);
    bool up = pspnet::wait_up(45000);
    pspnet::Report r = pspnet::report();
    kv(c, "wlan_switch", "%d", r.wlan_switch);
    kv(c, "free_kb_before", "%d", r.free_kb_before);
    kv(c, "free_kb_after", "%d", r.free_kb_after);
    kv(c, "max_kb_after", "%d", r.max_kb_after);
    kv(c, "pool", "%d", r.pool);
    kv(c, "pool_max_used", "%d", r.pool_max);
    kv(c, "rc", "\"common=0x%08X inet=0x%08X netinit=0x%08X inetinit=0x%08X resolver=0x%08X apctl=0x%08X connect=0x%08X\"",
       (unsigned)r.rc_common, (unsigned)r.rc_inet, (unsigned)r.rc_netinit, (unsigned)r.rc_inetinit, (unsigned)r.rc_resolver,
       (unsigned)r.rc_apctl, (unsigned)r.rc_connect);
    std::string states;
    for (auto& s : r.states) states += (states.empty() ? "" : ",") + std::to_string(s.first) + "@" + std::to_string(s.second);
    kvs(c, "apctl_states", states);
    check(c, "got_ip", up, "ms=%d ip=%s ssid=%s state=%s", r.ms_to_ip, quote(r.ip).c_str(), quote(r.ssid).c_str(),
          pspnet::state_name(pspnet::state()));
    if (!up) { c.reason = r.error.empty() ? pspnet::state_name(pspnet::state()) : r.error; return; }
    if (!cfg.present) { kvs(c, "worker", "skipped: no data/net.cfg"); return; }
    for (const std::string* b : {&cfg.logs_base, &cfg.hub_base}) {
        if (b->empty()) continue;
        uint32_t addr = 0;
        int ms = 0;
        std::string err;
        bool ok = psphttp::resolve(host_of(*b), addr, ms, err);
        check(c, b == &cfg.logs_base ? "dns_logs" : "dns_hub", ok, "ms=%d", ms);
    }
    psphttp::Response m;
    if (!cfg.hub_base.empty()) {
        bool ok = psphttp::get(cfg.hub_base + "/manifest.json", m, 10000);
        check(c, "get_manifest", ok && m.status == 200, "http=%d bytes=%u ms=%d err=%s", m.status, (unsigned)m.body.size(), m.ms,
              quote(m.error).c_str());
    }
    std::string id;
    std::string so_far;
    { pspsys::Lock l(*g_report_mtx); so_far = g_report; }
    bool posted = pspremote::worker_upload("probe", so_far, &id, false);
    check(c, "post_probe", posted, "id=%s", quote(id).c_str());
    int lines = pspremote::worker_poll();
    check(c, "cmd_poll", lines >= 0, "lines=%d", lines);
    bool res = pspremote::worker_put_res("probe-ok", "probe " + psplog::launch_id() + " ok\n");
    check(c, "post_res", res);
    psphttp::Response d;
    psphttp::get(cfg.logs_base + "/res/psp-probe-ok", d, 10000);
    if (d.date_utc > 0) kv(c, "rtc_minus_date_s", "%lld", pspsys::wall_clock() - d.date_utc);
    kvs(c, "worker_launch", pspremote::worker_launch());
}

void sec_net222(Ctx& c) {
    if (!pspnet::up()) { c.skip = true; c.reason = "net not up"; return; }
    const pspremote::NetCfg& cfg = pspremote::net_cfg();
    int rc = scePowerSetClockFrequency(222, 222, 111);
    check(c, "set_222", rc >= 0, "cpu=%d", scePowerGetCpuClockFrequencyInt());
    if (cfg.present && !cfg.logs_base.empty()) {
        int lines = pspremote::worker_poll();
        check(c, "cmd_poll", lines >= 0, "lines=%d", lines);
        psphttp::Response m;
        if (!cfg.hub_base.empty()) {
            bool ok = psphttp::get(cfg.hub_base + "/manifest.json", m, 10000);
            check(c, "get_manifest", ok && m.status == 200, "http=%d ms=%d", m.status, m.ms);
        }
    } else {
        kvs(c, "worker", "skipped: no data/net.cfg");
    }
    rc = scePowerSetClockFrequency(333, 333, 166);
    check(c, "back_to_333", rc >= 0, "cpu=%d", scePowerGetCpuClockFrequencyInt());
}

// ======================================== 13 volatile, 14 me ========================================
void sec_volatile(Ctx& c) {
    if (!pspsys::exists(app("volatile"))) { c.skip = true; c.reason = "no volatile switch file"; return; }
    void* p = nullptr;
    int size = 0;
    int rc = sceKernelVolatileMemLock(0, &p, &size);
    check(c, "lock", rc >= 0, "rc=0x%08X addr=0x%08X kb=%d", (unsigned)rc, (unsigned)(uintptr_t)p, size / 1024);
    if (rc >= 0) sceKernelVolatileMemUnlock(0);
}

void sec_me(Ctx& c) { c.skip = true; c.reason = "not built in v1 (M11)"; }

// ======================================== the runner ========================================
struct Section { const char* name; void (*fn)(Ctx&); int timeout_ms; };
const Section SECTIONS[] = {
    {"id", sec_id, 5000},        {"clock", sec_clock, 15000}, {"mem", sec_mem, 15000},      {"tool", sec_tool, 30000},
    {"thread", sec_thread, 20000}, {"bench", sec_bench, 40000}, {"ms", sec_ms, 60000},       {"ge", sec_ge, 20000},
    {"audio", sec_audio, 10000}, {"input", sec_input, 10000}, {"net", sec_net, 60000},       {"net222", sec_net222, 30000},
    {"volatile", sec_volatile, 5000}, {"me", sec_me, 1000},
};

struct Run { const Section* s; Ctx* c; };
void section_thread(void* p) {
    Run* r = (Run*)p;
    r->s->fn(*r->c);
}

int exit_callback(int, int, void*) {
    psplog::shutdown("HOME exit");
    sceKernelExitGame();
    return 0;
}
int callback_thread(SceSize, void*) {
    int cb = sceKernelCreateCallback("probe_exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

void exit_hook(void*) { g_exit = true; }

void write_report() {
    std::string copy;
    { pspsys::Lock l(*g_report_mtx); copy = g_report; }
    pspsys::write_file(app("probe.txt"), copy.data(), copy.size());
}

bool v_stage(const std::vector<std::string>&, std::string& out, void*) {
    std::string s;
    pspsys::read_file(app("probe_stage"), s, 64);
    out = "probe stage " + (s.empty() ? std::string("(none)") : s) + "\n";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    g_entry_total_kb = pspsys::free_kb();
    g_entry_max_kb = pspsys::max_free_kb();
    g_report_mtx = new pspsys::Mutex();
    g_app = (argc > 0 && argv && argv[0] && strchr(argv[0], ':')) ? pspsys::dir_name(argv[0]) : std::string("ms0:/PSP/GAME/NBPROBE");

    SceUID cbt = sceKernelCreateThread("probe_cb", callback_thread, 0x11, 0x1000, PSP_THREAD_ATTR_USER, NULL);
    if (cbt >= 0) sceKernelStartThread(cbt, 0, NULL);

    psplog::Options lo;
    lo.app_dir = g_app;
    lo.host0_dir = "host0:/blitz-psp";
    lo.build = BLITZ_BUILD;
    lo.commit = BLITZ_COMMIT;
    lo.flavour = "probe";
    lo.stdout_immediate = true;
    psplog::init(lo);
    PLOG("probe", "argv[0] %s", argc > 0 && argv[0] ? argv[0] : "(none)");

    pspremote::Options ro;
    ro.app_dir = g_app;
    ro.host0_dir = "host0:/blitz-psp";
    pspremote::init(ro);
    pspremote::set_exit_hook(exit_hook, nullptr);
    pspremote::add_verb("stage", v_stage, nullptr, false);
    pspremote::start();

    pspDebugScreenInit();
    // BRANDING: probe screen title
    pspDebugScreenPrintf("New Blitz probe %s (%s)\nlaunch %s\n\n", BLITZ_BUILD, BLITZ_COMMIT, psplog::launch_id().c_str());

    std::string killed;
    pspsys::read_file(app("probe_stage"), killed, 64);
    while (!killed.empty() && (killed.back() == '\n' || killed.back() == '\r' || killed.back() == ' ')) killed.pop_back();
    if (!killed.empty()) PWARN("probe", "the last run died in section %s: it is skipped this time", killed.c_str());

    int emu = pspsys::is_emulator();
    pline("v1 build=%s commit=%s launch=%s device=%s emulator=%d", BLITZ_BUILD, BLITZ_COMMIT, psplog::launch_id().c_str(),
          pspremote::net_cfg().device.c_str(), emu);
    uint64_t t_start = pspsys::now_us();
    int pass = 0, partial = 0, fail = 0, skip = 0;
    std::vector<std::string> summary;
    for (const Section& s : SECTIONS) {
        Ctx c;
        c.sec = s.name;
        pline("begin %s", s.name);
        double t0 = us_now();
        std::string status;
        if (killed == s.name) {
            status = "fail";
            c.reason = "killed_last_run";
        } else {
            pspsys::write_file_direct(app("probe_stage"), s.name, strlen(s.name));
            psplog::flush();
            pspDebugScreenPrintf("  %-9s ...", s.name);
            Run r{&s, &c};
            int th = pspsys::thread_start("probe_section", section_thread, &r, 256 * 1024, 0x20);
            if (!th) {
                status = "fail";
                c.reason = "thread_start";
            } else if (!pspsys::thread_join(th, s.timeout_ms)) {
                pspsys::thread_kill(th);
                status = "fail";
                c.reason = "timeout";
            } else if (c.skip) {
                status = "skip";
            } else if (c.fail == 0 && c.ok > 0) {
                status = "ok";
            } else if (c.fail == 0) {
                status = c.reason.empty() ? "ok" : "fail";
            } else {
                status = c.ok > 0 ? "partial" : "fail";
            }
        }
        int ms = (int)((us_now() - t0) / 1000);
        std::string extra;
        if (status != "skip") extra += " ok=" + std::to_string(c.ok) + " fail=" + std::to_string(c.fail);
        if (!c.reason.empty()) extra += " reason=" + (c.reason.find(' ') == std::string::npos ? c.reason : quote(c.reason));
        pline("result %s status=%s ms=%d%s", s.name, status.c_str(), ms, extra.c_str());
        if (status == "ok") pass++;
        else if (status == "partial") partial++;
        else if (status == "skip") skip++;
        else fail++;
        summary.push_back(std::string(s.name) + " " + status);
        if (strcmp(s.name, "ge") != 0) pspDebugScreenPrintf(" %s\n", status.c_str());
        else { pspDebugScreenInit(); for (auto& l : summary) pspDebugScreenPrintf("  %s\n", l.c_str()); }
        write_report();
    }
    pspsys::remove_file(app("probe_stage"));
    double secs = (double)(pspsys::now_us() - t_start) / 1e6;
    pline("done pass=%d partial=%d fail=%d skip=%d secs=%.1f", pass, partial, fail, skip, secs);
    write_report();
    if (pspremote::worker_ready()) {
        std::string body;
        { pspsys::Lock l(*g_report_mtx); body = g_report; }
        std::string id;
        if (pspremote::worker_upload("probe", body, &id, false)) PLOG("probe", "report uploaded: %s", id.c_str());
    }
    psplog::flush();

    pspDebugScreenInit();
    // BRANDING: probe screen title
    pspDebugScreenPrintf("New Blitz probe %s (%s)\n\n", BLITZ_BUILD, BLITZ_COMMIT);
    for (auto& l : summary) pspDebugScreenPrintf("  %s\n", l.c_str());
    pspDebugScreenPrintf("\nDONE pass %d partial %d fail %d skip %d (%.1f s)\n", pass, partial, fail, skip, secs);

    std::string serve;
    int wait_s = -1;   // -1: until START
    if (pspsys::read_file(app("serve"), serve, 32)) wait_s = atoi(serve.c_str()) > 0 ? atoi(serve.c_str()) : 60;
    else if (emu || pspsys::exists(app("autoexit"))) wait_s = 10;
    if (wait_s > 0) {
        PLOG("probe", "serving commands for %d s (exit to leave sooner)", wait_s);
        pspDebugScreenPrintf("\nexiting in %d s (commands are served until then)\n", wait_s);
    } else {
        PLOG("probe", "waiting for START (commands are served meanwhile)");
        pspDebugScreenPrintf("\nPress START to exit\n");
    }
    uint64_t until = pspsys::now_us() + (uint64_t)(wait_s > 0 ? wait_s : 0) * 1000000ull;
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    while (!g_exit) {
        if (wait_s > 0 && pspsys::now_us() >= until) break;
        if (wait_s < 0) {
            SceCtrlData d;
            sceCtrlPeekBufferPositive(&d, 1);
            if (d.Buttons & PSP_CTRL_START) break;
        }
        pspsys::sleep_ms(100);
    }
    PLOG("probe", "exit");
    pspremote::stop();
    psplog::shutdown(g_exit ? "exit command" : "probe done");
    sceKernelExitGame();
    return 0;
}
