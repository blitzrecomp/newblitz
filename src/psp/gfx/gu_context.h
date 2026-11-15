// The PSP renderer, v1 (docs/PSP_PORT.md 4.5, M6): an F3DEX2 / F3DLX2.Rej display-list walker that drives the GE through
// sceGu. This header is the whole interface M5/M7 wire in (the RendererContext's send_dl and update_screen):
//
//   blitzgfx::Config cfg;  cfg.log = my_log_line;      // once-only notices (unknown opcodes, combiner keys, ...)
//   blitzgfx::init(cfg);                                // GE, framebuffers, VRAM (PSP); nothing to set up on the Mac
//   ...per frame (gfx thread):
//   blitzgfx::begin_frame();
//   blitzgfx::submit_dl(rdram, 8 << 20, task->data_ptr, task->ucode);   // one OSTask's display list (more than one is fine)
//   blitzgfx::end_frame();                              // finish the GE list and wait for the GE
//   blitzgfx::present();                                // swap (on VI / osViSwapBuffer)
//   const blitzgfx::Stats& s = blitzgfx::stats();       // this frame's counters and timings
//
// rdram is N64ModernRuntime's RDRAM (native 32-bit words, each N64 big-endian word byte-reversed; the dumps in
// build/dumps are the same). dl is the display list address (virtual, physical or segmented with segment 0 = 0).
// ucode_text is the OSTask's ucode address: 0x80028FA0 starts in F3DLX2.Rej, anything else in F3DEX2; G_LOAD_UCODE inside
// the list switches between them. Nothing in RDRAM is written. Every vertex and texel the GE reads is a renderer-owned
// copy, so the caller may let the game run on as soon as submit_dl returns (v1.1: signal DP done before end_frame).
//
// Backends: PSP_TARGET psp (sceGu, gfx/ge_psp.cpp) and the Mac (gfx/ge_null.cpp: every GE call is counted, nothing is
// drawn), so the walker, texture decoder and combiner solver are the same code on both and can be tested on the Mac.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace blitzgfx {

struct Config {
    uint32_t list_bytes = 512 * 1024;   // one GE display list (vertices and indices live in it too)
    bool near_clip = true;              // F3DEX2 sections: CPU clip of triangles that cross near/far/guard planes
    float guard = 4.0f;                 // x/y guard band for that clip, in multiples of w (the GE's 4096-pixel range)
    bool hash_textures = true;          // re-hash texture sources once per frame (catches repainted textures)
    bool cull_ccw = false;              // flip the GE's cull winding if the backend sees it the other way round
    bool rect_linear = true;            // filter TEXRECTs linearly: 512 -> 480 is not 1:1, and point sampling drops one
                                        // column in 16 (1-pixel strokes of the HUD font vanish)
    bool vsync = false;                 // PSP: swap at the next vblank, and wait for it before the GE draws into the buffer
                                        // that was on screen. Off: pspgu's default, the swap at the next hsync (a tear line
                                        // on hardware wherever the scan-out is; PPSSPP shows whole frames either way)
    void (*log)(const char* line) = nullptr;
};

struct Stats {
    // walker (matches tools/gbi_dump.py's decode: commands, per-opcode counts, vertices, triangles)
    uint32_t cmds, ex_cmds, unknown_ops, ucode_switches, rej_cmds, dl_calls, dl_overflow;
    uint32_t vtx_cmds, vtx_loaded, modifyvtx, tri_cmds, tris;
    uint32_t op_counts[257];            // [0x100] = RT64 G_EX commands
    // geometry
    uint32_t tris_s16, tris_float, tris_clipped, tris_culled, tris_mixed, nonaffine_mv, lit_vtx;
    uint32_t batches, texrects, fillrects, zclears;
    // textures
    uint32_t tex_lookups, tex_hits, tex_uploads, tex_bytes, tex_hash_bytes, tex_resets, tex_failed, tex_evictions;
    // combiner / blender
    uint32_t cc_inexact_draws, bl_inexact_draws;
    // GE
    uint32_t ge_list_bytes, ge_flushes, ge_draws, ge_state_cmds;
    // timings (microseconds): walk = CPU time inside submit_dl, ge = waiting for the GE in end_frame
    uint32_t walk_us, ge_us;
    uint32_t vsync_waits, vsync_us;     // begin_frame waited for the vblank that shows the last swap (Config::vsync)
};

// A combiner key seen since init, with how the GE does it (for logs and the M6 report).
struct ComboInfo {
    uint64_t key;        // the whole G_SETCOMBINE command, 0xFC in the top byte
    int cycles;          // 1 or 2
    bool exact;          // false: fell back to MODULATE (logged once)
    uint32_t uses;       // draws
    char desc[112];
};

bool init(const Config& cfg);
void shutdown();
void begin_frame();
void submit_dl(const uint8_t* rdram, uint32_t rdram_size, uint32_t dl, uint32_t ucode_text);
void end_frame();
void present();
const Stats& stats();
const char* backend_name();
void reset_texture_cache();                 // next frame starts cold (the replay's cold/warm timings)
int combiner_list(ComboInfo* out, int max); // returns how many exist (fills up to max)
void texture_memory(uint32_t& vram_bytes, uint32_t& ram_bytes);   // the texture cache's use of each arena now
void* draw_buffer();                        // the colour buffer being drawn (PSP: uncached VRAM, 512 x 272 5650; Mac: null)

}  // namespace blitzgfx
