// The renderer's GE backend (sceGu). VRAM: two 512 x 272 5650 colour buffers (0x000000, 0x044000), a 16-bit z buffer
// (0x088000), then 1.2 MB of textures (0x0CC000-0x200000, written through the uncached alias); a 512 KB RAM overflow
// for textures. One display list per frame (Config::list_bytes, GU_DIRECT); vertices and indices are allocated inside
// it with sceGuGetMemory. The dcache is written back before every sceGuFinish. Depth: near = 65535, test GEQUAL,
// cleared to 0. Every set_* call is compared with a shadow copy first, so repeated state costs nothing.
#include "ge.h"
#include "arena.h"

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psputils.h>

#include <malloc.h>
#include <string.h>

namespace blitzgfx {
namespace ge {
namespace {

const uint32_t FB0 = 0x000000, FB1 = 0x044000, ZB = 0x088000, TEX0 = 0x0CC000, TEX_END = 0x200000;
const size_t RAM_TEX = 512 * 1024;

Config g_cfg;
unsigned int* g_list = nullptr;
uint8_t* g_vram = nullptr;       // uncached VRAM
uint8_t* g_ramtex = nullptr;
Arena g_vram_arena, g_ram_arena;
uint32_t g_draw = FB0;           // the buffer the GE draws into (the other one is shown)
bool g_started = false;
bool g_swapped = false;          // a swap is waiting for its vblank (Config::vsync)
unsigned int g_swap_vcount = 0;  // sceDisplayGetVcount() at that swap

struct Shadow {
    int valid;
    float vp[4];
    int sc[4];
    uint32_t proj_id, model_id;
    const Texture* tex;
    int tex_on;
    int lin, rs, rt;
    float tc[4];
    int func, rgba;
    uint32_t env;
    int blend, bsrc, bdst;
    int at, aref;
    int dt, dw;
    int cull;
    int smooth;
    int dither;
} g_sh;

void invalidate() { memset(&g_sh, 0xFF, sizeof g_sh); g_sh.valid = 0; g_sh.tex = (const Texture*)1; }

void restart_list() {
    sceKernelDcacheWritebackAll();
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuStart(GU_DIRECT, g_list);
    if (g_stats) g_stats->ge_flushes++;
}

inline void st() { if (g_stats) g_stats->ge_state_cmds++; }

int factor(int f, bool dst, unsigned& fix) {
    switch (f) {
    case BF_ZERO: fix = 0; return GU_FIX;
    case BF_ONE: fix = 0xFFFFFF; return GU_FIX;
    case BF_SRC_ALPHA: return GU_SRC_ALPHA;
    default: return GU_ONE_MINUS_SRC_ALPHA;
    }
    (void)dst;
}

}  // namespace

bool init(const Config& cfg) {
    g_cfg = cfg;
    g_list = (unsigned int*)memalign(64, cfg.list_bytes);
    g_ramtex = (uint8_t*)memalign(64, RAM_TEX);
    if (!g_list || !g_ramtex) return false;
    // pspgu writes the list through the uncached alias: no dirty line of the cached one may be written back over it later
    // (every end_frame writes the whole dcache back)
    sceKernelDcacheWritebackInvalidateRange(g_list, cfg.list_bytes);
    g_vram_arena.init(TEX_END - TEX0);
    g_ram_arena.init(RAM_TEX);
    g_draw = FB0;
    g_vram = (uint8_t*)((uintptr_t)sceGeEdramGetAddr() | 0x40000000u);
    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_5650, (void*)FB0, 512);
    sceGuDispBuffer(480, 272, (void*)FB1, 512);
    sceGuDepthBuffer((void*)ZB, 512);
    sceGuOffset(2048 - 240, 2048 - 136);
    sceGuViewport(2048, 2048, 480, 272);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, 480, 272);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_CLIP_PLANES);
    static ScePspIMatrix4 dither = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
    sceGuSetDither(&dither);
    sceGuEnable(GU_DITHER);
    sceGuClearColor(0xFF000000);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    if (cfg.vsync) guSwapBuffersBehaviour(PSP_DISPLAY_SETBUF_NEXTFRAME);
    invalidate();
    return true;
}

