// The v0 screen (docs/PSP_PORT.md §4.5): pspDebugScreen text, redrawn twice a second by its own low-priority thread:
// build, flavour, VI count and rate, tasks per second, memory, threads, ROM cache, net, then the newest log lines.
// The `shot` command returns this screen (sceDisplayGetFrameBuf: 480 x 272, 8888).
// With the GE renderer (M7, psp_gfx.cpp) this thread stops drawing once the renderer is up (overlay_use_ge): it still
// builds the status block (for `stat` and the HANG block) and the newest log lines, and the renderer draws the overlay
// mode's part of them into its frames: off, hud (a counter line), log (that and the log lines). Mode: <app>/overlay,
// `set overlay`, SELECT + triangle.
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>

#include "psp_log.h"
#include "psp_net.h"
#include "psp_remote.h"
#include "psp_sys.h"
#include "host/host_backends.h"

namespace {
    constexpr int cols = 68, rows = 34, status_rows = 10;
    std::string g_title;
    pspsys::Mutex* g_status_mutex = nullptr;
    std::string g_status;
    pspsys::Mutex* g_draw_mutex = nullptr;   // held while this thread draws; overlay_use_ge takes it to stop the drawing
    bool g_ge = false;
    std::atomic<int> g_mode{host::OVERLAY_HUD};
    std::vector<host::OverlayLine> g_log_lines;   // the newest (under g_status_mutex)

    struct Rates {
        uint64_t t_us = 0, vi = 0, dl = 0, audio = 0, frames = 0, shown = 0;
    };

    std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
    std::string fmt(const char* f, ...) {
        char buf[256];
        va_list ap;
        va_start(ap, f);
        vsnprintf(buf, sizeof buf, f, ap);
        va_end(ap);
        return buf;
    }

    std::string build_status(Rates& last) {
        uint64_t now = pspsys::now_us();
        uint64_t vi = host::vi_count();
        host::GfxStats g = host::gfx_stats();
        uint64_t audio = host::audio_tasks();
        double dt = last.t_us ? (now - last.t_us) / 1e6 : 0;
        auto rate = [&](uint64_t a, uint64_t b) { return dt > 0 ? (a - b) / dt : 0.0; };
        double vis = rate(vi, last.vi), dls = rate(g.display_lists, last.dl), aus = rate(audio, last.audio), frs = rate(g.frames, last.frames);
        double shs = rate(g.shown, last.shown);
        last = { now, vi, g.display_lists, audio, g.frames, g.shown };
        host::MemStats m = host::mem_stats();
        host::PoolStats p = host::pool_stats();
        host::ThreadCounts c = host::thread_counts();
        host::RomStats r = host::rom_stats();
        double hit = r.hits + r.misses ? 100.0 * r.hits / (double)(r.hits + r.misses) : 100.0;
        std::string s;
        s += g_title + "\n";
        s += fmt("launch %s  up %.0f s\n", psplog::launch_id().c_str(), psplog::uptime());
        s += fmt("VI %llu  %.1f/s   tasks/s: gfx %.1f audio %.1f  frames %.1f/s  shown %.1f/s (%s, present %s)\n",
                 (unsigned long long)vi, vis, dls, aus, frs, shs, host::gfx_mode_ge() ? "GE" : "null", host::present_mode_name(host::present_mode()));
        s += fmt("free %d KB (largest %d)  heap %d KB used of %d  guards %s\n", m.free_kb, m.max_kb, m.heap_used_kb, m.heap_kb,
                 m.guards_ok ? "ok" : "BAD");
        s += fmt("threads %d alive (most %d)  pool %d/%d busy (most %d)  fallback %d\n", c.alive, c.max_alive, p.busy, p.slots,
                 p.max_busy, p.fallbacks);
        s += fmt("pool stack high-water %u of %u bytes\n", (unsigned)p.worst_used, (unsigned)p.slot_bytes);
        s += fmt("ROM cache hit %.1f%%  misses %llu  worst %.1f ms\n", hit, (unsigned long long)r.misses, r.worst_ms);
        pspremote::Counters rc = pspremote::counters();
        s += fmt("net %s  host0 %s  cmds %d+%d  uploads %d\n", pspnet::state_name(pspnet::state()), psplog::host0_up() ? "up" : "down",
                 rc.host0_cmds, rc.cmds, rc.uploads);
        return s;
    }

    void draw_line(int y, const std::string& text, uint32_t color) {
        char line[cols + 1];
        size_t n = text.size() < (size_t)cols ? text.size() : (size_t)cols;
        memcpy(line, text.data(), n);
        for (size_t i = n; i < (size_t)cols; i++) line[i] = ' ';
        line[cols] = 0;
        pspDebugScreenSetXY(0, y);
        pspDebugScreenSetTextColor(color);
        pspDebugScreenPuts(line);
    }

