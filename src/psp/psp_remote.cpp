// See psp_remote.h.
#include "psp_remote.h"
#include "psp_http.h"
#include "psp_log.h"
#include "psp_net.h"
#include "psp_sha256.h"
#include "psp_sys.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <new>

#ifdef __PSP__
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspthreadman.h>
#endif

namespace pspremote {
namespace {

constexpr size_t MAX_FILE = 1536 * 1024;   // log/get/ls replies: 1.5 MB at most

struct Verb { std::string name; VerbFn fn; void* ctx; bool named; };

Options g_opt;
NetCfg g_cfg;
std::vector<Verb> g_verbs;
std::map<std::string, std::string> g_settings;
void (*g_exit_hook)(void*) = nullptr;
void* g_exit_ctx = nullptr;
void (*g_stat_hook)(std::string&, void*) = nullptr;
void* g_stat_ctx = nullptr;
bool (*g_peek)(uint32_t, uint32_t, std::string&) = nullptr;
pspsys::Mutex* g_mtx = nullptr;
std::string g_worker_launch, g_digits;
long long g_last_seq = -1;
int g_thread = 0;
volatile bool g_stop = false;
Counters g_count = {};
uint64_t g_fast_until = 0;

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') j++;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool safe_name(const std::string& n) {
    if (n.empty() || n.size() > 60) return false;
    for (char c : n) if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    return n[0] != '.';
}

// an app-folder path from a command argument: relative, no "..", no device, and never net.cfg (its keys would go out in a
// result: over plain HTTP to the Worker, or onto the Mac's disk)
bool app_file(const std::string& rel, std::string& out) {
    if (rel.empty() || rel[0] == '/' || rel.find(':') != std::string::npos || rel.find("..") != std::string::npos) return false;
    std::string low;
    for (char c : rel) low += (char)tolower((unsigned char)c);   // FAT: names are case-insensitive
    if (low.find("net.cfg") != std::string::npos) return false;
    out = psplog::app_path(rel);
    return true;
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string base() { return g_cfg.logs_base; }

void note_reply(const psphttp::Response& r) {
    if (!g_worker_launch.empty() || r.date_utc <= 0) return;
    long long boot = r.date_utc - (long long)psplog::uptime();
    g_worker_launch = pspsys::format_utc(boot, true) + "-" + g_digits;
    PLOG("remote", "Worker launch id %s (server Date %s minus uptime %.1f s); local launch id %s; RTC is %lld s %s the server",
         g_worker_launch.c_str(), r.date.c_str(), psplog::uptime(), psplog::launch_id().c_str(),
         llabs(pspsys::wall_clock() - r.date_utc), pspsys::wall_clock() >= r.date_utc ? "ahead of" : "behind");
}

// ---- verbs ----
bool v_ping(const std::vector<std::string>&, std::string& out, void*) {
    out = fmt("pong %s %.1f\n", psplog::launch_id().c_str(), psplog::uptime());
    return true;
}

bool v_stat(const std::vector<std::string>&, std::string& out, void*) {
    psplog::Stats s = psplog::stats();
    out = fmt("launch %s uptime %.1f build %s commit %s flavour %s\n", psplog::launch_id().c_str(), psplog::uptime(),
              psplog::options().build.c_str(), psplog::options().commit.c_str(), psplog::options().flavour.c_str());
    out += fmt("mem free_kb=%d max_kb=%d\n", pspsys::free_kb(), pspsys::max_free_kb());
#ifdef __PSP__
    SceUID ids[64];
    int count = 0;
    if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, 64, &count) >= 0) {
        out += fmt("threads %d\n", count);
        for (int i = 0; i < count && i < 64; i++) {
            SceKernelThreadInfo ti;
            memset(&ti, 0, sizeof ti);
            ti.size = sizeof ti;
            if (sceKernelReferThreadStatus(ids[i], &ti) >= 0)
                out += fmt("  thread %-24s prio 0x%02X status %d stack %d KB\n", ti.name, ti.currentPriority, ti.status, ti.stackSize / 1024);
        }
    }
#endif
    out += fmt("log lines=%llu dropped=%llu flushes=%llu ms0_fail=%llu host0=%s ring_used=%d\n", (unsigned long long)s.lines,
               (unsigned long long)s.dropped, (unsigned long long)s.flushes, (unsigned long long)s.ms0_fail,
               psplog::host0_up() ? "up" : "down", s.ring_used);
    out += fmt("net %s worker %s polls=%d cmds=%d rejected=%d uploads=%d upload_fail=%d host0_cmds=%d\n",
               pspnet::state_name(pspnet::state()), g_cfg.present ? (worker_ready() ? "ready" : "waiting") : "off (no net.cfg)",
               g_count.polls, g_count.cmds, g_count.rejected, g_count.uploads, g_count.upload_fail, g_count.host0_cmds);
    if (g_stat_hook) g_stat_hook(out, g_stat_ctx);
    return true;
}

