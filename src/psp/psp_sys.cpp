// See psp_sys.h.
#include "psp_sys.h"
#include <string.h>
#include <stdio.h>
#include <exception>

#ifdef __PSP__
#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <pspsysmem.h>
#ifndef PSP_THREAD_ATTR_LOW_MEM_STACK
#define PSP_THREAD_ATTR_LOW_MEM_STACK 0x00400000   // not in pspsdk's enum; the firmware's value
#endif
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#endif

namespace pspsys {

// ---- civil time, used by both builds (no timegm on newlib) ----
static long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

std::string format_utc(long long secs, bool for_id) {
    long long z = secs / 86400;
    long long rem = secs % 86400;
    if (rem < 0) { rem += 86400; z--; }
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    char buf[40];
    int H = (int)(rem / 3600), M = (int)(rem / 60 % 60), S = (int)(rem % 60);
    if (for_id) snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d-%02d-%02dZ", (int)y, m, d, H, M, S);
    else snprintf(buf, sizeof buf, "%04d-%02u-%02u %02d:%02d:%02dZ", (int)y, m, d, H, M, S);
    return buf;
}

long long utc_from_fields(int y, int mo, int d, int h, int mi, int s) {
    return days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s;
}

std::string dir_name(const std::string& in) {
    std::string path;   // "ms0:/PSP/GAME/X//EBOOT.PBP" (PPSSPP with --root) -> single slashes
    for (char c : in) if (!(c == '/' && !path.empty() && path.back() == '/')) path += c;
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        size_t colon = path.find(':');
        return colon == std::string::npos ? std::string(".") : path.substr(0, colon + 1);
    }
    if (slash > 0 && path[slash - 1] == ':') return path.substr(0, slash);   // "ms0:/EBOOT.PBP" -> "ms0:"
    return path.substr(0, slash);
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == ':') return a + b;
    return a + "/" + b;
}

bool make_dirs(const std::string& path) {
    if (is_dir(path)) return true;
    // every prefix after the device ("ms0:") or the leading '/'
    size_t start = path.find(':');
    start = (start == std::string::npos) ? 1 : start + 2;
    for (size_t i = start; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            std::string part = path.substr(0, i);
            if (!part.empty() && !is_dir(part)) make_dir(part);
        }
    }
    return is_dir(path);
}

#ifdef __PSP__
// ======================================== PSP ========================================
uint64_t now_us() { return sceKernelGetSystemTimeWide(); }
void sleep_ms(int ms) { sceKernelDelayThread((SceUInt)ms * 1000); }
uint32_t low_digits() { return sceKernelGetSystemTimeLow() % 10000000u; }

struct Start { ThreadFn fn; void* arg; char name[32]; };
static char g_cwd[256];
void set_thread_cwd(const std::string& dir) {
    snprintf(g_cwd, sizeof g_cwd, "%s", dir.c_str());
    sceIoChdir(g_cwd);
}
const char* thread_cwd() { return g_cwd; }
static int thread_entry(SceSize, void* argp) {
    // PPSSPP resolves host0: through the calling thread's current directory and a new thread has none ("NOCWD"), so every
    // thread starts in the app folder (real firmware keeps one per process; this is harmless there)
    if (g_cwd[0]) sceIoChdir(g_cwd);
    Start* s = *(Start**)argp;
    ThreadFn fn = s->fn; void* arg = s->arg;
    char name[32];
    memcpy(name, s->name, sizeof name);
    delete s;
    try {
        fn(arg);
    } catch (const std::exception& e) {   // an exception must not take the whole app down through std::terminate
        fprintf(stderr, "thread %s: uncaught exception: %s\n", name, e.what());
    } catch (...) {
        fprintf(stderr, "thread %s: uncaught exception\n", name);
    }
    return 0;
}

int thread_start(const char* name, ThreadFn fn, void* arg, int stack_bytes, int prio) {
    SceUID id = sceKernelCreateThread(name, thread_entry, prio, stack_bytes,
                                      PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU | PSP_THREAD_ATTR_LOW_MEM_STACK, NULL);
    if (id < 0) return 0;
    Start* s = new Start{fn, arg, {0}};
    snprintf(s->name, sizeof s->name, "%s", name);
    if (sceKernelStartThread(id, sizeof(s), &s) < 0) { delete s; sceKernelDeleteThread(id); return 0; }
    return id;
}