void shutdown() { sceGuTerm(); }

void begin_frame() {
    // The draw buffer was on screen until the vblank after the last swap: the GE's clear must not start before that vblank.
    // A blocking wait (the game's threads run meanwhile); it happens only when a list follows the swap within one vblank.
    if (g_swapped && sceDisplayGetVcount() == g_swap_vcount) {
        uint64_t t0 = sceKernelGetSystemTimeWide();
        sceDisplayWaitVblankStart();
        if (g_stats) { g_stats->vsync_waits++; g_stats->vsync_us += (uint32_t)(sceKernelGetSystemTimeWide() - t0); }
    }
    g_swapped = false;
    sceGuStart(GU_DIRECT, g_list);
    g_started = true;
    invalidate();
    sceGuSendCommandi(212, 0);                       // scissor to the whole screen for the clear
    sceGuSendCommandi(213, ((272 - 1) << 10) | (480 - 1));
    sceGuClearColor(0xFF000000);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    ScePspFMatrix4 id;
    memset(&id, 0, sizeof id);
    id.x.x = id.y.y = id.z.z = id.w.w = 1;
    sceGuSetMatrix(GU_VIEW, &id);
}

uint32_t end_frame() {
    if (!g_started) return 0;
    sceKernelDcacheWritebackAll();
    sceGuFinish();
    uint64_t t0 = sceKernelGetSystemTimeWide();
    sceGuSync(0, 0);
    g_started = false;
    return (uint32_t)(sceKernelGetSystemTimeWide() - t0);
}

void present() {
    g_draw = (uint32_t)(uintptr_t)sceGuSwapBuffers();   // the new draw buffer (relative to VRAM)
    if (g_cfg.vsync) {
        g_swapped = true;
        g_swap_vcount = sceDisplayGetVcount();
    }
}

void* draw_buffer() { return g_vram ? g_vram + g_draw : nullptr; }

void* list_mem(size_t bytes) {
    size_t need = (bytes + 15) & ~(size_t)15;
    if ((size_t)sceGuCheckList() + need + 16384 > g_cfg.list_bytes) restart_list();
    uint8_t* p = (uint8_t*)sceGuGetMemory((int)need + 16);
    return (void*)(((uintptr_t)p + 15) & ~(uintptr_t)15);
}

uint32_t list_used() { return (uint32_t)sceGuCheckList(); }

void* tex_mem(size_t bytes, size_t align) {
    size_t at;
    if (g_vram_arena.alloc(bytes, align, at)) return g_vram + TEX0 + at;
    if (g_ram_arena.alloc(bytes, align, at)) return g_ramtex + at;
    return nullptr;
}

void tex_free(void* p) {
    uint8_t* b = (uint8_t*)p;
    if (b >= g_vram + TEX0 && b < g_vram + TEX_END) g_vram_arena.free((size_t)(b - (g_vram + TEX0)));
    else if (b >= g_ramtex && b < g_ramtex + RAM_TEX) g_ram_arena.free((size_t)(b - g_ramtex));
    g_sh.tex = (const Texture*)1;   // a new texture may get this one's address
}

void tex_reset() {
    g_vram_arena.reset();
    g_ram_arena.reset();
    g_sh.tex = (const Texture*)1;
}

void cpu_wrote(const void* p, size_t bytes) {
    // VRAM is written through the uncached alias; the RAM overflow and static CLUTs go through the dcache, and the GE may
    // already be running this list up to the last sceGuGetMemory (it moves the stall address), so write back now
    if (((uintptr_t)p & 0x40000000u) == 0) sceKernelDcacheWritebackRange(p, bytes);
}