bool v_log(const std::vector<std::string>& a, std::string& out, void*) {
    std::string path;
    if (!app_file(a.size() > 2 ? a[2] : std::string("logs/blitz.log"), path)) { out = "log: bad file\n"; return false; }
    psplog::flush();
    if (!pspsys::read_file(path, out, MAX_FILE, true)) { out = "log: can't read " + path + "\n"; return false; }
    return true;
}

bool v_ls(const std::vector<std::string>& a, std::string& out, void*) {
    std::string path = psplog::app_path("");
    if (a.size() > 2 && !app_file(a[2], path)) { out = "ls: bad dir\n"; return false; }
    std::vector<pspsys::DirEntry> list;
    if (!pspsys::list_dir(path, list)) { out = "ls: can't open " + path + "\n"; return false; }
    std::sort(list.begin(), list.end(), [](const pspsys::DirEntry& x, const pspsys::DirEntry& y) { return x.name < y.name; });
    out.clear();
    for (auto& e : list) out += e.dir ? fmt("%10s  %s/\n", "-", e.name.c_str()) : fmt("%10lld  %s\n", e.size, e.name.c_str());
    if (out.size() > MAX_FILE) out.resize(MAX_FILE);
    return true;
}

bool v_get(const std::vector<std::string>& a, std::string& out, void*) {
    std::string path;
    if (a.size() < 3 || !app_file(a[2], path)) { out = "get: get <name> <file>\n"; return false; }
    if (!pspsys::read_file(path, out, MAX_FILE, false)) { out = "get: can't read " + path + "\n"; return false; }
    return true;
}

bool v_shot(const std::vector<std::string>& a, std::string& out, void*) {
#ifdef __PSP__
    void* top = nullptr;
    int bw = 0, pf = 0;
    if (sceDisplayGetFrameBuf(&top, &bw, &pf, 0) < 0 || !top || bw <= 0) { out = "shot: no framebuffer\n"; return false; }
    int bpp = pf == 3 ? 4 : 2;
    const uint8_t* src = (const uint8_t*)((uintptr_t)top | 0x40000000u);   // uncached: what the GE wrote
    // "shot <name> half": every other pixel of every other row, a quarter of the memory (65 KB at 16 bits)
    bool half = false;
    for (size_t i = 2; i < a.size(); i++) half = half || a[i] == "half";
    int w = half ? 240 : 480, h = half ? 136 : 272;
    out = fmt("BLITZSHOT1 %d %d %d\n", w, h, pf);
    size_t row = (size_t)w * bpp, at = out.size();
    out.resize(at + row * h);
    for (int y = 0; y < h; y++) {
        const uint8_t* line = src + (size_t)(half ? y * 2 : y) * bw * bpp;
        if (!half) memcpy(&out[at + (size_t)y * row], line, row);
        else for (int x = 0; x < w; x++) memcpy(&out[at + (size_t)y * row + (size_t)x * bpp], line + (size_t)x * 2 * bpp, (size_t)bpp);
    }
    return true;
#else
    out = "shot: no display on the Mac\n";
    return false;
#endif
}

bool v_set(const std::vector<std::string>& a, std::string& out, void*) {
    if (a.size() < 3) { out = "set: set <key> <value>\n"; return false; }
    std::string value;
    for (size_t i = 2; i < a.size(); i++) value += (i > 2 ? " " : "") + a[i];
    g_settings[a[1]] = value;
    PLOG("remote", "set %s = %s (v1 stub: stored, nothing reads it yet)", a[1].c_str(), value.c_str());
    out = "set " + a[1] + "=" + value + " (stub: stored only)\n";
    return true;
}

