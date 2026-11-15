// Shared pieces of the renderer: RDRAM reads, RDP tiles, TMEM and the texture cache, the combiner solver.
#pragma once
#include "ge.h"
#include "gu_context.h"

#include <stdint.h>
#include <string.h>

#include <unordered_map>
#include <vector>

namespace blitzgfx {

void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));   // through Config::log
const Config& config();

// RDRAM as N64ModernRuntime keeps it: native 32-bit words, each N64 big-endian word byte-reversed.
struct Mem {
    const uint8_t* base = nullptr;
    uint32_t size = 0;
    static uint32_t phys(uint32_t a) { return a & 0x1FFFFFFFu; }
    bool valid(uint32_t a, uint32_t n) const { uint32_t p = phys(a); return p <= size && n <= size - p; }
    uint32_t u32(uint32_t a) const { uint32_t p = phys(a) & ~3u; return p + 4 <= size ? *(const uint32_t*)(base + p) : 0; }
    uint16_t u16(uint32_t a) const { uint32_t p = phys(a) & ~1u; return p + 2 <= size ? *(const uint16_t*)(base + (p ^ 2)) : 0; }
    uint8_t u8(uint32_t a) const { uint32_t p = phys(a); return p < size ? base[p ^ 3] : 0; }
};

enum { FMT_RGBA = 0, FMT_YUV = 1, FMT_CI = 2, FMT_IA = 3, FMT_I = 4 };

struct Tile {
    uint8_t fmt = 0, siz = 0;
    uint16_t line = 0, tmem = 0;
    uint8_t palette = 0, cmt = 0, maskt = 0, shiftt = 0, cms = 0, masks = 0, shifts = 0;
    uint16_t uls = 0, ult = 0, lrs = 0, lrt = 0;
};

struct Timg { uint8_t fmt = 0, siz = 0; uint16_t width = 1; uint32_t addr = 0; };

// A GE texture made from one tile of TMEM.
struct TexEntry {
    ge::Texture tex;
    int dw, dh;                 // decoded period/size before power-of-two padding
    bool repeat_s, repeat_t;
    uint32_t last_frame;
};

// TMEM, recorded rather than filled: each load is a record, and each of TMEM's 512 64-bit lines remembers the record
// that last wrote it. A draw's tile is looked up by a key over the tile and the records (with a hash of the bytes they
// read); only on a miss are those records replayed into a scratch TMEM, as the RDP fills it, and the tile decoded.
class Tmem {
public:
    void begin_frame(uint32_t frame);
    void reset_cache();
    void load_block(const Mem& m, const Timg& ti, const Tile& t, int sl, int tl, int sh, int dxt);
    void load_tile(const Mem& m, const Timg& ti, const Tile& t, int sl, int tl, int sh, int th);
    void load_tlut(const Mem& m, const Timg& ti, const Tile& t, int sl, int tl, int sh, int th);
    // tlut_mode: othermode H textlut (0 none, 2 RGBA16, 3 IA16). nullptr if it can't be made.
    const TexEntry* get(const Mem& m, const Tile& t, int tlut_mode);

private:
    enum Kind : uint8_t { K_BLOCK, K_TILE, K_TLUT };
    struct Rec {
        Kind kind;
        Timg timg;
        uint16_t tmem, line;
        uint16_t sl, tl, sh, th;
        uint32_t src, nbytes;
        uint32_t hash;
        bool hashed;
    };
    uint16_t add(const Mem& m, Kind k, const Timg& ti, const Tile& t, int sl, int tl, int sh, int th, uint32_t src, uint32_t nb,
             uint32_t first_line, uint32_t nlines);
    void replay(const Mem& m, const Rec& r);
    uint32_t rec_hash(const Mem& m, Rec& r);
    void store(uint32_t byte_addr, const uint8_t* data, uint32_t n, bool odd, bool split32);
    uint32_t texel(const Tile& t, int s, int tt, int tlut_mode, bool* indexed) const;
    TexEntry* decode(const Mem& m, const Tile& t, int tlut_mode, uint64_t key);
    void* alloc_tex(size_t bytes);   // ge::tex_mem, freeing least recently used entries of earlier frames until it fits
    const TexEntry* lookup(const Mem& m, const Tile& t, int tlut_mode);

    std::vector<Rec> recs_;
    uint16_t owner_[512] = {};       // record index + 1 (0: nothing this frame)
    uint8_t scratch_[4096] = {};     // TMEM in N64 byte order
    std::unordered_map<uint64_t, TexEntry> cache_;
    uint32_t frame_ = 0;
    std::vector<uint8_t> lin_;       // linear decode before swizzle
    // get()'s answer for a tile, valid until the next load into TMEM (draws re-resolve the same tile many times)
    struct Last { Tile tile; int tlut; uint32_t serial; const TexEntry* e; };
    Last last_[8] = {};
    uint32_t serial_ = 1;
};

// The combiner (both cycles) reduced to what the GE can do: a texture function, vertex colour as an affine function of
// the shade colour, and the env colour.
struct CombineOut {
    bool textured;               // GE texturing on
    int func;                    // ge::TexFunc
    bool tcc_rgba;
    uint32_t env_abgr;
    float c0[3], cs[3], csa[3];  // vertex rgb = c0 + cs * shade.rgb + csa * shade.a
    float a0, asa;               // vertex a   = a0 + asa * shade.a
    bool use_tex1;               // the result is mostly TEXEL1: sample tile + 1 (TEXEL1 = TEXEL0 is v1's one texture)
    bool exact;
};
// combine: the G_SETCOMBINE command's two words; tex_on: G_TEXTURE on; colours are N64 RGBA (r in the top byte).
CombineOut solve_combiner(uint32_t w0, uint32_t w1, bool two_cycle, bool tex_on, uint32_t prim, uint32_t env,
                          uint8_t prim_lodfrac);
void describe_combiner(char* out, size_t n, uint32_t w0, uint32_t w1, bool two_cycle, const CombineOut& c);

struct BlendOut {
    bool blend;
    int src, dst;                // ge::BlendFactor
    bool alpha_test;
    int alpha_ref;
    bool depth_test, depth_write;
    bool exact;
};
BlendOut solve_blender(uint32_t othermode_h, uint32_t othermode_l, bool zbuffer, uint32_t blend_color);

uint32_t hash32(const void* p, size_t n, uint32_t h = 2166136261u);

}  // namespace blitzgfx
