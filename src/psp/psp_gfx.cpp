// The PSP's renderer in the game (docs/PSP_PORT.md §4.5, M7): ultramodern's RendererContext over src/psp/gfx (sceGu).
//
//   send_dl        (gfx thread, after ultramodern's sp_complete) walks the task's display list into the open GE list:
//                  begin_frame on the first list of a frame, then submit_dl. ultramodern signals DP done when it returns;
//                  the GE may still be drawing, which is safe because every vertex and texel it reads is a renderer-owned
//                  copy (plan v1.1). A skipped list (presentation skip, below) is not walked at all.
//   update_screen  (every VI) when a frame is open and the VI origin has changed (the game swapped to the buffer it drew),
//                  or 3 VIs have passed without a swap: end_frame (finish the GE list, wait for it), the overlay, present.
//                  send_dl presents an open frame first when the game has called osViSwapBuffer since that frame's lists
//                  (the next framebuffer changed): the frame is complete even if the VI that shows it hasn't come yet.
//
// Presentation skip (plan 4.5 v1.1), setting "present" (switch file <app>/present, `set present`, SELECT + circle):
//   30    every display list is drawn
//   auto  a list is not drawn when the last tick was late (> 35 ms since the one before: drawing this one would make the
//         next tick late too); never two in a row, so at least 1 frame in 2 is shown (default)
//   20    every third list is skipped (20 shown/s at 30 ticks/s)
//
// Vsync (switch file <app>/vsync; gfx/ge_psp.cpp, Config::vsync): the swap happens at the next vblank, and begin_frame
// waits for that vblank when a list follows the swap within one (the GE must not clear the buffer still on screen); [perf]
// vsync_waits/vsync_ms. Off by default: pspgu's swap at the next hsync, so on hardware every present tears wherever the
// scan-out is (PPSSPP shows whole frames). In PPSSPP the wait came on most frames (the swap is usually found by the next
// send_dl) and cost shown/s 24-30 -> 17-25 in auto; triple buffering (256 KB of texture VRAM) would avoid the wait.
//
// The overlay (<app>/overlay "off|hud|log", `set overlay`, SELECT + triangle) is drawn into the frame the GE just finished,
// with pspDebugScreen in 5650 mode: hud = one line of counters at the top; log = that plus the newest log lines at the
// bottom. The switch file <app>/nogfx keeps the M5 null renderer and its full debug screen (the A/B).
//
// [perf] (every 5 s, psp_main.cpp) gets its renderer half from gfx_perf_fields(): ticks/s (display lists), shown/s, skip,
// game_ms p50/p95 (per tick: the time between two lists minus the renderer's CPU time and the kernel's idle time),
// gfx_cpu_ms (walk per drawn list), ge_ms (end_frame's wait for the GE), tex_vram_kb/tex_ram_kb, unk_cc.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"

#include "gfx/gu_context.h"
#include "psp_log.h"
#include "psp_sys.h"
#include "host/host_backends.h"

namespace {
    std::atomic<int> g_present{host::PRESENT_AUTO};

    // The perf window: written by the gfx thread, read (and cleared) by the watchdog every 5 s.
    struct Window {
        uint64_t ticks = 0, shown = 0, skipped = 0;
        uint64_t walk_us = 0, ge_us = 0, other_us = 0;   // sums over shown frames
        uint64_t vsync_waits = 0, vsync_us = 0;          // begin_frame's waits for the vblank after a swap
        uint32_t frames = 0;
        uint64_t tex_uploads = 0, tex_evictions = 0, tex_resets = 0, tex_kb = 0;
        std::vector<float> game_ms;                     // per tick
        uint64_t idle_us = 0;
    };
    pspsys::Mutex* g_win_mutex = nullptr;
    Window g_win;
    std::atomic<uint32_t> g_tex_vram{0}, g_tex_ram{0}, g_unk_cc{0}, g_cc_keys{0};
    bool g_idle_ok = false;

