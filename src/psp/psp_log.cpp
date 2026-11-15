// See psp_log.h.
#include "psp_log.h"
#include "psp_sys.h"
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <ctype.h>

#ifdef __PSP__
#include <pspiofilemgr.h>
#else
#include <unistd.h>
#endif

namespace psplog {
namespace {

constexpr size_t RING = 64 * 1024;
constexpr long long MS0_CAP = 1024 * 1024;
constexpr size_t DEAD_TAIL = 256 * 1024;
constexpr int PENDING_KEEP = 5;

char g_ring[RING];
uint64_t g_head = 0;          // bytes ever written
uint64_t g_flushed = 0;       // bytes handed to stdout + ms0
uint64_t g_host0_pos = 0;     // bytes handed to host0
pspsys::Mutex* g_mtx = nullptr;
pspsys::Mutex* g_flush_mtx = nullptr;
pspsys::Event* g_wake = nullptr;
Options g_opt;
std::string g_launch, g_dead, g_blitz_log, g_blitz_old, g_marker, g_host0_log;
std::vector<std::string> g_secrets;
uint64_t g_t0 = 0;
bool g_ready = false, g_stop = false, g_host0 = false;
uint64_t g_host0_checked = 0;
long long g_ms0_size = 0;
int g_writer = 0;
int g_flush_ms = 1000;
Stats g_stats = {};

void write_stdout(const char* s, size_t n) {
#ifdef __PSP__
    sceIoWrite(1, s, n);
#else
    if (g_opt.stdout_fd >= 0) {
        while (n > 0) {
            ssize_t w = ::write(g_opt.stdout_fd, s, n);
            if (w <= 0) break;
            s += w;
            n -= (size_t)w;
        }
        return;
    }
    fwrite(s, 1, n, stdout);
    fflush(stdout);
#endif
}

// whole lines out of the ring between two positions (the caller holds g_mtx)
std::string slice(uint64_t from, uint64_t to) {
    if (to > g_head) to = g_head;
    if (to > RING && from < to - RING) from = to - RING;
    std::string out;
    if (from >= to) return out;
    // at most two memcpys (the ring wraps once): g_mtx is held here, and a game thread's push may be waiting on it
    size_t n = (size_t)(to - from), at = (size_t)(from % RING), first = n < RING - at ? n : RING - at;
    out.resize(n);
    memcpy(&out[0], g_ring + at, first);
    if (n > first) memcpy(&out[first], g_ring, n - first);
    return out;
}

void push(const char* s, size_t n, bool urgent) {
    if (!g_mtx) { write_stdout(s, n); return; }
    {
        pspsys::Lock l(*g_mtx);
        if (n > RING / 2) n = RING / 2;
        if (g_head + n - g_flushed > RING) { g_stats.dropped++; return; }
        size_t at = (size_t)(g_head % RING), first = n < RING - at ? n : RING - at;
        memcpy(g_ring + at, s, first);
        if (n > first) memcpy(g_ring, s + first, n - first);
        g_head += n;
        g_stats.lines++;
        g_stats.bytes += n;
    }
    if (g_opt.stdout_immediate) write_stdout(s, n);
    if (urgent && g_wake) g_wake->signal();
}

void rotate_ms0() {
    pspsys::remove_file(g_blitz_old);
    if (pspsys::exists(g_blitz_log)) pspsys::rename_file(g_blitz_log, g_blitz_old);
    g_ms0_size = 0;
}

void check_host0(bool force) {
    if (g_opt.host0_dir.empty()) return;
    uint64_t now = pspsys::now_us();
    if (!force && now - g_host0_checked < 10 * 1000000ull) return;
    g_host0_checked = now;
    bool up = pspsys::is_dir(g_opt.host0_dir);
    if (up && !g_host0) {
        pspsys::make_dir(pspsys::join(g_opt.host0_dir, "run"));
        // catch up from the oldest line still in the ring, so the host0 log starts at the launch line
        pspsys::Lock l(*g_mtx);
        g_host0_pos = g_head > RING ? g_head - RING : 0;
    }
    g_host0 = up;
}

void writer_main(void*) {
    while (!g_stop) {
        g_wake->wait(g_flush_ms);
        if (g_stop) break;
        flush();
    }
}

std::string launch_stamp() {
    long long t = pspsys::wall_clock();
    char digits[16];
    snprintf(digits, sizeof digits, "%u", (unsigned)pspsys::low_digits());
    return pspsys::format_utc(t, true) + "-" + digits;
}

void prune_pending() {
    std::vector<PendingFile> p = pending();
    std::vector<std::string> dead;
    for (auto& f : p) if (f.name.compare(0, 5, "dead-") == 0) dead.push_back(f.path);
    std::sort(dead.begin(), dead.end());
    while ((int)dead.size() > PENDING_KEEP) { pspsys::remove_file(dead.front()); dead.erase(dead.begin()); }
}

}  // namespace

void init(const Options& opt) {
    if (g_ready) return;
    g_opt = opt;
    g_flush_ms = opt.flush_ms > 0 ? opt.flush_ms : 1000;
    g_t0 = pspsys::now_us();
    g_mtx = new pspsys::Mutex();
    g_flush_mtx = new pspsys::Mutex();
    g_wake = new pspsys::Event();
    g_launch = launch_stamp();
    pspsys::set_thread_cwd(opt.app_dir);

    std::string logs = app_path("logs");
    pspsys::make_dirs(pspsys::join(logs, "pending"));
    g_blitz_log = pspsys::join(logs, "blitz.log");
    g_blitz_old = pspsys::join(logs, "blitz.1.log");
    g_marker = pspsys::join(logs, "running");

    // A marker left behind means the last run died without its exit path: keep its tail for upload.
    std::string marker, dead_launch;
    if (pspsys::read_file(g_marker, marker, 512)) {
        size_t at = marker.find("launch=");
        if (at != std::string::npos) {
            size_t end = marker.find_first_of(" \r\n", at + 7);
            dead_launch = marker.substr(at + 7, end == std::string::npos ? std::string::npos : end - (at + 7));
        }
        for (char& c : dead_launch) if (!isalnum((unsigned char)c) && c != '-' && c != '_') c = '_';
        if (dead_launch.empty()) dead_launch = g_launch + "-unknown";
        std::string tail_text;
        pspsys::read_file(g_blitz_log, tail_text, DEAD_TAIL, true);
        if (tail_text.size() == DEAD_TAIL) {   // start at a whole line
            size_t nl = tail_text.find('\n');
            if (nl != std::string::npos) tail_text.erase(0, nl + 1);
        }
        std::string head = "dead run " + dead_launch + " (marker: " + redact(marker.substr(0, marker.find('\n'))) +
                           "), found by launch " + g_launch + "; last " + std::to_string(tail_text.size()) + " bytes of blitz.log:\n";
        g_dead = pspsys::join(pspsys::join(logs, "pending"), "dead-" + dead_launch + ".log");
        std::string body = head + tail_text;
        pspsys::write_file(g_dead, body.data(), body.size());
        prune_pending();
    }
    rotate_ms0();
    std::string mark = "launch=" + g_launch + " build=" + opt.build + " commit=" + opt.commit + " flavour=" + opt.flavour + "\n";
    pspsys::write_file_direct(g_marker, mark.data(), mark.size());

    if (!opt.host0_dir.empty()) g_host0_log = pspsys::join(pspsys::join(opt.host0_dir, "run"), g_launch + ".log");
    g_ready = true;
    log(INFO, "log", "launch %s build %s commit %s flavour %s", g_launch.c_str(), opt.build.c_str(), opt.commit.c_str(),
        opt.flavour.c_str());
    log(INFO, "log", "app folder %s", opt.app_dir.c_str());
    if (!g_dead.empty()) log(WARN, "log", "the last run (%s) died without exiting: its tail is in %s", dead_launch.c_str(), g_dead.c_str());
    check_host0(true);
    log(INFO, "log", "host0 sink %s (%s)", g_host0 ? "up" : "down", opt.host0_dir.empty() ? "off" : opt.host0_dir.c_str());
    flush();
    g_writer = pspsys::thread_start("blitz_log", writer_main, nullptr, 32 * 1024, 0x17);
}

bool ready() { return g_ready; }
const std::string& launch_id() { return g_launch; }
const std::string& dead_run() { return g_dead; }
const Options& options() { return g_opt; }
double uptime() { return g_t0 ? (double)(pspsys::now_us() - g_t0) / 1e6 : 0.0; }
std::string app_path(const std::string& rel) { return pspsys::join(g_opt.app_dir, rel); }
bool host0_up() { return g_host0; }
void set_flush_ms(int ms) { g_flush_ms = ms > 50 ? ms : 50; }

void vlog(Level level, const char* tag, const char* fmt, va_list ap) {
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);
    char pre[64];
    const char* lv = level == ERROR ? "ERROR " : level == WARN ? "WARN " : "";
    snprintf(pre, sizeof pre, "[%8.3f] [%s] %s", uptime(), tag, lv);
    std::string line = redact(std::string(pre) + msg);
    line += '\n';
    push(line.data(), line.size(), level == ERROR);
}

