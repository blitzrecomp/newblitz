// TMEM and the texture cache (docs/PSP_PORT.md 4.5). The load and texel rules are ported from tools/gbi_dump.py's Tmem
// (which follows the RDP: odd lines stored with their 32-bit halves swapped, RGBA32 split into a red/green and a
// blue/alpha half, TLUT entries in the high half replicated four times). Our own code.
//
// N64 -> GE formats: CI4/I4/IA4 -> T4, CI8/I8/IA8 -> T8 (with the TLUT, a grey ramp or an IA ramp as the CLUT),
// RGBA16 -> 5551, IA16/RGBA32 -> 8888. Sizes are padded to powers of two (at least 16 bytes by 8 rows, for the swizzle);
// wrapping tiles decode one period (two when mirrored, so mirroring is exact) and repeat, clamping ones replicate the edge.
#include "gfx_internal.h"

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>

namespace blitzgfx {
namespace {

const int SIZ_BITS[4] = {4, 8, 16, 32};

uint64_t fnv64(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) { h ^= (v >> (i * 8)) & 0xFF; h *= 1099511628211ull; }
    return h;
}

uint32_t pow2(uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }

uint32_t rgba5551_to_abgr(uint16_t v) {
    uint32_t r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | ((v & 1) ? 0xFF000000u : 0);
}
uint32_t ia16_to_abgr(uint16_t v) { uint32_t i = v >> 8; return i | (i << 8) | (i << 16) | ((uint32_t)(v & 255) << 24); }
uint32_t grey(uint32_t i, uint32_t a) { return i | (i << 8) | (i << 16) | (a << 24); }

// Fixed CLUTs for the intensity formats (8888 entries).
alignas(64) uint32_t g_clut_i4[16], g_clut_ia4[16], g_clut_i8[256], g_clut_ia8[256], g_clut_rgba4[16], g_clut_rgba8[256];
bool g_cluts_made = false;
void make_cluts() {
    for (int v = 0; v < 16; v++) {
        g_clut_i4[v] = grey(v * 17, v * 17);
        uint32_t i = (uint32_t)((v >> 1) * 255 / 7);
        g_clut_ia4[v] = grey(i, (v & 1) ? 255 : 0);
        g_clut_rgba4[v] = grey(v, 255);
    }
    for (int v = 0; v < 256; v++) {
        g_clut_i8[v] = grey(v, v);
        g_clut_ia8[v] = grey((v >> 4) * 17, (v & 15) * 17);
        g_clut_rgba8[v] = grey(v, 255);
    }
    g_cluts_made = true;
    ge::cpu_wrote(g_clut_i4, sizeof g_clut_i4); ge::cpu_wrote(g_clut_ia4, sizeof g_clut_ia4);
    ge::cpu_wrote(g_clut_rgba4, sizeof g_clut_rgba4); ge::cpu_wrote(g_clut_i8, sizeof g_clut_i8);
    ge::cpu_wrote(g_clut_ia8, sizeof g_clut_ia8); ge::cpu_wrote(g_clut_rgba8, sizeof g_clut_rgba8);
}

// One axis of the RDP's tile addressing reduced to the decoded image: x in [0, padded) -> tile-relative texel.
struct Axis {
    int size;       // tile size (texels)
    int period;     // decoded width before padding
    bool repeat, mirror;
    int map(int x) const {
        if (!repeat) return x < size ? x : size - 1;
        if (mirror) {
            int p2 = period, h = p2 / 2, i = x % p2;
            return i < h ? i : p2 - 1 - i;
        }
        return x % period;
    }
};
Axis make_axis(int ul, int lr, int mask, int cm) {
    Axis a;
    a.size = (lr >> 2) - (ul >> 2) + 1;
    if (a.size <= 0) a.size = mask ? (1 << mask) : 1;
    bool clamp = (cm & 2) || mask == 0;
    a.mirror = false;
    if (mask && !(cm & 2)) {
        a.repeat = true;
        a.mirror = (cm & 1) != 0;
        a.period = (1 << mask) * (a.mirror ? 2 : 1);
    } else if (mask && a.size > (1 << mask)) {   // clamp with a smaller mask: wraps inside the tile (approximated)
        a.repeat = true;
        a.period = 1 << mask;
    } else {
        (void)clamp;
        a.repeat = false;
        a.period = a.size;
    }
    if (a.period > 512) a.period = 512;
    if (a.size > 512) a.size = 512;
    return a;
}

}  // namespace