    // The HUD's counters at its last refresh (once a second, on the gfx thread).
    struct Hud {
        uint64_t t_us = 0, vi = 0, ticks = 0, shown = 0;
        uint64_t walk_us = 0, ge_us = 0;
        uint32_t frames = 0;
    } g_hud;

    uint64_t idle_clock_us() {
        SceKernelSystemStatus st;
        memset(&st, 0, sizeof st);
        st.size = sizeof st;
        if (sceKernelReferSystemStatus(&st) < 0) return 0;
        return ((uint64_t)st.idleClocks.hi << 32) | st.idleClocks.low;
    }

    void gfx_log(const char* line) { PLOG("gfx", "%s", line); }

    // The HANG block's breadcrumb (host::gfx_breadcrumb): which step the Gfx thread is in and since when.
    volatile int g_stage = 0;
    volatile uint64_t g_stage_us = 0;
    void stage(int s) { g_stage = s; g_stage_us = pspsys::now_us(); }

    class GuRenderer final : public ultramodern::renderer::RendererContext {
    public:
        explicit GuRenderer(uint8_t* rdram) : rdram_(rdram) {
            setup_result = ultramodern::renderer::SetupResult::Success;
            chosen_api = ultramodern::renderer::GraphicsApi::Auto;
            blitzgfx::Config cfg;
            cfg.log = gfx_log;
            if (pspsys::exists(psplog::app_path("cull_ccw"))) cfg.cull_ccw = true;
            if (pspsys::exists(psplog::app_path("noclip"))) cfg.near_clip = false;
            if (pspsys::exists(psplog::app_path("vsync"))) cfg.vsync = true;   // swap at the vblank (see the top of the file)
            ok_ = blitzgfx::init(cfg);
            last_idle_ = idle_clock_us();
            g_idle_ok = last_idle_ != 0;
            PLOG("gfx", "GE renderer (%s): %s; list %u KB, near_clip=%d cull_ccw=%d vsync=%d, present=%s, overlay=%s, idle clock %s",
                 blitzgfx::backend_name(), ok_ ? "up" : "INIT FAILED", cfg.list_bytes / 1024, cfg.near_clip, cfg.cull_ccw, cfg.vsync,
                 host::present_mode_name(g_present.load()), host::overlay_mode_name(host::overlay_mode()),
                 g_idle_ok ? "yes" : "unavailable (game_ms includes idle time)");
        }
        bool valid() override { return ok_; }
        bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
        void enable_instant_present() override {}
        void send_dummy_workload(uint32_t) override { host::gfx_count(0, 0, 0, 0, 1); }
        void shutdown() override { blitzgfx::shutdown(); }
        uint32_t get_display_framerate() const override { return 60; }
        float get_resolution_scale() const override { return 1.0f; }

        void send_dl(const OSTask* task) override {
            uint64_t now = pspsys::now_us();
            uint64_t idle = g_idle_ok ? idle_clock_us() : 0;
            float game_ms = -1;
            uint64_t interval = last_tick_ ? now - last_tick_ : 0;
            if (last_tick_) {
                int64_t g = (int64_t)interval - (int64_t)busy_us_ - (int64_t)(idle - last_idle_);
                game_ms = g > 0 ? g / 1000.0f : 0.0f;
            }
            uint64_t idle_delta = idle - last_idle_;
            last_tick_ = now;
            last_idle_ = idle;
            busy_us_ = 0;
            tick_++;

            bool skip = false;
            switch (g_present.load()) {
                case host::PRESENT_30: break;
                case host::PRESENT_20: skip = tick_ % 3 == 0; break;
                default: skip = !last_skipped_ && interval > 35000; break;
            }
            last_skipped_ = skip;
            host::gfx_count(1, 0, 0, skip ? 1 : 0, 0);
            {
                pspsys::Lock l(*g_win_mutex);
                g_win.ticks++;
                if (skip) g_win.skipped++;
                if (game_ms >= 0 && g_win.game_ms.size() < 2048) g_win.game_ms.push_back(game_ms);
                g_win.idle_us += idle_delta;
            }
            // The game has swapped buffers (osViSwapBuffer) since the open frame's lists: that frame is complete. Without
            // this, a slow walk let the next list arrive before the VI that shows the swap, and both went into one frame.
            uint32_t next_fb = (uint32_t)osViGetNextFramebuffer();
            if (frame_open_ && next_fb != open_fb_) present_frame();
            if (skip) return;
            if (!frame_open_) {
                blitzgfx::begin_frame();
                frame_open_ = true;
                vis_open_ = 0;
                open_fb_ = next_fb;
            }
            stage(1);
            blitzgfx::submit_dl(rdram_, 8u << 20, task->t.data_ptr, task->t.ucode);
            stage(0);
            busy_us_ += pspsys::now_us() - now;
        }