void log(Level level, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(level, tag, fmt, ap);
    va_end(ap);
}

void raw(const char* text) {
    std::string line = redact(text);
    if (line.empty() || line.back() != '\n') line += '\n';
    push(line.data(), line.size(), false);
}

void flush() {
    if (!g_ready) return;
    pspsys::Lock fl(*g_flush_mtx);
    check_host0(false);
    std::string chunk, host0_chunk;
    {
        pspsys::Lock l(*g_mtx);
        chunk = slice(g_flushed, g_head);
        g_flushed = g_head;
        if (g_host0) { host0_chunk = slice(g_host0_pos, g_head); g_host0_pos = g_head; }
    }
    if (chunk.empty() && host0_chunk.empty()) return;
    g_stats.flushes++;
    if (!g_opt.stdout_immediate && !chunk.empty()) {
        size_t s = 0;
        while (s < chunk.size()) {   // one write per line: PPSSPP prints each write as its own "stdout:" line
            size_t e = chunk.find('\n', s);
            e = e == std::string::npos ? chunk.size() : e + 1;
            write_stdout(chunk.data() + s, e - s);
            s = e;
        }
    }
    if (!chunk.empty()) {
        if (g_ms0_size + (long long)chunk.size() > MS0_CAP) rotate_ms0();
        if (pspsys::append_file(g_blitz_log, chunk.data(), chunk.size())) g_ms0_size += (long long)chunk.size();
        else g_stats.ms0_fail++;
    }
    if (!host0_chunk.empty()) {
        if (pspsys::append_file(g_host0_log, host0_chunk.data(), host0_chunk.size()))
            g_stats.host0_lines += (uint64_t)std::count(host0_chunk.begin(), host0_chunk.end(), '\n');
        else g_host0 = false;   // unplugged: look again in 10 s
    }
}