bool v_peek(const std::vector<std::string>& a, std::string& out, void*) {
    if (a.size() < 4) { out = "peek: peek <name> <addr> <len>\n"; return false; }
    if (!g_peek) { out = "peek: no RDRAM in this build\n"; return false; }
    uint32_t addr = (uint32_t)strtoul(a[2].c_str(), nullptr, 0), len = (uint32_t)strtoul(a[3].c_str(), nullptr, 0);
    if (len > 65536) len = 65536;
    return g_peek(addr, len, out);
}

bool v_exit(const std::vector<std::string>&, std::string& out, void*) {
    out = "bye " + psplog::launch_id() + "\n";
    return true;
}

void default_exit(void*) { go_home("exit command"); }

std::string g_restart_which;
bool g_restart_pending = false;

// restart [classic|modern|probe]: this game again, or another of the kit's, after the reply is sent.
bool v_restart(const std::vector<std::string>& a, std::string& out, void*) {
    std::string which = a.size() > 1 ? a[1] : "";
    if (!which.empty() && which != "classic" && which != "modern" && which != "probe") { out = "usage: restart [classic|modern|probe]\n"; return false; }
    g_restart_which = which;
    g_restart_pending = true;
    out = "restarting " + (which.empty() ? std::string("this game") : which) + " (launch " + psplog::launch_id() + ")\n";
    return true;
}

const Verb* find_verb(const std::string& name) {
    for (auto& v : g_verbs) if (v.name == name) return &v;
    return nullptr;
}

void register_builtins() {
    if (!g_verbs.empty()) return;
    g_verbs.push_back({"ping", v_ping, nullptr, false});
    g_verbs.push_back({"stat", v_stat, nullptr, false});
    g_verbs.push_back({"log", v_log, nullptr, true});
    g_verbs.push_back({"ls", v_ls, nullptr, true});
    g_verbs.push_back({"get", v_get, nullptr, true});
    g_verbs.push_back({"shot", v_shot, nullptr, true});
    g_verbs.push_back({"set", v_set, nullptr, false});
    g_verbs.push_back({"peek", v_peek, nullptr, true});
    g_verbs.push_back({"exit", v_exit, nullptr, false});
    g_verbs.push_back({"restart", v_restart, nullptr, false});
}

// ---- host0 mailbox ----
void host0_reply(const std::string& name, const std::string& body) {
    std::string res = pspsys::join(g_opt.host0_dir, "res");
    pspsys::make_dir(res);
    if (!pspsys::write_file(pspsys::join(res, name), body.data(), body.size()))
        PWARN("remote", "host0: can't write res/%s", name.c_str());
}

bool g_exit_pending = false;

void run_and_reply(const std::string& line, bool worker) {
    std::string name, body;
    bool ok = true;
    execute(line, name, body, ok);
    if (!ok) name = "error";
    try {
        if (worker) worker_put_res(name, body);
        else host0_reply(name, body);
    } catch (...) {   // the reply itself ran out of memory: say so in a small one, and carry on polling
        std::string small = "reply to '" + line.substr(0, 60) + "' failed (out of memory?)\n";
        if (worker) worker_put_res("error", small);
        else host0_reply("error", small);
    }
    PLOG("remote", "%s: %s -> res/%s%s (%u bytes)%s", worker ? "worker" : "host0", line.c_str(), worker ? "psp-" : "", name.c_str(),
         (unsigned)body.size(), ok ? "" : " FAILED");
    std::vector<std::string> a = split(line);
    if (ok && !a.empty() && a[0] == "exit") g_exit_pending = true;
}

