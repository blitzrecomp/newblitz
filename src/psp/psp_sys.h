// The PSP layer's small portability shim (docs/PSP_PORT.md §2): the same calls on the PSP (sceKernel*, sceIo*) and on the Mac
// (pthreads, POSIX files), so psp_log, psp_http, psp_net and psp_remote compile unchanged for psp-host and the probe's Mac test.
// Paths are passed through as given: "ms0:/PSP/GAME/NBPROBE/logs/blitz.log" on the PSP, a plain path on the Mac.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

namespace pspsys {

// Time
uint64_t now_us();                 // monotonic microseconds (sceKernelGetSystemTimeWide / CLOCK_MONOTONIC)
void sleep_ms(int ms);
uint32_t low_digits();             // sceKernelGetSystemTimeLow() % 10000000 (the launch id's digits)

// Threads. prio follows the PSP's rule (lower runs first); the Mac ignores it. Returns 0 on failure.
typedef void (*ThreadFn)(void* arg);
int thread_start(const char* name, ThreadFn fn, void* arg, int stack_bytes, int prio);
// Waits for the thread to end. timeout_ms < 0 waits for ever. Returns true if it ended.
bool thread_join(int id, int timeout_ms);
// Kills a thread that overran (PSP: sceKernelTerminateDeleteThread). The Mac can't, so it detaches and returns false.
bool thread_kill(int id);
int thread_self();
// The folder every thread from thread_start begins in (sceIoChdir): PPSSPP needs a current directory for host0: paths.
void set_thread_cwd(const std::string& dir);
const char* thread_cwd();          // that folder ("" before set_thread_cwd), for threads made elsewhere (psp_threads.cpp)

class Mutex {
public:
    Mutex();
    ~Mutex();
    void lock();
    void unlock();
private:
    void* impl_;
};
struct Lock {
    explicit Lock(Mutex& m) : m_(m) { m_.lock(); }
    ~Lock() { m_.unlock(); }
    Mutex& m_;
};

// A counting semaphore with a timed wait (the log writer's wake-up).
class Event {
public:
    Event();
    ~Event();
    void signal();
    bool wait(int timeout_ms);   // true if signalled
private:
    void* impl_;
};

// Files
bool exists(const std::string& path);
bool is_dir(const std::string& path);
long long file_size(const std::string& path);   // -1 if missing
bool read_file(const std::string& path, std::string& out, size_t max_bytes = (size_t)-1, bool tail = false);
bool write_file(const std::string& path, const void* data, size_t len);             // tmp + rename
bool write_file_direct(const std::string& path, const void* data, size_t len);      // straight over (no rename)
bool append_file(const std::string& path, const void* data, size_t len);            // open, append, close
bool remove_file(const std::string& path);
bool rename_file(const std::string& from, const std::string& to);
bool make_dir(const std::string& path);         // one level; true if it exists afterwards
bool make_dirs(const std::string& path);        // every level below a device root
struct DirEntry { std::string name; bool dir; long long size; };
bool list_dir(const std::string& path, std::vector<DirEntry>& out);
std::string dir_name(const std::string& path);
std::string join(const std::string& a, const std::string& b);

// Memory census (PSP: user partition; the Mac reports 0)
int free_kb();
int max_free_kb();

// Emulator check (PSP: sceIoDevctl("kemulator:", 3); the Mac: 0)
int is_emulator();

// UTC wall clock from the RTC (seconds since 1970); 0 if unknown.
long long wall_clock();
std::string format_utc(long long secs, bool for_id);   // "2026-11-03T14-05-06Z" (for_id) or "2026-11-03 14:05:06Z"
long long utc_from_fields(int y, int mo, int d, int h, int mi, int s);

}  // namespace pspsys