void Tmem::begin_frame(uint32_t frame) {
    frame_ = frame;
    serial_++;
    recs_.clear();
    memset(owner_, 0, sizeof owner_);
    if (!g_cluts_made) make_cluts();
}

void Tmem::reset_cache() {
    serial_++;
    cache_.clear();
    ge::tex_reset();
}

uint16_t Tmem::add(const Mem&, Kind k, const Timg& ti, const Tile& t, int sl, int tl, int sh, int th, uint32_t src, uint32_t nb,
               uint32_t first_line, uint32_t nlines) {
    if (recs_.size() >= 0xFFFE) return 0;
    serial_++;
    Rec r;
    r.kind = k;
    r.timg = ti;
    r.tmem = t.tmem;
    r.line = t.line;
    r.sl = (uint16_t)sl; r.tl = (uint16_t)tl; r.sh = (uint16_t)sh; r.th = (uint16_t)th;
    r.src = src;
    r.nbytes = nb;
    r.hash = 0;
    r.hashed = false;
    recs_.push_back(r);
    uint16_t id = (uint16_t)recs_.size();
    for (uint32_t i = 0; i < nlines && i < 512; i++) owner_[(first_line + i) & 511] = id;
    return id;
}

void Tmem::load_block(const Mem& m, const Timg& ti, const Tile& t, int sl, int tl, int sh, int dxt) {
    int bpp = SIZ_BITS[ti.siz & 3];
    uint32_t start = ti.addr + (uint32_t)((tl * ti.width + sl) * bpp / 8);
    int texels = sh - sl + 1;
    if (texels < 0) texels = 0;
    uint32_t nbytes = (uint32_t)((texels * bpp + 7) / 8);
    uint32_t lines;
    if (ti.siz == 3) {   // RGBA32: each half gets 2 bytes per texel
        lines = (uint32_t)((texels * 2 + 7) / 8);
        uint16_t id = add(m, K_BLOCK, ti, t, sl, tl, sh, dxt, start, nbytes, t.tmem & 255, lines);
        for (uint32_t i = 0; id && i < lines && i < 256; i++) owner_[256 | ((t.tmem + i) & 255)] = id;
    } else {
        lines = (nbytes + 7) / 8;
        add(m, K_BLOCK, ti, t, sl, tl, sh, dxt, start, nbytes, t.tmem, lines);
    }
}

void Tmem::load_tile(const Mem& m, const Timg& ti, const Tile& t, int sl, int tl, int sh, int th) {
    int bpp = SIZ_BITS[ti.siz & 3];
    int x0 = sl >> 2, y0 = tl >> 2, x1 = sh >> 2, y1 = th >> 2;
    if (x1 < x0 || y1 < y0) return;
    uint32_t start = ti.addr + (uint32_t)((y0 * ti.width + x0) * bpp / 8);
    uint32_t nbytes = (uint32_t)(((y1 - y0) * ti.width + (x1 - x0 + 1)) * bpp / 8);
    int rows = y1 - y0 + 1;
    int row_bytes = ((x1 - x0 + 1) * (ti.siz == 3 ? 16 : bpp) + 7) / 8;
    int row_lines = (row_bytes + 7) / 8;
    // owner lines row by row
    uint16_t id = add(m, K_TILE, ti, t, sl, tl, sh, th, start, nbytes, 0, 0);
    if (!id) return;
    for (int r = 0; r < rows; r++) {
        uint32_t l = t.tmem + (uint32_t)(r * t.line);
        for (int i = 0; i < row_lines; i++) {
            if (ti.siz == 3) { owner_[(l + i) & 255] = id; owner_[256 | ((l + i) & 255)] = id; }
            else owner_[(l + i) & 511] = id;
        }
    }
}