void poll_host0() {
    std::string cmd = pspsys::join(g_opt.host0_dir, "cmd");
    std::vector<pspsys::DirEntry> list;
    if (!pspsys::list_dir(cmd, list)) { pspsys::make_dir(cmd); return; }
    const pspsys::DirEntry* best = nullptr;
    unsigned long long best_seq = 0;
    for (auto& e : list) {
        if (e.dir || e.name.size() < 5 || e.name.compare(e.name.size() - 4, 4, ".txt") != 0) continue;
        char* end = nullptr;
        unsigned long long seq = strtoull(e.name.c_str(), &end, 10);
        if (!end || strcmp(end, ".txt") != 0) continue;
        if (!best || seq < best_seq) { best = &e; best_seq = seq; }
    }
    if (!best) return;
    std::string path = pspsys::join(cmd, best->name), text;
    bool read = pspsys::read_file(path, text, 64 * 1024);
    pspsys::remove_file(path);
    if (!read) return;
    size_t s = 0;
    while (s < text.size()) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(s, e - s));
        s = e + 1;
        if (line.empty() || line[0] == '#') continue;
        g_count.host0_cmds++;
        run_and_reply(line, false);
    }
}

void write_seen() {
    std::string s = fmt("launch=%s uptime=%.1f net=%s pid=%s\n", psplog::launch_id().c_str(), psplog::uptime(),
                        pspnet::state_name(pspnet::state()), pid().c_str());
    pspsys::write_file_direct(pspsys::join(g_opt.host0_dir, "seen"), s.data(), s.size());
}

// ---- Worker ----
std::string seq_path() { return psplog::app_path("data/cmd_seq"); }

void load_seq() {
    std::string s;
    if (pspsys::read_file(seq_path(), s, 64)) g_last_seq = atoll(s.c_str());
}

void save_seq() {
    std::string s = std::to_string(g_last_seq) + "\n";
    pspsys::write_file(seq_path(), s.data(), s.size());
}

// "<seq> <verb args> #<tag>" -> verb args, or "" (and why) when it doesn't check out
std::string verify(const std::string& raw, std::string& why) {
    std::string line = trim(raw);
    size_t hash = line.rfind(" #");
    size_t sp = line.find(' ');
    if (hash == std::string::npos || sp == std::string::npos || sp >= hash) { why = "unsigned"; return ""; }
    std::string seq = line.substr(0, sp), body = trim(line.substr(sp + 1, hash - sp - 1)), tag = line.substr(hash + 2);
    if (seq.size() > 18) { why = "bad seq"; return ""; }   // atoll must not overflow (a huge seq would lock out every later one)
    for (char c : seq) if (c < '0' || c > '9') { why = "bad seq"; return ""; }
    if (g_cfg.cmd_key.empty()) { why = "no cmd_key in net.cfg"; return ""; }
    if (pspsha::command_tag(g_cfg.cmd_key, seq, body) != tag) { why = "bad tag"; return ""; }
    long long n = atoll(seq.c_str());
    pspsys::Lock l(*g_mtx);
    if (n <= g_last_seq) { why = "replayed seq " + seq + " (last " + std::to_string(g_last_seq) + ")"; return ""; }
    g_last_seq = n;
    save_seq();
    return body;
}

void poll_thread(void*) {
    uint64_t last_seen = 0, next_worker = 0, last_host0_try = 0;
    double next_run_upload = g_opt.run_upload_first_s;
    bool host0 = false, pending_done = false;
    while (!g_stop) {
        uint64_t now = pspsys::now_us();
        if (!g_opt.host0_dir.empty()) {
            if (host0 || now - last_host0_try > 10 * 1000000ull) {
                last_host0_try = now;
                bool was = host0;
                host0 = pspsys::is_dir(g_opt.host0_dir);
                if (host0 && !was) PLOG("remote", "host0 mailbox up: %s/cmd", g_opt.host0_dir.c_str());
                if (!host0 && was) PWARN("remote", "host0 mailbox gone");
            }
            if (host0) {
                poll_host0();
                if (now - last_seen > 5 * 1000000ull) { write_seen(); last_seen = now; }
            }
        }
        if (g_opt.worker && worker_ready()) {
            if (!pending_done) { pending_done = true; upload_pending(); }
            if (g_opt.run_upload_first_s > 0 && psplog::uptime() >= next_run_upload) {
                worker_upload_run();
                next_run_upload = psplog::uptime() + (g_opt.run_upload_every_s > 0 ? g_opt.run_upload_every_s : 300);
            }
            if (now >= next_worker) {
                int n = worker_poll();
                if (n > 0) g_fast_until = pspsys::now_us() + 120 * 1000000ull;
                next_worker = pspsys::now_us() + (pspsys::now_us() < g_fast_until ? 2 : 10) * 1000000ull;
            }
        }
        if (g_restart_pending) {
            g_restart_pending = false;
            pspsys::sleep_ms(300);   // let the reply land first
            relaunch(g_restart_which);
            default_exit(nullptr);   // the load failed: leave cleanly rather than run on with the log shut
        }
        if (g_exit_pending) {
            g_exit_pending = false;
            pspsys::sleep_ms(300);   // let the reply land first
            if (g_exit_hook) g_exit_hook(g_exit_ctx);
            else default_exit(nullptr);
        }
        pspsys::sleep_ms(host0 ? 500 : 1000);
    }
}

}  // namespace

