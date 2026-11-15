// The renderer's backend: a thin layer over sceGu (ge_psp.cpp) or a counting stand-in for the Mac (ge_null.cpp).
// Everything N64-specific happens before this layer; these calls are plain GE state and draws. Each set_* call is
// cached by the backend, so the walker may call them for every draw.
#pragma once
#include "gu_context.h"

#include <stddef.h>
#include <stdint.h>

namespace blitzgfx {

// Vertex layouts (GE order: texture, colour, position; each component aligned to its size).
struct GVtx16 { int16_t u, v; uint32_t c; int16_t x, y, z, pad; };   // 3D, s16: u/v and x/y/z read as value/32768
struct GVtxF { float u, v; uint32_t c; float x, y, z; };              // 3D float (clipped and mixed-matrix triangles),
                                                                       // and 2D (through mode: x/y in pixels, u/v texels)
static_assert(sizeof(GVtx16) == 16, "GE s16 vertex is 16 bytes");
static_assert(sizeof(GVtxF) == 24, "GE float vertex is 24 bytes");

namespace ge {

enum Psm { PSM_5650 = 0, PSM_5551 = 1, PSM_4444 = 2, PSM_8888 = 3, PSM_T4 = 4, PSM_T8 = 5 };
enum TexFunc { TFX_MODULATE = 0, TFX_DECAL = 1, TFX_BLEND = 2, TFX_REPLACE = 3, TFX_ADD = 4 };
enum BlendFactor { BF_ZERO = 0, BF_ONE, BF_SRC_ALPHA, BF_INV_SRC_ALPHA };
enum Cull { CULL_NONE = 0, CULL_N64_BACK, CULL_N64_FRONT };   // N64 back = clockwise on screen (y down)

struct Texture {
    const void* data;      // swizzled, 16-byte aligned
    const void* clut;      // 8888 entries, 64-byte aligned (T4/T8 only)
    int psm, w, h;         // w, h powers of two
    int clut_entries;
};

bool init(const Config& cfg);
void shutdown();
void begin_frame();
uint32_t end_frame();               // finishes the list and waits for the GE; returns the wait in microseconds
void present();
void* list_mem(size_t bytes);       // in the current list (16-byte aligned); flushes the list first if it is full
uint32_t list_used();
// Texture memory: VRAM first, then a RAM overflow, each a first-fit arena (arena.h). nullptr when neither has a gap: the
// cache frees its least recently used entries (tex_free) and tries again; as a last resort it calls wait_idle() and then
// tex_reset() (nothing queued reads the old textures any more).
void* tex_mem(size_t bytes, size_t align);
void tex_free(void* p);             // a block from tex_mem; the caller knows the GE no longer reads it
void tex_reset();
void cpu_wrote(const void* p, size_t bytes);   // texture data written through the cache: make it visible to the GE
void wait_idle();                   // finish what is queued, wait, start a new list (GE state is kept)
uint32_t tex_used();
void tex_split(uint32_t& vram_bytes, uint32_t& ram_bytes);   // tex_used() by arena
void* draw_buffer();                // the colour buffer the GE draws into now (uncached; 512 x 272, 5650); null on the Mac

void set_viewport(float xscale, float yscale, float xcenter, float ycenter);   // PSP pixels, centre relative to (0,0)
void set_scissor(int x0, int y0, int x1, int y1);                              // PSP pixels, x1/y1 exclusive
void set_proj(const float m[16], uint32_t id);    // id: skip the upload when the same matrix is already set
void set_model(const float m[12], uint32_t id);   // rows x, y, z, translation (row vectors, as N64 Mtx)
void set_texture(const Texture* t);               // nullptr: texturing off
void set_tex_sampling(bool linear, bool repeat_s, bool repeat_t);
void set_texcoord(float su, float sv, float ou, float ov);
void set_texfunc(int func, bool rgba, uint32_t env_abgr);
void set_blend(bool on, int src, int dst);
void set_alpha_test(bool on, int ref);            // pass when alpha >= ref
void set_depth(bool test, bool write);            // test: GEQUAL (near = 65535)
void set_cull(int mode);
void set_shade_smooth(bool smooth);
void set_dither(bool on);                         // the N64's rgbdither (these lists set DISABLE)

void draw_indexed(const GVtx16* v, const uint8_t* idx, int count);   // triangles, 3D, 8-bit indices
void draw_float(const GVtxF* v, int count);                          // triangles, 3D, no indices
void draw_rect(const GVtxF* v, bool strip4);                         // 2D: a sprite (2 vertices) or a 4-vertex strip

extern Stats* g_stats;

}  // namespace ge
}  // namespace blitzgfx