void Tmem::load_tlut(const Mem& m, const Timg& ti, const Tile& t, int sl, int tl, int sh, int th) {
    int first = sl >> 2, last = sh >> 2;
    int count = last - first + 1;
    if (count <= 0) return;
    uint32_t start = ti.addr + (uint32_t)((tl >> 2) * ti.width * 2 + first * 2);
    add(m, K_TLUT, ti, t, sl, tl, sh, th, start, (uint32_t)count * 2, t.tmem, (uint32_t)count);
}

void Tmem::store(uint32_t byte_addr, const uint8_t* data, uint32_t n, bool odd, bool split32) {
    for (uint32_t k = 0; k < n; k += 8) {
        uint8_t chunk[8] = {};
        memcpy(chunk, data + k, std::min<uint32_t>(8, n - k));
        if (odd) { uint8_t tmp[4]; memcpy(tmp, chunk, 4); memcpy(chunk, chunk + 4, 4); memcpy(chunk + 4, tmp, 4); }
        uint32_t a = (byte_addr + k) & 0xFFF;
        if (split32) a &= 0x7FF;
        memcpy(scratch_ + (a & 0xFF8), chunk, 8);
    }
}

void Tmem::replay(const Mem& m, const Rec& r) {
    uint8_t raw[4096 + 64];
    const Timg& ti = r.timg;
    int bpp = SIZ_BITS[ti.siz & 3];
    if (r.kind == K_BLOCK) {
        int texels = r.sh - r.sl + 1;
        uint32_t nbytes = std::min<uint32_t>(r.nbytes, 4096);
        for (uint32_t i = 0; i < nbytes; i++) raw[i] = m.u8(r.src + i);
        uint32_t base = (uint32_t)r.tmem * 8;
        int dxt = r.th;
        if (ti.siz == 3) {
            int words = (texels * 2 + 7) / 8;
            for (int wi = 0; wi < words && wi < 256; wi++) {
                bool odd = ((((int)r.tl << 11) + wi * dxt) >> 11) & 1;
                uint8_t lo[8] = {}, hi[8] = {};
                for (int k = 0; k < 4; k++) {
                    uint32_t i = (uint32_t)(wi * 4 + k) * 4;
                    if (i + 4 <= nbytes) { lo[k * 2] = raw[i]; lo[k * 2 + 1] = raw[i + 1]; hi[k * 2] = raw[i + 2]; hi[k * 2 + 1] = raw[i + 3]; }
                }
                store(base + wi * 8, lo, 8, odd, true);
                store(((base + wi * 8) & 0x7FF) | 0x800, hi, 8, odd, false);
            }
        } else {
            for (uint32_t wi = 0; wi < (nbytes + 7) / 8; wi++) {
                bool odd = ((((int)r.tl << 11) + (int)wi * dxt) >> 11) & 1;
                uint8_t chunk[8] = {};
                for (int k = 0; k < 8 && wi * 8 + k < nbytes; k++) chunk[k] = raw[wi * 8 + k];
                store(base + wi * 8, chunk, 8, odd, false);
            }
        }
    } else if (r.kind == K_TILE) {
        int x0 = r.sl >> 2, y0 = r.tl >> 2, x1 = r.sh >> 2, y1 = r.th >> 2;
        uint32_t row_bytes = (uint32_t)(((x1 - x0 + 1) * bpp + 7) / 8);
        if (row_bytes > 4096) row_bytes = 4096;
        for (int rr = 0; rr <= y1 - y0 && rr < 4096; rr++) {
            uint32_t src = ti.addr + (uint32_t)(((y0 + rr) * ti.width + x0) * bpp / 8);
            for (uint32_t i = 0; i < row_bytes; i++) raw[i] = m.u8(src + i);
            uint32_t dst = (uint32_t)(r.tmem + rr * r.line) * 8;
            if (ti.siz == 3) {
                uint8_t lo[2048], hi[2048];
                uint32_t n = 0;
                for (uint32_t i = 0; i + 4 <= row_bytes && n + 2 <= 2048; i += 4, n += 2) {
                    lo[n] = raw[i]; lo[n + 1] = raw[i + 1]; hi[n] = raw[i + 2]; hi[n + 1] = raw[i + 3];
                }
                store(dst, lo, n, rr & 1, true);
                store((dst & 0x7FF) | 0x800, hi, n, rr & 1, false);
            } else {
                store(dst, raw, row_bytes, rr & 1, false);
            }
        }
    } else {
        int count = (int)(r.nbytes / 2);
        for (int i = 0; i < count; i++) {
            uint8_t e0 = m.u8(r.src + i * 2), e1 = m.u8(r.src + i * 2 + 1);
            uint32_t a = ((uint32_t)r.tmem * 8 + (uint32_t)i * 8) & 0xFFF;
            for (int k = 0; k < 4; k++) { scratch_[(a + k * 2) & 0xFFF] = e0; scratch_[(a + k * 2 + 1) & 0xFFF] = e1; }
        }
    }
}