void wait_idle() { if (g_started) restart_list(); }
uint32_t tex_used() { return (uint32_t)(g_vram_arena.used() + g_ram_arena.used()); }
void tex_split(uint32_t& vram, uint32_t& ram) { vram = (uint32_t)g_vram_arena.used(); ram = (uint32_t)g_ram_arena.used(); }

void set_viewport(float xs, float ys, float xc, float yc) {
    if (g_sh.vp[0] == xs && g_sh.vp[1] == ys && g_sh.vp[2] == xc && g_sh.vp[3] == yc) return;
    g_sh.vp[0] = xs; g_sh.vp[1] = ys; g_sh.vp[2] = xc; g_sh.vp[3] = yc;
    sceGuSendCommandf(66, xs);
    sceGuSendCommandf(67, ys);
    sceGuSendCommandf(69, 2048 - 240 + xc);
    sceGuSendCommandf(70, 2048 - 136 + yc);
    st();
}

void set_scissor(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 480) x1 = 480;
    if (y1 > 272) y1 = 272;
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    if (g_sh.sc[0] == x0 && g_sh.sc[1] == y0 && g_sh.sc[2] == x1 && g_sh.sc[3] == y1) return;
    g_sh.sc[0] = x0; g_sh.sc[1] = y0; g_sh.sc[2] = x1; g_sh.sc[3] = y1;
    sceGuSendCommandi(212, (y0 << 10) | x0);
    sceGuSendCommandi(213, ((y1 - 1) << 10) | (x1 - 1));
    st();
}

void set_proj(const float m[16], uint32_t id) {
    if (g_sh.proj_id == id) return;
    g_sh.proj_id = id;
    ScePspFMatrix4 p;
    memcpy(&p, m, sizeof p);
    sceGuSetMatrix(GU_PROJECTION, &p);
    st();
}

void set_model(const float m[12], uint32_t id) {
    if (g_sh.model_id == id) return;
    g_sh.model_id = id;
    ScePspFMatrix4 w;
    memset(&w, 0, sizeof w);
    w.x.x = m[0]; w.x.y = m[1]; w.x.z = m[2];
    w.y.x = m[3]; w.y.y = m[4]; w.y.z = m[5];
    w.z.x = m[6]; w.z.y = m[7]; w.z.z = m[8];
    w.w.x = m[9]; w.w.y = m[10]; w.w.z = m[11]; w.w.w = 1;
    sceGuSetMatrix(GU_MODEL, &w);
    st();
}

void set_texture(const Texture* t) {
    if (!t) {
        if (g_sh.tex_on != 0) { sceGuDisable(GU_TEXTURE_2D); g_sh.tex_on = 0; st(); }
        return;
    }
    if (g_sh.tex_on != 1) { sceGuEnable(GU_TEXTURE_2D); g_sh.tex_on = 1; st(); }
    if (g_sh.tex == t) return;
    g_sh.tex = t;
    if (t->clut) {
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        sceGuClutLoad(t->clut_entries / 8, t->clut);
    }
    sceGuTexMode(t->psm, 0, 0, 1);
    sceGuTexImage(0, t->w, t->h, t->w, t->data);
    sceGuTexFlush();
    st();
}

void set_tex_sampling(bool linear, bool rs, bool rt) {
    if (g_sh.lin != (int)linear) { g_sh.lin = linear; sceGuTexFilter(linear ? GU_LINEAR : GU_NEAREST, linear ? GU_LINEAR : GU_NEAREST); st(); }
    if (g_sh.rs != (int)rs || g_sh.rt != (int)rt) {
        g_sh.rs = rs; g_sh.rt = rt;
        sceGuTexWrap(rs ? GU_REPEAT : GU_CLAMP, rt ? GU_REPEAT : GU_CLAMP);
        st();
    }
}

void set_texcoord(float su, float sv, float ou, float ov) {
    if (g_sh.tc[0] == su && g_sh.tc[1] == sv && g_sh.tc[2] == ou && g_sh.tc[3] == ov) return;
    g_sh.tc[0] = su; g_sh.tc[1] = sv; g_sh.tc[2] = ou; g_sh.tc[3] = ov;
    sceGuTexScale(su, sv);
    sceGuTexOffset(ou, ov);
    st();
}