        void update_screen() override {
            host::gfx_count(0, 1, 0, 0, 0);
            uint32_t origin = ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG;
            if (frame_open_) {
                vis_open_++;
                if (origin != last_origin_ || vis_open_ >= 3) present_frame();
            }
            last_origin_ = origin;
        }

    private:
        void present_frame() {
            stage(2);
            blitzgfx::end_frame();   // waits for the GE (blocked, not CPU: the game runs meanwhile)
            stage(3);
            uint64_t t0 = pspsys::now_us();
            const blitzgfx::Stats& s = blitzgfx::stats();
            uint32_t walk = s.walk_us, ge = s.ge_us;
            uint32_t uploads = s.tex_uploads, evictions = s.tex_evictions, resets = s.tex_resets, tex_bytes = s.tex_bytes;
            uint32_t vsync_waits = s.vsync_waits, vsync_us = s.vsync_us;
            frame_open_ = false;
            frames_++;
            if (frames_ % 30 == 1) {
                uint32_t v = 0, r = 0;
                blitzgfx::texture_memory(v, r);
                g_tex_vram = v;
                g_tex_ram = r;
                static blitzgfx::ComboInfo ci[64];
                int n = blitzgfx::combiner_list(ci, 64), unk = 0;
                for (int i = 0; i < n && i < 64; i++) unk += ci[i].exact ? 0 : 1;
                g_unk_cc = (uint32_t)unk;
                g_cc_keys = (uint32_t)n;
            }
            draw_overlay(walk, ge);
            stage(4);
            blitzgfx::present();
            stage(0);
            host::gfx_count(0, 0, 1, 0, 0);
            uint64_t other = pspsys::now_us() - t0;
            busy_us_ += other;
            pspsys::Lock l(*g_win_mutex);
            g_win.shown++;
            g_win.frames++;
            g_win.walk_us += walk;
            g_win.ge_us += ge;
            g_win.other_us += other;
            g_win.tex_uploads += uploads;
            g_win.tex_evictions += evictions;
            g_win.tex_resets += resets;
            g_win.tex_kb += tex_bytes / 1024;
            g_win.vsync_waits += vsync_waits;
            g_win.vsync_us += vsync_us;
        }