uint32_t Tmem::rec_hash(const Mem& m, Rec& r) {
    if (r.hashed) return r.hash;
    r.hashed = true;
    uint32_t h = 2166136261u;
    if (config().hash_textures) {
        uint32_t a = Mem::phys(r.src) & ~3u, end = (Mem::phys(r.src) + r.nbytes + 3) & ~3u;
        if (end > m.size) end = m.size;
        for (; a < end; a += 4) {
            uint32_t w = *(const uint32_t*)(m.base + a);
            h = (h ^ w) * 0x01000193u;
            h ^= h >> 13;
        }
        if (ge::g_stats) ge::g_stats->tex_hash_bytes += r.nbytes;
    }
    r.hash = h;
    return h;
}

// One texel of the scratch TMEM at tile-relative (s, t): an index (indexed formats) or ABGR8888.
uint32_t Tmem::texel(const Tile& tile, int s, int t, int tlut_mode, bool* indexed) const {
    const uint8_t* tm = scratch_;
    int siz = tile.siz, fmt = tile.fmt;
    uint32_t x = (t & 1) ? 4 : 0;
    uint32_t base = (uint32_t)(tile.tmem + tile.line * t) * 8;
    uint32_t v;
    if (siz == 0) {
        uint32_t a = ((base + (uint32_t)(s >> 1)) ^ x) & 0xFFF;
        v = (s & 1) ? (tm[a] & 15) : (tm[a] >> 4);
    } else if (siz == 1) {
        v = tm[((base + (uint32_t)s) ^ x) & 0xFFF];
    } else if (siz == 2) {
        uint32_t a = ((base + (uint32_t)s * 2) ^ x) & 0xFFE;
        v = ((uint32_t)tm[a] << 8) | tm[a + 1];
    } else {
        uint32_t a = ((base + (uint32_t)s * 2) ^ x) & 0x7FE;
        *indexed = false;
        return tm[a] | ((uint32_t)tm[a + 1] << 8) | ((uint32_t)tm[a | 0x800] << 16) | ((uint32_t)tm[(a | 0x800) + 1] << 24);
    }
    if (siz <= 1) { *indexed = true; return v; }
    (void)tlut_mode;
    *indexed = false;
    if (fmt == FMT_RGBA) return 0;   // 5551 handled by the caller
    if (fmt == FMT_IA) return ia16_to_abgr((uint16_t)v);
    return grey(v & 255, 255);
}