bool load_net_cfg(const std::string& path, NetCfg& out) {
    out = NetCfg();
    std::string text;
    if (!pspsys::read_file(path, text, 16 * 1024)) return false;
    out.present = true;
    size_t s = 0;
    while (s < text.size()) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(s, e - s));
        s = e + 1;
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "logs_base") out.logs_base = v;
        else if (k == "hub_base") out.hub_base = v;
        else if (k == "cmd_key") out.cmd_key = v;
        else if (k == "device") out.device = v;
        else if (k == "wifi_profile") out.wifi_profile = atoi(v.c_str());
        else if (k == "home") out.home = v;
    }
    while (!out.logs_base.empty() && out.logs_base.back() == '/') out.logs_base.pop_back();
    while (!out.hub_base.empty() && out.hub_base.back() == '/') out.hub_base.pop_back();
    // the keys: the last path segment of each base, and the command key
    for (const std::string* b : {&out.logs_base, &out.hub_base}) {
        size_t slash = b->find_last_of('/');
        if (slash != std::string::npos && slash + 1 < b->size() && b->find("://") + 2 < slash) psplog::add_secret(b->substr(slash + 1));
    }
    psplog::add_secret(out.cmd_key);
    return true;
}

const NetCfg& net_cfg() { return g_cfg; }

void init(const Options& opt) {
    g_opt = opt;
    if (!g_mtx) g_mtx = new pspsys::Mutex();
    register_builtins();
    const std::string& id = psplog::launch_id();
    size_t dash = id.rfind('-');
    g_digits = dash == std::string::npos ? "0" : id.substr(dash + 1);
    std::string cfg = psplog::app_path("data/net.cfg");
    if (load_net_cfg(cfg, g_cfg))
        PLOG("remote", "net.cfg present: logs %s, hub %s, cmd_key %s, device %s, wifi profile %d, home %s",
             g_cfg.logs_base.empty() ? "none" : "set", g_cfg.hub_base.empty() ? "none" : "set", g_cfg.cmd_key.empty() ? "none" : "set",
             g_cfg.device.c_str(), g_cfg.wifi_profile, g_cfg.home.empty() ? "the XMB" : g_cfg.home.c_str());
    else
        PLOG("remote", "no net.cfg: the Worker channel is off (host0 and ms0 only)");
    load_seq();
    if (!opt.host0_dir.empty()) {
        pspsys::make_dir(pspsys::join(opt.host0_dir, "cmd"));
        pspsys::make_dir(pspsys::join(opt.host0_dir, "res"));
    }
    // <app>/cmd.txt: a recipe of set lines, read once
    std::string recipe;
    std::string rpath = psplog::app_path("cmd.txt");
    if (pspsys::read_file(rpath, recipe, 16 * 1024)) {
        size_t s = 0;
        while (s < recipe.size()) {
            size_t e = recipe.find('\n', s);
            if (e == std::string::npos) e = recipe.size();
            std::string line = trim(recipe.substr(s, e - s));
            s = e + 1;
            if (line.compare(0, 4, "set ") != 0) { if (!line.empty() && line[0] != '#') PWARN("remote", "cmd.txt: only set lines, skipped: %s", line.c_str()); continue; }
            std::string n, b;
            bool ok;
            execute(line, n, b, ok);
        }
        pspsys::rename_file(rpath, psplog::app_path("cmd.done"));
    }
}