bool thread_join(int id, int timeout_ms) {
    int r;
    if (timeout_ms < 0) r = sceKernelWaitThreadEnd(id, NULL);
    else { SceUInt t = (SceUInt)timeout_ms * 1000; r = sceKernelWaitThreadEnd(id, &t); }
    if (r < 0) return false;
    sceKernelDeleteThread(id);
    return true;
}

bool thread_kill(int id) { return sceKernelTerminateDeleteThread(id) >= 0; }
int thread_self() { return sceKernelGetThreadId(); }

Mutex::Mutex() { impl_ = (void*)(intptr_t)sceKernelCreateSema("blitz_mtx", 0, 1, 1, NULL); }
Mutex::~Mutex() { sceKernelDeleteSema((SceUID)(intptr_t)impl_); }
void Mutex::lock() { sceKernelWaitSema((SceUID)(intptr_t)impl_, 1, NULL); }
void Mutex::unlock() { sceKernelSignalSema((SceUID)(intptr_t)impl_, 1); }

Event::Event() { impl_ = (void*)(intptr_t)sceKernelCreateSema("blitz_evt", 0, 0, 1 << 20, NULL); }
Event::~Event() { sceKernelDeleteSema((SceUID)(intptr_t)impl_); }
void Event::signal() { sceKernelSignalSema((SceUID)(intptr_t)impl_, 1); }
bool Event::wait(int timeout_ms) {
    SceUInt t = (SceUInt)timeout_ms * 1000;
    return sceKernelWaitSema((SceUID)(intptr_t)impl_, 1, timeout_ms < 0 ? NULL : &t) >= 0;
}

bool exists(const std::string& path) {
    SceIoStat st;
    memset(&st, 0, sizeof st);
    if (sceIoGetstat(path.c_str(), &st) >= 0) return true;
    return is_dir(path);
}

bool is_dir(const std::string& path) {
    SceUID d = sceIoDopen(path.c_str());
    if (d < 0) return false;
    sceIoDclose(d);
    return true;
}

long long file_size(const std::string& path) {
    SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    long long n = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoClose(fd);
    return n;
}

bool read_file(const std::string& path, std::string& out, size_t max_bytes, bool tail) {
    out.clear();
    SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    long long size = sceIoLseek(fd, 0, PSP_SEEK_END);
    long long start = 0;
    if (size < 0) size = 0;
    if ((size_t)size > max_bytes) { if (tail) start = size - (long long)max_bytes; size = (long long)max_bytes; }
    sceIoLseek(fd, start, PSP_SEEK_SET);
    out.resize((size_t)size);
    size_t got = 0;
    while (got < out.size()) {
        int n = sceIoRead(fd, &out[got], (SceSize)(out.size() - got));
        if (n <= 0) break;
        got += (size_t)n;
    }
    out.resize(got);
    sceIoClose(fd);
    return true;
}

static bool write_fd(SceUID fd, const void* data, size_t len) {
    const char* p = (const char*)data;
    while (len) {
        int n = sceIoWrite(fd, p, (SceSize)len);
        if (n <= 0) return false;
        p += n; len -= (size_t)n;
    }
    return true;
}

bool write_file_direct(const std::string& path, const void* data, size_t len) {
    SceUID fd = sceIoOpen(path.c_str(), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return false;
    bool ok = write_fd(fd, data, len);
    sceIoClose(fd);
    return ok;
}

bool write_file(const std::string& path, const void* data, size_t len) {
    std::string tmp = path + ".tmp";
    if (!write_file_direct(tmp, data, len)) return false;
    sceIoRemove(path.c_str());   // FAT: the rename target must not exist
    if (sceIoRename(tmp.c_str(), path.c_str()) >= 0) return true;
    sceIoRemove(tmp.c_str());
    return write_file_direct(path, data, len);
}

bool append_file(const std::string& path, const void* data, size_t len) {
    SceUID fd = sceIoOpen(path.c_str(), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0) return false;
    bool ok = write_fd(fd, data, len);
    sceIoClose(fd);
    return ok;
}

bool remove_file(const std::string& path) { return sceIoRemove(path.c_str()) >= 0; }
bool rename_file(const std::string& from, const std::string& to) {
    sceIoRemove(to.c_str());
    return sceIoRename(from.c_str(), to.c_str()) >= 0;
}
bool make_dir(const std::string& path) { sceIoMkdir(path.c_str(), 0777); return is_dir(path); }

bool list_dir(const std::string& path, std::vector<DirEntry>& out) {
    out.clear();
    SceUID d = sceIoDopen(path.c_str());
    if (d < 0) return false;
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof e);
        if (sceIoDread(d, &e) <= 0) break;
        if (!strcmp(e.d_name, ".") || !strcmp(e.d_name, "..")) continue;
        out.push_back({e.d_name, FIO_S_ISDIR(e.d_stat.st_mode) != 0, (long long)e.d_stat.st_size});
    }
    sceIoDclose(d);
    return true;
}