TexEntry* Tmem::decode(const Mem& m, const Tile& tile, int tlut_mode, uint64_t key) {
    Axis ax = make_axis(tile.uls, tile.lrs, tile.masks, tile.cms);
    Axis ay = make_axis(tile.ult, tile.lrt, tile.maskt, tile.cmt);
    int siz = tile.siz;
    int psm, minw;
    const uint32_t* fixed_clut = nullptr;
    int clut_n = 0;
    bool use_tlut = tlut_mode >= 2 && siz <= 1;
    if (siz == 0) { psm = ge::PSM_T4; minw = 32; clut_n = 16; }
    else if (siz == 1) { psm = ge::PSM_T8; minw = 16; clut_n = 256; }
    else if (siz == 2 && tile.fmt == FMT_RGBA) { psm = ge::PSM_5551; minw = 8; }
    else { psm = ge::PSM_8888; minw = 4; }
    if (siz <= 1 && !use_tlut) {
        if (tile.fmt == FMT_IA) fixed_clut = siz == 0 ? g_clut_ia4 : g_clut_ia8;
        else if (tile.fmt == FMT_RGBA) fixed_clut = siz == 0 ? g_clut_rgba4 : g_clut_rgba8;
        else fixed_clut = siz == 0 ? g_clut_i4 : g_clut_i8;   // I, CI without TLUT (read as intensity), YUV
    }
    int W = (int)std::min<uint32_t>(512, pow2((uint32_t)std::max(ax.repeat ? ax.period : ax.size, minw)));
    int H = (int)std::min<uint32_t>(512, pow2((uint32_t)std::max(ay.repeat ? ay.period : ay.size, 8)));
    // a repeating axis must tile the padded size exactly
    if (ax.repeat && (W % ax.period)) ax.repeat = false;
    if (ay.repeat && (H % ay.period)) ay.repeat = false;
    int bpp = psm == ge::PSM_T4 ? 4 : psm == ge::PSM_T8 ? 8 : psm == ge::PSM_5551 ? 16 : 32;
    size_t row_bytes = (size_t)W * bpp / 8, bytes = row_bytes * H;
    lin_.assign(bytes, 0);
    uint8_t* lin = lin_.data();
    for (int y = 0; y < H; y++) {
        int ty = ay.map(y);
        uint8_t* row = lin + row_bytes * y;
        for (int x = 0; x < W; x++) {
            int tx = ax.map(x);
            bool indexed = false;
            uint32_t v = texel(tile, tx, ty, tlut_mode, &indexed);
            if (psm == ge::PSM_T4) {
                row[x >> 1] |= (uint8_t)((v & 15) << ((x & 1) * 4));   // GE T4: low nibble first
            } else if (psm == ge::PSM_T8) {
                row[x] = (uint8_t)v;
            } else if (psm == ge::PSM_5551) {
                uint32_t a = ((tile.tmem + tile.line * ty) * 8u + (uint32_t)tx * 2) ^ ((ty & 1) ? 4u : 0u);
                a &= 0xFFE;
                uint16_t n = (uint16_t)((scratch_[a] << 8) | scratch_[a + 1]);
                uint16_t r = (n >> 11) & 31, g = (n >> 6) & 31, b = (n >> 1) & 31;
                uint16_t o = (uint16_t)(r | (g << 5) | (b << 10) | ((n & 1) << 15));
                memcpy(row + x * 2, &o, 2);
            } else {
                memcpy(row + x * 4, &v, 4);
            }
        }
    }
    // texture + CLUT memory
    size_t clut_bytes = (size_t)clut_n * 4;
    size_t need = bytes + (use_tlut ? clut_bytes + 64 : 0);
    uint8_t* mem = (uint8_t*)alloc_tex(need);
    if (!mem) return nullptr;
    // swizzle: 16-byte x 8-row blocks
    size_t bw = row_bytes / 16;
    uint8_t* out = mem;
    for (int by = 0; by < H / 8; by++)
        for (size_t bx = 0; bx < bw; bx++)
            for (int r = 0; r < 8; r++) {
                memcpy(out, lin + (size_t)(by * 8 + r) * row_bytes + bx * 16, 16);
                out += 16;
            }
    const uint32_t* clut = fixed_clut;
    if (use_tlut) {
        uint32_t* c = (uint32_t*)(((uintptr_t)(mem + bytes) + 63) & ~(uintptr_t)63);
        for (int i = 0; i < clut_n; i++) {
            int idx = siz == 0 ? ((tile.palette << 4) | i) : i;
            uint32_t a = (0x800 + (uint32_t)idx * 8) & 0xFFF;
            uint16_t e = (uint16_t)((scratch_[a] << 8) | scratch_[a + 1]);
            c[i] = tlut_mode == 3 ? ia16_to_abgr(e) : rgba5551_to_abgr(e);
        }
        clut = c;
    }
    ge::cpu_wrote(mem, need);
#ifndef __PSP__
    if (const char* dir = getenv("GFX_DUMP_TEX")) {   // the Mac test: each decoded texture as a PPM (alpha dropped)
        static int n = 0;
        char path[512];
        snprintf(path, sizeof path, "%s/tex%04d_fmt%d_siz%d_tmem%03X_%dx%d.ppm", dir, n++, tile.fmt, tile.siz, tile.tmem, W, H);
        if (FILE* f = fopen(path, "wb")) {
            fprintf(f, "P6 %d %d 255\n", W, H);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    const uint8_t* row = lin + row_bytes * y;
                    uint32_t c;
                    if (psm == ge::PSM_T4) c = clut[(row[x >> 1] >> ((x & 1) * 4)) & 15];
                    else if (psm == ge::PSM_T8) c = clut[row[x]];
                    else if (psm == ge::PSM_5551) { uint16_t v; memcpy(&v, row + x * 2, 2); c = ((v & 31) << 3) | (((v >> 5) & 31) << 11) | (((v >> 10) & 31) << 19); }
                    else memcpy(&c, row + x * 4, 4);
                    uint8_t px[3] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16)};
                    fwrite(px, 1, 3, f);
                }
            fclose(f);
        }
    }
