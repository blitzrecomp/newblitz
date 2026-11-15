// The display-list replay EBOOT (docs/PSP_PORT.md M6): renders RDRAM dumps through the sceGu renderer (src/psp/gfx).
// For each <app>/dumps/rdram_<n>.bin with its .txt sidecar (copied there by tools/psp/dlreplay.sh; they are game data,
// never committed) it renders the frame PASSES times (the first with an empty texture cache), presents it, and writes
// <app>/shots/rdram_<n>.shot (BLITZSHOT1, sceDisplayGetFrameBuf's raw rows; tools/psp/shot2png.py) plus one
// "[dlreplay]" line per pass with the renderer's counters and timings. The combiner keys seen and a "done" line close
// the run, then it exits (tools/psp/run_ppsspp.sh stops PPSSPP at "[log] exit:").
#include "../gfx/gu_context.h"
#include "../psp_log.h"
#include "../psp_sys.h"
#include "dump_info.h"

#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

PSP_MODULE_INFO("NBDLReplay", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
PSP_HEAP_SIZE_KB(4096);

#ifndef BLITZ_BUILD
#define BLITZ_BUILD "0"
#endif
#ifndef BLITZ_COMMIT
#define BLITZ_COMMIT "unknown"
#endif

namespace {

const int PASSES = 3;
const uint32_t RDRAM_BYTES = 8u << 20;
alignas(64) uint8_t g_rdram[RDRAM_BYTES];   // the game's static RDRAM block will live the same way (4.2)
std::string g_app;

void gfx_log(const char* line) { PLOG("gfx", "%s", line); }

int exit_callback(int, int, void*) {
    psplog::shutdown("HOME exit");
    sceKernelExitGame();
    return 0;
}
int callback_thread(SceSize, void*) {
    int cb = sceKernelCreateCallback("dlreplay_exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

bool load_dump(const std::string& path, uint32_t& size) {
    SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    size = 0;
    while (size < RDRAM_BYTES) {
        int n = sceIoRead(fd, g_rdram + size, (SceSize)std::min<uint32_t>(RDRAM_BYTES - size, 1u << 20));
        if (n <= 0) break;
        size += (uint32_t)n;
    }
    sceIoClose(fd);
    return size >= (4u << 20);
}

bool write_shot(const std::string& path) {
    void* top = nullptr;
    int bw = 0, pf = 0;
    if (sceDisplayGetFrameBuf(&top, &bw, &pf, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0 || !top || bw <= 0) return false;
    int bpp = pf == 3 ? 4 : 2;
    const uint8_t* src = (const uint8_t*)((uintptr_t)top | 0x40000000u);   // uncached: what the GE wrote
    char head[64];
    int hn = snprintf(head, sizeof head, "BLITZSHOT1 480 272 %d\n", pf);
    std::string out(head, (size_t)hn);
    size_t row = (size_t)480 * bpp, at = out.size();
    out.resize(at + row * 272);
    for (int y = 0; y < 272; y++) memcpy(&out[at + (size_t)y * row], src + (size_t)y * bw * bpp, row);
    return pspsys::write_file(path, out.data(), out.size());
}

int dump_number(const std::string& name) {
    size_t u = name.find('_');
    return u == std::string::npos ? 0 : atoi(name.c_str() + u + 1);
}

}  // namespace

int main(int argc, char** argv) {
    g_app = (argc > 0 && argv && argv[0] && strchr(argv[0], ':')) ? pspsys::dir_name(argv[0]) : std::string("ms0:/PSP/GAME/NBDLREPLAY");
    SceUID cbt = sceKernelCreateThread("dlreplay_cb", callback_thread, 0x11, 0x1000, PSP_THREAD_ATTR_USER, NULL);
    if (cbt >= 0) sceKernelStartThread(cbt, 0, NULL);
    scePowerSetClockFrequency(333, 333, 166);

    psplog::Options lo;
    lo.app_dir = g_app;
    lo.host0_dir = "host0:/blitz-psp";
    lo.build = BLITZ_BUILD;
    lo.commit = BLITZ_COMMIT;
    lo.flavour = "dlreplay";
    lo.stdout_immediate = true;
    psplog::init(lo);
    PLOG("dlreplay", "build %s (%s), app %s, emulator=%d, free %d KB", BLITZ_BUILD, BLITZ_COMMIT, g_app.c_str(),
         pspsys::is_emulator(), pspsys::free_kb());

    blitzgfx::Config cfg;
    cfg.log = gfx_log;
    if (pspsys::exists(pspsys::join(g_app, "cull_ccw"))) cfg.cull_ccw = true;
    if (pspsys::exists(pspsys::join(g_app, "noclip"))) cfg.near_clip = false;
    if (!blitzgfx::init(cfg)) {
        PERR("dlreplay", "renderer init failed");
        psplog::shutdown("init failed");
        sceKernelExitGame();
        return 1;
    }
    PLOG("dlreplay", "renderer %s, list %u KB, near_clip=%d cull_ccw=%d", blitzgfx::backend_name(), cfg.list_bytes / 1024,
         cfg.near_clip, cfg.cull_ccw);

    std::vector<pspsys::DirEntry> ents;
    std::string ddir = pspsys::join(g_app, "dumps");
    pspsys::list_dir(ddir, ents);
    std::vector<std::string> dumps;
    for (auto& e : ents)
        if (!e.dir && e.name.size() > 10 && e.name.compare(0, 6, "rdram_") == 0 && e.name.compare(e.name.size() - 4, 4, ".bin") == 0)
            dumps.push_back(e.name);
    std::sort(dumps.begin(), dumps.end(), [](const std::string& a, const std::string& b) { return dump_number(a) < dump_number(b); });
    PLOG("dlreplay", "%d dumps in %s", (int)dumps.size(), ddir.c_str());
    pspsys::make_dirs(pspsys::join(g_app, "shots"));

    uint32_t total_unknown = 0;
    int done = 0;
    for (const std::string& name : dumps) {
        std::string bin = pspsys::join(ddir, name), stem = dlreplay::stem(name), side;
        dlreplay::DumpInfo info;
        if (!pspsys::read_file(pspsys::join(ddir, stem + ".txt"), side, 4096) || !dlreplay::parse_info(side, info)) {
            PWARN("dlreplay", "%s: no .txt sidecar with dlist=; skipped", stem.c_str());
            continue;
        }
        uint32_t size = 0;
        uint64_t t_load = sceKernelGetSystemTimeWide();
        if (!load_dump(bin, size)) { PWARN("dlreplay", "%s: can't read it", stem.c_str()); continue; }
        PLOG("dlreplay", "%s: %u KB in %u ms, dlist=%08X ucode=%08X", stem.c_str(), size / 1024,
             (unsigned)((sceKernelGetSystemTimeWide() - t_load) / 1000), info.dlist, info.ucode);
        blitzgfx::reset_texture_cache();
        for (int pass = 0; pass < PASSES; pass++) {
            uint64_t t0 = sceKernelGetSystemTimeWide();
            blitzgfx::begin_frame();
            blitzgfx::submit_dl(g_rdram, size, info.dlist, info.ucode);
            blitzgfx::end_frame();
            uint32_t total = (uint32_t)(sceKernelGetSystemTimeWide() - t0);
            const blitzgfx::Stats& s = blitzgfx::stats();
            if (pass == 0) total_unknown += s.unknown_ops;
            PLOG("dlreplay", "%s pass=%d %s cmds=%u unknown_ops=%u ex=%u vtx=%u tris=%u s16=%u float=%u clipped=%u culled=%u "
                 "batches=%u draws=%u rects=%u tex_lookups=%u hits=%u uploads=%u tex_kb=%u list_kb=%u flushes=%u "
                 "cc_fallback_draws=%u walk_ms=%.2f ge_ms=%.2f total_ms=%.2f",
                 stem.c_str(), pass, pass == 0 ? "cold" : "warm", s.cmds, s.unknown_ops, s.ex_cmds, s.vtx_loaded, s.tris,
                 s.tris_s16, s.tris_float, s.tris_clipped, s.tris_culled, s.batches, s.ge_draws, s.texrects + s.fillrects,
                 s.tex_lookups, s.tex_hits, s.tex_uploads, s.tex_bytes / 1024, s.ge_list_bytes / 1024, s.ge_flushes,
                 s.cc_inexact_draws, s.walk_us / 1000.0, s.ge_us / 1000.0, total / 1000.0);
        }
        blitzgfx::present();
        sceDisplayWaitVblankStart();
        sceDisplayWaitVblankStart();
        std::string shot = pspsys::join(pspsys::join(g_app, "shots"), stem + ".shot");
        bool ok = write_shot(shot);
        PLOG("dlreplay", "%s: shot %s %s", stem.c_str(), shot.c_str(), ok ? "written" : "FAILED");
        done++;
    }
    blitzgfx::ComboInfo ci[128];
    int n = blitzgfx::combiner_list(ci, 128);
    int fallback = 0;
    for (int i = 0; i < n && i < 128; i++) {
        PLOG("dlreplay", "combiner 0x%016llX %d-cycle %s uses=%u %s", (unsigned long long)ci[i].key, ci[i].cycles,
             ci[i].exact ? "exact" : "FALLBACK", ci[i].uses, ci[i].desc);
        if (!ci[i].exact) fallback++;
    }
    PLOG("dlreplay", "done dumps=%d unknown_ops=%u combiner_keys=%d fallback_keys=%d", done, total_unknown, n, fallback);
    blitzgfx::shutdown();
    psplog::shutdown("dlreplay done");
    sceKernelExitGame();
    return 0;
}