void set_texfunc(int func, bool rgba, uint32_t env) {
    if (g_sh.func != func || g_sh.rgba != (int)rgba) { g_sh.func = func; g_sh.rgba = rgba; sceGuTexFunc(func, rgba ? GU_TCC_RGBA : GU_TCC_RGB); st(); }
    if (func == TFX_BLEND && g_sh.env != (env & 0xFFFFFF)) { g_sh.env = env & 0xFFFFFF; sceGuTexEnvColor(g_sh.env); st(); }
}

void set_blend(bool on, int src, int dst) {
    if (g_sh.blend != (int)on) { g_sh.blend = on; if (on) sceGuEnable(GU_BLEND); else sceGuDisable(GU_BLEND); st(); }
    if (!on || (g_sh.bsrc == src && g_sh.bdst == dst)) return;
    g_sh.bsrc = src; g_sh.bdst = dst;
    unsigned fa = 0, fb = 0;
    int s = factor(src, false, fa), d = factor(dst, true, fb);
    sceGuBlendFunc(GU_ADD, s, d, fa, fb);
    st();
}

void set_alpha_test(bool on, int ref) {
    if (g_sh.at != (int)on) { g_sh.at = on; if (on) sceGuEnable(GU_ALPHA_TEST); else sceGuDisable(GU_ALPHA_TEST); st(); }
    if (on && g_sh.aref != ref) { g_sh.aref = ref; sceGuAlphaFunc(GU_GEQUAL, ref, 0xFF); st(); }
}

void set_depth(bool test, bool write) {
    // the GE writes depth only with the test on, so a write without a test is a test that always passes
    bool on = test || write;
    if (g_sh.dt != (on ? (test ? 1 : 2) : 0)) {
        g_sh.dt = on ? (test ? 1 : 2) : 0;
        if (on) { sceGuEnable(GU_DEPTH_TEST); sceGuDepthFunc(test ? GU_GEQUAL : GU_ALWAYS); }
        else sceGuDisable(GU_DEPTH_TEST);
        st();
    }
    if (g_sh.dw != (int)write) { g_sh.dw = write; sceGuDepthMask(write ? 0 : 1); st(); }
}

void set_cull(int mode) {
    if (g_sh.cull == mode) return;
    g_sh.cull = mode;
    if (mode == CULL_NONE) { sceGuDisable(GU_CULL_FACE); st(); return; }
    bool back = mode == CULL_N64_BACK;
    if (g_cfg.cull_ccw) back = !back;
    sceGuEnable(GU_CULL_FACE);
    sceGuFrontFace(back ? GU_CCW : GU_CW);
    st();
}

void set_shade_smooth(bool smooth) {
    if (g_sh.smooth == (int)smooth) return;
    g_sh.smooth = smooth;
    sceGuShadeModel(smooth ? GU_SMOOTH : GU_FLAT);
    st();
}

void set_dither(bool on) {
    if (g_sh.dither == (int)on) return;
    g_sh.dither = on;
    if (on) sceGuEnable(GU_DITHER); else sceGuDisable(GU_DITHER);
    st();
}

void draw_indexed(const GVtx16* v, const uint8_t* idx, int count) {
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_INDEX_8BIT | GU_TRANSFORM_3D, count, idx, v);
    if (g_stats) g_stats->ge_draws++;
}

void draw_float(const GVtxF* v, int count) {
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, count, 0, v);
    if (g_stats) g_stats->ge_draws++;
}

void draw_rect(const GVtxF* v, bool strip4) {
    sceGuDrawArray(strip4 ? GU_TRIANGLE_STRIP : GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   strip4 ? 4 : 2, 0, v);
    if (g_stats) g_stats->ge_draws++;
}

}  // namespace ge
const char* backend_name() { return "sceGu"; }
}  // namespace blitzgfx