#endif
    TexEntry& e = cache_[key];
    e.tex.data = mem;
    e.tex.clut = clut;
    e.tex.psm = psm;
    e.tex.w = W;
    e.tex.h = H;
    e.tex.clut_entries = clut ? clut_n : 0;
    e.dw = ax.repeat ? ax.period : ax.size;
    e.dh = ay.repeat ? ay.period : ay.size;
    e.repeat_s = ax.repeat;
    e.repeat_t = ay.repeat;
    e.last_frame = frame_;
    if (ge::g_stats) { ge::g_stats->tex_uploads++; ge::g_stats->tex_bytes += (uint32_t)need; }
    (void)m;
    return &e;
}

void* Tmem::alloc_tex(size_t need) {
    void* p = ge::tex_mem(need, 64);
    if (p) return p;
    // Least recently used first. Only entries no draw of this frame uses: the GE has finished every earlier frame's list
    // (end_frame waits for it), so their memory is free to reuse; this frame's list may still read the others.
    std::vector<std::pair<uint32_t, uint64_t>> old;
    old.reserve(cache_.size());
    for (auto& [k, e] : cache_)
        if (e.last_frame != frame_) old.push_back({ e.last_frame, k });
    if (old.empty()) return nullptr;
    std::sort(old.begin(), old.end());
    serial_++;   // the per-tile memo may point at an entry freed below
    for (auto& [f, k] : old) {
        auto it = cache_.find(k);
        ge::tex_free(const_cast<void*>(it->second.tex.data));
        cache_.erase(it);
        if (ge::g_stats) ge::g_stats->tex_evictions++;
        if ((p = ge::tex_mem(need, 64)) != nullptr) return p;
    }
    return nullptr;
}

const TexEntry* Tmem::get(const Mem& m, const Tile& tile, int tlut_mode) {
    if (ge::g_stats) ge::g_stats->tex_lookups++;
    Last& last = last_[(tile.tmem ^ tile.palette ^ (tile.siz << 3)) & 7];
    if (last.e && last.serial == serial_ && last.tlut == tlut_mode && !memcmp(&last.tile, &tile, sizeof tile)) {
        if (ge::g_stats) ge::g_stats->tex_hits++;
        return last.e;
    }
    const TexEntry* found = lookup(m, tile, tlut_mode);
    last.tile = tile;
    last.tlut = tlut_mode;
    last.serial = serial_;
    last.e = found;
    return found;
}

