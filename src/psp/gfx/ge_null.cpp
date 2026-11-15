// The renderer's backend on the Mac: every GE call is counted and nothing is drawn, so the walker, the texture decoder
// and the combiner solver run (and can be checked against tools/gbi_dump.py) without a PSP.
#include "ge.h"
#include "arena.h"

#include <stdlib.h>
#include <string.h>

namespace blitzgfx {
namespace ge {
namespace {
uint8_t* g_list = nullptr;
size_t g_list_size = 0, g_list_used = 0;
uint8_t* g_tex = nullptr;
const size_t TEX_BYTES = (1216 + 512) * 1024;   // the PSP's VRAM share plus its RAM overflow
Arena g_arena;
}  // namespace

bool init(const Config& cfg) {
    g_list_size = cfg.list_bytes;
    g_list = (uint8_t*)aligned_alloc(64, g_list_size);
    g_tex = (uint8_t*)aligned_alloc(64, TEX_BYTES);
    // GFX_TEX_KB=<n>: a smaller arena, to exercise the cache's LRU eviction on the Mac
    const char* kb = getenv("GFX_TEX_KB");
    g_arena.init(kb && atoi(kb) > 0 && (size_t)atoi(kb) * 1024 < TEX_BYTES ? (size_t)atoi(kb) * 1024 : TEX_BYTES);
    return g_list && g_tex;
}
void shutdown() { free(g_list); free(g_tex); g_list = g_tex = nullptr; }
void begin_frame() { g_list_used = 0; }
uint32_t end_frame() { return 0; }
void present() {}
void* list_mem(size_t bytes) {
    bytes = (bytes + 15) & ~(size_t)15;
    if (g_list_used + bytes > g_list_size) { if (g_stats) g_stats->ge_flushes++; g_list_used = 0; }
    void* p = g_list + g_list_used;
    g_list_used += bytes;
    return p;
}
uint32_t list_used() { return (uint32_t)g_list_used; }
void* tex_mem(size_t bytes, size_t align) {
    size_t at;
    if (!g_arena.alloc(bytes, align, at)) return nullptr;
    return g_tex + at;
}
void tex_free(void* p) { g_arena.free((size_t)((uint8_t*)p - g_tex)); }
void tex_reset() { g_arena.reset(); }
void cpu_wrote(const void*, size_t) {}
void wait_idle() { if (g_stats) g_stats->ge_flushes++; }
uint32_t tex_used() { return (uint32_t)g_arena.used(); }
void tex_split(uint32_t& vram, uint32_t& ram) {
    size_t u = g_arena.used(), v = (size_t)1216 * 1024;
    vram = (uint32_t)(u < v ? u : v);
    ram = (uint32_t)(u - vram);
}
void* draw_buffer() { return nullptr; }

#define STATE() do { if (g_stats) g_stats->ge_state_cmds++; } while (0)
void set_viewport(float, float, float, float) { STATE(); }
void set_scissor(int, int, int, int) { STATE(); }
void set_proj(const float*, uint32_t) { STATE(); }
void set_model(const float*, uint32_t) { STATE(); }
void set_texture(const Texture*) { STATE(); }
void set_tex_sampling(bool, bool, bool) { STATE(); }
void set_texcoord(float, float, float, float) { STATE(); }
void set_texfunc(int, bool, uint32_t) { STATE(); }
void set_blend(bool, int, int) { STATE(); }
void set_alpha_test(bool, int) { STATE(); }
void set_depth(bool, bool) { STATE(); }
void set_cull(int) { STATE(); }
void set_shade_smooth(bool) { STATE(); }
void set_dither(bool) { STATE(); }
void draw_indexed(const GVtx16*, const uint8_t*, int) { if (g_stats) g_stats->ge_draws++; }
void draw_float(const GVtxF*, int) { if (g_stats) g_stats->ge_draws++; }
void draw_rect(const GVtxF*, bool) { if (g_stats) g_stats->ge_draws++; }

}  // namespace ge
const char* backend_name() { return "null"; }
}  // namespace blitzgfx