void shutdown(const char* why) {
    if (!g_ready) return;
    log(INFO, "log", "exit: %s (lines %llu, dropped %llu)", why ? why : "", (unsigned long long)g_stats.lines,
        (unsigned long long)g_stats.dropped);
    g_stop = true;
    if (g_wake) g_wake->signal();
    if (g_writer) pspsys::thread_join(g_writer, 2000);
    g_writer = 0;
    flush();
    pspsys::remove_file(g_marker);
}

void add_secret(const std::string& s) {
    if (s.size() >= 6) g_secrets.push_back(s);
}

std::string redact(const std::string& in) {
    std::string out = in;
    for (const std::string& s : g_secrets) {
        size_t at;
        while ((at = out.find(s)) != std::string::npos) out.replace(at, s.size(), "<KEY>");
    }
    size_t pos = 0;
    while ((pos = out.find("://", pos)) != std::string::npos) {
        size_t host = pos + 3;
        size_t host_end = out.find_first_of("/ \t\r\n\"'?", host);
        if (host_end == std::string::npos) host_end = out.size();
        std::string h = out.substr(host, host_end - host);
        const std::string wd = ".workers.dev";
        if (h.size() > wd.size() && h.compare(h.size() - wd.size(), wd.size(), wd) == 0) {
            size_t dot = h.find('.');
            std::string nh = h.substr(0, dot) + ".<account>.workers.dev";
            out.replace(host, h.size(), nh);
            host_end = host + nh.size();
        }
        if (host_end < out.size() && out[host_end] == '/') {
            size_t seg = host_end + 1;
            size_t seg_end = out.find_first_of("/ \t\r\n\"'?", seg);
            if (seg_end == std::string::npos) seg_end = out.size();
            bool followed = seg_end < out.size() && out[seg_end] == '/';
            if (seg_end > seg && (followed || seg_end - seg >= 16) && out.compare(seg, seg_end - seg, "<KEY>") != 0)
                out.replace(seg, seg_end - seg, "<KEY>");
        }
        pos = host;
    }
    return out;
}

std::string tail(size_t max_bytes) {
    pspsys::Lock l(*g_mtx);
    uint64_t from = g_head > max_bytes ? g_head - max_bytes : 0;
    std::string s = slice(from, g_head);
    if (from > 0 || g_head > RING) {
        size_t nl = s.find('\n');
        if (nl != std::string::npos) s.erase(0, nl + 1);
    }
    return s;
}

std::vector<PendingFile> pending() {
    std::vector<PendingFile> out;
    std::vector<pspsys::DirEntry> list;
    std::string dir = app_path("logs/pending");
    if (!pspsys::list_dir(dir, list)) return out;
    for (auto& e : list) {
        if (e.dir || e.name.size() < 5 || e.name.compare(e.name.size() - 4, 4, ".log") != 0) continue;
        out.push_back({pspsys::join(dir, e.name), e.name, e.size});
    }
    std::sort(out.begin(), out.end(), [](const PendingFile& a, const PendingFile& b) { return a.name < b.name; });
    return out;
}

Stats stats() {
    Stats s = g_stats;
    if (g_mtx) { pspsys::Lock l(*g_mtx); s.ring_used = (int)(g_head - g_flushed); }
    return s;
}

}  // namespace psplog