int free_kb() { return (int)(sceKernelTotalFreeMemSize() / 1024); }
int max_free_kb() { return (int)(sceKernelMaxFreeMemSize() / 1024); }

int is_emulator() {
    static int cached = -1;
    if (cached < 0) {
        int out = 0;
        int r = sceIoDevctl("kemulator:", 3, NULL, 0, &out, sizeof out);
        cached = (r >= 0 && out) ? 1 : 0;
    }
    return cached;
}

long long wall_clock() {
    ScePspDateTime t;
    memset(&t, 0, sizeof t);
    if (sceRtcGetCurrentClock(&t, 0) < 0 || t.year < 2000) return 0;
    return utc_from_fields(t.year, t.month, t.day, t.hour, t.minute, t.second);
}

#else
// ======================================== Mac / POSIX ========================================
void set_thread_cwd(const std::string&) {}   // the Mac has real paths everywhere
const char* thread_cwd() { return ""; }
uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
uint32_t low_digits() { return (uint32_t)(now_us() % 10000000u); }

namespace {
struct HostThread {
    pthread_t t;
    ThreadFn fn;
    void* arg;
    pthread_mutex_t m;
    pthread_cond_t c;
    bool done;
};
Mutex g_threads_mtx;
std::vector<HostThread*> g_threads;   // index + 1 = id
void* host_entry(void* p) {
    HostThread* h = (HostThread*)p;
    h->fn(h->arg);
    pthread_mutex_lock(&h->m);
    h->done = true;
    pthread_cond_broadcast(&h->c);
    pthread_mutex_unlock(&h->m);
    return nullptr;
}
}

int thread_start(const char*, ThreadFn fn, void* arg, int stack_bytes, int) {
    HostThread* h = new HostThread();
    h->fn = fn; h->arg = arg; h->done = false;
    pthread_mutex_init(&h->m, nullptr);
    pthread_cond_init(&h->c, nullptr);
    pthread_attr_t a;
    pthread_attr_init(&a);
    // the PSP's stack size, rounded up to the host's minimum, so a deep stack shows up here too
    size_t sz = (size_t)stack_bytes < 65536 ? 65536 : (size_t)stack_bytes;
    pthread_attr_setstacksize(&a, (sz + 16383) & ~(size_t)16383);
    if (pthread_create(&h->t, &a, host_entry, h) != 0) { pthread_attr_destroy(&a); delete h; return 0; }
    pthread_attr_destroy(&a);
    Lock l(g_threads_mtx);
    g_threads.push_back(h);
    return (int)g_threads.size();
}

static HostThread* host_thread(int id) {
    Lock l(g_threads_mtx);
    if (id <= 0 || (size_t)id > g_threads.size()) return nullptr;
    return g_threads[(size_t)id - 1];
}

bool thread_join(int id, int timeout_ms) {
    HostThread* h = host_thread(id);
    if (!h) return false;
    pthread_mutex_lock(&h->m);
    if (timeout_ms < 0) {
        while (!h->done) pthread_cond_wait(&h->c, &h->m);
    } else {
        struct timeval now;
        gettimeofday(&now, nullptr);
        struct timespec until;
        long long ns = (long long)now.tv_usec * 1000 + (long long)(timeout_ms % 1000) * 1000000;
        until.tv_sec = now.tv_sec + timeout_ms / 1000 + (time_t)(ns / 1000000000);
        until.tv_nsec = (long)(ns % 1000000000);
        while (!h->done) if (pthread_cond_timedwait(&h->c, &h->m, &until) == ETIMEDOUT) break;
    }
    bool done = h->done;
    pthread_mutex_unlock(&h->m);
    if (done) pthread_join(h->t, nullptr);
    return done;
}