const TexEntry* Tmem::lookup(const Mem& m, const Tile& tile, int tlut_mode) {
    Axis ax = make_axis(tile.uls, tile.lrs, tile.masks, tile.cms);
    Axis ay = make_axis(tile.ult, tile.lrt, tile.maskt, tile.cmt);
    int rows = ay.repeat ? std::min(ay.period, 1 << tile.maskt) : ay.size;
    int cols = ax.repeat ? std::min(ax.period, 1 << tile.masks) : ax.size;
    int bpp = tile.siz == 3 ? 16 : SIZ_BITS[tile.siz & 3];
    int row_lines = ((cols * bpp + 7) / 8 + 7) / 8;
    // the records behind the TMEM lines this tile reads
    uint16_t ids[512];   // at most one record per TMEM line, so this never overflows (64 dropped records silently)
    int nid = 0;
    auto note_line = [&](uint32_t l) {
        uint16_t id = owner_[l & 511];
        if (!id) return;
        for (int i = 0; i < nid; i++) if (ids[i] == id) return;
        if (nid < 512) ids[nid++] = id;
    };
    int line = tile.line ? tile.line : row_lines;
    for (int r = 0; r < rows && r < 512; r++)
        for (int i = 0; i < row_lines; i++) {
            uint32_t l = tile.tmem + (uint32_t)(r * line + i);
            if (tile.siz == 3) { note_line(l & 255); note_line(256 | (l & 255)); }
            else note_line(l);
        }
    bool use_tlut = tlut_mode >= 2 && tile.siz <= 1;
    if (use_tlut) {
        int first = tile.siz == 0 ? tile.palette * 16 : 0, n = tile.siz == 0 ? 16 : 256;
        for (int i = 0; i < n; i++) note_line(256 + (uint32_t)((first + i) & 255));
    }
    std::sort(ids, ids + nid);
    uint64_t key = 1469598103934665603ull;
    key = fnv64(key, (uint64_t)tile.fmt | (uint64_t)tile.siz << 4 | (uint64_t)tile.line << 8 | (uint64_t)tile.tmem << 20 |
                         (uint64_t)tile.palette << 32 | (uint64_t)tile.cms << 36 | (uint64_t)tile.cmt << 38 |
                         (uint64_t)tile.masks << 40 | (uint64_t)tile.maskt << 44 | (uint64_t)(use_tlut ? tlut_mode : 0) << 48);
    key = fnv64(key, (uint64_t)(uint16_t)ax.size | (uint64_t)(uint16_t)ay.size << 16);
    for (int i = 0; i < nid; i++) {
        Rec& r = recs_[ids[i] - 1];
        key = fnv64(key, (uint64_t)r.src | (uint64_t)r.nbytes << 32);
        key = fnv64(key, (uint64_t)r.kind | (uint64_t)r.timg.siz << 4 | (uint64_t)r.timg.width << 8 | (uint64_t)r.tmem << 20 |
                             (uint64_t)r.line << 32 | (uint64_t)r.sl << 44);
        key = fnv64(key, (uint64_t)r.tl | (uint64_t)r.sh << 16 | (uint64_t)r.th << 32 | (uint64_t)r.timg.fmt << 48);
        key = fnv64(key, rec_hash(m, r));
    }
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        it->second.last_frame = frame_;
        if (ge::g_stats) ge::g_stats->tex_hits++;
        return &it->second;
    }
    memset(scratch_, 0, sizeof scratch_);
    for (int i = 0; i < nid; i++) replay(m, recs_[ids[i] - 1]);
    TexEntry* e = decode(m, tile, tlut_mode, key);
    if (!e) {
        // no room even after freeing every entry of earlier frames (this frame alone fills both arenas): drain the GE,
        // drop every texture, try once more
        ge::wait_idle();
        cache_.clear();
        serial_++;
        ge::tex_reset();
        if (ge::g_stats) ge::g_stats->tex_resets++;
        e = decode(m, tile, tlut_mode, key);
        if (!e && ge::g_stats) ge::g_stats->tex_failed++;
    }
    return e;
}

}  // namespace blitzgfx
