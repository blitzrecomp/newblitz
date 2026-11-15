// The PSP's log (docs/PSP_PORT.md §5.1): one 64 KB RAM ring, a writer thread, and the sinks that are up:
//   stdout      always (PPSSPP prints it as "stdout:" lines; PSPLink's pspsh shows it)
//   host0:      <host0 dir>/run/<launch>.log, appended live, once <host0 dir> opens (checked at boot and every 10 s)
//   ms0:        <app>/logs/blitz.log, open-append-close per flush, 1 MB cap, blitz.1.log rotated at launch and at the cap
//   Worker      not here: psp_remote uploads pending/ files and tails through the hooks below
// A caller never blocks on a sink: a full ring drops the line and counts it. Lines look like "[  12.345] [tag] message".
// Dead runs: <app>/logs/running holds the launch id, build and flavour while the app runs. If it is there at launch, the last
// 256 KB of blitz.log goes to <app>/logs/pending/dead-<that launch>.log (5 kept) before the log rotates.
// Secrets: every line passes redact(): registered secrets become <KEY>, a URL's first path segment (the key) becomes <KEY>, and
// a workers.dev host becomes <worker>.<account>.workers.dev.
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace psplog {

enum Level { INFO = 0, WARN = 1, ERROR = 2 };

struct Options {
    std::string app_dir;           // "ms0:/PSP/GAME/NBPROBE" (the EBOOT's own folder) or a Mac folder
    std::string host0_dir;         // "host0:/blitz-psp", or a Mac folder; empty: no host0 sink
    std::string build = "0", commit = "unknown", flavour = "probe";
    bool stdout_immediate = false; // write stdout from the caller (the probe: nothing lost if it dies mid-section)
    int flush_ms = 1000;           // 1 s in menus, 5 s in play (set_flush_ms)
    int stdout_fd = -1;            // Mac only: the stdout sink writes to this fd instead (psp-host captures fds 1 and 2)
};

void init(const Options& opt);
bool ready();
const std::string& launch_id();          // YYYY-MM-DDTHH-MM-SSZ-<1-7 digits>, from the boot RTC
const std::string& dead_run();           // pending/dead-*.log written at this launch, or ""
double uptime();                          // seconds since init
const Options& options();

void log(Level level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void vlog(Level level, const char* tag, const char* fmt, va_list ap);
void raw(const char* line);              // one line exactly as given (PROBE lines), no prefix

void flush();                            // drain the ring to every sink now (the writer does this on its own)
void set_flush_ms(int ms);
void shutdown(const char* why);          // last line, flush, stop the writer, delete the running marker

void add_secret(const std::string& secret);
std::string redact(const std::string& line);

std::string tail(size_t max_bytes);      // the newest whole lines still in the ring
std::string app_path(const std::string& rel);
bool host0_up();

struct PendingFile { std::string path, name; long long size; };
std::vector<PendingFile> pending();      // <app>/logs/pending/*.log, oldest first

struct Stats { uint64_t lines, dropped, bytes, flushes, ms0_fail, host0_lines; int ring_used; };
Stats stats();

}  // namespace psplog

#define PLOG(tag, ...)  ::psplog::log(::psplog::INFO, tag, __VA_ARGS__)
#define PWARN(tag, ...) ::psplog::log(::psplog::WARN, tag, __VA_ARGS__)
#define PERR(tag, ...)  ::psplog::log(::psplog::ERROR, tag, __VA_ARGS__)