bool thread_kill(int id) {
    HostThread* h = host_thread(id);
    if (h) pthread_detach(h->t);
    return false;
}
int thread_self() { return (int)(intptr_t)pthread_self(); }

Mutex::Mutex() { pthread_mutex_t* m = new pthread_mutex_t; pthread_mutex_init(m, nullptr); impl_ = m; }
Mutex::~Mutex() { pthread_mutex_destroy((pthread_mutex_t*)impl_); delete (pthread_mutex_t*)impl_; }
void Mutex::lock() { pthread_mutex_lock((pthread_mutex_t*)impl_); }
void Mutex::unlock() { pthread_mutex_unlock((pthread_mutex_t*)impl_); }

namespace { struct HostEvent { pthread_mutex_t m; pthread_cond_t c; int count; }; }
Event::Event() {
    HostEvent* e = new HostEvent();
    pthread_mutex_init(&e->m, nullptr); pthread_cond_init(&e->c, nullptr); e->count = 0;
    impl_ = e;
}
Event::~Event() { delete (HostEvent*)impl_; }
void Event::signal() {
    HostEvent* e = (HostEvent*)impl_;
    pthread_mutex_lock(&e->m); e->count++; pthread_cond_signal(&e->c); pthread_mutex_unlock(&e->m);
}
bool Event::wait(int timeout_ms) {
    HostEvent* e = (HostEvent*)impl_;
    pthread_mutex_lock(&e->m);
    struct timeval now;
    gettimeofday(&now, nullptr);
    struct timespec until;
    long long ns = (long long)now.tv_usec * 1000 + (long long)(timeout_ms % 1000) * 1000000;
    until.tv_sec = now.tv_sec + timeout_ms / 1000 + (time_t)(ns / 1000000000);
    until.tv_nsec = (long)(ns % 1000000000);
    while (e->count == 0) {
        if (timeout_ms < 0) pthread_cond_wait(&e->c, &e->m);
        else if (pthread_cond_timedwait(&e->c, &e->m, &until) == ETIMEDOUT) break;
    }
    bool got = e->count > 0;
    if (got) e->count--;
    pthread_mutex_unlock(&e->m);
    return got;
}

bool exists(const std::string& path) { struct stat st; return stat(path.c_str(), &st) == 0; }
bool is_dir(const std::string& path) { struct stat st; return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
long long file_size(const std::string& path) { struct stat st; return stat(path.c_str(), &st) == 0 ? (long long)st.st_size : -1; }

bool read_file(const std::string& path, std::string& out, size_t max_bytes, bool tail) {
    out.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long long size = ftell(f), start = 0;
    if (size < 0) size = 0;
    if ((size_t)size > max_bytes) { if (tail) start = size - (long long)max_bytes; size = (long long)max_bytes; }
    fseek(f, (long)start, SEEK_SET);
    out.resize((size_t)size);
    size_t got = size ? fread(&out[0], 1, out.size(), f) : 0;
    out.resize(got);
    fclose(f);
    return true;
}

bool write_file_direct(const std::string& path, const void* data, size_t len) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}
bool write_file(const std::string& path, const void* data, size_t len) {
    std::string tmp = path + ".tmp";
    if (!write_file_direct(tmp, data, len)) return false;
    return rename(tmp.c_str(), path.c_str()) == 0;
}
bool append_file(const std::string& path, const void* data, size_t len) {
    FILE* f = fopen(path.c_str(), "ab");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}
bool remove_file(const std::string& path) { return unlink(path.c_str()) == 0; }
bool rename_file(const std::string& from, const std::string& to) { return rename(from.c_str(), to.c_str()) == 0; }
bool make_dir(const std::string& path) { mkdir(path.c_str(), 0777); return is_dir(path); }

bool list_dir(const std::string& path, std::vector<DirEntry>& out) {
    out.clear();
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    while (struct dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string full = join(path, e->d_name);
        struct stat st;
        bool ok = stat(full.c_str(), &st) == 0;
        out.push_back({e->d_name, ok && S_ISDIR(st.st_mode), ok ? (long long)st.st_size : 0});
    }
    closedir(d);
    return true;
}

int free_kb() { return 0; }
int max_free_kb() { return 0; }
int is_emulator() { return 0; }
long long wall_clock() { return (long long)time(nullptr); }
#endif

}  // namespace pspsys