        void draw_overlay(uint32_t walk, uint32_t ge) {
            int mode = host::overlay_mode();
            uint64_t now = pspsys::now_us();
            g_hud.walk_us += walk;
            g_hud.ge_us += ge;
            g_hud.frames++;
            if (now - g_hud.t_us >= 1000000) {
                host::GfxStats gs = host::gfx_stats();
                uint64_t vi = host::vi_count();
                double dt = g_hud.t_us ? (now - g_hud.t_us) / 1e6 : 0;
                if (dt > 0) {
                    snprintf(hud_line_, sizeof hud_line_, "VI %4.1f tk %4.1f sh %4.1f cpu %4.1f ge %4.1f tex %u+%uK %dK",
                             (vi - g_hud.vi) / dt, (gs.display_lists - g_hud.ticks) / dt, (gs.shown - g_hud.shown) / dt,
                             g_hud.frames ? g_hud.walk_us / 1000.0 / g_hud.frames : 0.0,
                             g_hud.frames ? g_hud.ge_us / 1000.0 / g_hud.frames : 0.0, (unsigned)(g_tex_vram.load() / 1024),
                             (unsigned)(g_tex_ram.load() / 1024), pspsys::free_kb());
                }
                g_hud = Hud{ now, vi, gs.display_lists, gs.shown, 0, 0, 0 };
            }
            if (mode == host::OVERLAY_OFF) return;
            void* fb = blitzgfx::draw_buffer();
            if (!fb) return;
            if (!screen_inited_) {
                // 5650 at the draw buffer, no display setup (the GE owns the display); it clears that buffer once
                pspDebugScreenInitEx(fb, PSP_DISPLAY_PIXEL_FORMAT_565, 0);
                pspDebugScreenClearLineDisable();
                screen_inited_ = true;
            }
            pspDebugScreenSetBase((u32*)fb);
            pspDebugScreenEnableBackColor(1);
            pspDebugScreenSetBackColor(0xFF000000u);
            put_line(0, hud_line_, 0xFF00FFFFu);
            if (mode == host::OVERLAY_LOG) {
                std::vector<host::OverlayLine> lines = host::overlay_log_lines(10);
                int y = 34 - (int)lines.size();
                for (const host::OverlayLine& l : lines) put_line(y++, l.text.c_str(), l.color);
            }
        }

        static void put_line(int y, const char* text, uint32_t color) {
            char line[61];
            size_t n = strnlen(text, 60);
            memcpy(line, text, n);
            for (size_t i = n; i < 60; i++) line[i] = ' ';
            line[60] = 0;
            pspDebugScreenSetXY(0, y);
            pspDebugScreenSetTextColor(color);
            pspDebugScreenPuts(line);
        }

        uint8_t* rdram_;
        bool ok_ = false;
        bool frame_open_ = false, last_skipped_ = false, screen_inited_ = false;
        int vis_open_ = 0;
        uint32_t last_origin_ = 0, open_fb_ = 0;
        uint64_t last_tick_ = 0, last_idle_ = 0, busy_us_ = 0, tick_ = 0, frames_ = 0;
        char hud_line_[96] = "renderer: GE";
    };

    float percentile(std::vector<float>& v, double p) {
        if (v.empty()) return 0;
        size_t k = (size_t)(p * (v.size() - 1) + 0.5);
        std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
        return v[k];
    }
}

namespace host {
    std::unique_ptr<ultramodern::renderer::RendererContext> create_psp_renderer(uint8_t* rdram, ultramodern::renderer::WindowHandle wh, bool dev) {
        if (g_win_mutex == nullptr) g_win_mutex = new pspsys::Mutex();
        std::string text;
        if (pspsys::read_file(psplog::app_path("present"), text, 64)) {
            int m = present_mode_from(text);
            if (m >= 0) g_present = m;
            PLOG("gfx", "present switch: %s", present_mode_name(g_present.load()));
        }
        if (pspsys::exists(psplog::app_path("nogfx"))) {
            PLOG("gfx", "nogfx switch: the null renderer (M5) and the full debug screen");
            return create_null_renderer(rdram, wh, dev);
        }
        overlay_use_ge(true);   // the overlay thread stops drawing; this renderer draws into its frames
        auto r = std::make_unique<GuRenderer>(rdram);
        if (!r->valid()) {
            PERR("gfx", "the GE renderer failed to start: the null renderer instead");
            overlay_use_ge(false);
            return create_null_renderer(rdram, wh, dev);
        }
        set_gfx_mode_ge(true);
        return r;
    }

