// gfx-dltest: the PSP renderer's walker on the Mac (null GE backend) over RDRAM dumps. For each dump it prints the
// counts tools/psp/gbi_check.py compares with tools/gbi_dump.py's decode, then the renderer's own counters.
//   gfx-dltest build/dumps/rdram_543.bin [...]      (each needs its .txt sidecar for the display list address)
#include "../gfx/gu_context.h"
#include "dump_info.h"

#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
void log_line(const char* s) { printf("  note: %s\n", s); }

bool read_all(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: gfx-dltest <rdram_N.bin>...\n"); return 2; }
    blitzgfx::Config cfg;
    cfg.log = log_line;
    if (!blitzgfx::init(cfg)) { fprintf(stderr, "init failed\n"); return 1; }
    int bad = 0;
    for (int a = 1; a < argc; a++) {
        std::string bin = argv[a], txt = bin.substr(0, bin.size() - 4) + ".txt", data, side;
        dlreplay::DumpInfo info;
        if (!read_all(bin, data) || !read_all(txt, side) || !dlreplay::parse_info(side, info)) {
            fprintf(stderr, "%s: can't read it or its .txt\n", bin.c_str());
            bad++;
            continue;
        }
        std::vector<uint8_t> rdram(data.begin(), data.end());
        printf("dump %s dlist=%08X ucode=%08X\n", dlreplay::stem(bin).c_str(), info.dlist, info.ucode);
        if (!getenv("GFX_KEEP_CACHE")) blitzgfx::reset_texture_cache();   // GFX_KEEP_CACHE=1: one cache over every dump (eviction test)
        int passes = getenv("GFX_PASSES") ? atoi(getenv("GFX_PASSES")) : 2;   // more: a profiling loop
        for (int pass = 0; pass < passes; pass++) {
            blitzgfx::begin_frame();
            blitzgfx::submit_dl(rdram.data(), (uint32_t)rdram.size(), info.dlist, info.ucode);
            blitzgfx::end_frame();
            const blitzgfx::Stats& s = blitzgfx::stats();
            if (pass == 0) {
                printf("decode cmds=%u vtx_cmds=%u vtx_loaded=%u tri_cmds=%u tris=%u unknown_ops=%u ex_cmds=%u ucode_switches=%u\n",
                       s.cmds, s.vtx_cmds, s.vtx_loaded, s.tri_cmds, s.tris, s.unknown_ops, s.ex_cmds, s.ucode_switches);
                printf("ops");
                for (int i = 0; i < 257; i++) if (s.op_counts[i]) printf(" %X=%u", i, s.op_counts[i]);
                printf("\n");
            }
            if (pass >= 2) continue;
            printf("%s rej_cmds=%u s16=%u float=%u clipped=%u culled=%u mixed=%u nonaffine=%u batches=%u texrects=%u "
                   "fillrects=%u zclears=%u tex lookups=%u hits=%u uploads=%u bytes=%u hashed=%u resets=%u failed=%u evicted=%u "
                   "cc_inexact=%u bl_inexact=%u list=%u draws=%u state=%u walk_us=%u\n",
                   pass == 0 ? "cold" : "warm", s.rej_cmds, s.tris_s16, s.tris_float, s.tris_clipped, s.tris_culled,
                   s.tris_mixed, s.nonaffine_mv, s.batches, s.texrects, s.fillrects, s.zclears, s.tex_lookups, s.tex_hits,
                   s.tex_uploads, s.tex_bytes, s.tex_hash_bytes, s.tex_resets, s.tex_failed, s.tex_evictions, s.cc_inexact_draws,
                   s.bl_inexact_draws, s.ge_list_bytes, s.ge_draws, s.ge_state_cmds, s.walk_us);
        }
    }
    blitzgfx::ComboInfo ci[256];
    int n = blitzgfx::combiner_list(ci, 256);
    printf("combiners %d\n", n);
    for (int i = 0; i < n && i < 256; i++)
        printf("  0x%016llX %dc %s uses=%u %s\n", (unsigned long long)ci[i].key, ci[i].cycles, ci[i].exact ? "exact   " : "FALLBACK",
               ci[i].uses, ci[i].desc);
    blitzgfx::shutdown();
    return bad ? 1 : 0;
}