void start() {
    if (g_thread) return;
    g_stop = false;
    g_thread = pspsys::thread_start("blitz_remote", poll_thread, nullptr, 64 * 1024, 0x17);
    if (!g_thread) PERR("remote", "remote thread failed to start");
}

void stop() {
    g_stop = true;
    if (g_thread) pspsys::thread_join(g_thread, 3000);
    g_thread = 0;
}

void add_verb(const char* name, VerbFn fn, void* ctx, bool named) {
    register_builtins();
    for (auto& v : g_verbs) if (v.name == name) { v.fn = fn; v.ctx = ctx; v.named = named; return; }
    g_verbs.push_back({name, fn, ctx, named});
}
void set_exit_hook(void (*fn)(void*), void* ctx) { g_exit_hook = fn; g_exit_ctx = ctx; }
void set_stat_hook(void (*fn)(std::string&, void*), void* ctx) { g_stat_hook = fn; g_stat_ctx = ctx; }
void set_peek(bool (*fn)(uint32_t, uint32_t, std::string&)) { g_peek = fn; }
std::string setting(const std::string& key) { auto it = g_settings.find(key); return it == g_settings.end() ? "" : it->second; }

void execute(const std::string& line, std::string& res_name, std::string& body, bool& ok) {
    register_builtins();
    std::vector<std::string> a = split(line);
    ok = false;
    res_name = "error";
    if (a.empty()) { body = "empty command\n"; return; }
    const Verb* v = find_verb(a[0]);
    if (!v) { body = "unknown verb: " + a[0] + "\n"; return; }
    if (v->named) {
        if (a.size() < 2 || !safe_name(a[1])) { body = a[0] + ": the first argument is the result's name ([A-Za-z0-9._-])\n"; return; }
        res_name = a[0] == "shot" ? "shot-" + a[1] : a[1];
    } else {
        res_name = a[0];
    }
    // A command must never end the remote thread: a screenshot's 261 KB failing to allocate in a heavy moment threw
    // bad_alloc and the PSP stopped answering (2026-11-04, Classic's main menu).
    try {
        ok = v->fn(a, body, v->ctx);
    } catch (const std::bad_alloc&) {
        body = a[0] + ": out of memory\n";
        ok = false;
    } catch (...) {
        body = a[0] + ": failed (exception)\n";
        ok = false;
    }
}

bool worker_ready() { return g_cfg.present && !g_cfg.logs_base.empty() && pspnet::up(); }

bool worker_upload(const char* kind, const std::string& body, std::string* reply_id, bool with_launch) {
    if (!worker_ready()) return false;
    std::string url = base() + "/up?device=" + g_cfg.device + "&kind=" + kind;
    if (with_launch) {
        if (g_worker_launch.empty()) {   // the launch id comes from the server's clock: ask once first
            psphttp::Response r0;
            psphttp::get(base() + "/res/psp-clock", r0, 10000);
            note_reply(r0);
        }
        url += "&launch=" + (g_worker_launch.empty() ? psplog::launch_id() : g_worker_launch);
    }
    psphttp::Response r;
    bool ok = psphttp::post(url, body, r, 15000) && r.status == 200;
    note_reply(r);
    if (ok) {
        g_count.uploads++;
        if (reply_id) *reply_id = trim(r.body);
        PLOG("remote", "upload kind=%s %u bytes -> %s (%d ms)", kind, (unsigned)body.size(), trim(r.body).c_str(), r.ms);
    } else {
        g_count.upload_fail++;
        PWARN("remote", "upload kind=%s failed: status %d %s", kind, r.status, r.error.c_str());
    }
    return ok;
}

bool worker_upload_run() {
    std::string tail;
    psplog::flush();
    pspsys::read_file(psplog::app_path("logs/blitz.log"), tail, 256 * 1024, true);
    const psplog::Options& o = psplog::options();
    std::string head = "[" + pspsys::format_utc(pspsys::wall_clock(), false) + "] build " + o.build + " commit " + o.commit +
                       " flavour " + o.flavour + " launch " + psplog::launch_id() + ", game started\n";
    return worker_upload("run", head + tail, nullptr, true);
}