    int present_mode() { return g_present.load(); }
    void set_present_mode(int m) { if (m >= PRESENT_30 && m <= PRESENT_20) g_present = m; }
    const char* present_mode_name(int m) { return m == PRESENT_30 ? "30" : m == PRESENT_20 ? "20" : "auto"; }
    int present_mode_from(const std::string& text) {
        if (text.compare(0, 2, "30") == 0) return PRESENT_30;
        if (text.compare(0, 2, "20") == 0) return PRESENT_20;
        if (text.compare(0, 4, "auto") == 0) return PRESENT_AUTO;
        return -1;
    }

    // " ticks/s=30.0 shown/s=24.1 skip=6 game_ms p50=21.3 p95=31.8 gfx_cpu_ms=7.2 ge_ms=9.8 ..." for the last dt seconds.
    std::string gfx_perf_fields(double dt) {
        Window w;
        if (g_win_mutex != nullptr) {
            pspsys::Lock l(*g_win_mutex);
            w = std::move(g_win);
            g_win = Window();
        }
        if (!gfx_mode_ge()) {
            char buf[96];
            snprintf(buf, sizeof buf, " gfx=null");
            return buf;
        }
        char buf[512];
        double p50 = percentile(w.game_ms, 0.5), p95 = percentile(w.game_ms, 0.95);
        snprintf(buf, sizeof buf,
                 " ticks/s=%.1f shown/s=%.1f skip=%llu game_ms p50=%.1f p95=%.1f%s gfx_cpu_ms=%.1f ge_ms=%.1f overlay_ms=%.2f"
                 " idle_ms/s=%.0f vsync_waits=%llu vsync_ms=%.1f tex_vram_kb=%u tex_ram_kb=%u tex_up=%llu(%lluKB) tex_evict=%llu tex_reset=%llu unk_cc=%u cc_keys=%u present=%s",
                 dt > 0 ? w.ticks / dt : 0.0, dt > 0 ? w.shown / dt : 0.0, (unsigned long long)w.skipped, p50, p95,
                 g_idle_ok ? "" : "(incl idle)", w.frames ? w.walk_us / 1000.0 / w.frames : 0.0,
                 w.frames ? w.ge_us / 1000.0 / w.frames : 0.0, w.frames ? w.other_us / 1000.0 / w.frames : 0.0,
                 dt > 0 ? w.idle_us / 1000.0 / dt : 0.0, (unsigned long long)w.vsync_waits,
                 w.vsync_waits ? w.vsync_us / 1000.0 / w.vsync_waits : 0.0, (unsigned)(g_tex_vram.load() / 1024), (unsigned)(g_tex_ram.load() / 1024),
                 (unsigned long long)w.tex_uploads, (unsigned long long)w.tex_kb, (unsigned long long)w.tex_evictions,
                 (unsigned long long)w.tex_resets,
                 (unsigned)g_unk_cc.load(), (unsigned)g_cc_keys.load(), present_mode_name(g_present.load()));
        return buf;
    }
}

namespace blitzgfx { extern volatile uint32_t g_walk_pc, g_walk_cmds, g_walk_op, g_walk_runs; }

std::string host::gfx_breadcrumb() {
    static const char* names[] = {"idle", "walking a display list (submit_dl)", "waiting for the GE (end_frame)", "overlay",
                                  "presenting"};
    int st = g_stage;
    char b[256];
    snprintf(b, sizeof b, "gfx stage %d (%s) for %.1f s; walker run %u at pc 0x%08X op 0x%02X, %u commands into it\n", st,
             st >= 0 && st <= 4 ? names[st] : "?", (pspsys::now_us() - g_stage_us) / 1e6, (unsigned)blitzgfx::g_walk_runs,
             (unsigned)blitzgfx::g_walk_pc, (unsigned)blitzgfx::g_walk_op, (unsigned)blitzgfx::g_walk_cmds);
    return b;
}