    // The newest log lines that fit, without their uptime prefix, coloured by level.
    std::vector<host::OverlayLine> log_lines(size_t max) {
        std::string tail = psplog::tail(4096);
        std::vector<host::OverlayLine> lines;
        size_t a = 0;
        while (a < tail.size()) {
            size_t e = tail.find('\n', a);
            if (e == std::string::npos) e = tail.size();
            std::string l = tail.substr(a, e - a);
            size_t close = l.find(']');
            if (!l.empty() && l[0] == '[' && close != std::string::npos && close < 16) l.erase(0, close + 2);
            uint32_t color = l.find("ERROR") != std::string::npos ? 0xFF4040FFu : l.find("WARN") != std::string::npos ? 0xFF40FFFFu : 0xFFC0C0C0u;
            lines.push_back({ l, color });
            a = e + 1;
        }
        if (lines.size() > max) lines.erase(lines.begin(), lines.end() - (ptrdiff_t)max);
        return lines;
    }

    void overlay_thread(void*) {
        Rates last;
        for (;;) {
            std::string status = build_status(last);
            std::vector<host::OverlayLine> lines = log_lines(rows);
            {
                pspsys::Lock l(*g_status_mutex);
                g_status = status;
                g_log_lines = lines;
            }
            g_draw_mutex->lock();
            if (!g_ge) {
                int y = 0;
                size_t s = 0;
                while (s < status.size() && y < status_rows) {
                    size_t e = status.find('\n', s);
                    if (e == std::string::npos) e = status.size();
                    uint32_t color = y == 0 ? 0xFF00FFFFu : 0xFFFFFFFFu;   // the title line in yellow
                    draw_line(y++, status.substr(s, e - s), color);
                    s = e + 1;
                }
                while (y < status_rows) draw_line(y++, "", 0xFFFFFFFFu);
                draw_line(y++, std::string(cols, '-'), 0xFF808080u);
                int room = rows - y;
                size_t first = lines.size() > (size_t)room ? lines.size() - room : 0;
                for (size_t i = first; i < lines.size(); i++) draw_line(y++, lines[i].text, lines[i].color);
                while (y < rows) draw_line(y++, "", 0xFFFFFFFFu);
            }
            g_draw_mutex->unlock();
            pspsys::sleep_ms(500);
        }
    }
}

namespace host {
    void start_overlay(const std::string& title) {
        g_title = title;
        g_status_mutex = new pspsys::Mutex();
        g_draw_mutex = new pspsys::Mutex();
        std::string text;
        if (pspsys::read_file(psplog::app_path("overlay"), text, 64)) {
            int m = overlay_mode_from(text);
            if (m >= 0) g_mode = m;
            PLOG("overlay", "overlay switch: %s", overlay_mode_name(g_mode.load()));
        }
        pspDebugScreenInit();
        pspDebugScreenClearLineDisable();
        if (!pspsys::thread_start("blitz_overlay", overlay_thread, nullptr, 32 * 1024, 0x3B)) PERR("overlay", "overlay thread failed to start");
    }

    void overlay_use_ge(bool on) {
        if (g_draw_mutex == nullptr) { g_ge = on; return; }
        pspsys::Lock l(*g_draw_mutex);   // waits for a redraw in progress
        g_ge = on;
    }

    int overlay_mode() { return g_mode.load(); }
    void set_overlay_mode(int m) { if (m >= OVERLAY_OFF && m <= OVERLAY_LOG) g_mode = m; }
    const char* overlay_mode_name(int m) { return m == OVERLAY_OFF ? "off" : m == OVERLAY_LOG ? "log" : "hud"; }
    int overlay_mode_from(const std::string& text) {
        if (text.compare(0, 3, "off") == 0) return OVERLAY_OFF;
        if (text.compare(0, 3, "log") == 0) return OVERLAY_LOG;
        if (text.empty() || text[0] == '\n' || text.compare(0, 3, "hud") == 0) return OVERLAY_HUD;
        return -1;
    }

    std::vector<OverlayLine> overlay_log_lines(int max) {
        if (g_status_mutex == nullptr) return {};
        pspsys::Lock l(*g_status_mutex);
        if ((int)g_log_lines.size() <= max) return g_log_lines;
        return std::vector<OverlayLine>(g_log_lines.end() - max, g_log_lines.end());
    }

    std::string overlay_status() {
        if (g_status_mutex == nullptr) return "";
        pspsys::Lock l(*g_status_mutex);
        return g_status;
    }
}