bool worker_put_res(const std::string& name, const std::string& body) {
    if (!worker_ready()) return false;
    psphttp::Response r;
    bool ok = psphttp::post(base() + "/res/psp-" + name, body, r, 15000) && r.status == 200;
    note_reply(r);
    if (!ok) PWARN("remote", "res/psp-%s failed: status %d %s", name.c_str(), r.status, r.error.c_str());
    return ok;
}

int worker_poll() {
    if (!worker_ready()) return -1;
    psphttp::Response r;
    g_count.polls++;
    if (!psphttp::get(base() + "/cmd?pid=" + pid(), r, 10000) || r.status != 200) {
        if (g_count.polls < 5 || g_count.polls % 30 == 0) PWARN("remote", "cmd poll failed: status %d %s", r.status, r.error.c_str());
        return -1;
    }
    note_reply(r);
    int n = 0;
    size_t s = 0;
    while (s < r.body.size()) {
        size_t e = r.body.find('\n', s);
        if (e == std::string::npos) e = r.body.size();
        std::string line = trim(r.body.substr(s, e - s));
        s = e + 1;
        if (line.empty()) continue;
        std::string why;
        std::string cmd = verify(line, why);
        if (cmd.empty()) {
            g_count.rejected++;
            PWARN("remote", "worker: rejected a command line (%s)", why.c_str());
            continue;
        }
        g_count.cmds++;
        n++;
        run_and_reply(cmd, true);
    }
    return n;
}

int upload_pending() {
    int n = 0;
    for (const psplog::PendingFile& f : psplog::pending()) {
        std::string body;
        if (!pspsys::read_file(f.path, body, 4 * 1024 * 1024 - 1024)) continue;
        if (worker_upload("crash", body, nullptr, false)) { pspsys::remove_file(f.path); n++; PLOG("remote", "pending %s uploaded and removed", f.name.c_str()); }
        else break;
    }
    return n;
}

const std::string& worker_launch() { return g_worker_launch; }
std::string pid() { return "psp-" + g_digits; }
Counters counters() { return g_count; }

}  // namespace pspremote

#ifdef __PSP__
extern "C" int nbr_launch_eboot(const char* path);   // src/psp/nbremote/nbr_launch_psp.c: ARK's launcher call, then sceKernelLoadExec
#endif

namespace pspremote {
namespace {
// <the folder the games sit in>/<folder>/EBOOT.PBP
std::string kit_eboot(const char* folder) {
    std::string app = psplog::app_path("");
    while (!app.empty() && app.back() == '/') app.pop_back();
    size_t cut = app.rfind('/');
    return (cut == std::string::npos ? app : app.substr(0, cut)) + "/" + folder + "/EBOOT.PBP";
}
}  // namespace

bool relaunch(const std::string& which) {
    std::string path = psplog::app_path("EBOOT.PBP");
    if (!which.empty()) path = kit_eboot(which == "classic" ? "NBCLASSIC" : which == "modern" ? "NEWBLITZ" : "NBPROBE");
    PLOG("remote", "relaunching %s", path.c_str());
    psplog::shutdown("relaunch");
#ifdef __PSP__
    int rc = nbr_launch_eboot(path.c_str());
    printf("[remote] launching %s failed: 0x%08X\n", path.c_str(), (unsigned)rc);
#endif
    return false;
}

void go_home(const char* why, bool clean) {
#ifdef __PSP__
    if (g_cfg.home == "remote") {
        std::string path = kit_eboot("NBREMOTE");
        if (pspsys::exists(path)) {
            PLOG("remote", "%s: home is NB Remote (%s)", why, path.c_str());
            if (clean) psplog::shutdown(why);
            else psplog::flush();
            int rc = nbr_launch_eboot(path.c_str());
            printf("[remote] launching %s failed: 0x%08X; the XMB instead\n", path.c_str(), (unsigned)rc);
            sceKernelExitGame();
            for (;;) sceKernelSleepThread();
        }
        PWARN("remote", "home=remote, but there is no %s: the XMB", path.c_str());
    }
    if (clean) psplog::shutdown(why);
    else psplog::flush();
    sceKernelExitGame();
    for (;;) sceKernelSleepThread();
#else
    if (clean) psplog::shutdown(why);
    else psplog::flush();
    exit(0);
#endif
}
}  // namespace pspremote
